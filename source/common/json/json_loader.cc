#include "common/json/json_loader.h"

#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stack>
#include <string>
#include <vector>

#include "common/common/assert.h"
#include "common/common/fmt.h"
#include "common/common/hash.h"
#include "common/common/utility.h"
#include "common/protobuf/utility.h"

// Do not let nlohmann/json leak outside of this file.
#include "include/nlohmann/json.hpp"

#include "absl/strings/match.h"

namespace Envoy {
namespace Json {

namespace {
/**
 * Internal representation of Object.
 */
class Field;
using FieldSharedPtr = std::shared_ptr<Field>;

class Field : public Object {
public:
  // Container factories for handler.
  static FieldSharedPtr createObject() { return FieldSharedPtr{new Field(Type::Object)}; }
  static FieldSharedPtr createArray() { return FieldSharedPtr{new Field(Type::Array)}; }
  static FieldSharedPtr createNull() { return FieldSharedPtr{new Field(Type::Null)}; }

  bool isNull() const override { return type_ == Type::Null; }
  bool isArray() const override { return type_ == Type::Array; }
  bool isObject() const override { return type_ == Type::Object; }

  // Value factory.
  template <typename T> static FieldSharedPtr createValue(T value) {
    return FieldSharedPtr{new Field(value)}; // NOLINT(modernize-make-shared)
  }

  void append(FieldSharedPtr field_ptr) {
    checkType(Type::Array);
    value_.array_value_.push_back(field_ptr);
  }
  void insert(const std::string& key, FieldSharedPtr field_ptr) {
    checkType(Type::Object);
    value_.object_value_[key] = field_ptr;
  }

  uint64_t hash() const override;

  bool getBoolean(const std::string& name) const override;
  bool getBoolean(const std::string& name, bool default_value) const override;
  double getDouble(const std::string& name) const override;
  double getDouble(const std::string& name, double default_value) const override;
  int64_t getInteger(const std::string& name) const override;
  int64_t getInteger(const std::string& name, int64_t default_value) const override;
  ObjectSharedPtr getObject(const std::string& name, bool allow_empty) const override;
  std::vector<ObjectSharedPtr> getObjectArray(const std::string& name,
                                              bool allow_empty) const override;
  std::string getString(const std::string& name) const override;
  std::string getString(const std::string& name, const std::string& default_value) const override;
  std::vector<std::string> getStringArray(const std::string& name, bool allow_empty) const override;
  std::vector<ObjectSharedPtr> asObjectArray() const override;
  std::string asString() const override { return stringValue(); }
  bool asBoolean() const override { return booleanValue(); }
  double asDouble() const override { return doubleValue(); }
  int64_t asInteger() const override { return integerValue(); }
  std::string asJsonString() const override;

  bool empty() const override;
  bool hasObject(const std::string& name) const override;
  void iterate(const ObjectCallback& callback) const override;
  void validateSchema(const std::string&) const override;

private:
  enum class Type {
    Array,
    Boolean,
    Double,
    Integer,
    Null,
    Object,
    String,
  };
  static const char* typeAsString(Type t) {
    switch (t) {
    case Type::Array:
      return "Array";
    case Type::Boolean:
      return "Boolean";
    case Type::Double:
      return "Double";
    case Type::Integer:
      return "Integer";
    case Type::Null:
      return "Null";
    case Type::Object:
      return "Object";
    case Type::String:
      return "String";
    }

    NOT_REACHED_GCOVR_EXCL_LINE;
  }

  struct Value {
    std::vector<FieldSharedPtr> array_value_;
    bool boolean_value_;
    double double_value_;
    int64_t integer_value_;
    std::map<std::string, FieldSharedPtr> object_value_;
    std::string string_value_;
  };

  explicit Field(Type type) : type_(type) {}
  explicit Field(const std::string& value) : type_(Type::String) { value_.string_value_ = value; }
  explicit Field(int64_t value) : type_(Type::Integer) { value_.integer_value_ = value; }
  explicit Field(double value) : type_(Type::Double) { value_.double_value_ = value; }
  explicit Field(bool value) : type_(Type::Boolean) { value_.boolean_value_ = value; }

  bool isType(Type type) const { return type == type_; }
  void checkType(Type type) const {
    if (!isType(type)) {
      throw Exception(
          fmt::format("JSON field accessed with type '{}' does not match actual type '{}'.",
                      typeAsString(type), typeAsString(type_)));
    }
  }

