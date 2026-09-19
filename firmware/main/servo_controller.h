#pragma once

#ifndef _SERVO_CONTROLLER_H_
#define _SERVO_CONTROLLER_H_

#include <Arduino.h>
#include <driver/ledc.h>

class ServoController {
 public:
  static constexpr gpio_num_t kPinServo0 = GPIO_NUM_0;   // Servo 1 (e.g. Yaw / Horizontal)
  static constexpr gpio_num_t kPinServo1 = GPIO_NUM_25;  // Servo 2 (e.g. Pitch / Vertical)

  static constexpr ledc_channel_t kChannel0 = LEDC_CHANNEL_0;
  static constexpr ledc_channel_t kChannel1 = LEDC_CHANNEL_1;

  static constexpr uint32_t kPwmFreqHz = 50;              // 50Hz for standard RC servos
  static constexpr ledc_timer_bit_t kPwmResolution = LEDC_TIMER_14_BIT;
  static constexpr uint32_t kPwmPeriodTicks = 16384;      // 2^14
  static constexpr uint32_t kPwmPeriodUs = 20000;         // 20ms

  // MG92B pulse width range in microseconds
  static constexpr float kMinPulseUs = 500.0f;            // 0 degrees
  static constexpr float kMaxPulseUs = 2500.0f;           // 180 degrees
  static constexpr float kDefaultAngle = 90.0f;           // Neutral center

  // Safe operating angle ranges
  static constexpr float kServo0MinAngle = 40.0f;         // GPIO 0 Min safe angle
  static constexpr float kServo0MaxAngle = 120.0f;        // GPIO 0 Max safe angle
  static constexpr float kServo1MinAngle = 50.0f;         // GPIO 25 Min safe angle
  static constexpr float kServo1MaxAngle = 110.0f;        // GPIO 25 Max safe angle

  static ServoController& GetInstance();

  // Initialize LEDC timers, channels, and position both servos to 90 degrees
  void Init();

  // Set angle (0.0 to 180.0 degrees) for a specific pin (0 or 25)
  void SetAngle(int pin, float angle);

  // Set angle by channel index (0 or 1)
  void SetAngleByIndex(int index, float angle);

  // Set both angles at once
  void SetBothAngles(float angle0, float angle25);

  // Get current angle for a pin (0 or 25)
  float GetAngle(int pin) const;

  // Center both servos to 90 degrees
  void CenterAll();

  // Sweep test for calibration
  void RunSweepTest();

  // Trigger cute "摇头晃脑" action (smooth tilt + nod bobble)
  void TriggerHeadBobble();
  void RunHeadBobble();
  bool IsAnimating() const { return is_animating_; }

 private:
  ServoController() = default;
  ~ServoController() = default;
  ServoController(const ServoController&) = delete;
  ServoController& operator=(const ServoController&) = delete;

  uint32_t AngleToDuty(float angle) const;

  bool initialized_ = false;
  volatile bool is_animating_ = false;
  float angle_servo0_ = kDefaultAngle;
  float angle_servo1_ = kDefaultAngle;
};

#endif  // _SERVO_CONTROLLER_H_
