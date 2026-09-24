#pragma once

#include <string>

// Client for tools/home_bridge (a process on the home LAN that alone holds the smart-home
// credentials). This board only ever holds a LAN-scoped HMAC key, set in home_config.h, which is
// git-ignored. Without that file the feature compiles out and Configured() returns false.
namespace home_bridge {

bool Configured();

// Blocking (typically 0.3-2 s, bounded by internal timeouts). On success fills `toast` (3 short
// lines for the alert card) and `speech` (a paragraph for the assistant to read). On failure
// returns false and puts a short reason in `speech`.
bool FetchPoolSummary(std::string& toast, std::string& speech);

// Local allow-list check (same targets/ranges as the bridge). `value` is °F, used for pool_set/spa_set.
bool ValidatePoolSet(const std::string& target, const std::string& action, int value, std::string& error);
const char* PoolTargetNameZh(const std::string& target);

// Blocking (up to ~15 s: the bridge waits for the panel to confirm). Same toast/speech contract.
bool PoolSet(const std::string& target, const std::string& action, int value, std::string& toast,
             std::string& speech);

}  // namespace home_bridge
