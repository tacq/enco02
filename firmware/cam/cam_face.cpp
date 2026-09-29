#include "cam_face.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <string.h>

#include <list>
#include <vector>

#include "human_face_detect_mnp01.hpp"
#include "human_face_detect_msr01.hpp"

namespace cam_face {
namespace {

// Stage 1 (MSR01) proposes candidate boxes on a scaled-down copy of the frame; stage 2 (MNP01)
// re-scores each candidate at full resolution and adds the five landmarks. Thresholds start from
// the ones Espressif ship in the CameraWebServer example for this exact pair.
//
// kResizeScale sets the smallest face stage 1 can see. Espressif's 0.2 searches a QVGA frame at
// 64x48, and in use that missed faces sitting towards a corner of the picture - there a face is
// often small, lit from the side or clipped by the frame edge, and at 64x48 it is only a handful of
// pixels. 0.3 (96x72) finds them; stage 1 costs roughly the square of the scale, measured ~60ms ->
// see /status face_ms.
//
// kMnpScore 0.4 rather than 0.5, for the same faces: a face half out of the frame re-scores lower in
// stage 2. Nothing else in a room survives stage 1 and scores 0.4, and the main board still
// ignores anything under conf 30.
constexpr float kMsrScore = 0.1f;
constexpr float kMsrNms = 0.5f;
constexpr int kMsrTopK = 10;
constexpr float kResizeScale = 0.3f;
constexpr float kMnpScore = 0.4f;
constexpr float kMnpNms = 0.3f;
constexpr int kMnpTopK = 5;

// While a face is locked, search only a window round where it was last seen instead of the whole
// frame (optimisation B). Stage 1 costs roughly the area it scans, so a window 2.6x the face's size
// is typically a quarter to a third of a QVGA frame; stage 2 only ever looks at candidates anyway.
// Same scale as the full search, so a face is the same size in pixels to the detector either way.
// If the window comes up empty the whole frame is searched straight away, in the same call - a
// face that jumped is found again at once rather than one detection later.
constexpr float kRoiFaceMult = 2.6f;
// Smallest window: stage 1 scales it by kResizeScale, and much under ~48x36 it has too few pixels to
// work with. Also the full frame is used whenever the window would be most of it anyway.
constexpr int kRoiMinW = 160;
constexpr int kRoiMinH = 120;
constexpr float kRoiMaxFraction = 0.6f;

// Exponential smoothing, weight of the NEW reading (optimisation D: was 0.65). The main board's
// glide already smooths the motion itself, so smoothing here too only adds lag - at 0.65 roughly
// half a detection's worth. 0.85 keeps just enough to take the edge off the box's pixel jitter.
// Roll is noisier (two points ~40px apart, so one pixel is ~1.5 deg) and gets more.
constexpr float kPosAlpha = 0.85f;
constexpr float kRollAlpha = 0.6f;

// How long before Analyse() starts the picture was actually taken. With two frame buffers and
// GRAB_LATEST (cam_main's InitTrackingCamera) the frame handed over has just finished; its exposure
// was centred ~half a frame (~20ms at QVGA) plus readout earlier. Sent to the main board as part of
// the frame's age (optimisation A).
constexpr uint32_t kCaptureLeadMs = 30;

// A face that jumps further than this (in -100..100 units) from the last one is treated as a new
// face: smoothing restarts from it instead of dragging the old position across the picture.
constexpr int kJumpReset = 45;

// Missed this many frames in a row: the face being followed is gone, start afresh.
constexpr uint8_t kMissReset = 2;

// Beyond this the eye landmarks are no longer reliable (the face is on its side, or it is not a face).
constexpr float kMaxRollDeg = 60.0f;

HumanFaceDetectMSR01* g_s1 = nullptr;
HumanFaceDetectMNP01* g_s2 = nullptr;
uint16_t* g_roi = nullptr;  // PSRAM copy of the search window, QVGA-sized

bool g_have = false;
float g_dx = 0.0f;
float g_dy = 0.0f;
float g_roll = 0.0f;
uint8_t g_misses = 0;
// Last face box in frame pixels, for the next search window.
int g_box[4] = {0, 0, 0, 0};

uint32_t g_last_ms = 0;
float g_avg_ms = 0.0f;
bool g_last_roi = false;

// One detected face, copied out of the detector's result list (which the next infer() overwrites),
// in frame coordinates.
struct Face {
  float score;
  int box[4];
  int kp[10];
};

// Built on first use rather than at static-init time: the model constructors allocate, and at
// static-init PSRAM is not up yet.
void EnsureModels() {
  if (g_s1 == nullptr) {
    g_s1 = new HumanFaceDetectMSR01(kMsrScore, kMsrNms, kMsrTopK, kResizeScale);
  }
  if (g_s2 == nullptr) {
    g_s2 = new HumanFaceDetectMNP01(kMnpScore, kMnpNms, kMnpTopK);
  }
  if (g_roi == nullptr) {
    g_roi = static_cast<uint16_t*>(heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM));
  }
}

int16_t Clamp100(float v) {
  if (v > 100.0f) {
    return 100;
  }
  if (v < -100.0f) {
    return -100;
  }
  return static_cast<int16_t>(lroundf(v));
}

// Runs both stages on `img` (iw x ih, sitting at (ox, oy) in a fw x fh frame) and picks one face:
// while one is being followed, the one nearest to it - so a second person walking through the back
// of the shot does not steal the head. Otherwise the biggest, i.e. the closest.
bool Detect(uint16_t* img, int iw, int ih, int ox, int oy, int fw, int fh, Face* out) {
  std::list<dl::detect::result_t>& candidates = g_s1->infer(img, {ih, iw, 3});
  std::list<dl::detect::result_t>& results = g_s2->infer(img, {ih, iw, 3}, candidates);
  const dl::detect::result_t* best = nullptr;
  float best_key = 0.0f;
  for (const auto& r : results) {
    if (r.box.size() < 4 || r.keypoint.size() < 10) {
      continue;
    }
    const float cx = (r.box[0] + r.box[2]) * 0.5f + ox;
    const float cy = (r.box[1] + r.box[3]) * 0.5f + oy;
    float key;
    if (g_have) {
      const float ndx = cx * 200.0f / fw - 100.0f - g_dx;
      const float ndy = cy * 200.0f / fh - 100.0f - g_dy;
      key = -(ndx * ndx + ndy * ndy);  // nearer = larger
    } else {
      key = static_cast<float>((r.box[2] - r.box[0]) * (r.box[3] - r.box[1]));
    }
    if (best == nullptr || key > best_key) {
      best = &r;
      best_key = key;
    }
  }
  if (best == nullptr) {
    return false;
  }
  out->score = best->score;
  for (int i = 0; i < 4; ++i) {
    out->box[i] = best->box[i] + ((i & 1) ? oy : ox);
  }
  for (int i = 0; i < 10; ++i) {
    out->kp[i] = best->keypoint[i] + ((i & 1) ? oy : ox);
  }
  return true;
}

// Tries the window round the last face. False if there is no usable window or nothing in it.
bool DetectNearLast(const uint8_t* rgb565, int w, int h, Face* out) {
  if (!g_have || g_roi == nullptr || w > 320 || h > 240) {
    return false;
  }
  const int bw = g_box[2] - g_box[0];
  const int bh = g_box[3] - g_box[1];
  if (bw <= 0 || bh <= 0) {
    return false;
  }
  int rw = static_cast<int>(bw * kRoiFaceMult);
  int rh = static_cast<int>(bh * kRoiFaceMult);
  rw = rw < kRoiMinW ? kRoiMinW : (rw > w ? w : rw);
  rh = rh < kRoiMinH ? kRoiMinH : (rh > h ? h : rh);
  rw &= ~1;
  rh &= ~1;
  if (static_cast<float>(rw) * rh > kRoiMaxFraction * w * h) {
    return false;  // most of the frame anyway: let the full search do it
  }
  int x0 = (g_box[0] + g_box[2]) / 2 - rw / 2;
  int y0 = (g_box[1] + g_box[3]) / 2 - rh / 2;
  x0 = x0 < 0 ? 0 : (x0 > w - rw ? w - rw : x0);
  y0 = y0 < 0 ? 0 : (y0 > h - rh ? h - rh : y0);
  // Rows of the window, packed. ~40KB from PSRAM to PSRAM: well under a millisecond.
  const uint16_t* src = reinterpret_cast<const uint16_t*>(rgb565);
  for (int y = 0; y < rh; ++y) {
    memcpy(g_roi + y * rw, src + (y0 + y) * w + x0, rw * 2);
  }
  return Detect(g_roi, rw, rh, x0, y0, w, h, out);
}

TrackSample Miss() {
  if (g_misses < 255) {
    ++g_misses;
  }
  if (g_misses >= kMissReset) {
    g_have = false;
  }
  TrackSample none{};
  return none;
}

}  // namespace

