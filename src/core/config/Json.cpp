#include "core/config/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace bm::json {

namespace {

const Value& NullValue() {
  static const Value kNull;
  return kNull;
}

void EscapeString(std::string& out, const std::string& value) {
  out.push_back('"');
  for (char c : value) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned char>(c));
          out += buffer;
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

void AppendUtf8(std::string& out, uint32_t code) {
  if (code < 0x80) {
    out.push_back(static_cast<char>(code));
  } else if (code < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (code >> 6)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else if (code < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (code >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
  }
}

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  Value ParseDocument() {
    SkipSpace();
    Value value = ParseValue();
    SkipSpace();
    if (pos_ != text_.size()) throw std::runtime_error("JSON 尾部存在多余内容");
    return value;
  }

 private:
  void SkipSpace() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) pos_++;
  }

  char Peek() const {
    if (pos_ >= text_.size()) throw std::runtime_error("JSON 提前结束");
    return text_[pos_];
  }

  void Expect(char expected) {
    if (Peek() != expected) throw std::runtime_error("JSON 结构非法");
    pos_++;
  }

  Value ParseValue() {
    switch (Peek()) {
      case '{': return ParseObject();
      case '[': return ParseArray();
      case '"': return Value(ParseString());
      case 't': ExpectWord("true"); return Value(true);
      case 'f': ExpectWord("false"); return Value(false);
      case 'n': ExpectWord("null"); return Value();
      default: return ParseNumber();
    }
  }

  void ExpectWord(std::string_view word) {
    if (text_.substr(pos_, word.size()) != word) throw std::runtime_error("JSON 字面量非法");
    pos_ += word.size();
  }

  Value ParseObject() {
    Expect('{');
    Value object = Value::MakeObject();
    SkipSpace();
    if (Peek() == '}') {
      pos_++;
      return object;
    }
    while (true) {
      SkipSpace();
      std::string key = ParseString();
      SkipSpace();
      Expect(':');
      SkipSpace();
      object.Set(std::move(key), ParseValue());
      SkipSpace();
      if (Peek() == ',') {
        pos_++;
        continue;
      }
      Expect('}');
      return object;
    }
  }

  Value ParseArray() {
    Expect('[');
    Value array = Value::MakeArray();
    SkipSpace();
    if (Peek() == ']') {
      pos_++;
      return array;
    }
    while (true) {
      SkipSpace();
      array.Push(ParseValue());
      SkipSpace();
      if (Peek() == ',') {
        pos_++;
        continue;
      }
      Expect(']');
      return array;
    }
  }

  std::string ParseString() {
    Expect('"');
    std::string result;
    while (true) {
      if (pos_ >= text_.size()) throw std::runtime_error("JSON 字符串未结束");
      char c = text_[pos_++];
      if (c == '"') return result;
      if (c != '\\') {
        result.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) throw std::runtime_error("JSON 转义非法");
      char escape = text_[pos_++];
      switch (escape) {
        case '"': result.push_back('"'); break;
        case '\\': result.push_back('\\'); break;
        case '/': result.push_back('/'); break;
        case 'b': result.push_back('\b'); break;
        case 'f': result.push_back('\f'); break;
        case 'n': result.push_back('\n'); break;
        case 'r': result.push_back('\r'); break;
        case 't': result.push_back('\t'); break;
        case 'u': result += ParseUnicode(); break;
        default: throw std::runtime_error("JSON 转义非法");
      }
    }
  }

