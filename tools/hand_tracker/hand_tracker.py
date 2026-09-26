#!/usr/bin/env python3
"""ENCO-02 hand tracker: finds one raised finger in the head camera's video and steers the head to it.

Why this runs on a computer
---------------------------
Recognising a hand needs a neural network. Every "ESP32-CAM gesture tracking" project splits the work the
same way: the ESP32-CAM only streams video, a PC runs Google MediaPipe on it. The camera's own skin-colour
finder (firmware/cam/cam_tracker.cpp) is what fits on the chip, and it mistakes beige walls and furniture
for fingers. This script is the MediaPipe half.

  camera       --MJPEG, GET /stream?raw=1&tracker=1-->          this script (MediaPipe GestureRecognizer)
  this script  --"S seq dx dy conf lean kind gesture mac", same socket-->  camera
  camera       --UART "G 1" / "T dx dy conf lean 1"-->            main board, which moves the servos

Every line is HMAC-SHA256 signed with ENCO_TRACK_KEY (the same value as CAM_TRACK_KEY in the camera's
cam_config.h) over the nonce the camera picks for each connection; see firmware/cam/cam_remote.h.

What it looks for
-----------------
* MediaPipe's "Pointing_Up" gesture = a clear raised index finger. Held for about 0.7 s it arms tracking
  (the camera debounces it, the main board switches tracking on).
* While tracking, any hand with the index finger out and the other three curled is followed, tilted or
  not - the tilt ("lean") is what the head's 歪头 mirrors.
The point the head brings to the middle of the picture is the middle of the index finger (knuckle to tip).

Usage (from tools/hand_tracker/)
--------------------------------
  .venv/bin/python hand_tracker.py --download-model   # once: fetch the model, verify its SHA-256
  .venv/bin/python hand_tracker.py                    # run; camera at $ENCO_CAM_HOST or 192.168.86.65
  .venv/bin/python hand_tracker.py --show             # with a preview window (press q to quit)
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import logging
import math
import os
import re
import socket
import stat
import sys
import threading
import time
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

import cv2
import numpy as np

HERE = Path(__file__).resolve().parent
DEFAULT_CAM = "192.168.86.65"

MODEL_PATH = HERE / "models" / "gesture_recognizer.task"
# The versioned URL rather than .../latest/..., so the pinned hash below stays valid.
MODEL_URL = (
    "https://storage.googleapis.com/mediapipe-models/gesture_recognizer/"
    "gesture_recognizer/float16/1/gesture_recognizer.task"
)
MODEL_SHA256 = "97952348cf6a6a4915c2ea1496b4b37ebabc50cbbf80571435643c455f2b0482"
MODEL_MAX_BYTES = 64 * 1024 * 1024

# --- what counts as a finger -------------------------------------------------------------------------
# A detection this unsure of which hand it is looking at is almost always not a hand: posters, reflections.
MIN_HANDEDNESS = 0.6
# ...and neither is anything smaller than this fraction of the picture's short side. A real hand at 2 m
# is still ~40 px of a 240 px frame; the one false detection seen in 115 empty-room frames was 10 px.
MIN_HAND_FRACTION = 0.12
# "Pointing_Up" score needed to follow the finger, and the higher one needed to count towards arming.
POINTING_TRACK = 0.5
POINTING_ARM = 0.6
# Confidence sent for a finger recognised only by its shape (index out, others curled). Below the main
# board's 80, so these samples move the head but never teach it which way its servos turn.
SHAPE_ONLY_CONF = 65
# Index finger "out": wrist-to-tip at least this much longer than wrist-to-middle-joint. The other three
# count as curled below the second ratio. Asymmetric on purpose: a half-bent finger is neither.
INDEX_OUT_RATIO = 1.2
OTHERS_CURLED_RATIO = 1.05
# A shape-only match must point roughly up (|lean| at most this, in degrees)... Every false shape match
# seen live was a "finger" lying sideways (lean exactly +/-90) somewhere in the room.
SHAPE_MAX_LEAN = 45
# ...and only counts while a real Pointing_Up was seen this recently: it keeps a lock through poor
# frames, it never starts one.
SHAPE_HOLD_S = 1.5
# Inference runs on a copy this wide. The palm detector works at 192 px and the landmarker at 224 px,
# so nothing is lost, and the GPU buffers MediaPipe allocates per frame are a quarter of the size.
INFER_WIDTH = 320

# The main board's deadband (kDeadband in firmware/main/cam_link.cpp), drawn in the preview only.
DEADBAND = 12

MAX_FRAME_BYTES = 256 * 1024
NONCE_RE = re.compile(r"^[0-9a-f]{32}$")
KEY_RE = re.compile(r"^[0-9A-Za-z_\-]{32,128}$")
# The camera is addressed by IP or plain host name; nothing that could smuggle a path or header.
HOST_RE = re.compile(r"^[0-9A-Za-z.\-]{1,253}$")

log = logging.getLogger("hand_tracker")


# --- configuration -------------------------------------------------------------------------------------
def load_key() -> bytes:
    """ENCO_TRACK_KEY from the environment, else from .env next to this script. Never logged."""
    key = os.environ.get("ENCO_TRACK_KEY", "").strip()
    env_path = HERE / ".env"
    if not key and env_path.exists():
        if env_path.stat().st_mode & (stat.S_IRWXG | stat.S_IRWXO):
            log.warning("%s is readable by other users - run: chmod 600 %s", env_path, env_path)
        for raw in env_path.read_text(encoding="utf-8").splitlines():
            name, sep, value = raw.strip().partition("=")
            if sep and name.strip() == "ENCO_TRACK_KEY":
                key = value.strip().strip("'\"")
                break
    if not KEY_RE.match(key):
        raise SystemExit(
            "ENCO_TRACK_KEY is missing or malformed. It must be the same 32+ character value as "
            "CAM_TRACK_KEY in firmware/cam/cam_config.h; put it in tools/hand_tracker/.env (chmod 600)."
        )
    return key.encode("ascii")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def download_model() -> None:
    """Fetches the model over HTTPS (fixed URL) and keeps it only if it matches the pinned SHA-256."""
    MODEL_PATH.parent.mkdir(parents=True, exist_ok=True)
    part = MODEL_PATH.with_suffix(".part")
    log.info("downloading %s", MODEL_URL)
    with urllib.request.urlopen(MODEL_URL, timeout=60) as response, part.open("wb") as out:
        total = 0
        while chunk := response.read(1 << 20):
            total += len(chunk)
            if total > MODEL_MAX_BYTES:
                raise SystemExit("model download is unexpectedly large - aborted")
            out.write(chunk)
    if sha256_file(part) != MODEL_SHA256:
        part.unlink(missing_ok=True)
        raise SystemExit("downloaded model does not match the pinned SHA-256 - not used")
    part.replace(MODEL_PATH)
    log.info("model saved to %s (SHA-256 verified)", MODEL_PATH)


def check_model() -> Path:
    if not MODEL_PATH.exists():
        raise SystemExit(f"model missing: {MODEL_PATH}\n  run: .venv/bin/python hand_tracker.py --download-model")
    if sha256_file(MODEL_PATH) != MODEL_SHA256:
        raise SystemExit(f"{MODEL_PATH} does not match the pinned SHA-256 - re-run with --download-model")
    return MODEL_PATH


# --- finger finding ------------------------------------------------------------------------------------
@dataclass
class Sample:
    """One line's worth for the camera. Ranges match firmware/cam/cam_remote.cpp exactly."""

    dx: int = 0  # finger centre, -100 (left edge) .. 100 (right edge)
    dy: int = 0  # finger centre, -100 (top) .. 100 (bottom)
    conf: int = 0  # 0 = no finger
    lean: int = 0  # degrees, + = fingertip leans right
    kind: int = 0  # 1 = finger
    gesture: int = 0  # 1 = clear raised index finger (counts towards arming)


