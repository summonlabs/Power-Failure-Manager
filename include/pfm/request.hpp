// Power Failure Manager -- bounded response requests and their lifecycle.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pfm/classification.hpp"
#include "pfm/evidence.hpp"
#include "pfm/ids.hpp"
#include "pfm/policy.hpp"
#include "pfm/refs.hpp"

namespace summon::pfm {

// The bounded request vocabulary. Each kind is a request to the authority that
// owns the effect; PFM never performs the effect and never asserts it happened.
enum class RequestKind : std::uint8_t {
  Unknown = 0,
  IsolateElement = 1,
  VerifyDeEnergization = 2,
  BlockTransfer = 3,
  SelectFeed = 4,
  StartGenerator = 5,
  SynchronizeGenerator = 6,
  TransferToGenerator = 7,
  PreserveReserve = 8,
  ShedLoad = 9,
  CapPower = 10,
  DeferReclose = 11,
  Reclose = 12,
  RestoreNormalFeed = 13,
};

[[nodiscard]] std::string_view request_kind_name(RequestKind value) noexcept;
[[nodiscard]] std::string_view request_kind_code(RequestKind value) noexcept;

// The adjacent authority that owns the requested effect. These are DCCP
// boundaries, not subsystems of PFM.
enum class RequestOwner : std::uint8_t {
  Unknown = 0,
  FeedAuthority = 1,
  PowerControlPlane = 2,
  PduControl = 3,
  UpsControl = 4,
  GeneratorControl = 5,
  LoadShedding = 6,
  PowerCapacity = 7,
  FacilityFailureDomainRegistry = 8,
  IncidentStateFabric = 9,
};

[[nodiscard]] std::string_view request_owner_name(RequestOwner value) noexcept;
// The default owner for a request kind, used by the planner.
[[nodiscard]] RequestOwner owner_for_kind(RequestKind kind) noexcept;

// Proof required before the requested effect counts as achieved. An
// acknowledgement is never proof; only current external evidence is.
enum class ProofKind : std::uint8_t {
  None = 0,
  Isolation = 1,
  DeEnergization = 2,
  BreakerOpen = 3,
  Transfer = 4,
  Synchronization = 5,
  Generation = 6,
  Reserve = 7,
  LoadShed = 8,
  StableSource = 9,
  FeedSelected = 10,
};

[[nodiscard]] std::string_view proof_kind_name(ProofKind value) noexcept;
// The proof a request kind must be verified by.
[[nodiscard]] ProofKind required_proof_for_kind(RequestKind kind) noexcept;

// Request lifecycle:
//
//   Planned -> Issued -> Acknowledged -> Observed -> Verified
//
// with the terminal states Failed, Superseded, Abandoned, Refused, Expired and
// the non-terminal Indeterminate. Indeterminate means the dispatch outcome is
// unknown, which is what a crash around dispatch leaves behind; it is resolved
// explicitly and is never silently retried.
enum class RequestState : std::uint8_t {
  Planned = 0,
  Issued = 1,
  Acknowledged = 2,
  Observed = 3,
  Verified = 4,
  Failed = 5,
  Superseded = 6,
  Abandoned = 7,
  Refused = 8,
  Expired = 9,
  Indeterminate = 10,
};

[[nodiscard]] std::string_view request_state_name(RequestState value) noexcept;
[[nodiscard]] bool request_state_is_terminal(RequestState value) noexcept;
[[nodiscard]] bool request_state_is_open(RequestState value) noexcept;
[[nodiscard]] bool request_state_transition_legal(RequestState from, RequestState to) noexcept;

// The element and authority a request is addressed to.
struct RequestTarget {
  RefToken element{};
  ElementKind kind{ElementKind::Unknown};
  RequestOwner owner{RequestOwner::Unknown};
  RefToken controller{};

  [[nodiscard]] friend bool operator==(const RequestTarget& a, const RequestTarget& b) noexcept {
    return a.element == b.element && a.kind == b.kind && a.owner == b.owner &&
           a.controller == b.controller;
  }
};

// One attempt at one bounded request. Attempts are retained individually: a
// re-issue is a new attempt identity with the same idempotency key, so a lost
// response can never produce a second consequential mutation.
struct ResponseRequest {
  ResponseRequestId id{};
  RequestKind kind{RequestKind::Unknown};
  RequestTarget target{};
  PlanGeneration plan_generation{};
  IncidentGeneration incident_generation{};
  AttemptId attempt{};
  std::uint32_t attempt_ordinal{0};
  IdempotencyKey idempotency{};
  Fingerprint fingerprint{};
  RequestState state{RequestState::Planned};
  ProofKind required_proof{ProofKind::None};

  bool has_magnitude{false};
  BasisPoints magnitude{};

  Timestamp created_at{};
  bool has_issued_at{false};
  Timestamp issued_at{};
  bool has_acknowledged_at{false};
  Timestamp acknowledged_at{};
  bool has_observed_at{false};
  Timestamp observed_at{};
  bool has_verified_at{false};
  Timestamp verified_at{};
  Timestamp deadline{};

  ReasonCode justification{ReasonCode::None};
  RefToken justification_subject{};
  std::vector<Fingerprint> evidence{};
  std::string owner_note{};

  // Verification provenance: which external source confirmed the effect and
  // which evidence document it was.
  RefToken verification_source{};
  Fingerprint verification_evidence{};
  Timestamp verification_observed_at{};

  // Set when the attempt was superseded or replaced by a later attempt.
  AttemptId superseded_by{};
  ReasonCode terminal_reason{ReasonCode::None};

  [[nodiscard]] bool is_open() const noexcept { return request_state_is_open(state); }
  [[nodiscard]] bool satisfies(ProofKind required) const noexcept;
};

// Computes the semantic fingerprint of a request from the fields that make it
// a distinct consequential operation. Two requests with equal fingerprints are
// the same operation and share an idempotency key.
[[nodiscard]] Fingerprint response_request_fingerprint(const ResponseRequest& request) noexcept;

[[nodiscard]] Result<void> validate(const ResponseRequest& request, const Bounds& bounds);

}  // namespace summon::pfm