  // Value return type functions.
  std::string stringValue() const {
    checkType(Type::String);
    return value_.string_value_;
  }
  std::vector<FieldSharedPtr> arrayValue() const {
    checkType(Type::Array);
    return value_.array_value_;
  }
  bool booleanValue() const {
    checkType(Type::Boolean);
    return value_.boolean_value_;
  }
  double doubleValue() const {
    checkType(Type::Double);
    return value_.double_value_;
  }
  int64_t integerValue() const {
    checkType(Type::Integer);
    return value_.integer_value_;
  }

  nlohmann::json asJsonDocument() const;
  static void buildJsonDocument(const Field& field, nlohmann::json& value);

  const Type type_;
  Value value_;
};

/**
 * Consume events from SAX callbacks to build JSON Field.
 */
class ObjectHandler : public nlohmann::json_sax<nlohmann::json> {
public:
  ObjectHandler() : state_(State::ExpectRoot) {}

  bool start_object(std::size_t) override;
  bool end_object() override;
  bool key(std::string& val) override;
  bool start_array(std::size_t) override;
  bool end_array() override;
  bool boolean(bool value) override { return handleValueEvent(Field::createValue(value)); }
  bool number_integer(int64_t value) override {
    return handleValueEvent(Field::createValue(static_cast<int64_t>(value)));
  }
  bool number_unsigned(uint64_t value) override {
    return handleValueEvent(Field::createValue(static_cast<int64_t>(value)));
  }
  bool number_float(double value, const std::string&) override {
    return handleValueEvent(Field::createValue(value));
  }
  bool null() override { return handleValueEvent(Field::createNull()); }
  bool string(std::string& value) override { return handleValueEvent(Field::createValue(value)); }
  bool binary(binary_t&) override { return false; }
  bool parse_error(std::size_t position, const std::string& token,
                   const nlohmann::detail::exception& ex) override {
    error_offset_ = position;
    error_ = ex.what();
    error_token_ = token;
    return true;
  }

  bool hasParseError() { return !error_.empty(); }
  std::size_t getErrorOffset() { return error_offset_; }
  std::string getParseError() { return error_;}
  std::string getErrorToken() { return error_token_; }

  ObjectSharedPtr getRoot() { return root_; }

private:
  bool handleValueEvent(FieldSharedPtr ptr);

  enum class State {
    ExpectRoot,
    ExpectKeyOrEndObject,
    ExpectValueOrStartObjectArray,
    ExpectArrayValueOrEndArray,
    ExpectFinished,
  };
  State state_;

  std::stack<FieldSharedPtr> stack_;
  std::string key_;

  FieldSharedPtr root_;