@dataclass
class Finding:
    sample: Sample = field(default_factory=Sample)
    landmarks: list | None = None  # 21 normalised hand landmarks of the hand used (or the best seen)
    label: str = "no hand"


def _clamp(v: float, lo: int, hi: int) -> int:
    return max(lo, min(hi, int(round(v))))


def _dist(a, b, w: int, h: int) -> float:
    return math.hypot((a.x - b.x) * w, (a.y - b.y) * h)


def _reach(lm, tip: int, pip: int, w: int, h: int) -> float:
    """How far a fingertip reaches from the wrist compared with its middle joint (>1 = straight)."""
    base = _dist(lm[0], lm[pip], w, h)
    return _dist(lm[0], lm[tip], w, h) / base if base > 1e-6 else 0.0


def index_only(lm, w: int, h: int) -> bool:
    if _reach(lm, 8, 6, w, h) < INDEX_OUT_RATIO:
        return False
    return all(_reach(lm, tip, pip, w, h) < OTHERS_CURLED_RATIO for tip, pip in ((12, 10), (16, 14), (20, 18)))


def pick_finger(result, w: int, h: int, shape_allowed: bool = True) -> Finding:
    """Chooses the hand to follow, if any, and turns it into a Sample. shape_allowed: whether a finger
    recognised only by its shape may be followed (see SHAPE_HOLD_S)."""
    best = None
    seen = Finding()
    for i, lm in enumerate(result.hand_landmarks or []):
        handed = result.handedness[i][0].score if result.handedness and result.handedness[i] else 0.0
        top = result.gestures[i][0] if result.gestures and result.gestures[i] else None
        name = top.category_name if top else "None"
        score = float(top.score) if top else 0.0
        xs = [p.x * w for p in lm]
        ys = [p.y * h for p in lm]
        size = max(max(xs) - min(xs), max(ys) - min(ys))
        if handed < MIN_HANDEDNESS or size < MIN_HAND_FRACTION * min(w, h):
            continue  # too small or too doubtful to be a hand in front of the robot
        pointing = score if name == "Pointing_Up" else 0.0
        shape = index_only(lm, w, h)
        if pointing < POINTING_TRACK and not shape:
            if seen.landmarks is None:
                seen = Finding(Sample(), lm, f"hand, not pointing ({name})")
            continue
        rank = pointing + (0.25 if shape else 0.0) + size / 10000.0
        if best is None or rank > best[0]:
            best = (rank, lm, pointing, shape)
    if best is None:
        return seen

    _, lm, pointing, shape = best
    knuckle, tip = lm[5], lm[8]
    # Aim at the middle of the whole hand (its bounding box), not at the finger. Measured live: with the
    # finger centred, lowering the hand pushed the palm off the bottom of the picture first, and
    # MediaPipe finds hands by their palm, so the finger was lost exactly when the head needed to look
    # down. With the hand centred there is equal margin on every side. For a raised finger the box
    # centre sits around the knuckles, so the finger still ends up near the middle.
    xs = [p.x for p in lm]
    ys = [p.y for p in lm]
    cx = (min(xs) + max(xs)) / 2.0
    cy = (min(ys) + max(ys)) / 2.0
    lean = math.degrees(math.atan2((tip.x - knuckle.x) * w, (knuckle.y - tip.y) * h))
    if pointing < POINTING_TRACK and (not shape_allowed or abs(lean) > SHAPE_MAX_LEAN):
        return Finding(Sample(), lm, "index finger (shape) ignored")
    if pointing >= POINTING_TRACK:
        conf = _clamp(50 + 50 * pointing, 1, 100)
        label = f"Pointing_Up {pointing:.2f}"
    else:
        conf = SHAPE_ONLY_CONF
        label = "index finger (shape)"
    sample = Sample(
        dx=_clamp(cx * 200 - 100, -100, 100),
        dy=_clamp(cy * 200 - 100, -100, 100),
        conf=conf,
        lean=_clamp(lean, -90, 90),
        kind=1,
        gesture=1 if pointing >= POINTING_ARM else 0,
    )
    return Finding(sample, lm, label)


