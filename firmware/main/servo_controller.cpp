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

  ESP_LOGI(TAG, "Initializing MG92B servos on Pin 0 (LEDC Ch0) and Pin 25 (LEDC Ch1)...");

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

  // 2. Configure Channel 0 on GPIO 0
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

  // 3. Configure Channel 1 on GPIO 25
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

  angle_servo0_ = kDefaultAngle;
  angle_servo1_ = kDefaultAngle;
  initialized_ = true;

  ESP_LOGI(TAG, "Servos initialized at 90 degrees (Duty: %u)", default_duty);
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
    ESP_LOGD(TAG, "Servo 0 (Pin 0) set to %.1f deg [safe: %.0f-%.0f] (duty: %u)",
             angle, kServo0MinAngle, kServo0MaxAngle, duty);
  } else if (pin == 25 || pin == kPinServo1) {
    if (angle < kServo1MinAngle) angle = kServo1MinAngle;
    if (angle > kServo1MaxAngle) angle = kServo1MaxAngle;
    angle_servo1_ = angle;
    uint32_t duty = AngleToDuty(angle);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, kChannel1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, kChannel1);
    ESP_LOGD(TAG, "Servo 1 (Pin 25) set to %.1f deg [safe: %.0f-%.0f] (duty: %u)",
             angle, kServo1MinAngle, kServo1MaxAngle, duty);
  } else {
    ESP_LOGW(TAG, "Unknown servo pin: %d", pin);
  }
}

void ServoController::SetAngleByIndex(int index, float angle) {
  if (index == 0) {
    SetAngle(kPinServo0, angle);
  } else if (index == 1) {
    SetAngle(kPinServo1, angle);
  }
}

void ServoController::SetBothAngles(float angle0, float angle25) {
  SetAngle(kPinServo0, angle0);
  SetAngle(kPinServo1, angle25);
}

float ServoController::GetAngle(int pin) const {
  if (pin == 0 || pin == kPinServo0) {
    return angle_servo0_;
  } else if (pin == 25 || pin == kPinServo1) {
    return angle_servo1_;
  }
  return 90.0f;
}

void ServoController::CenterAll() {
  SetBothAngles(90.0f, 90.0f);
  ESP_LOGI(TAG, "Both servos centered to 90 degrees (safe center)");
}

void ServoController::LookUp(float delta_deg) {
  float current = GetAngle(kPinServo0);
  float target = current - delta_deg;
  ESP_LOGI(TAG, "LookUp: %.1f -> %.1f deg", current, target);
  SetAngle(kPinServo0, target);
}

void ServoController::LookDown(float delta_deg) {
  float current = GetAngle(kPinServo0);
  float target = current + delta_deg;
  ESP_LOGI(TAG, "LookDown: %.1f -> %.1f deg", current, target);
  SetAngle(kPinServo0, target);
}

void ServoController::TiltLeft(float delta_deg) {
  float current = GetAngle(kPinServo1);
  float target = current - delta_deg;
  ESP_LOGI(TAG, "TiltLeft: %.1f -> %.1f deg", current, target);
  SetAngle(kPinServo1, target);
}

void ServoController::TiltRight(float delta_deg) {
  float current = GetAngle(kPinServo1);
  float target = current + delta_deg;
  ESP_LOGI(TAG, "TiltRight: %.1f -> %.1f deg", current, target);
  SetAngle(kPinServo1, target);
}

void ServoController::RunSweepTest() {
  ESP_LOGI(TAG, "Starting servo sweep test within safe ranges...");
  // 1. Move to lower safe bounds (Pin 0: 50 deg, Pin 25: 60 deg)
  for (int step = 0; step <= 30; step += 2) {
    float a0 = 90.0f - (40.0f * step / 30.0f); // 90 -> 50
    float a1 = 90.0f - (30.0f * step / 30.0f); // 90 -> 60
    SetBothAngles(a0, a1);
    delay(20);
  }
  delay(150);

  // 2. Move to upper safe bounds (Pin 0: 115 deg, Pin 25: 105 deg)
  for (int step = 0; step <= 30; step += 2) {
    float a0 = 50.0f + (65.0f * step / 30.0f); // 50 -> 115
    float a1 = 60.0f + (45.0f * step / 30.0f); // 60 -> 105
    SetBothAngles(a0, a1);
    delay(20);
  }
  delay(150);

  // 3. Smoothly return to 90 deg center
  for (int step = 0; step <= 30; step += 2) {
    float a0 = 115.0f - (25.0f * step / 30.0f); // 115 -> 90
    float a1 = 105.0f - (15.0f * step / 30.0f); // 105 -> 90
    SetBothAngles(a0, a1);
    delay(20);
  }
  CenterAll();
  ESP_LOGI(TAG, "Servo sweep test completed at 90 degrees");
}

void ServoController::TriggerHeadBobble() {
  if (is_animating_) {
    ESP_LOGW(TAG, "Already animating, ignoring head bobble trigger");
    return;
  }
  is_animating_ = true;
  xTaskCreate(
      [](void* arg) {
        auto* controller = static_cast<ServoController*>(arg);
        controller->RunHeadBobble();
        controller->is_animating_ = false;
        vTaskDelete(nullptr);
      },
      "head_bobble",
      3072,
      this,
      1,
      nullptr);
}

void ServoController::RunHeadBobble() {
  ESP_LOGI(TAG, "Executing cute '摇头晃脑' (head bobble) gesture...");
  // 2 complete gentle cycles of combined tilt & nod
  const int total_steps = 72;
  const int delay_ms = 25;

  for (int step = 0; step <= total_steps; ++step) {
    float p = static_cast<float>(step) / static_cast<float>(total_steps); // 0.0 to 1.0
    // Hanning-style smooth envelope (starts 0, peaks at middle, ends at 0)
    float envelope = sinf(M_PI * p);

    // Servo 1 (Pin 25, Tilt): swings left and right (safe: 74 to 106 deg)
    float tilt_delta = 16.0f * sinf(4.0f * M_PI * p) * envelope;
    float tilt_angle = 90.0f + tilt_delta;

    // Servo 0 (Pin 0, Pitch): playful nod / lift (safe: 78 to 102 deg)
    // Larger angle is nod down, smaller angle is head up
    float pitch_delta = 12.0f * cosf(4.0f * M_PI * p) * envelope;
    float pitch_angle = 90.0f + pitch_delta;

    SetBothAngles(pitch_angle, tilt_angle);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
  }

  CenterAll();
  ESP_LOGI(TAG, "'摇头晃脑' gesture complete at center (90, 90)");
}
