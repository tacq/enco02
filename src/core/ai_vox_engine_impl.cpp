#include "ai_vox_engine_impl.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_crt_bundle.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "audio_input_engine.h"
#include "audio_output_engine.h"
#include "components/cjson_util/cjson_util.h"
#include "fetch_config.h"
#include "wake_net/wake_net.h"

#ifndef CLOGGER_SEVERITY
#define CLOGGER_SEVERITY CLOGGER_SEVERITY_WARN
#endif
#include "clogger/clogger.h"

namespace ai_vox {

namespace {

enum WebSocketFrameType : uint8_t {
  kWebsocketTextFrame = 0x01,    // 文本帧
  kWebsocketBinaryFrame = 0x02,  // 二进制帧
  kWebsocketCloseFrame = 0x08,   // 关闭连接
  kWebsocketPingFrame = 0x09,    // Ping 帧
  kWebsocketPongFrame = 0x0A,    // Pong 帧
};

std::string GetMacAddress() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char mac_str[18] = {0};
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return std::string(mac_str);
}

std::string Uuid() {
  // UUID v4 需要 16 字节的随机数据
  uint8_t uuid[16] = {0};

  // 使用 ESP32 的硬件随机数生成器
  esp_fill_random(uuid, sizeof(uuid));

  // 设置版本 (版本 4) 和变体位
  uuid[6] = (uuid[6] & 0x0F) | 0x40;  // 版本 4
  uuid[8] = (uuid[8] & 0x3F) | 0x80;  // 变体 1

  // 将字节转换为标准的 UUID 字符串格式
  char uuid_str[37] = {0};
  snprintf(uuid_str,
           sizeof(uuid_str),
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           uuid[0],
           uuid[1],
           uuid[2],
           uuid[3],
           uuid[4],
           uuid[5],
           uuid[6],
           uuid[7],
           uuid[8],
           uuid[9],
           uuid[10],
           uuid[11],
           uuid[12],
           uuid[13],
           uuid[14],
           uuid[15]);

  return std::string(uuid_str);
}

}  // namespace

EngineImpl &EngineImpl::GetInstance() {
  static std::once_flag s_once_flag;
  static EngineImpl *s_instance = nullptr;
  std::call_once(s_once_flag, []() { s_instance = new EngineImpl; });
  return *s_instance;
}

EngineImpl::EngineImpl()
    : uuid_(Uuid()),
      ota_url_("https://api.tenclass.net/xiaozhi/ota/"),
      websocket_url_("wss://api.tenclass.net/xiaozhi/v1/"),
      websocket_headers_{
          {"Authorization", "Bearer test-token"},
      },
      task_queue_("AiVoxMain", 1024 * 4, tskIDLE_PRIORITY + 1),
      network_task_queue_("AiVoxNetwork", 1024 * 4, tskIDLE_PRIORITY + 1, true) {
}

EngineImpl::~EngineImpl() {
  // TODO
}

void EngineImpl::SetObserver(std::shared_ptr<Observer> observer) {
  std::lock_guard lock(mutex_);
  if (state_ != State::kIdle) {
    return;
  }

  observer_ = std::move(observer);
}

void EngineImpl::SetOtaUrl(const std::string url) {
  std::lock_guard lock(mutex_);
  if (state_ != State::kIdle) {
    return;
  }
  ota_url_ = std::move(url);
}

void EngineImpl::ConfigWebsocket(const std::string url, const std::map<std::string, std::string> headers) {
  std::lock_guard lock(mutex_);
  if (state_ != State::kIdle) {
    return;
  }

  websocket_url_ = std::move(url);
  for (auto [key, value] : headers) {
    websocket_headers_.insert_or_assign(std::move(key), std::move(value));
  }
}

