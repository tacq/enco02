# ENCO-02 User Manual

**English** | [简体中文](MANUAL.zh-CN.md)

ENCO-02 is a desktop AI companion robot. It has an animated character face on a 240×320 screen, a 3-axis motorised head, a camera "eye", and voice control through the XiaoZhi cloud assistant. It can also run your pool and spa through a small bridge on your Mac.

Features below are **sorted by cool level**, most impressive first. Every voice phrase listed is one the firmware or the assistant recognises. You don't need exact wording: the cloud model understands paraphrases, and the most important commands are also matched on the device itself, so they work even when the model only chats back.

---

## Quick start

1. **Power on.** The screen shows the face and the status bar. On the very first boot, see [Wi‑Fi setup](#15-wi-fi-setup-hotspot-portal).
2. **Wake her:** say **"Hi 安可"** (also "安可", "Hi ENCO", "你好"). The status changes to **聆听中** (listening).
3. **Talk normally.** She answers out loud, the caption bar shows the text, and her face reacts.
4. **Send her to rest:** say **"退下" / "去休息" / "待命"**, or say nothing for 120 s.

---

## ★★★★★ 1. Living character face

The screen is a hand-painted K3 "elegant bob" character, drawn in full colour straight from flash.

| Behaviour | What happens |
|---|---|
| **Blinking** | Open → half → shut → half → open (~240 ms), every 2.4–6 s at random, so it never looks mechanical |
| **Lip-sync with jaw** | The mouth and jaw follow the actual speaker output (not the network state), so the lips move exactly while audio plays and stop within 200 ms of silence |
| **Hair breeze** | Bangs and side locks sway gently every 1.6–4.8 s |
| **Emotion reactions** | Every AI reply carries a mood (21 kinds). The face shows the matching expression for 2.5 s after she stops talking |
| **Random ambient expressions** | Idle: a calm expression every 10–25 s, held 2–3.6 s. Listening: an attentive one every 2.4–5 s, held 1.6–2.4 s. **On by default.** Say **"关闭随机表情"** to turn off, **"打开随机表情"** to turn back on (the setting survives reboots) |

### Expressions on demand (voice)

| Say | Expression |
|---|---|
| "开心一点" / "笑一个" | 😊 **happy** |
| "难过" / "伤心的表情" | 😢 **sad** |
| "眨个眼" / "wink" | 😉 **wink** |
| "嘟嘴卖萌" / "卖个萌" | 😗 **pout** |
| "惊讶" / "吓一跳" | 😮 **surprised** |
| "做个生气的表情" | 😠 **angry** |
| "害羞" | ☺️ **shy** (soft blush) |
| "思考一下" / "想一想" | 🤔 **thinking** |
| "恢复表情" / "正常一点" | back to **neutral** |

- A face you ask for stays **4 s after she finishes speaking**. Her own mood changes can't override it during that time.
- If the screen is in chat-text mode, asking for an expression switches back to the face.
- **Angry** is never picked at random. It only appears when you ask for it.

How the server's moods map to faces:

| Server emotion | Face |
|---|---|
| happy, laughing, funny, delicious | happy |
| sad, crying | sad |
| winking, silly, cool | wink |
| kissy, loving | pout |
| surprised, shocked | surprised |
| angry | angry |
| embarrassed | shy |
| thinking, confused | thinking |
| neutral, relaxed, confident, sleepy | face left alone (sleepy keeps her eyes shut) |

---

## ★★★★★ 2. "这是什么?": camera vision

Hold something up and ask. The ESP32‑CAM in her head takes a photo, has it recognised, and she tells you what it is.

| Say | Result |
|---|---|
| "这是什么?" | Identifies the main object |
| "看看我手里拿的是什么" | Focuses on what you're holding |
| "你看到了什么?" / "帮我看看" | Describes what is in front of her |
| "这个字写的什么?" (any question) | The question is passed along with the photo |

- The screen briefly shows a **viewfinder** while the photo is taken, then returns to the face with **已识别** ("recognised").
- Answers are kept to about 20 characters so they're quick to speak.
- The image never passes through the main board: the camera board does the capture, upload and recognition itself, and sends back one sentence.

---

## ★★★★☆ 3. Head tracking: she follows your finger

Hold up **one finger** (the rest of the hand in a fist) and the 3‑axis head follows it. Yaw (转头) and pitch (抬头/低头) keep the fingertip in the middle of the picture. Roll (歪头) tilts the head the same way the finger leans.

| How | Action |
|---|---|
| **Hold up one finger** to the camera for ~1 s | Tracking on (the status bar shows **跟踪开启**) |
| Say **"开启跟踪" / "跟着我"**, or press **📷 跟踪** on the web page | Tracking on |
| Say **"别看我了" / "停止跟踪"**, or press the button again | Tracking off |

- Tracking is **off after every boot**.
- Turning tracking on stops any running animation, and random animations stay paused while it is on.
- It pauses during a head gesture and for 4 s after any head command, so "向左转头" isn't immediately undone.
- **Finger gone:** after 4 s the head glides back to the neutral pose and waits. If the gesture turned tracking on, tracking turns itself off after 20 s with no finger and random animations resume. Tracking turned on by voice or the web page stays on until you turn it off.
- **Roll is open loop.** The camera is not on the roll stage, so tilting the head does not rotate the picture and cannot be checked by the camera. The head simply tilts by as much as the finger leans past ~6°, up to ±20°.
- **Self-calibrating direction (yaw/pitch).** The camera rides on the yaw and pitch stages, so turning the right way pulls the fingertip toward centre. After each ~4° of movement she checks, using only confident detections. If the fingertip got *further* from centre twice in a row, or she is pinned on an end stop with the finger still off to that side for 2 s, that axis was backwards. She flips it and saves it, and the serial log shows `[track] ... flipped`.
- The web page shows the current 左右 / 上下 / 歪头 directions, with buttons to flip each by hand or 恢复默认. Roll cannot learn its direction, so if she tilts the opposite way to your finger, press **歪头方向**.
- Needs the finger-tracking camera firmware (`enco02_cam`). With the old face-tracking firmware the serial log says so once and the head does not move.

### Who finds the finger

**Best: the hand tracker on the Mac** (`tools/hand_tracker`; setup is in its README).
- The ESP32‑CAM can't run a hand-recognition network. Like every ESP32‑CAM gesture project, it streams video to a computer.
- On the Mac, Google MediaPipe finds the hand and sends the finger position back. Each sample is signed, so nothing else on the Wi‑Fi can steer the head.
- While the tracker runs, the camera's sensor compresses JPEG itself: 640×480, ~15 fps while tracking, ~7 fps while waiting for the gesture.
- **Start it:** run `tools/hand_tracker/.venv/bin/python tools/hand_tracker/hand_tracker.py`, or install it as a login agent with `tools/hand_tracker/install_launchd.sh`.
- **Check it:** `http://192.168.86.65/status` shows `"ext":1` and `"sensor":"jpeg"`.

**Fallback: the camera's own skin-colour finder.** It takes over automatically whenever the Mac tracker isn't connected. It needs no computer, but it can mistake skin-coloured walls or furniture for a finger.

---

## ★★★★☆ 4. Pool & spa control by voice

Needs the home bridge running on your Mac (see [§16](#16-home-bridge-on-the-mac)). Your iAqualink password stays on the Mac and never leaves your home network; the robot only receives short text summaries.

| Say | What it does |
|---|---|
| "泳池状态" / "pool status" / "SPA 好了吗?" | Reads the status aloud and shows a **status card** on screen until she finishes speaking |
| "打开过滤泵" / "关掉过滤泵" | `pool_pump` on/off (filter pump) |
| "打开 SPA 模式" / "关闭 SPA" | `spa_pump` on/off (spa mode) |
| "打开泳池加热" / "打开 SPA 加热" | `pool_heater` / `spa_heater` |
| "打开泳池灯" / "打开 SPA 灯" | `pool_light` / `spa_light` |
| "打开清洁机" | `cleaner` |
| "泳池温度设到 82 度" | `pool_set`, range **70–90 °F** |
| "SPA 温度调到 100 度" | `spa_set`, range **80–104 °F** |

- Temperatures are in **°F**.
- After a change the bridge re-reads the panel for up to about 6 s, to confirm it really happened.
- If she says **"连接不上家庭网关"**, the Mac is asleep or the bridge isn't running. See [Troubleshooting](#troubleshooting).

---

## ★★★★☆ 5. Live camera viewfinder

| Say | Result |
|---|---|
| "打开摄像头" / "开启相机" | Full-screen live picture from the camera, with a sci‑fi HUD: blinking **REC**, telemetry line, character thumbnail |
| "关闭摄像头" / "退出" / "不看了" / "返回" | Back to the face |

- While the viewfinder is open she doesn't go to standby.
- The stream pauses automatically while she speaks, to save memory.

---

## ★★★☆☆ 6. Head movement by voice

| Say | Movement | Face |
|---|---|---|
| "抬头" / "往上看" / "看天花板" | Look up 10° | happy |
| "低头" / "往下看" / "看地面" | Look down 10° | thinking |
| "向左歪头" / "左偏头" | Tilt left 10° | wink |
| "向右歪头" / "右偏头" | Tilt right 10° | wink |
| "向左转头" / "往左看" | Turn left 10° | surprised |
| "向右转头" / "往右看" | Turn right 10° | surprised |
| "摇头" / "摇头晃脑" / "不要不要" | Playful ~3 s shake: yaw ±28°, tilt ±17°, nod ±8° | laughing |
| "头摆正" / "向前看" / "头复位" | Centre all axes | neutral |
| "随机动作" / "做个动作" | One random predefined animation | – |
| "开始随机动作" | Random animations on: standby and awake, every 6–12 s (saved; also the 🤖 button on the web page) | – |
| "停止随机动作" | Random animations off: stop and ease back to centre (saved) | – |

All moves ease in and out (cosine curve): soft start, soft stop, ~30°/s average, never faster than 0.45 s per move.

Repeated commands within 2.5 s are ignored, so one sentence can't move the head twice.

---

## ★★★☆☆ 7. Wake word & standby companion mode

| State | Behaviour |
|---|---|
| **Standby** (caption: *待命中 · 说 "Hi 安可" 唤醒*) | Speaker muted. Idle life: random mood every 4.5–9 s, a predefined head animation every 6–12 s (look around, curious tilt, glance, nod, breathe, sleepy, shy, think, 摇头晃脑…; never the same twice in a row) |
| **Wake** | "Hi 安可", "安可", "Hi ENCO", "你好", "小智"; timer or camera requests also wake her directly |
| **Awake** | Listens and answers; your volume is restored |
| **Back to standby** | "退下" / "去休息" / "待命", or 120 s with no conversation |

She never drops to standby while a countdown or the camera viewfinder is running.

---

## ★★★☆☆ 8. Countdown timer & reminders

The countdown runs **on the robot**, so it keeps going even after the conversation ends.

| Say | Result |
|---|---|
| "5 分钟后提醒我" / "定个五分钟的闹钟" | 5:00 countdown |
| "一个半小时后叫我" | 1:30:00 |
| "一分半倒计时" / "1分30秒" / "90 seconds" | 1:30 |
| "提醒我 10 分钟后喝水" | Countdown **with a note**: the alert card says 喝水 |
| "还剩多久?" | Seconds left |
| "取消定时器" / "不用提醒了" | Cancel |

- Chinese numbers work naturally: 两, 二十五, 半, 一个半, 一分半.
- The maximum is **24 h**.
- While running, a **T‑MINUS panel** takes over the caption spot.
- When it fires:
  - the face looks surprised and **时间到** appears on screen;
  - she wakes and announces it, e.g. "五分钟时间到", or "…提醒我喝水" when it's short enough;
  - an alert card is shown.
- If she's busy talking when the time is up, she keeps retrying the announcement for up to 30 s.

---

## ★★★☆☆ 9. On-screen notes (toasts)

| Say | Result |
|---|---|
| "提醒我明天带伞" / "记一下…" / "通知我…" | A small card on screen (title ≤ 8 characters, up to 3 short lines of body) that fades out after about 5 s |
| "知道了" / "取消提醒" | Dismiss it |

Only one card is shown at a time; a new one replaces the old one.

---

## ★★☆☆☆ 10. Smart volume control

| Say | Result |
|---|---|
| "大声一点" / "声音大点" / "调大音量" | +10 |
| "小声一点" / "轻一点" / "安静点" | −10 |
| "太小声了" / "听不见" | +10 (understood as a complaint) |
| "太吵了" / "太大声" | −10 |
| "音量调到 50" | Set exactly (0–100) |
| "现在音量多少?" | Tells you the level |

- Questions ("声音大吗?") are never mistaken for commands.
- Relative changes never go below 10, and "quieter" never turns up a level that is already lower (or a mute).
- **Mute:** say "音量调到 0", or press **静音** on the web page. She stays muted through standby, wake-ups and reboots until you raise the volume. Timer announcements are silent too, but still shown on screen.
- **Remembered across power-off.** The level is saved about 3 s after your last change (never while she is talking) and restored at the next boot. **20 %** is only the default on a fresh board.
- The volume icon in the status bar updates every time. In standby the speaker is parked silent, but the page and "现在音量多少?" still report your level, and a change made in standby takes effect when she wakes.

---

## ★★☆☆☆ 11. Caption bar

The pill at the bottom shows what you said (**你:**), her replies (**Enco:**), and status hints.

- **One line.** Long text scrolls sideways as a smooth loop at about 3 characters per second. Short text stays centred.
- **Voice toggle:**
  - "打开字幕" shows it;
  - "关闭字幕" hides it;
  - "字幕" toggles.
- **The setting survives reboots.** A running countdown still uses that spot.

---

## ★★☆☆☆ 12. Status bar

| Position | Item |
|---|---|
| Left | **Clock**, `07:24 PM`. Synced from the internet after Wi‑Fi connects, re-synced hourly, US Pacific time with automatic daylight saving. Shows `--:--` until synced |
| Centre | Status text (聆听中, 说话中, 待命, …), or the countdown |
| Right | **Volume** icon, then **Wi‑Fi** icon: full / fair (dim) / weak (amber) / off |

---

## ★★☆☆☆ 13. Screen modes

| Say | Result |
|---|---|
| "切换到对话模式" / "显示文字" | Chat-text mode: a transcript view instead of the face |
| "切换到表情模式" | Back to the character |
| "切换屏幕" | Toggle between the two |

---

## ★☆☆☆☆ 14. Web debug page

The page is **off by default** to save memory. Turn it on by voice:

| Say | Effect |
|---|---|
| "打开调试页面" | Starts the page; a toast shows the IP and she reads out `http://<robot-ip>/` |
| "关闭调试页面" | Stops it |

- It switches itself off after **10 minutes**. Say "打开调试页面" again to reset the timer.
- It also stops if free memory drops below 5 KB, or when the camera view opens.
- If free memory is below 14 KB, she refuses and asks you to try later.
- Open it by IP; `enco02.local` (mDNS) was removed to save memory.
- **Testing build:** with `kDebugServerAlwaysOn = true` (main.cpp) the page starts by itself once she is idle, never times out, and is paused only while she speaks.

**Page controls:** servo sliders (moves glide, never snap) · centre / bobble / sweep · screen mode ·
**🎬 predefined animations** (🤖 random on/off switch, one button each, 🎲 random, ⏹ stop) · **⏱ speed** per axis (抬头 / 歪头 / 转头, 10–120 °/s, "试一下" test, "保存为默认" saves to flash) ·
**🔊 volume** · **camera tracking** on/off · **💬 text box** (sends text as if you had said it).

| Endpoint | Use |
|---|---|
| `/` or `/servo` | Servo test page |
| `/api/status` | JSON status (angles, heap, volume, tracking) |
| `/api/servo` | Glide a servo to an angle (GET/POST) |
| `/api/center` | Centre the head |
| `/api/sweep` | Sweep test |
| `/api/bobble` | Head bobble |
| `/api/ui_mode` | Get/set face vs. chat mode |
| `/api/anim` | List animations; `?name=nod` play; `?random=1`; `?stop=1` |
| `/api/speed` | Get speeds; `?axis=0..2&value=`; `?test=0..2`; `?save=1` |
| `/api/volume` | Get; `?value=0..100` set |
| `/api/track` | Get; `?on=0\|1`, `?flip=yaw\|pitch\|roll`, `?reset_dir=1` |
| `/api/say` | POST `text=` (1–120 bytes) |

---

## ★☆☆☆☆ 15. Wi‑Fi setup (hotspot portal)

If no Wi‑Fi is saved, or it can't connect:

1. The screen shows **【Wi‑Fi 配网模式】** with a hotspot name.
2. Join that hotspot from your phone.
3. Open **`192.168.4.1`** in a browser, pick your network (or type the SSID), and enter the password.
4. She saves it, connects, and plays a "network connected" chime.

The credentials are stored on the device and survive reboots and firmware updates.

---

## ★☆☆☆☆ 16. Home bridge on the Mac

`tools/home_bridge/` is the only thing that knows your iAqualink login (in `.env`, never committed).

| Command | Purpose |
|---|---|
| `tools/home_bridge/install_launchd.sh` | Install as a background service: starts at login, restarts if it crashes, and keeps the Mac from idle-sleeping. Log: `~/Library/Logs/enco-home-bridge.log` |
| `python bridge.py discover` | List devices on the pool system |
| `python bridge.py status` / `summary` | Print raw status / the exact text the robot would get |
| `python bridge.py set <target> on\|off\|set <value>` | Test control from the terminal |
| `python bridge.py serve --host <mac-ip> --port 8787 --allow-control` | Run the server by hand |

- **Security:**
  - every request is signed (HMAC-SHA256) with a one-time nonce;
  - it is rate-limited to 20 requests per minute;
  - only allow-listed devices and ranges are accepted.
- **Power:** a MacBook still sleeps when the **lid is closed on battery**. Keep it plugged in with the lid open, or move the bridge to an always-on machine such as a Raspberry Pi.

---

## ★☆☆☆☆ 17. Face-art pipeline (for designers)

`python3 tools/face_assets/build_face_assets.py firmware/main` turns the source renders in `tools/face_assets/source/` into flash-ready RGB565 sprites.

- The work is done at 2× resolution, with automatic alignment of each variant.
- Edges are feathered and blended, then scaled down so they stay smooth.
- Hair sway frames are generated automatically.
- A per-sprite tweak table can soften effects; the shy blush uses one.
- Pixel-exact previews are written to `tools/face_assets/build/frame_*.png`.

---

## Hardware & memory limits

### Boards

| Part | Detail |
|---|---|
| Main board | ESP32‑D0WD‑V3 (dual core, 240 MHz), **520 KB SRAM, no PSRAM**, **4 MB flash** |
| Camera board | AI‑Thinker ESP32‑CAM (OV2640, 4 MB PSRAM), linked by UART |
| Display | 240×320 SPI LCD, RGB565 |
| Head | 3 servos: pitch, yaw, roll |

### Flash (4 MB = 4,194,304 bytes)

| Partition | Offset | Size | Use |
|---|---|---|---|
| bootloader + partition table | 0x0000 | 36 KB | Boot |
| nvs | 0x9000 | 20 KB (20,480 B) | Wi‑Fi login, volume, caption setting |
| otadata | 0xE000 | 8 KB | Unused (kept for compatibility) |
| **app0** | 0x10000 | **3,968 KB (4,063,232 B)** | Firmware |
| coredump | 0x3F0000 | 64 KB | Crash dumps |

There is **only one app slot**, so over-the-air updates aren't possible. Every update goes over USB.

**Firmware usage (current build):**

| Item | Bytes | Share of app0 |
|---|---|---|
| **Firmware total** | **3,469,862** | **85.4 %** |
| **Free** | **593,370 (~579 KB)** | 14.6 % |
| └ Character face art (base + 33 sprites) | 595,576 | 14.7 % |
| └ Chinese UI font (puhui 16 px), glyph bitmaps | ~488,000 | ~12 % |
| └ Icon fonts (Font Awesome 16/30 px) | ~14,400 | 0.4 % |
| └ Audio prompts (3 MP3s) | ~16,200 | 0.4 % |
| └ Code + libraries (Wi‑Fi, TLS, LVGL, Opus, wake word, …) | rest (~2.36 MB) | ~58 % |

Linker sections: `.flash.text` 1,335,500 B (code), `.flash.rodata` 2,014,060 B (constant data including art and fonts), `.iram0.text` 93,559 B.

**What still fits in the ~579 KB free:**

| Addition | Approx. cost | Fits? |
|---|---|---|
| One more expression (eyes + mouth sprite) | ~24 KB | ✅ about 15–20 more, keeping a safety margin |
| Longer / more voice prompts (MP3) | 3–8 KB each | ✅ |
| A second full character (base + all sprites) | ~600 KB | ❌ |
| Full-screen animations (150 KB per frame) | 150 KB/frame | ❌ beyond 2–3 frames |

### RAM (520 KB SRAM, no PSRAM)

Static: `.dram0.data` 25,716 B plus `.dram0.bss` 51,424 B ≈ 77 KB.

| Moment | Free heap | Largest block |
|---|---|---|
| Boot, before audio buffers | ~215 KB | ~108 KB |
| After display + LVGL | ~158 KB | ~108 KB |
| Wi‑Fi connected | ~88 KB | ~76 KB |
| **Standby** | **~83 KB** | ~40–50 KB |
| **Listening / speaking** | **~20–22 KB** | ~13–17 KB |
| Lowest seen | ~5–8 KB | — |

**What this means in practice:**
- **Talking is the tight moment.** Wi‑Fi needs 2,308-byte blocks while she speaks, so every feature is designed to cost almost no RAM then.
- **The face art is free in RAM.** It is drawn straight from flash.
- **The camera is a separate board.** A single JPEG (25–40 KB) is bigger than the free memory during a conversation.
- **The voice-command list has a fixed buffer.** The list sent to the assistant uses **4,736 of its 5,120 bytes**. Going past that doubles the buffer and costs 5 KB of RAM for the whole session, so any new voice feature needs its description trimmed elsewhere.
- **Display task stack:** 8,704 B, about 1.4 KB spare at peak.

**Upgrade path:** an ESP32‑S3 N16R8 (16 MB flash, 8 MB PSRAM) would remove both the flash and the RAM limits.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| "连接不上家庭网关" | The Mac is asleep or the bridge is stopped. Check `launchctl print gui/$(id -u)/com.enco.homebridge`, and keep the Mac plugged in with the lid open |
| Clock shows `--:--` | Not synced yet (needs internet); give it a few seconds after Wi‑Fi connects |
| No caption bar | You turned it off. Say "打开字幕" |
| Head won't follow | Tracking is off after boot. Hold up one finger for ~1 s, or say "开启跟踪". The camera needs the finger-tracking firmware. For reliable detection, run the Mac hand tracker (`tools/hand_tracker`) |
| Head chases something that isn't a finger | The camera's fallback finder saw a skin-coloured object. Run the Mac hand tracker, which recognises real hands |
| Web debug page stops answering during a conversation | On purpose: requests wait until free heap is back above 16 KB (standby), because answering them mid-conversation ran the board out of memory |
| She's silent in standby | By design: the speaker is muted in standby. Say "Hi 安可" |
| Silent even when awake (status bar shows the muted speaker) | Volume is 0, and a mute is remembered across reboots. Say "大声一点" / "音量调到 30", or use the web page |
| Wi‑Fi changed | Wait for the setup hotspot to appear and redo [§15](#15-wi-fi-setup-hotspot-portal) |
