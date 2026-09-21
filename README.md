# ENCO-02

**ENCO-02** is a sci-fi styled desktop AI assistant robot designed for natural, expressive human-robot interaction.

## Overview

ENCO-02 combines embedded hardware control, visual tracking, and conversational AI into a compact desktop companion with a futuristic aesthetic.

## Key Features & Hardware Architecture

- **Main Controller (ESP32 IoT Board)**
  - **Audio I/O**: Drives the onboard microphone and speaker for voice interaction.
  - **Display Control**: Manages the screen interface.
  - **3-DOF Head Actuation**: Controls 3 motors to achieve expressive head movements:
    - **Nodding (Pitch)**
    - **Shaking (Yaw)**
    - **Tilting (Roll)**
- **Vision Module (ESP32-CAM)**
  - Tracks people and object movements in real time.
  - Feeds tracking signals to the motion controller to dynamically orient the robot's head toward the user or point of interest.
  - Answers "这是什么?" by taking a photo and recognising it.
- **Interactive Screen Interface**
  - Displays expressive, animated robot faces.
  - Shows real-time text messages and responses when interacting with the underlying AI backend.

## Vision module

A second, physically separate board (AI-Thinker ESP32-CAM, OV2640, 4MB PSRAM)
in the robot's head. It is flashed separately from the main board and the two
talk over a UART.

### Why two boards and a serial cable

The main board has no PSRAM, and while the assistant is speaking its largest
free contiguous heap block is about **2 KB** — the Wi-Fi driver has been logged
failing a 2,308 byte allocation there. A JPEG is 25–40 KB. So the main board
can never touch the image.

Instead the camera board does everything image-shaped itself and sends back
text. The main board only ever handles one short sentence.

### Wiring

Cross-over, and **a common ground is mandatory**:

| ESP32-CAM | → | Main board |
|---|---|---|
| GPIO 13 (TX) | → | GPIO 18 |
| GPIO 14 (RX) | ← | GPIO 19 |
| GND | ↔ | GND |
| 5V | ↔ | 5V |

GPIO 13/14 are the only pins on the ESP32-CAM that are both free with the
OV2640 attached and not boot strapping pins. Using them means **no microSD
card** may be fitted; nothing is stored locally, so this costs nothing.

> The ESP32-CAM peaks around 310 mA. Give it 5V from the same regulator as the
> main board (not a 3V3 pin) and put a 470 µF capacitor across 5V/GND right at
> the module, or it will brown out the first time it takes a picture.

### Building it

```sh
cp firmware/cam/cam_config.h.example firmware/cam/cam_config.h
# fill in Wi-Fi credentials and a vision API key
pio run -e enco02_cam -t upload
```

`cam_config.h` is gitignored. A bare ESP32-CAM needs a USB-serial adapter with
GPIO 0 pulled to GND while resetting; the ESP32-CAM-MB shield does this for you.

### What it exposes to the assistant

| MCP tool | spoken trigger |
|---|---|
| `self.camera.look` | 这是什么 / 你看到了什么 / 帮我看看 |
| `self.camera.track_on` | 看着我 / 跟着我 |
| `self.camera.track_off` | 别看我了 |

Head tracking is on by default. It is suppressed while a gesture animation is
running and for 4 seconds after any explicit head command, so "向左转头" is not
immediately undone.