void EngineImpl::AddMcpTool(std::string name, std::string description, std::map<std::string, ParamSchemaVariant> attributes) {
  std::lock_guard lock(mutex_);
  if (state_ != State::kIdle) {
    return;
  }
  mcp_tool_manager_.AddTool(std::move(name), mcp::Tool(std::move(description), std::move(attributes)));
}

void EngineImpl::Start(std::shared_ptr<AudioInputDevice> audio_input_device, std::shared_ptr<AudioOutputDevice> audio_output_device) {
  CLOGD();
  std::lock_guard lock(mutex_);
  if (state_ != State::kIdle) {
    return;
  }

  audio_input_device_ = std::move(audio_input_device);
  audio_output_device_ = std::move(audio_output_device);
#ifdef ARDUINO_ESP32S3_DEV
  wake_net_ = std::make_unique<WakeNet>([this]() { task_queue_.Enqueue([this]() { OnWakeUp(); }); }, audio_input_device_);
  wake_net_->Start();
#endif

  esp_websocket_client_config_t websocket_cfg;
  memset(&websocket_cfg, 0, sizeof(websocket_cfg));
  websocket_cfg.uri = websocket_url_.c_str();
  websocket_cfg.task_prio = tskIDLE_PRIORITY;
  websocket_cfg.crt_bundle_attach = esp_crt_bundle_attach;

  CLOGI("url: %s", websocket_cfg.uri);
  web_socket_client_ = esp_websocket_client_init(&websocket_cfg);
  if (web_socket_client_ == nullptr) {
    CLOGE("esp_websocket_client_init failed with %s", websocket_cfg.uri);
    abort();
  }
  for (const auto &[key, value] : websocket_headers_) {
    esp_websocket_client_append_header(web_socket_client_, key.c_str(), value.c_str());
  }
  esp_websocket_client_append_header(web_socket_client_, "Protocol-Version", "1");
  esp_websocket_client_append_header(web_socket_client_, "Device-Id", GetMacAddress().c_str());
  esp_websocket_client_append_header(web_socket_client_, "Client-Id", uuid_.c_str());
  esp_websocket_register_events(web_socket_client_, WEBSOCKET_EVENT_ANY, &EngineImpl::OnWebsocketEvent, this);

  ChangeState(State::kInitted);
  ChangeState(State::kLoadingProtocol);
  network_task_queue_.Enqueue([this]() { LoadProtocol(); });
}

void EngineImpl::Advance() {
  std::lock_guard lock(mutex_);
  if (state_ == State::kIdle) {
    return;
  }
  task_queue_.Enqueue([this]() { AdvanceInternal(); });
}

// void EngineImpl::Process() {
//   std::lock_guard lock(mutex_);
//   if (state_ == State::kIdle) {
//     return;
//   }
//   task_queue_.Process();
// }

void EngineImpl::SendText(std::string text) {
  std::lock_guard lock(mutex_);
  if (state_ == State::kIdle) {
    return;
  }
  SendTextInternal(std::move(text));
}

void EngineImpl::SendMcpCallResponse(const int64_t id, std::variant<std::string, int64_t, bool> response) {
  std::lock_guard lock(mutex_);
  if (state_ == State::kIdle) {
    return;
  }

  task_queue_.Enqueue([this, id, response = std::move(response)]() mutable {
    auto root_json_obj = cjson_util::MakeUnique();
    // "jsonrpc"
    cJSON_AddStringToObject(root_json_obj.get(), "jsonrpc", "2.0");
    // "id"
    cJSON_AddNumberToObject(root_json_obj.get(), "id", id);

    cJSON *result_json_obj = cJSON_CreateObject();
    cJSON *content_json_obj = cJSON_CreateArray();
    cJSON *text_json_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(text_json_obj, "type", "text");
    if (auto value = std::get_if<std::string>(&response)) {
      cJSON_AddStringToObject(text_json_obj, "text", value->c_str());
    } else if (auto value = std::get_if<int64_t>(&response)) {
      cJSON_AddStringToObject(text_json_obj, "text", std::to_string(*value).c_str());
    } else if (auto value = std::get_if<bool>(&response)) {
      cJSON_AddStringToObject(text_json_obj, "text", *value ? "true" : "false");
    }
    cJSON_AddItemToArray(content_json_obj, text_json_obj);
    cJSON_AddItemToObject(result_json_obj, "content", content_json_obj);
    cJSON_AddBoolToObject(result_json_obj, "isError", false);
    cJSON_AddItemToObject(root_json_obj.get(), "result", result_json_obj);
    SendMcpResponse(id, std::move(root_json_obj));
  });
}

