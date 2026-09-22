#pragma once

#ifndef _AI_VOX_MCP_TOOL_MANAGER_H_
#define _AI_VOX_MCP_TOOL_MANAGER_H_

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "ai_vox_types.h"
#include "components/cjson_util/cjson_util.h"

namespace ai_vox {
namespace mcp {
struct Tool {
  std::string description;
  std::map<std::string, ParamSchemaVariant> param_schemas;

  auto ToJson() const {
    auto root_json_obj = cjson_util::MakeUnique();
    cJSON_AddStringToObject(root_json_obj.get(), "description", description.c_str());

    cJSON *input_schema_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(input_schema_obj, "type", "object");

    if (!param_schemas.empty()) {
      cJSON *required_array_obj = cJSON_CreateArray();
      cJSON *properties_obj = cJSON_CreateObject();

      for (const auto &[k, v] : param_schemas) {
        if (auto param_scheme = std::get_if<ParamSchema<int64_t>>(&v)) {
          auto p_obj = param_scheme->ToJson();
          if (p_obj) {
            cJSON_AddItemToObject(properties_obj, k.c_str(), p_obj.release());
          }
          if (!param_scheme->default_value) {
            cJSON_AddItemToArray(required_array_obj, cJSON_CreateString(k.c_str()));
          }
        } else if (auto param_scheme = std::get_if<ParamSchema<std::string>>(&v)) {
          auto p_obj = param_scheme->ToJson();
          if (p_obj) {
            cJSON_AddItemToObject(properties_obj, k.c_str(), p_obj.release());
          }
          if (!param_scheme->default_value) {
            cJSON_AddItemToArray(required_array_obj, cJSON_CreateString(k.c_str()));
          }
        } else if (auto param_scheme = std::get_if<ParamSchema<bool>>(&v)) {
          auto p_obj = param_scheme->ToJson();
          if (p_obj) {
            cJSON_AddItemToObject(properties_obj, k.c_str(), p_obj.release());
          }
          if (!param_scheme->default_value) {
            cJSON_AddItemToArray(required_array_obj, cJSON_CreateString(k.c_str()));
          }
        }
      }

      cJSON_AddItemToObject(input_schema_obj, "properties", properties_obj);
      if (cJSON_GetArraySize(required_array_obj) > 0) {
        cJSON_AddItemToObject(input_schema_obj, "required", required_array_obj);
      } else {
        cJSON_Delete(required_array_obj);
      }
    }

    cJSON_AddItemToObject(root_json_obj.get(), "inputSchema", input_schema_obj);

    return root_json_obj;
  }
};

// Tool definitions are only ever needed to answer a single "tools/list" request: dispatch of
// "tools/call" goes straight to the observer by name. Keeping the Tool objects (a long UTF-8
// description plus a map of parameter schemas, times ~12 tools) alive for the lifetime of the
// device cost around 25KB of internal RAM on a board that has to fit a 40KB TLS handshake.
//
// So each tool is serialised exactly once, appended to a pre-reserved string, and then destroyed.
// This also removes the old O(n^2) behaviour where the entire array was re-serialised on every
// AddTool() call, which fragmented the heap 12 times over during boot.
class ToolManager {
 public:
  ToolManager() {
    // Sized from the real payload, not a guess: the server's "tools/list" response measures 4,878
    // bytes on the wire for the current tool set. At the old 2560 this string outgrew its buffer
    // during boot and libstdc++ doubled it to 5120 - holding both buffers at once and leaving a
    // 2.5KB hole behind. Reserving the true size costs nothing and avoids the churn.
    //
    // If you add tools and this number goes stale the only penalty is that one realloc coming back,
    // so it is a soft target rather than something that has to be kept exact.
    tools_body_.reserve(5120);
  }

  void AddTool(std::string name, Tool tool) {
    auto tool_json = tool.ToJson();
    if (!tool_json) {
      return;
    }
    cJSON_AddStringToObject(tool_json.get(), "name", name.c_str());
    const auto serialized = cjson_util::ToString(tool_json);
    if (serialized.empty()) {
      return;
    }
    if (!tools_body_.empty()) {
      tools_body_ += ',';
    }
    tools_body_ += serialized;
    // `tool` (description + param_schemas) is released here, at the end of the scope.
  }

  // Returns the full "tools/list" result payload. Built on demand: it is needed once per session,
  // and holding a second copy permanently would defeat the point of the above.
  std::string GetToolsJsonString() const {
    std::string json;
    json.reserve(tools_body_.size() + 16);
    json += "{\"tools\":[";
    json += tools_body_;
    json += "]}";
    return json;
  }

 private:
  std::string tools_body_;
};
}  // namespace mcp
}  // namespace ai_vox
#endif