class FingerFinder:
    """MediaPipe GestureRecognizer (it bundles the palm detector and the 21-point hand landmarker)."""

    def __init__(self, model: Path, delegate: str) -> None:
        os.environ.setdefault("GLOG_minloglevel", "2")  # MediaPipe's C++ side is chatty
        import mediapipe as mp
        from mediapipe.tasks.python import vision
        from mediapipe.tasks.python.core.base_options import BaseOptions

        if delegate == "auto":
            # The macOS wheels crash in TensorsToDetectionsCalculator with the CPU delegate ("Service is
            # unavailable": it wants the Metal helper), so the GPU is the only working choice there.
            delegate = "gpu" if sys.platform == "darwin" else "cpu"
        options = vision.GestureRecognizerOptions(
            base_options=BaseOptions(
                model_asset_path=str(model),
                delegate=BaseOptions.Delegate.GPU if delegate == "gpu" else BaseOptions.Delegate.CPU,
            ),
            running_mode=vision.RunningMode.VIDEO,
            num_hands=2,
            min_hand_detection_confidence=0.5,
            min_hand_presence_confidence=0.5,
            min_tracking_confidence=0.5,
        )
        self._mp = mp
        self._recognizer = vision.GestureRecognizer.create_from_options(options)
        self._last_ts = 0
        self._last_pointing = -1e9  # time.monotonic() of the last Pointing_Up
        log.info("MediaPipe %s ready (%s delegate)", getattr(mp, "__version__", "?"), delegate)

    def analyse(self, bgr: np.ndarray) -> Finding:
        if bgr.shape[1] > INFER_WIDTH:
            f = INFER_WIDTH / bgr.shape[1]
            bgr = cv2.resize(bgr, None, fx=f, fy=f, interpolation=cv2.INTER_AREA)
        h, w = bgr.shape[:2]
        # RGBA, not RGB: the GPU path on macOS only accepts 4-channel images.
        rgba = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGBA)
        image = self._mp.Image(image_format=self._mp.ImageFormat.SRGBA, data=rgba)
        now = time.monotonic()
        ts = max(int(now * 1000), self._last_ts + 1)  # VIDEO mode wants strictly increasing
        self._last_ts = ts
        shape_allowed = now - self._last_pointing <= SHAPE_HOLD_S
        finding = pick_finger(self._recognizer.recognize_for_video(image, ts), w, h, shape_allowed)
        if finding.sample.conf > SHAPE_ONLY_CONF:
            self._last_pointing = now
        return finding

    def close(self) -> None:
        self._recognizer.close()


