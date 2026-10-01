// Power Failure Manager -- supplied electrical topology and failure domains.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/topology.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace summon::pfm {
namespace {

Status invalid(StatusCode code, std::string message, std::string context) {
  return Status::error(code, std::move(message)).with_context(std::move(context));
}

bool element_ref_less(const TopologyElement& a, const TopologyElement& b) noexcept {
  return a.element < b.element;
}

bool domain_ref_less(const FailureDomain& a, const FailureDomain& b) noexcept {
  return a.domain < b.domain;
}

bool is_switching_device(ElementKind kind) noexcept {
  switch (kind) {
    case ElementKind::Breaker:
    case ElementKind::Circuit:
    case ElementKind::Switchgear:
    case ElementKind::StaticTransferSwitch:
    case ElementKind::AutomaticTransferSwitch:
      return true;
    default:
      return false;
  }
}

void sort_unique(std::vector<RefToken>& values) {
  std::sort(values.begin(), values.end());
  values.erase(std::unique(values.begin(), values.end()), values.end());
}

}  // namespace

Result<void> validate(const TopologySnapshot& snapshot, const Bounds& bounds) {
  if (snapshot.generation.is_absent()) {
    return invalid(StatusCode::InvalidArgument, "topology snapshot has no generation",
                   "topology.generation");
  }
  if (snapshot.elements.size() > bounds.max_topology_elements) {
    return invalid(StatusCode::TopologyBoundsExceeded, "topology exceeds the element bound",
                   "topology.elements");
  }
  if (snapshot.domains.size() > bounds.max_failure_domains) {
    return invalid(StatusCode::TopologyBoundsExceeded, "topology exceeds the failure-domain bound",
                   "topology.domains");
  }

  std::vector<RefToken> refs;
  refs.reserve(snapshot.elements.size());
  for (const auto& element : snapshot.elements) {
    if (!element.element.is_set()) {
      return invalid(StatusCode::InvalidArgument, "topology element has no reference",
                     "topology.element");
    }
    if (element.kind == ElementKind::Unknown) {
      return invalid(StatusCode::InvalidArgument, "topology element has no kind",
                     element.element.to_string());
    }
    if (ref_kind_for_element(element.kind) != element.element.kind()) {
      return invalid(StatusCode::InvalidArgument,
                     "topology element kind does not match its reference kind",
                     element.element.to_string());
    }
    if (element.label.size() > bounds.max_label_length) {
      return invalid(StatusCode::FieldTooLong, "topology element label exceeds the bound",
                     element.element.to_string());
    }
    if (element.load_group.is_set() && element.load_group.kind() != RefKind::LoadGroup) {
      return invalid(StatusCode::InvalidArgument,
                     "topology element load group is not a load-group reference",
                     element.element.to_string());
    }
    refs.push_back(element.element);
  }
  std::sort(refs.begin(), refs.end());
  if (std::adjacent_find(refs.begin(), refs.end()) != refs.end()) {
    return invalid(StatusCode::DuplicateElement, "topology contains a duplicate element",
                   "topology.elements");
  }
  const auto has_element = [&refs](const RefToken& ref) {
    return std::binary_search(refs.begin(), refs.end(), ref);
  };
  const auto kind_of = [&snapshot](const RefToken& ref) {
    for (const auto& element : snapshot.elements) {
      if (element.element == ref) {
        return element.kind;
      }
    }
    return ElementKind::Unknown;
  };

  std::vector<RefToken> domain_refs;
  domain_refs.reserve(snapshot.domains.size());
  for (const auto& domain : snapshot.domains) {
    if (domain.domain.kind() != RefKind::FailureDomain) {
      return invalid(StatusCode::InvalidArgument,
                     "failure domain reference is not a failure-domain reference",
                     domain.domain.to_string());
    }
    if (domain.members.size() > bounds.max_topology_elements) {
      return invalid(StatusCode::TopologyBoundsExceeded, "failure domain exceeds the member bound",
                     domain.domain.to_string());
    }
    for (const auto& member : domain.members) {
      if (!has_element(member)) {
        return invalid(StatusCode::TopologyUnknownElement,
                       "failure domain names an element the topology does not contain",
                       member.to_string());
      }
    }
    domain_refs.push_back(domain.domain);
  }
  std::sort(domain_refs.begin(), domain_refs.end());
  if (std::adjacent_find(domain_refs.begin(), domain_refs.end()) != domain_refs.end()) {
    return invalid(StatusCode::DuplicateElement, "topology contains a duplicate failure domain",
                   "topology.domains");
  }

  for (const auto& element : snapshot.elements) {
    if (element.upstream.is_set()) {
      if (element.upstream == element.element) {
        return invalid(StatusCode::TopologyCycle, "topology element supplies itself",
                       element.element.to_string());
      }
      if (!has_element(element.upstream)) {
        return invalid(StatusCode::TopologyUnknownElement,
                       "topology names an upstream element the snapshot does not contain",
                       element.element.to_string());
      }
    }
    if (element.domain.is_set() &&
        !std::binary_search(domain_refs.begin(), domain_refs.end(), element.domain) &&
        element.domain.kind() != RefKind::PowerDomain) {
      return invalid(StatusCode::FailureDomainUnknown,
                     "topology element names an unknown domain", element.element.to_string());
    }
    if (element.has_isolation_point) {
      if (!element.isolation_point.is_set()) {
        return invalid(StatusCode::InvalidArgument,
                       "isolation point flag is set without a reference",
                       element.element.to_string());
      }
      if (!has_element(element.isolation_point)) {
        return invalid(StatusCode::TopologyUnknownElement,
                       "isolation point is not part of the supplied topology",
                       element.isolation_point.to_string());
      }
      if (!is_switching_device(kind_of(element.isolation_point))) {
        return invalid(StatusCode::InvalidArgument,
                       "isolation point is not a switching device",
                       element.isolation_point.to_string());
      }
    }
  }

  // Cycle detection over the upstream chain. Path length is bounded by the
  // element count, so a pathological graph is refused rather than traversed
  // forever.
  for (const auto& start : snapshot.elements) {
    RefToken current = start.upstream;
    std::size_t steps = 0;
    while (current.is_set()) {
      if (current == start.element) {
        return invalid(StatusCode::TopologyCycle, "topology contains an upstream cycle",
                       start.element.to_string());
      }
      if (++steps > snapshot.elements.size()) {
        return invalid(StatusCode::TopologyCycle, "topology upstream chain is unbounded",
                       start.element.to_string());
      }
      const auto* next = [&snapshot, &current]() -> const TopologyElement* {
        for (const auto& element : snapshot.elements) {
          if (element.element == current) {
            return &element;
          }
        }
        return nullptr;
      }();
      if (next == nullptr) {
        break;
      }
      current = next->upstream;
    }
  }
  return {};
}