TrackSample Analyse(const uint8_t* rgb565, int width, int height) {
  EnsureModels();
  const uint32_t t0 = millis();

  Face f;
  bool found = DetectNearLast(rgb565, width, height, &f);
  g_last_roi = found;
  if (!found) {
    // The detectors take a non-const pointer but only read the image.
    uint16_t* img = reinterpret_cast<uint16_t*>(const_cast<uint8_t*>(rgb565));
    found = Detect(img, width, height, 0, 0, width, height, &f);
  }

  g_last_ms = millis() - t0;
  g_avg_ms = (g_avg_ms == 0.0f) ? static_cast<float>(g_last_ms) : g_avg_ms * 0.9f + static_cast<float>(g_last_ms) * 0.1f;

  if (!found) {
    return Miss();
  }
  g_misses = 0;
  for (int i = 0; i < 4; ++i) {
    g_box[i] = f.box[i];
  }

  // Landmark order: left eye, mouth left, nose, right eye, mouth right ("left" = the left of the
  // picture). Sorted by x anyway, so a mirrored sensor cannot flip the sign of the tilt.
  int lx = f.kp[0], ly = f.kp[1], rx = f.kp[6], ry = f.kp[7];
  if (lx > rx) {
    int t = lx;
    lx = rx;
    rx = t;
    t = ly;
    ly = ry;
    ry = t;
  }
  // + = the right-hand eye sits lower = the top of the head leans to the right of the picture,
  // the same sense as the finger's lean, so the main board's mirror mapping carries over unchanged.
  float roll = atan2f(static_cast<float>(ry - ly), static_cast<float>(rx - lx)) * (180.0f / static_cast<float>(M_PI));
  if (roll > kMaxRollDeg) {
    roll = kMaxRollDeg;
  } else if (roll < -kMaxRollDeg) {
    roll = -kMaxRollDeg;
  }

  const float dx = (f.box[0] + f.box[2]) * 100.0f / width - 100.0f;
  const float dy = (f.box[1] + f.box[3]) * 100.0f / height - 100.0f;
  if (!g_have || fabsf(dx - g_dx) > kJumpReset || fabsf(dy - g_dy) > kJumpReset) {
    g_dx = dx;
    g_dy = dy;
    g_roll = roll;
    g_have = true;
  } else {
    g_dx += (dx - g_dx) * kPosAlpha;
    g_dy += (dy - g_dy) * kPosAlpha;
    g_roll += (roll - g_roll) * kRollAlpha;
  }

  TrackSample s{};
  s.dx = Clamp100(g_dx);
  s.dy = Clamp100(g_dy);
  float conf = f.score * 100.0f;
  s.conf = static_cast<uint8_t>(conf > 100.0f ? 100 : (conf < 1.0f ? 1 : lroundf(conf)));
  s.roll = static_cast<int16_t>(lroundf(g_roll));
  s.gesture = 0;
  s.kind = 2;
  s.tip_x = static_cast<int16_t>(lx);
  s.tip_y = static_cast<int16_t>(ly);
  s.base_x = static_cast<int16_t>(rx);
  s.base_y = static_cast<int16_t>(ry);
  const int bw = f.box[2] - f.box[0];
  s.width = static_cast<uint8_t>(bw < 0 ? 0 : (bw > 255 ? 255 : bw));
  s.box_x0 = static_cast<int16_t>(f.box[0]);
  s.box_y0 = static_cast<int16_t>(f.box[1]);
  s.box_x1 = static_cast<int16_t>(f.box[2]);
  s.box_y1 = static_cast<int16_t>(f.box[3]);
  s.capture_ms = t0 - kCaptureLeadMs;
  return s;
}

void Reset() {
  g_have = false;
  g_misses = 0;
}

uint32_t LastInferMs() { return g_last_ms; }

uint32_t AvgInferMs() { return static_cast<uint32_t>(g_avg_ms + 0.5f); }

bool LastWasWindowed() { return g_last_roi; }

}  // namespace cam_face
