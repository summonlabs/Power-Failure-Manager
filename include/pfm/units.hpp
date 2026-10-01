// Power Failure Manager -- exact integer quantities and checked arithmetic.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <string>

#include "pfm/status.hpp"

namespace summon::pfm {

// Every externally influenced quantity is an exact integer with an explicit
// unit. Floating point is never used as an authority or accounting boundary.
template <class Tag, class Rep = std::int64_t>
class Quantity {
 public:
  using rep_type = Rep;

  constexpr Quantity() noexcept = default;
  constexpr explicit Quantity(Rep value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Quantity from_value(Rep value) noexcept {
    return Quantity{value};
  }
  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }

  [[nodiscard]] friend constexpr bool operator==(Quantity, Quantity) noexcept = default;
  [[nodiscard]] friend constexpr auto operator<=>(Quantity, Quantity) noexcept = default;

 private:
  Rep value_{0};
};

struct TimestampTag {};
struct DurationTag {};
struct BasisPointsTag {};
struct MilliVoltsTag {};
struct MilliHertzTag {};
struct MilliAmpsTag {};
struct MilliWattsTag {};
struct MilliPercentTag {};
struct PermilleTag {};

using Timestamp = Quantity<TimestampTag>;
using Duration = Quantity<DurationTag>;
using BasisPoints = Quantity<BasisPointsTag>;
using MilliVolts = Quantity<MilliVoltsTag>;
using MilliHertz = Quantity<MilliHertzTag>;
using MilliAmps = Quantity<MilliAmpsTag>;
using MilliWatts = Quantity<MilliWattsTag>;
// Percentage of a nominal value, in thousandths of a percent: 100000 == 100%.
using MilliPercent = Quantity<MilliPercentTag>;
// Ratio in thousandths: 1000 permille == 1.0.
using Permille = Quantity<PermilleTag>;

[[nodiscard]] constexpr BasisPoints basis_points_from_value(std::int64_t value) noexcept {
  return BasisPoints::from_value(value);
}
[[nodiscard]] constexpr MilliPercent milli_percent_from_value(std::int64_t value) noexcept {
  return MilliPercent::from_value(value);
}

inline constexpr std::int64_t kBasisPointsFull = 10000;
inline constexpr std::int64_t kMilliPercentFull = 100000;

// Full scale (100%) in basis points.
[[nodiscard]] constexpr BasisPoints basis_points_full() noexcept {
  return BasisPoints::from_value(kBasisPointsFull);
}

[[nodiscard]] bool basis_points_in_range(BasisPoints value) noexcept;
[[nodiscard]] bool milli_percent_in_range(MilliPercent value) noexcept;

// --- checked arithmetic ----------------------------------------------------
//
// Every arithmetic operation on an externally influenced value goes through
// one of these helpers. Overflow and underflow are refused rather than
// wrapped, and the refusal is reported as ArithmeticOverflow.

[[nodiscard]] Result<std::int64_t> checked_add_i64(std::int64_t a, std::int64_t b) noexcept;
[[nodiscard]] Result<std::int64_t> checked_sub_i64(std::int64_t a, std::int64_t b) noexcept;
[[nodiscard]] Result<std::int64_t> checked_mul_i64(std::int64_t a, std::int64_t b) noexcept;
[[nodiscard]] Result<std::uint64_t> checked_add_u64(std::uint64_t a, std::uint64_t b) noexcept;
[[nodiscard]] Result<std::uint64_t> checked_mul_u64(std::uint64_t a, std::uint64_t b) noexcept;

template <class Tag>
[[nodiscard]] Result<Quantity<Tag>> checked_add(Quantity<Tag> a, Quantity<Tag> b) noexcept {
  auto sum = checked_add_i64(a.value(), b.value());
  if (!sum.ok()) {
    return sum.status();
  }
  return Quantity<Tag>::from_value(sum.value());
}

template <class Tag>
[[nodiscard]] Result<Quantity<Tag>> checked_sub(Quantity<Tag> a, Quantity<Tag> b) noexcept {
  auto difference = checked_sub_i64(a.value(), b.value());
  if (!difference.ok()) {
    return difference.status();
  }
  return Quantity<Tag>::from_value(difference.value());
}

template <class Tag>
[[nodiscard]] Result<Quantity<Tag>> checked_scale(Quantity<Tag> value, std::int64_t factor) noexcept {
  auto product = checked_mul_i64(value.value(), factor);
  if (!product.ok()) {
    return product.status();
  }
  return Quantity<Tag>::from_value(product.value());
}

// --- time ------------------------------------------------------------------

// Milliseconds since the Unix epoch, UTC. The runtime never reads the wall
// clock directly: every observation instant is supplied by the caller or by
// the injected Clock, so decisions are reproducible.
[[nodiscard]] Result<Timestamp> checked_add(Timestamp instant, Duration delta) noexcept;
[[nodiscard]] Result<Timestamp> checked_sub(Timestamp instant, Duration delta) noexcept;
// Returns an error when the second instant precedes the first.
[[nodiscard]] Result<Duration> checked_difference(Timestamp later, Timestamp earlier) noexcept;

[[nodiscard]] constexpr Duration duration_from_millis(std::int64_t millis) noexcept {
  return Duration::from_value(millis);
}
[[nodiscard]] constexpr Duration duration_from_seconds(std::int64_t seconds) noexcept {
  return Duration::from_value(seconds * 1000);
}
[[nodiscard]] constexpr std::int64_t duration_millis(Duration value) noexcept {
  return value.value();
}
// Rounds up, so a non-zero sub-second duration never becomes zero.
[[nodiscard]] constexpr std::int64_t duration_seconds_ceil(Duration value) noexcept {
  return (value.value() + 999) / 1000;
}
[[nodiscard]] constexpr Timestamp timestamp_from_unix_millis(std::int64_t millis) noexcept {
  return Timestamp::from_value(millis);
}
[[nodiscard]] constexpr std::int64_t timestamp_unix_millis(Timestamp value) noexcept {
  return value.value();
}

// True when "instant" is within "window" of "now" in either direction.
[[nodiscard]] bool within_window(Timestamp instant, Timestamp now, Duration window) noexcept;

}  // namespace summon::pfm
