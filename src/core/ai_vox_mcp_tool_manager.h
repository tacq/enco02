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
    // Sized for the current tool set; avoids a dozen reallocations while the list is being built.
    tools_body_.reserve(2560);
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
