#pragma once

// 极简 JSON：只覆盖本协议用到的子集（对象 / 数组 / 字符串 / 数字 / 布尔 / null）。
// M1 接入正式库后替换本实现，调用方接口保持不变。

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace rc::json {

class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Value() = default;
  Value(std::nullptr_t) {}
  Value(bool value) : type_(Type::Bool), bool_(value) {}
  Value(const char* text) : type_(Type::String), string_(text) {}
  Value(std::string text) : type_(Type::String), string_(std::move(text)) {}

  template <typename T,
            std::enable_if_t<std::is_arithmetic_v<T> &&
                             !std::is_same_v<T, bool>, int> = 0>
  Value(T number) : type_(Type::Number), number_(static_cast<double>(number)) {}

  static Value make_object();
  static Value make_array();

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_object() const { return type_ == Type::Object; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_string() const { return type_ == Type::String; }
  bool is_number() const { return type_ == Type::Number; }

  void set(std::string_view key, Value value);
  const Value* get(std::string_view key) const;
  void push(Value value);
  std::size_t size() const;
  const Value& at(std::size_t index) const;

  std::string as_string(std::string_view fallback = "") const;
  int64_t as_int(int64_t fallback = 0) const;
  double as_double(double fallback = 0.0) const;
  bool as_bool(bool fallback = false) const;

  std::string dump() const;

  // 解析失败抛出 std::runtime_error。
  static Value parse(std::string_view text);

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  std::vector<Value> array_;
  std::vector<std::pair<std::string, Value>> object_;
};

}  // namespace rc::json
