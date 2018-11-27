// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdint.h>
#include <zircon/time.h>

namespace zx {

class duration {
public:
    constexpr duration() = default;

    explicit constexpr duration(zx_duration_t value)
        : value_(value) {}

    static constexpr duration infinite() { return duration(ZX_TIME_INFINITE); }

    static constexpr duration infinite_past() { return duration(ZX_TIME_INFINITE_PAST); }

    constexpr zx_duration_t get() const { return value_; }

    constexpr duration operator+(duration other) const {
        return duration(zx_duration_add_duration(value_, other.value_));
    }

    constexpr duration operator-(duration other) const {
        return duration(zx_duration_sub_duration(value_, other.value_));
    }

    constexpr duration operator*(int64_t multiplier) const {
        return duration(zx_duration_mul_int64(value_, multiplier));
    }

    constexpr duration operator/(int64_t divisor) const {
        return duration(value_ / divisor);
    }

    constexpr duration operator%(duration divisor) const {
        return duration(value_ % divisor.value_);
    }

    constexpr int64_t operator/(duration other) const {
        return value_ / other.value_;
    }

    constexpr duration& operator+=(duration other) {
        value_ = zx_duration_add_duration(value_, other.value_);
        return *this;
    }

    constexpr duration& operator-=(duration other) {
        value_ = zx_duration_sub_duration(value_, other.value_);
        return *this;
    }

    constexpr duration& operator*=(int64_t multiplier) {
        value_ = zx_duration_mul_int64(value_, multiplier);
        return *this;
    }

    constexpr duration& operator/=(int64_t divisor) {
        value_ /= divisor;
        return *this;
    }

    constexpr bool operator==(duration other) const { return value_ == other.value_; }
    constexpr bool operator!=(duration other) const { return value_ != other.value_; }
    constexpr bool operator<(duration other) const { return value_ < other.value_; }
    constexpr bool operator<=(duration other) const { return value_ <= other.value_; }
    constexpr bool operator>(duration other) const { return value_ > other.value_; }
    constexpr bool operator>=(duration other) const { return value_ >= other.value_; }

    constexpr int64_t to_nsecs() const { return value_; }

    constexpr int64_t to_usecs() const { return value_ / ZX_USEC(1); }

    constexpr int64_t to_msecs() const { return value_ / ZX_MSEC(1); }

    constexpr int64_t to_secs() const { return value_ / ZX_SEC(1); }

    constexpr int64_t to_mins() const { return value_ / ZX_MIN(1); }

    constexpr int64_t to_hours() const { return value_ / ZX_HOUR(1); }

private:
    zx_duration_t value_ = 0;
};

class time {
public:
    constexpr time() = default;

    explicit constexpr time(zx_time_t value) : value_(value) {}

    static constexpr time infinite() {
        return time(ZX_TIME_INFINITE);
    }

    static constexpr time infinite_past() {
        return time(ZX_TIME_INFINITE_PAST);
    }

    constexpr zx_time_t get() const { return value_; }

    zx_time_t* get_address() { return &value_; }

    constexpr duration operator-(time other) const {
        return duration(zx_time_sub_time(value_, other.value_));
    }

    constexpr time operator+(duration delta) const {
        return time(zx_time_add_duration(value_, delta.get()));
    }

    constexpr time operator-(duration delta) const {
        return time(zx_time_sub_duration(value_, delta.get()));
    }

    constexpr time& operator+=(duration delta) {
      value_ = zx_time_add_duration(value_, delta.get());
      return *this;
    }

    constexpr time& operator-=(duration delta) {
      value_ = zx_time_sub_duration(value_, delta.get());
      return *this;
    }

    constexpr bool operator==(time other) const { return value_ == other.value_; }
    constexpr bool operator!=(time other) const { return value_ != other.value_; }
    constexpr bool operator<(time other) const { return value_ < other.value_; }
    constexpr bool operator<=(time other) const { return value_ <= other.value_; }
    constexpr bool operator>(time other) const { return value_ > other.value_; }
    constexpr bool operator>=(time other) const { return value_ >= other.value_; }

private:
    zx_time_t value_ = 0;
};

constexpr inline duration nsec(int64_t n) { return duration(ZX_NSEC(n)); }

constexpr inline duration usec(int64_t n) { return duration(ZX_USEC(n)); }

constexpr inline duration msec(int64_t n) { return duration(ZX_MSEC(n)); }

constexpr inline duration sec(int64_t n) { return duration(ZX_SEC(n)); }

constexpr inline duration min(int64_t n) { return duration(ZX_MIN(n)); }

constexpr inline duration hour(int64_t n) { return duration(ZX_HOUR(n)); }

} // namespace zx
