#include "wifi_configurator.h"

#include <Preferences.h>
#include <esp_bit_defs.h>
#include <esp_heap_caps.h>
#include <esp_private/wifi.h>
#include <esp_smartconfig.h>
#include <esp_wifi.h>

#include "components/clogger/clogger.h"
#include "smartconfig_ack.h"

namespace {
constexpr char kPreferenceKey[] = "WiFiConnector";
constexpr uint32_t kSmartConfigDoneBit = BIT0;
constexpr uint32_t kConnectedBit = BIT1;
}  // namespace

WifiConfigurator::WifiConfigurator(WiFiClass& wifi, const smartconfig_type_t smartconfig_type)
    : wifi_(wifi), smartconfig_type_(smartconfig_type), event_group_(xEventGroupCreate()), state_changed_queue_(xQueueCreate(10, sizeof(State))) {
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
    wifi_.useStaticBuffers(true);
  } else {
    wifi_.useStaticBuffers(false);
  }

  xEventGroupSetBits(event_group_, kSmartConfigDoneBit);
  wifi_.mode(WIFI_STA);
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &EventHandler, this));
}

WifiConfigurator::~WifiConfigurator() {
  std::lock_guard lock(mutex_);
  esp_event_handler_unregister(SC_EVENT, ESP_EVENT_ANY_ID, &EventHandler);
  esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, &EventHandler);
  vQueueDelete(state_changed_queue_);
  vEventGroupDelete(event_group_);
}

void WifiConfigurator::Start() {
  std::lock_guard lock(mutex_);

  if (state_ != State::kIdle) {
    return;
  }

  Preferences prefs;
  prefs.begin(kPreferenceKey, false);

  String ssid;
  if (prefs.isKey("ssid")) {
    ssid = prefs.getString("ssid");
  }

  String password;
  if (prefs.isKey("password")) {
    password = prefs.getString("password");
  }

  prefs.end();

  if (ssid.length() > 0) {
    CLOGI("wifi begin with %s, %s", ssid.c_str(), password.c_str());
    wifi_.begin(ssid.c_str(), password.c_str());
    state_ = State::kConnecting;
    xQueueSend(state_changed_queue_, &state_, portMAX_DELAY);
  } else {
    return StartSmartConfig();
  }
}

void WifiConfigurator::Start(const std::string& wifi_ssid, const std::string& wifi_password) {
  std::lock_guard lock(mutex_);

  if (state_ != State::kIdle) {
    return;
  }

  Preferences prefs;
  prefs.begin(kPreferenceKey, false);
  prefs.putString("ssid", wifi_ssid.c_str());
  prefs.putString("password", wifi_password.c_str());
  prefs.end();
  Start();
}

void WifiConfigurator::StartSmartConfig() {
  std::lock_guard lock(mutex_);
  if (state_ == State::kSmartConfiguring) {
    return;
  }

  xEventGroupClearBits(event_group_, kSmartConfigDoneBit);

  esp_wifi_disconnect();

  ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &EventHandler, this));

  smartconfig_start_config_t conf = SMARTCONFIG_START_CONFIG_DEFAULT();

  if (smartconfig_type_ == SC_TYPE_ESPTOUCH_V2) {
    conf.esp_touch_v2_enable_crypt = true;
    conf.esp_touch_v2_key = nullptr;
  }

  auto err = esp_smartconfig_set_type(smartconfig_type_);
  if (err != ESP_OK) {
    CLOGE("esp_smartconfig_set_type failed with error 0x%x", err);
    return;
  }
  err = esp_smartconfig_internal_start(&conf);
  if (err != ESP_OK) {
    CLOGE("esp_smartconfig_internal_start failed with error 0x%x", err);
    return;
  }

  state_ = State::kSmartConfiguring;
  xQueueSend(state_changed_queue_, &state_, portMAX_DELAY);
  return;
}

bool WifiConfigurator::finished() const {
  return (xEventGroupGetBits(event_group_) & (kConnectedBit | kSmartConfigDoneBit)) == (kConnectedBit | kSmartConfigDoneBit);
}

WifiConfigurator::State WifiConfigurator::WaitStateChanged() const {
  State state = State::kIdle;
  xQueueReceive(state_changed_queue_, &state, portMAX_DELAY);
  return state;
}

void WifiConfigurator::OnEvent(esp_event_base_t event_base, int32_t event_id, void* event_data) {
  if (SC_EVENT == event_base) {
    switch (event_id) {
      case SC_EVENT_SCAN_DONE: {
        CLOGI("smartconfig scan done");
        break;
      }
      case SC_EVENT_FOUND_CHANNEL: {
        CLOGI("smartconfig found channel");
        break;
      }
      case SC_EVENT_GOT_SSID_PSWD: {
        const auto data = reinterpret_cast<smartconfig_event_got_ssid_pswd_t*>(event_data);
        CLOGI(
            "smartconfig got SSID and password, SSID: %.*s, password: %.*s", sizeof(data->ssid), data->ssid, sizeof(data->password), data->password);
        std::lock_guard lock(mutex_);
        if (state_ != State::kSmartConfiguring) {
          return;
        }

        state_ = State::kConnecting;
        sc_send_ack_start(data->type, data->token, data->cellphone_ip);
        xQueueSend(state_changed_queue_, &state_, portMAX_DELAY);
        break;
      }
      case SC_EVENT_SEND_ACK_DONE: {
        CLOGI("smartconfig send ack done");
        std::lock_guard lock(mutex_);
        xEventGroupSetBits(event_group_, kSmartConfigDoneBit);
        const auto err = esp_smartconfig_internal_stop();
        if (err == ESP_OK) {
          sc_send_ack_stop();
        }
        if (finished()) {
          state_ = State::kFinished;
          xQueueSend(state_changed_queue_, &state_, portMAX_DELAY);
        }
        break;
      }
      default: {
        break;
      }
    }
  } else if (IP_EVENT == event_base) {
    switch (event_id) {
      case IP_EVENT_STA_GOT_IP: {
        CLOGI("got ip: " IPSTR, IP2STR(&reinterpret_cast<ip_event_got_ip_t*>(event_data)->ip_info.ip));
        xEventGroupSetBits(event_group_, kConnectedBit);
        std::lock_guard lock(mutex_);
        state_ = State::kConnected;
        xQueueSend(state_changed_queue_, &state_, portMAX_DELAY);
        if (finished()) {
          state_ = State::kFinished;
          xQueueSend(state_changed_queue_, &state_, portMAX_DELAY);
        }
        Preferences prefs;
        prefs.begin(kPreferenceKey, false);
        prefs.putString("ssid", wifi_.SSID());
        prefs.putString("password", wifi_.psk());
        prefs.end();
        break;
      }
      default: {
        break;
      }
    }
  }
}
