// Power Failure Manager -- affected scope and isolation boundary.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <vector>

#include "pfm/classification.hpp"
#include "pfm/topology.hpp"

namespace summon::pfm {

// The resolved consequence of the classified failures: what is affected, what
// is not proven unaffected, which isolation points matter, and which upstream
// or shared-domain questions remain open.
//
// Every list is canonically ordered by reference, so the scope is independent
// of iteration order and of the order in which failures were classified.
struct AffectedScope {
  TopologyGeneration topology_generation{};
  std::vector<RefToken> failed_elements{};
  std::vector<RefToken> impacted_elements{};
  std::vector<RefToken> load_groups{};
  std::vector<RefToken> domains{};
  std::vector<RefToken> isolation_points{};
  std::vector<RefToken> shared_domains{};
  // Upstream elements whose current state could not be established. Recovery
  // is blocked while this list is non-empty and the policy requires it.
  std::vector<RefToken> unresolved_upstream{};
  // Elements inside the scope for which no current evidence exists at all.
  std::vector<RefToken> unevidenced_elements{};
  bool shared_domain_impacted{false};
  bool upstream_evidence_unresolved{false};

  [[nodiscard]] bool contains(const RefToken& element) const noexcept;
};

// Resolves the scope of a classification over a validated topology. Elements
// that could not be classified because their evidence was unusable are treated
// as affected and unevidenced: absence of evidence is never read as health.
[[nodiscard]] Result<AffectedScope> resolve_scope(const ClassificationResult& classification,
                                                  const TopologyIndex& topology,
                                                  const ElectricalPolicy& policy,
                                                  const Bounds& bounds);

}  // namespace summon::pfm
