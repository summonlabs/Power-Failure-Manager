// Power Failure Manager -- the public runtime.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pfm/authority.hpp"
#include "pfm/classification.hpp"
#include "pfm/clock.hpp"
#include "pfm/evidence.hpp"
#include "pfm/journal.hpp"
#include "pfm/obligations.hpp"
#include "pfm/plan.hpp"
#include "pfm/recovery.hpp"
#include "pfm/request.hpp"
#include "pfm/state.hpp"
#include "pfm/store.hpp"
#include "pfm/transport.hpp"

namespace summon::pfm {

struct RuntimeOptions {
  // Durable mode requires a directory. Volatile mode ignores it.
  std::string store_directory{};
  StoreMode mode{StoreMode::Durable};
  Bounds bounds{};
  // Seed policy for a fresh store. An existing durable store's policy wins
  // until an explicit publish_policy is accepted.
  ElectricalPolicy policy{};
  // Opening identity. A durable store that already holds state is fenced
  // against these values; the runtime itself never invents an epoch or an
  // incarnation.
  ControlEpoch epoch{};
  ControllerIncarnation incarnation{};
  [[nodiscard]] static RuntimeOptions volatile_memory();
};

struct RuntimeStatus {
  StoreMode mode{StoreMode::Volatile};
  std::string store_directory{};
  ControlEpoch epoch{};
  ControllerIncarnation incarnation{};
  StateRevision revision{};
  TopologyGeneration topology_generation{};
  PolicyGeneration policy_generation{};
  EvidenceGeneration evidence_generation{};

  bool incident_live{false};
  IncidentId incident{};
  IncidentGeneration incident_generation{};
  IncidentLifecycle lifecycle{IncidentLifecycle::None};
  FailureClass primary_failure{FailureClass::None};
  RefToken primary_element{};

  bool has_plan{false};
  PlanGeneration plan_generation{};
  std::size_t failures{0};
  std::size_t impacted_elements{0};

  std::size_t observations{0};
  std::size_t requests{0};
  std::size_t open_requests{0};
  std::size_t verified_requests{0};
  std::size_t journal_entries{0};
  std::uint64_t retired_journal_entries{0};

  bool recovery_eligible{false};
  bool operator_authorized{false};

  CommitSequence commit_sequence{};
  std::uint64_t commits{0};
  bool fell_back{false};
};

struct EvaluationOutcome {
  PlanGeneration generation{};
  FailureClass primary_failure{FailureClass::None};
  RefToken primary_element{};
  std::size_t failures{0};
  std::size_t requests_planned{0};
  std::size_t requests_dispatched{0};
  std::size_t accepted{0};
  std::size_t refused{0};
  std::size_t failed{0};
  std::size_t indeterminate{0};
  bool recovery_eligible{false};
  // The requests that were issued to an adjacent controller during this
  // evaluation, in dispatch order.
  std::vector<ResponseRequestId> dispatched_requests{};
};

// The runtime serialises durable mutation with one lock, never calls adapter or
// transport code while holding it, and refuses a callback that re-enters the
// runtime mid-dispatch with ReentrancyRefused.
//
// Durable writes are write-ahead: the intent, its plan generation, its attempt
// identity, and its idempotency key are committed before a request leaves the
// process, so a death around dispatch can never produce a blind duplicate
// consequential request.
class PowerFailureRuntime {
 public:
  // The implementation type is named publicly only so that the translation
  // unit can define it; its definition is not part of this header.
  struct Impl;

  ~PowerFailureRuntime();
  PowerFailureRuntime(const PowerFailureRuntime&) = delete;
  PowerFailureRuntime& operator=(const PowerFailureRuntime&) = delete;
  PowerFailureRuntime(PowerFailureRuntime&&) = delete;
  PowerFailureRuntime& operator=(PowerFailureRuntime&&) = delete;

  [[nodiscard]] static Result<std::unique_ptr<PowerFailureRuntime>> open(
      const RuntimeOptions& options, std::shared_ptr<Clock> clock,
      std::shared_ptr<ResponseTransport> transport);

  // --- read-only ----------------------------------------------------------

