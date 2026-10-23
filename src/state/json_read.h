// A small JSON reader (t2, 2026-10-23) for the patch column of the firmware's preset databases
// ({"type":"fm","knobs":[...],...}). Parses into a tree; depth and size are bounded.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace op1::state {

struct Json {
  enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;

  const Json* get(const std::string& key) const {
    for (const auto& kv : obj)
      if (kv.first == key) return &kv.second;
    return nullptr;
  }
};

bool json_parse(const std::string& text, Json& out, std::string& err);

}  // namespace op1::state
