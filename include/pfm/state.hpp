// Power Failure Manager -- durable domain state and its single mutator.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <vector>

#include "pfm/authority.hpp"
#include "pfm/evidence.hpp"
#include "pfm/ids.hpp"
#include "pfm/journal.hpp"
#include "pfm/obligations.hpp"
#include "pfm/plan.hpp"
#include "pfm/policy.hpp"
#include "pfm/recovery.hpp"
#include "pfm/request.hpp"
#include "pfm/topology.hpp"

namespace summon::pfm {

// Incident lifecycle. Recovery is never implicit: it is entered only through an
// explicit, authorized recovery request, and a regression to Active is the
// only legal way back.
enum class IncidentLifecycle : std::uint8_t {
  None = 0,
  Active = 1,
  Isolating = 2,
  Stabilizing = 3,
  Recovering = 4,
  Recovered = 5,
  Closed = 6,
};

[[nodiscard]] std::string_view incident_lifecycle_name(IncidentLifecycle value) noexcept;
[[nodiscard]] bool incident_lifecycle_transition_legal(IncidentLifecycle from,
                                                       IncidentLifecycle to) noexcept;

// The retained current observation for one element. A slot restored from
// durable state is marked recovered and counts for nothing until a live
// observation from an external source replaces it.
struct ObservationSlot {
  RefToken element{};
  ElementKind kind{ElementKind::Unknown};
  ElectricalObservation observation{};
  Freshness freshness{Freshness::Unknown};
  bool present{false};
  bool recovered{false};
  bool contradicted{false};
  std::vector<Fingerprint> conflict_evidence{};
};

struct RequestSlot {
  ResponseRequest request{};
};

struct ObligationSlot {
  ProtectedObligation obligation{};
  bool report_current{false};
};

// A record of an explicit obligation relaxation. Relaxation is never implicit
// and never applies to a hard safety interlock or a regulatory obligation.
struct RelaxationRecord {
  ObligationId obligation{};
  ProtectionClass protection{ProtectionClass::ServiceLevel};
  ControlEpoch epoch{};
  IncidentId incident{};
  Timestamp granted_at{};
  RefToken granted_by{};
};

struct IncidentProjection {
  IncidentId incident{};
  IncidentGeneration generation{};
  IncidentLifecycle lifecycle{IncidentLifecycle::None};
  Timestamp opened_at{};
  Timestamp updated_at{};
  FailureClass primary_failure{FailureClass::None};
  RefToken primary_element{};
  bool has_plan{false};
  PlanGeneration plan_generation{};
  Fingerprint plan_fingerprint{};
  bool operator_authorized{false};
  Timestamp authorized_at{};
  IncidentGeneration authorized_generation{};
  bool has_stable_since{false};
  Timestamp stable_since{};
  std::uint64_t recovery_steps{0};
  TransitionSequence transitions{};
  // Every element the incident has ever impacted, sorted and unique. Recovery
  // is assessed against this cumulative scope, not only against the failures
  // visible in the newest plan, so isolating an element cannot silently drop it
  // from the recovery question.
  std::vector<RefToken> impacted_scope{};
};

// The complete durable state of one PFM controller.
struct DomainState {
  ControlEpoch epoch{};
  ControllerIncarnation incarnation{};
  StateRevision revision{};
  TopologyGeneration topology_generation{};
  EvidenceGeneration evidence_generation{};
  DispatchSequence dispatch_sequence{};
  PolicyGeneration policy_generation{};
  ElectricalPolicy policy{};
  Bounds bounds{};

  IncidentProjection incident{};
  bool has_plan{false};
  ResponsePlan plan{};
  bool has_topology{false};
  TopologySnapshot topology{};

  std::vector<ObservationSlot> observations{};
  std::vector<RequestSlot> requests{};
  std::vector<ObligationSlot> obligations{};
  std::vector<RelaxationRecord> relaxations{};
  std::vector<TransitionRecord> transitions{};

  ResponseRequestId next_request_id{};
  AttemptId next_attempt_id{};
  PlanGeneration next_plan_generation{};

  [[nodiscard]] const ObservationSlot* find_observation(const RefToken& element) const noexcept;
  [[nodiscard]] const RequestSlot* find_request(ResponseRequestId id) const noexcept;
  [[nodiscard]] RequestSlot* find_request(ResponseRequestId id) noexcept;
  [[nodiscard]] const ObligationSlot* find_obligation(ObligationId id) const noexcept;
};

// The durable projection an incoming authority token is fenced against.
[[nodiscard]] AuthorityExpectation expectation_of(const DomainState& state);

// Structural validation of a whole domain state: identity and generation
// presence, canonical ordering and uniqueness of every collection, declared
// counts against the configured bounds, and agreement between the incident
// projection and the published plan. A state that fails validation is refused
// whole rather than partially trusted.
[[nodiscard]] Result<void> validate(const DomainState& state, const Bounds& bounds);

// The observations the planner may treat as current at this instant: one per
// element, never a recovered slot, never a stale or contradicted one, taken
// from the retained slot with the highest observation sequence.
[[nodiscard]] std::vector<ElectricalObservation> current_observations(
    const DomainState& state, Timestamp now, const ElectricalPolicy& policy);

// Canonical byte encoding of the whole domain state. Used for the replay
// equality check and for byte-for-byte determinism proofs.
[[nodiscard]] std::vector<std::uint8_t> encode_state(const DomainState& state);

// Folds one journal entry onto the state. This is the only mutator: no other
// function may change durable decision state. Returns the resulting revision
// and refuses an entry that cannot legally follow.
[[nodiscard]] Result<void> apply_journal_entry(DomainState& state, const JournalEntry& entry);

// Refreshes currency markings for every retained observation and obligation at
// an instant. This is derived bookkeeping, not a decision.
void refresh_currency(DomainState& state, Timestamp now);

}  // namespace summon::pfm
