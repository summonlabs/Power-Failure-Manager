// Power Failure Manager -- exact quantities and checked arithmetic.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/units.hpp"

#include <string>

namespace summon::pfm {
namespace {

Status overflow(std::string_view what) {
  return Status::error(StatusCode::ArithmeticOverflow, std::string{what})
      .with_context("units");
}

}  // namespace

bool basis_points_in_range(BasisPoints value) noexcept {
  return value.value() >= 0 && value.value() <= kBasisPointsFull;
}

bool milli_percent_in_range(MilliPercent value) noexcept {
  return value.value() >= 0 && value.value() <= kMilliPercentFull;
}

Result<std::int64_t> checked_add_i64(std::int64_t a, std::int64_t b) noexcept {
  if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
    return overflow("addition overflow");
  }
  if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
    return overflow("addition underflow");
  }
  return a + b;
}

Result<std::int64_t> checked_sub_i64(std::int64_t a, std::int64_t b) noexcept {
  if (b == std::numeric_limits<std::int64_t>::min()) {
    return overflow("subtraction overflow");
  }
  return checked_add_i64(a, -b);
}

Result<std::int64_t> checked_mul_i64(std::int64_t a, std::int64_t b) noexcept {
  if (a == 0 || b == 0) {
    return std::int64_t{0};
  }
  if (a == -1 && b == std::numeric_limits<std::int64_t>::min()) {
    return overflow("multiplication overflow");
  }
  if (b == -1 && a == std::numeric_limits<std::int64_t>::min()) {
    return overflow("multiplication overflow");
  }
  const std::int64_t product = a * b;
  if (product / b != a) {
    return overflow("multiplication overflow");
  }
  return product;
}

Result<std::uint64_t> checked_add_u64(std::uint64_t a, std::uint64_t b) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return Status::error(StatusCode::ArithmeticOverflow, "unsigned addition overflow")
        .with_context("units");
  }
  return a + b;
}

Result<std::uint64_t> checked_mul_u64(std::uint64_t a, std::uint64_t b) noexcept {
  if (a == 0 || b == 0) {
    return std::uint64_t{0};
  }
  const std::uint64_t product = a * b;
  if (product / b != a) {
    return Status::error(StatusCode::ArithmeticOverflow, "unsigned multiplication overflow")
        .with_context("units");
  }
  return product;
}

Result<Timestamp> checked_add(Timestamp instant, Duration delta) noexcept {
  auto value = checked_add_i64(instant.value(), delta.value());
  if (!value.ok()) {
    return value.status();
  }
  return Timestamp::from_value(value.value());
}

Result<Timestamp> checked_sub(Timestamp instant, Duration delta) noexcept {
  auto value = checked_sub_i64(instant.value(), delta.value());
  if (!value.ok()) {
    return value.status();
  }
  return Timestamp::from_value(value.value());
}

Result<Duration> checked_difference(Timestamp later, Timestamp earlier) noexcept {
  auto value = checked_sub_i64(later.value(), earlier.value());
  if (!value.ok()) {
    return value.status();
  }
  if (value.value() < 0) {
    return Status::error(StatusCode::InvalidArgument, "instants are out of order")
        .with_context("units.difference");
  }
  return Duration::from_value(value.value());
}

bool within_window(Timestamp instant, Timestamp now, Duration window) noexcept {
  if (window.value() < 0) {
    return false;
  }
  auto difference = checked_sub_i64(now.value(), instant.value());
  if (!difference.ok()) {
    return false;
  }
  if (difference.value() < 0) {
    auto ahead = checked_sub_i64(instant.value(), now.value());
    if (!ahead.ok()) {
      return false;
    }
    return ahead.value() <= window.value();
  }
  return difference.value() <= window.value();
}

}  // namespace summon::pfm
