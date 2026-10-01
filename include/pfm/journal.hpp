// Power Failure Manager -- the authoritative input journal.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <string>
#include <vector>

#include "pfm/evidence.hpp"
#include "pfm/ids.hpp"
#include "pfm/obligations.hpp"
#include "pfm/plan.hpp"
#include "pfm/policy.hpp"
#include "pfm/request.hpp"
#include "pfm/topology.hpp"

namespace summon::pfm {

// Every durable mutation is recorded as one journal entry. The live state is
// exactly the fold of the journal onto its checkpoint, and a load verifies
// that equality byte for byte.
enum class JournalKind : std::uint8_t {
  None = 0,
  IncidentOpened = 1,
  IncidentClosed = 2,
  EvidenceAdmitted = 3,
  EvidenceInvalidated = 4,
  TopologyPublished = 5,
  PolicyPublished = 6,
  ObligationsPublished = 7,
  PlanComputed = 8,
  RequestIssued = 10,
  RequestAcknowledged = 11,
  RequestObserved = 12,
  RequestVerified = 13,
  RequestFailed = 14,
  RequestRefused = 15,
  RequestSuperseded = 16,
  RequestAbandoned = 17,
  RequestIndeterminate = 18,
  RequestResolved = 19,
  RecoveryAuthorized = 20,
  RecoveryStarted = 21,
  RecoveryCompleted = 22,
  ObligationRelaxed = 23,
  EpochRolled = 24,
  StabilityObserved = 25,
  StabilityCleared = 26,
};

[[nodiscard]] std::string_view journal_kind_name(JournalKind value) noexcept;

// The payload carries the complete input needed to fold the entry. Entries are
// self-contained: replay never consults a clock, a transport, or an adapter.
struct JournalPayload {
  bool has_observation{false};
  ElectricalObservation observation{};

  bool has_topology{false};
  TopologySnapshot topology{};

  bool has_policy{false};
  ElectricalPolicy policy{};

  bool has_obligations{false};
  std::vector<ProtectedObligation> obligations{};

  bool has_plan{false};
  ResponsePlan plan{};

  ResponseRequestId request{};
  RequestState from_state{RequestState::Planned};
  RequestState to_state{RequestState::Planned};
  bool has_request_state{false};
  AttemptId attempt{};
  RefToken source{};
  Fingerprint evidence{};
  ReasonCode reason{ReasonCode::None};
  std::string note{};

  IncidentId incident{};
  IncidentGeneration incident_generation{};
  bool has_incident{false};

  ControlEpoch epoch{};
  ControllerIncarnation incarnation{};
  // True when the epoch was rolled to a new controller rather than resumed by
  // the same one. The two cases dispose of in-flight work differently.
  bool epoch_rollover{false};

  bool has_recovery_authorization{false};
  Timestamp authorized_at{};

  ObligationId obligation{};
  bool has_obligation{false};
  ProtectionClass relaxation_class{ProtectionClass::ServiceLevel};
  RefToken granted_by{};
};

// A complete, self-describing journal entry. The fingerprint covers the entry
// type, the revision it produced, and the payload, so a corrupted or reordered
// entry is detected rather than replayed.
struct JournalEntry {
  JournalSequence sequence{};
  JournalKind kind{JournalKind::None};
  Timestamp recorded_at{};
  StateRevision revision{};
  JournalPayload payload{};
  Fingerprint fingerprint{};

  [[nodiscard]] friend bool operator<(const JournalEntry& a, const JournalEntry& b) noexcept {
    return a.sequence < b.sequence;
  }
};

struct TransitionRecord {
  TransitionSequence sequence{};
  Timestamp recorded_at{};
  JournalKind kind{JournalKind::None};
  StateRevision from_revision{};
  StateRevision to_revision{};
  ReasonCode reason{ReasonCode::None};
  RefToken subject{};
};

[[nodiscard]] Result<void> validate(const JournalEntry& entry, const Bounds& bounds);

// Fingerprint of the entry's identity-bearing content. Recomputing it on load
// detects both corruption and a reordered or substituted entry.
[[nodiscard]] Fingerprint journal_entry_fingerprint(const JournalEntry& entry) noexcept;

}  // namespace summon::pfm
