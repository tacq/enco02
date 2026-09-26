#include "servo_controller.h"

#include <Preferences.h>
#include <esp_log.h>
#include <esp_random.h>
#include <string.h>

[[maybe_unused]] static const char* TAG = "ServoController";

ServoController& ServoController::GetInstance() {
  static ServoController instance;
  return instance;
}

uint32_t ServoController::AngleToDuty(float angle) const {
  if (angle < 0.0f) angle = 0.0f;
  if (angle > 180.0f) angle = 180.0f;

  // Linear mapping: 0 deg -> kMinPulseUs, 180 deg -> kMaxPulseUs
  float pulse_us = kMinPulseUs + (angle / 180.0f) * (kMaxPulseUs - kMinPulseUs);
  // Convert pulse duration in microseconds to LEDC 14-bit duty count
  uint32_t duty = static_cast<uint32_t>((pulse_us / static_cast<float>(kPwmPeriodUs)) * kPwmPeriodTicks + 0.5f);
  return duty;
}

void ServoController::Init() {
  if (initialized_) {
    return;
  }

  ESP_LOGI(TAG, "Initializing 3x MG92B servos on Pin 0 (Pitch), Pin 25 (Roll), Pin 15 (Yaw)...");

  // 1. Configure LEDC Timer (50 Hz, 14-bit)
  ledc_timer_config_t timer_conf = {};
  timer_conf.speed_mode = LEDC_LOW_SPEED_MODE;
  timer_conf.duty_resolution = kPwmResolution;
  timer_conf.timer_num = LEDC_TIMER_0;
  timer_conf.freq_hz = kPwmFreqHz;
  timer_conf.clk_cfg = LEDC_AUTO_CLK;
  esp_err_t err = ledc_timer_config(&timer_conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ledc_timer_config failed: %d", err);
    return;
  }

  uint32_t default_duty = AngleToDuty(kDefaultAngle);
  uint32_t default_duty_yaw = AngleToDuty(kServo2DefaultAngle);

  // 2. Configure Channel 0 on GPIO 0 (Pitch)
  ledc_channel_config_t ch0_conf = {};
  ch0_conf.gpio_num = kPinServo0;
  ch0_conf.speed_mode = LEDC_LOW_SPEED_MODE;
  ch0_conf.channel = kChannel0;
  ch0_conf.intr_type = LEDC_INTR_DISABLE;
  ch0_conf.timer_sel = LEDC_TIMER_0;
  ch0_conf.duty = default_duty;
  ch0_conf.hpoint = 0;
  err = ledc_channel_config(&ch0_conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ledc_channel_config Ch0 failed: %d", err);
  }

  // 3. Configure Channel 1 on GPIO 25 (Roll)
  ledc_channel_config_t ch1_conf = {};
  ch1_conf.gpio_num = kPinServo1;
  ch1_conf.speed_mode = LEDC_LOW_SPEED_MODE;
  ch1_conf.channel = kChannel1;
  ch1_conf.intr_type = LEDC_INTR_DISABLE;
  ch1_conf.timer_sel = LEDC_TIMER_0;
  ch1_conf.duty = default_duty;
  ch1_conf.hpoint = 0;
  err = ledc_channel_config(&ch1_conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ledc_channel_config Ch1 failed: %d", err);
  }

  // 4. Configure Channel 2 on GPIO 26 (Yaw: Entire Head Rotation, 70 deg looking front)
  ledc_channel_config_t ch2_conf = {};
  ch2_conf.gpio_num = kPinServo2;
  ch2_conf.speed_mode = LEDC_LOW_SPEED_MODE;
  ch2_conf.channel = kChannel2;
  ch2_conf.intr_type = LEDC_INTR_DISABLE;
  ch2_conf.timer_sel = LEDC_TIMER_0;
  ch2_conf.duty = default_duty_yaw;
  ch2_conf.hpoint = 0;
  err = ledc_channel_config(&ch2_conf);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ledc_channel_config Ch2 failed: %d", err);
  }

  angle_servo0_ = kDefaultAngle;
  angle_servo1_ = kDefaultAngle;
  angle_servo2_ = kServo2DefaultAngle;
  initialized_ = true;
  LoadAxisSpeeds();

  // Create the gesture task up front, while the heap is still empty and a contiguous block is
  // guaranteed. The stack is a plain static buffer so this cannot fail later on.
  if (animation_task_ == nullptr) {
    static StaticTask_t animation_task_buffer;
    static StackType_t animation_task_stack[kAnimationTaskStackSize];
    animation_task_ = xTaskCreateStaticPinnedToCore(&ServoController::AnimationTaskEntry,
                                                    "servo_anim",
                                                    kAnimationTaskStackSize,
                                                    this,
                                                    3,
                                                    animation_task_stack,
                                                    &animation_task_buffer,
                                                    1);
  }

  ESP_LOGI(TAG, "All 3 servos initialized: Pitch 90°, Roll 90°, Yaw 70° (Front)");
}

