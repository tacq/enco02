#include "cam_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "servo_controller.h"

namespace {

// Tracking response. These four numbers are what stop the head looking either
// palsied or asleep.

// Ignore anything inside +/-12% of frame centre. A centroid wanders by a few
// percent even against a static scene, and without a deadband the head hunts
// back and forth forever and the servos buzz.
constexpr int kDeadband = 12;

// Degrees of head movement per 100% of frame offset. A subject at the very edge
// of frame is about 30 degrees off axis for this lens, so the head needs to
// travel roughly that far to centre them.
constexpr int kGainNumer = 30;
constexpr int kGainDenom = 100;

// Ceiling per update, in degrees. Together with kMoveIntervalMs this caps the
// head at 50 deg/s - fast enough to keep up with someone walking past, slow
// enough that an MG92B does not slam.
constexpr float kMaxStepDeg = 3.0f;
constexpr uint32_t kMoveIntervalMs = 60;

// Below this the cam is guessing (motion fallback on a noisy frame), and acting
// on a guess is how the head ends up staring at a curtain.
constexpr int kMinConf = 30;

// After an explicit "向左转头", leave the head where the user put it for a
// while instead of immediately dragging it back.
constexpr uint32_t kManualHoldMs = 4000;

// If nothing has arrived for this long the cam is unplugged, unpowered or
// wedged. Stop claiming the feature works.
constexpr uint32_t kPresenceTimeoutMs = 8000;
constexpr uint32_t kPingIntervalMs = 3000;

}  // namespace

CamLink& CamLink::GetInstance() {
  static CamLink instance;
  return instance;
}

void CamLink::Init() {
  if (initialised_) {
    return;
  }
  // 256 bytes is four times the longest message. It must be set before begin(),
  // which is where the driver allocates it - at boot, with ~180KB free, rather
  // than mid-session with 2KB of contiguous heap.
  Serial2.setRxBufferSize(256);
  Serial2.begin(kBaud, SERIAL_8N1, kPinRx, kPinTx);
  initialised_ = true;
  line_len_ = 0;
  result_[0] = '\0';
  printf("cam link: uart2 rx=%d tx=%d @%u\n", kPinRx, kPinTx, static_cast<unsigned>(kBaud));
}

void CamLink::SendCommand(const char* cmd) {
  if (!initialised_) {
    return;
  }
  Serial2.print(cmd);
  Serial2.print('\n');
}

void CamLink::SetTrackingEnabled(bool enabled) {
  tracking_enabled_ = enabled;
  // Push it immediately rather than waiting for the next ping, so "别看我了"
  // takes effect while the user is still saying it.
  SendCommand(enabled ? "A 1" : "A 0");
  tracking_armed_ = enabled;
}

void CamLink::NoteManualHeadCommand() {
  manual_until_ms_ = millis() + kManualHoldMs;
}

bool CamLink::RequestLook(int64_t mcp_id) {
  if (!initialised_ || !present_ || look_pending_) {
    return false;
  }
  look_id_ = mcp_id;
  look_pending_ = true;
  look_started_ms_ = millis();
  SendCommand("V");
  return true;
}

bool CamLink::TakeLookResult(int64_t* id, bool* ok, const char** text) {
  if (!result_ready_) {
    return false;
  }
  result_ready_ = false;
  *id = result_id_;
  *ok = result_ok_;
  *text = result_;
  return true;
}