void EngineImpl::SendMcpCallError(const int64_t id, const std::string error) {
  std::lock_guard lock(mutex_);
  if (state_ == State::kIdle) {
    return;
  }

  task_queue_.Enqueue([this, id, error = std::move(error)]() mutable {
    auto root_json_obj = cjson_util::MakeUnique();
    // "jsonrpc"
    cJSON_AddStringToObject(root_json_obj.get(), "jsonrpc", "2.0");
    // "id"
    cJSON_AddNumberToObject(root_json_obj.get(), "id", id);

    auto error_json_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(error_json_obj, "message", error.c_str());
    cJSON_AddItemToObject(root_json_obj.get(), "error", error_json_obj);
    SendMcpResponse(id, std::move(root_json_obj));
  });
}

void EngineImpl::OnWebsocketEvent(void *self, esp_event_base_t base, int32_t event_id, void *event_data) {
  reinterpret_cast<EngineImpl *>(self)->OnWebsocketEvent(base, event_id, event_data);
}

void EngineImpl::OnWebsocketEvent(esp_event_base_t base, int32_t event_id, void *event_data) {
  esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
  switch (event_id) {
    case WEBSOCKET_EVENT_BEGIN: {
      CLOGI("WEBSOCKET_EVENT_BEGIN");
      break;
    }
    case WEBSOCKET_EVENT_CONNECTED: {
      CLOGI("WEBSOCKET_EVENT_CONNECTED");
      task_queue_.Enqueue([this]() { OnWebSocketConnected(); });
      break;
    }
    case WEBSOCKET_EVENT_DISCONNECTED: {
      CLOGI("WEBSOCKET_EVENT_DISCONNECTED");
      task_queue_.Enqueue([this]() { OnWebSocketDisconnected(); });
      break;
    }
    case WEBSOCKET_EVENT_DATA: {
      if (!data->fin) {
        // TODO:
        printf("[%s][%d] TODO: fragmented frame\n", __FILE__, __LINE__);
        vTaskDelay(portMAX_DELAY);
        abort();
      }

      switch (data->op_code) {
        case kWebsocketTextFrame: {
          FlexArray<uint8_t> frame(data->data_len);
          memcpy(frame.data(), data->data_ptr, data->data_len);
          task_queue_.Enqueue([this, frame = std::move(frame)]() mutable {
            if (observer_) {
              observer_->PushEvent(TextReceivedEvent{
                  .content = std::string(reinterpret_cast<const char *>(frame.data()), frame.size()),
              });
            }
            OnJsonData(std::move(frame));
          });
          break;
        }
        case kWebsocketBinaryFrame: {
          FlexArray<uint8_t> frame(data->data_len);
          memcpy(frame.data(), data->data_ptr, data->data_len);
          task_queue_.Enqueue([this, frame = std::move(frame)]() mutable { OnAudioFrame(std::move(frame)); });
          break;
        }
        default: {
          break;
        }
      }
      break;
    }
    case WEBSOCKET_EVENT_ERROR: {
      CLOGE("WEBSOCKET_EVENT_ERROR");
      break;
    }
    case WEBSOCKET_EVENT_FINISH: {
      CLOGI("WEBSOCKET_EVENT_FINISH");
      task_queue_.Enqueue([this]() { OnWebSocketDisconnected(); });
      break;
    }
    default: {
      break;
    }
  }
}