int ServoController::PinToIndex(int pin) {
  if (pin == 0 || pin == kPinServo0) return 0;
  if (pin == 25 || pin == kPinServo1) return 1;
  if (pin == 26 || pin == 15 || pin == kPinServo2) return 2;
  return -1;
}

void ServoController::SetAngle(int pin, float angle) {
  // A direct command overrides any glide in progress on that axis (tracking, gestures, etc.).
  const int idx = PinToIndex(pin);
  if (idx >= 0) {
    glide_active_[idx] = false;
    glide_velocity_[idx] = 0.0f;
  }
  ApplyAngle(pin, angle);
}

void ServoController::GlideTo(int pin, float angle, float speed_scale) {
  if (!initialized_) {
    Init();
  }
  if (!OnAnimationTask()) StopAnimation();
  const int idx = PinToIndex(pin);
  if (idx < 0) {
    ESP_LOGW(TAG, "Unknown servo pin: %d", pin);
    return;
  }
  static const float kMin[3] = {kServo0MinAngle, kServo1MinAngle, kServo2MinAngle};
  static const float kMax[3] = {kServo0MaxAngle, kServo1MaxAngle, kServo2MaxAngle};
  if (angle < kMin[idx]) angle = kMin[idx];
  if (angle > kMax[idx]) angle = kMax[idx];
  if (speed_scale <= 0.0f) speed_scale = 1.0f;
  // Retargeting mid-glide keeps the current velocity, so dragging a slider stays fluid.
  glide_cruise_[idx] = axis_speed_[idx] * kGlideCruiseFactor * speed_scale;
  glide_target_[idx] = angle;
  glide_active_[idx] = true;
}

void ServoController::Tick() {
  const uint32_t now = millis();
  const uint32_t elapsed = now - last_tick_ms_;
  if (elapsed < kSmoothFrameMs) {
    return;
  }
  last_tick_ms_ = now;
  if (!glide_active_[0] && !glide_active_[1] && !glide_active_[2]) {
    return;
  }
  // Clamp dt so a stalled loop does not produce one big jump afterwards.
  const float dt = (elapsed > 100 ? 100 : elapsed) / 1000.0f;
  static const int kPins[3] = {kPinServo0, kPinServo1, kPinServo2};
  for (int i = 0; i < 3; ++i) {
    if (!glide_active_[i]) {
      continue;
    }
    const float current = GetAngle(kPins[i]);
    const float error = glide_target_[i] - current;
    if (fabsf(error) < 0.3f && fabsf(glide_velocity_[i]) < 5.0f) {
      ApplyAngle(kPins[i], glide_target_[i]);
      glide_active_[i] = false;
      glide_velocity_[i] = 0.0f;
      continue;
    }
    const float cruise = glide_cruise_[i];
    const float accel = cruise * kGlideAccelFactor;
    // Velocity that would still let us stop in time (v^2 = 2*a*d), capped at the cruise speed.
    float desired = sqrtf(2.0f * accel * fabsf(error));
    if (desired > cruise) desired = cruise;
    if (error < 0.0f) desired = -desired;
    // Accelerate / decelerate towards it at a bounded rate: soft start, soft stop.
    const float max_dv = accel * dt;
    float v = glide_velocity_[i];
    if (desired > v + max_dv) {
      v += max_dv;
    } else if (desired < v - max_dv) {
      v -= max_dv;
    } else {
      v = desired;
    }
    glide_velocity_[i] = v;
    float next = current + v * dt;
    // Never overshoot the target.
    if ((error > 0.0f && next > glide_target_[i]) || (error < 0.0f && next < glide_target_[i])) {
      next = glide_target_[i];
    }
    ApplyAngle(kPins[i], next);
  }
}