void canonicalise(TopologySnapshot& snapshot) {
  std::sort(snapshot.elements.begin(), snapshot.elements.end(), element_ref_less);
  for (auto& domain : snapshot.domains) {
    sort_unique(domain.members);
  }
  std::sort(snapshot.domains.begin(), snapshot.domains.end(), domain_ref_less);
}

Result<TopologyIndex> TopologyIndex::build(const TopologySnapshot& snapshot, const Bounds& bounds) {
  if (auto r = validate(snapshot, bounds); !r.ok()) {
    return r.status();
  }
  TopologySnapshot canonical = snapshot;
  canonicalise(canonical);

  TopologyIndex index;
  index.generation_ = canonical.generation;
  index.elements_ = std::move(canonical.elements);
  index.domains_ = std::move(canonical.domains);
  index.children_.assign(index.elements_.size(), {});
  for (std::size_t i = 0; i < index.elements_.size(); ++i) {
    const auto& upstream = index.elements_[i].upstream;
    if (!upstream.is_set()) {
      continue;
    }
    const auto parent = index.index_of(upstream);
    if (parent != index.elements_.size()) {
      index.children_[parent].push_back(i);
    }
  }
  return index;
}

std::size_t TopologyIndex::index_of(const RefToken& element) const noexcept {
  std::size_t low = 0;
  std::size_t high = elements_.size();
  while (low < high) {
    const std::size_t mid = low + (high - low) / 2;
    if (elements_[mid].element < element) {
      low = mid + 1;
    } else if (element < elements_[mid].element) {
      high = mid;
    } else {
      return mid;
    }
  }
  return elements_.size();
}

const TopologyElement* TopologyIndex::find(const RefToken& element) const noexcept {
  const auto position = index_of(element);
  if (position == elements_.size()) {
    return nullptr;
  }
  return &elements_[position];
}

