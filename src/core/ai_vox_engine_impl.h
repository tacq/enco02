#pragma once

#ifndef _AI_VOX_ENGINE_IMPL_H_
#define _AI_VOX_ENGINE_IMPL_H_

#include <esp_event_base.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <condition_variable>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ai_vox_engine.h"
#include "components/cjson_util/cjson_util.h"
#include "components/task_queue/active_task_queue.h"
#include "components/task_queue/passive_task_queue.h"
#include "core/ai_vox_mcp_tool_manager.h"
#include "espressif_esp_websocket_client/esp_websocket_client.h"
#include "flex_array/flex_array.h"

struct button_dev_t;
class AudioInputEngine;
class AudioOutputEngine;
class WakeNet;
class Config;
namespace ai_vox {

class EngineImpl : public Engine {
 public:
  static EngineImpl &GetInstance();
  EngineImpl();
  ~EngineImpl();
  void SetObserver(std::shared_ptr<Observer> observer) override;
  void SetOtaUrl(const std::string url) override;
  void ConfigWebsocket(const std::string url, const std::map<std::string, std::string> headers) override;
  void AddMcpTool(std::string name, std::string description, std::map<std::string, ParamSchemaVariant> attributes) override;
  void Start(std::shared_ptr<AudioInputDevice> audio_input_device, std::shared_ptr<AudioOutputDevice> audio_output_device) override;
  void Advance() override;
  // void Process() override;
  void SendText(std::string text) override;
  void SendMcpCallResponse(const int64_t id, std::variant<std::string, int64_t, bool> response) override;
  void SendMcpCallError(const int64_t id, const std::string error) override;

 private:
  enum class State {
    kIdle,
    kInitted,
    kLoadingProtocol,
    kLoadingProtocolFailed,
    kWebsocketConnecting,
    kWebsocketConnectingWithWakeup,
    kWebsocketConnected,
    kWebsocketConnectedWithWakeup,
    kWebsocketConnectedFailed,
    kStandby,
    kListening,
    kSpeaking,
  };

  EngineImpl(const EngineImpl &) = delete;
  EngineImpl &operator=(const EngineImpl &) = delete;

  static void OnWebsocketEvent(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);

  void OnWebsocketEvent(esp_event_base_t base, int32_t event_id, void *event_data);
  void OnAudioFrame(FlexArray<uint8_t> &&data);
  void OnJsonData(FlexArray<uint8_t> &&data);
  void OnMcpJsonObj(cJSON *json_obj);
  void OnWebSocketConnected();
  void OnWebSocketDisconnected();
  void OnAudioOutputDataConsumed();
  void AdvanceInternal();
  void OnWakeUp();
  void OnLoadProtocol(const std::shared_ptr<Config> config);

  void LoadProtocol();
  void StartListening();
  void AbortSpeaking();
  void AbortSpeaking(const std::string &reason);
  bool ConnectWebSocket();
  void DisconnectWebSocket();
  void SendTextInternal(std::string text);
  void SendMcpResponse(const int64_t id, std::unique_ptr<cJSON, cjson_util::CjsonDeleter> json_obj);
  void ChangeState(const State new_state);

  mutable std::recursive_mutex mutex_;
  State state_ = State::kIdle;
  ChatState chat_state_ = ChatState::kIdle;
  std::shared_ptr<AudioInputDevice> audio_input_device_;
  std::shared_ptr<AudioOutputDevice> audio_output_device_;
  std::shared_ptr<Observer> observer_;
  esp_websocket_client_handle_t web_socket_client_ = nullptr;
  std::string uuid_;
  std::string session_id_;
  std::shared_ptr<AudioInputEngine> audio_input_engine_;
  std::shared_ptr<AudioOutputEngine> audio_output_engine_;
  std::string ota_url_;
  std::string websocket_url_;
  std::map<std::string, std::string> websocket_headers_;
#ifdef ARDUINO_ESP32S3_DEV
  std::unique_ptr<WakeNet> wake_net_;
#endif
  // PassiveTaskQueue task_queue_;
  ActiveTaskQueue task_queue_;
  ActiveTaskQueue network_task_queue_;
  mcp::ToolManager mcp_tool_manager_;
  const uint32_t audio_frame_duration_ = 60;
};
}  // namespace ai_vox

#endif