void ServoController::ApplyAngle(int pin, float angle) {
  if (!initialized_) {
    Init();
  }

  if (pin == 0 || pin == kPinServo0) {
    if (angle < kServo0MinAngle) angle = kServo0MinAngle;
    if (angle > kServo0MaxAngle) angle = kServo0MaxAngle;
    angle_servo0_ = angle;
    uint32_t duty = AngleToDuty(angle);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, kChannel0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, kChannel0);
    ESP_LOGD(TAG, "Servo 0 / Pitch (Pin 0) set to %.1f deg [safe: %.0f-%.0f] (duty: %u)",
             angle, kServo0MinAngle, kServo0MaxAngle, duty);
  } else if (pin == 25 || pin == kPinServo1) {
    if (angle < kServo1MinAngle) angle = kServo1MinAngle;
    if (angle > kServo1MaxAngle) angle = kServo1MaxAngle;
    angle_servo1_ = angle;
    uint32_t duty = AngleToDuty(angle);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, kChannel1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, kChannel1);
    ESP_LOGD(TAG, "Servo 1 / Roll (Pin 25) set to %.1f deg [safe: %.0f-%.0f] (duty: %u)",
             angle, kServo1MinAngle, kServo1MaxAngle, duty);
  } else if (pin == 26 || pin == 15 || pin == kPinServo2) {
    if (angle < kServo2MinAngle) angle = kServo2MinAngle;
    if (angle > kServo2MaxAngle) angle = kServo2MaxAngle;
    angle_servo2_ = angle;
    uint32_t duty = AngleToDuty(angle);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, kChannel2, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, kChannel2);
    ESP_LOGD(TAG, "Servo 2 / Yaw (Pin 26) set to %.1f deg [safe: %.0f-%.0f] (duty: %u)",
             angle, kServo2MinAngle, kServo2MaxAngle, duty);
  } else {
    ESP_LOGW(TAG, "Unknown servo pin: %d", pin);
  }
}

void ServoController::SetAngleByIndex(int index, float angle) {
  if (index == 0) {
    SetAngle(kPinServo0, angle);
  } else if (index == 1) {
    SetAngle(kPinServo1, angle);
  } else if (index == 2) {
    SetAngle(kPinServo2, angle);
  }
}

void ServoController::SetBothAngles(float angle0, float angle25) {
  SetAngle(kPinServo0, angle0);
  SetAngle(kPinServo1, angle25);
}

void ServoController::SetAllAngles(float angle0, float angle25, float angle26) {
  SetAngle(kPinServo0, angle0);
  SetAngle(kPinServo1, angle25);
  SetAngle(kPinServo2, angle26);
}

float ServoController::GetAngle(int pin) const {
  if (pin == 0 || pin == kPinServo0) {
    return angle_servo0_;
  } else if (pin == 25 || pin == kPinServo1) {
    return angle_servo1_;
  } else if (pin == 26 || pin == 15 || pin == kPinServo2) {
    return angle_servo2_;
  }
  return 90.0f;
}

void ServoController::MoveAngleSmooth(int pin, float target_angle, float speed_deg_s) {
  if (!OnAnimationTask()) StopAnimation();
  if (pin == 0 || pin == kPinServo0) {
    if (target_angle < kServo0MinAngle) target_angle = kServo0MinAngle;
    if (target_angle > kServo0MaxAngle) target_angle = kServo0MaxAngle;
  } else if (pin == 25 || pin == kPinServo1) {
    if (target_angle < kServo1MinAngle) target_angle = kServo1MinAngle;
    if (target_angle > kServo1MaxAngle) target_angle = kServo1MaxAngle;
  } else if (pin == 26 || pin == 15 || pin == kPinServo2) {
    if (target_angle < kServo2MinAngle) target_angle = kServo2MinAngle;
    if (target_angle > kServo2MaxAngle) target_angle = kServo2MaxAngle;
  }

  float current = GetAngle(pin);
  float diff = target_angle - current;
  float abs_diff = fabsf(diff);
  if (abs_diff < 0.2f) {
    SetAngle(pin, target_angle);
    return;
  }

  if (speed_deg_s <= 0.0f) {
    speed_deg_s = AxisSpeed(PinToIndex(pin));
  }
  int steps = EasedFrameCount(abs_diff, speed_deg_s);
  for (int i = 1; i <= steps; ++i) {
    if (abort_ && OnAnimationTask()) return;
    float angle = current + diff * EaseInOut(static_cast<float>(i) / static_cast<float>(steps));
    SetAngle(pin, angle);
    delay(kSmoothFrameMs);
  }
  SetAngle(pin, target_angle);
}

float ServoController::EaseInOut(float t) {
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  // Cosine ease: zero velocity at both ends, peak speed = pi/2 x average speed in the middle.
  return 0.5f - 0.5f * cosf(static_cast<float>(M_PI) * t);
}

int ServoController::EasedFrameCount(float distance_deg, float speed_deg_s) {
  if (speed_deg_s < 1.0f) speed_deg_s = kSmoothSpeedDegPerSec;
  float duration_ms = distance_deg / speed_deg_s * 1000.0f;
  if (duration_ms < static_cast<float>(kSmoothMinDurationMs)) {
    duration_ms = static_cast<float>(kSmoothMinDurationMs);
  }
  int steps = static_cast<int>(ceilf(duration_ms / static_cast<float>(kSmoothFrameMs)));
  return steps < 1 ? 1 : steps;
}