void EngineImpl::OnAudioFrame(FlexArray<uint8_t> &&data) {
  if (audio_output_engine_) {
    audio_output_engine_->Write(std::move(data));
  }
}

void EngineImpl::OnJsonData(FlexArray<uint8_t> &&data) {
  CLOGI("%.*s", static_cast<int>(data.size()), data.data());

  auto root_json_obj = cjson_util::MakeUnique(cJSON_ParseWithLength(reinterpret_cast<const char *>(data.data()), data.size()));

  if (!cJSON_IsObject(root_json_obj.get())) {
    CLOGE("Invalid JSON data");
    return;
  }

  auto type = cjson_util::GetString(root_json_obj.get(), "type");
  if (!type) {
    CLOGE("missing or invalid 'type' field in JSON data");
    return;
  }
  CLOGI("got type: %s", type->c_str());

  if (*type == "hello") {
    const auto state = state_;
    if (state_ != State::kWebsocketConnected && state_ != State::kWebsocketConnectedWithWakeup) {
      CLOGE("Invalid state: %u", state_);
      return;
    }

    if (auto session_id = cjson_util::GetString(root_json_obj.get(), "session_id")) {
      session_id_ = std::move(*session_id);
      CLOGI("got session id: %s", session_id_.c_str());
    }

    StartListening();

    if (state == State::kWebsocketConnectedWithWakeup) {
      auto message_json_obj = cjson_util::MakeUnique();
      cJSON_AddStringToObject(message_json_obj.get(), "session_id", session_id_.c_str());
      cJSON_AddStringToObject(message_json_obj.get(), "type", "listen");
      cJSON_AddStringToObject(message_json_obj.get(), "state", "detect");
      cJSON_AddStringToObject(message_json_obj.get(), "text", "你好小智");
      SendTextInternal(cjson_util::ToString(message_json_obj));
    }
  } else if (*type == "goodbye") {
    CLOGI("goodbye");
    if (const auto session_id = cjson_util::GetString(root_json_obj.get(), "session_id")) {
      CLOGI("session id: %s, current session id: %s", session_id->c_str(), session_id_.c_str());
      if (session_id_ != *session_id) {
        CLOGW("session id mismatch, ignoring goodbye, session id: %s, current session id: %s", session_id->c_str(), session_id_.c_str());
        return;
      }
    }
  } else if (*type == "tts") {
    const auto tts_state = cjson_util::GetString(root_json_obj.get(), "state");
    if (!tts_state) {
      CLOGE("missing or invalid 'state' field in JSON data");
      return;
    }
    CLOGI("tts/%s", tts_state->c_str());
    if (tts_state == "start") {
      if (state_ == State::kSpeaking) {
        CLOGW("already in speaking");
        return;
      } else if (state_ != State::kListening) {
        CLOGW("on tts start in invalid state: %u", state_);
        return;
      }

      audio_input_engine_.reset();
#ifdef ARDUINO_ESP32S3_DEV
      wake_net_->Start();
#endif
      audio_output_engine_ = std::make_shared<AudioOutputEngine>(audio_output_device_, audio_frame_duration_);
      ChangeState(State::kSpeaking);
    } else if (tts_state == "stop") {
      if (audio_output_engine_) {
        audio_output_engine_->NotifyDataEnd([this]() { task_queue_.Enqueue([this]() { OnAudioOutputDataConsumed(); }); });
      }
    } else if (tts_state == "sentence_start") {
      auto text = cjson_util::GetString(root_json_obj.get(), "text");
      if (text) {
        CLOGI("<< %s", text->c_str());
      }
      if (observer_) {
        observer_->PushEvent(ChatMessageEvent{ChatRole::kAssistant, std::move(*text)});
      }
    } else if (tts_state == "sentence_end") {
      // Do nothing
    }
  } else if (*type == "stt") {
    auto text = cjson_util::GetString(root_json_obj.get(), "text");
    if (text) {
      CLOGI(">> %s", text->c_str());
      if (observer_) {
        observer_->PushEvent(ChatMessageEvent{ChatRole::kUser, std::move(*text)});
      }
    }
  } else if (*type == "llm") {
    auto emotion = cjson_util::GetString(root_json_obj.get(), "emotion");
    if (emotion) {
      CLOGI("emotion: %s", emotion->c_str());
      if (observer_) {
        observer_->PushEvent(EmotionEvent{std::move(*emotion)});
      }
    }
  } else if (*type == "mcp") {
    OnMcpJsonObj(cJSON_GetObjectItem(root_json_obj.get(), "payload"));
  } else {
    CLOGE("unknown type: %s", type->c_str());
  }
}

