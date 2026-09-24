"""ENCO home bridge: the only process that holds smart-home credentials.

The iAqualink account password and session tokens live here, on a machine on the home LAN, and
nowhere else. ENCO (the ESP32) never sees them, and neither does the XiaoZhi cloud: the device only
sends this bridge a tiny HMAC-signed request such as {"target": "pool_heater", "action": "on"}.

Subcommands:
  discover   Log in and print every system/device on the account (read-only). Use this once to
             learn the device keys, then write pool_config.json.
  status     Print the current state of the devices listed in pool_config.json (read-only).
  serve      Run the LAN HTTP API that ENCO calls.

Secrets are read from tools/home_bridge/.env (git-ignored, must be chmod 600):
  IAQUALINK_USERNAME, IAQUALINK_PASSWORD, BRIDGE_HMAC_KEY
"""

from __future__ import annotations

import argparse
import asyncio
import hashlib
import hmac
import json
import logging
import os
import stat
import sys
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

from iaqualink.client import AqualinkClient

HERE = Path(__file__).resolve().parent
ENV_PATH = HERE / ".env"
CONFIG_PATH = HERE / "pool_config.json"

LOG = logging.getLogger("home_bridge")

MAX_BODY_BYTES = 512
SIG_MAX_SKEW_S = 30
RATE_LIMIT_PER_MIN = 12
STATUS_CACHE_S = 10


# --------------------------------------------------------------------------------------- secrets
def load_env() -> None:
    """Minimal .env loader (KEY=VALUE per line). Avoids an extra dependency."""
    if not ENV_PATH.exists():
        sys.exit(f"missing {ENV_PATH} - copy .env.example to .env, fill it in, chmod 600 .env")
    mode = ENV_PATH.stat().st_mode
    if mode & (stat.S_IRWXG | stat.S_IRWXO):
        sys.exit(f"{ENV_PATH} is readable by other users - run: chmod 600 {ENV_PATH}")
    for raw in ENV_PATH.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        os.environ.setdefault(key.strip(), value.strip().strip('"').strip("'"))


def require_env(name: str) -> str:
    value = os.environ.get(name, "")
    if not value:
        # Fail closed: never fall back to a default secret.
        sys.exit(f"{name} is not set in {ENV_PATH}")
    return value


def make_client() -> AqualinkClient:
    return AqualinkClient(require_env("IAQUALINK_USERNAME"), require_env("IAQUALINK_PASSWORD"))


# --------------------------------------------------------------------------------------- config
def load_config() -> dict[str, Any]:
    """pool_config.json maps friendly targets to device keys, with hard safety limits.

    Only targets listed here can ever be touched through the API (allow-list).
    """
    if not CONFIG_PATH.exists():
        sys.exit(f"missing {CONFIG_PATH} - run `discover`, then copy pool_config.example.json")
    cfg = json.loads(CONFIG_PATH.read_text(encoding="utf-8"))
    if not isinstance(cfg.get("targets"), dict) or not cfg["targets"]:
        sys.exit("pool_config.json: 'targets' must be a non-empty object")
    for name, t in cfg["targets"].items():
        if t.get("kind") not in ("switch", "thermostat"):
            sys.exit(f"pool_config.json: target {name!r} has invalid kind")
        if t["kind"] == "thermostat":
            lo, hi = t.get("min"), t.get("max")
            if not isinstance(lo, int) or not isinstance(hi, int) or lo >= hi:
                sys.exit(f"pool_config.json: thermostat {name!r} needs integer min < max")
    return cfg


# --------------------------------------------------------------------------------------- iAqualink
def describe(device: Any) -> dict[str, Any]:
    info: dict[str, Any] = {
        "key": getattr(device, "name", ""),
        "label": getattr(device, "label", ""),
        "state": getattr(device, "state", None),
        "class": type(device).__name__,
    }
    for attr in ("is_on", "current_temperature", "target_temperature", "min_temperature",
                 "max_temperature", "unit"):
        if hasattr(device, attr):
            try:
                info[attr] = getattr(device, attr)
            except Exception:  # noqa: BLE001 - some properties raise when data is missing
                pass
    return info


async def get_system(client: AqualinkClient, serial: str | None) -> Any:
    systems = await client.get_systems()
    if not systems:
        raise RuntimeError("no iAqualink systems on this account")
    if serial:
        if serial not in systems:
            raise RuntimeError("configured system serial not found on this account")
        return systems[serial]
    return next(iter(systems.values()))