  std::string ParseUnicode() {
    uint32_t first = ParseHex4();
    std::string result;
    if (first >= 0xD800 && first <= 0xDBFF) {
      if (text_.substr(pos_, 2) != "\\u") throw std::runtime_error("JSON 代理对非法");
      pos_ += 2;
      uint32_t second = ParseHex4();
      if (second < 0xDC00 || second > 0xDFFF) throw std::runtime_error("JSON 代理对非法");
      AppendUtf8(result, 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00));
    } else {
      AppendUtf8(result, first);
    }
    return result;
  }

  uint32_t ParseHex4() {
    if (pos_ + 4 > text_.size()) throw std::runtime_error("JSON 转义非法");
    uint32_t value = 0;
    for (int i = 0; i < 4; i++) {
      char c = text_[pos_++];
      value <<= 4;
      if (c >= '0' && c <= '9') value |= static_cast<uint32_t>(c - '0');
      else if (c >= 'a' && c <= 'f') value |= static_cast<uint32_t>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') value |= static_cast<uint32_t>(c - 'A' + 10);
      else throw std::runtime_error("JSON 转义非法");
    }
    return value;
  }

  Value ParseNumber() {
    size_t start = pos_;
    if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) pos_++;
    while (pos_ < text_.size() && ((text_[pos_] >= '0' && text_[pos_] <= '9') || text_[pos_] == '.' || text_[pos_] == 'e' ||
                                   text_[pos_] == 'E' || text_[pos_] == '-' || text_[pos_] == '+'))
      pos_++;
    if (pos_ == start) throw std::runtime_error("JSON 数值非法");
    return Value(std::strtod(std::string(text_.substr(start, pos_ - start)).c_str(), nullptr));
  }

  std::string_view text_;
  size_t pos_ = 0;
};

}  // namespace

Value::Value(bool value) : type_(Type::Bool), bool_(value) {}
Value::Value(int value) : type_(Type::Number), number_(value) {}
Value::Value(int64_t value) : type_(Type::Number), number_(static_cast<double>(value)) {}
Value::Value(double value) : type_(Type::Number), number_(value) {}
Value::Value(std::string value) : type_(Type::String), string_(std::move(value)) {}
Value::Value(const char* value) : type_(Type::String), string_(value) {}

Value Value::MakeObject() {
  Value value;
  value.type_ = Type::Object;
  return value;
}

Value Value::MakeArray() {
  Value value;
  value.type_ = Type::Array;
  return value;
}

Value Value::Parse(std::string_view text) { return Parser(text).ParseDocument(); }

bool Value::AsBool() const {
  if (type_ != Type::Bool) throw std::runtime_error("JSON 字段不是布尔值");
  return bool_;
}

double Value::AsNumber() const {
  if (type_ != Type::Number) throw std::runtime_error("JSON 字段不是数值");
  return number_;
}

int64_t Value::AsInt() const { return static_cast<int64_t>(AsNumber()); }

const std::string& Value::AsString() const {
  if (type_ != Type::String) throw std::runtime_error("JSON 字段不是字符串");
  return string_;
}

const std::vector<Value>& Value::AsArray() const {
  if (type_ != Type::Array) throw std::runtime_error("JSON 字段不是数组");
  return array_;
}

const Value& Value::operator[](std::string_view key) const {
  if (type_ != Type::Object) return NullValue();
  for (const auto& [name, value] : object_)
    if (name == key) return value;
  return NullValue();
}

bool Value::Has(std::string_view key) const {
  if (type_ != Type::Object) return false;
  for (const auto& [name, value] : object_)
    if (name == key) return true;
  return false;
}

void Value::Set(std::string key, Value value) {
  for (auto& [name, existing] : object_) {
    if (name == key) {
      existing = std::move(value);
      return;
    }
  }
  object_.emplace_back(std::move(key), std::move(value));
}

void Value::Push(Value value) { array_.push_back(std::move(value)); }

std::string Value::Dump() const {
  std::string out;
  DumpTo(out);
  return out;
}

void Value::DumpTo(std::string& out) const {
  switch (type_) {
    case Type::Null: out += "null"; break;
    case Type::Bool: out += bool_ ? "true" : "false"; break;
    case Type::Number: {
      char buffer[32];
      if (number_ == std::floor(number_) && std::fabs(number_) < 9.0e15) {
        std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(number_));
      } else {
        std::snprintf(buffer, sizeof(buffer), "%.17g", number_);
      }
      out += buffer;
      break;
    }
    case Type::String: EscapeString(out, string_); break;
    case Type::Array: {
      out.push_back('[');
      for (size_t i = 0; i < array_.size(); i++) {
        if (i) out.push_back(',');
        array_[i].DumpTo(out);
      }
      out.push_back(']');
      break;
    }
    case Type::Object: {
      out.push_back('{');
      for (size_t i = 0; i < object_.size(); i++) {
        if (i) out.push_back(',');
        EscapeString(out, object_[i].first);
        out.push_back(':');
        object_[i].second.DumpTo(out);
      }
      out.push_back('}');
      break;
    }
  }
}

}  // namespace bm::json
