// Power Failure Manager -- strongly typed identities, generations, epochs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <compare>
#include <cstdint>
#include <string>

namespace summon::pfm {

// A distinct 64-bit identity or counter. The tag type gives every semantic
// concept its own type: an epoch can never be passed where a revision is
// expected, and an incident id can never be compared to an attempt id.
template <class Tag, class Rep = std::uint64_t>
class Id {
 public:
  using rep_type = Rep;

  constexpr Id() noexcept = default;
  constexpr explicit Id(Rep value) noexcept : value_(value) {}

  [[nodiscard]] static constexpr Id from_value(Rep value) noexcept { return Id{value}; }
  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }

  // Zero means "absent" / "not yet assigned" for identity types.
  [[nodiscard]] constexpr bool is_set() const noexcept { return value_ != 0; }
  [[nodiscard]] constexpr bool is_absent() const noexcept { return value_ == 0; }

  [[nodiscard]] friend constexpr bool operator==(Id, Id) noexcept = default;
  [[nodiscard]] friend constexpr auto operator<=>(Id, Id) noexcept = default;

 private:
  Rep value_{0};
};

struct IncidentIdTag {};
struct IncidentGenerationTag {};
struct PolicyGenerationTag {};
struct ConfigGenerationTag {};
struct EvidenceGenerationTag {};
struct TopologyGenerationTag {};
struct StateRevisionTag {};
struct PlanGenerationTag {};
struct JournalSequenceTag {};
struct TransitionSequenceTag {};
struct ObservationSequenceTag {};
struct AttemptIdTag {};
struct ResponseRequestIdTag {};
struct ObligationIdTag {};
struct CommitSequenceTag {};
struct StoreGenerationTag {};
struct ControllerIncarnationTag {};
struct ControlEpochTag {};
struct DispatchSequenceTag {};

using IncidentId = Id<IncidentIdTag>;
using IncidentGeneration = Id<IncidentGenerationTag>;
using PolicyGeneration = Id<PolicyGenerationTag>;
using ConfigGeneration = Id<ConfigGenerationTag>;
using EvidenceGeneration = Id<EvidenceGenerationTag>;
using TopologyGeneration = Id<TopologyGenerationTag>;
using StateRevision = Id<StateRevisionTag>;
using PlanGeneration = Id<PlanGenerationTag>;
using JournalSequence = Id<JournalSequenceTag>;
using TransitionSequence = Id<TransitionSequenceTag>;
using ObservationSequence = Id<ObservationSequenceTag>;
using AttemptId = Id<AttemptIdTag>;
using ResponseRequestId = Id<ResponseRequestIdTag>;
using ObligationId = Id<ObligationIdTag>;
using CommitSequence = Id<CommitSequenceTag>;
using StoreGeneration = Id<StoreGenerationTag>;
using ControllerIncarnation = Id<ControllerIncarnationTag>;
using ControlEpoch = Id<ControlEpochTag>;
using DispatchSequence = Id<DispatchSequenceTag>;

// 128-bit content fingerprint. Used both as the semantic fingerprint of a
// bounded response request and as the idempotency key derived from it. Two
// distinct 64-bit halves are kept so a key is never truncated into a
// collision-prone single word.
class Fingerprint {
 public:
  constexpr Fingerprint() noexcept = default;
  constexpr Fingerprint(std::uint64_t high, std::uint64_t low) noexcept : high_(high), low_(low) {}

  [[nodiscard]] constexpr std::uint64_t high() const noexcept { return high_; }
  [[nodiscard]] constexpr std::uint64_t low() const noexcept { return low_; }
  [[nodiscard]] constexpr bool is_set() const noexcept { return high_ != 0 || low_ != 0; }

  [[nodiscard]] friend constexpr bool operator==(Fingerprint, Fingerprint) noexcept = default;
  [[nodiscard]] friend constexpr auto operator<=>(Fingerprint, Fingerprint) noexcept = default;

  // Canonical lowercase hex, 32 characters.
  [[nodiscard]] std::string to_hex() const;

 private:
  std::uint64_t high_{0};
  std::uint64_t low_{0};
};

// A caller-supplied idempotency key. Deliberately a distinct type from
// Fingerprint so a fingerprint can never be mistaken for a key chosen by an
// external coordinator.
class IdempotencyKey {
 public:
  constexpr IdempotencyKey() noexcept = default;
  constexpr IdempotencyKey(std::uint64_t high, std::uint64_t low) noexcept
      : high_(high), low_(low) {}

  [[nodiscard]] static constexpr IdempotencyKey from_fingerprint(Fingerprint fingerprint) noexcept {
    return IdempotencyKey{fingerprint.high(), fingerprint.low()};
  }

  [[nodiscard]] constexpr std::uint64_t high() const noexcept { return high_; }
  [[nodiscard]] constexpr std::uint64_t low() const noexcept { return low_; }
  [[nodiscard]] constexpr bool is_set() const noexcept { return high_ != 0 || low_ != 0; }
  [[nodiscard]] constexpr Fingerprint as_fingerprint() const noexcept {
    return Fingerprint{high_, low_};
  }

  [[nodiscard]] friend constexpr bool operator==(IdempotencyKey, IdempotencyKey) noexcept = default;
  [[nodiscard]] friend constexpr auto operator<=>(IdempotencyKey, IdempotencyKey) noexcept = default;

  [[nodiscard]] std::string to_hex() const;

 private:
  std::uint64_t high_{0};
  std::uint64_t low_{0};
};

// Deterministic ordering helpers used to keep canonical output independent of
// container iteration order.
template <class T>
[[nodiscard]] constexpr bool less_by_value(const T& a, const T& b) noexcept {
  return a < b;
}

}  // namespace summon::pfm
