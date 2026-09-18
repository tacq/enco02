#pragma once

#ifndef _AI_VOX_ENGINE_H_
#define _AI_VOX_ENGINE_H_

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "ai_vox_observer.h"
#include "audio_device/audio_input_device.h"
#include "audio_device/audio_output_device.h"

namespace ai_vox {

class Engine {
 public:
  static Engine& GetInstance();
  Engine() = default;
  virtual ~Engine() = default;
  virtual void SetObserver(std::shared_ptr<Observer> observer) = 0;
  virtual void SetOtaUrl(const std::string url) = 0;
  virtual void ConfigWebsocket(const std::string url, const std::map<std::string, std::string> headers) = 0;
  virtual void AddMcpTool(std::string name, std::string description, std::map<std::string, ParamSchemaVariant> attributes) = 0;
  virtual void Start(std::shared_ptr<AudioInputDevice> audio_input_device, std::shared_ptr<AudioOutputDevice> audio_output_device) = 0;
  virtual void Advance() = 0;
  // virtual void Process() = 0;
  virtual void SendText(std::string text) = 0;
  virtual void SendMcpCallResponse(const int64_t id, std::variant<std::string, int64_t, bool> response) = 0;
  virtual void SendMcpCallError(const int64_t id, const std::string error) = 0;

 private:
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;
};

}  // namespace ai_vox

#endif