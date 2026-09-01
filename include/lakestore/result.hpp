#pragma once
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
namespace lakestore {
enum class ErrorCode {
  NotFound,
  AlreadyExists,
  PreconditionFailed,
  Timeout,
  Transient,
  Fatal,
  InvalidArgument,
  Corruption,
  Conflict,
  Expired,
  UnknownOutcome
};
struct Error {
  ErrorCode code;
  std::string message;
};
class Failure : public std::runtime_error {
 public:
  explicit Failure(Error error) : std::runtime_error(error.message), error_(std::move(error)) {}
  const Error& error() const noexcept { return error_; }

 private:
  Error error_;
};
template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}
  Result(Error error) : value_(std::move(error)) {}
  explicit operator bool() const noexcept { return std::holds_alternative<T>(value_); }
  const Error& error() const { return std::get<Error>(value_); }
  T& value() & {
    if (!*this) throw Failure(error());
    return std::get<T>(value_);
  }
  const T& value() const& {
    if (!*this) throw Failure(error());
    return std::get<T>(value_);
  }
  T value() && {
    if (!*this) throw Failure(error());
    return std::move(std::get<T>(value_));
  }

 private:
  std::variant<T, Error> value_;
};
using Status = Result<std::monostate>;
inline Status ok() { return std::monostate{}; }
[[noreturn]] inline void fail(ErrorCode code, std::string message) {
  throw Failure({code, std::move(message)});
}
// Table/format APIs throw typed Failure; only ObjectStore is a Result boundary.
}  // namespace lakestore
