#include "cam_link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// For esp_read_mac(): the vision token is issued against this board's MAC, so
// the K command has to carry it.
#include <esp_mac.h>
#include <hal/uart_ll.h>

#include <Preferences.h>
#include <math.h>

#include "servo_controller.h"
#include "video_sink.h"

namespace {

// Tracking response. These four numbers are what stop the head looking either
// palsied or asleep.

// Ignore anything inside +/-12% of frame centre. A finger held "still" still
// wanders by a few percent, and without a deadband the head hunts back and
// forth forever and the servos buzz.
//
// MUST match kCentredBand in the cam's cam_main.cpp: the cam stops sending 'T'
// once the finger is inside this band and holding still, so if the two numbers
// disagree the head either hunts or stops short of centre.
constexpr int kDeadband = 12;

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
// 0.07 puts the knee at an offset of ~55 instead, so the whole usable range of
// the picture maps onto a real spread of speeds: the head creeps when the
// finger is slightly off-centre and only runs flat out when it is near the
// frame edge.
//
// Pitch gets less: the picture is 4:3, so one unit of dy is ~0.2 deg of view
// against ~0.27 deg for dx, and the same gain would make the vertical loop a
// third more aggressive (and overshoot, with the cam's ~2 frames of lag).
constexpr int kYawGainNumer = 7;
constexpr int kPitchGainNumer = 5;
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

// OFF. Measured live (2026-09-26): with the defaults the head centred the finger on both axes
// (yaw 50 -> 74 took dx -94 -> +17; pitch 82 -> 63 took dy -82 -> -13), but the learner still
// flipped yaw, twice in one session, because the hand itself moved faster than the head and a
// moving subject looks exactly like a wrong-way turn. The defaults below are right for this build;
// the web page's flip buttons remain for a rebuilt head.
constexpr bool kAutoLearnDirections = false;

// Direction learning. With the camera on the head, turning the right way pulls the subject toward
// centre; turning the wrong way pushes them out. After the head has really moved kLearnWindowDeg on
// an axis, the subject's offset is compared with where it was at the start of that window. 4 deg is
// ~12 offset units on a ~65 deg lens, so a wrong turn adds well over kLearnMargin; kLearnVotes wrong
// windows in a row (a person walking faster than the head can produce one) flips the axis.
constexpr float kLearnWindowDeg = 4.0f;
constexpr int kLearnMargin = 8;
constexpr uint8_t kLearnVotes = 2;
// Pinned on an end stop with the subject still far off to that side for this long also counts as a
// wrong-way vote - but only until the axis has been confirmed right a few times, after which it
// just means the person is outside the head's range.
constexpr int kLearnStuckOffset = 35;
constexpr uint32_t kLearnStuckMs = 2000;
constexpr uint8_t kLearnConfirmedEnough = 3;
// Direction learning only listens to confident (strict-shape) finger reports. A loose match is the
// cam following an existing lock through a poor frame; if that frame was really a knuckle or the
// desk edge, its offset says nothing about which way the head should have turned.
constexpr int kLearnMinConf = 80;

// Roll (歪头) mirrors the finger's lean. The cam is NOT on the roll stage - measured: rolling the
// head does not rotate the picture - so this cannot be a closed loop the way yaw and pitch are. It
// is a straight mapping from the lean the cam reports to a roll angle: finger tipped to one side,
// head tipped the same way, as if the head were lining itself up with the finger.
//
// Soft deadband, like the yaw/pitch step(): the tilt is measured from the edge of the deadband, so
// the target passes through upright continuously instead of jumping 6 deg when the lean crosses
// it. 1:1 past that, capped at +/-20 deg, which stays inside the roll servo's 50..110 on both sides
// of 90.
constexpr int kLeanDeadbandDeg = 6;
constexpr float kLeanGain = 1.0f;
constexpr float kMaxLeanTiltDeg = 20.0f;
// Re-aim the roll only for a change at least this big. The lean still jitters a degree or two frame
// to frame after the cam's smoothing, and a servo nudged by 1 deg every 80ms buzzes.
constexpr float kLeanRetargetDeg = 2.0f;
// Roll glide speed as a fraction of the roll axis' tuned speed. Slow on purpose: this is an
// expression, not a correction, and nothing is waiting on it.
constexpr float kLeanGlideScale = 0.6f;

// Face (or, in finger builds, finger) out of sight this long: glide back to the neutral pose, once,
// and wait there - which is also where the user is most likely to reappear.
constexpr uint32_t kFingerLostRecentreMs = 4000;
constexpr float kRecentreGlideScale = 0.6f;
// Armed by the gesture (not by voice or the web page) and no finger for this long: the user has
// moved on, so disarm, which hands the head back to the idle animations and puts the cam back to
// watching for the gesture.
constexpr uint32_t kGestureIdleOffMs = 20000;

// If nothing has arrived for this long the cam is unplugged, unpowered or
// wedged. Stop claiming the feature works.
constexpr uint32_t kPresenceTimeoutMs = 8000;
constexpr uint32_t kPingIntervalMs = 3000;

// Default yaw and roll directions for the face-tracking camera firmware, and the NVS keys they are
// saved under. Both are the reverse of what the finger build used: measured live (2026-09-28), the
// head turned to ITS left when the user moved to their left (away from them - it should turn to its
// right, towards them), and tilted the same way as the user instead of mirroring. Both axes flipping
// together is what a horizontally mirrored picture does; the Arduino 2.0.x camera driver the face
// build moved to evidently delivers the OV2640's picture the other way round. New keys, so a value
// saved for the finger build cannot bring the old sense back; the web page's flip buttons still work
// and are saved under these keys.
constexpr int8_t kDefaultYawDir = 1;
constexpr int8_t kDefaultRollDir = -1;
constexpr const char* kYawDirKey = "ydir_f";
constexpr const char* kRollDirKey = "rdir_f";

// Video framing. The magic is two bytes because a single 0xA5 can occur as
// line noise on a floating RX pin, and a false positive costs a whole frame.
constexpr uint8_t kFrameMagic0 = 0xA5;
constexpr uint8_t kFrameMagic1 = 0x5A;
constexpr size_t kFrameHeaderRest = 8;  // len, w, h, crc - the magic is already read

// Face packet: A5 F1 | seq dx dy roll conf size | crc16 (see SendFacePacket() in the cam's
// cam_main.cpp). Fixed length, so there is nothing to parse - eight bytes read straight into place.
constexpr uint8_t kFaceMagic1 = 0xF1;
constexpr size_t kFacePacketRest = 9;  // seq dx dy roll conf size age crc16

// Face tracking (ApplyFaceTracking). Degrees of head turn per unit of frame offset: the OV2640's
// lens sees ~65 deg across and ~50 deg down, and dx/dy span -100..100 of that.
constexpr float kFaceYawDegPerUnit = 0.33f;
constexpr float kFacePitchDegPerUnit = 0.25f;
// Fraction of the measured correction aimed for per report.
//
// This used to be ~0.5, because a report describes a picture one detection old and the head had
// usually moved some of the way since: aiming for the whole offset from where the head is NOW
// double-counts that movement, overshoots, then hunts. So it crept up on the face over 2-3 reports -
// the slow settle the user noticed. Now each report carries the picture's age and the aim is taken
// from where the head was pointing WHEN THE PICTURE WAS TAKEN (latency compensation, see
// AnglesAt()): target = angle_then + offset. That target is right regardless of how far the head has
// moved since, so nearly all of the offset can be corrected at once. Not quite 1.0: the degrees-
// per-unit figures above are the lens' nominal field of view, not a calibration.
//
// Pitch gets a narrower deadband: a vertical unit is fewer degrees than a horizontal one, and the
// picture is only 3/4 as tall as it is wide, so a face drifts off the top or bottom much sooner.
constexpr float kFaceYawGain = 0.85f;
constexpr float kFacePitchGain = 0.9f;
constexpr int kFaceYawDeadband = kDeadband;  // = the cam's kCentredBand
constexpr int kFacePitchDeadband = 7;
// Glide speed as a fraction of the axis' tuned speed; the servo controller scales its acceleration
// with it too, so this is how quickly the head gets going as well as its top speed.
constexpr float kFaceYawGlideScale = 1.5f;
constexpr float kFacePitchGlideScale = 1.8f;
// Reports older than this are not compensated beyond it: something stalled, and the angle history
// does not reach further back anyway.
constexpr uint32_t kFaceMaxAgeMs = 600;

// Where the head waits while tracking is on but no face is in view: 20 deg heads-up (pitch angle
// DOWN = look up). This is a desktop robot, and the user's face is almost always above it, so
// looking straight ahead would leave most faces at or past the top of the picture. Used when
// tracking is switched on and when a lost face sends the head back.
constexpr float kTrackHomePitch = ServoController::kDefaultAngle - 20.0f;

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
  Serial2.setRxBufferSize(2048);
  Serial2.begin(kBaud, SERIAL_8N1, kPinRx, kPinTx);
  uart_ll_set_sclk(UART_LL_GET_HW(2), SOC_MOD_CLK_APB);
  uart_ll_set_baudrate(UART_LL_GET_HW(2), kBaud, 80000000);
  // Bounds readBytes() inside the video path. The Stream default is 1000ms,
  // which on a truncated frame would freeze the head, the screen and the button
  // for a full second.
  Serial2.setTimeout(kFrameReadTimeoutMs);
  initialised_ = true;
  line_len_ = 0;
  result_[0] = '\0';
  LoadTrackDirs();
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
  // An explicit decision (voice, web page, a look finishing). The 'G' handler marks a gesture arm
  // again straight after calling this.
  armed_by_gesture_ = false;
  // The tracker yields to animations, so a random idle animation still playing would hold it off.
  // Tracking wins: end it now (the head stays where it is and tracking takes over from there).
  if (enabled) {
    auto& servos = ServoController::GetInstance();
    servos.StopAnimation();
    // Heads up first (see kTrackHomePitch), so the user's face is in the picture to begin with.
    // The first face report simply re-aims this glide from wherever the head has got to.
    servos.GlideTo(ServoController::kPinServo0, kTrackHomePitch, kRecentreGlideScale);
    servos.GlideTo(ServoController::kPinServo1, ServoController::kDefaultAngle, kRecentreGlideScale);
    roll_target_ = ServoController::kDefaultAngle;
    // Start the lost-finger clocks now, so switching tracking on with no finger in view waits the
    // full grace period before SuperviseTracking() recentres or disarms, rather than firing at once.
    last_finger_ms_ = millis();
    recentred_ = false;
  }
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