async def cmd_discover() -> None:
    async with make_client() as client:
        systems = await client.get_systems()
        for serial, system in systems.items():
            print(f"\nSYSTEM {serial}  name={getattr(system, 'name', '')!r}  "
                  f"type={type(system).__name__}  online={getattr(system, 'online', '?')}")
            devices = await system.get_devices()
            for key in sorted(devices):
                d = describe(devices[key])
                extra = {k: v for k, v in d.items() if k not in ("key", "label")}
                print(f"  {key:<28} {d['label']!r:<28} {extra}")


# --------------------------------------------------------------------------------------- controller
class PoolController:
    """Owns the single iAqualink session on a private asyncio loop thread."""

    def __init__(self, cfg: dict[str, Any]) -> None:
        self.cfg = cfg
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.thread.start()
        self.client: AqualinkClient | None = None
        self.system: Any = None
        self._status_cache: tuple[float, dict[str, Any]] | None = None
        self._lock = threading.Lock()

    def run(self, coro: Any, timeout: float = 20.0) -> Any:
        return asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout)

    async def _ensure(self) -> Any:
        if self.client is None:
            self.client = make_client()
            await self.client.__aenter__()
        if self.system is None:
            self.system = await get_system(self.client, self.cfg.get("system_serial"))
        return self.system

    async def _devices(self) -> dict[str, Any]:
        system = await self._ensure()
        return await system.get_devices()

    async def _status(self) -> dict[str, Any]:
        system = await self._ensure()
        await system.update()
        devices = await system.get_devices()
        out: dict[str, Any] = {}
        for name, t in self.cfg["targets"].items():
            d = devices.get(t["device"])
            out[name] = describe(d) if d is not None else {"error": "device missing"}
            out[name].pop("key", None)
        return out

    def status(self) -> dict[str, Any]:
        with self._lock:
            now = time.monotonic()
            if self._status_cache and now - self._status_cache[0] < STATUS_CACHE_S:
                return self._status_cache[1]
            result = self.run(self._status())
            self._status_cache = (now, result)
            return result

    async def _apply(self, target: str, action: str, value: Any) -> str:
        t = self.cfg["targets"][target]
        devices = await self._devices()
        d = devices.get(t["device"])
        if d is None:
            raise RuntimeError("device missing")
        if t["kind"] == "switch" or action in ("on", "off"):
            if action == "on":
                if not getattr(d, "is_on", False):
                    await d.turn_on()
            elif action == "off":
                if getattr(d, "is_on", False):
                    await d.turn_off()
            else:
                raise ValueError("switch accepts on/off")
            return f"{target} {action}"
        # thermostat
        if action != "set":
            raise ValueError("thermostat accepts set")
        if not isinstance(value, int) or isinstance(value, bool):
            raise ValueError("value must be an integer")
        if not t["min"] <= value <= t["max"]:
            raise ValueError(f"value out of range {t['min']}-{t['max']}")
        await d.set_temperature(value)
        return f"{target} set {value}"

    def apply(self, target: str, action: str, value: Any) -> str:
        with self._lock:
            self._status_cache = None
            return self.run(self._apply(target, action, value))


# --------------------------------------------------------------------------------------- HTTP API
class RateLimiter:
    def __init__(self, per_min: int) -> None:
        self.per_min = per_min
        self.hits: dict[str, deque[float]] = {}
        self.lock = threading.Lock()

    def allow(self, ip: str) -> bool:
        now = time.monotonic()
        with self.lock:
            q = self.hits.setdefault(ip, deque())
            while q and now - q[0] > 60:
                q.popleft()
            if len(q) >= self.per_min:
                return False
            q.append(now)
            return True


class ReplayGuard:
    def __init__(self) -> None:
        self.seen: dict[str, float] = {}
        self.lock = threading.Lock()

    def fresh(self, sig: str) -> bool:
        now = time.time()
        with self.lock:
            for k in [k for k, t in self.seen.items() if now - t > 2 * SIG_MAX_SKEW_S]:
                del self.seen[k]
            if sig in self.seen:
                return False
            self.seen[sig] = now
            return True


def expected_signature(key: bytes, ts: str, method: str, path: str, body: bytes) -> str:
    msg = f"{ts}\n{method}\n{path}\n".encode() + body
    return hmac.new(key, msg, hashlib.sha256).hexdigest()