std::vector<RefToken> TopologyIndex::ancestors(const RefToken& element) const {
  std::vector<RefToken> result;
  std::size_t position = index_of(element);
  if (position == elements_.size()) {
    return result;
  }
  std::size_t guard = 0;
  while (position != elements_.size() && guard++ <= elements_.size()) {
    result.push_back(elements_[position].element);
    const auto& upstream = elements_[position].upstream;
    if (!upstream.is_set()) {
      break;
    }
    position = index_of(upstream);
  }
  return result;
}

std::vector<RefToken> TopologyIndex::descendants(const RefToken& element) const {
  std::vector<RefToken> result;
  const std::size_t root = index_of(element);
  if (root == elements_.size()) {
    return result;
  }
  std::vector<std::size_t> stack{root};
  std::vector<bool> seen(elements_.size(), false);
  seen[root] = true;
  while (!stack.empty()) {
    const std::size_t current = stack.back();
    stack.pop_back();
    result.push_back(elements_[current].element);
    for (const auto child : children_[current]) {
      if (!seen[child]) {
        seen[child] = true;
        stack.push_back(child);
      }
    }
  }
  sort_unique(result);
  return result;
}

std::vector<RefToken> TopologyIndex::load_groups_under(const RefToken& element) const {
  std::vector<RefToken> result;
  for (const auto& ref : descendants(element)) {
    const auto* found = find(ref);
    if (found == nullptr) {
      continue;
    }
    if (found->load_group.is_set()) {
      result.push_back(found->load_group);
    }
    if (found->element.kind() == RefKind::LoadGroup) {
      result.push_back(found->element);
    }
  }
  sort_unique(result);
  return result;
}

std::vector<RefToken> TopologyIndex::domains_of(const RefToken& element) const {
  std::vector<RefToken> result;
  const auto* found = find(element);
  if (found == nullptr) {
    return result;
  }
  if (found->domain.is_set()) {
    result.push_back(found->domain);
  }
  for (const auto& domain : domains_) {
    if (std::binary_search(domain.members.begin(), domain.members.end(), element)) {
      result.push_back(domain.domain);
    }
  }
  sort_unique(result);
  return result;
}

std::vector<RefToken> TopologyIndex::domains_of(const std::vector<RefToken>& elements) const {
  std::vector<RefToken> result;
  for (const auto& element : elements) {
    for (const auto& domain : domains_of(element)) {
      result.push_back(domain);
    }
  }
  sort_unique(result);
  return result;
}

const FailureDomain* TopologyIndex::domain(const RefToken& domain_ref) const noexcept {
  std::size_t low = 0;
  std::size_t high = domains_.size();
  while (low < high) {
    const std::size_t mid = low + (high - low) / 2;
    if (domains_[mid].domain < domain_ref) {
      low = mid + 1;
    } else if (domain_ref < domains_[mid].domain) {
      high = mid;
    } else {
      return &domains_[mid];
    }
  }
  return nullptr;
}

bool TopologyIndex::domain_is_shared(const RefToken& domain_ref) const {
  const auto* found = domain(domain_ref);
  if (found == nullptr) {
    return false;
  }
  if (found->shared) {
    return true;
  }
  // A domain that carries more than one distinct downstream power domain is
  // shared whether or not the supplying authority flagged it.
  std::vector<RefToken> downstream;
  for (const auto& member : found->members) {
    const auto* element = find(member);
    if (element != nullptr && element->domain.is_set() &&
        element->domain.kind() == RefKind::PowerDomain) {
      downstream.push_back(element->domain);
    }
  }
  std::sort(downstream.begin(), downstream.end());
  downstream.erase(std::unique(downstream.begin(), downstream.end()), downstream.end());
  return downstream.size() > 1;
}

std::vector<RefToken> TopologyIndex::isolated_by(const RefToken& isolation_point) const {
  std::vector<RefToken> result;
  for (const auto& element : elements_) {
    if (element.has_isolation_point && element.isolation_point == isolation_point) {
      result.push_back(element.element);
    }
  }
  return result;
}

bool TopologyIndex::isolation_point_for(const RefToken& element, RefToken& out) const {
  for (const auto& ref : ancestors(element)) {
    const auto* found = find(ref);
    if (found != nullptr && found->has_isolation_point) {
      out = found->isolation_point;
      return true;
    }
  }
  return false;
}

}  // namespace summon::pfm
