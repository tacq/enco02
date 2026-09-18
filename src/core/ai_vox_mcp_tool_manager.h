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
    // cJSON_AddStringToObject(root_json_obj, "name", name_.c_str());
    cJSON_AddStringToObject(root_json_obj.get(), "description", description.c_str());

    cJSON *input_schema_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(input_schema_obj, "type", "object");

    cJSON *required_array_obj = cJSON_CreateArray();
    cJSON *properties_obj = cJSON_CreateObject();

    for (const auto &[k, v] : param_schemas) {
      if (auto param_scheme = std::get_if<ParamSchema<int64_t>>(&v)) {
        cJSON_AddItemToObject(properties_obj, k.c_str(), param_scheme->ToJson().release());
        if (!param_scheme->default_value) {
          cJSON_AddItemToArray(required_array_obj, cJSON_CreateString(k.c_str()));
        }
      } else if (auto param_scheme = std::get_if<ParamSchema<std::string>>(&v)) {
        cJSON_AddItemToObject(properties_obj, k.c_str(), param_scheme->ToJson().release());
        if (!param_scheme->default_value) {
          cJSON_AddItemToArray(required_array_obj, cJSON_CreateString(k.c_str()));
        }
      } else if (auto param_scheme = std::get_if<ParamSchema<bool>>(&v)) {
        cJSON_AddItemToObject(properties_obj, k.c_str(), param_scheme->ToJson().release());
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

    cJSON_AddItemToObject(root_json_obj.get(), "inputSchema", input_schema_obj);

    return root_json_obj;
  }
};

class ToolManager {
 public:
  ToolManager() = default;

  void AddTool(std::string name, Tool tool) {
    tools_.insert_or_assign(std::move(name), std::move(tool));
  }

  auto ToJson() {
    auto root_json_obj = cjson_util::MakeUnique();
    cJSON *tools_array_obj = cJSON_CreateArray();

    for (const auto &[name, tool] : tools_) {
      auto tool_json_obj = tool.ToJson().release();
      cJSON_AddStringToObject(tool_json_obj, "name", name.c_str());
      cJSON_AddItemToArray(tools_array_obj, tool_json_obj);
    }

    cJSON_AddItemToObject(root_json_obj.get(), "tools", tools_array_obj);
    return root_json_obj;
  }

 private:
  std::map<std::string, Tool> tools_;
};
}  // namespace mcp
}  // namespace ai_vox
#endif
