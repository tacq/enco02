#pragma once

#ifndef _SERVO_WEB_SERVER_H_
#define _SERVO_WEB_SERVER_H_

#include <Arduino.h>
#include <WebServer.h>
#include <memory>

class ServoWebServer {
 public:
  static ServoWebServer& GetInstance();

  // Start the web server and mDNS
  void Start();

  // Handle client requests (call periodically in loop)
  void HandleClient();

  // Stop web server
  void Stop();

  bool IsRunning() const { return running_; }

  // App features that live in main.cpp. Plain function pointers; any left null answers 503.
  struct Hooks {
    int (*get_volume)() = nullptr;
    void (*set_volume)(int volume) = nullptr;
    // Returns nullptr when the text was handed to the assistant, else a short error.
    const char* (*send_text)(const String& text) = nullptr;
    // Random head animation switch (standby + awake; tracking overrides it).
    bool (*get_head_random)() = nullptr;
    void (*set_head_random)(bool on) = nullptr;
  };
  void SetHooks(const Hooks& hooks) { hooks_ = hooks; }

 private:
  ServoWebServer();
  ~ServoWebServer();
  ServoWebServer(const ServoWebServer&) = delete;
  ServoWebServer& operator=(const ServoWebServer&) = delete;

  void SetupRoutes();
  void HandleRoot();
  void HandleApiStatus();
  void HandleApiServo();
  void HandleApiCenter();
  void HandleApiSweep();
  void HandleApiBobble();
  void HandleApiUiMode();
  void HandleApiVolume();
  void HandleApiTrack();
  void HandleApiSay();
  void HandleApiSpeed();
  void HandleApiAnim();
  void HandleNotFound();

  std::unique_ptr<WebServer> server_;
  bool running_ = false;
  Hooks hooks_;
};

#endif  // _SERVO_WEB_SERVER_H_
