#pragma once

#ifndef _AI_VOX_TYPES_H_
#define _AI_VOX_TYPES_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>

#include "components/cjson_util/cjson_util.h"

namespace ai_vox {
enum class ChatState : uint8_t {
  kIdle,
  kInitted,
  kLoading,
  kLoadingFailed,
  kStandby,
  kConnecting,
  kConnected,
  kConnectfailed,
  kListening,
  kSpeaking,
};

enum class ChatRole : uint8_t {
  kAssistant,
  kUser,
};

struct TextReceivedEvent {
  std::string content;
};

struct TextTranslatedEvent {
  std::string content;
};

struct StateChangedEvent {
  ChatState old_state;
  ChatState new_state;
};

struct ChatMessageEvent {
  ChatRole role;
  std::string content;
};

struct ActivationEvent {
  std::string code;
  std::string message;
};

struct EmotionEvent {
  std::string emotion;
};

struct McpToolCallEvent {
  int64_t id;
  std::string name;
  std::map<std::string, std::variant<std::string, int64_t, bool>> params;

  template <typename T>
  const T* param(const std::string& key) const {
    auto it = params.find(key);
    if (it == params.end()) {
      return nullptr;
    }
    return std::get_if<T>(&it->second);
  }

  std::string ToString() const {
    const auto root_json_obj = cjson_util::MakeUnique();
    cJSON_AddNumberToObject(root_json_obj.get(), "id", id);
    cJSON_AddStringToObject(root_json_obj.get(), "name", name.c_str());
    cJSON* params_json_obj = cJSON_CreateObject();
    for (const auto& [key, value] : params) {
      auto value_json_obj = cJSON_CreateObject();
      if (auto value_ptr = std::get_if<std::string>(&value)) {
        cJSON_AddStringToObject(value_json_obj, "type", "string");
        cJSON_AddStringToObject(value_json_obj, "value", value_ptr->c_str());
      } else if (auto value_ptr = std::get_if<int64_t>(&value)) {
        cJSON_AddStringToObject(value_json_obj, "type", "integer");
        cJSON_AddNumberToObject(value_json_obj, "value", *value_ptr);
      } else if (auto value_ptr = std::get_if<bool>(&value)) {
        cJSON_AddStringToObject(value_json_obj, "type", "boolean");
        cJSON_AddBoolToObject(value_json_obj, "value", *value_ptr);
      }
      cJSON_AddItemToObject(params_json_obj, key.c_str(), value_json_obj);
    }
    cJSON_AddItemToObject(root_json_obj.get(), "params", params_json_obj);
    return cjson_util::ToString(root_json_obj, true);
  };
};

template <typename T>
struct ParamSchema {
  static_assert(false, "You can only use ParamSchema<int64_t>, ParamSchema<std::string>, or ParamSchema<bool>.");
};

template <>
struct ParamSchema<int64_t> {
  std::optional<int64_t> default_value;
  std::optional<int64_t> min;
  std::optional<int64_t> max;

  auto ToJson() const {
    auto json_obj = cjson_util::MakeUnique();
    cJSON_AddStringToObject(json_obj.get(), "type", "integer");
    if (default_value) {
      cJSON_AddNumberToObject(json_obj.get(), "default", *default_value);
    }
    if (min) {
      cJSON_AddNumberToObject(json_obj.get(), "minimum", *min);
    }
    if (max) {
      cJSON_AddNumberToObject(json_obj.get(), "maximum", *max);
    }
    return json_obj;
  }
};

template <>
struct ParamSchema<std::string> {
  std::optional<std::string> default_value;

  auto ToJson() const {
    auto json_obj = cjson_util::MakeUnique();
    cJSON_AddStringToObject(json_obj.get(), "type", "string");
    if (default_value) {
      cJSON_AddStringToObject(json_obj.get(), "default", default_value->c_str());
    }
    return json_obj;
  }
};

template <>
struct ParamSchema<bool> {
  std::optional<bool> default_value;

  auto ToJson() const {
    auto json_obj = cjson_util::MakeUnique();
    cJSON_AddStringToObject(json_obj.get(), "type", "boolean");
    if (default_value) {
      cJSON_AddBoolToObject(json_obj.get(), "default", *default_value);
    }
    return json_obj;
  }
};

using ParamSchemaVariant = std::variant<ParamSchema<int64_t>, ParamSchema<std::string>, ParamSchema<bool>>;

}  // namespace ai_vox

#endif