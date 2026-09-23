#include "web_wifi_configurator.h"

#include <Preferences.h>
#include <esp_wifi.h>

namespace {
constexpr char kPreferenceKey[] = "WiFiConnector";

const char kIndexHtmlHeader[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ENCO-02 小智 配网</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; background: #0f172a; color: #f8fafc; padding: 24px 16px; margin: 0; }
    .card { max-width: 400px; margin: 0 auto; background: #1e293b; border-radius: 16px; padding: 24px; box-shadow: 0 10px 25px rgba(0,0,0,0.5); border: 1px solid #334155; }
    h2 { margin-top: 0; color: #38bdf8; font-size: 22px; text-align: center; }
    p { color: #94a3b8; font-size: 14px; text-align: center; margin-bottom: 20px; }
    label { display: block; font-size: 13px; font-weight: 600; margin-bottom: 6px; color: #cbd5e1; }
    select, input[type="text"], input[type="password"] { width: 100%; box-sizing: border-box; padding: 12px; background: #0f172a; border: 1px solid #475569; border-radius: 8px; color: #fff; font-size: 15px; margin-bottom: 16px; outline: none; }
    select:focus, input:focus { border-color: #38bdf8; }
    button { width: 100%; padding: 14px; background: #0284c7; color: #fff; border: none; border-radius: 8px; font-size: 16px; font-weight: 600; cursor: pointer; transition: background 0.2s; }
    button:hover { background: #0369a1; }
  </style>
</head>
<body>
  <div class="card">
    <h2>🤖 ENCO-02 Wi-Fi 设置</h2>
    <p>请选择您的 2.4GHz Wi-Fi 并输入密码</p>
    <form action="/save" method="POST">
      <label for="ssid">Wi-Fi 名称 (SSID)</label>
      <select name="ssid" id="ssid">
)rawliteral";

const char kIndexHtmlFooter[] PROGMEM = R"rawliteral(
      </select>
      <label for="manual_ssid">或者手动输入 SSID</label>
      <input type="text" id="manual_ssid" name="manual_ssid" placeholder="手动输入 Wi-Fi 名称">
      <label for="password">Wi-Fi 密码</label>
      <input type="password" id="password" name="password" placeholder="请输入密码">
      <button type="submit">保存并连接</button>
    </form>
  </div>
</body>
</html>
)rawliteral";

const char kSuccessHtml[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>配网成功</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, sans-serif; background: #0f172a; color: #f8fafc; display: flex; align-items: center; justify-content: center; height: 100vh; margin: 0; }
    .card { background: #1e293b; border-radius: 16px; padding: 32px 24px; text-align: center; max-width: 360px; box-shadow: 0 10px 25px rgba(0,0,0,0.5); border: 1px solid #334155; }
    h2 { color: #4ade80; margin: 0 0 12px 0; }
    p { color: #94a3b8; font-size: 14px; line-height: 1.6; }
  </style>
</head>
<body>
  <div class="card">
    <h2>✓ 配置成功</h2>
    <p>Wi-Fi 凭据已保存至 ENCO-02。<br>机器人正在尝试连接网络，请观察屏幕状态！</p>
  </div>
</body>
</html>
)rawliteral";

}  // namespace

WebWifiConfigurator::WebWifiConfigurator() {}

WebWifiConfigurator::~WebWifiConfigurator() {
  Stop();
}

String WebWifiConfigurator::StartApAndServer() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char ap_name[32];
  snprintf(ap_name, sizeof(ap_name), "xiaozhi-%02X%02X", mac[4], mac[5]);
  ap_ssid_ = String(ap_name);

  printf("[WebWifi] Starting SoftAP: %s\n", ap_ssid_.c_str());
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(ap_ssid_.c_str());

  IPAddress myIP = WiFi.softAPIP();
  printf("[WebWifi] AP IP address: %s\n", myIP.toString().c_str());

  server_.on("/", HTTP_GET, [this]() { HandleRoot(); });
  server_.on("/save", HTTP_POST, [this]() { HandleSave(); });
  server_.onNotFound([this]() { HandleNotFound(); });
  server_.begin();

  printf("[WebWifi] HTTP server started on port 80\n");
  configured_ = false;
  running_ = true;
  return ap_ssid_;
}

void WebWifiConfigurator::HandleClient() {
  if (running_) {
    server_.handleClient();
  }
}

void WebWifiConfigurator::Stop() {
  if (!running_) {
    return;
  }
  running_ = false;
  server_.stop();
  server_.close();
  WiFi.scanDelete();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  ap_ssid_ = String();
  ssid_ = String();
  password_ = String();
  printf("[WebWifi] Provisioning server and SoftAP stopped\n");
}

void WebWifiConfigurator::HandleRoot() {
  String page = FPSTR(kIndexHtmlHeader);

  int n = WiFi.scanNetworks();
  if (n == 0) {
    page += "<option value=\"\">未扫描到网络</option>";
  } else {
    for (int i = 0; i < n; ++i) {
      String ssid = WiFi.SSID(i);
      if (ssid.length() > 0) {
        page += "<option value=\"" + ssid + "\">" + ssid + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
      }
    }
  }
  WiFi.scanDelete();

  page += FPSTR(kIndexHtmlFooter);
  server_.send(200, "text/html", page);
}

void WebWifiConfigurator::HandleSave() {
  String selected_ssid = server_.arg("ssid");
  String manual_ssid = server_.arg("manual_ssid");
  String pass = server_.arg("password");

  String final_ssid = (manual_ssid.length() > 0) ? manual_ssid : selected_ssid;

  if (final_ssid.length() > 0) {
    ssid_ = final_ssid;
    password_ = pass;

    printf("[WebWifi] Saving credentials: SSID='%s'\n", ssid_.c_str());

    Preferences prefs;
    prefs.begin(kPreferenceKey, false);
    prefs.putString("ssid", ssid_.c_str());
    prefs.putString("password", password_.c_str());
    prefs.end();

    server_.send(200, "text/html", FPSTR(kSuccessHtml));
    configured_ = true;
  } else {
    server_.send(400, "text/plain", "SSID cannot be empty");
  }
}

void WebWifiConfigurator::HandleNotFound() {
  server_.sendHeader("Location", "/");
  server_.send(302, "text/plain", "");
}
