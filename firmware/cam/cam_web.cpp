#include "cam_web.h"

#include <WebServer.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <img_converters.h>

#include "cam_vision.h"

namespace cam_web {
namespace {

WebServer g_server(80);
bool g_started = false;
SampleSink g_sink = nullptr;
VisionHandler g_vision = nullptr;
TrackSample g_last_sample = {0, 0, 0};

// UART link activity, pushed in from cam_main. Reported by /status so the
// main->cam direction can be verified over WiFi alone.
uint32_t g_link_rx_bytes = 0;
uint32_t g_link_cmds = 0;
uint32_t g_link_last_rx_ms = 0;

// Big-endian RGB565, because that is what the OV2640 hands us. Look at
// cam_tracker.cpp:32 if you doubt the byte order: `(buf[idx] << 8) | buf[idx+1]`
// puts red in the top 5 bits of the first byte. Get the endianness wrong here
// and the crosshair comes out purple-and-yellow and the image converter still
// accepts it without complaint.
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
// sees its own reticle and locks onto it on the next frame.
//
// Two marks:
//
//   - The grey box is the main board's +/-12% deadband (kDeadband in
//     cam_link.cpp). Inside it the servos do not move. Seeing the box makes it
//     obvious why the head is still when you are nearly centred.
//
//   - The crosshair is where the tracker thinks the subject is. Green for a
//     skin-tone lock (conf >= 60), amber for the motion fallback (conf >= 25),
//     nothing at all below that because the cam does not report those samples
//     over the wire either.
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

  if (s.conf < 25) {
    return;
  }

  const uint16_t col = (s.conf >= 60) ? Pack565(40, 240, 80) : Pack565(255, 190, 30);
  const int tx = (s.dx + 100) * w / 200;
  const int ty = (s.dy + 100) * h / 200;