# --- camera connection ---------------------------------------------------------------------------------
class CamStream:
    """One /stream?raw=1&tracker=1 connection: MJPEG frames in, signed sample lines out."""

    def __init__(self, host: str, key: bytes, timeout: float = 5.0) -> None:
        self._key = key
        self._seq = 0
        self._send_lock = threading.Lock()
        self._sock = socket.create_connection((host, 80), timeout=timeout)
        try:
            self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self._rfile = self._sock.makefile("rb")
            request = (
                f"GET /stream?raw=1&tracker=1 HTTP/1.1\r\nHost: {host}\r\n"
                "User-Agent: enco-hand-tracker\r\nAccept: multipart/x-mixed-replace\r\n\r\n"
            )
            self._sock.sendall(request.encode("ascii"))
            status = self._readline().decode("latin-1").strip()
            headers = self._read_headers()
            parts = status.split(None, 2)
            if len(parts) < 2 or parts[1] != "200":
                raise ConnectionError(f"camera answered '{status}': {self._error_body(headers)}")
            nonce = headers.get("x-track-nonce", "")
            if not NONCE_RE.match(nonce):
                raise ConnectionError("no X-Track-Nonce from the camera - is its firmware up to date?")
            self._nonce = nonce
        except BaseException:
            self.close()
            raise

    def _readline(self) -> bytes:
        line = self._rfile.readline(2048)
        if not line:
            raise ConnectionError("camera closed the connection")
        if not line.endswith(b"\n"):
            raise ConnectionError("over-long header line from the camera")
        return line

    def _read_headers(self) -> dict[str, str]:
        headers: dict[str, str] = {}
        for _ in range(64):
            line = self._readline().decode("latin-1").strip()
            if not line:
                return headers
            name, sep, value = line.partition(":")
            if sep:
                headers[name.strip().lower()] = value.strip()
        raise ConnectionError("too many headers from the camera")

    def _error_body(self, headers: dict[str, str]) -> str:
        try:
            length = min(int(headers.get("content-length", "0")), 512)
            body = self._rfile.read(length) if length > 0 else b""
        except (OSError, ValueError):
            body = b""
        return body.decode("utf-8", "replace").strip() or "(no details)"

    def read_frame(self) -> tuple[bytes, bool]:
        """Next JPEG from the multipart stream, plus whether the robot has tracking armed."""
        for _ in range(8):
            line = self._readline().strip()
            if line == b"--frame":
                break
            if line:
                raise ConnectionError("lost the MJPEG framing")
        else:
            raise ConnectionError("lost the MJPEG framing")
        headers = self._read_headers()
        try:
            length = int(headers.get("content-length", ""))
        except ValueError as exc:
            raise ConnectionError("frame without a Content-Length") from exc
        if not 0 < length <= MAX_FRAME_BYTES:
            raise ConnectionError(f"implausible frame length {length}")
        data = self._rfile.read(length)
        if len(data) != length:
            raise ConnectionError("camera closed the connection mid-frame")
        if data[:2] != b"\xff\xd8":
            raise ConnectionError("frame is not a JPEG")
        return data, headers.get("x-armed") == "1"

    def send_sample(self, s: Sample) -> None:
        self._seq += 1
        body = f"S {self._seq} {s.dx} {s.dy} {s.conf} {s.lean} {s.kind} {s.gesture}"
        mac = hmac.new(self._key, f"{self._nonce} {body}".encode("ascii"), hashlib.sha256).hexdigest()[:32]
        with self._send_lock:
            self._sock.sendall(f"{body} {mac}\n".encode("ascii"))

    def close(self) -> None:
        try:
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self._sock.close()


