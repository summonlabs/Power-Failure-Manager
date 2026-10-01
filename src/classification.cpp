// Power Failure Manager -- typed electrical-failure classification.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/classification.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace summon::pfm {
namespace {

struct FailureClassEntry {
  FailureClass klass;
  std::string_view name;
  std::string_view code;
  std::uint8_t severity;
};

// The severity order is the decision order: ambiguity outranks every concrete
// failure, because unresolved evidence is never downgraded into a smaller
// problem.
constexpr FailureClassEntry kFailureClasses[] = {
    {FailureClass::None, "none", "pfm.failure.none", 0},
    {FailureClass::AmbiguousElectricalEvidence, "ambiguous-electrical-evidence",
     "pfm.failure.ambiguous_evidence", 255},
    {FailureClass::UtilityFeedLoss, "utility-feed-loss", "pfm.failure.utility_feed_loss", 100},
    // The domain finding amplifies scope; it is never the primary cause. The
    // concrete failure that triggered it is named first, while unresolved
    // evidence still outranks both.
    {FailureClass::SharedUpstreamDomainFailure, "shared-upstream-domain-failure",
     "pfm.failure.shared_upstream_domain", 30},
    {FailureClass::SwitchgearFailure, "switchgear-failure", "pfm.failure.switchgear", 90},
    {FailureClass::BusFailure, "bus-failure", "pfm.failure.bus", 85},
    {FailureClass::UpsFailure, "ups-failure", "pfm.failure.ups", 80},
    {FailureClass::PduFailure, "pdu-failure", "pfm.failure.pdu", 75},
    {FailureClass::GeneratorFailure, "generator-failure", "pfm.failure.generator", 70},
    {FailureClass::GeneratorTransferFailure, "generator-transfer-failure",
     "pfm.failure.generator_transfer", 68},
    {FailureClass::GeneratorSyncFailure, "generator-sync-failure", "pfm.failure.generator_sync",
     66},
    {FailureClass::GeneratorStartFailure, "generator-start-failure",
     "pfm.failure.generator_start", 64},
    {FailureClass::UpsReserveInsufficient, "ups-reserve-insufficient",
     "pfm.failure.ups_reserve", 60},
    {FailureClass::CircuitFailure, "circuit-failure", "pfm.failure.circuit", 55},
    {FailureClass::BreakerFailure, "breaker-failure", "pfm.failure.breaker", 50},
    {FailureClass::PduBranchFailure, "pdu-branch-failure", "pfm.failure.pdu_branch", 45},
    {FailureClass::UtilityFeedDegradation, "utility-feed-degradation",
     "pfm.failure.utility_feed_degradation", 40},
};

struct ReasonEntry {
  ReasonCode code;
  std::string_view name;
};

constexpr ReasonEntry kReasonCodes[] = {
    {ReasonCode::None, "none"},
    {ReasonCode::FeedDeEnergized, "feed-de-energized"},
    {ReasonCode::FeedVoltageOutOfBand, "feed-voltage-out-of-band"},
    {ReasonCode::FeedFrequencyOutOfBand, "feed-frequency-out-of-band"},
    {ReasonCode::ElementDeEnergizedWhileUpstreamEnergized,
     "element-de-energized-while-upstream-energized"},
    {ReasonCode::ElementDeEnergizedWithUpstreamUnknown,
     "element-de-energized-with-upstream-unknown"},
    {ReasonCode::BreakerTripped, "breaker-tripped"},
    {ReasonCode::BreakerOpenUnderLoad, "breaker-open-under-load"},
    {ReasonCode::UpsFaulted, "ups-faulted"},
    {ReasonCode::UpsOnBattery, "ups-on-battery"},
    {ReasonCode::UpsReserveBelowFloor, "ups-reserve-below-floor"},
    {ReasonCode::UpsReserveBelowCriticalFloor, "ups-reserve-below-critical-floor"},
    {ReasonCode::GeneratorFaulted, "generator-faulted"},
    {ReasonCode::GeneratorStartWindowExpired, "generator-start-window-expired"},
    {ReasonCode::GeneratorNotSynchronized, "generator-not-synchronized"},
    {ReasonCode::GeneratorTransferFailed, "generator-transfer-failed"},
    {ReasonCode::EvidenceStale, "evidence-stale"},
    {ReasonCode::EvidenceExpired, "evidence-expired"},
    {ReasonCode::EvidenceMissing, "evidence-missing"},
    {ReasonCode::EvidenceContradictory, "evidence-contradictory"},
    {ReasonCode::EvidenceQualityBad, "evidence-quality-bad"},
    {ReasonCode::UpstreamEvidenceUnresolved, "upstream-evidence-unresolved"},
    {ReasonCode::SharedDomainUnresolved, "shared-domain-unresolved"},
    {ReasonCode::DomainMemberFailed, "domain-member-failed"},
    {ReasonCode::ScopeIncludesSharedDomain, "scope-includes-shared-domain"},
    {ReasonCode::DownstreamHealthyDoesNotImplyRecovery, "downstream-healthy-does-not-imply-recovery"},
    {ReasonCode::JustificationWithdrawn, "justification-withdrawn"},
    {ReasonCode::AttemptFailed, "attempt-failed"},
    {ReasonCode::AttemptExpired, "attempt-expired"},
    {ReasonCode::IndeterminateDispatchResolved, "indeterminate-dispatch-resolved"},
    {ReasonCode::DispatchNotAccepted, "dispatch-not-accepted"},
    {ReasonCode::AuthorizationNotCurrent, "authorization-not-current"},
    {ReasonCode::DispatchOutcomeUnknown, "dispatch-outcome-unknown"},
};

const FailureClassEntry& failure_entry(FailureClass klass) noexcept {
  for (const auto& entry : kFailureClasses) {
    if (entry.klass == klass) {
      return entry;
    }
  }
  return kFailureClasses[0];
}

// Which observation of an element is the representative one when several
// current reports agree. Ordering is explicit so that the choice never depends
// on arrival order.
int origin_priority(ObservationOrigin origin) noexcept {
  switch (origin) {
    case ObservationOrigin::ExternalMeter: return 4;
    case ObservationOrigin::ExternalController: return 3;
    case ObservationOrigin::OperatorConsole: return 2;
    case ObservationOrigin::SyntheticPlant: return 1;
    case ObservationOrigin::Unknown: return 0;
  }
  return 0;
}

bool observation_better(const ElectricalObservation& a, const ElectricalObservation& b) noexcept {
  if (a.sequence != b.sequence) {
    return b.sequence < a.sequence;
  }
  const int pa = origin_priority(a.origin);
  const int pb = origin_priority(b.origin);
  if (pa != pb) {
    return pb < pa;
  }
  return a.evidence_fingerprint < b.evidence_fingerprint;
}

void add_reason(ClassifiedFailure& failure, ReasonStep step, const Bounds& bounds) {
  failure.reasons.push_back(std::move(step));
  if (failure.reasons.size() > bounds.max_reason_codes) {
    failure.reasons.resize(bounds.max_reason_codes);
  }
}

void add_evidence(ReasonStep& step, const ElectricalObservation& observation,
                  const Bounds& bounds) {
  step.evidence.push_back(observation.evidence_fingerprint);
  if (step.evidence.size() > bounds.max_reason_evidence_refs) {
    step.evidence.resize(bounds.max_reason_evidence_refs);
  }
}

bool energy_loss_reason(ReasonCode code) noexcept {
  return code == ReasonCode::ElementDeEnergizedWhileUpstreamEnergized ||
         code == ReasonCode::ElementDeEnergizedWithUpstreamUnknown;
}

// Whether the upstream chain of an element already contains a classified
// failure. A de-energized element below a failed element is a consequence, not
// a separate root cause.
bool has_failed_ancestor(const RefToken& element, const TopologyIndex& topology,
                         const std::map<RefToken, FailureClass>& failed) {
  const auto chain = topology.ancestors(element);
  for (const auto& ref : chain) {
    if (ref == element) {
      continue;
    }
    if (failed.find(ref) != failed.end()) {
      return true;
    }
  }
  return false;
}

}  // namespace

