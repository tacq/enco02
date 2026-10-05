# ENCO-02 App ⇄ Robot protocol (v1)

This is the contract the phone app is written against. Endpoints marked
**existing** are already served by
[`servo_web_server.cpp`](../../firmware/main/servo_web_server.cpp); everything
marked **new** still has to be added to the firmware. The mock robot in
[`tool/mock_robot.dart`](../tool/mock_robot.dart) implements all of it so the
app can be developed before the firmware catches up.

Conventions:

- Plain HTTP on port 80. Requests use `GET` for reads and `POST`
  (`application/x-www-form-urlencoded`) for writes. The firmware's
  `WebServer::arg()` reads both query and form body, so existing handlers
  accept the POST form unchanged.
- Responses are small JSON objects (< 2 KB, most < 300 B).
- Errors: `401` missing/bad token, `403` missing/bad admin key, `404` unknown
  name, `409` busy (speaking, animating, OTA running), `503` feature not
  available right now. Body: `{"ok":false,"error":"<short reason>"}`.

## 1. Pairing (SoftAP, robot at `192.168.4.1`)

The robot enters pairing mode when it has no saved Wi-Fi, or after
`POST /api/wifi/reset`, or on the reset voice command.

### QR code shown on the LCD

```
enco://pair?v=1&id=ENCO-7F3A&ap=ENCO-7F3A&pw=<ap password>&s=<pairing secret>
```

| field | rule |
|---|---|
| `v` | protocol version, `1` |
| `id` | device id, `[A-Za-z0-9-]{4,32}`, stable per board (from MAC) |
| `ap` | SoftAP SSID, 1-32 bytes |
| `pw` | SoftAP WPA2 password, 8-63 printable ASCII, random per pairing session |
| `s`  | pairing secret, base64url, 16-64 chars, random per pairing session |

`pw` and `s` are generated with `esp_random()` (hardware RNG, with RF on)
each time pairing mode starts, and never leave the screen except through the
QR code. The AP is no longer open.

### Endpoints (all require header `X-Pair-Secret: <s>`)

| | request | response |
|---|---|---|
| `GET /prov/info` | | `{"id":"ENCO-7F3A","name":"ENCO-02","proto":1,"fw":{"main":"1.0.0","cam":"1.0.0"}}` |
| `GET /prov/scan` | | `{"networks":[{"ssid":"Home","rssi":-52,"secure":true}]}` (2.4 GHz only, deduplicated, strongest first, max 20) |
| `POST /prov/wifi` | `ssid`, `password` | `202 {"ok":true}`; robot starts STA connect **while keeping the AP up** |
| `GET /prov/result` | | `{"state":"idle\|connecting\|connected\|failed","ip":"192.168.1.42","port":80,"token":"<base64url 32B>","reason":"auth_failed\|no_ap\|timeout"}` (`ip`/`port`/`token` only when connected, `reason` only when failed) |
| `POST /prov/finish` | | `{"ok":true}`; robot saves Wi-Fi + token to NVS, drops the AP after ~2 s, and starts normally |

The **device token** is generated when `connected` is reached. The robot stores
it in NVS (namespace `enco_app`, key `token`). Only one paired phone is
supported in v1; pairing again replaces the token.

## 2. Discovery on the home LAN

The DHCP address can change, and mDNS is too expensive for this board. So:

- App → broadcast `255.255.255.255:48902` UDP: `{"q":"enco","id":"ENCO-7F3A"}`
  (`id` may be `"*"`).
- Robot → unicast reply to the sender:
  `{"id":"ENCO-7F3A","name":"ENCO-02","ip":"192.168.1.42","port":80,"proto":1}`.

The robot replies only when `id` matches or is `*`. Its replies carry no secrets.

## 3. Robot API (home LAN), header `Authorization: Bearer <token>`

### Info & status

| endpoint | status | notes |
|---|---|---|
| `GET /api/info` | new | `{"id","name","proto":1,"fw":{"main","cam"},"characters":["k3","fox"],"expressions":["happy",...]}` |
| `GET /api/status` | existing (+fields) | adds `"rssi"`, `"character"`, `"caption"`, `"head_auto"`, `"uptime_s"` |
| `POST /api/name` | new | `name` (1-24 UTF-8 bytes) |

### Control (normal user)

| endpoint | status | params |
|---|---|---|
| `POST /api/servo` | existing | `pin` (0 pitch, 25 roll, 26 yaw), `angle`; firmware clamps to limits |
| `POST /api/center` | existing | |
| `GET /api/anim` | existing | list |
| `POST /api/anim` | existing | `name` \| `random=1` \| `stop=1` \| `auto=0/1` |
| `POST /api/track` | existing | `on=0/1` |
| `POST /api/volume` | existing | `value` 0-100 |
| `POST /api/ui_mode` | existing | `mode=face\|chat` |
| `POST /api/say` | existing | `text` 1-120 bytes |
| `POST /api/character` | new | `name` (`k3`, `fox`) |
| `POST /api/expression` | new | `name` from `/api/info` |
| `POST /api/caption` | new | `on=0/1` |
| `POST /api/wifi/reset` | new | forget Wi-Fi + token, reboot into pairing mode |
| `POST /api/unpair` | new | forget token only (robot keeps Wi-Fi, shows QR again for re-pair) |

### Developer / admin, additionally header `X-Admin-Key: <key>`

The admin key is `APP_ADMIN_KEY` in `home_config.h`, which is gitignored. If
it is not defined, every admin endpoint returns 403 (fail closed).

| endpoint | params / response |
|---|---|
| `GET /api/admin/limits` | `{"axes":[{"axis":0,"pin":0,"min":40,"max":120,"center":90},...]}` |
| `POST /api/admin/limits` | `axis`, `min`, `max`, `center`, optional `save=1` (persist to NVS) |
| `POST /api/speed` | existing: `axis`+`value` \| `test` \| `save=1` (moved behind admin) |
| `POST /api/track` | existing: `flip=yaw\|pitch\|roll`, `reset_dir=1` (admin) |
| `GET /api/admin/diag` | `{"heap":..,"min_heap":..,"largest_block":..,"uptime_s":..,"rssi":..,"cam":bool,"reset_reason":"..."}` |
| `POST /api/admin/reboot` | |

## 4. Firmware updates (OTA)

See [firmware_updates.md](firmware_updates.md) for the design. Endpoints:

| endpoint | notes |
|---|---|
| `GET /api/ota/status` | `{"state":"idle\|checking\|available\|downloading\|verifying\|installing\|done\|failed","target":"main\|cam","progress":0-100,"error":"...","available":[{"target":"main","version":"1.2.0","size":123,"notes":{"en":"..","zh":".."}}]}` |
| `POST /api/ota/check` | robot fetches the manifest itself, and fills `available` |
| `POST /api/ota/start` | `target`; optional `version`, `url`, `sha256`, `size` (when the app found the update). Without them the robot installs what it found in `available`. `409` if already running or the robot is busy |
