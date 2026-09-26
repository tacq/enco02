#pragma once

#ifndef _SERVO_CONTROLLER_H_
#define _SERVO_CONTROLLER_H_

#include <Arduino.h>
#include <driver/ledc.h>

class ServoController {
 public:
  static constexpr gpio_num_t kPinServo0 = GPIO_NUM_0;   // Servo 1: Pitch (俯仰: 抬头/低头)
  static constexpr gpio_num_t kPinServo1 = GPIO_NUM_25;  // Servo 2: Roll (偏侧: 左歪/右歪)
  static constexpr gpio_num_t kPinServo2 = GPIO_NUM_26;  // Servo 3: Yaw (整头水平旋转: 左转/右转)

  static constexpr ledc_channel_t kChannel0 = LEDC_CHANNEL_0;
  static constexpr ledc_channel_t kChannel1 = LEDC_CHANNEL_1;
  static constexpr ledc_channel_t kChannel2 = LEDC_CHANNEL_2;

  static constexpr uint32_t kPwmFreqHz = 50;              // 50Hz for standard RC servos
  static constexpr ledc_timer_bit_t kPwmResolution = LEDC_TIMER_14_BIT;
  static constexpr uint32_t kPwmPeriodTicks = 16384;      // 2^14
  static constexpr uint32_t kPwmPeriodUs = 20000;         // 20ms

  // MG92B pulse width range in microseconds
  static constexpr float kMinPulseUs = 500.0f;            // 0 degrees
  static constexpr float kMaxPulseUs = 2500.0f;           // 180 degrees
  static constexpr float kDefaultAngle = 90.0f;           // Neutral center for Pitch & Roll
  static constexpr float kServo2DefaultAngle = 70.0f;     // Neutral center for Yaw (looking straight front)

  // Safe operating angle ranges
  static constexpr float kServo0MinAngle = 40.0f;         // GPIO 0 Pitch Min (抬头极限)
  static constexpr float kServo0MaxAngle = 120.0f;        // GPIO 0 Pitch Max (低头极限)
  static constexpr float kServo1MinAngle = 50.0f;         // GPIO 25 Roll Min (左歪极限)
  static constexpr float kServo1MaxAngle = 110.0f;        // GPIO 25 Roll Max (右歪极限)
  static constexpr float kServo2MinAngle = 20.0f;         // GPIO 26 Yaw Min (左转极限)
  static constexpr float kServo2MaxAngle = 120.0f;        // GPIO 26 Yaw Max (右转极限)

  static constexpr float kDefaultStepDeg = 10.0f;         // Default single step (10 degrees)

  // Bytes, not words: the ESP-IDF Xtensa port defines StackType_t as uint8_t. Measured peak usage
  // on the device is 556 bytes (float math, LEDC writes, one ESP_LOGI), so 1.5KB is ample.
  static constexpr uint32_t kAnimationTaskStackSize = 1536;

  static ServoController& GetInstance();

  // Initialize LEDC timers, channels, and position all 3 servos to 90 degrees
  void Init();

  // Set angle for a specific pin (0, 25, or 26)
  void SetAngle(int pin, float angle);

  // Set angle by channel index (0, 1, or 2)
  void SetAngleByIndex(int index, float angle);

  // Set two or three angles at once
  void SetBothAngles(float angle0, float angle25);
  void SetAllAngles(float angle0, float angle25, float angle26);

  // Get current angle for a pin (0, 25, or 26)
  float GetAngle(int pin) const;

  // Center all 3 servos to 90 degrees
  void CenterAll();

  // Smooth interpolated movements to avoid mechanical inertia/shaking.
  // Motion follows a cosine ease-in/ease-out curve (soft start, soft stop). Duration is derived from
  // the distance at the axis' average speed (see SetAxisSpeed), never shorter than
  // kSmoothMinDurationMs. Pass speed_deg_s > 0 to override for one move.
  static constexpr float kSmoothSpeedDegPerSec = 40.0f;   // default per-axis speed until tuned
  static constexpr float kMinAxisSpeed = 10.0f;
  static constexpr float kMaxAxisSpeed = 120.0f;
  static constexpr uint32_t kSmoothMinDurationMs = 450;
  static constexpr uint32_t kSmoothFrameMs = 20;
  void MoveAngleSmooth(int pin, float target_angle, float speed_deg_s = 0.0f);
  void MoveAllSmooth(float target0, float target1, float target2, float speed_deg_s = 0.0f);

  // Per-axis average speed in deg/s (0 pitch 抬头, 1 roll 歪头, 2 yaw 转头). Tunable from the web
  // page and saved in NVS so the chosen value survives a reboot.
  void SetAxisSpeed(int axis, float deg_s);
  float AxisSpeed(int axis) const { return (axis >= 0 && axis < 3) ? axis_speed_[axis] : kSmoothSpeedDegPerSec; }
  void LoadAxisSpeeds();
  void SaveAxisSpeeds();

  // Non-blocking smooth move for callers that must not stall (web UI sliders, idle motion). GlideTo
  // only sets a target; Tick(), called from loop(), moves toward it with bounded acceleration. Cruise
  // speed is the axis speed x kGlideCruiseFactor x speed_scale. Any direct SetAngle /
  // MoveAngleSmooth on the same axis cancels the glide.
  static constexpr float kGlideCruiseFactor = 1.4f;   // ~ the peak speed of an eased move
  static constexpr float kGlideAccelFactor = 2.5f;    // reach cruise in ~0.4 s
  void GlideTo(int pin, float angle, float speed_scale = 1.0f);
  void Tick();

