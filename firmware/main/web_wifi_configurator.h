#pragma once

#ifndef _WEB_WIFI_CONFIGURATOR_H_
#define _WEB_WIFI_CONFIGURATOR_H_

#include <Arduino.h>
#include <WebServer.h>
#include <WiFi.h>

#include <functional>
#include <string>

class WebWifiConfigurator {
 public:
  using OnConfigSavedCallback = std::function<void(const String& ssid, const String& password)>;

  WebWifiConfigurator();
  ~WebWifiConfigurator();

  // Returns the AP SSID (e.g. "xiaozhi-1A2B")
  String StartApAndServer();

  // Call in loop / wait loop
  void HandleClient();

  bool IsConfigured() const { return configured_; }
  String GetSsid() const { return ssid_; }
  String GetPassword() const { return password_; }

  void Stop();

 private:
  void HandleRoot();
  void HandleSave();
  void HandleNotFound();

  WebServer server_{80};
  String ap_ssid_;
  String ssid_;
  String password_;
  bool configured_ = false;
};

#endif  // _WEB_WIFI_CONFIGURATOR_H_