  // Tilt the crosshair by s.roll (-100..+100) so head tilt (歪头) is visible
  // directly on the video stream.
  for (int d = -12; d <= 12; ++d) {
    const int tilt = (d * s.roll) / 180;
    PutPixel(buf, w, h, tx + d, ty + tilt, col);
    PutPixel(buf, w, h, tx + d, ty + tilt + 1, col);
    PutPixel(buf, w, h, tx - tilt, ty + d, col);
    PutPixel(buf, w, h, tx - tilt + 1, ty + d, col);
  }
}

// One frame through the same pipeline whether it is going to /snapshot.jpg or
// into the middle of the MJPEG stream. Caller frees `*jpg`.
bool GrabAnnotatedJpeg(uint8_t** jpg, size_t* jpg_len) {
  *jpg = nullptr;
  *jpg_len = 0;

  camera_fb_t* fb = esp_camera_fb_get();
  if (fb == nullptr) {
    return false;
  }

  g_last_sample = cam_tracker::Analyse(fb->buf, fb->width, fb->height);
  if (g_sink != nullptr) {
    g_sink(g_last_sample);
  }
  DrawOverlay(fb->buf, fb->width, fb->height, g_last_sample);

  const bool ok = frame2jpg(fb, 80, jpg, jpg_len);
  esp_camera_fb_return(fb);
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
  green crosshair = skin-tone lock (conf &ge; 60) &middot;
  amber = motion fallback
</div>
<div class="bar">
  <input id="q" value="这是什么" placeholder="question for the vision model">
  <button onclick="ask()">Ask vision (V)</button>
  <button onclick="flip('v')">Flip V</button>
  <button onclick="flip('h')">Flip H</button>
  <button onclick="reloadStream()">Reconnect stream</button>
</div>
<pre id="ans">vision answer will appear here</pre>
<pre id="st">loading status...</pre>
<script>
let vflip = 0, hmirror = 0;
function reloadStream() {
  document.getElementById('v').src = '/stream?t=' + Date.now();
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
    vflip = j.vflip; hmirror = j.hmirror;
    document.getElementById('st').textContent =
      'ip: ' + j.ip + '   heap: ' + j.heap + '   psram: ' + j.psram + '\n' +
      'vision route: ' + j.vision + '   vflip: ' + j.vflip + ' hmirror: ' + j.hmirror + '\n' +
      'last sample: dx=' + j.dx + ' dy=' + j.dy + ' roll=' + (j.roll || 0) +
      ' conf=' + j.conf + ' finger=' + (j.gesture || 0);
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
  char buf[400];
  // link_rx / link_cmds / link_age_ms answer "is the main board reaching us?"
  // over WiFi, with no serial adapter attached. The cam being silent looks
  // identical from the main board whether the fault is our transmit line or
  // its receive line; a byte counter on this side tells the two apart.
  // link_age_ms is -1 if we have never received anything at all.
  const long link_age = (g_link_last_rx_ms == 0)
                            ? -1L
                            : static_cast<long>(millis() - g_link_last_rx_ms);
  snprintf(buf, sizeof(buf),
           "{\"ip\":\"%s\",\"heap\":%u,\"psram\":%u,\"vision\":\"%s\","
           "\"vflip\":%d,\"hmirror\":%d,\"dx\":%d,\"dy\":%d,\"roll\":%d,\"conf\":%u,"
           "\"gesture\":%u,\"link_rx\":%u,\"link_cmds\":%u,\"link_age_ms\":%ld}",
           WiFi.localIP().toString().c_str(), static_cast<unsigned>(ESP.getFreeHeap()),
           static_cast<unsigned>(ESP.getFreePsram()),
           cam_vision::HasEndpoint() ? "server (xiaozhi)" : "config-key", vf, hm,
           g_last_sample.dx, g_last_sample.dy, g_last_sample.roll,
           static_cast<unsigned>(g_last_sample.conf),
           static_cast<unsigned>(g_last_sample.gesture),
           static_cast<unsigned>(g_link_rx_bytes), static_cast<unsigned>(g_link_cmds), link_age);
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
  if (g_server.hasArg("url")) {
    cam_vision::SetEndpoint(g_server.arg("url").c_str(),
                            g_server.arg("token").c_str(),
                            g_server.arg("mac").c_str());
  }
  HandleStatus();
}

void HandleSnapshot() {
  uint8_t* jpg = nullptr;
  size_t jpg_len = 0;
  if (!GrabAnnotatedJpeg(&jpg, &jpg_len)) {
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
      static_cast<unsigned>(jpg_len));
  c.write(jpg, jpg_len);
  free(jpg);
}

WiFiClient g_stream_client;
uint32_t g_last_stream_ms = 0;
constexpr uint32_t kStreamIntervalMs = 100;  // ~10 fps ceiling

// MJPEG over multipart/x-mixed-replace, pumped one frame at a time from Poll().
//
// A naive `while (c.connected())` loop here would monopolise Arduino's
// single-threaded WebServer for as long as the browser tab stayed open,
// starving /status, /configure, /vision and the UART link. Instead,
// HandleStream() just sends the multipart header and parks the socket in
// `g_stream_client`; Poll() emits one frame every kStreamIntervalMs and runs
// `g_server.handleClient()` in between so every other route works while the
// video is playing.
void HandleStream() {
  if (g_stream_client && g_stream_client.connected()) {
    g_stream_client.stop();
  }
  g_stream_client = g_server.client();
  g_stream_client.setNoDelay(true);
  g_stream_client.print(
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
      "Cache-Control: no-store\r\n"
      "Connection: close\r\n\r\n");
}

void PumpStreamFrame() {
  if (!g_stream_client || !g_stream_client.connected()) {
    return;
  }
  const uint32_t now = millis();
  if (now - g_last_stream_ms < kStreamIntervalMs) {
    return;
  }
  g_last_stream_ms = now;

  uint8_t* jpg = nullptr;
  size_t jpg_len = 0;
  if (!GrabAnnotatedJpeg(&jpg, &jpg_len)) {
    g_stream_client.stop();
    return;
  }

  g_stream_client.printf(
      "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
      static_cast<unsigned>(jpg_len));
  const size_t written = g_stream_client.write(jpg, jpg_len);
  g_stream_client.print("\r\n");
  free(jpg);
  if (written != jpg_len) {
    g_stream_client.stop();
  }
}

void HandleVision() {
  if (g_vision == nullptr) {
    g_server.send(503, "text/plain; charset=utf-8", "vision handler not wired");
    return;
  }
  if (g_stream_client && g_stream_client.connected()) {
    g_stream_client.stop();
  }
  const String q = g_server.arg("q");
  String out;
  const bool ok = g_vision(q.c_str(), &out);
  g_server.send(ok ? 200 : 502, "text/plain; charset=utf-8",
                (ok ? "L " : "E ") + out);
}

}  // namespace

void SetSampleSink(SampleSink sink) { g_sink = sink; }

void SetVisionHandler(VisionHandler handler) { g_vision = handler; }

void SetLastSample(const TrackSample& s) { g_last_sample = s; }

void SetLinkStats(uint32_t rx_bytes, uint32_t commands, uint32_t last_rx_ms) {
  g_link_rx_bytes = rx_bytes;
  g_link_cmds = commands;
  g_link_last_rx_ms = last_rx_ms;
}

bool IsStreaming() { return g_stream_client && g_stream_client.connected(); }

void StopStream() {
  if (g_stream_client) {
    g_stream_client.stop();
  }
}

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
  PumpStreamFrame();
}

}  // namespace cam_web
