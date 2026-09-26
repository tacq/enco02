#include "servo_controller.h"

#include <esp_log.h>

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

void ServoController::SetAngle(int pin, float angle) {
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

  int steps = EasedFrameCount(abs_diff, speed_deg_s);
  for (int i = 1; i <= steps; ++i) {
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

  int steps = EasedFrameCount(max_diff, speed_deg_s);
  for (int i = 1; i <= steps; ++i) {
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
  // 1. Move to lower safe bounds (Pin 0: 50, Pin 25: 60, Pin 26: 35)
  for (int step = 0; step <= 30; step += 2) {
    float a0 = 90.0f - (40.0f * step / 30.0f); // 90 -> 50
    float a1 = 90.0f - (30.0f * step / 30.0f); // 90 -> 60
    float a2 = 70.0f - (35.0f * step / 30.0f); // 70 -> 35
    SetAllAngles(a0, a1, a2);
    delay(20);
  }
  delay(150);

  // 2. Move to upper safe bounds (Pin 0: 115, Pin 25: 105, Pin 26: 105)
  for (int step = 0; step <= 30; step += 2) {
    float a0 = 50.0f + (65.0f * step / 30.0f); // 50 -> 115
    float a1 = 60.0f + (45.0f * step / 30.0f); // 60 -> 105
    float a2 = 35.0f + (70.0f * step / 30.0f); // 35 -> 105
    SetAllAngles(a0, a1, a2);
    delay(20);
  }
  delay(150);

  // 3. Smoothly return to center (90, 90, 70)
  for (int step = 0; step <= 30; step += 2) {
    float a0 = 115.0f - (25.0f * step / 30.0f); // 115 -> 90
    float a1 = 105.0f - (15.0f * step / 30.0f); // 105 -> 90
    float a2 = 105.0f - (35.0f * step / 30.0f); // 105 -> 70
    SetAllAngles(a0, a1, a2);
    delay(20);
  }
  CenterAll();
  ESP_LOGI(TAG, "Servo 3-axis sweep test completed at front center (90, 90, 70)");
}

void ServoController::AnimationTaskEntry(void* arg) {
  static_cast<ServoController*>(arg)->AnimationTaskLoop();
}

void ServoController::AnimationTaskLoop() {
  while (true) {
    // Park here until a gesture is requested. Notifications coalesce, which is exactly the desired
    // behaviour: several triggers arriving during one animation replay it at most once.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    RunHeadBobble();
    is_animating_ = false;
  }
}

void ServoController::TriggerHeadBobble() {
  if (animation_task_ == nullptr) {
    ESP_LOGW(TAG, "Gesture task not running, ignoring head bobble trigger");
    return;
  }
  if (is_animating_) {
    ESP_LOGW(TAG, "Already animating, ignoring head bobble trigger");
    return;
  }
  is_animating_ = true;
  xTaskNotifyGive(animation_task_);
}

void ServoController::RunHeadBobble() {
  ESP_LOGI(TAG, "Executing cute 3-axis '摇头晃脑' (head bobble) gesture...");
  // 2 complete cycles of combined shake, tilt & nod over ~3 s. Yaw (the actual "shake") dominates
  // and uses most of its range; every axis stays a few degrees inside its safe limit.
  const int total_steps = 120;
  const int delay_ms = 25;
  const float yaw_amp = 28.0f;    // 70 +- 28 -> 42..98   (limit 20..120)
  const float roll_amp = 17.0f;   // 90 +- 17 -> 73..107  (limit 50..110)
  const float pitch_amp = 8.0f;   // 90 +- 8  -> 82..98   (limit 40..120), 2x frequency bounce

  for (int step = 0; step <= total_steps; ++step) {
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

  CenterAll();
  ESP_LOGI(TAG, "'摇头晃脑' 3-axis gesture complete at front center (90, 90, 70)");
}
