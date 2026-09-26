#include "servo_web_server.h"

#include <WiFi.h>
#include <esp_log.h>
#include "servo_controller.h"
#include "cam_link.h"
#include "display.h"

extern std::unique_ptr<Display> g_display;

[[maybe_unused]] static const char* TAG = "ServoWebServer";

// The page lives in web/servo_index.html and is gzipped into servo_index_html.h at build time
// (tools/gen_web_page.py). 13 KB raw is too much to push through TCP on this heap; 3.5 KB is not.
#include "servo_index_html.h"

ServoWebServer::ServoWebServer() {}

ServoWebServer::~ServoWebServer() {
  Stop();
}

ServoWebServer& ServoWebServer::GetInstance() {
  static ServoWebServer instance;
  return instance;
}

void ServoWebServer::SetupRoutes() {
  if (!server_) {
    server_ = std::make_unique<WebServer>(80);
  }

  server_->on("/", HTTP_GET, [this]() { HandleRoot(); });
  server_->on("/servo", HTTP_GET, [this]() { HandleRoot(); });
  server_->on("/api/status", HTTP_GET, [this]() { HandleApiStatus(); });
  server_->on("/api/servo", HTTP_GET, [this]() { HandleApiServo(); });
  server_->on("/api/servo", HTTP_POST, [this]() { HandleApiServo(); });
  server_->on("/api/center", HTTP_GET, [this]() { HandleApiCenter(); });
  server_->on("/api/sweep", HTTP_GET, [this]() { HandleApiSweep(); });
  server_->on("/api/bobble", HTTP_GET, [this]() { HandleApiBobble(); });
  server_->on("/api/bobble", HTTP_POST, [this]() { HandleApiBobble(); });
  server_->on("/api/ui_mode", HTTP_GET, [this]() { HandleApiUiMode(); });
  server_->on("/api/ui_mode", HTTP_POST, [this]() { HandleApiUiMode(); });
  server_->on("/api/volume", HTTP_GET, [this]() { HandleApiVolume(); });
  server_->on("/api/track", HTTP_GET, [this]() { HandleApiTrack(); });
  // POST only: it makes her speak, so it should not fire from a prefetch or a pasted link.
  server_->on("/api/say", HTTP_POST, [this]() { HandleApiSay(); });
  server_->on("/api/speed", HTTP_GET, [this]() { HandleApiSpeed(); });
  server_->on("/api/anim", HTTP_GET, [this]() { HandleApiAnim(); });
  server_->onNotFound([this]() { HandleNotFound(); });
}

void ServoWebServer::Start() {
  if (running_) {
    return;
  }

  SetupRoutes();
  server_->begin();

  // No mDNS responder: it costs its own task plus several KB of buffers, which this no-PSRAM board
  // cannot spare while a voice session is live. Open the page by IP instead.
  running_ = true;
  ESP_LOGI(TAG, "Servo Web Server started on port 80 (IP: %s)", WiFi.localIP().toString().c_str());
}

void ServoWebServer::Stop() {
  if (!running_) {
    return;
  }
  if (server_) {
    server_->stop();
    server_->close();
    server_.reset();
  }
  running_ = false;
  ESP_LOGI(TAG, "Servo Web Server stopped");
}

void ServoWebServer::HandleClient() {
  if (running_ && server_) {
    server_->handleClient();
  }
}

void ServoWebServer::HandleRoot() {
  server_->sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server_->sendHeader("Pragma", "no-cache");
  server_->sendHeader("Expires", "0");
  // Gzipped and streamed straight from flash (send_P); every browser accepts gzip.
  server_->sendHeader("Content-Encoding", "gzip");
  server_->send_P(200, "text/html", reinterpret_cast<PGM_P>(kServoIndexHtmlGz), sizeof(kServoIndexHtmlGz));
}

void ServoWebServer::HandleApiStatus() {
  auto& controller = ServoController::GetInstance();
  float a0 = controller.GetAngle(0);
  float a25 = controller.GetAngle(25);
  float a26 = controller.GetAngle(26);
  uint32_t free_heap = esp_get_free_heap_size();

  char json[320];
  const char* mode_str = (g_display && g_display->GetUiMode() == Display::UiMode::kRobotFace) ? "face" : "chat";
  const int volume = hooks_.get_volume ? hooks_.get_volume() : -1;
  auto& cam = CamLink::GetInstance();
  snprintf(json, sizeof(json),
           "{\"servo0\":%.1f,\"servo25\":%.1f,\"servo26\":%.1f,\"servo15\":%.1f,\"ui_mode\":\"%s\",\"ip\":\"%s\",\"heap\":%lu,"
           "\"volume\":%d,\"tracking\":%s,\"cam\":%s}",
           a0, a25, a26, a26, mode_str, WiFi.localIP().toString().c_str(), static_cast<unsigned long>(free_heap),
           volume, cam.tracking_enabled() ? "true" : "false", cam.IsPresent() ? "true" : "false");

  server_->sendHeader("Access-Control-Allow-Origin", "*");
  server_->send(200, "application/json", json);
}

