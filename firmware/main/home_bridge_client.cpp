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
#include <mbedtls/md.h>

#include <cstring>
#endif

namespace home_bridge {

#if HOME_BRIDGE_ENABLED
namespace {

constexpr int32_t kConnectTimeoutMs = 1500;
constexpr uint32_t kNonceReadTimeoutMs = 2000;
// The bridge may need a fresh round trip to the iAqualink cloud for the summary.
constexpr uint32_t kSummaryReadTimeoutMs = 6000;
// Responses are a few hundred bytes; anything bigger is not ours.
constexpr size_t kMaxResponseBytes = 1536;

// Plain HTTP/1.1 over a LAN socket. No TLS: the request is authenticated by an HMAC over a
// single-use nonce, and nothing secret travels in either direction. Deliberately not HTTPClient:
// that pulls in a much larger buffer footprint than a two-line request needs on this heap.
bool HttpGet(const char* path, const std::string& extra_headers, uint32_t read_timeout_ms,
             int& status, std::string& body) {
  WiFiClient client;
  if (!client.connect(HOME_BRIDGE_HOST, HOME_BRIDGE_PORT, kConnectTimeoutMs)) {
    printf("[home] connect %s:%d failed\n", HOME_BRIDGE_HOST, HOME_BRIDGE_PORT);
    return false;
  }
  std::string req = "GET ";
  req += path;
  req += " HTTP/1.1\r\nHost: " HOME_BRIDGE_HOST "\r\nConnection: close\r\n";
  req += extra_headers;
  req += "\r\n";
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

}  // namespace

bool Configured() { return true; }

bool FetchPoolSummary(std::string& toast, std::string& speech) {
  toast.clear();
  speech.clear();
  if (WiFi.status() != WL_CONNECTED) {
    speech = "设备未联网";
    return false;
  }

  int status = 0;
  std::string nonce;
  if (!HttpGet("/nonce", std::string(), kNonceReadTimeoutMs, status, nonce) || status != 200) {
    speech = "连接不上家庭网关";
    return false;
  }
  Trim(nonce);
  if (nonce.size() != 32) {
    speech = "家庭网关响应异常";
    return false;
  }

  static const char kPath[] = "/pool/summary";
  const std::string sig = HmacHex(nonce + "\nGET\n" + kPath + "\n");
  if (sig.empty()) {
    speech = "签名失败";
    return false;
  }
  std::string headers = "X-Enco-Nonce: " + nonce + "\r\nX-Enco-Sig: " + sig + "\r\n";

  std::string body;
  if (!HttpGet(kPath, headers, kSummaryReadTimeoutMs, status, body)) {
    speech = "家庭网关超时";
    return false;
  }
  if (status != 200) {
    printf("[home] summary HTTP %d\n", status);
    speech = status == 401 ? "家庭网关拒绝了请求" : "泳池系统暂时无法连接";
    return false;
  }

  const size_t sep = body.find("\n---\n");
  if (sep == std::string::npos) {
    speech = body;
  } else {
    toast = body.substr(0, sep);
    speech = body.substr(sep + 5);
  }
  Trim(toast);
  Trim(speech);
  printf("[home] pool summary ok (%u bytes)\n", static_cast<unsigned>(body.size()));
  return !speech.empty();
}

#else  // !HOME_BRIDGE_ENABLED

bool Configured() { return false; }

bool FetchPoolSummary(std::string& toast, std::string& speech) {
  toast.clear();
  speech = "未配置家庭网关";
  return false;
}

#endif

}  // namespace home_bridge