class LatestFrame:
    """Hand-off from the reader thread: only the newest frame is kept, so a slow frame never queues up
    behind another and the head never chases where the finger used to be."""

    def __init__(self) -> None:
        self._cond = threading.Condition()
        self._item: tuple[int, bytes, bool, float] | None = None
        self._error: BaseException | None = None
        self._count = 0

    def put(self, jpeg: bytes, armed: bool) -> None:
        with self._cond:
            self._count += 1
            self._item = (self._count, jpeg, armed, time.monotonic())
            self._cond.notify()

    def fail(self, error: BaseException) -> None:
        with self._cond:
            self._error = error
            self._cond.notify()

    def take(self, after: int, timeout: float) -> tuple[int, bytes, bool, float] | None:
        deadline = time.monotonic() + timeout
        with self._cond:
            while True:
                if self._item is not None and self._item[0] > after:
                    return self._item
                if self._error is not None:
                    raise ConnectionError(str(self._error) or type(self._error).__name__)
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                self._cond.wait(remaining)


def read_frames(stream: CamStream, slot: LatestFrame) -> None:
    try:
        while True:
            slot.put(*stream.read_frame())
    except Exception as exc:  # noqa: BLE001 - any failure ends this connection; the main loop reconnects
        slot.fail(exc)


# --- preview -------------------------------------------------------------------------------------------
HAND_LINKS = (
    (0, 1), (1, 2), (2, 3), (3, 4), (0, 5), (5, 6), (6, 7), (7, 8), (5, 9), (9, 10), (10, 11), (11, 12),
    (9, 13), (13, 14), (14, 15), (15, 16), (13, 17), (0, 17), (17, 18), (18, 19), (19, 20),
)


def show_preview(bgr: np.ndarray, finding: Finding, armed: bool, fps: float) -> bool:
    """Draws what the tracker decided, at least 640 px wide. Returns False when q is pressed."""
    # The camera sends QVGA for the first second of a session, then VGA once its sensor compresses the
    # JPEG itself (cam_main.cpp, InitStreamCamera).
    scale = 2 if bgr.shape[1] < 400 else 1
    img = cv2.resize(bgr, None, fx=scale, fy=scale, interpolation=cv2.INTER_LINEAR) if scale != 1 else bgr.copy()
    h, w = img.shape[:2]
    x0, x1 = int(w * (100 - DEADBAND) / 200), int(w * (100 + DEADBAND) / 200)
    y0, y1 = int(h * (100 - DEADBAND) / 200), int(h * (100 + DEADBAND) / 200)
    cv2.rectangle(img, (x0, y0), (x1, y1), (150, 150, 150), 1)
    lm = finding.landmarks
    if lm is not None:
        pts = [(int(p.x * w), int(p.y * h)) for p in lm]
        colour = (80, 240, 40) if finding.sample.kind else (160, 160, 160)
        for a, b in HAND_LINKS:
            cv2.line(img, pts[a], pts[b], colour, 1)
        if finding.sample.kind:
            cv2.line(img, pts[5], pts[8], (0, 0, 255), 3)
            cx = int((finding.sample.dx + 100) * w / 200)
            cy = int((finding.sample.dy + 100) * h / 200)
            cv2.drawMarker(img, (cx, cy), (255, 255, 255), cv2.MARKER_CROSS, 18, 2)
    s = finding.sample
    lines = [
        f"{'ARMED' if armed else 'not armed'}   {fps:4.1f} fps",
        finding.label,
        f"dx {s.dx:+4d}  dy {s.dy:+4d}  lean {s.lean:+3d}  conf {s.conf:3d}" if s.kind else "",
    ]
    for i, text in enumerate(t for t in lines if t):
        cv2.putText(img, text, (8, 22 + 22 * i), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 0), 3)
        cv2.putText(img, text, (8, 22 + 22 * i), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (255, 255, 255), 1)
    cv2.imshow("ENCO-02 hand tracker", img)
    return (cv2.waitKey(1) & 0xFF) != ord("q")