void ServoController::MoveAllSmooth(float target0, float target1, float target2, float speed_deg_s) {
  if (!OnAnimationTask()) StopAnimation();  // voice / web / wake-up moves take over cleanly
  if (target0 < kServo0MinAngle) target0 = kServo0MinAngle;
  if (target0 > kServo0MaxAngle) target0 = kServo0MaxAngle;
  if (target1 < kServo1MinAngle) target1 = kServo1MinAngle;
  if (target1 > kServo1MaxAngle) target1 = kServo1MaxAngle;
  if (target2 < kServo2MinAngle) target2 = kServo2MinAngle;
  if (target2 > kServo2MaxAngle) target2 = kServo2MaxAngle;

  float cur0 = GetAngle(kPinServo0);
  float cur1 = GetAngle(kPinServo1);
  float cur2 = GetAngle(kPinServo2);

  float diff0 = target0 - cur0;
  float diff1 = target1 - cur1;
  float diff2 = target2 - cur2;

  float max_diff = fmaxf(fabsf(diff0), fmaxf(fabsf(diff1), fabsf(diff2)));
  if (max_diff < 0.2f) {
    SetAllAngles(target0, target1, target2);
    return;
  }

  int steps;
  if (speed_deg_s > 0.0f) {
    steps = EasedFrameCount(max_diff, speed_deg_s);
  } else {
    // Each axis at its own tuned speed; the whole move lasts as long as the slowest one needs.
    const float diffs[3] = {fabsf(diff0), fabsf(diff1), fabsf(diff2)};
    steps = 1;
    for (int a = 0; a < 3; ++a) {
      if (diffs[a] >= 0.2f) {
        const int s = EasedFrameCount(diffs[a], axis_speed_[a]);
        if (s > steps) steps = s;
      }
    }
  }
  for (int i = 1; i <= steps; ++i) {
    if (abort_ && OnAnimationTask()) return;
    float fraction = EaseInOut(static_cast<float>(i) / static_cast<float>(steps));
    float a0 = cur0 + diff0 * fraction;
    float a1 = cur1 + diff1 * fraction;
    float a2 = cur2 + diff2 * fraction;
    SetAllAngles(a0, a1, a2);
    delay(kSmoothFrameMs);
  }
  SetAllAngles(target0, target1, target2);
}

void ServoController::CenterAll() {
  MoveAllSmooth(kDefaultAngle, kDefaultAngle, kServo2DefaultAngle);
  ESP_LOGI(TAG, "All 3 servos centered smoothly: Pitch 90°, Roll 90°, Yaw 70° (Front)");
}

namespace {
constexpr const char* kServoPrefsNamespace = "servo";
constexpr const char* kSpeedKeys[3] = {"sp_pitch", "sp_roll", "sp_yaw"};
}  // namespace

void ServoController::SetAxisSpeed(int axis, float deg_s) {
  if (axis < 0 || axis > 2) {
    return;
  }
  if (deg_s < kMinAxisSpeed) deg_s = kMinAxisSpeed;
  if (deg_s > kMaxAxisSpeed) deg_s = kMaxAxisSpeed;
  axis_speed_[axis] = deg_s;
  ESP_LOGI(TAG, "Axis %d speed -> %.0f deg/s", axis, deg_s);
}

void ServoController::LoadAxisSpeeds() {
  Preferences prefs;
  if (!prefs.begin(kServoPrefsNamespace, /*readOnly=*/true)) {
    return;  // Nothing saved yet: keep the defaults.
  }
  for (int a = 0; a < 3; ++a) {
    SetAxisSpeed(a, prefs.getFloat(kSpeedKeys[a], kSmoothSpeedDegPerSec));
  }
  prefs.end();
}

void ServoController::SaveAxisSpeeds() {
  Preferences prefs;
  if (!prefs.begin(kServoPrefsNamespace, /*readOnly=*/false)) {
    return;
  }
  for (int a = 0; a < 3; ++a) {
    prefs.putFloat(kSpeedKeys[a], axis_speed_[a]);
  }
  prefs.end();
  ESP_LOGI(TAG, "Axis speeds saved: pitch %.0f, roll %.0f, yaw %.0f", axis_speed_[0], axis_speed_[1],
           axis_speed_[2]);
}

void ServoController::TriggerAxisTest(int axis) {
  if (axis < 0 || axis > 2) {
    return;
  }
  TriggerGesture(kGestureAxisTest | (static_cast<uint32_t>(axis) << 8), "axis test");
}

void ServoController::RunAxisTest(int axis) {
  // A typical voice-command sized move and back: 抬头 20°, 歪头 20°, 转头 30°.
  static const int kPins[3] = {kPinServo0, kPinServo1, kPinServo2};
  static const float kDelta[3] = {-20.0f, 20.0f, 30.0f};
  const float start = GetAngle(kPins[axis]);
  MoveAngleSmooth(kPins[axis], start + kDelta[axis]);
  delay(400);
  MoveAngleSmooth(kPins[axis], start);
}

