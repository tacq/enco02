#include "cam_tracker.h"

#include <stdlib.h>
#include <string.h>

// Why this is not face detection
// ------------------------------
// Every ESP32-CAM tutorial that shows a green box round a face uses `esp-face`
// (fd_forward.h / dl_lib.h). Those headers were dropped from esp32-camera years
// ago and do not exist in a current Arduino core. Their replacement, ESP-DL v2+,
// is built around the ESP32-S3's vector unit; on a plain ESP32 a single MTMN
// pass costs 1-2 seconds per frame. That is a slideshow, not a tracker - the
// head would be chasing where you stood two seconds ago.
//
// What the robot actually needs is "keep the head pointed at the person", and
// that does not require knowing it is a face. So:
//
//   1. a skin-tone chroma test finds somebody who is sitting still, and
//   2. frame differencing catches somebody moving that the chroma test missed
//      (bad white balance, backlight, a turned head).
//
// Skin wins when it has enough evidence, because it points at the head; motion
// is the fallback and points at whatever moved. Roughly 8ms per frame.

namespace {

// Sample every other pixel and every other row. At QVGA that is a 160x120 grid,
// which is plenty for a centroid and quarters the work.
constexpr int kStep = 2;

// Coarse luma grid for the motion stage.
constexpr int kGridW = 20;
constexpr int kGridH = 15;

uint8_t g_prev_grid[kGridW * kGridH];
bool g_have_prev = false;

// Output smoothing. The centroid of a blob jitters by a few percent frame to
// frame even when nothing moves; without this the head twitches continuously.
int32_t g_smooth_x = 0;
int32_t g_smooth_y = 0;
int32_t g_smooth_roll = 0;
bool g_have_smooth = false;

inline int Clamp(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Kovac et al.'s uniform-daylight skin rule, relaxed for indoor light and for
// the OV2640's enthusiastic auto white balance. R>95 and spread>15 in the
// original; a lamp-lit living room routinely lands below both.
inline bool IsSkin(int r, int g, int b) {
  if (r <= 70 || g <= 30 || b <= 15) {
    return false;
  }
  const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
  const int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
  if (mx - mn <= 10) {
    return false;  // grey: a wall, not a person
  }
  if (abs(r - g) <= 10) {
    return false;
  }
  return r > g && r > b;
}

}  // namespace

namespace cam_tracker {

void Reset() {
  g_have_prev = false;
  g_have_smooth = false;
  g_smooth_roll = 0;
}

TrackSample Analyse(const uint8_t* rgb565, int width, int height) {
  TrackSample out{0, 0, 0, 0, 0};
  if (rgb565 == nullptr || width <= 0 || height <= 0) {
    return out;
  }

  const int sw = width / kStep;
  const int sh = height / kStep;
  if (sw <= 0 || sh <= 0) {
    return out;
  }

  // Static, not stack. Together these are ~2.8KB, and the same loopTask later
  // runs an mbedTLS handshake inside Describe(); the two peaks must not stack
  // up. Analyse() is called from one task only, so sharing them is safe.
  static uint8_t grid[kGridW * kGridH];
  static uint32_t grid_sum[kGridW * kGridH];
  static uint16_t grid_n[kGridW * kGridH];
  static uint16_t head_row_n[160];
  static uint32_t head_row_sx[160];
  memset(grid_sum, 0, sizeof(grid_sum));
  memset(grid_n, 0, sizeof(grid_n));
  const int max_rows = sh < 160 ? sh : 160;
  memset(head_row_n, 0, static_cast<size_t>(max_rows) * sizeof(uint16_t));
  memset(head_row_sx, 0, static_cast<size_t>(max_rows) * sizeof(uint32_t));

  uint32_t skin_n = 0;
  uint32_t skin_sx = 0;
  uint32_t skin_sy = 0;
  int first_skin_y = -1;
  int gap_rows = 0;

  for (int y = 0; y < sh; ++y) {
    const uint8_t* row = rgb565 + static_cast<size_t>(y) * kStep * width * 2;
    const int gy = y * kGridH / sh;
    uint32_t row_skin_n = 0;
    uint32_t row_skin_sx = 0;
    for (int x = 0; x < sw; ++x) {
      const uint8_t hb = row[x * kStep * 2];
      const uint8_t lb = row[x * kStep * 2 + 1];
      // esp32-camera emits RGB565 high byte first: RRRRRGGG GGGBBBBB.
      const int r = hb & 0xF8;
      const int g = ((hb & 0x07) << 5) | ((lb & 0xE0) >> 3);
      const int b = (lb & 0x1F) << 3;

      if (IsSkin(r, g, b)) {
        ++row_skin_n;
        row_skin_sx += x;
      }

      const int gx = x * kGridW / sw;
      const int cell = gy * kGridW + gx;
      // Integer luma, close enough to Rec.601 and no multiply by a float.
      grid_sum[cell] += static_cast<uint32_t>((r * 77 + g * 150 + b * 29) >> 8);
      ++grid_n[cell];
    }

    // Lock onto the topmost full skin cluster (the head/face) rather than
    // averaging the face with hands resting on the desk at the bottom of the
    // frame. Only freeze the cluster once it has reached `skin_min` (a real
    // face-sized blob); if a small sliver above the collar ends before reaching
    // `skin_min`, reset and keep scanning downward.
    const uint32_t cluster_min = static_cast<uint32_t>(sw) * sh / 64;
    const bool past_head_band = (first_skin_y >= 0) && ((y - first_skin_y) > (sh * 45 / 100));
    if (gap_rows >= 5 || past_head_band) {
      if (skin_n >= cluster_min) {
        continue;  // already locked onto the head cluster; ignore hands below
      }
      // Top sliver was too small to be a face; reset and keep scanning lower rows
      skin_n = 0;
      skin_sx = 0;
      skin_sy = 0;
      first_skin_y = -1;
      gap_rows = 0;
      memset(head_row_n, 0, static_cast<size_t>(max_rows) * sizeof(uint16_t));
      memset(head_row_sx, 0, static_cast<size_t>(max_rows) * sizeof(uint32_t));
    }
    if (row_skin_n >= 2) {
      if (first_skin_y < 0) {
        first_skin_y = y;
      }
      gap_rows = 0;
      skin_n += row_skin_n;
      skin_sx += row_skin_sx;
      skin_sy += row_skin_n * static_cast<uint32_t>(y);
      if (y < max_rows) {
        head_row_n[y] = static_cast<uint16_t>(row_skin_n);
        head_row_sx[y] = row_skin_sx;
      }
    } else if (first_skin_y >= 0) {
      ++gap_rows;
    }
  }

  for (int i = 0; i < kGridW * kGridH; ++i) {
    grid[i] = grid_n[i] ? static_cast<uint8_t>(grid_sum[i] / grid_n[i]) : 0;
  }

  int cx = -1;
  int cy = -1;
  int raw_roll = 0;
  uint8_t conf = 0;

  // One-finger gesture (举起一根手指 -> start tracking).
  //
  // No ML here - ESP-DL's hand models need an S3's vector unit. What a single
  // raised index finger reliably IS, geometrically, is a *tall, narrow,
  // isolated* skin blob: a face is roughly as wide as it is tall (ratio ~1.3),
  // a flat palm is wider still, and a finger is 2.5x taller than it is wide.
  // Measuring the locked cluster's bounding box separates the three without a
  // single multiply-accumulate over a weight matrix.
  //
  // head_row_n[y] is the count of skin pixels on row y. For a convex blob that
  // count IS the row's width, which is all the aspect ratio needs.
  uint8_t gesture = 0;
  {
    int top = -1;
    int bottom = -1;
    int max_w = 0;
    for (int y = 0; y < max_rows; ++y) {
      if (head_row_n[y] == 0) {
        continue;
      }
      if (top < 0) {
        top = y;
      }
      bottom = y;
      if (head_row_n[y] > max_w) {
        max_w = head_row_n[y];
      }
    }
    if (top >= 0) {
      const int box_h = bottom - top + 1;
      // A finger is a *bounded* tall-narrow blob. The original test only had
      // lower bounds - narrow, and at least 2.5x taller than wide - and that
      // is satisfied perfectly by a skin-toned vertical edge: a door frame, a
      // wooden chair leg, the side of a cardboard box. Those are 2-3 sampled
      // pixels wide and run the full height of the picture, which reads as
      // "extremely narrow, extremely tall" and scored higher than a real
      // finger. Tracking switching itself on at boot with nobody gesturing was
      // exactly this.
      //
      // Four bounds turn "narrow and tall" into "finger-shaped":
      const bool narrow = (max_w > 0) && (max_w <= sw * 15 / 100);
      // ...but not a one-pixel noise column. A finger at arm's length is
      // ~20 real px across, which is 10 sampled px; 3% of the width is a
      // floor well below that and still above sensor noise.
      const bool thick_enough = max_w >= (sw * 3 / 100);
      const bool tall = box_h >= (max_w * 5) / 2;
      // ...and not 30x taller than wide. Fingers top out around 6:1 including
      // a bit of the hand below; beyond 8:1 it is a structural edge.
      const bool not_a_streak = box_h <= max_w * 8;
      // ...occupying a plausible slice of the frame, not all of it. Something
      // spanning more than 45% of the picture height at 15% of its width is
      // not attached to a human arm at any usable distance.
      const bool substantial = (box_h >= sh * 12 / 100) && (box_h <= sh * 45 / 100) && (skin_n >= 24);
      if (narrow && thick_enough && tall && not_a_streak && substantial) {
        gesture = 1;
      }
    }
  }

  // Stage 1: skin. Demand at least ~1.5% of the sampled pixels so a wooden door
  // or a beige sofa edge cannot pull the head around.
  const uint32_t skin_min = static_cast<uint32_t>(sw) * sh / 64;
  if (skin_n >= skin_min) {
    const int mean_x = static_cast<int>(skin_sx / skin_n);
    const int mean_y = static_cast<int>(skin_sy / skin_n);
    cx = mean_x * width / sw;
    cy = mean_y * height / sh;
    // Saturates at ~6% coverage, which is about a face at conversational range.
    conf = static_cast<uint8_t>(Clamp(static_cast<int>(skin_n * 100 / (skin_min * 4)), 40, 100));

    // Estimate head tilt (歪头 / roll) from the vertical cross-moment mu11/mu02
    // of the head cluster: when the top of the head (y < mean_y) shifts right
    // (x > mean_x) and the chin (y > mean_y) shifts left, mu11 < 0; when tilted
    // the other way, mu11 > 0.
    int64_t mu11 = 0;
    int64_t mu02 = 0;
    for (int y = 0; y < max_rows; ++y) {
      if (head_row_n[y] == 0) {
        continue;
      }
      const int32_t dy_row = y - mean_y;
      const int32_t sum_dx_row = static_cast<int32_t>(head_row_sx[y]) -
                                 static_cast<int32_t>(head_row_n[y]) * mean_x;
      mu11 += static_cast<int64_t>(sum_dx_row) * dy_row;
      mu02 += static_cast<int64_t>(head_row_n[y]) * dy_row * dy_row;
    }
    if (mu02 > 64) {
      // Top of head shifted right (dy_row < 0, sum_dx_row > 0 -> mu11 < 0)
      // corresponds to positive roll (+100 = ~30 deg head tilt).
      raw_roll = Clamp(static_cast<int>((-mu11 * 180) / mu02), -100, 100);
    }
  } else if (g_have_prev) {
    // Stage 2: motion.
    uint32_t mot_n = 0;
    uint32_t mot_sx = 0;
    uint32_t mot_sy = 0;
    uint32_t mot_w = 0;
    for (int gy = 0; gy < kGridH; ++gy) {
      for (int gx = 0; gx < kGridW; ++gx) {
        const int i = gy * kGridW + gx;
        const int d = abs(static_cast<int>(grid[i]) - static_cast<int>(g_prev_grid[i]));
        if (d > 18) {  // below this is sensor noise and auto-exposure breathing
          ++mot_n;
          mot_sx += static_cast<uint32_t>(gx) * d;
          mot_sy += static_cast<uint32_t>(gy) * d;
          mot_w += static_cast<uint32_t>(d);
        }
      }
    }
    // A handful of cells is noise; more than half the frame is a light being
    // switched on or the head itself moving, and following that is a feedback
    // loop that ends with the servos at their end stops.
    if (mot_n >= 4 && mot_n <= (kGridW * kGridH) / 2 && mot_w > 0) {
      cx = static_cast<int>(mot_sx / mot_w) * width / kGridW + width / (kGridW * 2);
      cy = static_cast<int>(mot_sy / mot_w) * height / kGridH + height / (kGridH * 2);
      conf = static_cast<uint8_t>(Clamp(static_cast<int>(mot_n) * 4 + 20, 20, 60));
    }
  }

  memcpy(g_prev_grid, grid, sizeof(grid));
  g_have_prev = true;

  // The gesture is reported on both paths. A raised finger is often too small
  // to clear skin_min, so it would otherwise be invisible on exactly the frames
  // it matters - the ones where tracking is still off and nothing is locked.
  out.gesture = gesture;

  if (cx < 0) {
    g_have_smooth = false;
    return out;
  }

  if (!g_have_smooth) {
    g_smooth_x = cx;
    g_smooth_y = cy;
    g_smooth_roll = raw_roll;
    g_have_smooth = true;
  } else {
    // 1/4 of the way to the new reading each frame: ~0.3s to settle at 12fps.
    g_smooth_x += (cx - g_smooth_x) / 4;
    g_smooth_y += (cy - g_smooth_y) / 4;
    g_smooth_roll += (raw_roll - g_smooth_roll) / 4;
  }

  const int half_w = width / 2;
  const int half_h = height / 2;
  out.dx = static_cast<int16_t>(Clamp((static_cast<int>(g_smooth_x) - half_w) * 100 / half_w, -100, 100));
  out.dy = static_cast<int16_t>(Clamp((static_cast<int>(g_smooth_y) - half_h) * 100 / half_h, -100, 100));
  out.conf = conf;
  out.roll = static_cast<int16_t>(Clamp(static_cast<int>(g_smooth_roll), -100, 100));
  return out;
}

}  // namespace cam_tracker
