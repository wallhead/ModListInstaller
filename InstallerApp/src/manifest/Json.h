#pragma once

#include "common/Result.h"

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace modlist {

class JsonValue {
public:
  using Array = std::vector<JsonValue>;
  using Object = std::map<std::string, JsonValue>;

  JsonValue();
  explicit JsonValue(std::nullptr_t);
  explicit JsonValue(bool value);
  explicit JsonValue(double value);
  explicit JsonValue(std::string value);
  explicit JsonValue(Array value);
  explicit JsonValue(Object value);

  bool IsNull() const;
  bool IsBool() const;
  bool IsNumber() const;
  bool IsString() const;
  bool IsArray() const;
  bool IsObject() const;

  bool AsBool(bool fallback = false) const;
  double AsNumber(double fallback = 0) const;
  const std::string& AsString() const;
  const Array& AsArray() const;
  const Object& AsObject() const;

  const JsonValue* Find(const std::string& key) const;

private:
  std::variant<std::nullptr_t, bool, double, std::string, Array, Object> value_;
};

Result<JsonValue> ParseJson(const std::string& input);

}  // namespace modlist
