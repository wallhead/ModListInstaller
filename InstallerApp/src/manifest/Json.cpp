#include "manifest/Json.h"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace modlist {

namespace {

int HexValue(char c);
void AppendUtf8(std::string& out, uint32_t codepoint);

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
          AppendUtf8(out, ParseUnicodeEscape());
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

  uint32_t ParseUnicodeEscape() {
    uint32_t value = ReadUnicodeWord();
    if (value >= 0xD800 && value <= 0xDBFF) {
      if (pos_ + 2 > input_.size() || input_[pos_] != '\\' || input_[pos_ + 1] != 'u') {
        throw std::runtime_error("Invalid JSON unicode surrogate pair");
      }
      pos_ += 2;
      const uint32_t low = ReadUnicodeWord();
      if (low < 0xDC00 || low > 0xDFFF) {
        throw std::runtime_error("Invalid JSON unicode surrogate pair");
      }
      value = 0x10000 + ((value - 0xD800) << 10) + (low - 0xDC00);
    } else if (value >= 0xDC00 && value <= 0xDFFF) {
      throw std::runtime_error("Invalid JSON unicode surrogate pair");
    }
    return value;
  }

  uint32_t ReadUnicodeWord() {
    if (pos_ + 4 > input_.size()) {
      throw std::runtime_error("Invalid JSON unicode escape");
    }
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const int digit = HexValue(input_[pos_++]);
      if (digit < 0) {
        throw std::runtime_error("Invalid JSON unicode escape");
      }
      value = (value << 4) | static_cast<uint32_t>(digit);
    }
    return value;
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

int HexValue(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return 10 + c - 'a';
  }
  if (c >= 'A' && c <= 'F') {
    return 10 + c - 'A';
  }
  return -1;
}

void AppendUtf8(std::string& out, uint32_t codepoint) {
  if (codepoint <= 0x7F) {
    out.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0x10FFFF) {
    out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    throw std::runtime_error("Invalid JSON unicode codepoint");
  }
}

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
