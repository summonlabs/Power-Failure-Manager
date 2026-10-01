// Power Failure Manager -- injected time source.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include "pfm/status.hpp"
#include "pfm/units.hpp"

namespace summon::pfm {

// The runtime never reads the wall clock directly. All decision instants are
// supplied explicitly or through an injected Clock, so a decision sequence is
// reproducible from its inputs.
class Clock {
 public:
  Clock() = default;
  virtual ~Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  Clock(Clock&&) = delete;
  Clock& operator=(Clock&&) = delete;

  [[nodiscard]] virtual Result<Timestamp> now() const = 0;
};

// Reads the host real-time clock. Real, monotonicity is not guaranteed across
// host clock adjustments; callers that need reproducible decisions supply a
// deterministic clock instead.
class SystemClock final : public Clock {
 public:
  [[nodiscard]] Result<Timestamp> now() const override;
};

// A caller-driven clock. Not thread-safe: it exists so that scenario, example,
// and test sequences are deterministic and free of wall-clock sleeps.
class ManualClock final : public Clock {
 public:
  ManualClock() = default;
  explicit ManualClock(Timestamp start) noexcept : current_(start) {}

  [[nodiscard]] Result<Timestamp> now() const override { return current_; }

  void set(Timestamp instant) noexcept { current_ = instant; }
  // Refuses to move time backwards: a replayed or reordered sequence must not
  // silently make stale evidence look fresh.
  Result<void> advance(Duration delta);

 private:
  Timestamp current_{};
};

}  // namespace summon::pfm
