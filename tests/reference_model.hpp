// Power Failure Manager -- independent reference model for the state machine tests.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// This header is deliberately NOT a second implementation of the library: it
// repeats the documented semantics from first principles so that a test can
// compare two independently derived answers and report the difference.
//
//   * Adjacency is rebuilt by scanning a TopologySnapshot. Closures are computed
//     by repeated relaxation until a fixed point is reached; TopologyIndex is
//     never used.
//   * Classification predicates for a small set of element kinds are evaluated
//     directly from an observation.
//   * Recovery gates are evaluated directly from the plan, the observations,
//     the obligations, and the policy.
//
// It is intentionally simple and slow. Correctness by inspection matters more
// here than speed.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "pfm/classification.hpp"
#include "pfm/obligations.hpp"
#include "pfm/plan.hpp"
#include "pfm/recovery.hpp"
#include "pfm/scope.hpp"
#include "pfm/topology.hpp"

namespace pfmref {

using summon::pfm::BreakerPosition;
using summon::pfm::ClassifiedFailure;
using summon::pfm::ElectricalObservation;
using summon::pfm::ElectricalPolicy;
using summon::pfm::ElementKind;
using summon::pfm::EnergizationState;
using summon::pfm::EvidenceQuality;
using summon::pfm::FailureClass;
using summon::pfm::FailureDomain;
using summon::pfm::Freshness;
using summon::pfm::GeneratorState;
using summon::pfm::IncidentGeneration;
using summon::pfm::MilliHertz;
using summon::pfm::MilliPercent;
using summon::pfm::MilliVolts;
using summon::pfm::ObservationOrigin;
using summon::pfm::ProtectedObligation;
using summon::pfm::ReasonCode;
using summon::pfm::RecoveryGate;
using summon::pfm::RefKind;
using summon::pfm::RefToken;
using summon::pfm::ResponsePlan;
using summon::pfm::Timestamp;
using summon::pfm::TopologyElement;
using summon::pfm::TopologySnapshot;
using summon::pfm::TransferState;

// --- small helpers ---------------------------------------------------------

inline std::vector<RefToken> model_sorted_unique(std::vector<RefToken> values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
  return values;
}

inline bool model_has_ref(const std::vector<RefToken>& values, const RefToken& ref) {
  return std::find(values.begin(), values.end(), ref) != values.end();
}

// Severity is the documented decision order: ambiguity outranks every concrete
// failure, then the concrete classes in the order the failure taxonomy lists
// them. The numbers are repeated here on purpose; a test compares them with
// failure_class_severity().
inline int model_severity(FailureClass klass) {
  switch (klass) {
    case FailureClass::AmbiguousElectricalEvidence: return 255;
    case FailureClass::UtilityFeedLoss: return 100;
    case FailureClass::SharedUpstreamDomainFailure: return 30;
    case FailureClass::SwitchgearFailure: return 90;
    case FailureClass::BusFailure: return 85;
    case FailureClass::UpsFailure: return 80;
    case FailureClass::PduFailure: return 75;
    case FailureClass::GeneratorFailure: return 70;
    case FailureClass::GeneratorTransferFailure: return 68;
    case FailureClass::GeneratorSyncFailure: return 66;
    case FailureClass::GeneratorStartFailure: return 64;
    case FailureClass::UpsReserveInsufficient: return 60;
    case FailureClass::CircuitFailure: return 55;
    case FailureClass::BreakerFailure: return 50;
    case FailureClass::PduBranchFailure: return 45;
    case FailureClass::UtilityFeedDegradation: return 40;
    case FailureClass::None: return 0;
  }
  return 0;
}

// Ordered by severity (most severe first), then class, then reference.
inline bool model_failure_less(const ClassifiedFailure& a, const ClassifiedFailure& b) {
  const int sa = model_severity(a.klass);
  const int sb = model_severity(b.klass);
  if (sa != sb) {
    return sa > sb;
  }
  if (a.klass != b.klass) {
    return static_cast<int>(a.klass) < static_cast<int>(b.klass);
  }
  return a.element < b.element;
}

inline bool model_energy_loss_reason(ReasonCode code) {
  return code == ReasonCode::ElementDeEnergizedWhileUpstreamEnergized ||
         code == ReasonCode::ElementDeEnergizedWithUpstreamUnknown;
}

// References that designate something that can report electrical state. A
// failure domain, a load group, and a rack cannot: their silence is not
// evidence, so they are not evidence questions.
inline bool model_ref_is_monitored(RefKind kind) {
  switch (kind) {
    case RefKind::UtilityFeed:
    case RefKind::Switchgear:
    case RefKind::Bus:
    case RefKind::Breaker:
    case RefKind::Circuit:
    case RefKind::Pdu:
    case RefKind::PduBranch:
    case RefKind::UpsUnit:
    case RefKind::UpsBus:
    case RefKind::StaticTransferSwitch:
    case RefKind::AutomaticTransferSwitch:
    case RefKind::Generator:
      return true;
    default:
      return false;
  }
}

// --- topology --------------------------------------------------------------

// Adjacency taken straight from the snapshot, with a brute-force closure.
struct ModelTopology {
  std::vector<TopologyElement> elements{};
  std::vector<FailureDomain> domains{};

  static ModelTopology from(const TopologySnapshot& snapshot) {
    ModelTopology model;
    model.elements = snapshot.elements;
    std::sort(model.elements.begin(), model.elements.end(),
              [](const TopologyElement& a, const TopologyElement& b) {
                return a.element < b.element;
              });
    model.domains = snapshot.domains;
    for (auto& domain : model.domains) {
      domain.members = model_sorted_unique(domain.members);
    }
    std::sort(model.domains.begin(), model.domains.end(),
              [](const FailureDomain& a, const FailureDomain& b) {
                return a.domain < b.domain;
              });
    return model;
  }