  std::string error_;
  std::string error_token_;
  std::size_t error_offset_;
};

void Field::buildJsonDocument(const Field& field, nlohmann::json& value) {
  switch (field.type_) {
  case Type::Array: {
    for (const auto& element : field.value_.array_value_) {
      switch (element->type_) {
      case Type::Array:
      case Type::Object: {
        nlohmann::json nested_value;
        buildJsonDocument(*element, nested_value);
        value.push_back(nested_value);
        break;
      }
      case Type::Boolean:
        value.push_back(element->value_.boolean_value_);
        break;
      case Type::Double:
        value.push_back(element->value_.double_value_);
        break;
      case Type::Integer:
        value.push_back(element->value_.integer_value_);
        break;
      case Type::Null:
        value.push_back(nlohmann::json::value_t::null);
        break;
      case Type::String:
        value.push_back(element->value_.string_value_);
      }
    }
    break;
  }
  case Type::Object: {
    for (const auto& item : field.value_.object_value_) {
      auto name = std::string(item.first.c_str());

      switch (item.second->type_) {
      case Type::Array:
      case Type::Object: {
        nlohmann::json nested_value;
        buildJsonDocument(*item.second, nested_value);
        value.emplace(name, nested_value);
        break;
      }
      case Type::Boolean:
        value.emplace(name, item.second->value_.boolean_value_);
        break;
      case Type::Double:
        value.emplace(name, item.second->value_.double_value_);
        break;
      case Type::Integer:
        value.emplace(name, item.second->value_.integer_value_);
        break;
      case Type::Null:
        value.emplace(name, nlohmann::json::value_t::null);
        break;
      case Type::String:
        value.emplace(name, item.second->value_.string_value_);
        break;
      }
    }
    break;
  }
  case Type::Null: {
    break;
  }
  default:
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
}

nlohmann::json Field::asJsonDocument() const {
  nlohmann::json j;
  buildJsonDocument(*this, j);
  return j;
}

uint64_t Field::hash() const {
  return HashUtil::xxHash64(asJsonString());
}

bool Field::getBoolean(const std::string& name) const {
  checkType(Type::Object);
  auto value_itr = value_.object_value_.find(name);
  if (value_itr == value_.object_value_.end() || !value_itr->second->isType(Type::Boolean)) {
    throw Exception(fmt::format("key '{}' missing or not a boolean", name));
  }
  return value_itr->second->booleanValue();
}

bool Field::getBoolean(const std::string& name, bool default_value) const {
  checkType(Type::Object);
  auto value_itr = value_.object_value_.find(name);
  if (value_itr != value_.object_value_.end()) {
    return getBoolean(name);
  } else {
    return default_value;
  }
}

double Field::getDouble(const std::string& name) const {
  checkType(Type::Object);
  auto value_itr = value_.object_value_.find(name);
  if (value_itr == value_.object_value_.end() || !value_itr->second->isType(Type::Double)) {
    throw Exception(fmt::format("key '{}' missing or not a double", name));
  }
  return value_itr->second->doubleValue();
}

double Field::getDouble(const std::string& name, double default_value) const {
  checkType(Type::Object);
  auto value_itr = value_.object_value_.find(name);
  if (value_itr != value_.object_value_.end()) {
    return getDouble(name);
  } else {
    return default_value;
  }
}

int64_t Field::getInteger(const std::string& name) const {
  checkType(Type::Object);
  auto value_itr = value_.object_value_.find(name);
  if (value_itr == value_.object_value_.end() || !value_itr->second->isType(Type::Integer)) {
    throw Exception(fmt::format("key '{}' missing or not an integer", name));
  }
  return value_itr->second->integerValue();
}

int64_t Field::getInteger(const std::string& name, int64_t default_value) const {
  checkType(Type::Object);
  auto value_itr = value_.object_value_.find(name);
  if (value_itr != value_.object_value_.end()) {
    return getInteger(name);
  } else {
    return default_value;
  }
}

ObjectSharedPtr Field::getObject(const std::string& name, bool allow_empty) const {
  checkType(Type::Object);
  auto value_itr = value_.object_value_.find(name);
  if (value_itr == value_.object_value_.end()) {
    if (allow_empty) {
      return createObject();
    } else {
      throw Exception(fmt::format("key '{}' missing from lines", name));
    }
  } else if (!value_itr->second->isType(Type::Object)) {
    throw Exception(fmt::format("key '{}' not an object", name));
  } else {
    return value_itr->second;
  }
}

std::vector<ObjectSharedPtr> Field::getObjectArray(const std::string& name,
                                                   bool allow_empty) const {
  checkType(Type::Object);
  auto value_itr = value_.object_value_.find(name);
  if (value_itr == value_.object_value_.end() || !value_itr->second->isType(Type::Array)) {
    if (allow_empty && value_itr == value_.object_value_.end()) {
      return std::vector<ObjectSharedPtr>();
    }
    throw Exception(fmt::format("key '{}' missing or not an array", name));
  }

  std::vector<FieldSharedPtr> array_value = value_itr->second->arrayValue();
  return {array_value.begin(), array_value.end()};
}

std::string Field::getString(const std::string& name) const {
  checkType(Type::Object);
  auto value_itr = value_.object_value_.find(name);
  if (value_itr == value_.object_value_.end() || !value_itr->second->isType(Type::String)) {
    throw Exception(fmt::format("key '{}' missing or not a string", name));
  }
  return value_itr->second->stringValue();
}

std::string Field::getString(const std::string& name, const std::string& default_value) const {
  checkType(Type::Object);
  auto value_itr = value_.object_value_.find(name);
  if (value_itr != value_.object_value_.end()) {
    return getString(name);
  } else {
    return default_value;
  }
}

std::vector<std::string> Field::getStringArray(const std::string& name, bool allow_empty) const {
  checkType(Type::Object);
  std::vector<std::string> string_array;
  auto value_itr = value_.object_value_.find(name);
  if (value_itr == value_.object_value_.end() || !value_itr->second->isType(Type::Array)) {
    if (allow_empty && value_itr == value_.object_value_.end()) {
      return string_array;
    }
    throw Exception(fmt::format("key '{}' missing or not an array", name));
  }

  std::vector<FieldSharedPtr> array = value_itr->second->arrayValue();
  string_array.reserve(array.size());
  for (const auto& element : array) {
    if (!element->isType(Type::String)) {
      throw Exception(fmt::format("JSON array '{}' does not contain all strings", name));
    }
    string_array.push_back(element->stringValue());
  }

  return string_array;
}

std::vector<ObjectSharedPtr> Field::asObjectArray() const {
  checkType(Type::Array);
  return {value_.array_value_.begin(), value_.array_value_.end()};
}

std::string Field::asJsonString() const {
  nlohmann::json j = asJsonDocument();
  return j.dump();
}

bool Field::empty() const {
  if (isType(Type::Object)) {
    return value_.object_value_.empty();
  } else if (isType(Type::Array)) {
    return value_.array_value_.empty();
  } else {
    throw Exception(
        fmt::format("Json does not support empty() on types other than array and object"));
  }
}

bool Field::hasObject(const std::string& name) const {
  checkType(Type::Object);
  auto value_itr = value_.object_value_.find(name);
  return value_itr != value_.object_value_.end();
}

void Field::iterate(const ObjectCallback& callback) const {
  checkType(Type::Object);
  for (const auto& item : value_.object_value_) {
    bool stop_iteration = !callback(item.first, *item.second);
    if (stop_iteration) {
      break;
    }
  }
}

void Field::validateSchema(const std::string&) const {
  throw Exception("not implemented");
}

bool ObjectHandler::start_object(std::size_t) {
  FieldSharedPtr object = Field::createObject();

  switch (state_) {
  case State::ExpectValueOrStartObjectArray:
    stack_.top()->insert(key_, object);
    stack_.push(object);
    state_ = State::ExpectKeyOrEndObject;
    return true;
  case State::ExpectArrayValueOrEndArray:
    stack_.top()->append(object);
    stack_.push(object);
    state_ = State::ExpectKeyOrEndObject;
    return true;
  case State::ExpectRoot:
    root_ = object;
    stack_.push(object);
    state_ = State::ExpectKeyOrEndObject;
    return true;
  default:
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
}

bool ObjectHandler::end_object() {
  switch (state_) {
  case State::ExpectKeyOrEndObject:
    stack_.pop();

    if (stack_.empty()) {
      state_ = State::ExpectFinished;
    } else if (stack_.top()->isObject()) {
      state_ = State::ExpectKeyOrEndObject;
    } else if (stack_.top()->isArray()) {
      state_ = State::ExpectArrayValueOrEndArray;
    }
    return true;
  default:
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
}

bool ObjectHandler::key(std::string& val) {
  switch (state_) {
  case State::ExpectKeyOrEndObject:
    key_ = val;
    state_ = State::ExpectValueOrStartObjectArray;
    return true;
  default:
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
}

bool ObjectHandler::start_array(std::size_t) {
  FieldSharedPtr array = Field::createArray();

  switch (state_) {
  case State::ExpectValueOrStartObjectArray:
    stack_.top()->insert(key_, array);
    stack_.push(array);
    state_ = State::ExpectArrayValueOrEndArray;
    return true;
  case State::ExpectArrayValueOrEndArray:
    stack_.top()->append(array);
    stack_.push(array);
    return true;
  case State::ExpectRoot:
    root_ = array;
    stack_.push(array);
    state_ = State::ExpectArrayValueOrEndArray;
    return true;
  default:
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
}

bool ObjectHandler::end_array() {
  switch (state_) {
  case State::ExpectArrayValueOrEndArray:
    stack_.pop();

    if (stack_.empty()) {
      state_ = State::ExpectFinished;
    } else if (stack_.top()->isObject()) {
      state_ = State::ExpectKeyOrEndObject;
    } else if (stack_.top()->isArray()) {
      state_ = State::ExpectArrayValueOrEndArray;
    }

    return true;
  default:
    NOT_REACHED_GCOVR_EXCL_LINE;
  }
}

bool ObjectHandler::handleValueEvent(FieldSharedPtr ptr) {
  switch (state_) {
  case State::ExpectValueOrStartObjectArray:
    state_ = State::ExpectKeyOrEndObject;
    stack_.top()->insert(key_, ptr);
    return true;
  case State::ExpectArrayValueOrEndArray:
    stack_.top()->append(ptr);
    return true;
  default:
    return false;
  }
}

} // namespace

ObjectSharedPtr Factory::loadFromString(const std::string& json) {
  ObjectHandler handler;

  nlohmann::json::sax_parse(json, &handler);

  if (handler.hasParseError()) {
    throw Exception(fmt::format("JSON supplied is not valid. Error(offset {}, token {}): {}\n",
                                handler.getErrorOffset(), handler.getErrorToken(),
                                handler.getParseError()));
  }

  return handler.getRoot();
}

} // namespace Json
} // namespace Envoy