std::string_view failure_class_name(FailureClass value) noexcept {
  return failure_entry(value).name;
}

std::string_view failure_class_code(FailureClass value) noexcept {
  return failure_entry(value).code;
}

bool failure_class_is_ambiguous(FailureClass value) noexcept {
  return value == FailureClass::AmbiguousElectricalEvidence;
}

std::uint8_t failure_class_severity(FailureClass value) noexcept {
  return failure_entry(value).severity;
}

std::string_view reason_code_name(ReasonCode value) noexcept {
  for (const auto& entry : kReasonCodes) {
    if (entry.code == value) {
      return entry.name;
    }
  }
  return "unknown";
}

bool ClassificationResult::has_ambiguous() const noexcept {
  for (const auto& failure : failures) {
    if (failure_class_is_ambiguous(failure.klass)) {
      return true;
    }
  }
  return false;
}

FailureClass ClassificationResult::primary_class() const noexcept {
  if (failures.empty()) {
    return FailureClass::None;
  }
  return failures.front().klass;
}

Result<ClassificationResult> classify(const std::vector<ElectricalObservation>& current,
                                      Timestamp now, const TopologyIndex& topology,
                                      const ElectricalPolicy& policy, const Bounds& bounds) {
  if (auto r = validate(policy); !r.ok()) {
    return r.status();
  }
  if (auto r = validate(bounds); !r.ok()) {
    return r.status();
  }
  if (current.size() > bounds.max_observations) {
    return Status::error(StatusCode::BoundsExceeded, "observation set exceeds the bound")
        .with_context("classify.observations");
  }

  // Group by element so that duplicates and contradictions are handled per
  // element, in reference order rather than arrival order.
  std::map<RefToken, std::vector<ElectricalObservation>> grouped;
  for (const auto& observation : current) {
    grouped[observation.element].push_back(observation);
  }

  ClassificationResult result;
  std::map<RefToken, FailureClass> failed_elements;
  std::vector<ClassifiedFailure> candidates;

  for (auto& [element, observations] : grouped) {
    const auto* topology_element = topology.find(element);
    const ElementKind kind =
        topology_element != nullptr ? topology_element->kind : observations.front().kind;

    // A contradiction is never resolved by preferring one report.
    bool contradicted = false;
    for (std::size_t i = 0; i < observations.size() && !contradicted; ++i) {
      for (std::size_t j = i + 1; j < observations.size(); ++j) {
        if (observations_conflict(observations[i], observations[j], policy)) {
          contradicted = true;
          break;
        }
      }
    }
    if (contradicted) {
      result.unresolved_elements.push_back(element);
      if (policy.treat_stale_evidence_as_failure) {
        ClassifiedFailure failure;
        failure.klass = FailureClass::AmbiguousElectricalEvidence;
        failure.element = element;
        failure.kind = kind;
        failure.primary_reason = ReasonCode::EvidenceContradictory;
        failure.evidence_current = false;
        ReasonStep step;
        step.code = ReasonCode::EvidenceContradictory;
        step.subject = element;
        step.freshness = Freshness::Fresh;
        for (const auto& observation : observations) {
          add_evidence(step, observation, bounds);
        }
        add_reason(failure, step, bounds);
        candidates.push_back(std::move(failure));
      }
      continue;
    }

    std::sort(observations.begin(), observations.end(), observation_better);
    const ElectricalObservation& observation = observations.front();

    // The runtime selects current observations, but classification still
    // refuses to use one whose quality is bad.
    if (observation.quality == EvidenceQuality::Bad) {
      result.unresolved_elements.push_back(element);
      if (policy.treat_stale_evidence_as_failure) {
        ClassifiedFailure failure;
        failure.klass = FailureClass::AmbiguousElectricalEvidence;
        failure.element = element;
        failure.kind = kind;
        failure.primary_reason = ReasonCode::EvidenceQualityBad;
        failure.evidence_current = false;
        ReasonStep step;
        step.code = ReasonCode::EvidenceQualityBad;
        step.subject = element;
        step.freshness = Freshness::Unknown;
        add_evidence(step, observation, bounds);
        add_reason(failure, step, bounds);
        candidates.push_back(std::move(failure));
      }
      continue;
    }

    const Freshness freshness = classify_freshness(observation.observed_at, now, policy);
    if (freshness != Freshness::Fresh) {
      result.unresolved_elements.push_back(element);
      if (policy.treat_stale_evidence_as_failure) {
        ReasonCode reason = ReasonCode::EvidenceStale;
        if (freshness == Freshness::Expired) {
          reason = ReasonCode::EvidenceExpired;
        } else if (freshness == Freshness::Future) {
          reason = ReasonCode::EvidenceStale;
        } else if (freshness == Freshness::Unknown) {
          reason = ReasonCode::EvidenceMissing;
        }
        ClassifiedFailure failure;
        failure.klass = FailureClass::AmbiguousElectricalEvidence;
        failure.element = element;
        failure.kind = kind;
        failure.primary_reason = reason;
        failure.evidence_current = false;
        ReasonStep step;
        step.code = reason;
        step.subject = element;
        step.freshness = freshness;
        add_evidence(step, observation, bounds);
        add_reason(failure, step, bounds);
        candidates.push_back(std::move(failure));
      }
      continue;
    }

    result.resolved_elements.push_back(element);

    // The supply chain at classification time. A loss of supply that the
    // upstream chain already explains is collateral, not a new root cause;
    // only an element that is de-energized while its entire chain is energized
    // has a break of its own.
    EnergizationState upstream_state = EnergizationState::Energized;
    {
      bool all_energized = true;
      bool any_de_energized = false;
      for (const auto& ancestor : topology.ancestors(element)) {
        if (ancestor == element) {
          continue;
        }
        const auto found = grouped.find(ancestor);
        if (found == grouped.end() || found->second.empty()) {
          all_energized = false;
          continue;
        }
        const auto state = found->second.front().energization;
        if (state == EnergizationState::Energized) {
          continue;
        }
        all_energized = false;
        if (state == EnergizationState::DeEnergized ||
            state == EnergizationState::PartiallyEnergized) {
          any_de_energized = true;
        }
      }
      if (all_energized) {
        upstream_state = EnergizationState::Energized;
      } else if (any_de_energized) {
        upstream_state = EnergizationState::DeEnergized;
      } else {
        upstream_state = EnergizationState::Unknown;
      }
    }

    const auto make_failure = [&](FailureClass klass, ReasonCode reason) {
      ClassifiedFailure failure;
      failure.klass = klass;
      failure.element = element;
      failure.kind = kind;
      failure.primary_reason = reason;
      failure.evidence_current = true;
      ReasonStep step;
      step.code = reason;
      step.subject = element;
      step.freshness = Freshness::Fresh;
      add_evidence(step, observation, bounds);
      add_reason(failure, step, bounds);
      return failure;
    };

    const bool upstream_energized = upstream_state == EnergizationState::Energized;
    const bool upstream_known = upstream_state != EnergizationState::Unknown;
    const ReasonCode loss_reason = upstream_energized
                                       ? ReasonCode::ElementDeEnergizedWhileUpstreamEnergized
                                       : ReasonCode::ElementDeEnergizedWithUpstreamUnknown;

    switch (kind) {
      case ElementKind::UtilityFeed: {
        if (observation.energization == EnergizationState::DeEnergized) {
          candidates.push_back(make_failure(FailureClass::UtilityFeedLoss, ReasonCode::FeedDeEnergized));
        } else if (observation.energization == EnergizationState::PartiallyEnergized) {
          candidates.push_back(
              make_failure(FailureClass::UtilityFeedDegradation, ReasonCode::FeedVoltageOutOfBand));
        } else if (observation.energization == EnergizationState::Energized) {
          if (observation.has_voltage) {
            auto within = voltage_within_tolerance(policy, observation.voltage);
            if (!within.ok()) {
              return within.status();
            }
            if (!within.value()) {
              candidates.push_back(make_failure(FailureClass::UtilityFeedDegradation,
                                                ReasonCode::FeedVoltageOutOfBand));
            }
          }
          if (observation.has_frequency) {
            auto within = frequency_within_tolerance(policy, observation.frequency);
            if (!within.ok()) {
              return within.status();
            }
            if (!within.value()) {
              candidates.push_back(make_failure(FailureClass::UtilityFeedDegradation,
                                                ReasonCode::FeedFrequencyOutOfBand));
            }
          }
        }
        break;
      }
      case ElementKind::Switchgear:
      case ElementKind::Bus: {
        const FailureClass klass = kind == ElementKind::Switchgear ? FailureClass::SwitchgearFailure
                                                                  : FailureClass::BusFailure;
        if (observation.breaker == BreakerPosition::Tripped) {
          candidates.push_back(make_failure(klass, ReasonCode::BreakerTripped));
        } else if (observation.energization == EnergizationState::DeEnergized &&
                   upstream_state != EnergizationState::DeEnergized) {
          candidates.push_back(make_failure(klass, loss_reason));
        }
        break;
      }
      case ElementKind::Breaker:
      case ElementKind::Circuit: {
        const FailureClass klass = kind == ElementKind::Breaker ? FailureClass::BreakerFailure
                                                                : FailureClass::CircuitFailure;
        if (observation.breaker == BreakerPosition::Tripped) {
          candidates.push_back(make_failure(klass, ReasonCode::BreakerTripped));
        } else if (observation.breaker == BreakerPosition::Open &&
                   observation.energization == EnergizationState::DeEnergized &&
                   upstream_energized) {
          // An open breaker is a deliberate state; it is only a failure when
          // the element below it is still carrying load, which is observed
          // separately. Without that evidence the open position is recorded as
          // a reason on the downstream element instead.
          break;
        } else if (observation.energization == EnergizationState::DeEnergized && upstream_known &&
                   upstream_energized) {
          candidates.push_back(make_failure(klass, loss_reason));
        }
        break;
      }
      case ElementKind::Pdu: {
        if (observation.energization == EnergizationState::DeEnergized &&
            upstream_state != EnergizationState::DeEnergized) {
          candidates.push_back(make_failure(FailureClass::PduFailure, loss_reason));
        }
        break;
      }
      case ElementKind::PduBranch: {
        if (observation.energization == EnergizationState::DeEnergized &&
            upstream_state != EnergizationState::DeEnergized) {
          candidates.push_back(make_failure(FailureClass::PduBranchFailure, loss_reason));
        }
        break;
      }
      case ElementKind::UpsUnit:
      case ElementKind::UpsBus: {
        if (observation.energization == EnergizationState::DeEnergized) {
          candidates.push_back(make_failure(FailureClass::UpsFailure, ReasonCode::UpsFaulted));
        } else if (observation.energization == EnergizationState::PartiallyEnergized) {
          candidates.push_back(make_failure(FailureClass::UpsFailure, ReasonCode::UpsOnBattery));
        }
        if (observation.has_reserve) {
          if (observation.reserve <= policy.reserve_critical_floor) {
            candidates.push_back(make_failure(FailureClass::UpsReserveInsufficient,
                                              ReasonCode::UpsReserveBelowCriticalFloor));
          } else if (observation.reserve <= policy.reserve_floor) {
            candidates.push_back(make_failure(FailureClass::UpsReserveInsufficient,
                                              ReasonCode::UpsReserveBelowFloor));
          }
        }
        break;
      }
      case ElementKind::Generator: {
        if (observation.generator == GeneratorState::Faulted) {
          candidates.push_back(make_failure(FailureClass::GeneratorFailure, ReasonCode::GeneratorFaulted));
        } else if (observation.transfer == TransferState::Failed) {
          if (observation.generator == GeneratorState::Off ||
              observation.generator == GeneratorState::CoolDown) {
            candidates.push_back(make_failure(FailureClass::GeneratorStartFailure,
                                              ReasonCode::GeneratorStartWindowExpired));
          } else if (observation.generator == GeneratorState::Running) {
            candidates.push_back(make_failure(FailureClass::GeneratorSyncFailure,
                                              ReasonCode::GeneratorNotSynchronized));
          } else if (observation.generator == GeneratorState::Synchronized) {
            candidates.push_back(make_failure(FailureClass::GeneratorTransferFailure,
                                              ReasonCode::GeneratorTransferFailed));
          }
        } else if (observation.transfer == TransferState::Transferring &&
                   observation.generator == GeneratorState::Running) {
          candidates.push_back(make_failure(FailureClass::GeneratorSyncFailure,
                                            ReasonCode::GeneratorNotSynchronized));
        }
        break;
      }
      case ElementKind::AutomaticTransferSwitch:
      case ElementKind::StaticTransferSwitch: {
        if (observation.transfer == TransferState::Failed) {
          candidates.push_back(
              make_failure(FailureClass::GeneratorTransferFailure, ReasonCode::GeneratorTransferFailed));
        }
        break;
      }
      case ElementKind::LoadGroup:
      case ElementKind::Rack:
      case ElementKind::Unknown:
        break;
    }
  }

  // Collateral de-energization is not a separate root cause: an element whose
  // failure is only "it lost supply" and which sits below a failed element is
  // captured by the affected scope instead. Depth ordering makes the outcome
  // independent of arrival order: an ancestor is always considered first.
  std::stable_sort(candidates.begin(), candidates.end(),
                   [&topology](const ClassifiedFailure& a, const ClassifiedFailure& b) {
                     if (energy_loss_reason(a.primary_reason) !=
                         energy_loss_reason(b.primary_reason)) {
                       return !energy_loss_reason(a.primary_reason);
                     }
                     if (energy_loss_reason(a.primary_reason)) {
                       const auto depth_a = topology.ancestors(a.element).size();
                       const auto depth_b = topology.ancestors(b.element).size();
                       if (depth_a != depth_b) {
                         return depth_a < depth_b;
                       }
                     }
                     return a < b;
                   });
  for (const auto& candidate : candidates) {
    if (energy_loss_reason(candidate.primary_reason) &&
        has_failed_ancestor(candidate.element, topology, failed_elements)) {
      continue;
    }
    failed_elements[candidate.element] = candidate.klass;
    result.failures.push_back(candidate);
  }

  // Shared upstream domains are promoted to a typed failure of their own: a
  // locally healthy element inside a shared domain is not recoverable while
  // the domain itself is unresolved.
  std::vector<ClassifiedFailure> domain_failures;
  for (const auto& failure : result.failures) {
    if (failure.element.kind() == RefKind::FailureDomain) {
      continue;
    }
    for (const auto& domain_ref : topology.domains_of(failure.element)) {
      if (!topology.domain_is_shared(domain_ref)) {
        continue;
      }
      ClassifiedFailure domain_failure;
      domain_failure.klass = FailureClass::SharedUpstreamDomainFailure;
      domain_failure.element = domain_ref;
      domain_failure.kind = ElementKind::Unknown;
      domain_failure.primary_reason = ReasonCode::DomainMemberFailed;
      domain_failure.evidence_current = failure.evidence_current;
      ReasonStep step;
      step.code = ReasonCode::DomainMemberFailed;
      step.subject = failure.element;
      step.freshness = Freshness::Fresh;
      step.evidence = failure.reasons.empty() ? std::vector<Fingerprint>{}
                                              : failure.reasons.front().evidence;
      add_reason(domain_failure, step, bounds);
      ReasonStep shared;
      shared.code = ReasonCode::SharedDomainUnresolved;
      shared.subject = domain_ref;
      shared.freshness = Freshness::Fresh;
      add_reason(domain_failure, shared, bounds);
      domain_failures.push_back(std::move(domain_failure));
    }
  }
  for (auto& failure : domain_failures) {
    bool duplicate = false;
    for (const auto& existing : result.failures) {
      if (existing.klass == failure.klass && existing.element == failure.element) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      result.failures.push_back(std::move(failure));
    }
  }

  for (auto& failure : result.failures) {
    std::sort(failure.reasons.begin(), failure.reasons.end());
    failure.reasons.erase(std::unique(failure.reasons.begin(), failure.reasons.end()),
                          failure.reasons.end());
    if (failure.reasons.size() > bounds.max_reason_codes) {
      failure.reasons.resize(bounds.max_reason_codes);
    }
    if (failure.primary_reason == ReasonCode::None && !failure.reasons.empty()) {
      failure.primary_reason = failure.reasons.front().code;
    }
  }
  std::sort(result.failures.begin(), result.failures.end());
  std::sort(result.unresolved_elements.begin(), result.unresolved_elements.end());
  result.unresolved_elements.erase(
      std::unique(result.unresolved_elements.begin(), result.unresolved_elements.end()),
      result.unresolved_elements.end());
  std::sort(result.resolved_elements.begin(), result.resolved_elements.end());
  result.resolved_elements.erase(
      std::unique(result.resolved_elements.begin(), result.resolved_elements.end()),
      result.resolved_elements.end());
  return result;
}

}  // namespace summon::pfm