void EngineImpl::OnWebSocketConnected() {
  CLOGI();
  if (state_ == State::kWebsocketConnecting) {
    ChangeState(State::kWebsocketConnected);
  } else if (state_ == State::kWebsocketConnectingWithWakeup) {
    ChangeState(State::kWebsocketConnectedWithWakeup);
  } else {
    CLOGE("invalid state: %u", state_);
    return;
  }

  auto root_json_obj = cjson_util::MakeUnique();
  cJSON_AddStringToObject(root_json_obj.get(), "type", "hello");
  cJSON_AddNumberToObject(root_json_obj.get(), "version", 1);
  cJSON_AddStringToObject(root_json_obj.get(), "transport", "websocket");

  // features
  auto const features_obj = cJSON_CreateObject();
  // mcp:true
  cJSON_AddBoolToObject(features_obj, "mcp", true);
  cJSON_AddItemToObject(root_json_obj.get(), "features", features_obj);

  auto const audio_params_obj = cJSON_CreateObject();
  cJSON_AddStringToObject(audio_params_obj, "format", "opus");
  cJSON_AddNumberToObject(audio_params_obj, "sample_rate", 16000);
  cJSON_AddNumberToObject(audio_params_obj, "channels", 1);
  cJSON_AddNumberToObject(audio_params_obj, "frame_duration", audio_frame_duration_);
  cJSON_AddItemToObject(root_json_obj.get(), "audio_params", audio_params_obj);
  SendTextInternal(cjson_util::ToString(root_json_obj));
}

