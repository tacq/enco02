#include "cam_tracker.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef CAM_TRACKER_DEBUG
#include <stdio.h>
#endif

// One raised finger, found without ML
// -----------------------------------
// ESP-DL's hand models need an S3's vector unit, and the old face/motion tracker could not be
// made reliable, so this finds exactly one thing - a raised finger - by colour and shape:
//
//   1. Colour. This OV2640 has a strong green cast. Measured on the user's own finger:
//      (192,212,168), greener than it is red, which the old "red > green" skin rule rejected
//      outright - the old tracker never saw skin at all and chased frame-difference noise. A
//      grey-world balance (scale red and blue so the frame averages to grey) turns that finger into
//      (248,212,195) and a white cabinet into (134,136,130): skin and not-skin again. The skin test
//      runs on the balanced colour. Where the hand is so bright that the sensor clipped (the lit
//      side of a fist comes out (248,252,216) - no colour left at all), a blown-out gap between two
//      stretches of skin on the same row is counted as skin.
//   1b. Brightness layer. Colour alone cannot tell the finger from a beige wall behind it: in the
//      user's room the wall measures hue 19 deg / 16% saturation after balancing, the finger 10 deg
//      / 14%, so the wall comes out as skin, the finger merges into it, and a finger inside a
//      bigger blob has no top of its own to walk down from - it was never found. Brightness does
//      separate them: the hand is nearer the camera and lit differently (finger luma ~230, wall
//      ~150). So when the frame's skin pixels form two clearly different brightness populations
//      (Otsu's threshold on their luma), the whole finger search runs a second time on just the
//      brighter one. The dimmer population is not searched on its own: it holds the soft rims
//      around a bright hand, which are exactly finger-shaped strips.
//   2. Shape. Skin pixels become runs per row, runs become connected blobs. Every place where a
//      blob has nothing above it is a possible fingertip. From each one the finger is walked
//      downwards, one connected run per row, while it stays narrow; it has to be long for its
//      width and end in something clearly wider (the fist). A face widens straight away, a
//      forearm has no wider base under it, and an open hand or a V sign has two or more such
//      fingers on one blob - which is not "one finger".
//   3. Lean. A least-squares line through the finger's row centres gives its angle.
//   4. Lock. Switching tracking on (and moving towards a new finger) needs a clean finger: the
//      whole of it in view, upright-ish, rounded tip, alone on its hand. Once locked, a rougher
//      sighting near the last one is still followed, so the head does not lose the finger the
//      moment its tip leaves the top of the picture.
//
// Tested offline against real frames from this camera (the scratch harness runs this exact file
// on BMPs). ~10ms per QVGA frame.

