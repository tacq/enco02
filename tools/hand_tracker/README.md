# ENCO-02 hand tracker

Finds one raised finger in the head camera's video and steers the head to it.

This is how every "ESP32-CAM gesture tracking" demo works: the ESP32-CAM only
streams video, and a computer runs Google MediaPipe on it. The camera cannot run
a hand-recognition network itself. Its own skin-colour finder
(`firmware/cam/cam_tracker.cpp`) is only a fallback for when this program is not
running, and it mistakes beige walls and furniture for fingers.

```
camera   --MJPEG  GET /stream?raw=1&tracker=1-->   hand_tracker.py (MediaPipe GestureRecognizer)
tracker  --"S seq dx dy conf lean kind gesture mac", same socket-->   camera
camera   --UART "G 1" / "T dx dy conf lean 1"-->   main board, which moves the servos
```

The camera keeps every decision it used to make: the gesture debounce, when to
send `T` lines, and the main board's speed and angle limits. Only the finger
finding moves to the computer.

## What it does

- **Arming:** hold up one index finger (MediaPipe's `Pointing_Up`) for about
  0.7 s. The camera sends `G 1` and the main board switches tracking on.
- **Following:** while tracking is on, the head turns and nods to bring the
  middle of your index finger (knuckle to tip) to the centre of the picture. It
  mirrors the finger's tilt with 歪头. The main board stops following 20 s after
  the finger goes away.
- **Rejected:** open hands, fists, and anything smaller than 12% of the
  picture's height (posters, reflections).

## Setup (once)

```sh
cd tools/hand_tracker
/opt/homebrew/bin/python3.13 -m venv .venv
.venv/bin/pip install -r requirements.txt
.venv/bin/python hand_tracker.py --download-model   # fetches the model and checks its SHA-256
```

**Shared key.** The camera accepts samples only if they are signed with its key.
Put the same value in two gitignored files:

1. `firmware/cam/cam_config.h`: add `#define CAM_TRACK_KEY "<key>"`, then
   reflash the camera.
2. `tools/hand_tracker/.env`: add `ENCO_TRACK_KEY=<key>`, then run `chmod 600 .env`.
   `.env.example` shows the format.

To make a key, run `python3 -c "import secrets; print(secrets.token_hex(32))"`.
Never commit either file.

## Run

```sh
cd tools/hand_tracker
.venv/bin/python hand_tracker.py            # camera at $ENCO_CAM_HOST, default 192.168.86.65
.venv/bin/python hand_tracker.py --show     # with a preview window (q quits)
.venv/bin/python hand_tracker.py --verbose  # log every finger sample
```

To have it start at login and restart after a crash, run `./install_launchd.sh`.
Its log goes to `~/Library/Logs/enco-hand-tracker.log`. To remove it, run
`launchctl bootout gui/$(id -u)/com.enco.handtracker`.

The log reports once every 10 s. Example:

```
14.8 fps 640x480 21 KB | finger in 0% of frames (0% Pointing_Up) | 3 stale frames skipped | 12 ms/frame
```

The first second of each connection runs at about 4 fps in 320x240. After that
the camera's sensor switches to compressing JPEG itself at 640x480.

## What changes on the camera while it runs

| | Tracker connected | Tracker not running |
|---|---|---|
| Who finds the finger | MediaPipe on this computer | the camera's skin-colour finder |
| Sensor mode | hardware JPEG, 640x480 | RGB565, 320x240 |
| Frame rate | ~15 fps while armed, ~7 idle | ~12 / ~6 |
| `/stream` in a browser | 409; the page falls back to 1 s snapshots | live, with overlay |
| `/snapshot.jpg` | raw frame, no overlay | overlay drawn in |

You can watch it from `http://<camera>/status`:

- `ext` is 1 while the tracker is steering.
- `ext_ok` and `ext_bad` count accepted and rejected samples.
- `sensor` and `res` show the sensor mode.

The robot's screen viewfinder always wins the camera. While it is on, the
tracker is disconnected and keeps retrying.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `camera answered '503 ...'` | no `CAM_TRACK_KEY` in the camera's firmware |
| `no X-Track-Nonce from the camera` | camera firmware older than the tracker protocol |
| `ext_bad` keeps climbing in `/status` | the two keys differ |
| connection drops every few seconds | the robot's screen viewfinder is on, or two trackers are running (e.g. the login agent and a manual run) and keep taking the camera from each other |
| MediaPipe aborts with `Service is unavailable` | the CPU delegate on macOS; keep `--delegate auto` (GPU) |

## Security

- Every sample line carries HMAC-SHA256 over a per-connection nonce and a
  strictly increasing sequence number.
- The camera rate-limits lines, checks the MAC in constant time, rejects
  replays, and drops a connection after 20 bad lines. See
  `firmware/cam/cam_remote.h`.
- The key is never logged. The script warns if `.env` is readable by other
  users.
- The video itself is not authenticated or encrypted. Anyone on the same Wi-Fi
  can watch it. Keep the camera on the home LAN and never forward port 80.
- The model is downloaded over HTTPS from a fixed, versioned URL and kept only if
  its SHA-256 matches.