  [[nodiscard]] const TopologyElement* find(const RefToken& element) const {
    for (const auto& candidate : elements) {
      if (candidate.element == element) {
        return &candidate;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const FailureDomain* domain(const RefToken& domain_ref) const {
    for (const auto& candidate : domains) {
      if (candidate.domain == domain_ref) {
        return &candidate;
      }
    }
    return nullptr;
  }

  // The element itself followed by its supply chain, nearest first.
  [[nodiscard]] std::vector<RefToken> chain(const RefToken& element) const {
    std::vector<RefToken> result;
    const TopologyElement* current = find(element);
    std::size_t guard = 0;
    while (current != nullptr && guard++ <= elements.size()) {
      result.push_back(current->element);
      if (!current->upstream.is_set()) {
        break;
      }
      current = find(current->upstream);
    }
    return result;
  }

  [[nodiscard]] std::vector<RefToken> ancestors(const RefToken& element) const {
    std::vector<RefToken> result = chain(element);
    if (!result.empty()) {
      result.erase(result.begin());
    }
    return result;
  }

  // Repeated relaxation until no further element can be reached from the
  // supplied consumers.
  [[nodiscard]] std::vector<RefToken> closure(const RefToken& element) const {
    std::vector<RefToken> reached;
    if (find(element) == nullptr) {
      return reached;
    }
    reached.push_back(element);
    bool changed = true;
    while (changed) {
      changed = false;
      for (const auto& candidate : elements) {
        if (!candidate.upstream.is_set()) {
          continue;
        }
        if (model_has_ref(reached, candidate.element)) {
          continue;
        }
        if (model_has_ref(reached, candidate.upstream)) {
          reached.push_back(candidate.element);
          changed = true;
        }
      }
    }
    return model_sorted_unique(reached);
  }

  [[nodiscard]] std::vector<RefToken> load_groups_under(const RefToken& element) const {
    std::vector<RefToken> result;
    for (const auto& ref : closure(element)) {
      const TopologyElement* found = find(ref);
      if (found == nullptr) {
        continue;
      }
      if (found->load_group.is_set()) {
        result.push_back(found->load_group);
      }
      if (ref.kind() == RefKind::LoadGroup) {
        result.push_back(ref);
      }
    }
    return model_sorted_unique(result);
  }

  [[nodiscard]] std::vector<RefToken> domains_of(const RefToken& element) const {
    std::vector<RefToken> result;
    const TopologyElement* found = find(element);
    if (found == nullptr) {
      return result;
    }
    if (found->domain.is_set()) {
      result.push_back(found->domain);
    }
    for (const auto& candidate : domains) {
      if (model_has_ref(candidate.members, element)) {
        result.push_back(candidate.domain);
      }
    }
    return model_sorted_unique(result);
  }

  // A domain is shared when the supplying authority flagged it, or when its
  // members sit in more than one distinct power domain.
  [[nodiscard]] bool domain_shared(const RefToken& domain_ref) const {
    const FailureDomain* found = domain(domain_ref);
    if (found == nullptr) {
      return false;
    }
    if (found->shared) {
      return true;
    }
    std::vector<RefToken> downstream;
    for (const auto& member : found->members) {
      const TopologyElement* element = find(member);
      if (element != nullptr && element->domain.is_set() &&
          element->domain.kind() == RefKind::PowerDomain) {
        downstream.push_back(element->domain);
      }
    }
    return model_sorted_unique(downstream).size() > 1;
  }

  [[nodiscard]] bool isolation_point_of(const RefToken& element, RefToken& out) const {
    const TopologyElement* found = find(element);
    if (found == nullptr || !found->has_isolation_point) {
      return false;
    }
    out = found->isolation_point;
    return true;
  }

  // The element's own isolation point, or the nearest upstream one.
  [[nodiscard]] bool isolation_point_for(const RefToken& element, RefToken& out) const {
    for (const auto& ref : chain(element)) {
      if (isolation_point_of(ref, out)) {
        return true;
      }
    }
    return false;
  }
};

// --- classification --------------------------------------------------------

inline int model_origin_rank(ObservationOrigin origin) {
  switch (origin) {
    case ObservationOrigin::ExternalMeter: return 4;
    case ObservationOrigin::ExternalController: return 3;
    case ObservationOrigin::OperatorConsole: return 2;
    case ObservationOrigin::SyntheticPlant: return 1;
    case ObservationOrigin::Unknown: return 0;
  }
  return 0;
}

// Highest sequence wins; ties are broken by origin priority then fingerprint.
inline std::size_t model_representative(std::vector<ElectricalObservation> observations) {
  std::sort(observations.begin(), observations.end(),
            [](const ElectricalObservation& a, const ElectricalObservation& b) {
              if (!(a.sequence == b.sequence)) {
                return b.sequence < a.sequence;
              }
              const int pa = model_origin_rank(a.origin);
              const int pb = model_origin_rank(b.origin);
              if (pa != pb) {
                return pb < pa;
              }
              return a.evidence_fingerprint < b.evidence_fingerprint;
            });
  return 0;
}

inline bool model_contradicts(const ElectricalObservation& a, const ElectricalObservation& b) {
  if (!(a.element == b.element)) {
    return false;
  }
  if (a.origin == b.origin && a.sequence == b.sequence) {
    return false;
  }
  if (a.energization != EnergizationState::Unknown &&
      b.energization != EnergizationState::Unknown && a.energization != b.energization) {
    return true;
  }
  const bool a_closed = a.breaker == BreakerPosition::Closed;
  const bool b_closed = b.breaker == BreakerPosition::Closed;
  if (a.breaker != BreakerPosition::Unknown && b.breaker != BreakerPosition::Unknown &&
      a_closed != b_closed) {
    return true;
  }
  if (a.generator != GeneratorState::Unknown && b.generator != GeneratorState::Unknown &&
      a.generator != b.generator) {
    const bool a_running = a.generator == GeneratorState::Running ||
                           a.generator == GeneratorState::Synchronized;
    const bool b_running = b.generator == GeneratorState::Running ||
                           b.generator == GeneratorState::Synchronized;
    const bool a_stopped = a.generator == GeneratorState::Off ||
                           a.generator == GeneratorState::Faulted;
    const bool b_stopped = b.generator == GeneratorState::Off ||
                           b.generator == GeneratorState::Faulted;
    if ((a_running && b_stopped) || (b_running && a_stopped)) {
      return true;
    }
  }
  if (a.transfer != TransferState::Unknown && b.transfer != TransferState::Unknown &&
      a.transfer != b.transfer) {
    return true;
  }
  return false;
}

inline Freshness model_freshness(Timestamp observed_at, Timestamp now,
                                 const ElectricalPolicy& policy) {
  if (observed_at.value() == 0) {
    return Freshness::Unknown;
  }
  const std::int64_t delta = now.value() - observed_at.value();
  if (delta < 0) {
    return Freshness::Future;
  }
  if (delta <= policy.evidence_freshness_window.value()) {
    return Freshness::Fresh;
  }
  if (delta <= policy.evidence_expiry_window.value()) {
    return Freshness::Stale;
  }
  return Freshness::Expired;
}

inline bool model_voltage_in_band(const ElectricalPolicy& policy, MilliVolts reading) {
  if (reading.value() <= 0) {
    return false;
  }
  const std::int64_t limit =
      (policy.nominal_voltage.value() * policy.voltage_tolerance.value()) / 100000;
  const std::int64_t difference = reading.value() - policy.nominal_voltage.value();
  return (difference < 0 ? -difference : difference) <= limit;
}

inline bool model_frequency_in_band(const ElectricalPolicy& policy, MilliHertz reading) {
  if (reading.value() <= 0) {
    return false;
  }
  const std::int64_t limit =
      (policy.nominal_frequency.value() * policy.frequency_tolerance.value()) / 100000;
  const std::int64_t difference = reading.value() - policy.nominal_frequency.value();
  return (difference < 0 ? -difference : difference) <= limit;
}

struct ModelClassification {
  std::vector<RefToken> resolved{};
  std::vector<RefToken> unresolved{};
  std::vector<ClassifiedFailure> failures{};
  std::vector<RefToken> failed_elements{};
};

namespace detail {

inline void push_ambiguous(std::vector<ClassifiedFailure>& out, const RefToken& element,
                           ElementKind kind, ReasonCode reason, bool treat_as_failure) {
  if (!treat_as_failure) {
    return;
  }
  ClassifiedFailure failure;
  failure.klass = FailureClass::AmbiguousElectricalEvidence;
  failure.element = element;
  failure.kind = kind;
  failure.primary_reason = reason;
  failure.evidence_current = false;
  out.push_back(failure);
}

inline void push_candidate(std::vector<ClassifiedFailure>& out, const RefToken& element,
                           ElementKind kind, FailureClass klass, ReasonCode reason) {
  ClassifiedFailure failure;
  failure.klass = klass;
  failure.element = element;
  failure.kind = kind;
  failure.primary_reason = reason;
  failure.evidence_current = true;
  out.push_back(failure);
}

}  // namespace detail

// Directly evaluated classification for the element kinds PFM reasons about.
inline ModelClassification model_classify(const std::vector<ElectricalObservation>& current,
                                          Timestamp now, const ModelTopology& topology,
                                          const ElectricalPolicy& policy) {
  ModelClassification result;
  std::map<RefToken, std::vector<ElectricalObservation>> grouped;
  for (const auto& observation : current) {
    grouped[observation.element].push_back(observation);
  }
  std::vector<ClassifiedFailure> candidates;

  for (auto& entry : grouped) {
    const RefToken element = entry.first;
    std::vector<ElectricalObservation>& observations = entry.second;
    const TopologyElement* topology_element = topology.find(element);
    const ElementKind kind = topology_element != nullptr
                                 ? topology_element->kind
                                 : (observations.empty() ? ElementKind::Unknown
                                                         : observations.front().kind);

    bool contradicted = false;
    for (std::size_t i = 0; i < observations.size() && !contradicted; ++i) {
      for (std::size_t j = i + 1; j < observations.size(); ++j) {
        if (model_contradicts(observations[i], observations[j])) {
          contradicted = true;
          break;
        }
      }
    }
    if (contradicted) {
      result.unresolved.push_back(element);
      detail::push_ambiguous(candidates, element, kind, ReasonCode::EvidenceContradictory,
                             policy.treat_stale_evidence_as_failure);
      continue;
    }

    const ElectricalObservation observation =
        observations[model_representative(observations)];
    if (observation.quality == EvidenceQuality::Bad) {
      result.unresolved.push_back(element);
      detail::push_ambiguous(candidates, element, kind, ReasonCode::EvidenceQualityBad,
                             policy.treat_stale_evidence_as_failure);
      continue;
    }
    const Freshness freshness = model_freshness(observation.observed_at, now, policy);
    if (freshness != Freshness::Fresh) {
      result.unresolved.push_back(element);
      ReasonCode reason = ReasonCode::EvidenceStale;
      if (freshness == Freshness::Expired) {
        reason = ReasonCode::EvidenceExpired;
      } else if (freshness == Freshness::Unknown) {
        reason = ReasonCode::EvidenceMissing;
      }
      detail::push_ambiguous(candidates, element, kind, reason,
                             policy.treat_stale_evidence_as_failure);
      continue;
    }
    result.resolved.push_back(element);

    // The supply chain of the element at this instant.
    bool all_energized = true;
    bool any_de_energized = false;
    for (const auto& ancestor : topology.ancestors(element)) {
      const auto found = grouped.find(ancestor);
      if (found == grouped.end() || found->second.empty()) {
        all_energized = false;
        continue;
      }
      const EnergizationState state = found->second.front().energization;
      if (state == EnergizationState::Energized) {
        continue;
      }
      all_energized = false;
      if (state == EnergizationState::DeEnergized ||
          state == EnergizationState::PartiallyEnergized) {
        any_de_energized = true;
      }
    }
    EnergizationState upstream_state = EnergizationState::Unknown;
    if (all_energized) {
      upstream_state = EnergizationState::Energized;
    } else if (any_de_energized) {
      upstream_state = EnergizationState::DeEnergized;
    }
    const bool upstream_energized = upstream_state == EnergizationState::Energized;
    const bool upstream_known = upstream_state != EnergizationState::Unknown;
    const ReasonCode loss_reason = upstream_energized
                                       ? ReasonCode::ElementDeEnergizedWhileUpstreamEnergized
                                       : ReasonCode::ElementDeEnergizedWithUpstreamUnknown;
    const bool de_energized = observation.energization == EnergizationState::DeEnergized;

    switch (kind) {
      case ElementKind::UtilityFeed: {
        if (de_energized) {
          detail::push_candidate(candidates, element, kind, FailureClass::UtilityFeedLoss,
                                 ReasonCode::FeedDeEnergized);
        } else if (observation.energization == EnergizationState::PartiallyEnergized) {
          detail::push_candidate(candidates, element, kind, FailureClass::UtilityFeedDegradation,
                                 ReasonCode::FeedVoltageOutOfBand);
        } else if (observation.energization == EnergizationState::Energized) {
          if (observation.has_voltage &&
              !model_voltage_in_band(policy, observation.voltage)) {
            detail::push_candidate(candidates, element, kind,
                                   FailureClass::UtilityFeedDegradation,
                                   ReasonCode::FeedVoltageOutOfBand);
          }
          if (observation.has_frequency &&
              !model_frequency_in_band(policy, observation.frequency)) {
            detail::push_candidate(candidates, element, kind,
                                   FailureClass::UtilityFeedDegradation,
                                   ReasonCode::FeedFrequencyOutOfBand);
          }
        }
        break;
      }
      case ElementKind::Switchgear:
      case ElementKind::Bus: {
        const FailureClass klass = kind == ElementKind::Switchgear ? FailureClass::SwitchgearFailure
                                                                  : FailureClass::BusFailure;
        if (observation.breaker == BreakerPosition::Tripped) {
          detail::push_candidate(candidates, element, kind, klass, ReasonCode::BreakerTripped);
        } else if (de_energized && upstream_state != EnergizationState::DeEnergized) {
          detail::push_candidate(candidates, element, kind, klass, loss_reason);
        }
        break;
      }
      case ElementKind::Breaker:
      case ElementKind::Circuit: {
        const FailureClass klass = kind == ElementKind::Breaker ? FailureClass::BreakerFailure
                                                                : FailureClass::CircuitFailure;
        if (observation.breaker == BreakerPosition::Tripped) {
          detail::push_candidate(candidates, element, kind, klass, ReasonCode::BreakerTripped);
        } else if (observation.breaker == BreakerPosition::Open && de_energized &&
                   upstream_energized) {
          // A deliberate open position is not a failure on its own.
          break;
        } else if (de_energized && upstream_known && upstream_energized) {
          detail::push_candidate(candidates, element, kind, klass, loss_reason);
        }
        break;
      }
      case ElementKind::Pdu: {
        if (de_energized && upstream_state != EnergizationState::DeEnergized) {
          detail::push_candidate(candidates, element, kind, FailureClass::PduFailure, loss_reason);
        }
        break;
      }
      case ElementKind::PduBranch: {
        if (de_energized && upstream_state != EnergizationState::DeEnergized) {
          detail::push_candidate(candidates, element, kind, FailureClass::PduBranchFailure,
                                 loss_reason);
        }
        break;
      }
      case ElementKind::UpsUnit:
      case ElementKind::UpsBus: {
        if (de_energized) {
          detail::push_candidate(candidates, element, kind, FailureClass::UpsFailure,
                                 ReasonCode::UpsFaulted);
        } else if (observation.energization == EnergizationState::PartiallyEnergized) {
          detail::push_candidate(candidates, element, kind, FailureClass::UpsFailure,
                                 ReasonCode::UpsOnBattery);
        }
        if (observation.has_reserve) {
          if (observation.reserve <= policy.reserve_critical_floor) {
            detail::push_candidate(candidates, element, kind,
                                   FailureClass::UpsReserveInsufficient,
                                   ReasonCode::UpsReserveBelowCriticalFloor);
          } else if (observation.reserve <= policy.reserve_floor) {
            detail::push_candidate(candidates, element, kind,
                                   FailureClass::UpsReserveInsufficient,
                                   ReasonCode::UpsReserveBelowFloor);
          }
        }
        break;
      }
      case ElementKind::Generator: {
        if (observation.generator == GeneratorState::Faulted) {
          detail::push_candidate(candidates, element, kind, FailureClass::GeneratorFailure,
                                 ReasonCode::GeneratorFaulted);
        } else if (observation.transfer == TransferState::Failed) {
          if (observation.generator == GeneratorState::Off ||
              observation.generator == GeneratorState::CoolDown) {
            detail::push_candidate(candidates, element, kind,
                                   FailureClass::GeneratorStartFailure,
                                   ReasonCode::GeneratorStartWindowExpired);
          } else if (observation.generator == GeneratorState::Running) {
            detail::push_candidate(candidates, element, kind,
                                   FailureClass::GeneratorSyncFailure,
                                   ReasonCode::GeneratorNotSynchronized);
          } else if (observation.generator == GeneratorState::Synchronized) {
            detail::push_candidate(candidates, element, kind,
                                   FailureClass::GeneratorTransferFailure,
                                   ReasonCode::GeneratorTransferFailed);
          }
        } else if (observation.transfer == TransferState::Transferring &&
                   observation.generator == GeneratorState::Running) {
          detail::push_candidate(candidates, element, kind, FailureClass::GeneratorSyncFailure,
                                 ReasonCode::GeneratorNotSynchronized);
        }
        break;
      }
      case ElementKind::AutomaticTransferSwitch:
      case ElementKind::StaticTransferSwitch: {
        if (observation.transfer == TransferState::Failed) {
          detail::push_candidate(candidates, element, kind,
                                 FailureClass::GeneratorTransferFailure,
                                 ReasonCode::GeneratorTransferFailed);
        }
        break;
      }
      case ElementKind::LoadGroup:
      case ElementKind::Rack:
      case ElementKind::Unknown:
        break;
    }
  }

  // Only an element that lost supply while its whole chain was live is a root
  // cause of its own; the rest is collateral captured by the affected scope.
  std::stable_sort(candidates.begin(), candidates.end(),
                   [&topology](const ClassifiedFailure& a, const ClassifiedFailure& b) {
                     const bool a_loss = model_energy_loss_reason(a.primary_reason);
                     const bool b_loss = model_energy_loss_reason(b.primary_reason);
                     if (a_loss != b_loss) {
                       return !a_loss;
                     }
                     if (a_loss) {
                       const std::size_t da = topology.chain(a.element).size();
                       const std::size_t db = topology.chain(b.element).size();
                       if (da != db) {
                         return da < db;
                       }
                     }
                     return model_failure_less(a, b);
                   });

  std::map<RefToken, FailureClass> failed_elements;
  for (const auto& candidate : candidates) {
    if (model_energy_loss_reason(candidate.primary_reason)) {
      bool failed_ancestor = false;
      for (const auto& ancestor : topology.ancestors(candidate.element)) {
        if (failed_elements.find(ancestor) != failed_elements.end()) {
          failed_ancestor = true;
          break;
        }
      }
      if (failed_ancestor) {
        continue;
      }
    }
    failed_elements[candidate.element] = candidate.klass;
    result.failures.push_back(candidate);
  }

  // A failure inside a shared failure domain is promoted to a typed failure of
  // the domain itself, once per domain.
  std::vector<ClassifiedFailure> domain_failures;
  for (const auto& failure : result.failures) {
    if (failure.element.kind() == RefKind::FailureDomain) {
      continue;
    }
    for (const auto& domain_ref : topology.domains_of(failure.element)) {
      if (!topology.domain_shared(domain_ref)) {
        continue;
      }
      ClassifiedFailure domain_failure;
      domain_failure.klass = FailureClass::SharedUpstreamDomainFailure;
      domain_failure.element = domain_ref;
      domain_failure.kind = ElementKind::Unknown;
      domain_failure.primary_reason = ReasonCode::DomainMemberFailed;
      domain_failure.evidence_current = failure.evidence_current;
      domain_failures.push_back(std::move(domain_failure));
    }
  }
  for (const auto& failure : domain_failures) {
    bool duplicate = false;
    for (const auto& existing : result.failures) {
      if (existing.klass == failure.klass && existing.element == failure.element) {
        duplicate = true;
        break;
      }
    }
    if (!duplicate) {
      result.failures.push_back(failure);
    }
  }

  std::sort(result.failures.begin(), result.failures.end(), model_failure_less);
  result.failed_elements = {};
  for (const auto& failure : result.failures) {
    result.failed_elements.push_back(failure.element);
  }
  result.failed_elements = model_sorted_unique(result.failed_elements);
  result.unresolved = model_sorted_unique(result.unresolved);
  result.resolved = model_sorted_unique(result.resolved);
  return result;
}

// --- affected scope --------------------------------------------------------

struct ModelScope {
  std::vector<RefToken> failed_elements{};
  std::vector<RefToken> impacted_elements{};
  std::vector<RefToken> load_groups{};
  std::vector<RefToken> domains{};
  std::vector<RefToken> isolation_points{};
  std::vector<RefToken> shared_domains{};
  std::vector<RefToken> unresolved_upstream{};
  std::vector<RefToken> unevidenced_elements{};
  bool shared_domain_impacted{false};
  bool upstream_evidence_unresolved{false};
};

inline ModelScope model_scope(const ModelTopology& topology,
                              const std::vector<ClassifiedFailure>& failures,
                              const std::vector<RefToken>& resolved,
                              const std::vector<RefToken>& unresolved) {
  ModelScope scope;
  const auto add_impact = [&topology, &scope](const RefToken& ref) {
    for (const auto& element : topology.closure(ref)) {
      scope.impacted_elements.push_back(element);
    }
    for (const auto& load : topology.load_groups_under(ref)) {
      scope.load_groups.push_back(load);
    }
  };

  for (const auto& failure : failures) {
    const RefToken& ref = failure.element;
    if (ref.kind() == RefKind::FailureDomain) {
      scope.failed_elements.push_back(ref);
      const FailureDomain* domain = topology.domain(ref);
      if (domain != nullptr) {
        for (const auto& member : domain->members) {
          scope.impacted_elements.push_back(member);
          add_impact(member);
        }
      }
      scope.domains.push_back(ref);
      if (topology.domain_shared(ref)) {
        scope.shared_domains.push_back(ref);
        scope.shared_domain_impacted = true;
      }
      continue;
    }
    scope.failed_elements.push_back(ref);
    add_impact(ref);
    for (const auto& domain : topology.domains_of(ref)) {
      scope.domains.push_back(domain);
      if (topology.domain_shared(domain)) {
        scope.shared_domains.push_back(domain);
        scope.shared_domain_impacted = true;
      }
    }
    scope.impacted_elements.push_back(ref);
    RefToken point;
    if (topology.isolation_point_for(ref, point)) {
      scope.isolation_points.push_back(point);
    }
    for (const auto& ancestor : topology.ancestors(ref)) {
      RefToken ancestor_point;
      if (topology.isolation_point_for(ancestor, ancestor_point)) {
        scope.isolation_points.push_back(ancestor_point);
      }
    }
  }

  for (const auto& element : unresolved) {
    scope.impacted_elements.push_back(element);
    add_impact(element);
    RefToken point;
    if (topology.isolation_point_for(element, point)) {
      scope.isolation_points.push_back(point);
    }
  }

  scope.failed_elements = model_sorted_unique(scope.failed_elements);
  scope.impacted_elements = model_sorted_unique(scope.impacted_elements);
  scope.load_groups = model_sorted_unique(scope.load_groups);
  scope.domains = model_sorted_unique(scope.domains);
  scope.isolation_points = model_sorted_unique(scope.isolation_points);
  scope.shared_domains = model_sorted_unique(scope.shared_domains);

  std::vector<RefToken> evidence_roots = scope.failed_elements;
  for (const auto& element : scope.failed_elements) {
    for (const auto& ancestor : topology.chain(element)) {
      evidence_roots.push_back(ancestor);
    }
  }
  evidence_roots = model_sorted_unique(evidence_roots);
  for (const auto& root : evidence_roots) {
    if (root.kind() == RefKind::FailureDomain) {
      continue;
    }
    if (model_has_ref(resolved, root)) {
      continue;
    }
    scope.unresolved_upstream.push_back(root);
  }
  scope.upstream_evidence_unresolved = !scope.unresolved_upstream.empty();

  for (const auto& element : scope.impacted_elements) {
    if (!model_ref_is_monitored(element.kind())) {
      continue;
    }
    if (model_has_ref(resolved, element)) {
      continue;
    }
    scope.unevidenced_elements.push_back(element);
  }
  scope.unevidenced_elements = model_sorted_unique(scope.unevidenced_elements);
  return scope;
}

// --- recovery --------------------------------------------------------------

// Whether a current observation reports the element as healthy for its kind.
inline bool model_element_healthy(const ElectricalObservation& observation, ElementKind kind,
                                  const ElectricalPolicy& policy, ReasonCode& reason) {
  if (observation.quality == EvidenceQuality::Bad) {
    reason = ReasonCode::EvidenceQualityBad;
    return false;
  }
  switch (kind) {
    case ElementKind::UtilityFeed: {
      if (observation.energization != EnergizationState::Energized) {
        reason = ReasonCode::FeedDeEnergized;
        return false;
      }
      if (observation.has_voltage && !model_voltage_in_band(policy, observation.voltage)) {
        reason = ReasonCode::FeedVoltageOutOfBand;
        return false;
      }
      if (observation.has_frequency && !model_frequency_in_band(policy, observation.frequency)) {
        reason = ReasonCode::FeedFrequencyOutOfBand;
        return false;
      }
      return true;
    }
    case ElementKind::Switchgear:
    case ElementKind::Bus: {
      if (observation.breaker == BreakerPosition::Tripped) {
        reason = ReasonCode::BreakerTripped;
        return false;
      }
      if (observation.energization == EnergizationState::DeEnergized) {
        reason = ReasonCode::ElementDeEnergizedWhileUpstreamEnergized;
        return false;
      }
      if (observation.energization == EnergizationState::Unknown) {
        reason = ReasonCode::EvidenceMissing;
        return false;
      }
      return true;
    }
    case ElementKind::Breaker:
    case ElementKind::Circuit: {
      if (observation.breaker == BreakerPosition::Tripped) {
        reason = ReasonCode::BreakerTripped;
        return false;
      }
      if (observation.breaker == BreakerPosition::Unknown) {
        reason = ReasonCode::EvidenceMissing;
        return false;
      }
      return true;
    }
    case ElementKind::Pdu:
    case ElementKind::PduBranch:
    case ElementKind::UpsBus: {
      if (observation.energization == EnergizationState::DeEnergized) {
        reason = ReasonCode::ElementDeEnergizedWhileUpstreamEnergized;
        return false;
      }
      if (observation.energization == EnergizationState::Unknown) {
        reason = ReasonCode::EvidenceMissing;
        return false;
      }
      return true;
    }
    case ElementKind::UpsUnit: {
      if (observation.energization == EnergizationState::DeEnergized) {
        reason = ReasonCode::UpsFaulted;
        return false;
      }
      if (observation.energization == EnergizationState::Unknown) {
        reason = ReasonCode::EvidenceMissing;
        return false;
      }
      return true;
    }
    case ElementKind::Generator: {
      if (observation.generator == GeneratorState::Faulted) {
        reason = ReasonCode::GeneratorFaulted;
        return false;
      }
      if (observation.generator == GeneratorState::Unknown) {
        reason = ReasonCode::EvidenceMissing;
        return false;
      }
      if (observation.transfer == TransferState::Failed) {
        reason = ReasonCode::GeneratorTransferFailed;
        return false;
      }
      return true;
    }
    case ElementKind::AutomaticTransferSwitch:
    case ElementKind::StaticTransferSwitch: {
      if (observation.transfer == TransferState::Failed ||
          observation.transfer == TransferState::Transferring) {
        reason = ReasonCode::GeneratorTransferFailed;
        return false;
      }
      if (observation.transfer == TransferState::Unknown) {
        reason = ReasonCode::EvidenceMissing;
        return false;
      }
      return true;
    }
    case ElementKind::LoadGroup:
    case ElementKind::Rack:
    case ElementKind::Unknown:
      return true;
  }
  return true;
}

inline bool model_open_state(summon::pfm::RequestState state) {
  switch (state) {
    case summon::pfm::RequestState::Planned:
    case summon::pfm::RequestState::Issued:
    case summon::pfm::RequestState::Acknowledged:
    case summon::pfm::RequestState::Observed:
    case summon::pfm::RequestState::Indeterminate:
      return true;
    default:
      return false;
  }
}

inline const ElectricalObservation* model_find_observation(
    const std::vector<ElectricalObservation>& observations, const RefToken& element) {
  for (const auto& observation : observations) {
    if (observation.element == element) {
      return &observation;
    }
  }
  return nullptr;
}

inline bool model_request_verified(const std::vector<summon::pfm::ResponseRequest>& requests,
                                   const RefToken& element, summon::pfm::ProofKind proof) {
  for (const auto& request : requests) {
    if (request.target.element == element && request.state == summon::pfm::RequestState::Verified &&
        (proof == summon::pfm::ProofKind::None || request.required_proof == proof)) {
      return true;
    }
  }
  return false;
}

struct ModelRecoveryInputs {
  Timestamp now{};
  ElectricalPolicy policy{};
  const ResponsePlan* plan{nullptr};
  std::vector<RefToken> incident_scope{};
  std::vector<ElectricalObservation> observations{};
  std::vector<ProtectedObligation> obligations{};
  bool has_stable_since{false};
  Timestamp stable_since{};
  bool operator_authorized{false};
  IncidentGeneration authorized_generation{};
  IncidentGeneration incident_generation{};
};

struct ModelRecovery {
  bool eligible{false};
  std::map<RecoveryGate, bool> gates{};
  std::vector<RefToken> blocking{};

  [[nodiscard]] bool gate(RecoveryGate value) const {
    const auto found = gates.find(value);
    return found != gates.end() && found->second;
  }
};

// Every gate, evaluated directly. A gate is satisfied only by current evidence.
inline ModelRecovery model_recovery(const ModelRecoveryInputs& inputs) {
  ModelRecovery result;
  if (inputs.plan == nullptr) {
    return result;
  }
  const ResponsePlan& plan = *inputs.plan;

  bool fault_cleared = true;
  for (const auto& failure : plan.failures) {
    if (failure.element.kind() == RefKind::FailureDomain) {
      // A domain clears only when every impacted member reports.
      for (const auto& member : plan.scope.impacted_elements) {
        if (model_find_observation(inputs.observations, member) == nullptr) {
          fault_cleared = false;
          break;
        }
      }
      if (!fault_cleared) {
        break;
      }
      continue;
    }
    const ElectricalObservation* observation =
        model_find_observation(inputs.observations, failure.element);
    if (observation == nullptr) {
      fault_cleared = false;
      break;
    }
    ReasonCode reason = ReasonCode::None;
    if (!model_element_healthy(*observation, failure.kind, inputs.policy, reason)) {
      fault_cleared = false;
      break;
    }
  }
  // The cumulative incident scope is the recovery question, not only the newest
  // plan. A reference that is not an electrical element cannot report, so it is
  // not an evidence question.
  if (fault_cleared) {
    for (const auto& element : inputs.incident_scope) {
      if (!model_ref_is_monitored(element.kind())) {
        continue;
      }
      const ElectricalObservation* observation =
          model_find_observation(inputs.observations, element);
      if (observation == nullptr) {
        fault_cleared = false;
        break;
      }
      ReasonCode reason = ReasonCode::None;
      if (!model_element_healthy(*observation, observation->kind, inputs.policy, reason)) {
        fault_cleared = false;
        break;
      }
    }
  }
  result.gates[RecoveryGate::FaultCleared] = fault_cleared;

  bool isolation_verified = true;
  bool de_energization_verified = true;
  if (inputs.policy.require_verified_isolation_for_recovery) {
    for (const auto& requirement : plan.isolations) {
      if (!requirement.has_isolation_point) {
        // There is no isolation point to prove, so the isolation gate fails
        // while the de-energization question is simply not asked at a point
        // that does not exist. Recovery is blocked either way.
        isolation_verified = false;
        continue;
      }
      if (!model_request_verified(plan.requests, requirement.element,
                                  summon::pfm::ProofKind::Isolation) &&
          !model_request_verified(plan.requests, requirement.isolation_point,
                                  summon::pfm::ProofKind::Isolation)) {
        isolation_verified = false;
      }
      if (!model_request_verified(plan.requests, requirement.element,
                                  summon::pfm::ProofKind::DeEnergization) &&
          !model_request_verified(plan.requests, requirement.isolation_point,
                                  summon::pfm::ProofKind::DeEnergization)) {
        de_energization_verified = false;
      }
    }
  }
  result.gates[RecoveryGate::IsolationVerified] = isolation_verified;
  result.gates[RecoveryGate::DeEnergizationVerified] = de_energization_verified;

  bool breaker_proven = true;
  for (const auto& failure : plan.failures) {
    if (failure.klass != FailureClass::BreakerFailure &&
        failure.klass != FailureClass::CircuitFailure &&
        failure.klass != FailureClass::SwitchgearFailure &&
        failure.klass != FailureClass::BusFailure) {
      continue;
    }
    const ElectricalObservation* observation =
        model_find_observation(inputs.observations, failure.element);
    if (observation == nullptr || observation->breaker == BreakerPosition::Unknown ||
        observation->breaker == BreakerPosition::Tripped) {
      breaker_proven = false;
      break;
    }
  }
  result.gates[RecoveryGate::BreakerStateProven] = breaker_proven;

  bool transfer_stable = true;
  bool generation_stable = true;
  for (const auto& element : plan.scope.impacted_elements) {
    const ElectricalObservation* observation = model_find_observation(inputs.observations, element);
    if (observation == nullptr) {
      continue;
    }
    if (observation->kind == ElementKind::UpsUnit || observation->kind == ElementKind::UpsBus ||
        observation->kind == ElementKind::StaticTransferSwitch ||
        observation->kind == ElementKind::AutomaticTransferSwitch) {
      if (observation->transfer == TransferState::Transferring ||
          observation->transfer == TransferState::Failed ||
          observation->transfer == TransferState::Unknown) {
        transfer_stable = false;
      }
    }
    if (observation->kind == ElementKind::Generator) {
      if (observation->generator != GeneratorState::Synchronized &&
          observation->generator != GeneratorState::Running) {
        generation_stable = false;
      }
      if (observation->transfer == TransferState::Failed) {
        generation_stable = false;
      }
    }
  }
  result.gates[RecoveryGate::TransferStable] = transfer_stable;
  result.gates[RecoveryGate::GenerationStable] = generation_stable;

  bool reserve_restored = true;
  if (inputs.policy.require_reserve_floor_for_recovery) {
    for (const auto& element : plan.scope.impacted_elements) {
      const ElectricalObservation* observation =
          model_find_observation(inputs.observations, element);
      if (observation == nullptr || (observation->kind != ElementKind::UpsUnit &&
                                     observation->kind != ElementKind::UpsBus)) {
        continue;
      }
      MilliPercent floor = inputs.policy.reserve_floor;
      for (const auto& obligation : inputs.obligations) {
        if (obligation.has_reserve_floor && obligation.target == element &&
            obligation.reserve_floor > floor) {
          floor = obligation.reserve_floor;
        }
      }
      if (!observation->has_reserve || observation->reserve < floor) {
        reserve_restored = false;
        break;
      }
    }
  }
  result.gates[RecoveryGate::ReserveRestored] = reserve_restored;

  bool shared_resolved = true;
  if (inputs.policy.require_shared_domain_resolution_for_recovery &&
      !plan.scope.shared_domains.empty()) {
    shared_resolved = false;
  }
  result.gates[RecoveryGate::SharedDomainResolved] = shared_resolved;

  result.gates[RecoveryGate::UpstreamEvidenceCurrent] = plan.scope.unresolved_upstream.empty();
  result.gates[RecoveryGate::EvidenceCurrent] = plan.scope.unevidenced_elements.empty();

  bool obligations_satisfied = true;
  for (const auto& obligation : inputs.obligations) {
    if (!obligation.has_report) {
      obligations_satisfied = false;
      break;
    }
    const Freshness freshness =
        model_freshness(obligation.reported_at, inputs.now, inputs.policy);
    if (freshness == Freshness::Expired || freshness == Freshness::Future) {
      obligations_satisfied = false;
      break;
    }
    if (obligation.status != summon::pfm::ObligationStatus::Satisfied) {
      obligations_satisfied = false;
      break;
    }
  }
  result.gates[RecoveryGate::ObligationsSatisfied] = obligations_satisfied;

  bool dwell = false;
  if (inputs.has_stable_since) {
    const std::int64_t elapsed = inputs.now.value() - inputs.stable_since.value();
    dwell = elapsed >= 0 && elapsed >= inputs.policy.stability_dwell.value();
  }
  result.gates[RecoveryGate::StabilityDwell] = dwell;

  bool authorized = true;
  if (inputs.policy.require_operator_authorization_for_recovery) {
    authorized = inputs.operator_authorized &&
                 inputs.authorized_generation == inputs.incident_generation;
  }
  result.gates[RecoveryGate::OperatorAuthorization] = authorized;

  bool settled = true;
  for (const auto& request : plan.requests) {
    if (model_open_state(request.state)) {
      settled = false;
      break;
    }
  }
  result.gates[RecoveryGate::RequestsSettled] = settled;

  result.eligible = true;
  for (const auto& entry : result.gates) {
    if (!entry.second) {
      result.eligible = false;
    }
  }
  return result;
}

}  // namespace pfmref
