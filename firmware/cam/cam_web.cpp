#include "cam_web.h"

#include <WebServer.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <img_converters.h>

#include "cam_remote.h"
#include "cam_vision.h"

namespace cam_web {
namespace {

WebServer g_server(80);
bool g_started = false;
SampleSink g_sink = nullptr;
VisionHandler g_vision = nullptr;
ExposureHandler g_exposure = nullptr;
TrackSample g_last_sample = {0, 0, 0};
int g_ae_level = 0;
// /stream?mask=1: paint the skin mask into the live view.
bool g_stream_mask = false;
// /stream?raw=1: frames exactly as the sensor saw them, no overlay drawn in.
bool g_stream_raw = false;
// /stream?raw=1&tracker=1: the stream belongs to tools/hand_tracker, which sends finger samples back
// up the same socket (see cam_remote.h).
bool g_stream_tracker = false;
// Whether the main board has tracking armed, pushed in from cam_main. The tracker stream runs faster
// while it is, and every frame tells the tracker (X-Armed) so its log shows what the robot is doing.
bool g_tracking_armed = false;
// One line from the tracker being assembled. Lines are ~70 bytes; anything longer is junk.
char g_in_line[128];
size_t g_in_len = 0;
bool g_in_overflow = false;

// UART link activity, pushed in from cam_main. Reported by /status so the
// main->cam direction can be verified over WiFi alone.
uint32_t g_link_rx_bytes = 0;
uint32_t g_link_cmds = 0;
uint32_t g_link_last_rx_ms = 0;

// Big-endian RGB565, because that is what the OV2640 hands us. Look at
// cam_tracker.cpp's Decode() if you doubt the byte order: red is the top 5
// bits of the first byte. Get the endianness wrong here and the marks come out
// purple-and-yellow and the image converter still accepts it without
// complaint.
constexpr uint16_t Pack565(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

inline void PutPixel(uint8_t* buf, int w, int h, int x, int y, uint16_t c) {
  if (x < 0 || x >= w || y < 0 || y >= h) {
    return;
  }
  const int idx = (y * w + x) * 2;
  buf[idx] = static_cast<uint8_t>(c >> 8);
  buf[idx + 1] = static_cast<uint8_t>(c & 0xFF);
}

// Drawn AFTER cam_tracker::Analyse() has read the buffer, so the tracker never
// sees its own marks and mistakes them for something on the next frame.
//
//   - The grey box is the main board's +/-12% deadband (kDeadband in
//     cam_link.cpp). While the finger sits inside it the head holds still.
//   - The line runs down the finger from tip to base: green for a clean finger
//     (the kind that can switch tracking on by itself), amber for a rougher
//     sighting that is only followed because tracking already had it.
//   - The white cross is the point the head is trying to bring to the middle.
void DrawOverlay(uint8_t* buf, int w, int h, const TrackSample& s) {
  constexpr uint16_t kBox = Pack565(150, 150, 150);
  const int bx0 = w * (100 - 12) / 200;
  const int bx1 = w * (100 + 12) / 200;
  const int by0 = h * (100 - 12) / 200;
  const int by1 = h * (100 + 12) / 200;
  for (int x = bx0; x <= bx1; x += 2) {
    PutPixel(buf, w, h, x, by0, kBox);
    PutPixel(buf, w, h, x, by1, kBox);
  }
  for (int y = by0; y <= by1; y += 2) {
    PutPixel(buf, w, h, bx0, y, kBox);
    PutPixel(buf, w, h, bx1, y, kBox);
  }

  if (s.conf == 0 || s.kind != 1) {
    return;
  }

  // Samples from the hand tracker (cam_remote) carry the finger centre only, no tip/base geometry;
  // for those just the cross is drawn.
  const bool has_line = s.tip_x != 0 || s.tip_y != 0 || s.base_x != 0 || s.base_y != 0;
  const uint16_t col = s.gesture ? Pack565(40, 240, 80) : Pack565(255, 190, 30);
  const int ddx = s.base_x - s.tip_x;
  const int ddy = s.base_y - s.tip_y;
  int steps = abs(ddx) > abs(ddy) ? abs(ddx) : abs(ddy);
  if (steps < 1) {
    steps = 1;
  }
  for (int t = 0; has_line && t <= steps; ++t) {
    const int x = s.tip_x + ddx * t / steps;
    const int y = s.tip_y + ddy * t / steps;
    PutPixel(buf, w, h, x - 1, y, col);
    PutPixel(buf, w, h, x, y, col);
    PutPixel(buf, w, h, x + 1, y, col);
  }

  constexpr uint16_t kCross = Pack565(255, 255, 255);
  const int cx = (s.dx + 100) * w / 200;
  const int cy = (s.dy + 100) * h / 200;
  for (int d = -6; d <= 6; ++d) {
    PutPixel(buf, w, h, cx + d, cy, kCross);
    PutPixel(buf, w, h, cx, cy + d, kCross);
  }
}

enum class Look { kOverlay, kMask, kRaw };

// A JPEG ready to send: either the sensor's own hardware-compressed frame, held
// in `fb` until it has been written out, or a software-encoded copy in `buf`.
// Release() gives back whichever it is; always call it.
struct JpegFrame {
  camera_fb_t* fb = nullptr;
  uint8_t* buf = nullptr;
  size_t len = 0;

  const uint8_t* data() const { return fb != nullptr ? fb->buf : buf; }

  void Release() {
    if (fb != nullptr) {
      esp_camera_fb_return(fb);
      fb = nullptr;
    }
    free(buf);
    buf = nullptr;
    len = 0;
  }
};

// One frame through the same pipeline whether it is going to /snapshot.jpg or
// into the middle of the MJPEG stream. Caller must Release() `*out`.
//
// `analyse` false skips the on-board finger finder entirely - used while the
// hand tracker (cam_remote) is steering, so its samples are the only ones that
// reach the sink; the overlay then shows the tracker's latest sample instead.
//
// A frame the sensor compressed itself (hand tracker mode, see cam_main's
// InitStreamCamera) is passed through untouched: there are no pixels to analyse
// or draw on, and decoding it only to encode it again would bring back the
// ~250ms software encode that mode exists to avoid.
bool GrabAnnotatedJpeg(JpegFrame* out, Look look, bool analyse, int quality) {
  *out = JpegFrame();

  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == nullptr) {
    return false;
  }
  if (fb->format == PIXFORMAT_JPEG) {
    out->fb = fb;
    out->len = fb->len;
    return true;
  }
  // Everything below reads the buffer as w*h*2 bytes of RGB565.
  if (fb->format != PIXFORMAT_RGB565) {
    esp_camera_fb_return(fb);
    return false;
  }

  if (analyse) {
    g_last_sample = cam_tracker::Analyse(fb->buf, fb->width, fb->height);
    if (g_sink != nullptr) {
      g_sink(g_last_sample);
    }
  }
  if (look == Look::kMask) {
    // Magenta = counted as skin, cyan = blown-out highlight (bridged when it
    // sits between two stretches of skin).
    cam_tracker::PaintSkin(fb->buf, fb->width, fb->height);
  }
  if (look != Look::kRaw) {
    DrawOverlay(fb->buf, fb->width, fb->height, g_last_sample);
  }

  const bool ok = frame2jpg(fb, quality, &out->buf, &out->len);
  esp_camera_fb_return(fb);
  if (!ok) {
    out->Release();
  }
  return ok;
}

constexpr char kIndexHtml[] PROGMEM = R"HTML(<!doctype html>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>enco02 cam</title>
<style>
  body { font: 14px/1.5 ui-monospace, SFMono-Regular, Menlo, monospace;
         max-width: 680px; margin: 24px auto; padding: 0 16px;
         background: #111; color: #ddd; }
  h1 { font-size: 15px; font-weight: 600; margin: 0 0 12px; color: #fff; }
  img { width: 100%; max-width: 640px; background: #000; display: block;
        border: 1px solid #333; image-rendering: pixelated; }
  .bar { margin: 10px 0; display: flex; gap: 8px; flex-wrap: wrap; }
  input { flex: 1; min-width: 180px; padding: 6px 8px; background: #1c1c1c;
          color: #eee; border: 1px solid #333; font: inherit; }
  button { padding: 6px 12px; background: #2a2a2a; color: #eee;
           border: 1px solid #444; font: inherit; cursor: pointer; }
  button:hover { background: #383838; }
  pre { background: #181818; border: 1px solid #2a2a2a; padding: 10px;
        white-space: pre-wrap; word-break: break-word; margin: 8px 0; }
  .legend { color: #888; font-size: 12px; }
</style>
<h1>ENCO-02 camera bring-up</h1>
<img id="v" src="/stream" alt="stream">
<div class="legend">
  grey box = main board &plusmn;12% deadband &middot;
  green line = clean single finger (can switch tracking on) &middot;
  amber = rough sighting, followed only once tracking has it &middot;
  white cross = point the head centres &middot;
  mask: magenta = skin, blue = brighter skin (searched again on its own, so a finger in front of a
  skin-coloured wall still stands out), cyan = blown-out highlight<br>
  While the hand tracker (tools/hand_tracker on a computer) is connected it owns the live stream and
  does the finger finding; this page then shows a snapshot every second, the cross marking the
  tracker's finger.
</div>
<div class="bar">
  <input id="q" value="这是什么" placeholder="question for the vision model">
  <button onclick="ask()">Ask vision (V)</button>
  <button onclick="flip('v')">Flip V</button>
  <button onclick="flip('h')">Flip H</button>
  <button onclick="reloadStream()">Reconnect stream</button>
</div>
<div class="bar">
  <button id="bm" onclick="toggleMask()">Skin mask: off</button>
  <button onclick="ae(-1)">Exposure &minus;</button>
  <button onclick="ae(1)">Exposure +</button>
  <button onclick="window.open('/snapshot.jpg?raw=1&t=' + Date.now())">Raw snapshot</button>
</div>
<pre id="ans">vision answer will appear here</pre>
<pre id="st">loading status...</pre>
<script>
let vflip = 0, hmirror = 0, mask = 0, aeLevel = 0, extActive = false;
const video = document.getElementById('v');
function reloadStream() {
  video.src = '/stream?mask=' + mask + '&t=' + Date.now();
}
// The camera refuses /stream (409) while the hand tracker holds it: show snapshots instead.
function nextSnapshot(delayMs) {
  setTimeout(() => { if (extActive) video.src = '/snapshot.jpg?t=' + Date.now(); }, delayMs);
}
video.onload = () => { if (extActive) nextSnapshot(1000); };
video.onerror = () => { if (extActive) nextSnapshot(1000); };
function toggleMask() {
  mask ^= 1;
  document.getElementById('bm').textContent = 'Skin mask: ' + (mask ? 'on' : 'off');
  reloadStream();
}
async function ae(step) {
  const next = Math.max(-2, Math.min(2, aeLevel + step));
  try {
    const r = await fetch('/configure?ae=' + next);
    const j = await r.json();
    aeLevel = j.ae;
  } catch (_) {}
  pollStatus();
}
async function flip(axis) {
  if (axis === 'v') vflip ^= 1; else hmirror ^= 1;
  document.getElementById('v').src = '';
  await fetch('/configure?vflip=' + vflip + '&hmirror=' + hmirror);
  reloadStream();
}
async function ask() {
  const ans = document.getElementById('ans');
  const q = document.getElementById('q').value;
  // Release the MJPEG connection first. WebServer is single-threaded, so a
  // browser holding /stream open would block /vision until the stream noticed
  // a second request - and on some browsers it sits in the same socket queue
  // and never arrives at all.
  document.getElementById('v').src = '';
  ans.textContent = 'capturing and asking... (takes ~3s)';
  try {
    const r = await fetch('/vision?q=' + encodeURIComponent(q));
    ans.textContent = await r.text();
  } catch (e) {
    ans.textContent = 'error: ' + e;
  }
  reloadStream();
}
async function pollStatus() {
  try {
    const r = await fetch('/status');
    const j = await r.json();
    vflip = j.vflip; hmirror = j.hmirror; aeLevel = j.ae;
    const wasExt = extActive;
    extActive = j.ext === 1;
    if (extActive && !wasExt) nextSnapshot(0);
    if (!extActive && wasExt) reloadStream();
    const finger = j.conf > 0
      ? (j.gesture ? 'clean' : 'rough') + ' conf=' + j.conf + ' at dx=' + j.dx + ' dy=' + j.dy +
        ' lean=' + j.roll + 'deg' +
        (extActive ? ' (from hand tracker)' : ' tip=(' + j.tip_x + ',' + j.tip_y + ') width=' + j.w + 'px')
      : 'none';
    document.getElementById('st').textContent =
      'ip: ' + j.ip + '   heap: ' + j.heap + '   psram: ' + j.psram + '\n' +
      'vision route: ' + j.vision + '   vflip: ' + j.vflip + ' hmirror: ' + j.hmirror + '\n' +
      'exposure: ' + j.ae + '   colour gains r=' + (j.gain_r / 256).toFixed(2) +
      ' b=' + (j.gain_b / 256).toFixed(2) +
      '   bright layer: ' + (j.split > 0 ? 'luma >= ' + j.split : 'none') + '\n' +
      'hand tracker: ' + (extActive ? 'steering' : 'not connected') +
      '   samples ok=' + j.ext_ok + ' rejected=' + j.ext_bad + '\n' +
      'finger: ' + finger;
  } catch (_) {}
}
setInterval(pollStatus, 2000);
pollStatus();
</script>
)HTML";

void HandleRoot() {
  g_server.send_P(200, "text/html; charset=utf-8", kIndexHtml);
}

void HandleStatus() {
  sensor_t* s = esp_camera_sensor_get();
  const int vf = (s != nullptr) ? s->status.vflip : 0;
  const int hm = (s != nullptr) ? s->status.hmirror : 0;
  int gain_r = 256;
  int gain_b = 256;
  cam_tracker::Gains(&gain_r, &gain_b);
  char buf[720];
  // link_rx / link_cmds / link_age_ms answer "is the main board reaching us?"
  // over WiFi, with no serial adapter attached. The cam being silent looks
  // identical from the main board whether the fault is our transmit line or
  // its receive line; a byte counter on this side tells the two apart.
  // link_age_ms is -1 if we have never received anything at all.
  const uint32_t now = millis();
  const long link_age = (g_link_last_rx_ms == 0)
                            ? -1L
                            : static_cast<long>(now - g_link_last_rx_ms);
  // ext* describe the hand tracker (cam_remote): is it steering right now, how
  // many of its lines were accepted / rejected since boot, how long since the
  // last good one (-1 = never). track_key says whether a key is configured at
  // all - never the key itself.
  //
  // sensor/res: what the OV2640 is producing - rgb565 for the on-board finder
  // and the robot's viewfinder, jpeg while the hand tracker is steering.
  const char* fmt = "off";
  int res_w = 0;
  int res_h = 0;
  if (s != nullptr) {
    fmt = s->pixformat == PIXFORMAT_JPEG ? "jpeg" : (s->pixformat == PIXFORMAT_RGB565 ? "rgb565" : "other");
    if (s->status.framesize < FRAMESIZE_INVALID) {
      res_w = resolution[s->status.framesize].width;
      res_h = resolution[s->status.framesize].height;
    }
  }
  snprintf(buf, sizeof(buf),
           "{\"ip\":\"%s\",\"heap\":%u,\"psram\":%u,\"vision\":\"%s\","
           "\"vflip\":%d,\"hmirror\":%d,\"ae\":%d,\"gain_r\":%d,\"gain_b\":%d,\"split\":%d,"
           "\"dx\":%d,\"dy\":%d,\"roll\":%d,\"conf\":%u,\"gesture\":%u,\"kind\":%u,"
           "\"tip_x\":%d,\"tip_y\":%d,\"base_x\":%d,\"base_y\":%d,\"w\":%u,"
           "\"link_rx\":%u,\"link_cmds\":%u,\"link_age_ms\":%ld,"
           "\"armed\":%d,\"track_key\":%d,\"ext\":%d,\"ext_ok\":%u,\"ext_bad\":%u,\"ext_age_ms\":%ld,"
           "\"sensor\":\"%s\",\"res\":\"%dx%d\"}",
           WiFi.localIP().toString().c_str(), static_cast<unsigned>(ESP.getFreeHeap()),
           static_cast<unsigned>(ESP.getFreePsram()),
           cam_vision::HasEndpoint() ? "server (xiaozhi)" : "config-key", vf, hm, g_ae_level, gain_r, gain_b,
           cam_tracker::SplitLuma(), g_last_sample.dx, g_last_sample.dy, g_last_sample.roll,
           static_cast<unsigned>(g_last_sample.conf),
           static_cast<unsigned>(g_last_sample.gesture), static_cast<unsigned>(g_last_sample.kind),
           g_last_sample.tip_x, g_last_sample.tip_y, g_last_sample.base_x, g_last_sample.base_y,
           static_cast<unsigned>(g_last_sample.width),
           static_cast<unsigned>(g_link_rx_bytes), static_cast<unsigned>(g_link_cmds), link_age,
           g_tracking_armed ? 1 : 0, cam_remote::Enabled() ? 1 : 0, cam_remote::Active(now) ? 1 : 0,
           static_cast<unsigned>(cam_remote::AcceptedCount()), static_cast<unsigned>(cam_remote::RejectedCount()),
           cam_remote::LastAcceptedAgeMs(now), fmt, res_w, res_h);
  g_server.send(200, "application/json", buf);
}

void HandleConfigure() {
  sensor_t* s = esp_camera_sensor_get();
  if (s != nullptr) {
    bool flipped = false;
    if (g_server.hasArg("vflip")) {
      s->set_vflip(s, g_server.arg("vflip").toInt() ? 1 : 0);
      flipped = true;
    }
    if (g_server.hasArg("hmirror")) {
      s->set_hmirror(s, g_server.arg("hmirror").toInt() ? 1 : 0);
      flipped = true;
    }
    if (flipped) {
      // With fb_count=1, changing SCCB orientation registers mid-DMA leaves one
      // stale or half-flipped frame in the buffer. Drain two frames so the very
      // next /snapshot.jpg or /stream frame is clean.
      delay(30);
      for (int i = 0; i < 2; ++i) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb != nullptr) {
          esp_camera_fb_return(fb);
        }
      }
    }
    cam_tracker::Reset();
  }
  if (g_server.hasArg("ae") && g_exposure != nullptr) {
    // Allow-list rather than toInt(): anything but -2..2 is ignored, not
    // silently turned into 0.
    static const char* const kLevels[] = {"-2", "-1", "0", "1", "2"};
    const String v = g_server.arg("ae");
    for (int i = 0; i < 5; ++i) {
      if (v == kLevels[i]) {
        g_exposure(i - 2);
        break;
      }
    }
  }
  if (g_server.hasArg("url")) {
    cam_vision::SetEndpoint(g_server.arg("url").c_str(),
                            g_server.arg("token").c_str(),
                            g_server.arg("mac").c_str());
  }
  HandleStatus();
}

void HandleSnapshot() {
  const bool raw = g_server.arg("raw") == "1";
  const Look look = raw                            ? Look::kRaw
                    : g_server.arg("mask") == "1" ? Look::kMask
                                                  : Look::kOverlay;
  // While the hand tracker is steering, a snapshot must not run the on-board
  // finder: its sample would go to the main board between the tracker's.
  const bool analyse = !cam_remote::Active(millis());
  JpegFrame frame;
  if (!GrabAnnotatedJpeg(&frame, look, analyse, raw ? 92 : 80)) {
    g_server.send(500, "text/plain", "capture failed");
    return;
  }
  WiFiClient c = g_server.client();
  c.printf(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: image/jpeg\r\n"
      "Content-Length: %u\r\n"
      "Cache-Control: no-store\r\n"
      "Connection: close\r\n\r\n",
      static_cast<unsigned>(frame.len));
  c.write(frame.data(), frame.len);
  frame.Release();
}

WiFiClient g_stream_client;
uint32_t g_last_stream_ms = 0;
constexpr uint32_t kStreamIntervalMs = 100;  // ~10 fps ceiling
// The hand tracker's stream: ~15 fps while tracking is armed, ~7 fps while it
// only has to spot the raised finger that arms it. Reachable because the sensor
// compresses these frames itself (cam_main's InitStreamCamera); the software
// encode of a QVGA RGB565 frame took ~250ms and capped everything at ~4 fps,
// which is what the first second of a session still runs at, until the
// tracker's first sample arrives and the sensor switches over.
constexpr uint32_t kTrackerFastIntervalMs = 66;
constexpr uint32_t kTrackerIdleIntervalMs = 150;

void EndTrackerSession() {
  if (!g_stream_tracker) {
    return;
  }
  g_stream_tracker = false;
  g_in_len = 0;
  g_in_overflow = false;
  cam_remote::EndSession();
  Serial.println("[cam] hand tracker disconnected");
}

void StopStreamClient() {
  if (g_stream_client) {
    g_stream_client.stop();
  }
  EndTrackerSession();
}

// MJPEG over multipart/x-mixed-replace, pumped one frame at a time from Poll().
//
// A naive `while (c.connected())` loop here would monopolise Arduino's
// single-threaded WebServer for as long as the browser tab stayed open,
// starving /status, /configure, /vision and the UART link. Instead,
// HandleStream() just sends the multipart header and parks the socket in
// `g_stream_client`; Poll() emits one frame every kStreamIntervalMs and runs
// `g_server.handleClient()` in between so every other route works while the
// video is playing.
//
// /stream?raw=1&tracker=1 is the hand tracker (tools/hand_tracker). It gets
// raw frames and a fresh nonce in X-Track-Nonce, and sends its finger samples
// back up the same socket (PumpStreamInput). WebServer lets go of a client once
// its handler returns, so nothing else ever reads from this socket.
void HandleStream() {
  const bool tracker = g_server.arg("tracker") == "1";
  if (tracker && !cam_remote::Enabled()) {
    g_server.send(503, "text/plain", "hand tracker disabled: no CAM_TRACK_KEY in cam_config.h");
    return;
  }
  // A browser must not kick the tracker off while it is steering the head.
  // (A new tracker connection does replace an old one - that is how it
  // recovers from a half-dead socket after a Wi-Fi hiccup.)
  if (!tracker && g_stream_tracker && IsStreaming() && cam_remote::Active(millis())) {
    g_server.send(409, "text/plain", "the hand tracker is using the stream; /snapshot.jpg still works");
    return;
  }
  StopStreamClient();
  g_stream_mask = g_server.arg("mask") == "1";
  g_stream_raw = tracker || g_server.arg("raw") == "1";
  g_stream_client = g_server.client();
  g_stream_client.setNoDelay(true);
  char nonce_header[64] = "";
  if (tracker) {
    char nonce[33];
    cam_remote::BeginSession(nonce);
    g_stream_tracker = true;
    snprintf(nonce_header, sizeof(nonce_header), "X-Track-Nonce: %s\r\n", nonce);
    Serial.printf("[cam] hand tracker connected from %s\n", g_stream_client.remoteIP().toString().c_str());
  }
  g_stream_client.printf(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
      "Cache-Control: no-store\r\n"
      "%s"
      "Connection: close\r\n\r\n",
      nonce_header);
}

// Reads the hand tracker's sample lines off the stream socket. Bounded per call
// so a flood cannot starve the rest of loop(); cam_remote rate-limits and
// authenticates every line, and a connection that keeps sending garbage is
// dropped.
void PumpStreamInput() {
  if (!g_stream_tracker) {
    return;
  }
  if (!g_stream_client || !g_stream_client.connected()) {
    EndTrackerSession();
    return;
  }
  uint8_t buf[128];
  int budget = 512;
  while (budget > 0 && g_stream_client.available() > 0) {
    const int want = budget < static_cast<int>(sizeof(buf)) ? budget : static_cast<int>(sizeof(buf));
    const int n = g_stream_client.read(buf, want);
    if (n <= 0) {
      break;
    }
    budget -= n;
    for (int i = 0; i < n; ++i) {
      const char c = static_cast<char>(buf[i]);
      if (c == '\n') {
        g_in_line[g_in_overflow ? 0 : g_in_len] = '\0';  // an overlong line is judged as empty: rejected
        TrackSample s = {};
        if (cam_remote::HandleLine(g_in_line, millis(), &s)) {
          g_last_sample = s;
          if (g_sink != nullptr) {
            g_sink(s);
          }
        }
        g_in_len = 0;
        g_in_overflow = false;
      } else if (c == '\r') {
        continue;
      } else if (g_in_len < sizeof(g_in_line) - 1) {
        g_in_line[g_in_len++] = c;
      } else {
        g_in_overflow = true;
      }
    }
  }
  if (cam_remote::ShouldDrop()) {
    Serial.println("[cam] hand tracker: too many bad lines, dropping it");
    StopStreamClient();
  }
}

void PumpStreamFrame() {
  if (!g_stream_client || !g_stream_client.connected()) {
    return;
  }
  const uint32_t now = millis();
  const uint32_t interval = !g_stream_tracker ? kStreamIntervalMs
                            : g_tracking_armed ? kTrackerFastIntervalMs
                                               : kTrackerIdleIntervalMs;
  if (now - g_last_stream_ms < interval) {
    return;
  }
  g_last_stream_ms = now;

  // Once the tracker's samples are arriving, the on-board finder stands down.
  // Until then - tracker still starting, or a key mismatch - it carries on.
  const bool analyse = !(g_stream_tracker && cam_remote::Active(now));
  const Look look = g_stream_raw ? Look::kRaw : (g_stream_mask ? Look::kMask : Look::kOverlay);
  // A hardware JPEG frame stays in the driver's buffer while it is written out
  // - no copy. With two frame buffers the sensor fills the other meanwhile.
  JpegFrame frame;
  if (!GrabAnnotatedJpeg(&frame, look, analyse, 80)) {
    StopStreamClient();
    return;
  }

  if (g_stream_tracker) {
    g_stream_client.printf(
        "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\nX-Armed: %d\r\n\r\n",
        static_cast<unsigned>(frame.len), g_tracking_armed ? 1 : 0);
  } else {
    g_stream_client.printf(
        "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
        static_cast<unsigned>(frame.len));
  }
  const size_t written = g_stream_client.write(frame.data(), frame.len);
  g_stream_client.print("\r\n");
  const bool complete = written == frame.len;
  frame.Release();
  if (!complete) {
    StopStreamClient();
  }
}

void HandleVision() {
  if (g_vision == nullptr) {
    g_server.send(503, "text/plain; charset=utf-8", "vision handler not wired");
    return;
  }
  StopStreamClient();
  const String q = g_server.arg("q");
  String out;
  const bool ok = g_vision(q.c_str(), &out);
  g_server.send(ok ? 200 : 502, "text/plain; charset=utf-8",
                (ok ? "L " : "E ") + out);
}

}  // namespace

void SetSampleSink(SampleSink sink) { g_sink = sink; }

void SetVisionHandler(VisionHandler handler) { g_vision = handler; }

void SetExposureHandler(ExposureHandler handler) { g_exposure = handler; }

void SetExposureLevel(int level) { g_ae_level = level; }

void SetLastSample(const TrackSample& s) { g_last_sample = s; }

void SetLinkStats(uint32_t rx_bytes, uint32_t commands, uint32_t last_rx_ms) {
  g_link_rx_bytes = rx_bytes;
  g_link_cmds = commands;
  g_link_last_rx_ms = last_rx_ms;
}

void SetTrackingArmed(bool armed) { g_tracking_armed = armed; }

bool IsStreaming() { return g_stream_client && g_stream_client.connected(); }

bool TrackerActive() { return cam_remote::Active(millis()); }

bool TrackerConnected() { return g_stream_tracker && IsStreaming(); }

void StopStream() { StopStreamClient(); }

void Begin() {
  if (g_started || WiFi.status() != WL_CONNECTED) {
    return;
  }
  g_server.on("/", HTTP_GET, HandleRoot);
  g_server.on("/stream", HTTP_GET, HandleStream);
  g_server.on("/snapshot.jpg", HTTP_GET, HandleSnapshot);
  g_server.on("/status", HTTP_GET, HandleStatus);
  g_server.on("/configure", HTTP_GET, HandleConfigure);
  g_server.on("/vision", HTTP_GET, HandleVision);
  g_server.begin();
  g_started = true;
  Serial.printf("[cam] web ui: http://%s/\n", WiFi.localIP().toString().c_str());
}

void Poll() {
  if (!g_started) {
    if (WiFi.status() == WL_CONNECTED) {
      Begin();
    }
    return;
  }
  g_server.handleClient();
  PumpStreamInput();
  PumpStreamFrame();
}

}  // namespace cam_web