void EngineImpl::OnMcpJsonObj(cJSON *root_json_obj) {
  CLOGI("%s", cjson_util::ToString(root_json_obj).c_str());

  if (!cJSON_IsObject(root_json_obj)) {
    return;
  }

  // "jsonrpc":"2.0"
  if (const auto jsonrpc = cjson_util::GetString(root_json_obj, "jsonrpc")) {
    CLOGI("jsonrpc: %s", jsonrpc.value().c_str());
    if (jsonrpc != "2.0") {
      CLOGE("invalid jsonrpc: %s", jsonrpc.value().c_str());
      return;
    }
  } else {
    CLOGE("jsonrpc is null");
    return;
  }

  // "id":int
  const auto id = cjson_util::GetNumber(root_json_obj, "id");
  CLOGI("id: %s", id ? std::to_string(id.value()).c_str() : "null");

  // "method": str
  const auto method = cjson_util::GetString(root_json_obj, "method");
  CLOGI("method: %s", method ? method.value().c_str() : "null");
  if (!method) {
    CLOGE("method is null");
    return;
  }

  if (*method == "initialize") {
    const auto app_desc = esp_app_get_description();
    auto response_json_obj = cjson_util::MakeUnique();
    cJSON_AddStringToObject(response_json_obj.get(), "jsonrpc", "2.0");
    cJSON_AddNumberToObject(response_json_obj.get(), "id", id.value());

    auto result_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(result_obj, "protocolVersion", "2024-11-05");

    auto capabilities_obj = cJSON_CreateObject();
    auto tools_obj = cJSON_CreateObject();
    cJSON_AddItemToObject(capabilities_obj, "tools", tools_obj);
    cJSON_AddItemToObject(result_obj, "capabilities", capabilities_obj);

    auto server_info_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(server_info_obj, "name", "ai-vox");
    cJSON_AddStringToObject(server_info_obj, "version", app_desc->version);
    cJSON_AddItemToObject(result_obj, "serverInfo", server_info_obj);
    cJSON_AddItemToObject(response_json_obj.get(), "result", result_obj);

    // auto const reply_text = cjson_util::ToString(reply_obj);
    SendMcpResponse(id.value(), std::move(response_json_obj));
  } else if (*method == "tools/list") {
    auto response_json_obj = cjson_util::MakeUnique();
    cJSON_AddStringToObject(response_json_obj.get(), "jsonrpc", "2.0");
    cJSON_AddNumberToObject(response_json_obj.get(), "id", id.value());
    cJSON_AddItemToObject(response_json_obj.get(), "result", mcp_tool_manager_.ToJson().release());
    SendMcpResponse(id.value(), std::move(response_json_obj));
  } else if (*method == "tools/call") {
    auto params_json_obj = cJSON_GetObjectItem(root_json_obj, "params");
    auto name = cjson_util::GetString(params_json_obj, "name");
    if (!name) {
      CLOGE("name of params is null");
      return;
    }

    const auto arguments_json_obj = cJSON_GetObjectItem(params_json_obj, "arguments");
    std::map<std::string, std::variant<std::string, int64_t, bool>> params;
    if (cJSON_IsObject(arguments_json_obj)) {
      for (cJSON *item = arguments_json_obj->child; item != nullptr; item = item->next) {
        if (cJSON_IsString(item)) {
          params[item->string] = item->valuestring;
        } else if (cJSON_IsBool(item)) {
          params[item->string] = item->valueint != 0;
        } else if (cJSON_IsNumber(item)) {
          params[item->string] = item->valueint;
        }
      }
    }

    if (observer_) {
      observer_->PushEvent(McpToolCallEvent{*id, *name, std::move(params)});
    }
  }
}

void EngineImpl::OnWebSocketDisconnected() {
  CLOGI();
  audio_input_engine_.reset();
  audio_output_engine_.reset();
  esp_websocket_client_close(web_socket_client_, pdMS_TO_TICKS(5000));

#ifdef ARDUINO_ESP32S3_DEV
  wake_net_->Start();
#endif
  ChangeState(State::kStandby);
}

void EngineImpl::OnAudioOutputDataConsumed() {
  CLOGI();
  if (state_ != State::kSpeaking) {
    CLOGD("invalid state: %u", state_);
    return;
  }
  StartListening();
}

void EngineImpl::AdvanceInternal() {
  CLOGI("state: %u", state_);
  switch (state_) {
    case State::kInitted:
    case State::kLoadingProtocolFailed: {
      ChangeState(State::kLoadingProtocol);
      network_task_queue_.Enqueue([this]() { LoadProtocol(); });
      break;
    }
    case State::kStandby: {
      if (ConnectWebSocket()) {
        ChangeState(State::kWebsocketConnecting);
      }
      break;
    }
    case State::kListening: {
      DisconnectWebSocket();
      break;
    }
    case State::kSpeaking: {
      AbortSpeaking();
      break;
    }
    default: {
      break;
    }
  }
}

void EngineImpl::OnWakeUp() {
  CLOGI();
  switch (state_) {
    case State::kInitted:
    case State::kLoadingProtocolFailed: {
      ChangeState(State::kLoadingProtocol);
      network_task_queue_.Enqueue([this]() { LoadProtocol(); });
      break;
    }
    case State::kStandby: {
      if (ConnectWebSocket()) {
        ChangeState(State::kWebsocketConnectingWithWakeup);
      }
      break;
    }
    case State::kSpeaking: {
      AbortSpeaking("wake_word_detected");
      break;
    }
    default: {
      break;
    }
  }
}