# --- smoothing -----------------------------------------------------------------------------------------
class OneEuro:
    """One Euro filter (Casiez et al., CHI 2012): a low-pass whose cut-off rises with speed. A hand held
    "still" trembles by a few units, which at a fixed cut-off either reaches the servos as buzz or, if
    filtered hard, makes every real move lag. This filters hard only while the hand is slow."""

    def __init__(self, min_cutoff: float, beta: float, d_cutoff: float = 1.0) -> None:
        self.min_cutoff, self.beta, self.d_cutoff = min_cutoff, beta, d_cutoff
        self.x: float | None = None
        self.dx = 0.0

    @staticmethod
    def _alpha(cutoff: float, dt: float) -> float:
        tau = 1.0 / (2.0 * math.pi * cutoff)
        return 1.0 / (1.0 + tau / dt)

    def __call__(self, value: float, dt: float) -> float:
        if self.x is None:
            self.x = value
            return value
        dt = max(dt, 1e-3)
        a_d = self._alpha(self.d_cutoff, dt)
        self.dx = a_d * (value - self.x) / dt + (1.0 - a_d) * self.dx
        a = self._alpha(self.min_cutoff + self.beta * abs(self.dx), dt)
        self.x = a * value + (1.0 - a) * self.x
        return self.x


# Tuned for offsets in -100..100 at ~12 fps. At rest (1 Hz) a frame moves the output a third of the way to
# the new reading, so a 4-unit tremor becomes ~1; at 100 units/s the cut-off is 6 Hz and it follows
# almost frame for frame. Lean is filtered a little harder: it is an expression, nothing waits on it.
SMOOTH_POS = (1.0, 0.05)
SMOOTH_LEAN = (0.7, 0.02)
# A gap longer than this is a new sighting: start the filters from the new position, not from where
# the finger was before.
SMOOTH_RESET_S = 0.5


class Smoother:
    def __init__(self) -> None:
        self._reset()
        self._last = -1e9

    def _reset(self) -> None:
        self._fx, self._fy, self._fl = OneEuro(*SMOOTH_POS), OneEuro(*SMOOTH_POS), OneEuro(*SMOOTH_LEAN)

    def apply(self, s: Sample, now: float) -> Sample:
        if not s.kind:
            return s
        dt = now - self._last
        if dt > SMOOTH_RESET_S:
            self._reset()
        self._last = now
        return Sample(
            dx=_clamp(self._fx(s.dx, dt), -100, 100),
            dy=_clamp(self._fy(s.dy, dt), -100, 100),
            conf=s.conf,
            lean=_clamp(self._fl(s.lean, dt), -90, 90),
            kind=s.kind,
            gesture=s.gesture,
        )


# --- main loop -----------------------------------------------------------------------------------------
class Stats:
    def __init__(self) -> None:
        self.reset()
        self.armed: bool | None = None
        self.had_finger = False

    def reset(self) -> None:
        self.t0 = time.monotonic()
        self.frames = self.fingers = self.pointing = self.skipped = self.jpeg_bytes = 0
        self.work = 0.0
        self.size = "?"

    def report_if_due(self, every: float = 10.0) -> None:
        span = time.monotonic() - self.t0
        if span < every or self.frames == 0:
            return
        log.info(
            "%.1f fps %s %.0f KB | finger in %d%% of frames (%d%% Pointing_Up) | %d stale frames skipped"
            " | %.0f ms/frame",
            self.frames / span, self.size, self.jpeg_bytes / self.frames / 1024,
            100 * self.fingers // self.frames, 100 * self.pointing // self.frames,
            self.skipped, 1000 * self.work / self.frames,
        )
        self.reset()


