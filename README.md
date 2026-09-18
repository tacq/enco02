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
- **Interactive Screen Interface**
  - Displays expressive, animated robot faces.
  - Shows real-time text messages and responses when interacting with the underlying AI backend.
