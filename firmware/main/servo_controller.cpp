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

  if (angle < 0.0f) angle = 0.0f;
  if (angle > 180.0f) angle = 180.0f;

  uint32_t duty = AngleToDuty(angle);

  if (pin == 0 || pin == kPinServo0) {
    angle_servo0_ = angle;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, kChannel0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, kChannel0);
    ESP_LOGD(TAG, "Servo 0 (Pin 0) set to %.1f deg (duty: %u)", angle, duty);
  } else if (pin == 25 || pin == kPinServo1) {
    angle_servo1_ = angle;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, kChannel1, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, kChannel1);
    ESP_LOGD(TAG, "Servo 1 (Pin 25) set to %.1f deg (duty: %u)", angle, duty);
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
  ESP_LOGI(TAG, "Both servos centered to 90 degrees");
}

void ServoController::RunSweepTest() {
  ESP_LOGI(TAG, "Starting servo sweep test...");
  // Smoothly move from 90 to 45
  for (int a = 90; a >= 45; a -= 2) {
    SetBothAngles(static_cast<float>(a), static_cast<float>(a));
    delay(15);
  }
  delay(100);
  // Move from 45 to 135
  for (int a = 45; a <= 135; a += 2) {
    SetBothAngles(static_cast<float>(a), static_cast<float>(a));
    delay(15);
  }
  delay(100);
  // Return to 90
  for (int a = 135; a >= 90; a -= 2) {
    SetBothAngles(static_cast<float>(a), static_cast<float>(a));
    delay(15);
  }
  ESP_LOGI(TAG, "Servo sweep test completed at 90 degrees");
}
