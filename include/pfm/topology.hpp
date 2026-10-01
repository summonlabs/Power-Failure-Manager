// Power Failure Manager -- supplied electrical topology and failure domains.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "pfm/evidence.hpp"
#include "pfm/ids.hpp"
#include "pfm/policy.hpp"
#include "pfm/refs.hpp"

namespace summon::pfm {

// One electrical element as described by the authority that owns topology.
// PFM never discovers, invents, or repairs topology: it consumes a validated
// generation-stamped snapshot and nothing else.
struct TopologyElement {
  RefToken element{};
  ElementKind kind{ElementKind::Unknown};
  // Immediate upstream supply. Unset for a utility feed or a site entry point.
  RefToken upstream{};
  // Power or failure domain this element belongs to.
  RefToken domain{};
  // Load group served, for a branch or a rack.
  RefToken load_group{};
  // Isolation point that can de-energize this element, when one exists.
  RefToken isolation_point{};
  bool has_isolation_point{false};
  // True when the element can hold or transfer supply (UPS, ATS, STS).
  bool transfer_capable{false};
  std::string label{};
};

// A failure domain groups elements that fail together. A domain is shared when
// it carries more than one independent downstream domain: a locally healthy
// element inside a shared domain is not recoverable while that domain is
// unresolved.
struct FailureDomain {
  RefToken domain{};
  std::vector<RefToken> members{};
  bool shared{false};
};

struct TopologySnapshot {
  TopologyGeneration generation{};
  std::vector<TopologyElement> elements{};
  std::vector<FailureDomain> domains{};
};

// Structural validation: bounded size, canonical ordering, unique identities,
// known kinds, resolvable upstream and domain references, no cycles, and at
// most one upstream per element. A snapshot that fails validation is refused
// whole; it is never partially applied.
[[nodiscard]] Result<void> validate(const TopologySnapshot& snapshot, const Bounds& bounds);

// Canonical ordering of elements and domains so that every derived list is
// independent of insertion order.
void canonicalise(TopologySnapshot& snapshot);

// An immutable, queryable index built from a validated snapshot. Lookups are
// ordered, deterministic, and bounded; there is no hash-iteration-order
// dependence anywhere in the derived results.
class TopologyIndex {
 public:
  TopologyIndex() = default;

  [[nodiscard]] static Result<TopologyIndex> build(const TopologySnapshot& snapshot,
                                                   const Bounds& bounds);

  [[nodiscard]] bool empty() const noexcept { return elements_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return elements_.size(); }
  [[nodiscard]] TopologyGeneration generation() const noexcept { return generation_; }

  [[nodiscard]] const TopologyElement* find(const RefToken& element) const noexcept;

  // The element and every upstream supply reachable from it, nearest first.
  [[nodiscard]] std::vector<RefToken> ancestors(const RefToken& element) const;
  // The element and every element supplied through it, sorted by reference.
  [[nodiscard]] std::vector<RefToken> descendants(const RefToken& element) const;
  // Load groups served by the element or anything below it, sorted.
  [[nodiscard]] std::vector<RefToken> load_groups_under(const RefToken& element) const;
  // Distinct power and failure domains the element participates in, sorted.
  [[nodiscard]] std::vector<RefToken> domains_of(const RefToken& element) const;
  // Distinct failure domains touched by any of the supplied elements, sorted.
  [[nodiscard]] std::vector<RefToken> domains_of(const std::vector<RefToken>& elements) const;
  [[nodiscard]] const FailureDomain* domain(const RefToken& domain) const noexcept;
  // A shared failure domain is one flagged shared, or one that contains
  // elements belonging to more than one distinct downstream domain.
  [[nodiscard]] bool domain_is_shared(const RefToken& domain) const;
  // Elements whose isolation point is the supplied reference, sorted.
  [[nodiscard]] std::vector<RefToken> isolated_by(const RefToken& isolation_point) const;
  // The isolation point for an element: its own, or the nearest upstream one.
  [[nodiscard]] bool isolation_point_for(const RefToken& element, RefToken& out) const;

 private:
  [[nodiscard]] std::size_t index_of(const RefToken& element) const noexcept;

  TopologyGeneration generation_{};
  std::vector<TopologyElement> elements_{};
  std::vector<FailureDomain> domains_{};
  // Child adjacency is derived once at build time and stored in the same
  // canonical element order.
  std::vector<std::vector<std::size_t>> children_{};
};

}  // namespace summon::pfm
