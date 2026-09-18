#pragma once

#ifndef _WIFI_CONNECTOR_H_
#define _WIFI_CONNECTOR_H_

#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <mutex>

class WifiConfigurator {
 public:
  enum class State : uint8_t {
    kIdle,
    kConnecting,
    kConnected,
    kSmartConfiguring,
    kSmartConfigDone,
    kFinished,
  };

  WifiConfigurator(WiFiClass& wifi, const smartconfig_type_t smartconfig_type);
  ~WifiConfigurator();
  void Start();
  void Start(const std::string& wifi_ssid, const std::string& wifi_password);
  void StartSmartConfig();
  bool finished() const;
  State WaitStateChanged() const;

 private:
  static void EventHandler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    reinterpret_cast<WifiConfigurator*>(arg)->OnEvent(event_base, event_id, event_data);
  }
  void OnEvent(esp_event_base_t event_base, int32_t event_id, void* event_data);

  WiFiClass& wifi_;
  const smartconfig_type_t smartconfig_type_ = SC_TYPE_ESPTOUCH;
  std::recursive_mutex mutex_;
  State state_ = State::kIdle;
  EventGroupHandle_t event_group_ = nullptr;
  QueueHandle_t state_changed_queue_ = nullptr;
};

#endif