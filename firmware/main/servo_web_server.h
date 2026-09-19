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
  void HandleNotFound();

  std::unique_ptr<WebServer> server_;
  bool running_ = false;
};

#endif  // _SERVO_WEB_SERVER_H_
