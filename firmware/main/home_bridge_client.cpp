#include "home_bridge_client.h"

#if __has_include("home_config.h")
#include "home_config.h"
#define HOME_BRIDGE_ENABLED 1
#else
#define HOME_BRIDGE_ENABLED 0
#endif

#if HOME_BRIDGE_ENABLED
#include <Arduino.h>
#include <WiFi.h>
#include <esp_system.h>
#include <mbedtls/md.h>

#include <cerrno>
#include <cstring>
#endif

namespace home_bridge {

namespace {
struct PoolTarget {
  const char* id;
  const char* name_zh;
  bool thermostat;
  int min_f;
  int max_f;
};
// Mirrors the allow-list in tools/home_bridge/pool_config.json (the bridge re-validates).
constexpr PoolTarget kPoolTargets[] = {
    {"pool_pump", "泳池水泵", false, 0, 0},  {"spa_pump", "SPA模式", false, 0, 0},
    {"pool_heater", "泳池加热", false, 0, 0}, {"spa_heater", "SPA加热", false, 0, 0},
    {"pool_light", "泳池灯", false, 0, 0},    {"spa_light", "SPA灯", false, 0, 0},
    {"cleaner", "清洁机", false, 0, 0},       {"pool_set", "泳池温度", true, 70, 90},
    {"spa_set", "SPA温度", true, 80, 104},
};

const PoolTarget* FindTarget(const std::string& id) {
  for (const auto& t : kPoolTargets) {
    if (id == t.id) return &t;
  }
  return nullptr;
}
}  // namespace

bool ValidatePoolSet(const std::string& target, const std::string& action, int value,
                     std::string& error) {
  const PoolTarget* t = FindTarget(target);
  if (t == nullptr) {
    error = "unknown target; use pool_pump|spa_pump|pool_heater|spa_heater|pool_light|spa_light|cleaner|pool_set|spa_set";
    return false;
  }
  if (t->thermostat) {
    if (action != "set") {
      error = "pool_set/spa_set need action=set with value in F";
      return false;
    }
    if (value < t->min_f || value > t->max_f) {
      error = std::string(t->name_zh) + " range " + std::to_string(t->min_f) + "-" +
              std::to_string(t->max_f) + "F";
      return false;
    }
    return true;
  }
  if (action != "on" && action != "off") {
    error = "switch targets need action=on|off";
    return false;
  }
  return true;
}

const char* PoolTargetNameZh(const std::string& target) {
  const PoolTarget* t = FindTarget(target);
  return t == nullptr ? "泳池设备" : t->name_zh;
}

#if HOME_BRIDGE_ENABLED
namespace {

constexpr int32_t kConnectTimeoutMs = 1500;
constexpr int32_t kRetryConnectTimeoutMs = 3000;
constexpr uint32_t kNonceReadTimeoutMs = 2000;
// The bridge may need a fresh round trip to the iAqualink cloud for the summary.
constexpr uint32_t kSummaryReadTimeoutMs = 6000;
// A change is applied, then the bridge polls the panel for up to ~6 s to confirm it.
constexpr uint32_t kSetReadTimeoutMs = 15000;
// Responses are a few hundred bytes; anything bigger is not ours.
constexpr size_t kMaxResponseBytes = 1536;

// Plain HTTP/1.1 over a LAN socket. No TLS: the request is authenticated by an HMAC over a
// single-use nonce, and nothing secret travels in either direction. Deliberately not HTTPClient:
// that pulls in a much larger buffer footprint than a two-line request needs on this heap.
bool HttpRequest(const char* method, const char* path, const std::string& extra_headers,
                 const std::string& req_body, uint32_t read_timeout_ms, int& status,
                 std::string& body) {
  WiFiClient client;
  // Two attempts: a Mac that has just woken (or whose Wi-Fi is dozing) often drops the first SYN,
  // and one 1.5 s try turned that into "连接不上家庭网关" every time.
  bool connected = false;
  for (int attempt = 0; attempt < 2 && !connected; ++attempt) {
    connected = client.connect(HOME_BRIDGE_HOST, HOME_BRIDGE_PORT, attempt == 0 ? kConnectTimeoutMs : kRetryConnectTimeoutMs);
    if (!connected) {
      printf("[home] connect %s:%d failed (attempt %d, errno %d, free %u)\n", HOME_BRIDGE_HOST,
             HOME_BRIDGE_PORT, attempt + 1, errno, static_cast<unsigned>(esp_get_free_heap_size()));
      client.stop();
    }
  }
  if (!connected) {
    return false;
  }
  std::string req = method;
  req += " ";
  req += path;
  req += " HTTP/1.1\r\nHost: " HOME_BRIDGE_HOST "\r\nConnection: close\r\n";
  req += extra_headers;
  if (!req_body.empty()) {
    req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(req_body.size()) +
           "\r\n";
  }
  req += "\r\n";
  req += req_body;
  client.write(reinterpret_cast<const uint8_t*>(req.data()), req.size());

  std::string raw;
  raw.reserve(512);
  const uint32_t deadline = millis() + read_timeout_ms;
  while (static_cast<int32_t>(deadline - millis()) > 0) {
    const int avail = client.available();
    if (avail > 0) {
      char buf[128];
      const int n = client.read(reinterpret_cast<uint8_t*>(buf), std::min<int>(avail, sizeof(buf)));
      if (n > 0) {
        raw.append(buf, n);
        if (raw.size() > kMaxResponseBytes) {
          client.stop();
          return false;
        }
      }
    } else if (!client.connected()) {
      break;
    } else {
      delay(10);
    }
  }
  client.stop();

  // "HTTP/1.0 200 OK\r\n...\r\n\r\n<body>"
  if (raw.size() < 12 || raw.compare(0, 5, "HTTP/") != 0) {
    return false;
  }
  status = atoi(raw.c_str() + 9);
  const size_t sep = raw.find("\r\n\r\n");
  body = sep == std::string::npos ? std::string() : raw.substr(sep + 4);
  return true;
}

std::string HmacHex(const std::string& msg) {
  unsigned char mac[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr ||
      mbedtls_md_hmac(info, reinterpret_cast<const unsigned char*>(HOME_BRIDGE_KEY), strlen(HOME_BRIDGE_KEY),
                      reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), mac) != 0) {
    return std::string();
  }
  static const char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (unsigned char b : mac) {
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}

void Trim(std::string& s) {
  while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == '\r' || s[i] == '\n' || s[i] == ' ')) ++i;
  s.erase(0, i);
}

