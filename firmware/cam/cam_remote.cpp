#include "cam_remote.h"

#include <esp_random.h>
#include <mbedtls/md.h>
#include <string.h>

#include "cam_config.h"

namespace cam_remote {
namespace {

// The shared secret lives in the gitignored cam_config.h, and the same value in the tracker's
// chmod-600 .env. Never logged, never sent anywhere - only used as the HMAC key.
#ifdef CAM_TRACK_KEY
constexpr char kKey[] = CAM_TRACK_KEY;
#else
constexpr char kKey[] = "";
#endif
constexpr size_t kKeyLen = sizeof(kKey) - 1;
constexpr size_t kMinKeyLen = 32;

constexpr size_t kNonceBytes = 16;
constexpr size_t kMacBytes = 16;  // HMAC-SHA256 truncated to 128 bits
constexpr size_t kMacHexLen = kMacBytes * 2;
// "S 2147483647 -100 -100 100 -90 1 1" is 35 characters; leave headroom but no more.
constexpr size_t kMaxBodyLen = 48;

// A sample older than this means the tracker has stalled; the on-board finder takes over again.
constexpr uint32_t kActiveMs = 1500;

// The tracker sends one line per video frame, 15 a second at most. Anything much beyond that is not
// the tracker, and is refused before any HMAC is computed on it.
constexpr uint32_t kMaxLinesPerSecond = 40;

// A real tracker never sends a bad line. This many on one connection and it is closed.
constexpr uint32_t kDropAfterRejects = 20;

char g_nonce[kNonceBytes * 2 + 1] = {0};
bool g_session = false;
bool g_session_ok = false;
uint32_t g_last_seq = 0;
uint32_t g_session_rejects = 0;
uint32_t g_window_start_ms = 0;
uint32_t g_window_lines = 0;

uint32_t g_accepted = 0;
uint32_t g_rejected = 0;
uint32_t g_last_ok_ms = 0;
bool g_ever_ok = false;

int HexVal(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return -1;
}

bool Reject() {
  ++g_rejected;
  ++g_session_rejects;
  return false;
}

// One decimal field: optional '-', 1..10 digits, then exactly one space or the end of the string.
// Anything else - '+', leading spaces, hex, trailing junk - fails the whole line.
bool NextInt(const char*& p, int64_t lo, int64_t hi, int64_t* out) {
  bool neg = false;
  if (*p == '-') {
    neg = true;
    ++p;
  }
  if (*p < '0' || *p > '9') {
    return false;
  }
  int64_t v = 0;
  int digits = 0;
  while (*p >= '0' && *p <= '9') {
    if (++digits > 10) {
      return false;
    }
    v = v * 10 + (*p - '0');
    ++p;
  }
  if (*p == ' ') {
    ++p;
  } else if (*p != '\0') {
    return false;
  }
  if (neg) {
    v = -v;
  }
  if (v < lo || v > hi) {
    return false;
  }
  *out = v;
  return true;
}

}  // namespace

bool Enabled() { return kKeyLen >= kMinKeyLen; }

void BeginSession(char nonce_hex[33]) {
  uint8_t raw[kNonceBytes];
  // Hardware RNG; with Wi-Fi running it is fed by RF noise.
  esp_fill_random(raw, sizeof(raw));
  static const char kHex[] = "0123456789abcdef";
  for (size_t i = 0; i < kNonceBytes; ++i) {
    g_nonce[2 * i] = kHex[raw[i] >> 4];
    g_nonce[2 * i + 1] = kHex[raw[i] & 0x0F];
  }
  g_nonce[kNonceBytes * 2] = '\0';
  memcpy(nonce_hex, g_nonce, sizeof(g_nonce));
  g_session = true;
  g_session_ok = false;
  g_last_seq = 0;
  g_session_rejects = 0;
  g_window_start_ms = 0;
  g_window_lines = 0;
}

void EndSession() {
  g_session = false;
  g_session_ok = false;
  // Forget the nonce too: nothing can be verified against a closed connection.
  memset(g_nonce, 0, sizeof(g_nonce));
}

bool HandleLine(const char* line, uint32_t now_ms, TrackSample* out) {
  if (!Enabled() || !g_session) {
    return Reject();
  }
  // Rate limit first, so a flood costs a counter and not an HMAC per line.
  if (g_window_lines == 0 || now_ms - g_window_start_ms >= 1000) {
    g_window_start_ms = now_ms;
    g_window_lines = 0;
  }
  if (++g_window_lines > kMaxLinesPerSecond) {
    return Reject();
  }

  const size_t len = strlen(line);
  if (len < 3 + 1 + kMacHexLen || len > kMaxBodyLen + 1 + kMacHexLen) {
    return Reject();
  }
  const size_t body_len = len - kMacHexLen - 1;
  if (line[0] != 'S' || line[1] != ' ' || line[body_len] != ' ') {
    return Reject();
  }
  uint8_t given[kMacBytes];
  for (size_t i = 0; i < kMacBytes; ++i) {
    const int hi = HexVal(line[body_len + 1 + 2 * i]);
    const int lo = HexVal(line[body_len + 2 + 2 * i]);
    if (hi < 0 || lo < 0) {
      return Reject();
    }
    given[i] = static_cast<uint8_t>((hi << 4) | lo);
  }

  // HMAC over "<nonce> <body>", binding the line to this connection.
  char msg[kNonceBytes * 2 + 1 + kMaxBodyLen];
  memcpy(msg, g_nonce, kNonceBytes * 2);
  msg[kNonceBytes * 2] = ' ';
  memcpy(msg + kNonceBytes * 2 + 1, line, body_len);
  uint8_t mac[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (info == nullptr ||
      mbedtls_md_hmac(info, reinterpret_cast<const unsigned char*>(kKey), kKeyLen,
                      reinterpret_cast<const unsigned char*>(msg), kNonceBytes * 2 + 1 + body_len, mac) != 0) {
    return Reject();
  }
  // Constant time: how many leading bytes matched must not show in how long this takes.
  uint8_t diff = 0;
  for (size_t i = 0; i < kMacBytes; ++i) {
    diff |= static_cast<uint8_t>(mac[i] ^ given[i]);
  }
  if (diff != 0) {
    return Reject();
  }

  // Authentic. The fields are still range-checked: a bug on the far side must not reach the servos.
  char body[kMaxBodyLen + 1];
  memcpy(body, line, body_len);
  body[body_len] = '\0';
  const char* p = body + 2;
  int64_t seq = 0, dx = 0, dy = 0, conf = 0, lean = 0, kind = 0, gesture = 0;
  if (!NextInt(p, 1, 2147483647LL, &seq) || !NextInt(p, -100, 100, &dx) || !NextInt(p, -100, 100, &dy) ||
      !NextInt(p, 0, 100, &conf) || !NextInt(p, -90, 90, &lean) || !NextInt(p, 0, 1, &kind) ||
      !NextInt(p, 0, 1, &gesture) || *p != '\0') {
    return Reject();
  }
  if (static_cast<uint32_t>(seq) <= g_last_seq) {
    return Reject();  // replayed or reordered
  }
  g_last_seq = static_cast<uint32_t>(seq);

  TrackSample s = {};
  s.kind = static_cast<uint8_t>(kind);
  if (s.kind == 1 && conf > 0) {
    s.dx = static_cast<int16_t>(dx);
    s.dy = static_cast<int16_t>(dy);
    s.conf = static_cast<uint8_t>(conf);
    s.roll = static_cast<int16_t>(lean);
    s.gesture = static_cast<uint8_t>(gesture);
  } else {
    s.kind = 0;  // "nothing this frame" carries no position and cannot arm anything
  }
  *out = s;

  ++g_accepted;
  g_last_ok_ms = now_ms;
  g_ever_ok = true;
  g_session_ok = true;
  return true;
}

bool Active(uint32_t now_ms) { return g_session && g_session_ok && (now_ms - g_last_ok_ms) < kActiveMs; }

bool ShouldDrop() { return g_session && g_session_rejects >= kDropAfterRejects; }

uint32_t AcceptedCount() { return g_accepted; }

uint32_t RejectedCount() { return g_rejected; }

long LastAcceptedAgeMs(uint32_t now_ms) { return g_ever_ok ? static_cast<long>(now_ms - g_last_ok_ms) : -1L; }

}  // namespace cam_remote
