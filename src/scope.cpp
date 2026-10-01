// Power Failure Manager -- affected scope and isolation boundary.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/scope.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace summon::pfm {
namespace {

void sort_unique(std::vector<RefToken>& values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

bool contains_ref(const std::vector<RefToken>& values, const RefToken& ref) {
  return std::binary_search(values.begin(), values.end(), ref);
}

}  // namespace

bool AffectedScope::contains(const RefToken& element) const noexcept {
  return std::binary_search(impacted_elements.begin(), impacted_elements.end(), element);
}

Result<AffectedScope> resolve_scope(const ClassificationResult& classification,
                                    const TopologyIndex& topology,
                                    const ElectricalPolicy& policy, const Bounds& bounds) {
  if (auto r = validate(policy); !r.ok()) {
    return r.status();
  }
  AffectedScope scope;
  scope.topology_generation = topology.generation();

  const auto add_impact = [&](const RefToken& ref) {
    for (const auto& element : topology.descendants(ref)) {
      scope.impacted_elements.push_back(element);
    }
    for (const auto& load : topology.load_groups_under(ref)) {
      scope.load_groups.push_back(load);
    }
  };

  for (const auto& failure : classification.failures) {
    if (failure.element.kind() == RefKind::FailureDomain) {
      const auto* domain = topology.domain(failure.element);
      scope.failed_elements.push_back(failure.element);
      if (domain != nullptr) {
        for (const auto& member : domain->members) {
          scope.impacted_elements.push_back(member);
          add_impact(member);
        }
      }
      scope.domains.push_back(failure.element);
      if (topology.domain_is_shared(failure.element)) {
        scope.shared_domains.push_back(failure.element);
        scope.shared_domain_impacted = true;
      }
      continue;
    }
    scope.failed_elements.push_back(failure.element);
    add_impact(failure.element);
    for (const auto& domain : topology.domains_of(failure.element)) {
      scope.domains.push_back(domain);
      if (topology.domain_is_shared(domain)) {
        scope.shared_domains.push_back(domain);
        scope.shared_domain_impacted = true;
      }
    }
    // A failed element is impacted by definition: it is inside the boundary
    // even when the topology does not describe it.
    scope.impacted_elements.push_back(failure.element);

    RefToken isolation_point;
    if (topology.isolation_point_for(failure.element, isolation_point)) {
      scope.isolation_points.push_back(isolation_point);
    }
    for (const auto& ancestor : topology.ancestors(failure.element)) {
      RefToken ancestor_point;
      if (topology.isolation_point_for(ancestor, ancestor_point)) {
        scope.isolation_points.push_back(ancestor_point);
      }
    }
  }

  // Elements marked unresolved by classification are inside the boundary and
  // count as unevidenced: absence of evidence is never read as health.
  for (const auto& element : classification.unresolved_elements) {
    scope.impacted_elements.push_back(element);
    add_impact(element);
    RefToken isolation_point;
    if (topology.isolation_point_for(element, isolation_point)) {
      scope.isolation_points.push_back(isolation_point);
    }
  }

  sort_unique(scope.failed_elements);
  sort_unique(scope.impacted_elements);
  sort_unique(scope.load_groups);
  sort_unique(scope.domains);
  sort_unique(scope.isolation_points);
  sort_unique(scope.shared_domains);

  if (scope.impacted_elements.size() > bounds.max_scope_elements) {
    return Status::error(StatusCode::BoundsExceeded,
                         "affected scope exceeds the configured element bound")
        .with_context("scope.impacted_elements");
  }

  // Upstream questions that remain open. A locally healthy element below an
  // unresolved upstream is not recoverable, so every ancestor of every failed
  // or unresolved element is checked for current evidence.
  std::vector<RefToken> evidence_roots = scope.failed_elements;
  for (const auto& element : scope.failed_elements) {
    for (const auto& ancestor : topology.ancestors(element)) {
      evidence_roots.push_back(ancestor);
    }
  }
  sort_unique(evidence_roots);
  for (const auto& root : evidence_roots) {
    if (root.kind() == RefKind::FailureDomain) {
      continue;
    }
    if (contains_ref(classification.resolved_elements, root)) {
      continue;
    }
    scope.unresolved_upstream.push_back(root);
  }
  scope.upstream_evidence_unresolved = !scope.unresolved_upstream.empty();

  for (const auto& element : scope.impacted_elements) {
    // Only elements that report electrical state can be unevidenced. A load
    // group is supplied by the branch that serves it and is not itself a
    // monitor, so its silence is not evidence of anything.
    if (!ref_kind_is_element(element.kind())) {
      continue;
    }
    if (contains_ref(classification.resolved_elements, element)) {
      continue;
    }
    scope.unevidenced_elements.push_back(element);
  }
  sort_unique(scope.unevidenced_elements);
  return scope;
}

}  // namespace summon::pfm
