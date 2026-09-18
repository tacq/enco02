#include "cjson_util.h"

namespace cjson_util {
std::unique_ptr<cJSON, CjsonDeleter> MakeUnique() {
  return std::unique_ptr<cJSON, CjsonDeleter>(cJSON_CreateObject());
}

std::unique_ptr<cJSON, CjsonDeleter> MakeUnique(cJSON* obj) {
  return std::unique_ptr<cJSON, CjsonDeleter>(obj);
}

std::unique_ptr<cJSON, CjsonDeleter> ArrayMakeUnique() {
  return std::unique_ptr<cJSON, CjsonDeleter>(cJSON_CreateArray());
}

std::string ToString(const cJSON* const obj, const bool format) {
  return format ? std::unique_ptr<char, CjsonFreeDeleter>(cJSON_Print(obj)).get()
                : std::unique_ptr<char, CjsonFreeDeleter>(cJSON_PrintUnformatted(obj)).get();
}

std::string ToString(const std::unique_ptr<cJSON, CjsonDeleter>& obj, const bool format) {
  return ToString(obj.get(), format);
}

std::optional<int64_t> GetNumber(const cJSON* const obj, const char* const name) {
  auto item = cJSON_GetObjectItem(obj, name);
  if (cJSON_IsNumber(item)) {
    return item->valuedouble;
  }
  return std::nullopt;
}
std::optional<std::string> GetString(const cJSON* const obj, const char* const name) {
  auto item = cJSON_GetObjectItem(obj, name);
  if (cJSON_IsString(item)) {
    return item->valuestring;
  }
  return std::nullopt;
}
}  // namespace cjson_util