void EngineImpl::OnLoadProtocol(const std::shared_ptr<Config> config) {
  if (state_ != State::kLoadingProtocol) {
    CLOGW("invalid state: %u", state_);
    return;
  }

  if (!config) {
    CLOGD("message is null");
    ChangeState(State::kLoadingProtocolFailed);
    return;
  }

  CLOGI("mqtt endpoint: %s", config->mqtt.endpoint.c_str());
  CLOGI("mqtt client_id: %s", config->mqtt.client_id.c_str());
  CLOGI("mqtt username: %s", config->mqtt.username.c_str());
  CLOGI("mqtt password: %s", config->mqtt.password.c_str());
  CLOGI("mqtt publish_topic: %s", config->mqtt.publish_topic.c_str());
  CLOGI("mqtt subscribe_topic: %s", config->mqtt.subscribe_topic.c_str());

  CLOGI("activation code: %s", config->activation.code.c_str());
  CLOGI("activation message: %s", config->activation.message.c_str());

  if (!config->activation.code.empty()) {
    if (observer_) {
      observer_->PushEvent(ActivationEvent{config->activation.code, config->activation.message});
    }
    ChangeState(State::kInitted);
    return;
  }

  ChangeState(State::kStandby);
}

void EngineImpl::LoadProtocol() {
  CLOGI();
  auto config = GetConfigFromServer(ota_url_, uuid_);
  task_queue_.Enqueue([this, config = std::move(config)]() mutable { OnLoadProtocol(config); });
}

void EngineImpl::StartListening() {
  if (state_ != State::kWebsocketConnected && state_ != State::kWebsocketConnectedWithWakeup && state_ != State::kSpeaking) {
    CLOGI("invalid state: %u", state_);
    return;
  }

  auto root_json_obj = cjson_util::MakeUnique();
  cJSON_AddStringToObject(root_json_obj.get(), "session_id", session_id_.c_str());
  cJSON_AddStringToObject(root_json_obj.get(), "type", "listen");
  cJSON_AddStringToObject(root_json_obj.get(), "state", "start");
  cJSON_AddStringToObject(root_json_obj.get(), "mode", "auto");
  SendTextInternal(cjson_util::ToString(root_json_obj));

  audio_output_engine_.reset();
#ifdef ARDUINO_ESP32S3_DEV
  wake_net_->Stop();
#endif
  audio_input_engine_ = std::make_shared<AudioInputEngine>(
      audio_input_device_,
      [this](FlexArray<uint8_t> &&data) mutable {
        if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0 && network_task_queue_.size() > 5) {
          return;
        }

        network_task_queue_.Enqueue([this, data = std::move(data)]() mutable {
          if (esp_websocket_client_is_connected(web_socket_client_)) {
            const auto start_time = esp_timer_get_time();
            if (data.size() !=
                esp_websocket_client_send_bin(web_socket_client_, reinterpret_cast<const char *>(data.data()), data.size(), pdMS_TO_TICKS(3000))) {
              CLOGE("sending failed");
            }

            const auto elapsed_time = esp_timer_get_time() - start_time;
            if (elapsed_time > 100 * 1000) {
              CLOGW("network latency high: %lld ms, data size: %zu bytes, poor network condition detected", elapsed_time / 1000, data.size());
            }
          }
        });
      },
      audio_frame_duration_);
  ChangeState(State::kListening);
}

void EngineImpl::AbortSpeaking() {
  if (state_ != State::kSpeaking) {
    CLOGE("invalid state: %d", state_);
    return;
  }

  auto root_json_obj = cjson_util::MakeUnique();
  cJSON_AddStringToObject(root_json_obj.get(), "session_id", session_id_.c_str());
  cJSON_AddStringToObject(root_json_obj.get(), "type", "abort");
  SendTextInternal(cjson_util::ToString(root_json_obj));
}

