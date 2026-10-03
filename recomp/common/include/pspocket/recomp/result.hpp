#pragma once

#include <format>
#include <string>
#include <utility>
#include <variant>

#include "pspocket/core/log.hpp"

// Error handling for the offline toolchain. Parsers never throw and never abort on bad input: malformed
// files come back as an Error with a message that names the problem (CLAUDE.md rule 5 applies to the runtime;
// the toolchain reports and lets the caller decide).
namespace pspocket::recomp {

struct Error {
    std::string message;
};

template <class... Args>
Error make_error(std::format_string<Args...> fmt, Args&&... args)
{
    return Error{std::format(fmt, std::forward<Args>(args)...)};
}

template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : v_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : v_(std::in_place_index<1>, std::move(error)) {}

    bool ok() const { return v_.index() == 0; }
    explicit operator bool() const { return ok(); }

    T& value() &
    {
        PS_CHECK(ok());
        return *std::get_if<0>(&v_);
    }
    const T& value() const&
    {
        PS_CHECK(ok());
        return *std::get_if<0>(&v_);
    }
    T&& value() &&
    {
        PS_CHECK(ok());
        return std::move(*std::get_if<0>(&v_));
    }

    const Error& error() const
    {
        PS_CHECK(!ok());
        return *std::get_if<1>(&v_);
    }

    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }

private:
    std::variant<T, Error> v_;
};

} // namespace pspocket::recomp