  // If video was just turned off ("F 0"), the cam board needs ~350ms to finish
  // its in-flight JPEG write and run InitTrackingCamera(). Wait out any remaining
  // portion of that window and drain residual bytes before sending K and V.
  if (video_stopped_ms_ != 0) {
    const int32_t elapsed = static_cast<int32_t>(millis() - video_stopped_ms_);
    if (elapsed >= 0 && elapsed < 380) {
      const uint32_t until = millis() + static_cast<uint32_t>(380 - elapsed);
      while (static_cast<int32_t>(millis() - until) < 0) {
        while (Serial2.available() > 0) {
          Serial2.read();
        }
        delay(4);
      }
    }
    video_stopped_ms_ = 0;
    line_len_ = 0;
    line_corrupt_ = false;
  }

  // Recognition mode. Stop the head BEFORE asking for the photo.
  if (tracking_enabled_) {
    look_resume_tracking_ = true;
    look_resume_gesture_ = armed_by_gesture_;
    SetTrackingEnabled(false);
  }
  NoteManualHeadCommand();

  // Always refresh the vision endpoint before a look in case the cam missed K
  // while streaming video or re-initialising its sensor.
  SendVisionEndpoint();

  look_id_ = mcp_id;
  look_pending_ = true;
  look_started_ms_ = millis();
  look_last_tx_ms_ = look_started_ms_;
  look_retries_ = 0;