def process(stream: CamStream, slot: LatestFrame, finder: FingerFinder, args: argparse.Namespace,
            stats: Stats) -> None:
    last = 0
    fps = 0.0
    smoother = Smoother()
    t_prev = time.monotonic()
    while True:
        item = slot.take(last, timeout=5.0)
        if item is None:
            raise ConnectionError("no video for 5 s")
        number, jpeg, armed, received = item
        stats.skipped += max(0, number - last - 1)
        last = number
        bgr = cv2.imdecode(np.frombuffer(jpeg, np.uint8), cv2.IMREAD_COLOR)
        if bgr is None:
            continue
        finding = finder.analyse(bgr)
        finding.sample = smoother.apply(finding.sample, time.monotonic())
        stream.send_sample(finding.sample)

        now = time.monotonic()
        fps = 0.9 * fps + 0.1 / max(now - t_prev, 1e-3) if fps else 1.0 / max(now - t_prev, 1e-3)
        t_prev = now
        stats.frames += 1
        stats.work += now - received
        stats.jpeg_bytes += len(jpeg)
        stats.size = f"{bgr.shape[1]}x{bgr.shape[0]}"
        stats.fingers += finding.sample.kind
        stats.pointing += finding.sample.gesture
        if armed != stats.armed:
            log.info("robot tracking %s", "ARMED" if armed else "not armed")
            stats.armed = armed
        if bool(finding.sample.kind) != stats.had_finger:
            stats.had_finger = bool(finding.sample.kind)
            log.info("finger %s", f"found: {finding.label}" if stats.had_finger else "lost")
        if args.verbose and finding.sample.kind:
            s = finding.sample
            log.info("dx %+4d dy %+4d lean %+3d conf %3d gesture %d", s.dx, s.dy, s.lean, s.conf, s.gesture)
        stats.report_if_due()
        if args.show and not show_preview(bgr, finding, armed, fps):
            raise KeyboardInterrupt


def run(args: argparse.Namespace) -> None:
    key = load_key()
    model = check_model()
    finder = FingerFinder(model, args.delegate)
    stats = Stats()
    backoff = 1.0
    try:
        while True:
            try:
                stream = CamStream(args.cam, key)
            except (OSError, ConnectionError) as exc:
                log.warning("camera %s: %s - retrying in %.0f s", args.cam, exc, backoff)
                time.sleep(backoff)
                backoff = min(backoff * 2, 10.0)
                continue
            log.info("streaming from %s", args.cam)
            started = time.monotonic()
            slot = LatestFrame()
            reader = threading.Thread(target=read_frames, args=(stream, slot), daemon=True)
            reader.start()
            try:
                process(stream, slot, finder, args, stats)
            except (OSError, ConnectionError) as exc:
                log.warning("connection lost: %s", exc)
            finally:
                stream.close()
                reader.join(timeout=2.0)
            stats.armed = None
            stats.had_finger = False
            # A connection that dies at once (e.g. the robot's screen took the camera) backs off harder.
            backoff = 1.0 if time.monotonic() - started > 10 else min(backoff * 2, 10.0)
            time.sleep(backoff)
    except KeyboardInterrupt:
        log.info("stopped")
    finally:
        finder.close()
        if args.show:
            cv2.destroyAllWindows()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--cam", default=os.environ.get("ENCO_CAM_HOST", DEFAULT_CAM),
                        help="camera IP or host name (default: $ENCO_CAM_HOST or %(default)s)")
    parser.add_argument("--show", action="store_true", help="preview window (q to quit)")
    parser.add_argument("--verbose", action="store_true", help="log every finger sample")
    parser.add_argument("--delegate", choices=("auto", "gpu", "cpu"), default="auto",
                        help="MediaPipe inference device (auto: GPU on macOS, CPU elsewhere)")
    parser.add_argument("--download-model", action="store_true",
                        help="download the gesture model, verify its SHA-256, and exit")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s", datefmt="%H:%M:%S")
    if args.download_model:
        download_model()
        return
    if not HOST_RE.match(args.cam):
        raise SystemExit(f"not a host name or IP address: {args.cam!r}")
    run(args)


if __name__ == "__main__":
    main()
