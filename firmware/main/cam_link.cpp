#include "cam_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// For esp_read_mac(): the vision token is issued against this board's MAC, so
// the K command has to carry it.
#include <esp_mac.h>

#include "servo_controller.h"
#include "video_sink.h"

namespace {

// Tracking response. These four numbers are what stop the head looking either
// palsied or asleep.

// Ignore anything inside +/-15% of frame centre. A centroid wanders by a few
// percent even against a static scene, and without a deadband the head hunts
// back and forth forever and the servos buzz. Widened from 12 so that small
// shifts - someone leaning, or the centroid breathing around a stationary
// face - leave the head alone entirely.
//
// MUST match the suppression threshold in the cam's ReportSample(): the cam
// stops sending 'T' once the subject is inside this band, so if the two
// numbers disagree the head either hunts or stops short of centre.
constexpr int kDeadband = 15;

// Degrees of head movement per unit of frame offset, applied to the offset
// MEASURED PAST THE DEADBAND (see the step() lambda).
//
// This was 0.30, which made the controller bang-bang rather than proportional
// and is why tracking was not smooth. The step is capped at kMaxStepDeg = 3.0,
// so 0.30 saturated at an offset of just 10 - inside the deadband of 12. Every
// single update outside the deadband therefore produced exactly +/-3.0 deg:
// the head could only be perfectly still or slewing at full speed, with no
// values in between, and it stopped dead the instant it crossed the boundary.
//
// 0.07 puts the knee at an offset of ~58 instead, so the whole usable range of
// the picture maps onto a real spread of speeds: the head creeps when the
// subject is slightly off-centre and only runs flat out when they are near the
// frame edge.
constexpr int kGainNumer = 7;
constexpr int kGainDenom = 100;

// Ceiling per update, in degrees. Together with kMoveIntervalMs this caps the
// head at 50 deg/s - fast enough to keep up with someone walking past, slow
// enough that an MG92B does not slam.
constexpr float kMaxStepDeg = 3.0f;
constexpr uint32_t kMoveIntervalMs = 60;

// Most the per-update step may CHANGE between updates - an acceleration limit.
//
// kMaxStepDeg alone bounds speed but not the rate of change of speed, so the
// head could still go from stationary to 50 deg/s and back within one update.
// That discontinuity is what reads as a jerk even once the gain is sensible.
// At 0.45 deg the head takes ~7 updates (~0.55s at the cam's 12.5fps) to wind
// up to full speed and the same to wind down, which is the easing.
constexpr float kMaxAccelDeg = 0.45f;

// If no usable tracking update arrives for this long, forget the current speed
// and ease in from rest next time. Without this a subject who reappears after
// a gap gets picked up at whatever speed the head was doing when they left.
constexpr uint32_t kVelocityResetMs = 300;

// How long the cam must be silent before CoastTracking() takes over the
// deceleration ramp. Comfortably longer than the cam's 80ms tracking period so
// a normally-arriving stream of updates never trips it, short enough that the
// stop still feels like part of the same movement.
constexpr uint32_t kCoastTakeoverMs = 150;

// How hard the coast brakes, in degrees per update. Firmer than kMaxAccelDeg.
//
// The coast only really runs in one situation: the subject was lost (conf fell
// below kMinConf) while the head was moving, so no more updates arrive and
// nothing else is left to stop it. Reusing the gentle 0.45 acceleration figure
// there means the head glides a simulated 8.55 deg over 420ms after the
// subject disappears - a long, aimless drift toward where they used to be.
// 1.0 brings it to rest in 3.0 deg / 180ms, which still reads as a settle
// rather than a stop but does not wander.
//
// This costs nothing in normal tracking: simulating the full two-board loop
// with decel at 0.45, 0.9 and 1.5 gave identical results (no overshoot, no
// reversals, settling within 80ms of each other), because a converging
// P-controller has already wound the speed down long before the cam goes quiet.
constexpr float kCoastDecelDeg = 1.0f;

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

// Video framing. The magic is two bytes because a single 0xA5 can occur as
// line noise on a floating RX pin, and a false positive costs a whole frame.
constexpr uint8_t kFrameMagic0 = 0xA5;
constexpr uint8_t kFrameMagic1 = 0x5A;
constexpr size_t kFrameHeaderRest = 8;  // len, w, h, crc - the magic is already read

// A 240x176 JPEG at quality 14 is 5-7KB, which is 65-80ms at 921600. 400ms is
// long enough to absorb a slow frame and short enough that a truncated one does
// not stall loop() for a noticeable beat.
constexpr uint32_t kFrameReadTimeoutMs = 400;

// Largest frame worth trying to read. A HQVGA JPEG that claims to be bigger
// than this is a corrupted length field, and honouring it would block the loop
// for a second while the decoder waits for bytes that are not coming.
constexpr uint32_t kMaxFrameBytes = 24000;

// If the picture stops while the viewfinder is up, the cam has either gone to
// recognise something (which takes seconds) or dropped back to 115200 behind
// our back. kVideoStallMs is past both: the vision path is bounded by
// kLookTimeoutMs, and the cam's own fast-link watchdog fires at 7s.
constexpr uint32_t kVideoStallMs = 12000;

// CRC16/CCITT-FALSE, the same polynomial and seed the cam uses. Incremental,
// because the whole point of the streaming decoder is that the frame is never
// held in one piece - so neither can the thing that checks it.
uint16_t Crc16Update(uint16_t crc, const uint8_t* p, size_t n) {
  while (n-- > 0) {
    crc ^= static_cast<uint16_t>(*p++) << 8;
    for (int i = 0; i < 8; ++i) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

// One byte, waiting up to `timeout_ms` for it. Only used to confirm the second
// half of a frame magic, where the byte is already on the wire.
int ReadByteTimed(uint32_t timeout_ms) {
  const uint32_t deadline = millis() + timeout_ms;
  for (;;) {
    const int b = Serial2.read();
    if (b >= 0) {
      return b;
    }
    if (static_cast<int32_t>(millis() - deadline) >= 0) {
      return -1;
    }
  }
}

}  // namespace

CamLink& CamLink::GetInstance() {
  static CamLink instance;
  return instance;
}

void CamLink::Init() {
  if (initialised_) {
    return;
  }
  // 1024 bytes. It was 256, then 512, and neither was the real problem - at
  // 921600 the decoder simply could not drain the buffer as fast as it filled,
  // and no realistic buffer size fixes a sustained deficit. Lowering kFastBaud
  // to 460800 removed the deficit (see the note there), so this only has to
  // absorb transient preemption now: loopTask is priority 1 and the LVGL task
  // is 2, so a few milliseconds of interruption mid-frame is normal. At 460800
  // 1024 bytes is 22ms of slack, which is ample for that and costs 512 bytes
  // over the old value. Set before begin(), which is where the driver
  // allocates it - at boot with ~180KB free, rather than mid-session with 2KB
  // of contiguous heap.
  Serial2.setRxBufferSize(1024);
  Serial2.begin(kBaud, SERIAL_8N1, kPinRx, kPinTx);
  // Bounds readBytes() inside the video path. The Stream default is 1000ms,
  // which on a truncated frame would freeze the head, the screen and the button
  // for a full second.
  Serial2.setTimeout(kFrameReadTimeoutMs);
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

void CamLink::SetVisionEndpoint(const char* url, const char* token) {
  if (url == nullptr || *url == '\0') {
    return;
  }
  // The server re-advertises the same endpoint on every connect. Only log and
  // re-send when something actually changed, so a reconnect loop does not spam
  // the link.
  if (strncmp(vision_url_, url, sizeof(vision_url_)) == 0 && strncmp(vision_token_, token != nullptr ? token : "", sizeof(vision_token_)) == 0) {
    return;
  }
  snprintf(vision_url_, sizeof(vision_url_), "%s", url);
  snprintf(vision_token_, sizeof(vision_token_), "%s", token != nullptr ? token : "");
  printf("cam link: vision endpoint %s\n", vision_url_);
  SendVisionEndpoint();
}

void CamLink::SendVisionEndpoint() {
  if (!initialised_ || vision_url_[0] == '\0') {
    return;
  }

  // Our own MAC, because the token the server issued is bound to this board -
  // not to the camera, which the server has never heard of.
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);

  // 240, from the worst case the compiler works out: "K " + a 127 char url +
  // a 79 char token + a 17 char MAC + separators = 227. The cam's receive
  // buffer is the same size for exactly this reason - if these two ever
  // disagree, the cam silently drops the line as a framing error.
  char cmd[240];
  snprintf(cmd, sizeof(cmd), "K %s %s %02x:%02x:%02x:%02x:%02x:%02x", vision_url_, vision_token_, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  SendCommand(cmd);
}

bool CamLink::TakeGestureArmed() {
  if (!gesture_armed_) {
    return false;
  }
  gesture_armed_ = false;
  return true;
}

bool CamLink::RequestLook(int64_t mcp_id, const char* question) {
  if (!initialised_ || !present_ || look_pending_) {
    return false;
  }

  // Recognition mode. Stop the head BEFORE asking for the photo.
  //
  // Tracking and recognition are mutually exclusive on this hardware: the
  // tracker is steering the lens at the user's face at 3 deg per 60ms, so a
  // capture taken mid-pan is motion-blurred and framed on the wrong thing
  // entirely - which is exactly why "这是什么" came back with nonsense while
  // tracking was on. `A 0` stops the cam reporting, NoteManualHeadCommand()
  // stops this side from acting on any T already in the buffer, and the cam
  // adds its own 350ms settle before the shutter. Tracking is restored when the
  // answer arrives.
  if (tracking_enabled_) {
    look_resume_tracking_ = true;
    SetTrackingEnabled(false);
  }
  NoteManualHeadCommand();

  look_id_ = mcp_id;
  look_pending_ = true;
  look_started_ms_ = millis();

  if (question == nullptr || *question == '\0') {
    SendCommand("V");
    return true;
  }

  // One line per message, so a newline inside the question would split it in
  // two and the cam would act on half a sentence. The assistant writes this
  // text, so it is not hostile, but it is not constrained either.
  char cmd[192];
  int n = snprintf(cmd, sizeof(cmd), "V %s", question);
  if (n < 0) {
    SendCommand("V");
    return true;
  }
  if (static_cast<size_t>(n) >= sizeof(cmd)) {
    n = sizeof(cmd) - 1;
  }
  for (int i = 0; i < n; ++i) {
    if (cmd[i] == '\n' || cmd[i] == '\r') {
      cmd[i] = ' ';
    }
  }
  SendCommand(cmd);
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

void CamLink::ApplyTracking(int dx, int dy, int conf, int roll) {
  if (!tracking_enabled_ || conf < kMinConf) {
    return;
  }
  // A gesture animation owns the servos while it runs; fighting it would make
  // the bobble look broken and the head end up somewhere unintended.
  if (ServoController::GetInstance().IsAnimating()) {
    return;
  }
  const uint32_t now = millis();
  // Any usable update counts as "the cam is still talking", whether or not it
  // ends up moving anything. CoastTracking() keys off this to decide the cam
  // has gone quiet and the ramp-down is now its responsibility.
  last_track_msg_ms_ = now;
  if (static_cast<int32_t>(manual_until_ms_ - now) > 0) {
    return;
  }
  if (now - last_move_ms_ < kMoveIntervalMs) {
    return;
  }

  // Forget the previous speed if the subject has been missing for a moment, so
  // the head eases in from rest rather than resuming mid-slew.
  if (now - last_move_ms_ > kVelocityResetMs) {
    yaw_speed_ = 0.0f;
    pitch_speed_ = 0.0f;
  }

  // Soft deadband: measure the offset from the EDGE of the deadband, not from
  // the centre of the frame.
  //
  // The old version applied the gain to the raw offset, so the response jumped
  // straight from 0 to whatever the gain produced at the boundary - a step
  // discontinuity exactly where the head starts and stops moving, which is the
  // worst possible place for one. Subtracting the deadband makes the speed pass
  // smoothly through zero at the boundary: the head now drifts to a halt as it
  // centres instead of cutting out.
  auto step = [](int offset) -> float {
    int past = 0;
    if (offset >= kDeadband) {
      past = offset - kDeadband;
    } else if (offset <= -kDeadband) {
      past = offset + kDeadband;
    } else {
      return 0.0f;
    }
    float d = static_cast<float>(past * kGainNumer) / static_cast<float>(kGainDenom);
    if (d > kMaxStepDeg) {
      d = kMaxStepDeg;
    } else if (d < -kMaxStepDeg) {
      d = -kMaxStepDeg;
    }
    return d;
  };

  // Acceleration limit. `want` is the speed the controller would like right
  // now; this only lets the actual speed move kMaxAccelDeg toward it per
  // update, which rounds off both the start and the stop of every movement.
  auto accelerate = [](float current, float want) -> float {
    const float delta = want - current;
    if (delta > kMaxAccelDeg) {
      return current + kMaxAccelDeg;
    }
    if (delta < -kMaxAccelDeg) {
      return current - kMaxAccelDeg;
    }
    return want;
  };

  yaw_speed_ = accelerate(yaw_speed_, step(dx));
  pitch_speed_ = accelerate(pitch_speed_, step(dy));

  // Below a twentieth of a degree the servo cannot resolve the difference and
  // we are just writing the same PWM value; treat it as stopped so the
  // "nothing to do" check below can actually fire.
  if (yaw_speed_ < 0.05f && yaw_speed_ > -0.05f) {
    yaw_speed_ = 0.0f;
  }
  if (pitch_speed_ < 0.05f && pitch_speed_ > -0.05f) {
    pitch_speed_ = 0.0f;
  }

  const float yaw_step = yaw_speed_;
  const float pitch_step = pitch_speed_;

  auto& servos = ServoController::GetInstance();

  // Roll (歪头, kPinServo1 GPIO 25): mapped proportionally around neutral 90°
  // when we have a skin-tone face lock (conf >= 60), so tilting your head left
  // or right makes the robot tilt its head to match, and straightening your
  // head smoothly returns Roll to 90°.
  //
  // Clamped to the SAME kServo1Min/MaxAngle the rest of the firmware uses. This
  // was hardcoded to 68..112, which overshot the Roll servo's 110° mechanical
  // limit by two degrees - SetAngle() would have caught it, but a tracker that
  // aims outside the safe range is a tracker that parks the servo on its end
  // stop and stalls it.
  float roll_step = 0.0f;
  if (conf >= 60) {
    float target_roll = (abs(roll) < 15)
                            ? ServoController::kDefaultAngle
                            : (ServoController::kDefaultAngle + static_cast<float>(roll) * 0.20f);
    if (target_roll < ServoController::kServo1MinAngle) {
      target_roll = ServoController::kServo1MinAngle;
    } else if (target_roll > ServoController::kServo1MaxAngle) {
      target_roll = ServoController::kServo1MaxAngle;
    }
    const float diff_roll = target_roll - servos.GetAngle(ServoController::kPinServo1);
    if (diff_roll > 1.0f) {
      roll_step = diff_roll > 2.0f ? 2.0f : diff_roll;
    } else if (diff_roll < -1.0f) {
      roll_step = diff_roll < -2.0f ? -2.0f : diff_roll;
    }
  }

  if (yaw_step == 0.0f && pitch_step == 0.0f && roll_step == 0.0f) {
    return;
  }
  last_move_ms_ = now;

  // Subject right of centre -> turn right -> yaw angle up. Subject below centre
  // -> look down -> pitch angle up.
  //
  // Every write is clamped to that servo's own kServoNMin/MaxAngle here, on top
  // of the clamp inside SetAngle(). Belt and braces is warranted: this is the
  // one code path that moves the servos continuously and unattended, and a
  // tracker that walks the head into an end stop and holds it there stalls an
  // MG92B at ~700mA until something gives.
  //
  // SetAngle(), not MoveAngleSmooth(): the smooth variant sleeps ~18ms per
  // degree, and loop() also drives the display, the event pump and the timer.
  auto move = [&servos](gpio_num_t pin, float step, float lo, float hi) {
    if (step == 0.0f) {
      return;
    }
    float target = servos.GetAngle(pin) + step;
    if (target < lo) {
      target = lo;
    } else if (target > hi) {
      target = hi;
    }
    servos.SetAngle(pin, target);
  };

  move(ServoController::kPinServo2, yaw_step, ServoController::kServo2MinAngle, ServoController::kServo2MaxAngle);
  move(ServoController::kPinServo0, pitch_step, ServoController::kServo0MinAngle, ServoController::kServo0MaxAngle);
  move(ServoController::kPinServo1, roll_step, ServoController::kServo1MinAngle, ServoController::kServo1MaxAngle);
}

// Finishes a movement after the cam has stopped talking.
//
// The deceleration ramp in ApplyTracking() only advances when a 'T' arrives,
// and the cam deliberately stops sending them the moment the subject is inside
// the deadband and holding still. Those are exactly the circumstances at the
// end of every movement, so without this the head would reach the deadband at
// full speed and simply cease to be updated - stopping dead, which is the jerk
// the acceleration limit exists to remove. This keeps easing the stored speed
// down to zero on our own clock once the cam goes quiet.
void CamLink::CoastTracking() {
  if (yaw_speed_ == 0.0f && pitch_speed_ == 0.0f) {
    return;
  }
  if (!tracking_enabled_ || ServoController::GetInstance().IsAnimating()) {
    yaw_speed_ = 0.0f;
    pitch_speed_ = 0.0f;
    return;
  }
  const uint32_t now = millis();
  if (static_cast<int32_t>(manual_until_ms_ - now) > 0) {
    yaw_speed_ = 0.0f;
    pitch_speed_ = 0.0f;
    return;
  }
  // Only take over once the cam has actually gone quiet; while updates are
  // flowing ApplyTracking() owns the ramp and two of us stepping it would
  // decelerate at twice the intended rate.
  if (now - last_track_msg_ms_ < kCoastTakeoverMs) {
    return;
  }
  if (now - last_move_ms_ < kMoveIntervalMs) {
    return;
  }

  auto decay = [](float v) -> float {
    if (v > kCoastDecelDeg) {
      return v - kCoastDecelDeg;
    }
    if (v < -kCoastDecelDeg) {
      return v + kCoastDecelDeg;
    }
    return 0.0f;
  };
  yaw_speed_ = decay(yaw_speed_);
  pitch_speed_ = decay(pitch_speed_);

  if (yaw_speed_ == 0.0f && pitch_speed_ == 0.0f) {
    return;
  }
  last_move_ms_ = now;

  auto& servos = ServoController::GetInstance();
  auto move = [&servos](gpio_num_t pin, float step, float lo, float hi) {
    if (step == 0.0f) {
      return;
    }
    float target = servos.GetAngle(pin) + step;
    if (target < lo) {
      target = lo;
    } else if (target > hi) {
      target = hi;
    }
    servos.SetAngle(pin, target);
  };
  move(ServoController::kPinServo2, yaw_speed_, ServoController::kServo2MinAngle, ServoController::kServo2MaxAngle);
  move(ServoController::kPinServo0, pitch_speed_, ServoController::kServo0MinAngle, ServoController::kServo0MaxAngle);
}

// Changes the link speed on both boards, and waits for the cam to confirm.
//
// The confirmation is the whole point. The first version of this guessed: send
// the command, wait 60ms, switch. That lost, and lost silently - the cam only
// polls its UART between frames, and it was mid-way through transmitting a 6KB
// JPEG (65ms at the fast rate), so this board dropped to 115200 while the cam
// was still at 921600. Every command that followed - `F 0`, `A 0`, and the `V`
// carrying a "这是什么" - went into a camera that was not listening at that
// speed, and the user got "摄像头好像没反应" fifteen seconds later.
//
// So: send at the old rate, switch, then wait for the cam's `B` ack at the new
// rate. Returns false if it never comes, which is the caller's cue that the
// link is not where it thinks it is.
bool CamLink::SetLinkFast(bool fast, uint32_t ack_timeout_ms) {
  if (!initialised_) {
    return false;
  }
  if (fast == link_fast_) {
    return true;
  }
  SendCommand(fast ? "B 1" : "B 0");
  Serial2.flush();

  // Drain BEFORE the switch, not after. Anything sitting in the FIFO now was
  // clocked at the old rate and is noise once we retune, so it has to go - but
  // draining on the far side of updateBaudRate() is a race the ack can lose.
  //
  // The cam answers by doing the same dance we do: flush, delay(5), retune,
  // send "B". When its loop is idle it reads our line in well under a
  // millisecond, so its ack lands ~5.1ms after our flush - and our own delay(5)
  // expires at ~5.0ms. A drain placed after the switch therefore had roughly
  // 100us of margin against a two-byte reply, and would silently eat it
  // whenever the scheduler nudged us the wrong way. That is exactly what was
  // observed: "no ack for 460800 baud" followed by 8s of "camera lost" while
  // the cam - which HAD switched - waited for its own idle watchdog. Earlier
  // successes ("ack in 142ms") were the cases where the cam happened to be busy
  // with a frame grab and replied late enough to miss the drain.
  //
  // Nothing is lost by moving it: bytes still in flight during the delay are
  // read back as garbage lines, and the parser below already discards any line
  // that is not exactly "B".
  while (Serial2.available() > 0) {
    Serial2.read();
  }

  delay(5);
  Serial2.updateBaudRate(fast ? kFastBaud : kBaud);
  link_fast_ = fast;
  line_len_ = 0;

  const uint32_t deadline = millis() + ack_timeout_ms;
  uint8_t len = 0;
  char ack[8];
  while (static_cast<int32_t>(millis() - deadline) < 0) {
    const int c = Serial2.read();
    if (c < 0) {
      delay(1);
      continue;
    }
    if (c == '\n' || c == '\r') {
      if (len == 1 && ack[0] == 'B') {
        printf("cam link: %u baud (ack in %ums)\n", static_cast<unsigned>(fast ? kFastBaud : kBaud),
               static_cast<unsigned>(ack_timeout_ms - (deadline - millis())));
        return true;
      }
      len = 0;
      continue;
    }
    if (len < sizeof(ack) - 1) {
      ack[len++] = static_cast<char>(c);
    } else {
      len = 0;
    }
  }

  printf("cam link: no ack for %u baud\n", static_cast<unsigned>(fast ? kFastBaud : kBaud));
  return false;
}

bool CamLink::BeginVideo() {
  if (!initialised_ || !present_ || !video_sink::IsOpen()) {
    return false;
  }
  if (video_active_) {
    return true;
  }
  // 400ms: with video off the cam's loop is short, but it may still be in the
  // middle of a tracking frame grab or an HTTP poll.
  if (!SetLinkFast(true, 400)) {
    // A missing ack does NOT mean the cam ignored us. It may well have switched
    // and had its reply lost, in which case both ends are now at different
    // rates and every command is noise until the cam's 7s idle watchdog drops
    // it back on its own - eight seconds of "camera lost" for what is really a
    // two-byte hiccup.
    //
    // So tell it to come back down while we are still speaking its (possible)
    // new language, then revert. If it never switched, this line is garbage to
    // a port listening at 115200 and is discarded as an unknown command.
    SendCommand("B 0");
    Serial2.flush();
    delay(5);
    Serial2.updateBaudRate(kBaud);
    link_fast_ = false;
    line_len_ = 0;
    return false;
  }
  video_frames_ = 0;
  video_bad_ = 0;
  video_retry_ = 0;
  last_frame_ms_ = millis();
  SendCommand("F 1");
  video_active_ = true;
  return true;
}

void CamLink::EndVideo() {
  const bool was_active = video_active_;
  video_active_ = false;
  if (!initialised_) {
    return;
  }

  if (was_active) {
    printf("cam link: video off after %u frames, %u bad\n", static_cast<unsigned>(video_frames_), static_cast<unsigned>(video_bad_));
    // Stop the stream BEFORE changing speed, at the rate the cam is currently
    // listening at. Otherwise the baud switch races the frame still on the wire.
    SendCommand("F 0");
    Serial2.flush();
  }

  // 900ms, because `F 0` sends the cam into esp_camera_init() to go back to the
  // tracking frame size, and it services no UART at all for a few hundred
  // milliseconds while it does. The `B 0` waits in its receive buffer until it
  // is done; this side must not give up before then, because everything that
  // follows - `A 0`, `V` - depends on both ends agreeing on the rate.
  SetLinkFast(false, 900);

  // Whatever state the ack left us in, the slow rate is where the cam ends up:
  // if it never heard `B 0`, its own idle watchdog drops it back within 7s.
  if (link_fast_) {
    Serial2.updateBaudRate(kBaud);
    link_fast_ = false;
  }
}

size_t CamLink::VideoRead(void* ctx, uint8_t* dst, size_t len) {
  auto* self = static_cast<CamLink*>(ctx);
  if (self == nullptr || len == 0 || self->frame_left_ == 0) {
    return 0;
  }
  if (len > self->frame_left_) {
    len = self->frame_left_;  // never read past this frame into the next
  }

  // 64 bytes, only used for the discard path (tjpgd skipping a segment it does
  // not care about, or us draining a tail the decoder stopped short of). Those
  // bytes still have to go through the CRC, so they cannot simply be dropped.
  uint8_t sink[64];
  size_t got = 0;
  const uint32_t deadline = millis() + kFrameReadTimeoutMs;
  while (got < len) {
    size_t chunk = len - got;
    uint8_t* p = dst;
    if (dst != nullptr) {
      p = dst + got;
    } else {
      p = sink;
      if (chunk > sizeof(sink)) {
        chunk = sizeof(sink);
      }
    }
    const size_t n = Serial2.readBytes(p, chunk);
    if (n == 0) {
      break;  // the cam stopped mid-frame
    }
    self->frame_crc_ = Crc16Update(self->frame_crc_, p, n);
    got += n;
    self->frame_left_ -= n;
    if (static_cast<int32_t>(millis() - deadline) >= 0) {
      break;
    }
  }
  return got;
}

// One video frame. The two magic bytes are already consumed.
//
//   A5 5A | len:u16 | w:u16 | h:u16 | crc:u16 | <len bytes of JPEG>
//
// Nothing here buffers the payload: it is pulled through VideoRead() by the
// decoder, 16x16 pixels at a time, and CRCed on the way past. The board has
// 14KB of contiguous heap on a good day and a frame is 6KB, so this is not a
// micro-optimisation - it is the only way the feature fits.
void CamLink::ReceiveVideoFrame() {
  uint8_t hdr[kFrameHeaderRest];
  if (Serial2.readBytes(hdr, sizeof(hdr)) != sizeof(hdr)) {
    ++video_bad_;
    return;
  }
  const uint32_t len = static_cast<uint32_t>(hdr[0]) | (static_cast<uint32_t>(hdr[1]) << 8);
  const uint16_t w = static_cast<uint16_t>(hdr[2] | (hdr[3] << 8));
  const uint16_t h = static_cast<uint16_t>(hdr[4] | (hdr[5] << 8));
  const uint16_t want_crc = static_cast<uint16_t>(hdr[6] | (hdr[7] << 8));

  // A length or geometry outside what the cam can physically send means the
  // header itself was corrupted. Reading `len` bytes on that basis would eat
  // into the next frame and desynchronise the stream for good.
  if (len == 0 || len > kMaxFrameBytes || w == 0 || h == 0 || w > 640 || h > 480) {
    ++video_bad_;
    return;
  }

  frame_left_ = len;
  frame_crc_ = 0xFFFF;

  const uint32_t decode_start_ms = millis();

  bool ok = false;
  if (video_active_ && video_sink::IsOpen()) {
    ok = video_sink::DecodeFrame(&CamLink::VideoRead, this);
  }
  // Whatever happened, the rest of the payload has to come off the wire. A JPEG
  // is high-entropy, so a A5 5A pair inside an abandoned tail is a matter of
  // time - and it would be read as the start of a frame.
  if (frame_left_ > 0) {
    VideoRead(this, nullptr, frame_left_);
  }

  const uint32_t decode_ms = millis() - decode_start_ms;

  last_frame_ms_ = millis();
  last_rx_ms_ = last_frame_ms_;  // a frame proves the cam is alive as well as a line does

  if (ok && frame_crc_ == want_crc) {
    ++video_frames_;
    // After the picture, not before: it is drawn onto the frame, and the frame
    // would paint over it.
    video_sink::DrawReticle();
  } else {
    ++video_bad_;
  }

  // The one measurement that says whether this link rate is sustainable.
  //
  // `wire` is how long `len` bytes take to arrive at kFastBaud; `decode` is how
  // long we took to consume them. Decode longer than wire means we are falling
  // behind every frame and the driver buffer is absorbing the difference until
  // it cannot - which is exactly how 921600 failed. Seeing decode comfortably
  // under wire here is the evidence that 460800 fixed it rather than just
  // moving the problem.
  video_decode_ms_ += decode_ms;
  ++video_timed_;
  if (millis() - video_stat_ms_ >= 2000) {
    video_stat_ms_ = millis();
    const uint32_t avg = video_timed_ ? (video_decode_ms_ / video_timed_) : 0;
    const uint32_t wire_ms = (len * 10UL * 1000UL) / kFastBaud;
    printf("video: %u ok, %u bad, %uB/frame, decode %ums vs wire %ums\n", static_cast<unsigned>(video_frames_),
           static_cast<unsigned>(video_bad_), static_cast<unsigned>(len), static_cast<unsigned>(avg),
           static_cast<unsigned>(wire_ms));
    video_decode_ms_ = 0;
    video_timed_ = 0;
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
    SendVisionEndpoint();
  }

  switch (line[0]) {
    case 'T': {
      int dx = 0;
      int dy = 0;
      int conf = 0;
      int roll = 0;
      if (sscanf(line + 1, "%d %d %d %d", &dx, &dy, &conf, &roll) >= 3) {
        ApplyTracking(dx, dy, conf, roll);
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
      // The photo has been taken and answered; put the head back to work if the
      // user had tracking on before they asked.
      if (look_resume_tracking_) {
        look_resume_tracking_ = false;
        SetTrackingEnabled(true);
      }
      break;
    }
    case 'G': {
      // One raised finger, seen by the cam. It arms its own tracking the moment
      // it decides, so this only has to agree rather than reply - but going
      // through SetTrackingEnabled() keeps tracking_armed_ honest and stops
      // Poll() from immediately un-arming it.
      if (line[1] == ' ' && line[2] == '1' && !tracking_enabled_) {
        printf("cam link: one-finger gesture -> tracking on\n");
        gesture_armed_ = true;
        SetTrackingEnabled(true);
      }
      break;
    }
    case 'R': {
      // The cam rebooted, so its tracking state is back to the default - and
      // so is its vision endpoint, which lives in RAM over there.
      tracking_armed_ = false;
      SendVisionEndpoint();
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

  // Before reading anything: if a movement is winding down and the cam has
  // gone quiet, keep easing it to a stop. This has to run on our clock rather
  // than on arriving messages, because the cam stops sending precisely when
  // the head is arriving at centre.
  CoastTracking();

  while (Serial2.available() > 0) {
    const int c = Serial2.read();
    if (c < 0) {
      break;
    }
    // Video frames are binary and a JPEG is full of 0x0A, so they cannot travel
    // as lines. 0xA5 never occurs in the ASCII protocol, which lets the parser
    // stay exactly as it was and simply step aside when it sees the magic. Only
    // checked while the viewfinder is up, so nothing changes the rest of the
    // time.
    if (video_active_ && c == kFrameMagic0) {
      const int c2 = ReadByteTimed(5);
      if (c2 == kFrameMagic1) {
        line_len_ = 0;  // a frame cannot arrive mid-line; if it did, that line was junk
        ReceiveVideoFrame();
      }
      // A lone 0xA5 is noise. Both bytes are dropped: they are not ASCII, so
      // they could only corrupt a line anyway.
      continue;
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

  // Ping both while searching (!present_) and whenever the link has been quiet
  // for kPingIntervalMs (e.g., user is centered inside the deadband, room is
  // empty, or tracking is disabled with A 0) so kPresenceTimeoutMs never trips
  // on an attached camera.
  //
  // `|| video_active_` is not redundant. A video frame stamps last_rx_ms_ (see
  // ReceiveVideoFrame), so while streaming the link never looks quiet and this
  // never fired - meaning the cam received no commands at all for the whole
  // session. Its own 7s fast-link watchdog then did what it is designed to do
  // and dropped back to 115200, mid-stream, while this side stayed fast. Every
  // session died at the 7 second mark: frames stopped, then 8s later
  // "camera lost".
  //
  // Traffic is one-way here - the cam is talking, we are not - so liveness has
  // to be asserted deliberately. The `P` costs two bytes on an idle direction,
  // and the cam answers it between frames, never inside one.
  if ((!present_ || video_active_ || (now - last_rx_ms_ > kPingIntervalMs)) &&
      (now - last_ping_ms_ > kPingIntervalMs)) {
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
    // The cam never answered, but tracking was switched off on its behalf. Put
    // it back rather than leaving the head dead until the user asks again.
    if (look_resume_tracking_) {
      look_resume_tracking_ = false;
      SetTrackingEnabled(true);
    }
  }

  // The picture stopped. Either the cam reset, or its own fast-link watchdog
  // fired and it is back at 115200 while we are still listening at 921600 -
  // from here those look identical. Drop the link speed, which is the state the
  // cam will be in either way, and stop claiming video is running so the UI can
  // take itself off the viewfinder.
  if (video_active_ && now - last_frame_ms_ > kVideoStallMs) {
    printf("cam link: video stalled after %u frames (%u bad)\n", static_cast<unsigned>(video_frames_), static_cast<unsigned>(video_bad_));
    EndVideo();
  }

  // Nothing at all since BeginVideo(). The likeliest cause is the cam having
  // been mid-capture when the baud switch went out, so it never saw `F 1` - it
  // reads its UART between frames, not during one. Say it again at the new
  // rate, then give up fast: twelve seconds of a blank viewfinder is a much
  // worse failure than two.
  if (video_active_ && video_frames_ == 0 && video_bad_ == 0) {
    const uint32_t waited = now - last_frame_ms_;
    if (waited > 700 && video_retry_ == 0) {
      video_retry_ = 1;
      SendCommand("F 1");
    } else if (waited > 2000) {
      printf("cam link: no video from cam, giving up\n");
      EndVideo();
    }
  }

  // Too much of the stream is arriving damaged to be worth showing. Give up on
  // video rather than put a screen of coloured hash in front of the user;
  // 115200 still carries tracking and recognition perfectly well, which are the
  // features that matter.
  //
  // The message deliberately does NOT say "too noisy". It said that once, and
  // it was wrong: the failures were jd_prepare=OK with jd_decomp=JDR_FMT1 on
  // every frame, which is a starved decoder, not a dirty wire. If this fires
  // again, check whether the header is still parsing before blaming the cable.
  if (video_active_ && video_bad_ > 12 && video_bad_ > video_frames_) {
    printf("cam link: %u of %u frames corrupt at %u baud - giving up on video\n", static_cast<unsigned>(video_bad_),
           static_cast<unsigned>(video_bad_ + video_frames_), static_cast<unsigned>(kFastBaud));
    EndVideo();
  }
}