void ServoWebServer::HandleApiServo() {
  if (!server_->hasArg("pin") || !server_->hasArg("angle")) {
    server_->send(400, "application/json", "{\"error\":\"Missing pin or angle\"}");
    return;
  }

  int pin = server_->arg("pin").toInt();
  float angle = server_->arg("angle").toFloat();

  // Glide rather than jump: a typed value like 70 -> 45 would otherwise slam the servo there.
  ServoController::GetInstance().GlideTo(pin, angle);

  char json[128];
  snprintf(json, sizeof(json), "{\"success\":true,\"pin\":%d,\"angle\":%.1f}", pin, angle);
  server_->sendHeader("Access-Control-Allow-Origin", "*");
  server_->send(200, "application/json", json);
}

void ServoWebServer::HandleApiCenter() {
  auto& servo = ServoController::GetInstance();
  servo.GlideTo(ServoController::kPinServo0, ServoController::kDefaultAngle);
  servo.GlideTo(ServoController::kPinServo1, ServoController::kDefaultAngle);
  servo.GlideTo(ServoController::kPinServo2, ServoController::kServo2DefaultAngle);
  server_->sendHeader("Access-Control-Allow-Origin", "*");
  server_->send(200, "application/json", "{\"success\":true,\"action\":\"center\"}");
}

void ServoWebServer::HandleApiSweep() {
  server_->sendHeader("Access-Control-Allow-Origin", "*");
  server_->send(200, "application/json", "{\"success\":true,\"action\":\"sweep\"}");
  ServoController::GetInstance().TriggerSweepTest();
}

void ServoWebServer::HandleApiBobble() {
  server_->sendHeader("Access-Control-Allow-Origin", "*");
  server_->send(200, "application/json", "{\"success\":true,\"action\":\"head_bobble\"}");
  ServoController::GetInstance().TriggerHeadBobble();
}

void ServoWebServer::HandleApiUiMode() {
  if (!g_display) {
    server_->send(500, "application/json", "{\"error\":\"Display not ready\"}");
    return;
  }

  if (server_->hasArg("mode")) {
    String m = server_->arg("mode");
    if (m == "face") {
      g_display->SetUiMode(Display::UiMode::kRobotFace);
    } else if (m == "chat" || m == "text") {
      g_display->SetUiMode(Display::UiMode::kChatText);
    } else if (m == "toggle") {
      g_display->ToggleUiMode();
    }
  } else if (server_->hasArg("action") && server_->arg("action") == "toggle") {
    g_display->ToggleUiMode();
  }

  bool is_face = (g_display->GetUiMode() == Display::UiMode::kRobotFace);
  char json[128];
  snprintf(json, sizeof(json), "{\"success\":true,\"mode\":\"%s\"}", is_face ? "face" : "chat");
  server_->sendHeader("Access-Control-Allow-Origin", "*");
  server_->send(200, "application/json", json);
}

void ServoWebServer::HandleApiVolume() {
  server_->sendHeader("Access-Control-Allow-Origin", "*");
  if (!hooks_.get_volume || !hooks_.set_volume) {
    server_->send(503, "application/json", "{\"error\":\"volume not available\"}");
    return;
  }
  if (server_->hasArg("value")) {
    hooks_.set_volume(server_->arg("value").toInt());
  }
  char json[48];
  snprintf(json, sizeof(json), "{\"volume\":%d}", hooks_.get_volume());
  server_->send(200, "application/json", json);
}

void ServoWebServer::HandleApiTrack() {
  auto& cam = CamLink::GetInstance();
  if (server_->hasArg("on")) {
    cam.SetTrackingEnabled(server_->arg("on") == "1");
  }
  if (server_->hasArg("flip")) {
    const String axis = server_->arg("flip");
    if (axis == "yaw" || axis == "pitch" || axis == "roll") cam.FlipTrackDir(axis[0]);
  }
  if (server_->hasArg("reset_dir")) {
    cam.ResetTrackDirs();
  }
  char json[128];
  snprintf(json, sizeof(json), "{\"tracking\":%s,\"cam\":%s,\"yaw_dir\":%d,\"pitch_dir\":%d,\"roll_dir\":%d}",
           cam.tracking_enabled() ? "true" : "false", cam.IsPresent() ? "true" : "false", cam.yaw_dir(),
           cam.pitch_dir(), cam.roll_dir());
  server_->sendHeader("Access-Control-Allow-Origin", "*");
  server_->send(200, "application/json", json);
}

