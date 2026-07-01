#pragma once

#include <string>
#include <utility>

namespace modlist {

template <typename T>
class Result {
public:
  static Result Ok(T value) { return Result(true, std::move(value), {}); }
  static Result Error(std::string message) { return Result(false, T{}, std::move(message)); }

  bool ok() const { return ok_; }
  const T& value() const { return value_; }
  T& value() { return value_; }
  const std::string& error() const { return error_; }

private:
  Result(bool ok, T value, std::string error)
      : ok_(ok), value_(std::move(value)), error_(std::move(error)) {}

  bool ok_{false};
  T value_{};
  std::string error_;
};

template <>
class Result<void> {
public:
  static Result Ok() { return Result(true, {}); }
  static Result Error(std::string message) { return Result(false, std::move(message)); }

  bool ok() const { return ok_; }
  const std::string& error() const { return error_; }

private:
  Result(bool ok, std::string error) : ok_(ok), error_(std::move(error)) {}

  bool ok_{false};
  std::string error_;
};

}  // namespace modlist
