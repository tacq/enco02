#include "servo_web_server.h"

#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_log.h>
#include "servo_controller.h"

[[maybe_unused]] static const char* TAG = "ServoWebServer";

static const char kServoIndexHtml[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
  <title>ENCO-02 舵机调试控制台</title>
  <style>
    :root {
      --bg: #0b0f19;
      --card-bg: rgba(22, 27, 44, 0.85);
      --border: rgba(56, 189, 248, 0.2);
      --primary: #38bdf8;
      --primary-glow: rgba(56, 189, 248, 0.4);
      --accent: #f43f5e;
      --text: #f1f5f9;
      --text-muted: #94a3b8;
      --green: #10b981;
    }
    * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
    body {
      background-color: var(--bg);
      color: var(--text);
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      align-items: center;
      padding: 20px 16px 40px;
      background-image: radial-gradient(circle at 50% 0%, #1e293b 0%, #0b0f19 75%);
    }
    .container {
      width: 100%;
      max-width: 480px;
      display: flex;
      flex-direction: column;
      gap: 18px;
    }
    .header {
      text-align: center;
      padding: 10px 0;
    }
    .header h1 {
      font-size: 22px;
      font-weight: 700;
      letter-spacing: 1px;
      color: #fff;
      text-shadow: 0 0 15px var(--primary-glow);
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
    }
    .header p {
      font-size: 13px;
      color: var(--text-muted);
      margin-top: 4px;
    }
    .badge {
      display: inline-block;
      padding: 2px 8px;
      border-radius: 9999px;
      font-size: 11px;
      font-weight: 600;
      background: rgba(16, 185, 129, 0.2);
      color: var(--green);
      border: 1px solid rgba(16, 185, 129, 0.4);
    }
    .card {
      background: var(--card-bg);
      border: 1px solid var(--border);
      border-radius: 16px;
      padding: 18px;
      box-shadow: 0 10px 30px rgba(0, 0, 0, 0.5);
      backdrop-filter: blur(12px);
      -webkit-backdrop-filter: blur(12px);
    }
    .card-title {
      font-size: 15px;
      font-weight: 600;
      display: flex;
      justify-content: space-between;
      align-items: center;
      margin-bottom: 14px;
    }
    .angle-val {
      font-size: 20px;
      font-weight: 700;
      color: var(--primary);
      text-shadow: 0 0 10px var(--primary-glow);
    }
    .slider-container {
      position: relative;
      margin: 15px 0 20px;
    }
    input[type=range] {
      -webkit-appearance: none;
      width: 100%;
      height: 10px;
      border-radius: 5px;
      background: #1e293b;
      outline: none;
      box-shadow: inset 0 1px 3px rgba(0,0,0,0.5);
    }
    input[type=range]::-webkit-slider-thumb {
      -webkit-appearance: none;
      appearance: none;
      width: 28px;
      height: 28px;
      border-radius: 50%;
      background: #fff;
      border: 3px solid var(--primary);
      cursor: pointer;
      box-shadow: 0 0 12px var(--primary-glow);
      transition: transform 0.1s ease;
    }
    input[type=range]::-webkit-slider-thumb:active {
      transform: scale(1.15);
    }
    .scale-labels {
      display: flex;
      justify-content: space-between;
      font-size: 11px;
      color: var(--text-muted);
      margin-top: 6px;
    }
    .btn-group {
      display: grid;
      grid-template-columns: repeat(5, 1fr);
      gap: 6px;
      margin-top: 10px;
    }
    .btn {
      background: #1e293b;
      border: 1px solid rgba(255, 255, 255, 0.08);
      color: var(--text);
      padding: 8px 4px;
      border-radius: 8px;
      font-size: 12px;
      font-weight: 500;
      cursor: pointer;
      text-align: center;
      transition: all 0.15s ease;
    }
    .btn:active {
      transform: scale(0.95);
      background: var(--primary);
      color: #0b0f19;
      border-color: var(--primary);
    }
    .action-row {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 12px;
    }
    .btn-action {
      background: #1e293b;
      border: 1px solid var(--border);
      color: #fff;
      padding: 12px;
      border-radius: 12px;
      font-size: 14px;
      font-weight: 600;
      cursor: pointer;
      display: flex;
      align-items: center;
      justify-content: center;
      gap: 6px;
      transition: all 0.15s ease;
    }
    .btn-action.primary {
      background: linear-gradient(135deg, #0284c7, #0369a1);
      border: none;
      box-shadow: 0 4px 15px rgba(2, 132, 199, 0.35);
    }
    .btn-action:active {
      transform: scale(0.97);
    }
    .info-box {
      font-size: 12px;
      color: var(--text-muted);
      line-height: 1.6;
      display: flex;
      flex-direction: column;
      gap: 4px;
    }
    .info-box span {
      color: var(--text);
      font-family: monospace;
    }
  </style>
</head>
<body>
  <div class="container">
    <div class="header">
      <h1>🤖 ENCO-02 舵机调试</h1>
      <p>双轴 MG92B 模拟相机追踪控制台 <span class="badge" id="net-badge">在线</span></p>
    </div>

    <!-- Servo 0: PIN 0 -->
    <div class="card">
      <div class="card-title">
        <span>📍 舵机 1：GPIO 0 (俯仰 / Pitch)</span>
        <span class="angle-val" id="val0">90°</span>
      </div>
      <p style="font-size:11px;color:var(--primary);margin-top:-6px;margin-bottom:8px;">安全限位：40° ~ 120° (角大低头，角小抬头)</p>
      <div class="slider-container">
        <input type="range" id="slider0" min="40" max="120" value="90" oninput="onSliderInput(0, this.value)">
        <div class="scale-labels">
          <span>40° (抬头)</span>
          <span>60°</span>
          <span>90° (居中)</span>
          <span>105°</span>
          <span>120° (低头)</span>
        </div>
      </div>
      <div class="btn-group">
        <button class="btn" onclick="setAngle(0, 40)">40° 抬头</button>
        <button class="btn" onclick="setAngle(0, 60)">60°</button>
        <button class="btn" onclick="setAngle(0, 90)">90° 居中</button>
        <button class="btn" onclick="setAngle(0, 105)">105°</button>
        <button class="btn" onclick="setAngle(0, 120)">120° 低头</button>
      </div>
    </div>

    <!-- Servo 1: PIN 25 -->
    <div class="card">
      <div class="card-title">
        <span>📍 舵机 2：GPIO 25 (偏侧 / Roll & Tilt)</span>
        <span class="angle-val" id="val25">90°</span>
      </div>
      <p style="font-size:11px;color:var(--primary);margin-top:-6px;margin-bottom:8px;">安全限位：50° ~ 110° (角大右下歪，角小左下歪)</p>
      <div class="slider-container">
        <input type="range" id="slider25" min="50" max="110" value="90" oninput="onSliderInput(25, this.value)">
        <div class="scale-labels">
          <span>50° (向左歪)</span>
          <span>70°</span>
          <span>90° (居中)</span>
          <span>100°</span>
          <span>110° (向右歪)</span>
        </div>
      </div>
      <div class="btn-group">
        <button class="btn" onclick="setAngle(25, 50)">50° 左歪</button>
        <button class="btn" onclick="setAngle(25, 70)">70°</button>
        <button class="btn" onclick="setAngle(25, 90)">90° 居中</button>
        <button class="btn" onclick="setAngle(25, 100)">100°</button>
        <button class="btn" onclick="setAngle(25, 110)">110° 右歪</button>
      </div>
    </div>

    <!-- Quick Global Actions -->
    <div class="action-row" style="grid-template-columns: 1fr 1.2fr 1fr;">
      <button class="btn-action primary" onclick="centerAll()">🎯 归中 (90°)</button>
      <button class="btn-action" style="background: linear-gradient(135deg, #ec4899, #8b5cf6); border: none; box-shadow: 0 4px 15px rgba(236, 72, 153, 0.35);" onclick="runHeadBobble()">🕺 摇头晃脑</button>
      <button class="btn-action" onclick="runSweep()">🔄 巡航测试</button>
    </div>

    <!-- Diagnostics Info -->
    <div class="card info-box">
      <div>设备 IP: <span id="info-ip">加载中...</span></div>
      <div>局域网域名: <span>http://enco02.local</span></div>
      <div>可用内存: <span id="info-heap">--</span> KB</div>
    </div>
  </div>

  <script>
    let sendTimer = null;
    let pendingUpdates = {};

    function updateUiValues(pin, angle) {
      if (pin === 0) {
        document.getElementById('slider0').value = angle;
        document.getElementById('val0').innerText = angle + '°';
      } else if (pin === 25) {
        document.getElementById('slider25').value = angle;
        document.getElementById('val25').innerText = angle + '°';
      }
    }

    function onSliderInput(pin, val) {
      updateUiValues(pin, val);
      pendingUpdates[pin] = val;
      if (!sendTimer) {
        sendTimer = setTimeout(flushUpdates, 30);
      }
    }

    function flushUpdates() {
      sendTimer = null;
      for (const pin in pendingUpdates) {
        const angle = pendingUpdates[pin];
        delete pendingUpdates[pin];
        fetch('/api/servo?pin=' + pin + '&angle=' + angle).catch(e => console.error(e));
      }
    }

    function setAngle(pin, angle) {
      updateUiValues(pin, angle);
      fetch('/api/servo?pin=' + pin + '&angle=' + angle).catch(e => console.error(e));
    }

    function centerAll() {
      updateUiValues(0, 90);
      updateUiValues(25, 90);
      fetch('/api/center').catch(e => console.error(e));
    }

    function runHeadBobble() {
      fetch('/api/bobble').catch(e => console.error(e));
      setTimeout(syncStatus, 2200);
    }

    function runSweep() {
      fetch('/api/sweep').catch(e => console.error(e));
      setTimeout(syncStatus, 1500);
    }

    function syncStatus() {
      fetch('/api/status')
        .then(r => r.json())
        .then(data => {
          updateUiValues(0, Math.round(data.servo0));
          updateUiValues(25, Math.round(data.servo25));
          document.getElementById('info-ip').innerText = data.ip;
          document.getElementById('info-heap').innerText = Math.round(data.heap / 1024);
        })
        .catch(e => {
          document.getElementById('net-badge').innerText = '未连接';
          document.getElementById('net-badge').style.color = '#f43f5e';
        });
    }

    syncStatus();
  </script>
</body>
</html>
)rawliteral";

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
  server_->onNotFound([this]() { HandleNotFound(); });
}

void ServoWebServer::Start() {
  if (running_) {
    return;
  }

  SetupRoutes();
  server_->begin();

  if (MDNS.begin("enco02")) {
    MDNS.addService("http", "tcp", 80);
    ESP_LOGI(TAG, "mDNS responder started: http://enco02.local");
  } else {
    ESP_LOGW(TAG, "mDNS responder failed to start");
  }

  running_ = true;
  ESP_LOGI(TAG, "Servo Web Server started on port 80 (IP: %s)", WiFi.localIP().toString().c_str());
}

void ServoWebServer::Stop() {
  if (!running_) {
    return;
  }
  if (server_) {
    server_->stop();
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
  server_->sendHeader("Cache-Control", "no-cache");
  server_->send(200, "text/html", kServoIndexHtml);
}

void ServoWebServer::HandleApiStatus() {
  auto& controller = ServoController::GetInstance();
  float a0 = controller.GetAngle(0);
  float a25 = controller.GetAngle(25);
  uint32_t free_heap = esp_get_free_heap_size();

  char json[256];
  snprintf(json, sizeof(json),
           "{\"servo0\":%.1f,\"servo25\":%.1f,\"ip\":\"%s\",\"heap\":%lu}",
           a0, a25, WiFi.localIP().toString().c_str(), static_cast<unsigned long>(free_heap));

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

  ServoController::GetInstance().SetAngle(pin, angle);

  char json[128];
  snprintf(json, sizeof(json), "{\"success\":true,\"pin\":%d,\"angle\":%.1f}", pin, angle);
  server_->sendHeader("Access-Control-Allow-Origin", "*");
  server_->send(200, "application/json", json);
}

void ServoWebServer::HandleApiCenter() {
  ServoController::GetInstance().CenterAll();
  server_->sendHeader("Access-Control-Allow-Origin", "*");
  server_->send(200, "application/json", "{\"success\":true,\"action\":\"center\"}");
}

void ServoWebServer::HandleApiSweep() {
  server_->sendHeader("Access-Control-Allow-Origin", "*");
  server_->send(200, "application/json", "{\"success\":true,\"action\":\"sweep\"}");
  ServoController::GetInstance().RunSweepTest();
}

void ServoWebServer::HandleApiBobble() {
  server_->sendHeader("Access-Control-Allow-Origin", "*");
  server_->send(200, "application/json", "{\"success\":true,\"action\":\"head_bobble\"}");
  ServoController::GetInstance().TriggerHeadBobble();
}

void ServoWebServer::HandleNotFound() {
  server_->sendHeader("Location", "/");
  server_->send(302, "text/plain", "");
}