void CamLink::ApplyTracking(int dx, int dy, int conf) {
  if (!tracking_enabled_ || conf < kMinConf) {
    return;
  }
  // A gesture animation owns the servos while it runs; fighting it would make
  // the bobble look broken and the head end up somewhere unintended.
  if (ServoController::GetInstance().IsAnimating()) {
    return;
  }
  const uint32_t now = millis();
  if (static_cast<int32_t>(manual_until_ms_ - now) > 0) {
    return;
  }
  if (now - last_move_ms_ < kMoveIntervalMs) {
    return;
  }

  auto step = [](int offset) -> float {
    if (offset > -kDeadband && offset < kDeadband) {
      return 0.0f;
    }
    float d = static_cast<float>(offset * kGainNumer) / static_cast<float>(kGainDenom);
    if (d > kMaxStepDeg) {
      d = kMaxStepDeg;
    } else if (d < -kMaxStepDeg) {
      d = -kMaxStepDeg;
    }
    return d;
  };

  const float yaw_step = step(dx);
  const float pitch_step = step(dy);
  if (yaw_step == 0.0f && pitch_step == 0.0f) {
    return;
  }
  last_move_ms_ = now;

  auto& servos = ServoController::GetInstance();
  // Subject right of centre -> turn right -> yaw angle up. Subject below centre
  // -> look down -> pitch angle up. SetAngle() clamps to the safe range, so a
  // person standing off to the side just parks the head at its end stop.
  //
  // SetAngle(), not MoveAngleSmooth(): the smooth variant sleeps ~18ms per
  // degree, and loop() also drives the display, the event pump and the timer.
  if (yaw_step != 0.0f) {
    servos.SetAngle(ServoController::kPinServo2, servos.GetAngle(ServoController::kPinServo2) + yaw_step);
  }
  if (pitch_step != 0.0f) {
    servos.SetAngle(ServoController::kPinServo0, servos.GetAngle(ServoController::kPinServo0) + pitch_step);
  }
}

void CamLink::HandleLine(const char* line) {
  last_rx_ms_ = millis();
  if (!present_) {
    present_ = true;
    printf("cam link: camera present\n");
    // Whatever it thinks its state is, make it match ours.
    SendCommand(tracking_enabled_ ? "A 1" : "A 0");
    tracking_armed_ = tracking_enabled_;
  }

  switch (line[0]) {
    case 'T': {
      int dx = 0;
      int dy = 0;
      int conf = 0;
      if (sscanf(line + 1, "%d %d %d", &dx, &dy, &conf) == 3) {
        ApplyTracking(dx, dy, conf);
      }
      break;
    }
    case 'L':
    case 'E': {
      if (!look_pending_) {
        break;  // late arrival after a timeout, or an unsolicited boot error
      }
      look_pending_ = false;
      result_ready_ = true;
      result_ok_ = (line[0] == 'L');
      result_id_ = look_id_;
      const char* body = (line[1] == ' ') ? line + 2 : line + 1;
      strncpy(result_, body, sizeof(result_) - 1);
      result_[sizeof(result_) - 1] = '\0';
      break;
    }
    case 'R': {
      // The cam rebooted, so its tracking state is back to the default.
      tracking_armed_ = false;
      break;
    }
    default:
      break;
  }
}

void CamLink::Poll() {
  if (!initialised_) {
    return;
  }

  while (Serial2.available() > 0) {
    const int c = Serial2.read();
    if (c < 0) {
      break;
    }
    if (c == '\n' || c == '\r') {
      if (line_len_ > 0) {
        line_[line_len_] = '\0';
        HandleLine(line_);
        line_len_ = 0;
      }
      continue;
    }
    if (line_len_ < sizeof(line_) - 1) {
      line_[line_len_++] = static_cast<char>(c);
    } else {
      // A line this long is a framing error, not a message. Drop it whole
      // rather than act on a fragment.
      line_len_ = 0;
    }
  }

  const uint32_t now = millis();

  if (present_ && now - last_rx_ms_ > kPresenceTimeoutMs) {
    present_ = false;
    tracking_armed_ = false;
    printf("cam link: camera lost\n");
  }

  if (!present_ && now - last_ping_ms_ > kPingIntervalMs) {
    last_ping_ms_ = now;
    SendCommand("P");
  }

  // Re-arm if a reboot left the cam disagreeing with us.
  if (present_ && tracking_armed_ != tracking_enabled_ && now - last_ping_ms_ > kPingIntervalMs) {
    last_ping_ms_ = now;
    SendCommand(tracking_enabled_ ? "A 1" : "A 0");
    tracking_armed_ = tracking_enabled_;
  }

  if (look_pending_ && now - look_started_ms_ > kLookTimeoutMs) {
    look_pending_ = false;
    result_ready_ = true;
    result_ok_ = false;
    result_id_ = look_id_;
    strncpy(result_, "camera did not answer", sizeof(result_) - 1);
    result_[sizeof(result_) - 1] = '\0';
  }
}
