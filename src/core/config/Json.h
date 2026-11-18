#pragma once

#include "core/platform/Common.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 配置与布局快照需要的最小 JSON 实现；字段顺序保持写入顺序
namespace bm::json {

class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Value() = default;
  Value(bool value);
  Value(int value);
  Value(int64_t value);
  Value(double value);
  Value(std::string value);
  Value(const char* value);

  static Value MakeObject();
  static Value MakeArray();
  static Value Parse(std::string_view text);

  Type GetType() const { return type_; }
  bool AsBool() const;
  double AsNumber() const;
  int64_t AsInt() const;
  const std::string& AsString() const;
  const std::vector<Value>& AsArray() const;

  // 对象取值；缺失返回静态 Null，便于链式读取配置
  const Value& operator[](std::string_view key) const;
  bool Has(std::string_view key) const;

  void Set(std::string key, Value value);
  void Push(Value value);

  std::string Dump() const;

 private:
  void DumpTo(std::string& out) const;

  Type type_ = Type::Null;
  bool bool_ = false;
  double number_ = 0;
  std::string string_;
  std::vector<Value> array_;
  std::vector<std::pair<std::string, Value>> object_;
};

}  // namespace bm::json