// Nonce + HMAC-signed request; the reply body is "toast\n---\nspeech". Returns true on HTTP 200.
bool SignedTextRequest(const char* method, const char* path, const std::string& req_body,
                       uint32_t read_timeout_ms, std::string& toast, std::string& speech) {
  toast.clear();
  speech.clear();
  if (WiFi.status() != WL_CONNECTED) {
    speech = "设备未联网";
    return false;
  }

  int status = 0;
  std::string nonce;
  if (!HttpRequest("GET", "/nonce", std::string(), std::string(), kNonceReadTimeoutMs, status, nonce) ||
      status != 200) {
    speech = "连接不上家庭网关";
    return false;
  }
  Trim(nonce);
  if (nonce.size() != 32) {
    speech = "家庭网关响应异常";
    return false;
  }

  const std::string sig = HmacHex(nonce + "\n" + method + "\n" + path + "\n" + req_body);
  if (sig.empty()) {
    speech = "签名失败";
    return false;
  }
  std::string headers = "X-Enco-Nonce: " + nonce + "\r\nX-Enco-Sig: " + sig + "\r\n";

  std::string body;
  if (!HttpRequest(method, path, headers, req_body, read_timeout_ms, status, body)) {
    speech = "家庭网关超时";
    return false;
  }

  const size_t sep = body.find("\n---\n");
  if (sep != std::string::npos) {
    toast = body.substr(0, sep);
    speech = body.substr(sep + 5);
  }
  Trim(toast);
  Trim(speech);
  if (status != 200) {
    printf("[home] %s %s HTTP %d\n", method, path, status);
    if (status == 401) {
      speech = "家庭网关拒绝了请求";
    } else if (speech.empty()) {
      speech = "泳池系统暂时无法连接";
    }
    return false;
  }
  printf("[home] %s %s ok (%u bytes)\n", method, path, static_cast<unsigned>(body.size()));
  return !speech.empty();
}

}  // namespace

bool Configured() { return true; }

bool FetchPoolSummary(std::string& toast, std::string& speech) {
  return SignedTextRequest("GET", "/pool/summary", std::string(), kSummaryReadTimeoutMs, toast, speech);
}

bool PoolSet(const std::string& target, const std::string& action, int value, std::string& toast,
             std::string& speech) {
  std::string err;
  if (!ValidatePoolSet(target, action, value, err)) {
    toast.clear();
    speech = err;
    return false;
  }
  // target/action are allow-listed above, so no JSON escaping is needed.
  std::string body = "{\"target\":\"" + target + "\",\"action\":\"" + action + "\"";
  if (action == "set") {
    body += ",\"value\":" + std::to_string(value);
  }
  body += "}";
  return SignedTextRequest("POST", "/pool/set", body, kSetReadTimeoutMs, toast, speech);
}

#else  // !HOME_BRIDGE_ENABLED

bool Configured() { return false; }

bool FetchPoolSummary(std::string& toast, std::string& speech) {
  toast.clear();
  speech = "未配置家庭网关";
  return false;
}

bool PoolSet(const std::string&, const std::string&, int, std::string& toast, std::string& speech) {
  toast.clear();
  speech = "未配置家庭网关";
  return false;
}

#endif

}  // namespace home_bridge