void ServoController::LookUp(float delta_deg) {
  float current = GetAngle(kPinServo0);
  float target = current - delta_deg;
  ESP_LOGI(TAG, "LookUp: %.1f -> %.1f deg (smooth)", current, target);
  MoveAngleSmooth(kPinServo0, target);
}

void ServoController::LookDown(float delta_deg) {
  float current = GetAngle(kPinServo0);
  float target = current + delta_deg;
  ESP_LOGI(TAG, "LookDown: %.1f -> %.1f deg (smooth)", current, target);
  MoveAngleSmooth(kPinServo0, target);
}

void ServoController::TiltLeft(float delta_deg) {
  float current = GetAngle(kPinServo1);
  float target = current - delta_deg;
  ESP_LOGI(TAG, "TiltLeft: %.1f -> %.1f deg (smooth)", current, target);
  MoveAngleSmooth(kPinServo1, target);
}

void ServoController::TiltRight(float delta_deg) {
  float current = GetAngle(kPinServo1);
  float target = current + delta_deg;
  ESP_LOGI(TAG, "TiltRight: %.1f -> %.1f deg (smooth)", current, target);
  MoveAngleSmooth(kPinServo1, target);
}

// Yaw is mounted so that a larger angle turns the head to the robot's left (verified on hardware).
void ServoController::TurnLeft(float delta_deg) {
  float current = GetAngle(kPinServo2);
  float target = current + delta_deg;
  ESP_LOGI(TAG, "TurnLeft: %.1f -> %.1f deg (smooth)", current, target);
  MoveAngleSmooth(kPinServo2, target);
}

void ServoController::TurnRight(float delta_deg) {
  float current = GetAngle(kPinServo2);
  float target = current - delta_deg;
  ESP_LOGI(TAG, "TurnRight: %.1f -> %.1f deg (smooth)", current, target);
  MoveAngleSmooth(kPinServo2, target);
}

void ServoController::RunSweepTest() {
  ESP_LOGI(TAG, "Starting 3-axis servo sweep test within safe ranges...");
  // Eased moves to the lower bounds, the upper bounds, then back to centre.
  MoveAllSmooth(50.0f, 60.0f, 35.0f, kSweepSpeedDegPerSec);
  delay(200);
  MoveAllSmooth(115.0f, 105.0f, 105.0f, kSweepSpeedDegPerSec);
  delay(200);
  MoveAllSmooth(kDefaultAngle, kDefaultAngle, kServo2DefaultAngle, kSweepSpeedDegPerSec);
  ESP_LOGI(TAG, "Servo 3-axis sweep test completed at front center (90, 90, 70)");
}

void ServoController::AnimationTaskEntry(void* arg) {
  static_cast<ServoController*>(arg)->AnimationTaskLoop();
}

void ServoController::AnimationTaskLoop() {
  while (true) {
    // Park here until a gesture is requested. The notification value carries which gesture;
    // triggers arriving during one animation are rejected by is_animating_ (keyframe animations
    // are the exception: they queue in pending_anim_ and run back to back below).
    uint32_t gesture = 0;
    xTaskNotifyWait(0, UINT32_MAX, &gesture, portMAX_DELAY);
    abort_ = false;
    if (gesture & kGestureAnim) {
      int next = static_cast<int>((gesture >> 8) & 0xFF);
      while (next >= 0 && !abort_) {
        pending_anim_ = -1;
        current_anim_ = next;
        RunAnimation(next);
        next = pending_anim_;
      }
      current_anim_ = -1;
      pending_anim_ = -1;
    } else if (gesture & kGestureAxisTest) {
      RunAxisTest(static_cast<int>((gesture >> 8) & 0xFF));
    } else if (gesture & kGestureSweep) {
      RunSweepTest();
    } else {
      RunHeadBobble();
    }
    is_animating_ = false;
  }
}

bool ServoController::TriggerGesture(uint32_t gesture, const char* name) {
  if (animation_task_ == nullptr) {
    ESP_LOGW(TAG, "Gesture task not running, ignoring %s trigger", name);
    return false;
  }
  if (is_animating_) {
    ESP_LOGW(TAG, "Already animating, ignoring %s trigger", name);
    return false;
  }
  is_animating_ = true;
  xTaskNotify(animation_task_, gesture, eSetValueWithOverwrite);
  return true;
}

void ServoController::TriggerHeadBobble() {
  TriggerGesture(kGestureBobble, "head bobble");
}

void ServoController::TriggerSweepTest() {
  TriggerGesture(kGestureSweep, "sweep");
}