  // There-and-back move on one axis at its current speed, run on the gesture task. For tuning.
  void TriggerAxisTest(int axis);

  // Relative head motion methods (default 10 degrees, strictly bounded by safe limits)
  void LookUp(float delta_deg = kDefaultStepDeg);     // Pin 0 Pitch decreases (抬头, min 40)
  void LookDown(float delta_deg = kDefaultStepDeg);   // Pin 0 Pitch increases (低头, max 120)
  void TiltLeft(float delta_deg = kDefaultStepDeg);   // Pin 25 Roll decreases (向左歪头, min 50)
  void TiltRight(float delta_deg = kDefaultStepDeg);  // Pin 25 Roll increases (向右歪头, max 110)
  void TurnLeft(float delta_deg = kDefaultStepDeg);   // Pin 26 Yaw increases (向左转头, max 120)
  void TurnRight(float delta_deg = kDefaultStepDeg);  // Pin 26 Yaw decreases (向右转头, min 20)

  // Sweep test for calibration (eased). TriggerSweepTest runs it on the gesture task, non-blocking.
  static constexpr float kSweepSpeedDegPerSec = 45.0f;
  void RunSweepTest();
  void TriggerSweepTest();

  // Trigger cute "摇头晃脑" action (smooth 3-axis tilt + nod + rotate bobble)
  void TriggerHeadBobble();
  void RunHeadBobble();
  bool IsAnimating() const { return is_animating_; }

  // ---- Predefined keyframe animations (idle "alive" motions, also playable from the web page) ----
  // Each animation is a list of 3-axis poses relative to the neutral pose, each reached with an
  // easing curve and then held. Playback always starts from wherever the head currently is, so
  // switching animations never jumps. Requesting a new animation while one plays queues it for the
  // next pose boundary (where velocity is zero), so the hand-over is smooth too.
  static int AnimationCount();
  static const char* AnimationName(int index);   // ascii id, e.g. "look_around"
  static const char* AnimationLabel(int index);  // Chinese label for the web page
  static bool AnimationIsIdle(int index);        // part of the standby random pool
  static int FindAnimation(const char* name);    // -1 if unknown
  bool PlayAnimation(int index);
  // Picks a random idle animation, never the same one twice in a row.
  bool PlayRandomIdle();
  // Aborts any animation/gesture on the gesture task and waits briefly for it to let go.
  void StopAnimation();

 private:
  static constexpr uint32_t kGestureBobble = 1;
  static constexpr uint32_t kGestureSweep = 2;
  static constexpr uint32_t kGestureAxisTest = 4;   // axis index in bits 8..15
  static constexpr uint32_t kGestureAnim = 8;       // animation index in bits 8..15
  bool TriggerGesture(uint32_t gesture, const char* name);
  void RunAxisTest(int axis);
  void RunAnimation(int index);
  // Eased 3-axis move on the gesture task; returns false if aborted.
  bool AnimSegment(float p, float r, float y, uint32_t ms, uint8_t ease);
  bool OnAnimationTask() const;
  void StopGlides();
  volatile bool abort_ = false;
  volatile int pending_anim_ = -1;
  volatile int current_anim_ = -1;   // -1 while running a non-keyframe gesture
  int last_idle_anim_ = -1;

  ServoController() = default;
  ~ServoController() = default;
  ServoController(const ServoController&) = delete;
  ServoController& operator=(const ServoController&) = delete;

  uint32_t AngleToDuty(float angle) const;

  // Raw clamped PWM write; does not touch glide state.
  void ApplyAngle(int pin, float angle);
  static int PinToIndex(int pin);

  // Easing helpers for MoveAngleSmooth / MoveAllSmooth.
  static float EaseInOut(float t);
  static int EasedFrameCount(float distance_deg, float speed_deg_s);

  // Gestures run on their own task so the caller (the MCP tool handler) is not blocked for the ~2s
  // the animation takes. That task used to be created on demand, which meant asking a nearly
  // exhausted heap for a ~3.3KB contiguous stack at the exact moment TTS playback was running. It
  // is now created once at Init() on a statically allocated stack and parked on a notification.
  static void AnimationTaskEntry(void* arg);
  void AnimationTaskLoop();

  bool initialized_ = false;
  volatile bool is_animating_ = false;
  TaskHandle_t animation_task_ = nullptr;
  float angle_servo0_ = kDefaultAngle;
  float angle_servo1_ = kDefaultAngle;
  float angle_servo2_ = kServo2DefaultAngle;

  // Glide state per axis (0 pitch, 1 roll, 2 yaw). Written by GlideTo/SetAngle, read by Tick.
  volatile bool glide_active_[3] = {false, false, false};
  float glide_target_[3] = {kDefaultAngle, kDefaultAngle, kServo2DefaultAngle};
  float glide_velocity_[3] = {0.0f, 0.0f, 0.0f};
  float glide_cruise_[3] = {kSmoothSpeedDegPerSec, kSmoothSpeedDegPerSec, kSmoothSpeedDegPerSec};
  float axis_speed_[3] = {kSmoothSpeedDegPerSec, kSmoothSpeedDegPerSec, kSmoothSpeedDegPerSec};
  uint32_t last_tick_ms_ = 0;
};

#endif  // _SERVO_CONTROLLER_H_
