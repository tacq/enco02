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

  // Smooth interpolated movements to avoid mechanical inertia/shaking
  void MoveAngleSmooth(int pin, float target_angle, float step_deg = 1.0f, uint32_t step_delay_ms = 18);
  void MoveAllSmooth(float target0, float target1, float target2, float step_deg = 1.0f, uint32_t step_delay_ms = 18);

  // Relative head motion methods (default 10 degrees, strictly bounded by safe limits)
  void LookUp(float delta_deg = kDefaultStepDeg);     // Pin 0 Pitch decreases (抬头, min 40)
  void LookDown(float delta_deg = kDefaultStepDeg);   // Pin 0 Pitch increases (低头, max 120)
  void TiltLeft(float delta_deg = kDefaultStepDeg);   // Pin 25 Roll decreases (向左歪头, min 50)
  void TiltRight(float delta_deg = kDefaultStepDeg);  // Pin 25 Roll increases (向右歪头, max 110)
  void TurnLeft(float delta_deg = kDefaultStepDeg);   // Pin 26 Yaw decreases (向左转头, min 20)
  void TurnRight(float delta_deg = kDefaultStepDeg);  // Pin 26 Yaw increases (向右转头, max 120)

  // Sweep test for calibration
  void RunSweepTest();

  // Trigger cute "摇头晃脑" action (smooth 3-axis tilt + nod + rotate bobble)
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
  float angle_servo2_ = kServo2DefaultAngle;
};

#endif  // _SERVO_CONTROLLER_H_