void EngineImpl::AbortSpeaking(const std::string &reason) {
  if (state_ != State::kSpeaking) {
    CLOGE("invalid state: %d", state_);
    return;
  }

  auto root_json_obj = cjson_util::MakeUnique();
  cJSON_AddStringToObject(root_json_obj.get(), "session_id", session_id_.c_str());
  cJSON_AddStringToObject(root_json_obj.get(), "type", "abort");
  cJSON_AddStringToObject(root_json_obj.get(), "reason", reason.c_str());
  SendTextInternal(cjson_util::ToString(root_json_obj));
}

bool EngineImpl::ConnectWebSocket() {
  if (state_ != State::kStandby) {
    CLOGE("invalid state: %u", state_);
    return false;
  }

  CLOGI("esp_websocket_client_start");
  const auto ret = esp_websocket_client_start(web_socket_client_);
  CLOGI("websocket client start: %d", ret);
  return ret == ESP_OK;
}

void EngineImpl::DisconnectWebSocket() {
  audio_input_engine_.reset();
  audio_output_engine_.reset();
#ifdef ARDUINO_ESP32S3_DEV
  wake_net_->Start();
#endif
  esp_websocket_client_close(web_socket_client_, pdMS_TO_TICKS(5000));
}

void EngineImpl::SendTextInternal(std::string text) {
  network_task_queue_.Enqueue([this, text = std::move(text)]() mutable {
    if (esp_websocket_client_is_connected(web_socket_client_)) {
      const auto start_time = esp_timer_get_time();
      auto ret = esp_websocket_client_send_text(web_socket_client_, text.c_str(), text.length(), pdMS_TO_TICKS(10000));
      const auto elapsed_time = esp_timer_get_time() - start_time;
      if (ret != text.length()) {
        CLOGE("sending text failed, expected: %zu bytes, actual: %d bytes", text.length(), ret);
      }
      if (elapsed_time > 100 * 1000) {
        CLOGW("network latency high: %lld ms, data size: %zu bytes, poor network condition detected", elapsed_time / 1000, text.length());
      }
    }
  });
}

void EngineImpl::SendMcpResponse(const int64_t id, std::unique_ptr<cJSON, cjson_util::CjsonDeleter> json_obj) {
  auto root_json_obj = cjson_util::MakeUnique();
  // session id:
  cJSON_AddStringToObject(root_json_obj.get(), "session_id", session_id_.c_str());
  cJSON_AddStringToObject(root_json_obj.get(), "type", "mcp");
  cJSON_AddItemToObject(root_json_obj.get(), "payload", json_obj.release());
  SendTextInternal(cjson_util::ToString(root_json_obj));
}

void EngineImpl::ChangeState(const State new_state) {
  auto convert_state = [](const State state) {
    switch (state) {
      case State::kIdle:
        return ChatState::kIdle;
      case State::kInitted:
        return ChatState::kInitted;
      case State::kLoadingProtocol:
        return ChatState::kLoading;
      case State::kLoadingProtocolFailed:
        return ChatState::kLoadingFailed;
      case State::kWebsocketConnecting:
      case State::kWebsocketConnectingWithWakeup:
        return ChatState::kConnecting;
      case State::kWebsocketConnectedWithWakeup:
      case State::kWebsocketConnected:
        return ChatState::kConnecting;
      case State::kStandby:
        return ChatState::kStandby;
      case State::kListening:
        return ChatState::kListening;
      case State::kSpeaking:
        return ChatState::kSpeaking;
      default:
        return ChatState::kIdle;
    }
  };

  const auto new_chat_state = convert_state(new_state);

  if (new_chat_state != chat_state_ && observer_) {
    observer_->PushEvent(StateChangedEvent{chat_state_, new_chat_state});
  }

  state_ = new_state;
  chat_state_ = new_chat_state;
}

}  // namespace ai_vox