  [[nodiscard]] Result<RuntimeStatus> status() const;
  [[nodiscard]] Result<DomainState> state() const;
  [[nodiscard]] Result<ResponsePlan> plan() const;
  [[nodiscard]] Result<ClassificationResult> classification() const;
  [[nodiscard]] Result<AffectedScope> scope() const;
  [[nodiscard]] Result<RecoveryAssessment> recovery() const;
  [[nodiscard]] Result<std::vector<ResponseRequest>> requests() const;
  [[nodiscard]] Result<ResponseRequest> request(ResponseRequestId id) const;
  [[nodiscard]] Result<std::vector<TransitionRecord>> transitions() const;
  [[nodiscard]] Result<std::vector<JournalEntry>> journal() const;
  [[nodiscard]] Result<StoreStatus> store_status() const;

  // --- evidence, topology, policy, obligations ----------------------------

  [[nodiscard]] Result<void> publish_topology(const TopologySnapshot& snapshot,
                                              const AuthorityToken& token);
  [[nodiscard]] Result<void> publish_policy(const ElectricalPolicy& policy,
                                            const AuthorityToken& token);
  [[nodiscard]] Result<void> publish_obligations(
      const std::vector<ProtectedObligation>& obligations, const AuthorityToken& token);
  [[nodiscard]] Result<void> admit_evidence(const ElectricalObservation& observation,
                                            const AuthorityToken& token);
  [[nodiscard]] Result<void> admit_evidence(
      const std::vector<ElectricalObservation>& observations, const AuthorityToken& token);

  // --- decision -----------------------------------------------------------

  // Classifies the current evidence, publishes a plan, and dispatches the
  // justified bounded requests. Dispatch happens outside every runtime lock.
  [[nodiscard]] Result<EvaluationOutcome> evaluate(const AuthorityToken& token);

  // --- request lifecycle --------------------------------------------------

  [[nodiscard]] Result<void> acknowledge_request(ResponseRequestId id, const RefToken& source,
                                                 std::string note,
                                                 const AuthorityToken& token);
  [[nodiscard]] Result<void> record_effect(ResponseRequestId id,
                                           const ElectricalObservation& evidence,
                                           const AuthorityToken& token);
  [[nodiscard]] Result<void> fail_request(ResponseRequestId id, ReasonCode reason, std::string note,
                                          const AuthorityToken& token);
  [[nodiscard]] Result<void> cancel_request(ResponseRequestId id, ReasonCode reason,
                                            const AuthorityToken& token);
  // Resolves a request left Indeterminate by a death around dispatch. The
  // caller must state, with evidence, whether the effect happened.
  [[nodiscard]] Result<void> resolve_indeterminate(ResponseRequestId id, bool effect_observed,
                                                   const ElectricalObservation& evidence,
                                                   const AuthorityToken& token);
  // Re-issues a bounded request as a new attempt with the same idempotency key.
  [[nodiscard]] Result<void> retry_request(ResponseRequestId id, const AuthorityToken& token);

  // --- incident lifecycle and recovery ------------------------------------

  [[nodiscard]] Result<void> open_incident(const AuthorityToken& token);
  [[nodiscard]] Result<void> authorize_recovery(const AuthorityToken& token);
  [[nodiscard]] Result<void> begin_recovery(const AuthorityToken& token);
  [[nodiscard]] Result<void> complete_recovery(const AuthorityToken& token);
  [[nodiscard]] Result<void> close_incident(const AuthorityToken& token);
  [[nodiscard]] Result<void> relax_obligation(ObligationId id, const RefToken& granted_by,
                                              const AuthorityToken& token);

  // --- maintenance --------------------------------------------------------

  // Folds the retained journal into the checkpoint and publishes both.
  [[nodiscard]] Result<void> checkpoint(const AuthorityToken& token);
  // Establishes new authority. A higher epoch supersedes every non-terminal
  // request of the previous authority; an equal epoch with a different
  // incarnation is refused, because a new controller must roll the epoch.
  [[nodiscard]] Result<void> roll_epoch(ControlEpoch epoch, ControllerIncarnation incarnation);
  [[nodiscard]] Result<void> shutdown();

  // The authority token a caller should use right now, with the current
  // revision and the live incident identity. It is a convenience for
  // single-threaded operators, not a grant of authority.
  [[nodiscard]] Result<AuthorityToken> current_authority() const;

 private:
  PowerFailureRuntime() = default;

  std::unique_ptr<Impl> impl_{};
};

}  // namespace summon::pfm