def make_handler(ctrl: PoolController, key: bytes) -> type[BaseHTTPRequestHandler]:
    limiter = RateLimiter(RATE_LIMIT_PER_MIN)
    replay = ReplayGuard()

    class Handler(BaseHTTPRequestHandler):
        server_version = "enco-bridge"
        sys_version = ""

        def log_message(self, fmt: str, *args: Any) -> None:  # no bodies/headers in logs
            LOG.info("%s %s", self.client_address[0], fmt % args)

        def _send(self, code: int, payload: dict[str, Any]) -> None:
            data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(data)

        def _authenticate(self, body: bytes) -> bool:
            ts = self.headers.get("X-Enco-Ts", "")
            sig = self.headers.get("X-Enco-Sig", "")
            if not ts.isdigit() or len(sig) != 64:
                return False
            if abs(time.time() - int(ts)) > SIG_MAX_SKEW_S:
                return False
            want = expected_signature(key, ts, self.command, self.path, body)
            if not hmac.compare_digest(want, sig):
                return False
            return replay.fresh(sig)

        def _read_body(self) -> bytes | None:
            try:
                length = int(self.headers.get("Content-Length", "0"))
            except ValueError:
                return None
            if length < 0 or length > MAX_BODY_BYTES:
                return None
            return self.rfile.read(length) if length else b""

        def _guard(self) -> bytes | None:
            if not limiter.allow(self.client_address[0]):
                self._send(429, {"ok": False, "error": "rate limited"})
                return None
            body = self._read_body()
            if body is None:
                self._send(413, {"ok": False, "error": "bad body"})
                return None
            if not self._authenticate(body):
                self._send(401, {"ok": False, "error": "unauthorized"})
                return None
            return body

        def do_GET(self) -> None:  # noqa: N802
            if self._guard() is None:
                return
            if self.path != "/pool/status":
                self._send(404, {"ok": False, "error": "not found"})
                return
            try:
                self._send(200, {"ok": True, "status": ctrl.status()})
            except Exception:  # noqa: BLE001
                LOG.exception("status failed")
                self._send(502, {"ok": False, "error": "pool service unavailable"})

        def do_POST(self) -> None:  # noqa: N802
            body = self._guard()
            if body is None:
                return
            if self.path != "/pool/set":
                self._send(404, {"ok": False, "error": "not found"})
                return
            try:
                req = json.loads(body.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError):
                self._send(400, {"ok": False, "error": "invalid json"})
                return
            target, action, value = req.get("target"), req.get("action"), req.get("value")
            if not isinstance(target, str) or target not in ctrl.cfg["targets"]:
                self._send(400, {"ok": False, "error": "unknown target"})
                return
            if action not in ("on", "off", "set"):
                self._send(400, {"ok": False, "error": "invalid action"})
                return
            try:
                self._send(200, {"ok": True, "result": ctrl.apply(target, action, value)})
            except ValueError as e:
                self._send(400, {"ok": False, "error": str(e)})
            except Exception:  # noqa: BLE001
                LOG.exception("apply failed")
                self._send(502, {"ok": False, "error": "pool service unavailable"})

        # Allow-list of methods: everything else is rejected.
        def _reject(self) -> None:
            self._send(405, {"ok": False, "error": "method not allowed"})

        do_PUT = do_DELETE = do_PATCH = do_HEAD = do_OPTIONS = do_TRACE = _reject  # type: ignore

    return Handler


def cmd_serve(host: str, port: int) -> None:
    key = require_env("BRIDGE_HMAC_KEY").encode()
    if len(key) < 32:
        sys.exit("BRIDGE_HMAC_KEY must be at least 32 characters (use `python3 -c "
                 "'import secrets; print(secrets.token_hex(32))'`)")
    cfg = load_config()
    ctrl = PoolController(cfg)
    server = ThreadingHTTPServer((host, port), make_handler(ctrl, key))
    LOG.info("listening on http://%s:%d (targets: %s)", host, port, ", ".join(cfg["targets"]))
    # TODO(security): plain HTTP on the LAN; requests are HMAC-signed and replay-protected, but
    # responses (pool state) are not encrypted. Acceptable on a trusted home LAN; do NOT expose
    # this port to the internet.
    server.serve_forever()


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    # The iaqualink library can log URLs at DEBUG; keep it quiet so nothing sensitive is printed.
    logging.getLogger("iaqualink").setLevel(logging.WARNING)
    logging.getLogger("httpx").setLevel(logging.WARNING)

    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("discover")
    sub.add_parser("status")
    s = sub.add_parser("serve")
    # Localhost by default; pass --host <this machine's LAN IP> once ENCO needs to reach it.
    s.add_argument("--host", default="127.0.0.1")
    s.add_argument("--port", type=int, default=8787)
    args = p.parse_args()

    load_env()
    if args.cmd == "discover":
        asyncio.run(cmd_discover())
    elif args.cmd == "status":
        ctrl = PoolController(load_config())
        print(json.dumps(ctrl.status(), ensure_ascii=False, indent=2))
    else:
        cmd_serve(args.host, args.port)


if __name__ == "__main__":
    main()