bool ServoController::OnAnimationTask() const {
  return animation_task_ != nullptr && xTaskGetCurrentTaskHandle() == animation_task_;
}

void ServoController::StopAnimation() {
  if (!is_animating_ || OnAnimationTask()) {
    return;
  }
  pending_anim_ = -1;
  abort_ = true;
  // Every animation loop checks abort_ once per 20-25 ms frame.
  for (int i = 0; i < 10 && is_animating_; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void ServoController::RunHeadBobble() {
  ESP_LOGI(TAG, "Executing cute 3-axis '摇头晃脑' (head bobble) gesture...");
  // The shake is built around the neutral pose, so ease there first instead of jumping to it.
  MoveAllSmooth(kDefaultAngle, kDefaultAngle, kServo2DefaultAngle);
  if (abort_) return;
  // 2 complete cycles of combined shake, tilt & nod over ~3 s. Yaw (the actual "shake") dominates
  // and uses most of its range; every axis stays a few degrees inside its safe limit.
  // ~3 s at the default 40 deg/s; follows the tuned speeds like the keyframe animations do.
  float time_scale = kSmoothSpeedDegPerSec / ((axis_speed_[0] + axis_speed_[1] + axis_speed_[2]) / 3.0f);
  if (time_scale < 0.5f) time_scale = 0.5f;
  if (time_scale > 2.0f) time_scale = 2.0f;
  const int total_steps = static_cast<int>(120 * time_scale);
  const int delay_ms = 25;
  const float yaw_amp = 28.0f;    // 70 +- 28 -> 42..98   (limit 20..120)
  const float roll_amp = 17.0f;   // 90 +- 17 -> 73..107  (limit 50..110)
  const float pitch_amp = 8.0f;   // 90 +- 8  -> 82..98   (limit 40..120), 2x frequency bounce

  for (int step = 0; step <= total_steps; ++step) {
    if (abort_) return;
    float p = static_cast<float>(step) / static_cast<float>(total_steps); // 0.0 to 1.0
    // Hanning-style smooth envelope (starts 0, peaks at middle, ends at 0)
    float envelope = sinf(M_PI * p);

    // Servo 2 (Pin 26, Yaw / Rotate): the main left-right shake
    float yaw_angle = kServo2DefaultAngle + yaw_amp * sinf(4.0f * M_PI * p) * envelope;

    // Servo 1 (Pin 25, Roll / Tilt): tilts in step with the shake for a playful sway
    float tilt_angle = kDefaultAngle + roll_amp * sinf(4.0f * M_PI * p + (M_PI / 4.0f)) * envelope;

    // Servo 0 (Pin 0, Pitch / Nod): light bounce at twice the shake frequency
    float pitch_angle = kDefaultAngle + pitch_amp * sinf(8.0f * M_PI * p) * envelope;

    SetAllAngles(pitch_angle, tilt_angle, yaw_angle);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
  }

  ESP_LOGI(TAG, "'摇头晃脑' 3-axis gesture complete at front center (90, 90, 70)");
}

// =================================================================================================
// Keyframe animations
//
// Design notes (the usual rules for making a small pan/tilt/roll head read as alive):
//  - Motion is always eased; nothing starts or stops at full speed.
//  - Glances are quick out (ease-out: fast start, soft landing) and slow back (ease-in-out).
//  - Every look is paired with a little tilt/pitch; pure single-axis moves read as mechanical.
//  - Holds matter as much as moves: a pose held for a beat reads as attention, not jitter.
//  - Asymmetry and variety: left/right versions differ slightly, and the idle picker never plays
//    the same animation twice in a row.
// Pose deltas are relative to neutral (pitch 90, roll 90, yaw 70):
//   dp < 0 looks up, dp > 0 looks down; dr < 0 tilts left, dr > 0 tilts right;
//   dy > 0 turns left, dy < 0 turns right.
// =================================================================================================
namespace {

enum Ease : uint8_t { kInOut = 0, kOut = 1, kIn = 2 };

struct Keyframe {
  int8_t dp, dr, dy;
  uint8_t ease;
  uint16_t move_ms;  // time to reach this pose (at the default 40 deg/s speed setting)
  uint16_t hold_ms;  // then hold it this long
};

struct Animation {
  const char* name;
  const char* label;
  bool idle;  // part of the standby random pool
  const Keyframe* frames;
  uint8_t count;
};

// 环顾四周: slow scan left, pause, sweep right, pause, home.
constexpr Keyframe kLookAround[] = {
    {-3, -4, 28, kInOut, 1100, 700},
    {-2, 5, -26, kInOut, 1700, 700},
    {0, 0, 0, kInOut, 1000, 200},
};
// 好奇歪头: lift a little and tilt, hold like "hm?", tilt the other way a bit less, home.
constexpr Keyframe kCuriousTilt[] = {
    {-5, 14, 4, kInOut, 700, 1100},
    {-3, -8, -3, kInOut, 800, 700},
    {0, 0, 0, kInOut, 700, 200},
};
// 瞥左边 / 瞥右边: unhurried glance with a small tilt, hold, slower return. (Was a 380 ms ease-out
// "quick glance", ~190 deg/s peak: too snappy on this head.)
constexpr Keyframe kGlanceLeft[] = {
    {-3, -4, 24, kInOut, 900, 1000},
    {0, 0, 0, kInOut, 1200, 200},
};
constexpr Keyframe kGlanceRight[] = {
    {-2, 5, -22, kInOut, 900, 950},
    {0, 0, 0, kInOut, 1200, 200},
};
// 点头: two soft nods, second one smaller.
constexpr Keyframe kNod[] = {
    {10, 0, 0, kInOut, 320, 60},
    {-2, 0, 0, kInOut, 320, 40},
    {7, 0, 0, kInOut, 300, 60},
    {0, 0, 0, kInOut, 400, 200},
};
// 仰望: look up and a little aside, as if something caught her eye above.
constexpr Keyframe kLookUp[] = {
    {-18, -5, 10, kInOut, 1000, 1400},
    {-12, 3, -6, kInOut, 900, 600},
    {0, 0, 0, kInOut, 1000, 200},
};
// 呼吸: very small slow rise and fall, the quietest "still alive" motion.
constexpr Keyframe kBreathe[] = {
    {-4, 1, 0, kInOut, 1500, 150},
    {2, -1, 0, kInOut, 1600, 150},
    {-3, 0, 0, kInOut, 1500, 150},
    {0, 0, 0, kInOut, 1400, 100},
};
// 犯困: head slowly droops with a tilt, then a little startled jolt back.
constexpr Keyframe kSleepy[] = {
    {14, -9, 3, kInOut, 2200, 900},
    {18, -11, 3, kInOut, 900, 500},
    {-4, 2, 0, kOut, 260, 300},
    {0, 0, 0, kInOut, 700, 200},
};
// 害羞: look down and away with a tilt, peek back, home.
constexpr Keyframe kShy[] = {
    {12, 9, -16, kInOut, 700, 1100},
    {6, 5, -6, kInOut, 600, 500},
    {0, 0, 0, kInOut, 800, 200},
};
// 思考: look up-left with a tilt, drift a bit further, home.
constexpr Keyframe kThink[] = {
    {-10, -8, 16, kInOut, 900, 1000},
    {-13, -10, 22, kInOut, 700, 800},
    {0, 0, 0, kInOut, 1000, 200},
};
// 惊讶: quick lift back, small tilt, settle.
constexpr Keyframe kSurprised[] = {
    {-11, 0, 0, kOut, 220, 450},
    {-8, 7, 3, kInOut, 350, 500},
    {0, 0, 0, kInOut, 800, 200},
};
// 小摇摆: rhythmic side-to-side sway (fun, not in the idle pool).
constexpr Keyframe kSway[] = {
    {0, 12, 10, kInOut, 380, 40},
    {0, -12, -10, kInOut, 520, 40},
    {0, 12, 10, kInOut, 520, 40},
    {0, -12, -10, kInOut, 520, 40},
    {0, 0, 0, kInOut, 450, 200},
};

#define ANIM(name, label, idle, frames) {name, label, idle, frames, sizeof(frames) / sizeof(frames[0])}
constexpr Animation kAnimations[] = {
    ANIM("look_around", "环顾四周", true, kLookAround),
    ANIM("curious", "好奇歪头", true, kCuriousTilt),
    ANIM("glance_left", "瞥左边", true, kGlanceLeft),
    ANIM("glance_right", "瞥右边", true, kGlanceRight),
    ANIM("nod", "点头", true, kNod),
    ANIM("look_up", "仰望", true, kLookUp),
    ANIM("breathe", "呼吸", true, kBreathe),
    ANIM("sleepy", "犯困", true, kSleepy),
    ANIM("shy", "害羞", true, kShy),
    ANIM("think", "思考", true, kThink),
    ANIM("surprised", "惊讶", false, kSurprised),
    ANIM("sway", "小摇摆", false, kSway),
};
#undef ANIM
constexpr int kAnimationCount = sizeof(kAnimations) / sizeof(kAnimations[0]);

float ApplyEase(uint8_t ease, float t) {
  if (t <= 0.0f) return 0.0f;
  if (t >= 1.0f) return 1.0f;
  switch (ease) {
    case kOut: {  // cubic ease-out: quick start, soft landing
      const float u = 1.0f - t;
      return 1.0f - u * u * u;
    }
    case kIn:  // cubic ease-in
      return t * t * t;
    default:  // cosine ease-in-out
      return 0.5f - 0.5f * cosf(static_cast<float>(M_PI) * t);
  }
}

}  // namespace

int ServoController::AnimationCount() { return kAnimationCount; }

const char* ServoController::AnimationName(int index) {
  return (index >= 0 && index < kAnimationCount) ? kAnimations[index].name : "";
}

const char* ServoController::AnimationLabel(int index) {
  return (index >= 0 && index < kAnimationCount) ? kAnimations[index].label : "";
}

bool ServoController::AnimationIsIdle(int index) {
  return index >= 0 && index < kAnimationCount && kAnimations[index].idle;
}

int ServoController::FindAnimation(const char* name) {
  for (int i = 0; i < kAnimationCount; ++i) {
    if (strcmp(kAnimations[i].name, name) == 0) return i;
  }
  return -1;
}

bool ServoController::PlayAnimation(int index) {
  if (index < 0 || index >= kAnimationCount) {
    return false;
  }
  if (is_animating_) {
    if (current_anim_ >= 0) {
      // Hand over at the next pose boundary, where the head is momentarily still.
      pending_anim_ = index;
      return true;
    }
    return false;  // bobble / sweep / axis test running
  }
  StopGlides();
  return TriggerGesture(kGestureAnim | (static_cast<uint32_t>(index) << 8), kAnimations[index].name);
}

bool ServoController::PlayRandomIdle() {
  // Index kAnimationCount stands for 摇头晃脑 (the procedural head bobble), which joins the pool too.
  const int kBobbleSlot = kAnimationCount;
  int pool[kAnimationCount + 1];
  int n = 0;
  for (int i = 0; i < kAnimationCount; ++i) {
    if (kAnimations[i].idle && i != last_idle_anim_) pool[n++] = i;
  }
  if (last_idle_anim_ != kBobbleSlot) pool[n++] = kBobbleSlot;
  if (n == 0) return false;
  const int pick = pool[esp_random() % n];
  if (pick == kBobbleSlot) {
    if (is_animating_) return false;
    StopGlides();
    if (!TriggerGesture(kGestureBobble, "head bobble")) return false;
    ESP_LOGI(TAG, "Animation: head bobble (idle)");
  } else if (!PlayAnimation(pick)) {
    return false;
  }
  last_idle_anim_ = pick;
  return true;
}

bool ServoController::AnimSegment(float p, float r, float y, uint32_t ms, uint8_t ease) {
  const float p0 = angle_servo0_, r0 = angle_servo1_, y0 = angle_servo2_;
  int steps = static_cast<int>(ms / kSmoothFrameMs);
  if (steps < 1) steps = 1;
  for (int i = 1; i <= steps; ++i) {
    if (abort_) return false;
    const float k = ApplyEase(ease, static_cast<float>(i) / static_cast<float>(steps));
    SetAllAngles(p0 + (p - p0) * k, r0 + (r - r0) * k, y0 + (y - y0) * k);
    vTaskDelay(pdMS_TO_TICKS(kSmoothFrameMs));
  }
  return true;
}

void ServoController::RunAnimation(int index) {
  const Animation& anim = kAnimations[index];
  ESP_LOGI(TAG, "Animation: %s", anim.name);
  // Authored at the default 40 deg/s; follow the tuned speeds so the sliders affect these too.
  const float mean_speed = (axis_speed_[0] + axis_speed_[1] + axis_speed_[2]) / 3.0f;
  float time_scale = kSmoothSpeedDegPerSec / mean_speed;
  if (time_scale < 0.5f) time_scale = 0.5f;
  if (time_scale > 2.0f) time_scale = 2.0f;

  for (uint8_t f = 0; f < anim.count; ++f) {
    const Keyframe& k = anim.frames[f];
    const uint32_t move_ms = static_cast<uint32_t>(k.move_ms * time_scale);
    if (!AnimSegment(kDefaultAngle + k.dp, kDefaultAngle + k.dr, kServo2DefaultAngle + k.dy, move_ms, k.ease)) {
      return;
    }
    // Hold, but hand over early if another animation is waiting: we are at rest right now.
    for (uint32_t held = 0; held < k.hold_ms; held += kSmoothFrameMs) {
      if (abort_ || pending_anim_ >= 0) return;
      vTaskDelay(pdMS_TO_TICKS(kSmoothFrameMs));
    }
    if (pending_anim_ >= 0) return;
  }
}

void ServoController::StopGlides() {
  for (int i = 0; i < 3; ++i) {
    glide_active_[i] = false;
    glide_velocity_[i] = 0.0f;
  }
}