  if (question == nullptr || *question == '\0') {
    snprintf(look_cmd_, sizeof(look_cmd_), "V");
    SendCommand(look_cmd_);
    return true;
  }

  // One line per message, so a newline inside the question would split it in
  // two and the cam would act on half a sentence.
  int n = snprintf(look_cmd_, sizeof(look_cmd_), "V %s", question);
  if (n < 0) {
    snprintf(look_cmd_, sizeof(look_cmd_), "V");
    SendCommand(look_cmd_);
    return true;
  }
  if (static_cast<size_t>(n) >= sizeof(look_cmd_)) {
    n = sizeof(look_cmd_) - 1;
  }
  for (int i = 0; i < n; ++i) {
    if (look_cmd_[i] == '\n' || look_cmd_[i] == '\r') {
      look_cmd_[i] = ' ';
    }
  }
  SendCommand(look_cmd_);
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

void CamLink::ApplyTracking(int dx, int dy, int conf, int lean) {
  if (!tracking_enabled_ || conf < kMinConf) {
    return;
  }
  const uint32_t now = millis();
  // The finger is in view, so SuperviseTracking()'s lost-finger clocks start over. Stamped before
  // the animation and manual-hold checks: the finger being there is a fact whether or not we act on
  // it this time.
  last_finger_ms_ = now;
  recentred_ = false;
  // A gesture animation owns the servos while it runs; fighting it would make
  // the bobble look broken and the head end up somewhere unintended.
  if (ServoController::GetInstance().IsAnimating()) {
    return;
  }
  // Any usable update counts as "the cam is still talking", whether or not it
  // ends up moving anything. CoastTracking() keys off this to decide the cam
  // has gone quiet and the ramp-down is now its responsibility.
  last_track_msg_ms_ = now;
  if (static_cast<int32_t>(manual_until_ms_ - now) > 0) {
    return;
  }
  // Roll first, and outside the move-interval gate below: it is a glide that runs on the servo
  // controller's own clock, so re-aiming it on every report costs nothing and keeps it current.
  MirrorLean(lean);
  if (now - last_move_ms_ < kMoveIntervalMs) {
    return;
  }

  // Forget the previous speed if the subject has been missing for a moment, so
  // the head eases in from rest rather than resuming mid-slew.
  if (now - last_move_ms_ > kVelocityResetMs) {
    yaw_speed_ = 0.0f;
    pitch_speed_ = 0.0f;
    yaw_learn_.active = false;
    pitch_learn_.active = false;
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
  auto step = [](int offset, int gain_numer) -> float {
    int past = 0;
    if (offset >= kDeadband) {
      past = offset - kDeadband;
    } else if (offset <= -kDeadband) {
      past = offset + kDeadband;
    } else {
      return 0.0f;
    }
    float d = static_cast<float>(past * gain_numer) / static_cast<float>(kGainDenom);
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

  // Speeds are kept in servo-angle space; the learned direction maps frame offset onto it.
  yaw_speed_ = accelerate(yaw_speed_, yaw_dir_ * step(dx, kYawGainNumer));
  pitch_speed_ = accelerate(pitch_speed_, pitch_dir_ * step(dy, kPitchGainNumer));

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
  const bool learn = conf >= kLearnMinConf;

  auto& servos = ServoController::GetInstance();

  if (yaw_step == 0.0f && pitch_step == 0.0f) {
    if (learn) {
      LearnAxis(true, dx, 0.0f, false, now);
      LearnAxis(false, dy, 0.0f, false, now);
    }
    return;
  }
  last_move_ms_ = now;

  // Direction: the learned yaw_dir_ / pitch_dir_ (already folded into the speeds) decide which way
  // an offset turns the head. Defaults: subject right of centre -> yaw angle DOWN (bigger yaw is
  // left); subject below centre -> pitch angle up (look down). If a default is wrong for this build
  // (e.g. the cam image is mirrored), LearnAxis notices and flips it.
  //
  // Every write is clamped to that servo's own kServoNMin/MaxAngle here, on top
  // of the clamp inside SetAngle(). Belt and braces is warranted: this is the
  // one code path that moves the servos continuously and unattended, and a
  // tracker that walks the head into an end stop and holds it there stalls an
  // MG92B at ~700mA until something gives.
  //
  // SetAngle(), not MoveAngleSmooth(): the smooth variant sleeps ~18ms per
  // degree, and loop() also drives the display, the event pump and the timer.
  // Returns the change actually applied (less than `step` when clamped at an end stop).
  auto move = [&servos](gpio_num_t pin, float step, float lo, float hi) -> float {
    if (step == 0.0f) {
      return 0.0f;
    }
    const float before = servos.GetAngle(pin);
    float target = before + step;
    if (target < lo) {
      target = lo;
    } else if (target > hi) {
      target = hi;
    }
    servos.SetAngle(pin, target);
    return target - before;
  };

  const float yaw_applied =
      move(ServoController::kPinServo2, yaw_step, ServoController::kServo2MinAngle, ServoController::kServo2MaxAngle);
  const float pitch_applied =
      move(ServoController::kPinServo0, pitch_step, ServoController::kServo0MinAngle, ServoController::kServo0MaxAngle);

  // Live-debug trace, at most twice a second so it does not flood the UART.
  static uint32_t last_trace_ms = 0;
  if (now - last_trace_ms >= 500) {
    last_trace_ms = now;
    printf("[track] dx %d dy %d conf %d -> yaw %.1f (%+.1f) pitch %.1f (%+.1f)\n", dx, dy, conf,
           servos.GetAngle(ServoController::kPinServo2), yaw_applied, servos.GetAngle(ServoController::kPinServo0),
           pitch_applied);
  }

  if (learn) {
    LearnAxis(true, dx, yaw_applied, yaw_step != 0.0f && fabsf(yaw_applied) < 0.01f, now);
    LearnAxis(false, dy, pitch_applied, pitch_step != 0.0f && fabsf(pitch_applied) < 0.01f, now);
  }
}

// Roll mirrors the finger's lean (see kLeanDeadbandDeg for why this is open loop). A glide rather
// than a SetAngle: the servo controller eases it on its own clock with bounded acceleration, so it
// keeps moving smoothly between reports - including when the cam goes quiet because the finger is
// centred and holding still - and re-aiming it mid-glide keeps the current velocity.
void CamLink::MirrorLean(int lean) {
  float tilt = 0.0f;
  if (lean > kLeanDeadbandDeg) {
    tilt = static_cast<float>(lean - kLeanDeadbandDeg) * kLeanGain;
  } else if (lean < -kLeanDeadbandDeg) {
    tilt = static_cast<float>(lean + kLeanDeadbandDeg) * kLeanGain;
  }
  if (tilt > kMaxLeanTiltDeg) {
    tilt = kMaxLeanTiltDeg;
  } else if (tilt < -kMaxLeanTiltDeg) {
    tilt = -kMaxLeanTiltDeg;
  }
  // roll_dir_ +1: finger tip leaning to the right of the picture -> roll angle up (向右歪头).
  const float target = ServoController::kDefaultAngle + static_cast<float>(roll_dir_) * tilt;

  auto& servos = ServoController::GetInstance();
  const float current = servos.GetAngle(ServoController::kPinServo1);
  // Checked against where the head actually is as well as where it was last sent: if something
  // else moved the roll servo meanwhile (a voice "歪头", the web slider), a stale roll_target_ must
  // not stop it being brought back.
  if (fabsf(target - roll_target_) < kLeanRetargetDeg && fabsf(target - current) < kLeanRetargetDeg) {
    return;
  }
  roll_target_ = target;
  servos.GlideTo(ServoController::kPinServo1, target, kLeanGlideScale);
}

// Runs every Poll(). The cam only talks while it can see a finger, so "the finger has gone" is an
// absence that has to be noticed on our own clock.
void CamLink::SuperviseTracking(uint32_t now) {
  if (!tracking_enabled_ || look_pending_) {
    return;
  }
  const uint32_t since = now - last_finger_ms_;

  if (armed_by_gesture_ && since >= kGestureIdleOffMs) {
    printf("[track] armed by gesture, nothing seen for %us -> tracking off\n",
           static_cast<unsigned>(kGestureIdleOffMs / 1000));
    SetTrackingEnabled(false);
    return;
  }

  if (recentred_ || since < kFingerLostRecentreMs) {
    return;
  }
  auto& servos = ServoController::GetInstance();
  // An explicit "向左转头" is still being honoured, or a gesture animation has the head: leave it,
  // and look again next time round.
  if (static_cast<int32_t>(manual_until_ms_ - now) > 0 || servos.IsAnimating()) {
    return;
  }
  recentred_ = true;
  yaw_speed_ = 0.0f;
  pitch_speed_ = 0.0f;
  yaw_learn_.active = false;
  pitch_learn_.active = false;
  roll_target_ = ServoController::kDefaultAngle;
  servos.GlideTo(ServoController::kPinServo2, ServoController::kServo2DefaultAngle, kRecentreGlideScale);
  servos.GlideTo(ServoController::kPinServo0, kTrackHomePitch, kRecentreGlideScale);
  servos.GlideTo(ServoController::kPinServo1, ServoController::kDefaultAngle, kRecentreGlideScale);
  printf("[track] face gone for %us -> back to centre\n", static_cast<unsigned>(kFingerLostRecentreMs / 1000));
}

void CamLink::LearnAxis(bool yaw, int offset, float applied_deg, bool at_limit, uint32_t now) {
  if (!kAutoLearnDirections) {
    return;
  }
  AxisLearn& L = yaw ? yaw_learn_ : pitch_learn_;
  const char* axis = yaw ? "yaw" : "pitch";
  const int mag = abs(offset);
  if (mag < kDeadband) {
    // Centred: nothing to learn, and whatever window was open is over.
    L.active = false;
    L.stuck_since_ms = 0;
    return;
  }

  bool wrong = false;

  // Pinned on an end stop while the subject stays well off to that side.
  if (at_limit && mag >= kLearnStuckOffset && L.confirmed < kLearnConfirmedEnough) {
    if (L.stuck_since_ms == 0) {
      L.stuck_since_ms = now;
    } else if (now - L.stuck_since_ms >= kLearnStuckMs) {
      L.stuck_since_ms = 0;
      printf("[track] %s pinned at end stop, subject still off-centre (%d)\n", axis, offset);
      wrong = true;
    }
  } else {
    L.stuck_since_ms = 0;
  }

  if (!wrong && applied_deg != 0.0f) {
    if (!L.active) {
      L.active = true;
      L.ref_offset = offset;
      L.moved_deg = 0.0f;
    }
    L.moved_deg += fabsf(applied_deg);
    if (L.moved_deg >= kLearnWindowDeg) {
      const int ref_mag = abs(L.ref_offset);
      const bool same_side = (offset > 0) == (L.ref_offset > 0);
      if (same_side && mag >= ref_mag + kLearnMargin) {
        printf("[track] %s moved %.1f deg but offset grew %d -> %d\n", axis, L.moved_deg, L.ref_offset, offset);
        wrong = true;
      } else if (!same_side || mag <= ref_mag - kLearnMargin) {
        L.wrong_votes = 0;
        if (L.confirmed < 255) ++L.confirmed;
      }
      L.active = false;
    }
  }

  if (!wrong) {
    return;
  }
  if (++L.wrong_votes < kLearnVotes) {
    return;
  }
  // Two strikes: this axis turns the head away from the subject. Flip it and start over.
  int8_t& dir = yaw ? yaw_dir_ : pitch_dir_;
  dir = static_cast<int8_t>(-dir);
  L = AxisLearn{};
  (yaw ? yaw_speed_ : pitch_speed_) = 0.0f;
  printf("[track] %s direction was wrong -> flipped to %d (saved)\n", axis, dir);
  SaveTrackDirs();
}

// Keys renamed from ydir/pdir when auto-learning was turned off (kAutoLearnDirections): the values
// it had saved were flipped by a moving hand, not by a wrong servo, and must not be loaded again.
void CamLink::LoadTrackDirs() {
  Preferences prefs;
  if (prefs.begin("track", true)) {
    const int8_t y = prefs.getChar(kYawDirKey, yaw_dir_);
    const int8_t p = prefs.getChar("pdir_m", pitch_dir_);
    const int8_t r = prefs.getChar(kRollDirKey, roll_dir_);
    prefs.end();
    yaw_dir_ = y < 0 ? -1 : 1;
    pitch_dir_ = p < 0 ? -1 : 1;
    roll_dir_ = r < 0 ? -1 : 1;
  }
  printf("[track] directions: yaw %d, pitch %d, roll %d\n", yaw_dir_, pitch_dir_, roll_dir_);
}

void CamLink::SaveTrackDirs() {
  Preferences prefs;
  if (prefs.begin("track", false)) {
    prefs.putChar(kYawDirKey, yaw_dir_);
    prefs.putChar("pdir_m", pitch_dir_);
    prefs.putChar(kRollDirKey, roll_dir_);
    prefs.end();
  }
}

void CamLink::FlipTrackDir(char axis) {
  const char* name = nullptr;
  if (axis == 'y' || axis == 'p') {
    const bool yaw = (axis == 'y');
    int8_t& dir = yaw ? yaw_dir_ : pitch_dir_;
    dir = static_cast<int8_t>(-dir);
    (yaw ? yaw_learn_ : pitch_learn_) = AxisLearn{};
    yaw_speed_ = 0.0f;
    pitch_speed_ = 0.0f;
    name = yaw ? "yaw" : "pitch";
  } else if (axis == 'r') {
    // Nothing is learned for roll, so there is nothing to reset: the next finger report re-aims it
    // on the other side.
    roll_dir_ = static_cast<int8_t>(-roll_dir_);
    name = "roll";
  } else {
    return;
  }
  printf("[track] %s direction flipped manually -> yaw %d, pitch %d, roll %d\n", name, yaw_dir_, pitch_dir_,
         roll_dir_);
  SaveTrackDirs();
}

void CamLink::ResetTrackDirs() {
  yaw_dir_ = kDefaultYawDir;
  pitch_dir_ = 1;
  roll_dir_ = kDefaultRollDir;
  yaw_learn_ = AxisLearn{};
  pitch_learn_ = AxisLearn{};
  yaw_speed_ = 0.0f;
  pitch_speed_ = 0.0f;
  printf("[track] directions reset to defaults\n");
  SaveTrackDirs();
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
bool CamLink::SetLinkFast(bool fast, uint32_t ack_timeout_ms, bool force) {
  if (!initialised_) {
    return false;
  }
  if (!force && fast == link_fast_) {
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
  const uint32_t target_baud = fast ? kFastBaud : kBaud;
  Serial2.updateBaudRate(target_baud);
  // Arduino 3.2.0 switches UARTs to the 1 MHz REF_TICK clock whenever baud <=
  // 250000 (REF_TICK_BAUDRATE_LIMIT). At ~230kbaud a 1 MHz receiver only has
  // 4.34 clock ticks per bit (div_int=4), so its 1us start-bit quantization
  // plus the cam's 1us fractional bit step shifts the sample point by up to 2us
  // (46% of a bit cell) and corrupts every JPEG header with JDR_FMT1. Keep
  // UART2 on the 80 MHz APB clock (12.5ns resolution, div_int=345, div_frag=0
  // at 231884 Hz) so every bit is sampled at its exact centre.
  uart_ll_set_sclk(UART_LL_GET_HW(2), SOC_MOD_CLK_APB);
  uart_ll_set_baudrate(UART_LL_GET_HW(2), target_baud, 80000000);
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
  if (!initialised_ || !video_sink::IsOpen()) {
    return false;
  }
  if (video_active_) {
    return true;
  }
  if (link_fast_) {
    SendCommand("B 0");
    Serial2.flush();
    delay(5);
    Serial2.updateBaudRate(kBaud);
    uart_ll_set_sclk(UART_LL_GET_HW(2), SOC_MOD_CLK_APB);
    uart_ll_set_baudrate(UART_LL_GET_HW(2), kBaud, 80000000);
    link_fast_ = false;
  }
  line_len_ = 0;

  video_sink::ResetErrors();
  video_frames_ = 0;
  video_bad_ = 0;
  video_retry_ = 0;
  video_decode_ms_ = 0;
  video_timed_ = 0;
  video_stat_ms_ = millis();
  last_frame_ms_ = millis();
  SendCommand("F 1");
  video_active_ = true;
  return true;
}

bool CamLink::FallbackVideoToSlow() {
  return BeginVideo();
}

void CamLink::EndVideo() {
  const bool was_active = video_active_;
  video_active_ = false;
  if (!initialised_) {
    return;
  }

  if (was_active) {
    printf("cam link: video off after %u frames, %u bad\n", static_cast<unsigned>(video_frames_), static_cast<unsigned>(video_bad_));
    SendCommand("F 0");
    Serial2.flush();
    // Non-blocking drain of bytes already in the FIFO. Do NOT block loopTask for
    // 300ms here: when the user says "关闭摄像头", TTS starts simultaneously and
    // blocking loopTask delays freeing the camera UI heap and starves the event pump.
    // Any in-flight 0xA5 0x5A JPEG frames are consumed cleanly by Poll() below.
    while (Serial2.available() > 0) {
      Serial2.read();
    }
    line_len_ = 0;
    line_corrupt_ = false;
    video_stopped_ms_ = millis();
  }

  if (link_fast_) {
    SetLinkFast(false, 500);
  }

  Serial2.updateBaudRate(kBaud);
  uart_ll_set_sclk(UART_LL_GET_HW(2), SOC_MOD_CLK_APB);
  uart_ll_set_baudrate(UART_LL_GET_HW(2), kBaud, 80000000);
  link_fast_ = false;
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
    for (size_t i = 0; i < n && self->frame_head_len_ < sizeof(self->frame_head_); ++i) {
      self->frame_head_[self->frame_head_len_++] = p[i];
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
  frame_head_len_ = 0;

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
  if (!video_active_) {
    return;
  }

  if (ok && frame_crc_ == want_crc) {
    ++video_frames_;
    // After the picture, not before: it is drawn onto the frame, and the frame
    // would paint over it.
    video_sink::DrawReticle();
  } else {
    ++video_bad_;
    if (video_bad_ <= 3) {
      printf("video: bad #%u len=%u %ux%u crc=%04x(want %04x) left=%u head=%02x %02x %02x %02x %02x %02x\n",
             static_cast<unsigned>(video_bad_), static_cast<unsigned>(len), static_cast<unsigned>(w),
             static_cast<unsigned>(h), static_cast<unsigned>(frame_crc_), static_cast<unsigned>(want_crc),
             static_cast<unsigned>(frame_left_), frame_head_[0], frame_head_[1], frame_head_[2],
             frame_head_[3], frame_head_[4], frame_head_[5]);
    }
  }

  // The one measurement that says whether this link rate is sustainable.
  //
  // `wire` is how long `len` bytes take to arrive at active_baud(); `decode` is
  // how long we took to consume them.
  video_decode_ms_ += decode_ms;
  ++video_timed_;
  if (millis() - video_stat_ms_ >= 2000) {
    video_stat_ms_ = millis();
    const uint32_t avg = video_timed_ ? (video_decode_ms_ / video_timed_) : 0;
    const uint32_t wire_ms = (len * 10UL * 1000UL) / active_baud();
    printf("video: %u ok, %u bad, %uB/frame, decode %ums vs wire %ums (@%u)\n", static_cast<unsigned>(video_frames_),
           static_cast<unsigned>(video_bad_), static_cast<unsigned>(len), static_cast<unsigned>(avg),
           static_cast<unsigned>(wire_ms), static_cast<unsigned>(active_baud()));
    video_decode_ms_ = 0;
    video_timed_ = 0;
  }
}

void CamLink::NoteCamAlive() {
  last_rx_ms_ = millis();
  if (!present_) {
    present_ = true;
    printf("cam link: camera present\n");
    // Whatever it thinks its state is, make it match ours.
    SendCommand(tracking_enabled_ ? "A 1" : "A 0");
    tracking_armed_ = tracking_enabled_;
    SendVisionEndpoint();
  }
}

void CamLink::NoteTrackingReport() {
  // The cam only reports while it believes tracking is armed. If we are not, the two boards
  // disagree - seen live after the main board rebooted mid-gesture: the cam had armed itself
  // and sent "G 1" into a board that was still booting. Recording what the cam evidently thinks
  // lets Poll()'s resync tell it "A 0".
  if (!tracking_enabled_) {
    tracking_armed_ = true;
  }
  // RunVision() on the cam board is synchronous and blocks loop(), so a tracking report arriving
  // >700ms after we sent 'V' proves the cam is idle in loop() and missed our 'V' command (e.g.
  // while re-initialising its sensor after F 0).
  if (look_pending_ && look_cmd_[0] != '\0' && look_retries_ < 2 && (millis() - look_last_tx_ms_ > 700)) {
    ++look_retries_;
    look_last_tx_ms_ = millis();
    printf("cam link: cam idle while look pending -> retry #%u: %s\n", static_cast<unsigned>(look_retries_), look_cmd_);
    SendVisionEndpoint();
    SendCommand(look_cmd_);
  }
}

void CamLink::ReceiveFacePacket() {
  uint8_t p[kFacePacketRest];
  for (size_t i = 0; i < kFacePacketRest; ++i) {
    const int b = ReadByteTimed(5);  // the whole packet is 0.9ms on the wire at 115200
    if (b < 0) {
      ++face_bad_;
      return;
    }
    p[i] = static_cast<uint8_t>(b);
  }
  const uint16_t crc = Crc16Update(0xFFFF, p, 7);
  if ((static_cast<uint16_t>(p[7]) | (static_cast<uint16_t>(p[8]) << 8)) != crc) {
    ++face_bad_;
    return;
  }
  NoteCamAlive();
  NoteTrackingReport();

  const uint32_t now = millis();
  if (face_last_ms_ != 0) {
    const uint32_t gap = now - face_last_ms_;
    if (gap < 2000) {
      face_period_ms_ = (face_period_ms_ == 0) ? gap : (face_period_ms_ * 3 + gap) / 4;
    }
  }
  face_last_ms_ = now;
  face_seq_ = p[0];
  ++face_ok_;
  // Age: the cam's own figure (picture taken -> packet sent, 4ms units) plus the ~1ms the packet
  // spent on the wire.
  ApplyFaceTracking(static_cast<int8_t>(p[1]), static_cast<int8_t>(p[2]), p[4], static_cast<int8_t>(p[3]),
                    static_cast<uint32_t>(p[6]) * 4 + 1);
}

void CamLink::RecordAngles(uint32_t now) {
  if (angle_hist_n_ != 0 && now - angle_hist_ms_[(angle_hist_head_ + kAngleHistLen - 1) % kAngleHistLen] < kAngleHistStepMs) {
    return;
  }
  auto& servos = ServoController::GetInstance();
  angle_hist_ms_[angle_hist_head_] = now;
  angle_hist_yaw_[angle_hist_head_] = servos.GetAngle(ServoController::kPinServo2);
  angle_hist_pitch_[angle_hist_head_] = servos.GetAngle(ServoController::kPinServo0);
  angle_hist_head_ = (angle_hist_head_ + 1) % kAngleHistLen;
  if (angle_hist_n_ < kAngleHistLen) {
    ++angle_hist_n_;
  }
}

void CamLink::AnglesAt(uint32_t t, float* yaw, float* pitch) const {
  auto& servos = ServoController::GetInstance();
  *yaw = servos.GetAngle(ServoController::kPinServo2);
  *pitch = servos.GetAngle(ServoController::kPinServo0);
  // Newest to oldest: the first sample at or before t, interpolated with the one after it.
  float next_yaw = *yaw;
  float next_pitch = *pitch;
  uint32_t next_ms = millis();
  for (uint8_t i = 0; i < angle_hist_n_; ++i) {
    const uint8_t idx = (angle_hist_head_ + kAngleHistLen - 1 - i) % kAngleHistLen;
    const uint32_t ms = angle_hist_ms_[idx];
    if (static_cast<int32_t>(ms - t) <= 0) {
      const uint32_t span = next_ms - ms;
      const float f = span > 0 ? static_cast<float>(t - ms) / static_cast<float>(span) : 0.0f;
      *yaw = angle_hist_yaw_[idx] + (next_yaw - angle_hist_yaw_[idx]) * f;
      *pitch = angle_hist_pitch_[idx] + (next_pitch - angle_hist_pitch_[idx]) * f;
      return;
    }
    next_yaw = angle_hist_yaw_[idx];
    next_pitch = angle_hist_pitch_[idx];
    next_ms = ms;
    // Older than the whole history: the oldest sample is the best there is.
    *yaw = next_yaw;
    *pitch = next_pitch;
  }
}

void CamLink::ApplyFaceTracking(int dx, int dy, int conf, int roll, uint32_t age_ms) {
  if (!tracking_enabled_ || conf < kMinConf) {
    return;
  }
  const uint32_t now = millis();
  // The face is in view: SuperviseTracking()'s lost-subject clocks start over.
  last_finger_ms_ = now;
  recentred_ = false;
  auto& servos = ServoController::GetInstance();
  if (servos.IsAnimating()) {
    return;
  }
  last_track_msg_ms_ = now;
  if (static_cast<int32_t>(manual_until_ms_ - now) > 0) {
    return;
  }
  // Mirror: the head tilts the way the face's image tilts, i.e. the user tilting to their left gets
  // the robot tilting to ITS right. roll_dir_ (web page flip button) reverses it.
  MirrorLean(roll);

  // The step controller is not used for faces; keep its state at rest so CoastTracking() stays out.
  yaw_speed_ = 0.0f;
  pitch_speed_ = 0.0f;

  // Soft deadband, as in ApplyTracking(): measured from the edge of the band, so the aim passes
  // through zero continuously instead of jumping when the face crosses it.
  auto past = [](int offset, int band) -> float {
    if (offset >= band) {
      return static_cast<float>(offset - band);
    }
    if (offset <= -band) {
      return static_cast<float>(offset + band);
    }
    return 0.0f;
  };
  // Aim from where the head was pointing when the picture was taken (see kFaceYawGain). Clamped to
  // the axis' own limits here as well as inside the servo controller - this is the one path that
  // moves the head unattended.
  float yaw_then = 0.0f;
  float pitch_then = 0.0f;
  AnglesAt(now - (age_ms > kFaceMaxAgeMs ? kFaceMaxAgeMs : age_ms), &yaw_then, &pitch_then);
  auto aim = [&servos](int pin, float then, float correction, float lo, float hi, float glide) -> float {
    if (correction == 0.0f) {
      // Inside the deadband: let any glide already under way finish.
      return servos.GetAngle(pin);
    }
    float target = then + correction;
    if (target < lo) {
      target = lo;
    } else if (target > hi) {
      target = hi;
    }
    servos.GlideTo(pin, target, glide);
    return target;
  };
  const float yaw_to = aim(ServoController::kPinServo2, yaw_then,
                           yaw_dir_ * past(dx, kFaceYawDeadband) * kFaceYawDegPerUnit * kFaceYawGain,
                           ServoController::kServo2MinAngle, ServoController::kServo2MaxAngle, kFaceYawGlideScale);
  const float pitch_to = aim(ServoController::kPinServo0, pitch_then,
                             pitch_dir_ * past(dy, kFacePitchDeadband) * kFacePitchDegPerUnit * kFacePitchGain,
                             ServoController::kServo0MinAngle, ServoController::kServo0MaxAngle, kFacePitchGlideScale);

  static uint32_t last_trace_ms = 0;
  if (now - last_trace_ms >= 500) {
    last_trace_ms = now;
    printf("[track] face dx %d dy %d roll %d conf %d age %ums -> yaw %.1f pitch %.1f (every %ums, %u ok %u bad)\n",
           dx, dy, roll, conf, static_cast<unsigned>(age_ms), yaw_to, pitch_to, static_cast<unsigned>(face_period_ms_),
           static_cast<unsigned>(face_ok_), static_cast<unsigned>(face_bad_));
  }
}

void CamLink::HandleLine(const char* line) {
  NoteCamAlive();

  switch (line[0]) {
    case 'T': {
      // "T dx dy conf lean kind". kind 1 is a finger - the only thing the head follows now.
      int dx = 0;
      int dy = 0;
      int conf = 0;
      int lean = 0;
      int kind = 0;
      const int n = sscanf(line + 1, "%d %d %d %d %d", &dx, &dy, &conf, &lean, &kind);
      if (n >= 5) {
        NoteTrackingReport();
        if (kind == 1) {
          ApplyTracking(dx, dy, conf, lean);
        }
      } else if (n >= 3 && !legacy_warned_) {
        // Four fields is the old face tracker, whose output the head no longer follows.
        legacy_warned_ = true;
        printf("cam link: camera runs the old text face-tracking firmware - flash enco02_cam\n");
      }
      break;
    }
    case 'L':
    case 'E': {
      if (line[1] != ' ') {
        break;  // require protocol space delimiter to reject stray noise
      }
      if (!look_pending_) {
        break;  // late arrival after a timeout, or an unsolicited boot error
      }
      look_pending_ = false;
      look_cmd_[0] = '\0';
      look_retries_ = 0;
      result_ready_ = true;
      result_ok_ = (line[0] == 'L');
      result_id_ = look_id_;
      const char* body = line + 2;
      strncpy(result_, body, sizeof(result_) - 1);
      result_[sizeof(result_) - 1] = '\0';
      printf("cam link: look result (%c): %.80s\n", line[0], result_);
      // The photo has been taken and answered; put the head back to work if the
      // user had tracking on before they asked.
      if (look_resume_tracking_) {
        look_resume_tracking_ = false;
        SetTrackingEnabled(true);
        armed_by_gesture_ = look_resume_gesture_;
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
        // After the call, which clears it: a gesture arm switches itself off again once the finger
        // has been gone for kGestureIdleOffMs.
        armed_by_gesture_ = true;
      }
      break;
    }
    case 'R': {
      // The cam rebooted, so its tracking state is back to the default - and
      // so is its vision endpoint, which lives in RAM over there.
      tracking_armed_ = false;
      SendVisionEndpoint();
      if (look_pending_ && look_cmd_[0] != '\0' && look_retries_ < 2) {
        ++look_retries_;
        look_last_tx_ms_ = millis();
        printf("cam link: cam rebooted while look pending -> retry #%u\n",
               static_cast<unsigned>(look_retries_));
        SendCommand(look_cmd_);
      }
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
  // Where the head points, every ~20ms, so a face report can be aimed from where the head was when
  // its picture was taken. Only worth the cycles while tracking.
  if (tracking_enabled_) {
    RecordAngles(millis());
  }

  while (Serial2.available() > 0) {
    const int c = Serial2.read();
    if (c < 0) {
      break;
    }
    // Video frames are binary and a JPEG is full of 0x0A, so they cannot travel
    // as lines. 0xA5 never occurs in the ASCII protocol, which lets the parser
    // stay exactly as it was and simply step aside when it sees the magic.
    // Always intercept 0xA5 0x5A even right after video_active_ turns false so
    // an in-flight JPEG frame after "F 0" never spills raw binary into line_[].
    if (c == kFrameMagic0) {
      const int c2 = ReadByteTimed(5);
      if (c2 == kFrameMagic1) {
        line_len_ = 0;  // a frame cannot arrive mid-line; if it did, that line was junk
        line_corrupt_ = false;
        ReceiveVideoFrame();
      } else if (c2 == kFaceMagic1) {
        // The cam only sends these between lines, never inside one.
        line_len_ = 0;
        line_corrupt_ = false;
        ReceiveFacePacket();
      }
      // A lone 0xA5 is noise. Both bytes are dropped: they are not ASCII, so
      // they could only corrupt a line anyway.
      continue;
    }
    if (c == '\n' || c == '\r') {
      if (line_len_ > 0 && !line_corrupt_) {
        line_[line_len_] = '\0';
        HandleLine(line_);
      }
      line_len_ = 0;
      line_corrupt_ = false;
      continue;
    }
    const uint8_t uc = static_cast<uint8_t>(c);
    if ((uc < 0x20 && uc != '\t') || uc >= 0xFE) {
      line_corrupt_ = true;
    }
    if (line_len_ < sizeof(line_) - 1) {
      line_[line_len_++] = static_cast<char>(c);
    } else {
      // A line this long is a framing error, not a message. Drop it whole
      // rather than act on a fragment.
      line_len_ = 0;
      line_corrupt_ = true;
    }
  }

  const uint32_t now = millis();

  if (present_ && !look_pending_ && now - last_rx_ms_ > kPresenceTimeoutMs) {
    present_ = false;
    tracking_armed_ = false;
    printf("cam link: camera lost\n");
  }

  // If a vision request is pending and the link has been silent for >900ms,
  // send a lightweight 'P' ping. If the cam is idle in loop() (because it
  // missed 'V' during sensor re-init), it will immediately reply "T 0 0 0 0",
  // triggering HandleLine('T') to re-send look_cmd_.
  if (look_pending_ && look_retries_ < 2 && (now - look_last_tx_ms_ > 900) &&
      (now - last_ping_ms_ > 900)) {
    last_ping_ms_ = now;
    SendCommand("P");
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
    look_cmd_[0] = '\0';
    look_retries_ = 0;
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
      armed_by_gesture_ = look_resume_gesture_;
    }
  }

  // If the cam briefly paused (e.g. sensor re-init or web snapshot), re-send
  // `F 1` rather than killing `video_active_` so the camera view stays open
  // until the user explicitly closes it.
  if (video_active_ && now - last_frame_ms_ > 1500) {
    last_frame_ms_ = now;
    SendCommand("F 1");
  }

  // Finger gone: recentre after a few seconds, and disarm a gesture-armed session after longer.
  SuperviseTracking(now);
}
