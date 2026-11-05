#include "base/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace rc::json {
namespace {

[[noreturn]] void fail(const std::string& what, std::size_t pos) {
  throw std::runtime_error("json parse error: " + what + " at offset " +
                           std::to_string(pos));
}

std::string escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

void append_utf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

class Parser {
 public:
  explicit Parser(std::string_view text) : src_(text) {}

  Value parse_document() {
    Value value = parse_value();
    skip_ws();
    if (!eof()) fail("trailing characters", pos_);
    return value;
  }

 private:
  std::string_view src_;
  std::size_t pos_ = 0;

  bool eof() const { return pos_ >= src_.size(); }
  char peek() const { return pos_ < src_.size() ? src_[pos_] : '\0'; }

  void skip_ws() {
    while (!eof()) {
      char c = src_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  void expect_word(std::string_view word) {
    if (src_.substr(pos_, word.size()) != word) fail("invalid literal", pos_);
    pos_ += word.size();
  }

  Value parse_value() {
    skip_ws();
    if (eof()) fail("unexpected end", pos_);
    switch (peek()) {
      case '{': return parse_object();
      case '[': return parse_array();
      case '"': return Value(parse_string());
      case 't': expect_word("true"); return Value(true);
      case 'f': expect_word("false"); return Value(false);
      case 'n': expect_word("null"); return Value();
      default: return parse_number();
    }
  }

  Value parse_object() {
    Value object = Value::make_object();
    ++pos_;  // {
    skip_ws();
    if (peek() == '}') {
      ++pos_;
      return object;
    }
    while (true) {
      skip_ws();
      if (peek() != '"') fail("expected object key", pos_);
      std::string key = parse_string();
      skip_ws();
      if (peek() != ':') fail("expected ':'", pos_);
      ++pos_;
      object.set(key, parse_value());
      skip_ws();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == '}') {
        ++pos_;
        break;
      }
      fail("expected ',' or '}'", pos_);
    }
    return object;
  }

  Value parse_array() {
    Value array = Value::make_array();
    ++pos_;  // [
    skip_ws();
    if (peek() == ']') {
      ++pos_;
      return array;
    }
    while (true) {
      array.push(parse_value());
      skip_ws();
      if (peek() == ',') {
        ++pos_;
        continue;
      }
      if (peek() == ']') {
        ++pos_;
        break;
      }
      fail("expected ',' or ']'", pos_);
    }
    return array;
  }

  std::string parse_string() {
    ++pos_;  // "
    std::string out;
    while (true) {
      if (eof()) fail("unterminated string", pos_);
      char c = src_[pos_++];
      if (c == '"') break;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (eof()) fail("unterminated escape", pos_);
      char esc = src_[pos_++];
      switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          uint32_t cp = parse_hex4();
          if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < src_.size() &&
              src_[pos_] == '\\' && src_[pos_ + 1] == 'u') {
            pos_ += 2;
            uint32_t low = parse_hex4();
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          }
          append_utf8(out, cp);
          break;
        }
        default: fail("invalid escape", pos_);
      }
    }
    return out;
  }

  uint32_t parse_hex4() {
    if (pos_ + 4 > src_.size()) fail("bad unicode escape", pos_);
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      char c = src_[pos_++];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        fail("bad hex digit", pos_);
      }
    }
    return value;
  }

  Value parse_number() {
    std::size_t start = pos_;
    if (peek() == '-' || peek() == '+') ++pos_;
    while (!eof()) {
      char c = src_[pos_];
      if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
          c == '+' || c == '-') {
        ++pos_;
      } else {
        break;
      }
    }
    if (pos_ == start) fail("unexpected character", pos_);
    const std::string text(src_.substr(start, pos_ - start));
    return Value(std::strtod(text.c_str(), nullptr));
  }
};

}  // namespace

Value Value::make_object() {
  Value v;
  v.type_ = Type::Object;
  return v;
}

Value Value::make_array() {
  Value v;
  v.type_ = Type::Array;
  return v;
}

void Value::set(std::string_view key, Value value) {
  if (type_ != Type::Object) {
    type_ = Type::Object;
    array_.clear();
    string_.clear();
    object_.clear();
  }
  for (auto& entry : object_) {
    if (entry.first == key) {
      entry.second = std::move(value);
      return;
    }
  }
  object_.emplace_back(std::string(key), std::move(value));
}

const Value* Value::get(std::string_view key) const {
  if (type_ != Type::Object) return nullptr;
  for (const auto& entry : object_) {
    if (entry.first == key) return &entry.second;
  }
  return nullptr;
}

void Value::push(Value value) {
  if (type_ != Type::Array) {
    type_ = Type::Array;
    object_.clear();
    string_.clear();
    array_.clear();
  }
  array_.push_back(std::move(value));
}

std::size_t Value::size() const {
  if (type_ == Type::Array) return array_.size();
  if (type_ == Type::Object) return object_.size();
  return 0;
}

const Value& Value::at(std::size_t index) const {
  static const Value kNull;
  if (type_ != Type::Array || index >= array_.size()) return kNull;
  return array_[index];
}

std::string Value::as_string(std::string_view fallback) const {
  if (type_ == Type::String) return string_;
  if (type_ == Type::Number) return std::to_string(static_cast<int64_t>(number_));
  return std::string(fallback);
}

int64_t Value::as_int(int64_t fallback) const {
  if (type_ == Type::Number) return static_cast<int64_t>(std::llround(number_));
  if (type_ == Type::String) {
    try {
      return std::stoll(string_);
    } catch (...) {
      return fallback;
    }
  }
  if (type_ == Type::Bool) return bool_ ? 1 : 0;
  return fallback;
}

double Value::as_double(double fallback) const {
  if (type_ == Type::Number) return number_;
  if (type_ == Type::String) {
    try {
      return std::stod(string_);
    } catch (...) {
      return fallback;
    }
  }
  return fallback;
}

bool Value::as_bool(bool fallback) const {
  if (type_ == Type::Bool) return bool_;
  if (type_ == Type::Number) return number_ != 0.0;
  if (type_ == Type::String) return string_ == "true" || string_ == "1";
  return fallback;
}

std::string Value::dump() const {
  switch (type_) {
    case Type::Null: return "null";
    case Type::Bool: return bool_ ? "true" : "false";
    case Type::Number: {
      if (std::isfinite(number_) && std::fabs(number_) < 9.0e18 &&
          number_ == std::floor(number_)) {
        return std::to_string(static_cast<int64_t>(number_));
      }
      char buf[40];
      std::snprintf(buf, sizeof(buf), "%.17g", number_);
      return buf;
    }
    case Type::String: return "\"" + escape(string_) + "\"";
    case Type::Array: {
      std::string out = "[";
      for (std::size_t i = 0; i < array_.size(); ++i) {
        if (i) out += ",";
        out += array_[i].dump();
      }
      out += "]";
      return out;
    }
    case Type::Object: {
      std::string out = "{";
      for (std::size_t i = 0; i < object_.size(); ++i) {
        if (i) out += ",";
        out += "\"" + escape(object_[i].first) + "\":" + object_[i].second.dump();
      }
      out += "}";
      return out;
    }
  }
  return "null";
}

Value Value::parse(std::string_view text) {
  Parser parser(text);
  return parser.parse_document();
}

}  // namespace rc::json
