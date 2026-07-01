#include "manifest/Json.h"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace modlist {

namespace {

class Parser {
public:
  explicit Parser(const std::string& input) : input_(input) {}

  Result<JsonValue> Parse() {
    try {
      SkipWhitespace();
      JsonValue value = ParseValue();
      SkipWhitespace();
      if (pos_ != input_.size()) {
        return Result<JsonValue>::Error("Unexpected trailing JSON content");
      }
      return Result<JsonValue>::Ok(std::move(value));
    } catch (const std::exception& ex) {
      return Result<JsonValue>::Error(ex.what());
    }
  }

private:
  JsonValue ParseValue() {
    SkipWhitespace();
    if (pos_ >= input_.size()) {
      throw std::runtime_error("Unexpected end of JSON");
    }
    const char c = input_[pos_];
    if (c == '{') {
      return JsonValue(ParseObject());
    }
    if (c == '[') {
      return JsonValue(ParseArray());
    }
    if (c == '"') {
      return JsonValue(ParseString());
    }
    if (c == 't') {
      ConsumeLiteral("true");
      return JsonValue(true);
    }
    if (c == 'f') {
      ConsumeLiteral("false");
      return JsonValue(false);
    }
    if (c == 'n') {
      ConsumeLiteral("null");
      return JsonValue(nullptr);
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      return JsonValue(ParseNumber());
    }
    throw std::runtime_error("Invalid JSON value");
  }

  JsonValue::Object ParseObject() {
    Expect('{');
    JsonValue::Object object;
    SkipWhitespace();
    if (Peek('}')) {
      ++pos_;
      return object;
    }
    while (true) {
      SkipWhitespace();
      if (!Peek('"')) {
        throw std::runtime_error("Expected JSON object key");
      }
      std::string key = ParseString();
      SkipWhitespace();
      Expect(':');
      object.emplace(std::move(key), ParseValue());
      SkipWhitespace();
      if (Peek('}')) {
        ++pos_;
        return object;
      }
      Expect(',');
    }
  }

  JsonValue::Array ParseArray() {
    Expect('[');
    JsonValue::Array array;
    SkipWhitespace();
    if (Peek(']')) {
      ++pos_;
      return array;
    }
    while (true) {
      array.push_back(ParseValue());
      SkipWhitespace();
      if (Peek(']')) {
        ++pos_;
        return array;
      }
      Expect(',');
    }
  }

  std::string ParseString() {
    Expect('"');
    std::string out;
    while (pos_ < input_.size()) {
      const char c = input_[pos_++];
      if (c == '"') {
        return out;
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= input_.size()) {
        throw std::runtime_error("Invalid JSON escape");
      }
      const char escaped = input_[pos_++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          out.push_back(escaped);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u':
          if (pos_ + 4 > input_.size()) {
            throw std::runtime_error("Invalid JSON unicode escape");
          }
          out.push_back('?');
          pos_ += 4;
          break;
        default:
          throw std::runtime_error("Invalid JSON escape");
      }
    }
    throw std::runtime_error("Unterminated JSON string");
  }

  double ParseNumber() {
    const size_t start = pos_;
    if (Peek('-')) {
      ++pos_;
    }
    ReadDigits();
    if (Peek('.')) {
      ++pos_;
      ReadDigits();
    }
    if (Peek('e') || Peek('E')) {
      ++pos_;
      if (Peek('+') || Peek('-')) {
        ++pos_;
      }
      ReadDigits();
    }
    return std::stod(input_.substr(start, pos_ - start));
  }

  void ReadDigits() {
    const size_t start = pos_;
    while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
      ++pos_;
    }
    if (start == pos_) {
      throw std::runtime_error("Expected JSON number digit");
    }
  }

  void ConsumeLiteral(const char* literal) {
    const std::string text(literal);
    if (input_.substr(pos_, text.size()) != text) {
      throw std::runtime_error("Invalid JSON literal");
    }
    pos_ += text.size();
  }

  void SkipWhitespace() {
    while (pos_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[pos_]))) {
      ++pos_;
    }
  }

  bool Peek(char c) const {
    return pos_ < input_.size() && input_[pos_] == c;
  }

  void Expect(char c) {
    SkipWhitespace();
    if (!Peek(c)) {
      std::ostringstream out;
      out << "Expected '" << c << "'";
      throw std::runtime_error(out.str());
    }
    ++pos_;
  }

  const std::string& input_;
  size_t pos_{0};
};

const std::string kEmptyString;
const JsonValue::Array kEmptyArray;
const JsonValue::Object kEmptyObject;

}  // namespace

JsonValue::JsonValue() : value_(nullptr) {}
JsonValue::JsonValue(std::nullptr_t) : value_(nullptr) {}
JsonValue::JsonValue(bool value) : value_(value) {}
JsonValue::JsonValue(double value) : value_(value) {}
JsonValue::JsonValue(std::string value) : value_(std::move(value)) {}
JsonValue::JsonValue(Array value) : value_(std::move(value)) {}
JsonValue::JsonValue(Object value) : value_(std::move(value)) {}

bool JsonValue::IsNull() const { return std::holds_alternative<std::nullptr_t>(value_); }
bool JsonValue::IsBool() const { return std::holds_alternative<bool>(value_); }
bool JsonValue::IsNumber() const { return std::holds_alternative<double>(value_); }
bool JsonValue::IsString() const { return std::holds_alternative<std::string>(value_); }
bool JsonValue::IsArray() const { return std::holds_alternative<Array>(value_); }
bool JsonValue::IsObject() const { return std::holds_alternative<Object>(value_); }

bool JsonValue::AsBool(bool fallback) const {
  return IsBool() ? std::get<bool>(value_) : fallback;
}

double JsonValue::AsNumber(double fallback) const {
  return IsNumber() ? std::get<double>(value_) : fallback;
}

const std::string& JsonValue::AsString() const {
  return IsString() ? std::get<std::string>(value_) : kEmptyString;
}

const JsonValue::Array& JsonValue::AsArray() const {
  return IsArray() ? std::get<Array>(value_) : kEmptyArray;
}

const JsonValue::Object& JsonValue::AsObject() const {
  return IsObject() ? std::get<Object>(value_) : kEmptyObject;
}

const JsonValue* JsonValue::Find(const std::string& key) const {
  if (!IsObject()) {
    return nullptr;
  }
  const auto& object = std::get<Object>(value_);
  const auto found = object.find(key);
  return found == object.end() ? nullptr : &found->second;
}

Result<JsonValue> ParseJson(const std::string& input) {
  return Parser(input).Parse();
}

}  // namespace modlist
