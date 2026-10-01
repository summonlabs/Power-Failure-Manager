// Power Failure Manager -- typed electrical-failure classification.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pfm/evidence.hpp"
#include "pfm/policy.hpp"
#include "pfm/topology.hpp"

namespace summon::pfm {

// The typed electrical failures PFM classifies. A class is a decision about
// what failed, not a restatement of an alarm: it is derived from typed,
// current evidence and from the supplied topology.
enum class FailureClass : std::uint8_t {
  None = 0,
  UtilityFeedLoss = 1,
  UtilityFeedDegradation = 2,
  SwitchgearFailure = 3,
  BusFailure = 4,
  BreakerFailure = 5,
  CircuitFailure = 6,
  PduFailure = 7,
  PduBranchFailure = 8,
  UpsFailure = 9,
  UpsReserveInsufficient = 10,
  GeneratorFailure = 11,
  GeneratorStartFailure = 12,
  GeneratorSyncFailure = 13,
  GeneratorTransferFailure = 14,
  SharedUpstreamDomainFailure = 15,
  AmbiguousElectricalEvidence = 16,
};

[[nodiscard]] std::string_view failure_class_name(FailureClass value) noexcept;
// Stable machine-readable code, e.g. "pfm.failure.utility_feed_loss".
[[nodiscard]] std::string_view failure_class_code(FailureClass value) noexcept;
[[nodiscard]] bool failure_class_is_ambiguous(FailureClass value) noexcept;
// Severity ordering used to select the primary failure of a plan. Ambiguity
// outranks every concrete failure: unresolved evidence is never downgraded.
[[nodiscard]] std::uint8_t failure_class_severity(FailureClass value) noexcept;

// Machine-readable reason codes. These are the atoms of the reason trace: the
// explanation of a classification is a deterministic list of them.
enum class ReasonCode : std::uint16_t {
  None = 0,
  FeedDeEnergized = 1,
  FeedVoltageOutOfBand = 2,
  FeedFrequencyOutOfBand = 3,
  ElementDeEnergizedWhileUpstreamEnergized = 4,
  ElementDeEnergizedWithUpstreamUnknown = 5,
  BreakerTripped = 6,
  BreakerOpenUnderLoad = 7,
  UpsFaulted = 8,
  UpsOnBattery = 9,
  UpsReserveBelowFloor = 10,
  UpsReserveBelowCriticalFloor = 11,
  GeneratorFaulted = 12,
  GeneratorStartWindowExpired = 13,
  GeneratorNotSynchronized = 14,
  GeneratorTransferFailed = 15,
  EvidenceStale = 16,
  EvidenceExpired = 17,
  EvidenceMissing = 18,
  EvidenceContradictory = 19,
  EvidenceQualityBad = 20,
  UpstreamEvidenceUnresolved = 21,
  SharedDomainUnresolved = 22,
  DomainMemberFailed = 23,
  ScopeIncludesSharedDomain = 24,
  DownstreamHealthyDoesNotImplyRecovery = 25,
  JustificationWithdrawn = 26,
  AttemptFailed = 27,
  AttemptExpired = 28,
  IndeterminateDispatchResolved = 29,
  DispatchNotAccepted = 30,
  AuthorizationNotCurrent = 31,
  DispatchOutcomeUnknown = 32,
};

[[nodiscard]] std::string_view reason_code_name(ReasonCode value) noexcept;

// One step of a reason trace: a code, the element it applies to, and the
// fingerprints of the evidence it rests on. The trace is the audit surface of
// a decision and is emitted in a deterministic order.
struct ReasonStep {
  ReasonCode code{ReasonCode::None};
  RefToken subject{};
  Freshness freshness{Freshness::Unknown};
  std::vector<Fingerprint> evidence{};

  [[nodiscard]] friend bool operator<(const ReasonStep& a, const ReasonStep& b) noexcept {
    if (a.code != b.code) {
      return static_cast<std::uint16_t>(a.code) < static_cast<std::uint16_t>(b.code);
    }
    return a.subject < b.subject;
  }
  [[nodiscard]] friend bool operator==(const ReasonStep& a, const ReasonStep& b) noexcept {
    return a.code == b.code && a.subject == b.subject && a.freshness == b.freshness &&
           a.evidence == b.evidence;
  }
};

struct ClassifiedFailure {
  FailureClass klass{FailureClass::None};
  RefToken element{};
  ElementKind kind{ElementKind::Unknown};
  ReasonCode primary_reason{ReasonCode::None};
  std::vector<ReasonStep> reasons{};
  // False when the failure was derived without usable current evidence for the
  // element, in which case the plan must protect rather than assume.
  bool evidence_current{true};

  [[nodiscard]] friend bool operator<(const ClassifiedFailure& a,
                                      const ClassifiedFailure& b) noexcept {
    const auto sa = failure_class_severity(a.klass);
    const auto sb = failure_class_severity(b.klass);
    if (sa != sb) {
      return sa > sb;  // most severe first
    }
    if (a.klass != b.klass) {
      return static_cast<std::uint8_t>(a.klass) < static_cast<std::uint8_t>(b.klass);
    }
    return a.element < b.element;
  }
  [[nodiscard]] friend bool operator==(const ClassifiedFailure& a,
                                       const ClassifiedFailure& b) noexcept {
    return a.klass == b.klass && a.element == b.element && a.kind == b.kind &&
           a.primary_reason == b.primary_reason && a.reasons == b.reasons &&
           a.evidence_current == b.evidence_current;
  }
};

struct ClassificationResult {
  // Ordered by severity, then class, then element reference.
  std::vector<ClassifiedFailure> failures{};
  // Elements that are monitored but whose current state could not be
  // established: stale, expired, missing, contradictory, or bad quality.
  std::vector<RefToken> unresolved_elements{};
  // Elements whose evidence was present and current at this instant.
  std::vector<RefToken> resolved_elements{};

  [[nodiscard]] bool has_failure() const noexcept { return !failures.empty(); }
  [[nodiscard]] bool has_ambiguous() const noexcept;
  [[nodiscard]] FailureClass primary_class() const noexcept;
};

// Classifies the supplied observations at one instant. Evidence that is not
// current at "now" is never read as health: it becomes an ambiguous failure
// when the policy fails closed, and the element is listed as unresolved.
// Contradictions between reports of the same element are reported as
// AmbiguousElectricalEvidence rather than resolved by picking a winner.
[[nodiscard]] Result<ClassificationResult> classify(
    const std::vector<ElectricalObservation>& current, Timestamp now,
    const TopologyIndex& topology, const ElectricalPolicy& policy, const Bounds& bounds);

}  // namespace summon::pfm