namespace {

constexpr int kStep = 2;  // every other pixel and row: QVGA becomes a 160x120 grid
constexpr int kMaxGridW = 160;
constexpr int kMaxGridH = 120;
// Skin runs per frame. A scene with more is too busy to find a finger in anyway.
constexpr int kMaxRuns = 1600;
// Longest finger segment walked, in grid rows.
constexpr int kMaxSegRows = 90;
// Finger-shaped segments kept per frame.
constexpr int kMaxCands = 24;
// Longest blown-out stretch inside a row that is bridged when skin continues on the far side.
constexpr int kMaxBrightGap = 24;

// Shape limits for following, in grid pixels.
constexpr int kMinBlobArea = 16;
constexpr int kMinLenRows = 8;         // ~16 frame px of finger
constexpr int kMinWidthX10 = 25;       // 2.5 grid px
constexpr int kLenPerWidthX10 = 16;    // length >= 1.6 x width
constexpr int kBasePerWidthX10 = 14;   // the fist below is >= 1.4 x the finger's width
// Stricter limits to START following (and to arm tracking by gesture). An arm reaching in from
// the top edge has the same outline as a finger whose tip is out of shot, a resting thumb looks
// like a finger lying down, and a strip of wood is as wide at its end as further down.
constexpr int kStrictMinLenRows = 10;
// Wider than 20 grid px (40 frame px) is a wrist or forearm more often. A finger held ~25cm from
// the lens already measures 13 including its soft edge (the old limit, which it missed by 0.1), and
// users hold it closer than that; the length, fist and tip tests below still have to pass.
constexpr int kStrictMaxWidthX10 = 200;
constexpr int kStrictLenPerWidthX10 = 22;
constexpr int kStrictBasePerWidthX10 = 18;
constexpr int kStrictMaxLeanDeg = 30;
constexpr int kStrictTipPerWidthX10 = 9;  // first row no wider than 0.9 x the finger: a round tip

// While locked, how far (grid px, per axis) the tip may move between frames and still count as
// the same finger, and how many frames it may go missing before the lock is dropped.
constexpr int kFollowRadius = 32;
constexpr uint8_t kMaxMissFrames = 4;

// Brightness layer (see 1b above). The two populations' mean luma must be at least this far apart,
// and each must have this many grid pixels (a finger alone is ~30, finger and fist ~200), before the
// brighter one is searched on its own.
constexpr int kMinLayerGapLuma = 40;
constexpr uint32_t kMinLayerPx = 48;
constexpr int kLumaBinWidth = 4;
constexpr int kLumaBins = 256 / kLumaBinWidth;

// The classified grid, one byte per grid pixel, so the finger search can run once per layer without
// classifying every pixel again. Skin pixels store their balanced luma, which the skin test keeps at
// 64 or more, so it never collides with the two other codes.
constexpr uint8_t kPxOther = 0;
constexpr uint8_t kPxBlown = 1;  // clipped near-white: could be skin, could be a shirt
constexpr uint8_t kPxSkinMin = 2;
uint8_t g_px[kMaxGridW * kMaxGridH];
uint32_t g_luma_hist[kLumaBins];  // skin pixels of the current frame by luma
int g_split = 0;                  // luma floor of this frame's bright layer, 0 = no bright layer

uint8_t g_run_y[kMaxRuns];
uint8_t g_run_x0[kMaxRuns];
uint8_t g_run_x1[kMaxRuns];
uint16_t g_parent[kMaxRuns];
uint16_t g_area[kMaxRuns];
uint8_t g_has_up[kMaxRuns];  // 1 = connected to a run in the row above: not a tip
uint8_t g_claim[kMaxRuns];   // which finger walk already went through this run (0 = none)
uint16_t g_row_start[kMaxGridH + 1];
int g_run_count = 0;

// Grey-world gains, x256.
int g_gain_r = 256;
int g_gain_b = 256;
bool g_have_gains = false;

// The finger last reported, for continuity and smoothing.
bool g_have_prev = false;
int32_t g_sx = 0;    // frame px x16
int32_t g_sy = 0;    // frame px x16
int32_t g_sang = 0;  // degrees x16
int g_prev_tip_x = 0;  // grid px
int g_prev_tip_y = 0;
uint8_t g_miss = 0;

struct Finger {
  int len;       // grid rows
  int width_x10; // mean width of the finger body, grid px x10
  int base_w;    // width of whatever it ends in, grid px
  float cx;      // centre, grid px
  float cy;
  float angle;   // degrees, + = tip leans right
  int tip_x;     // grid px
  int tip_y;
  int base_x;
  int base_y;
  int score;
  uint16_t root;  // blob (only comparable between candidates of the same layer)
  uint8_t layer;  // 0 = found among all skin, 1 = among the bright layer only
  bool tip_cut;   // tip on the top edge of the picture
  bool strict;
};

Finger g_cands[kMaxCands];
int g_ncand = 0;

inline int Clamp(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// esp32-camera emits RGB565 high byte first: RRRRRGGG GGGBBBBB.
inline void Decode(const uint8_t* p, int* r, int* g, int* b) {
  const uint8_t hb = p[0];
  const uint8_t lb = p[1];
  *r = hb & 0xF8;
  *g = ((hb & 0x07) << 5) | ((lb & 0xE0) >> 3);
  *b = (lb & 0x1F) << 3;
}

// The pixel's balanced luma (64..255) if it passes the skin test, 0 if it does not.
inline int SkinLuma(int r, int g, int b) {
  int r2 = (r * g_gain_r) >> 8;
  if (r2 > 255) {
    r2 = 255;
  }
  int b2 = (b * g_gain_b) >> 8;
  if (b2 > 255) {
    b2 = 255;
  }
  // Redder than both green and blue, and bright enough to have a colour at all.
  if (r2 < 56 || r2 - g < 10 || r2 - b2 < 10) {
    return 0;
  }
  const int luma = (r2 * 77 + g * 150 + b2 * 29) >> 8;
  if (luma < 64) {
    return 0;  // too dark for the colour to mean anything
  }
  const int mn = g < b2 ? g : b2;
  const int chroma = r2 - mn;  // > 0 here
  // Saturation (chroma / r2) at most 0.5: not a saturated red, orange or pink object.
  if (chroma * 2 > r2) {
    return 0;
  }
  // Hue. Measured after balancing: the user's skin runs from slightly magenta (blue a touch above
  // green, lit from the side) to +24 deg on the lit edge of a finger (saturation 21-22% there).
  // Wood - the desk, the chair, a door - is +14..37 deg and more saturated, so the further towards
  // yellow a pixel is, the less saturation it may have: 42% at 0 deg, 35% at 14 deg, 28% at 28 deg,
  // nothing beyond. (The wooden panel in the test frames: 28 deg, 30%.)
  if (g >= b2) {
    const int hue = 60 * (g - b2) / chroma;
    if (hue > 28 || chroma * 100 > (42 - hue / 2) * r2) {
      return 0;
    }
  } else if (60 * (b2 - g) > 20 * chroma) {
    return 0;  // more than 20 deg towards magenta
  }
  return luma;
}

// kPxOther, kPxBlown, or - for skin - its balanced luma (always >= 64, so >= kPxSkinMin).
// The highlight test is on the RAW colour - a clipped channel has no colour left to balance.
inline int Classify(int r, int g, int b) {
  const int luma = SkinLuma(r, g, b);
  if (luma != 0) {
    return luma;
  }
  const int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
  const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
  return (mn >= 176 && mx >= 216) ? kPxBlown : kPxOther;
}

// Grey world on a sparse 1-in-64 sample. Smoothed over frames so one hand filling the picture
// cannot swing the balance in a single step, and clamped to 0.85..2.0.
void UpdateGains(const uint8_t* rgb565, int width, int height) {
  uint32_t sr = 0;
  uint32_t sg = 0;
  uint32_t sb = 0;
  uint32_t n = 0;
  for (int y = 4; y < height; y += 8) {
    const uint8_t* row = rgb565 + static_cast<size_t>(y) * width * 2;
    for (int x = 4; x < width; x += 8) {
      int r;
      int g;
      int b;
      Decode(row + x * 2, &r, &g, &b);
      const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
      if (mx < 40 || mx > 240) {
        continue;  // too dark for its colour to mean anything, or clipped
      }
      sr += static_cast<uint32_t>(r);
      sg += static_cast<uint32_t>(g);
      sb += static_cast<uint32_t>(b);
      ++n;
    }
  }
  if (n < 64 || sr == 0 || sb == 0) {
    return;
  }
  const int gr = Clamp(static_cast<int>((sg << 8) / sr), 218, 512);
  const int gb = Clamp(static_cast<int>((sg << 8) / sb), 218, 512);
  if (!g_have_gains) {
    g_gain_r = gr;
    g_gain_b = gb;
    g_have_gains = true;
  } else {
    g_gain_r += (gr - g_gain_r) / 4;
    g_gain_b += (gb - g_gain_b) / 4;
  }
}

bool Emit(int y, int x0, int x1) {
  if (g_run_count >= kMaxRuns) {
    return false;
  }
  g_run_y[g_run_count] = static_cast<uint8_t>(y);
  g_run_x0[g_run_count] = static_cast<uint8_t>(x0);
  g_run_x1[g_run_count] = static_cast<uint8_t>(x1);
  ++g_run_count;
  return true;
}

// Classifies every grid pixel of the frame into g_px, and histograms the skin pixels' luma.
void ClassifyGrid(const uint8_t* rgb565, int width, int sw, int sh) {
  memset(g_luma_hist, 0, sizeof(g_luma_hist));
  for (int y = 0; y < sh; ++y) {
    const uint8_t* row = rgb565 + static_cast<size_t>(y) * kStep * width * 2;
    uint8_t* out = g_px + y * kMaxGridW;
    for (int x = 0; x < sw; ++x) {
      int r;
      int g;
      int b;
      Decode(row + x * kStep * 2, &r, &g, &b);
      const int v = Classify(r, g, b);
      out[x] = static_cast<uint8_t>(v);
      if (v >= kPxSkinMin) {
        ++g_luma_hist[v / kLumaBinWidth];
      }
    }
  }
}

// Otsu's threshold over the skin pixels' luma: the split that best separates them into a dimmer
// and a brighter population. Returns the brighter one's luma floor, or 0 when the skin in this
// frame is not two clearly different populations (too few pixels on either side, or means too
// close together - one evenly lit surface, which a split would only cut in two).
int FindSplit() {
  uint32_t total = 0;
  uint32_t sum_all = 0;
  for (int i = 0; i < kLumaBins; ++i) {
    total += g_luma_hist[i];
    sum_all += g_luma_hist[i] * static_cast<uint32_t>(i);
  }
  if (total < 2 * kMinLayerPx) {
    return 0;
  }
  uint32_t w0 = 0;
  uint32_t s0 = 0;
  float best_var = 0.0f;
  float best_gap = 0.0f;
  int best_t = 0;
  for (int t = 1; t < kLumaBins; ++t) {
    w0 += g_luma_hist[t - 1];
    s0 += g_luma_hist[t - 1] * static_cast<uint32_t>(t - 1);
    const uint32_t w1 = total - w0;
    if (w0 < kMinLayerPx || w1 < kMinLayerPx) {
      continue;
    }
    const float m0 = static_cast<float>(s0) / static_cast<float>(w0);
    const float m1 = static_cast<float>(sum_all - s0) / static_cast<float>(w1);
    const float var = static_cast<float>(w0) * static_cast<float>(w1) * (m1 - m0) * (m1 - m0);
    if (var > best_var) {
      best_var = var;
      best_gap = m1 - m0;
      best_t = t;
    }
  }
  if (best_t == 0 || best_gap * kLumaBinWidth < static_cast<float>(kMinLayerGapLuma)) {
    return 0;
  }
  return best_t * kLumaBinWidth;
}

// Skin runs for the whole frame, row by row, sorted by x within a row, counting only skin at least
// `floor` bright (0 = all skin). A one-pixel gap is bridged (sensor noise down the middle of a
// finger), and so is a longer gap that is entirely blown out (the clipped highlight on a knuckle);
// runs shorter than 2 px are dropped. False if the frame was too busy.
bool BuildRuns(int sw, int sh, int floor) {
  const int lo = floor > kPxSkinMin ? floor : kPxSkinMin;
  g_run_count = 0;
  for (int y = 0; y < sh; ++y) {
    g_row_start[y] = static_cast<uint16_t>(g_run_count);
    const uint8_t* row = g_px + y * kMaxGridW;
    int start = -1;
    int last = -10;
    bool gap_bright = true;  // every pixel since `last` was blown out
    for (int x = 0; x < sw; ++x) {
      const int v = row[x];
      if (v < lo) {
        if (v != kPxBlown) {
          gap_bright = false;  // not skin, or skin too dim for this layer
        }
        continue;
      }
      if (start < 0) {
        start = x;
      } else {
        const int gap = x - last - 1;
        if (gap > 1 && !(gap_bright && gap <= kMaxBrightGap)) {
          if (last - start + 1 >= 2 && !Emit(y, start, last)) {
            return false;
          }
          start = x;
        }
      }
      last = x;
      gap_bright = true;
    }
    if (start >= 0 && last - start + 1 >= 2 && !Emit(y, start, last)) {
      return false;
    }
  }
  g_row_start[sh] = static_cast<uint16_t>(g_run_count);
  return true;
}

uint16_t Find(uint16_t a) {
  while (g_parent[a] != a) {
    g_parent[a] = g_parent[g_parent[a]];
    a = g_parent[a];
  }
  return a;
}

void Union(uint16_t a, uint16_t b) {
  a = Find(a);
  b = Find(b);
  if (a == b) {
    return;
  }
  if (a < b) {
    g_parent[b] = a;
  } else {
    g_parent[a] = b;
  }
}

// 8-connected labelling of the runs. Afterwards every run's parent is its blob's root,
// g_area[root] is the blob's pixel count, and g_has_up[run] says whether anything touches the run
// from the row above (if not, it is the top of something: a possible fingertip).
void Label(int sh) {
  for (int i = 0; i < g_run_count; ++i) {
    g_parent[i] = static_cast<uint16_t>(i);
    g_has_up[i] = 0;
    g_claim[i] = 0;
  }
  for (int y = 1; y < sh; ++y) {
    int i = g_row_start[y - 1];
    const int ie = g_row_start[y];
    int j = g_row_start[y];
    const int je = g_row_start[y + 1];
    while (i < ie && j < je) {
      if (g_run_x0[i] <= g_run_x1[j] + 1 && g_run_x0[j] <= g_run_x1[i] + 1) {
        Union(static_cast<uint16_t>(i), static_cast<uint16_t>(j));
        g_has_up[j] = 1;
      }
      if (g_run_x1[i] < g_run_x1[j]) {
        ++i;
      } else {
        ++j;
      }
    }
  }
  for (int i = 0; i < g_run_count; ++i) {
    g_parent[i] = Find(static_cast<uint16_t>(i));
    g_area[i] = 0;
  }
  for (int i = 0; i < g_run_count; ++i) {
    g_area[g_parent[i]] += static_cast<uint16_t>(g_run_x1[i] - g_run_x0[i] + 1);
  }
}

// Walks down from one tip run, following the single run below it, while it looks like a finger.
// True if it is one. `id` marks the runs this walk used, so a second tip that only leads into the
// same finger (noise splitting the very top of it in two) is not counted as a second finger.
bool WalkFinger(uint16_t start, uint8_t id, int sw, int sh, Finger* f) {
  const int top = g_run_y[start];
  uint8_t sx0[kMaxSegRows];
  uint8_t sx1[kMaxSegRows];
  int len = 0;
  int body_w_sum = 0;  // widths of rows 2.. - the first two rows are the rounded tip
  int base_w = 0;
  bool based = false;
  int cur = start;
  int y = top;
  for (;;) {
    if (g_claim[cur] != 0) {
      return false;  // an earlier walk (a higher tip) already covers this finger
    }
    const int x0 = g_run_x0[cur];
    const int x1 = g_run_x1[cur];
    const int w = x1 - x0 + 1;
    if (len >= 4) {
      const int body_rows = len - 2;
      // Much wider than the finger so far (> 1.6x + 2px): the knuckles.
      if (w * 10 * body_rows > body_w_sum * 16 + 20 * body_rows) {
        base_w = w;
        based = true;
        break;
      }
      // Centre jumped sideways by more than half a finger: something else continues below.
      const int jump2 = abs((x0 + x1) - (sx0[len - 1] + sx1[len - 1]));
      const int wref = body_w_sum / body_rows;
      if (jump2 > (wref > 6 ? wref : 6)) {
        base_w = w;
        based = true;
        break;
      }
    }
    g_claim[cur] = id;
    sx0[len] = static_cast<uint8_t>(x0);
    sx1[len] = static_cast<uint8_t>(x1);
    if (len >= 2) {
      body_w_sum += w;
    }
    ++len;
    if (len >= kMaxSegRows || y + 1 >= sh) {
      break;  // ran off the bottom of the picture: nothing under it that we can see
    }
    // The run(s) directly below, 8-connected.
    int n = 0;
    int next = -1;
    int span0 = 255;
    int span1 = 0;
    for (int k = g_row_start[y + 1]; k < g_row_start[y + 2]; ++k) {
      if (g_run_x1[k] + 1 < x0) {
        continue;
      }
      if (g_run_x0[k] > x1 + 1) {
        break;  // runs are sorted by x
      }
      ++n;
      if (next < 0) {
        next = k;
      }
      span0 = g_run_x0[k] < span0 ? g_run_x0[k] : span0;
      span1 = g_run_x1[k] > span1 ? g_run_x1[k] : span1;
    }
    if (n == 0) {
      break;  // the blob just ends: nothing wider underneath, so not a finger on a fist
    }
    if (n > 1) {
      // The row splits under the finger: the creases of a fist, or the other curled fingers, or a
      // blown-out knuckle between two stretches of skin. What matters is how wide it all is.
      base_w = span1 - span0 + 1;
      based = true;
      break;
    }
    cur = next;
    ++y;
  }
#ifdef CAM_TRACKER_DEBUG
  if (len >= 6) {
    printf("  tip (%d,%d) w0=%d len=%d based=%d base_w=%d body_w=%.1f\n", (sx0[0] + sx1[0]) / 2, top,
           sx1[0] - sx0[0] + 1, len, based, base_w, len > 2 ? body_w_sum / (float)(len - 2) : 0.0f);
  }
#endif

  if (len < kMinLenRows || len > sh * 70 / 100) {
    return false;
  }
  const int body_start = len > 6 ? 2 : 0;
  const int body_rows = len - body_start;
  int wsum = 0;
  for (int i = body_start; i < len; ++i) {
    wsum += sx1[i] - sx0[i] + 1;
  }
  const int w_x10 = wsum * 10 / body_rows;
  if (w_x10 < kMinWidthX10 || w_x10 > sw * 10 / 8) {
    return false;
  }
  if (len * 100 < kLenPerWidthX10 * w_x10) {
    return false;  // not elongated enough
  }
  if (!based || base_w * 100 < kBasePerWidthX10 * w_x10) {
    return false;  // no fist below it
  }

  // Lean: least squares x = a + b*y through the body's row centres.
  float my = 0.0f;
  float mx = 0.0f;
  for (int i = body_start; i < len; ++i) {
    my += static_cast<float>(i);
    mx += (sx0[i] + sx1[i]) * 0.5f;
  }
  my /= static_cast<float>(body_rows);
  mx /= static_cast<float>(body_rows);
  float syy = 0.0f;
  float sxy = 0.0f;
  for (int i = body_start; i < len; ++i) {
    const float dy = static_cast<float>(i) - my;
    const float dx = (sx0[i] + sx1[i]) * 0.5f - mx;
    syy += dy * dy;
    sxy += dy * dx;
  }
  const float slope = syy > 0.0f ? sxy / syy : 0.0f;
  // x growing downwards (slope > 0) means the tip leans LEFT.
  f->angle = -atanf(slope) * 57.29578f;

  float cx = 0.0f;
  for (int i = 0; i < len; ++i) {
    cx += (sx0[i] + sx1[i]) * 0.5f;
  }
  f->cx = cx / static_cast<float>(len);
  f->cy = static_cast<float>(top) + static_cast<float>(len - 1) * 0.5f;
  f->len = len;
  f->width_x10 = w_x10;
  f->base_w = base_w;
  f->tip_x = (sx0[0] + sx1[0]) / 2;
  f->tip_y = top;
  f->base_x = (sx0[len - 1] + sx1[len - 1]) / 2;
  f->base_y = top + len - 1;
  f->root = g_parent[start];
  f->tip_cut = (top == 0);
  const int tip_w = sx1[0] - sx0[0] + 1;
  const float lean = f->angle < 0.0f ? -f->angle : f->angle;
  f->strict = !f->tip_cut && sx0[0] > 0 && sx1[0] < sw - 1 && len >= kStrictMinLenRows &&
              w_x10 <= kStrictMaxWidthX10 && len * 100 >= kStrictLenPerWidthX10 * w_x10 &&
              base_w * 100 >= kStrictBasePerWidthX10 * w_x10 && tip_w * 100 <= kStrictTipPerWidthX10 * w_x10 &&
              lean <= static_cast<float>(kStrictMaxLeanDeg);
  f->score = len * 640 / w_x10;  // 64 x length/width: the most finger-like segment wins
  return true;
}

}  // namespace

namespace cam_tracker {

void Reset() {
  g_have_prev = false;
  g_have_gains = false;
  g_miss = 0;
}

void Gains(int* red_q8, int* blue_q8) {
  *red_q8 = g_gain_r;
  *blue_q8 = g_gain_b;
}

TrackSample Analyse(const uint8_t* rgb565, int width, int height) {
  TrackSample out{};
  if (rgb565 == nullptr || width < kStep * 8 || height < kStep * 8) {
    return out;
  }
  int sw = width / kStep;
  int sh = height / kStep;
  if (sw > kMaxGridW) {
    sw = kMaxGridW;
  }
  if (sh > kMaxGridH) {
    sh = kMaxGridH;
  }

  UpdateGains(rgb565, width, height);
  ClassifyGrid(rgb565, width, sw, sh);
  g_split = FindSplit();

  // Pass 0 searches all skin, pass 1 (only if the frame has one) the bright layer on its own.
  int ncand = 0;
  const int passes = g_split > 0 ? 2 : 1;
  for (int layer = 0; layer < passes; ++layer) {
    if (!BuildRuns(sw, sh, layer == 0 ? 0 : g_split)) {
      continue;  // too busy to search at this level
    }
    Label(sh);
    const int first = ncand;
    for (int i = 0; i < g_run_count && ncand < kMaxCands; ++i) {
      if (g_has_up[i] != 0 || g_claim[i] != 0 || g_area[g_parent[i]] < kMinBlobArea) {
        continue;  // not the top of anything, already walked, or a speck
      }
      Finger f{};
      if (WalkFinger(static_cast<uint16_t>(i), static_cast<uint8_t>(ncand + 1), sw, sh, &f)) {
        f.layer = static_cast<uint8_t>(layer);
        g_cands[ncand++] = f;
      }
    }
    // One finger means one: a hand with two or more finger-shaped tops (a V sign, an open hand)
    // may still be followed once locked, but it never starts anything. Blob roots only mean
    // something within the pass that labelled them.
    for (int a = first; a < ncand; ++a) {
      for (int b = first; b < ncand; ++b) {
        if (a != b && g_cands[a].root == g_cands[b].root && g_cands[b].len * 10 >= g_cands[a].len * 6) {
          g_cands[a].strict = false;
          break;
        }
      }
    }
  }
  g_ncand = ncand;

  int best = -1;
  int best_score = 0;
  bool best_near = false;
  for (int c = 0; c < ncand; ++c) {
    const Finger& f = g_cands[c];
    const bool near = g_have_prev && abs(f.tip_x - g_prev_tip_x) < kFollowRadius &&
                      abs(f.tip_y - g_prev_tip_y) < kFollowRadius;
    if (!f.strict && !near) {
      continue;  // a rough sighting only counts next to the finger we are already following
    }
    const int score = f.score + (near ? 400 : 0) + (f.strict ? 150 : 0);
    if (best < 0 || score > best_score) {
      best = c;
      best_score = score;
      best_near = near;
    }
  }
#ifdef CAM_TRACKER_DEBUG
  printf("  split=%d\n", g_split);
  for (int c = 0; c < ncand; ++c) {
    printf("  cand L%d tip=(%d,%d) len=%d w=%.1f base_w=%d lean=%.0f strict=%d%s\n", g_cands[c].layer,
           g_cands[c].tip_x, g_cands[c].tip_y, g_cands[c].len, g_cands[c].width_x10 / 10.0f, g_cands[c].base_w,
           g_cands[c].angle, g_cands[c].strict, c == best ? "  <= best" : "");
  }
#endif

  if (best < 0) {
    if (g_miss < 255) {
      ++g_miss;
    }
    if (g_miss >= kMaxMissFrames) {
      g_have_prev = false;  // gone for good: the next sighting has to be a clean one again
    }
    return out;
  }
  const Finger& f = g_cands[best];

  const int fx = static_cast<int>(f.cx * kStep) + kStep / 2;
  const int fy = static_cast<int>(f.cy * kStep) + kStep / 2;
  const int ang = static_cast<int>(f.angle);
  if (!best_near) {
    g_sx = fx * 16;
    g_sy = fy * 16;
    g_sang = ang * 16;
  } else {
    // Half way to the new position each frame (a finger moves quickly and the head should keep
    // up), a third of the way for the lean, which is noisier.
    g_sx += (fx * 16 - g_sx) / 2;
    g_sy += (fy * 16 - g_sy) / 2;
    g_sang += (ang * 16 - g_sang) / 3;
  }
  g_have_prev = true;
  g_miss = 0;
  g_prev_tip_x = f.tip_x;
  g_prev_tip_y = f.tip_y;

  const int half_w = width / 2;
  const int half_h = height / 2;
  out.dx = static_cast<int16_t>(Clamp((static_cast<int>(g_sx / 16) - half_w) * 100 / half_w, -100, 100));
  out.dy = static_cast<int16_t>(Clamp((static_cast<int>(g_sy / 16) - half_h) * 100 / half_h, -100, 100));
  out.roll = static_cast<int16_t>(Clamp(static_cast<int>(g_sang / 16), -80, 80));
  const int ratio_x10 = f.len * 100 / f.width_x10;
  const int shape = Clamp((ratio_x10 - kLenPerWidthX10) * 2, 0, 20);
  out.conf = static_cast<uint8_t>(f.strict ? 80 + shape : (f.tip_cut ? 50 : 55 + shape));
  out.gesture = f.strict ? 1 : 0;
  out.kind = 1;
  out.tip_x = static_cast<int16_t>(f.tip_x * kStep + kStep / 2);
  out.tip_y = static_cast<int16_t>(f.tip_y * kStep);
  out.base_x = static_cast<int16_t>(f.base_x * kStep + kStep / 2);
  out.base_y = static_cast<int16_t>(f.base_y * kStep + kStep - 1);
  out.width = static_cast<uint8_t>(Clamp(f.width_x10 * kStep / 10, 1, 255));
  return out;
}

void PaintSkin(uint8_t* rgb565, int width, int height) {
  if (rgb565 == nullptr) {
    return;
  }
  for (int y = 0; y + 1 < height; y += kStep) {
    for (int x = 0; x + 1 < width; x += kStep) {
      uint8_t* p = rgb565 + (static_cast<size_t>(y) * width + x) * 2;
      int r;
      int g;
      int b;
      Decode(p, &r, &g, &b);
      const int v = Classify(r, g, b);
      if (v == kPxOther) {
        continue;
      }
      // Skin magenta (0xF81F), skin in the bright layer blue (0x001F - not green, the clean-finger
      // line drawn on top is green), blown-out highlight cyan (0x07FF). Big-endian.
      const uint16_t c = v == kPxBlown ? 0x07FF : (g_split > 0 && v >= g_split ? 0x001F : 0xF81F);
      const uint8_t hi = static_cast<uint8_t>(c >> 8);
      const uint8_t lo = static_cast<uint8_t>(c & 0xFF);
      for (int dy = 0; dy < kStep; ++dy) {
        uint8_t* q = rgb565 + (static_cast<size_t>(y + dy) * width + x) * 2;
        for (int dx = 0; dx < kStep; ++dx) {
          q[dx * 2] = hi;
          q[dx * 2 + 1] = lo;
        }
      }
    }
  }
}

int SplitLuma() {
  return g_split;
}

}  // namespace cam_tracker

#ifdef CAM_TRACKER_DEBUG
// Host harness only: tip/base (frame px) and flags of every candidate from the last Analyse().
extern "C" int cam_tracker_debug_cands(int* out, int max) {
  int n = 0;
  for (int c = 0; c < g_ncand && n < max; ++c, ++n) {
    out[n * 6 + 0] = g_cands[c].tip_x * kStep + kStep / 2;
    out[n * 6 + 1] = g_cands[c].tip_y * kStep;
    out[n * 6 + 2] = g_cands[c].base_x * kStep + kStep / 2;
    out[n * 6 + 3] = g_cands[c].base_y * kStep + kStep - 1;
    out[n * 6 + 4] = g_cands[c].strict ? 1 : 0;
    out[n * 6 + 5] = g_cands[c].width_x10 * kStep / 10;
  }
  return n;
}
#endif