void ServoWebServer::HandleApiSay() {
  if (!hooks_.send_text) {
    server_->send(503, "application/json", "{\"ok\":false,\"error\":\"not available\"}");
    return;
  }
  String text = server_->arg("text");
  text.trim();
  // A few short sentences at most; this goes down the wake-word channel.
  if (text.length() == 0 || text.length() > 120) {
    server_->send(400, "application/json", "{\"ok\":false,\"error\":\"text must be 1-120 bytes\"}");
    return;
  }
  const char* why = hooks_.send_text(text);
  if (why != nullptr) {
    char json[160];
    snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", why);
    server_->send(409, "application/json", json);
    return;
  }
  server_->send(200, "application/json", "{\"ok\":true}");
}

// /api/speed                       -> current speeds
// /api/speed?axis=0..2&value=deg_s -> set one axis (live, not yet saved)
// /api/speed?test=0..2             -> there-and-back test move on that axis
// /api/speed?save=1                -> persist all three to NVS
void ServoWebServer::HandleApiSpeed() {
  auto& servo = ServoController::GetInstance();
  if (server_->hasArg("axis") && server_->hasArg("value")) {
    servo.SetAxisSpeed(server_->arg("axis").toInt(), server_->arg("value").toFloat());
  }
  if (server_->hasArg("test")) {
    servo.TriggerAxisTest(server_->arg("test").toInt());
  }
  if (server_->hasArg("save")) {
    servo.SaveAxisSpeeds();
  }
  char json[96];
  snprintf(json, sizeof(json), "{\"speed\":[%.0f,%.0f,%.0f]}", servo.AxisSpeed(0), servo.AxisSpeed(1),
           servo.AxisSpeed(2));
  server_->sendHeader("Access-Control-Allow-Origin", "*");
  server_->send(200, "application/json", json);
}

// /api/anim              -> {"anims":[{"name":..,"label":..,"idle":..},...],"playing":bool}
// /api/anim?name=<id>    -> play that animation (queued smoothly if another one is playing)
// /api/anim?random=1     -> play a random idle animation
// /api/anim?stop=1       -> stop whatever is playing (head stays where it is)
void ServoWebServer::HandleApiAnim() {
  auto& servo = ServoController::GetInstance();
  server_->sendHeader("Access-Control-Allow-Origin", "*");
  if (server_->hasArg("stop")) {
    servo.StopAnimation();
    server_->send(200, "application/json", "{\"ok\":true}");
    return;
  }
  if (server_->hasArg("auto")) {
    if (hooks_.set_head_random) hooks_.set_head_random(server_->arg("auto") == "1");
    char json[48];
    snprintf(json, sizeof(json), "{\"ok\":true,\"auto\":%s}",
             (hooks_.get_head_random && hooks_.get_head_random()) ? "true" : "false");
    server_->send(200, "application/json", json);
    return;
  }
  if (server_->hasArg("name") || server_->hasArg("random")) {
    bool ok;
    if (server_->hasArg("name")) {
      const int index = ServoController::FindAnimation(server_->arg("name").c_str());
      if (index < 0) {
        server_->send(404, "application/json", "{\"ok\":false,\"error\":\"unknown animation\"}");
        return;
      }
      ok = servo.PlayAnimation(index);
      if (!ok) {  // a non-keyframe gesture (bobble / sweep / test) is running: end it, then play
        servo.StopAnimation();
        ok = servo.PlayAnimation(index);
      }
    } else {
      ok = servo.PlayRandomIdle();
      if (!ok) {
        servo.StopAnimation();
        ok = servo.PlayRandomIdle();
      }
    }
    server_->send(ok ? 200 : 409, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"busy\"}");
    return;
  }
  // Names and labels are compile-time constants (no quotes / backslashes), so no escaping needed.
  // Static, not on the stack: loopTask has under 1 KB of stack headroom. send_P streams it without
  // the heap String copy that send() makes.
  static char json[1100];
  int len = snprintf(json, sizeof(json), "{\"anims\":[");
  for (int i = 0; i < ServoController::AnimationCount() && len < static_cast<int>(sizeof(json)) - 96; ++i) {
    len += snprintf(json + len, sizeof(json) - len, "%s{\"name\":\"%s\",\"label\":\"%s\",\"idle\":%s}",
                    i ? "," : "", ServoController::AnimationName(i), ServoController::AnimationLabel(i),
                    ServoController::AnimationIsIdle(i) ? "true" : "false");
  }
  len += snprintf(json + len, sizeof(json) - len, "],\"playing\":%s,\"auto\":%s}", servo.IsAnimating() ? "true" : "false",
                  (hooks_.get_head_random && hooks_.get_head_random()) ? "true" : "false");
  server_->send_P(200, "application/json", json, len);
}

void ServoWebServer::HandleNotFound() {
  server_->sendHeader("Location", "/");
  server_->send(302, "text/plain", "");
}
