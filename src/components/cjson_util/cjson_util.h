#pragma once

#ifndef _CJSON_UTIL_H_
#define _CJSON_UTIL_H_

#include <memory>
#include <optional>
#include <string>

#include "cJSON.h"

namespace cjson_util {

struct CjsonFreeDeleter {
  void operator()(void* obj) const {
    if (obj != nullptr) {
      cJSON_free(obj);
    }
  }
};

struct CjsonDeleter {
  void operator()(cJSON* obj) const {
    if (obj != nullptr) {
      cJSON_Delete(obj);
    }
  }
};

std::unique_ptr<cJSON, CjsonDeleter> MakeUnique();
std::unique_ptr<cJSON, CjsonDeleter> MakeUnique(cJSON* obj);
std::unique_ptr<cJSON, CjsonDeleter> ArrayMakeUnique();
std::string ToString(const cJSON* const obj, const bool format = false);
std::string ToString(const std::unique_ptr<cJSON, CjsonDeleter>& obj, const bool format = false);
std::optional<int64_t> GetNumber(const cJSON* const obj, const char * const name);
std::optional<std::string> GetString(const cJSON* const obj, const char * const name);
}  // namespace cjson_util

#endif