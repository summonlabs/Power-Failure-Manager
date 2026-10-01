// Power Failure Manager -- clock implementations.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/clock.hpp"

#include <chrono>

namespace summon::pfm {

Result<Timestamp> SystemClock::now() const {
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count();
  return Timestamp::from_value(static_cast<std::int64_t>(millis));
}

Result<void> ManualClock::advance(Duration delta) {
  if (delta.value() < 0) {
    return Status::error(StatusCode::InvalidArgument,
                         "a manual clock never moves backwards")
        .with_context("clock.advance");
  }
  auto next = checked_add(current_, delta);
  if (!next.ok()) {
    return next.status();
  }
  current_ = next.value();
  return {};
}

}  // namespace summon::pfm
