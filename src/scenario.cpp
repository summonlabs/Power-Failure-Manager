// Power Failure Manager -- synthetic electrical plant and scenarios.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#include "pfm/scenario.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "pfm/codec.hpp"
#include "pfm/report.hpp"

namespace summon::pfm {
namespace {

std::string index_name(const char* prefix, std::vector<std::uint32_t> parts) {
  std::string name{prefix};
  for (const auto part : parts) {
    name.push_back('-');
    name.append(std::to_string(part));
  }
  return name;
}

std::string make_ref_name(const char* prefix, std::vector<std::uint32_t> parts) {
  return index_name(prefix, std::move(parts));
}

}  // namespace

SyntheticPlant::SyntheticPlant(SyntheticPlantConfig config) : config_(std::move(config)) {
  build_topology();
  for (const auto& element : topology_.elements) {
    ElementState state;
    state.voltage = config_.nominal_voltage;
    state.frequency = config_.nominal_frequency;
    if (element.kind == ElementKind::UpsUnit) {
      state.reserve = milli_percent_from_value(80000);
      state.has_reserve = true;
    }
    if (element.kind == ElementKind::Generator) {
      state.generator = GeneratorState::Off;
      state.transfer = TransferState::OnUtility;
    }
    if (element.kind == ElementKind::PduBranch || element.kind == ElementKind::Rack ||
        element.kind == ElementKind::LoadGroup) {
      state.current = MilliAmps::from_value(10000);
      state.has_current = true;
    }
    states_.emplace_back(element.element, state);
  }
  recompute_energization();
}

void SyntheticPlant::build_topology() {
  topology_ = TopologySnapshot{};
  topology_.generation = TopologyGeneration::from_value(config_.topology_generation);

  auto add = [this](RefKind kind, const std::string& name, ElementKind element_kind,
                    const std::string& upstream, const std::string& domain,
                    const std::string& isolation_point) {
    TopologyElement element;
    auto ref = RefToken::make(kind, name);
    if (!ref.ok()) {
      return;
    }
    element.element = ref.value();
    element.kind = element_kind;
    if (!upstream.empty()) {
      auto parent = RefToken::parse(upstream);
      if (parent.ok()) {
        element.upstream = parent.value();
      }
    }
    if (!domain.empty()) {
      auto domain_ref = RefToken::parse(domain);
      if (domain_ref.ok()) {
        element.domain = domain_ref.value();
      }
    }
    if (!isolation_point.empty()) {
      auto point = RefToken::parse(isolation_point);
      if (point.ok()) {
        element.isolation_point = point.value();
        element.has_isolation_point = true;
      }
    }
    element.transfer_capable = element_kind == ElementKind::UpsUnit ||
                               element_kind == ElementKind::StaticTransferSwitch ||
                               element_kind == ElementKind::AutomaticTransferSwitch;
    element.label = name;
    topology_.elements.push_back(std::move(element));
  };

  std::vector<RefToken> site_members;
  for (std::uint32_t i = 0; i < std::max<std::uint32_t>(1, config_.switchgear_fanout); ++i) {
    const std::string feed = "utility-feed:" + make_ref_name("feed", {i});
    const std::string brk_feed = "breaker:" + make_ref_name("brk-feed", {i});
    const std::string sg = "switchgear:" + make_ref_name("sg", {i});
    const std::string gen = "generator:" + make_ref_name("gen", {i});
    const std::string pd = "power-domain:" + make_ref_name("pd", {i});
    const std::string fd = "failure-domain:" + make_ref_name("fd", {i});

    add(RefKind::UtilityFeed, make_ref_name("feed", {i}), ElementKind::UtilityFeed, "", pd, "");
    add(RefKind::Breaker, make_ref_name("brk-feed", {i}), ElementKind::Breaker, feed, "", "");
    add(RefKind::Switchgear, make_ref_name("sg", {i}), ElementKind::Switchgear, brk_feed, pd,
        brk_feed);
    add(RefKind::Generator, make_ref_name("gen", {i}), ElementKind::Generator, sg, pd, "");

    FailureDomain domain;
    auto domain_ref = RefToken::parse(fd);
    if (domain_ref.ok()) {
      domain.domain = domain_ref.value();
    }
    domain.shared = true;
    {
      auto ref = RefToken::parse(feed);
      if (ref.ok()) {
        domain.members.push_back(ref.value());
        site_members.push_back(ref.value());
      }
      auto sg_ref = RefToken::parse(sg);
      if (sg_ref.ok()) {
        domain.members.push_back(sg_ref.value());
        site_members.push_back(sg_ref.value());
      }
    }

    for (std::uint32_t j = 0; j < std::max<std::uint32_t>(1, config_.bus_fanout); ++j) {
      const std::string bus = "bus:" + make_ref_name("bus", {i, j});
      const std::string brk_bus = "breaker:" + make_ref_name("brk-bus", {i, j});
      const std::string ups = "ups:" + make_ref_name("ups", {i, j});
      const std::string pdu = "pdu:" + make_ref_name("pdu", {i, j});
      const std::string brk_pdu = "breaker:" + make_ref_name("brk-pdu", {i, j});

      add(RefKind::Bus, make_ref_name("bus", {i, j}), ElementKind::Bus, sg, pd, brk_bus);
      add(RefKind::Breaker, make_ref_name("brk-bus", {i, j}), ElementKind::Breaker, bus, "", "");
      add(RefKind::UpsUnit, make_ref_name("ups", {i, j}), ElementKind::UpsUnit, brk_bus, pd, "");
      add(RefKind::Pdu, make_ref_name("pdu", {i, j}), ElementKind::Pdu, ups, pd, brk_pdu);
      add(RefKind::Breaker, make_ref_name("brk-pdu", {i, j}), ElementKind::Breaker, ups, "", "");

      for (const auto& ref : {bus, brk_bus, ups, pdu, brk_pdu}) {
        auto parsed = RefToken::parse(ref);
        if (parsed.ok()) {
          domain.members.push_back(parsed.value());
        }
      }

      for (std::uint32_t k = 0; k < 2; ++k) {
        const std::string branch = "pdu-branch:" + make_ref_name("branch", {i, j, k});
        const std::string load = "load-group:" + make_ref_name("load", {i, j, k});
        add(RefKind::PduBranch, make_ref_name("branch", {i, j, k}), ElementKind::PduBranch, pdu, pd,
            "");
        add(RefKind::LoadGroup, make_ref_name("load", {i, j, k}), ElementKind::LoadGroup, branch,
            "", "");
        for (const auto& ref : {branch, load}) {
          auto parsed = RefToken::parse(ref);
          if (parsed.ok()) {
            domain.members.push_back(parsed.value());
          }
        }
        if (!topology_.elements.empty()) {
          topology_.elements.back().load_group = RefToken{};
        }
        // The branch names the load group it serves.
        for (auto& element : topology_.elements) {
          if (element.element.to_string() == branch) {
            auto parsed = RefToken::parse(load);
            if (parsed.ok()) {
              element.load_group = parsed.value();
            }
          }
        }
      }
    }
    topology_.domains.push_back(std::move(domain));
  }

  FailureDomain site;
  auto site_ref = RefToken::make(RefKind::FailureDomain, make_ref_name("fd", {999}));
  if (site_ref.ok()) {
    site.domain = site_ref.value();
  }
  site.members = site_members;
  std::sort(site.members.begin(), site.members.end());
  site.shared = true;
  topology_.domains.push_back(std::move(site));
  canonicalise(topology_);
}

void SyntheticPlant::recompute_energization() {
  for (auto& [element, state] : states_) {
    const auto* topology_element = [this, &element]() -> const TopologyElement* {
      for (const auto& candidate : topology_.elements) {
        if (candidate.element == element) {
          return &candidate;
        }
      }
      return nullptr;
    }();
    if (topology_element == nullptr) {
      continue;
    }
    const auto upstream_state = [this, &topology_element]() {
      if (!topology_element->upstream.is_set()) {
        return EnergizationState::Unknown;
      }
      for (const auto& [ref, candidate] : states_) {
        if (ref == topology_element->upstream) {
          return candidate.energization;
        }
      }
      return EnergizationState::Unknown;
    }();
    const bool upstream_energized = upstream_state == EnergizationState::Energized;

    switch (topology_element->kind) {
      case ElementKind::UtilityFeed:
        break;  // supplied explicitly by the operator or the fault injector
      case ElementKind::Breaker:
        state.energization = upstream_energized && state.breaker == BreakerPosition::Closed
                                 ? EnergizationState::Energized
                                 : EnergizationState::DeEnergized;
        break;
      case ElementKind::UpsUnit:
        if (upstream_energized) {
          state.energization = EnergizationState::Energized;
        } else if (state.reserve.value() > 0) {
          state.energization = EnergizationState::PartiallyEnergized;
        } else {
          state.energization = EnergizationState::DeEnergized;
        }
        break;
      case ElementKind::Generator:
        state.energization = (state.generator == GeneratorState::Running ||
                              state.generator == GeneratorState::Synchronized)
                                 ? EnergizationState::Energized
                                 : EnergizationState::DeEnergized;
        break;
      case ElementKind::Switchgear:
      case ElementKind::Bus:
      case ElementKind::Pdu:
      case ElementKind::PduBranch:
        state.energization =
            upstream_energized ? EnergizationState::Energized : EnergizationState::DeEnergized;
        break;
      case ElementKind::LoadGroup:
      case ElementKind::Rack:
        state.energization =
            upstream_energized ? EnergizationState::Energized : EnergizationState::DeEnergized;
        if (!upstream_energized) {
          state.current = MilliAmps::from_value(0);
        }
        break;
      case ElementKind::Unknown:
      case ElementKind::StaticTransferSwitch:
      case ElementKind::AutomaticTransferSwitch:
        break;
    }
  }
}

SyntheticPlant::ElementState* SyntheticPlant::state_for(const RefToken& element) {
  for (auto& [ref, state] : states_) {
    if (ref == element) {
      return &state;
    }
  }
  return nullptr;
}

const SyntheticPlant::ElementState* SyntheticPlant::state_for(const RefToken& element) const {
  for (const auto& [ref, state] : states_) {
    if (ref == element) {
      return &state;
    }
  }
  return nullptr;
}

Result<RefToken> SyntheticPlant::element_ref(RefKind kind, std::string_view name) const {
  for (const auto& element : topology_.elements) {
    if (element.element.kind() == kind && element.element.name() == name) {
      return element.element;
    }
  }
  return Status::error(StatusCode::TopologyUnknownElement,
                       "the synthetic plant has no such element")
      .with_context(std::string{name});
}

std::vector<RefToken> SyntheticPlant::elements_of_kind(ElementKind kind) const {
  std::vector<RefToken> result;
  for (const auto& element : topology_.elements) {
    if (element.kind == kind) {
      result.push_back(element.element);
    }
  }
  return result;
}

void SyntheticPlant::lose_utility(const RefToken& feed) {
  auto* state = state_for(feed);
  if (state != nullptr) {
    state->energization = EnergizationState::DeEnergized;
    state->voltage = MilliVolts::from_value(0);
    state->frequency = MilliHertz::from_value(0);
  }
  recompute_energization();
}

void SyntheticPlant::restore_utility(const RefToken& feed) {
  auto* state = state_for(feed);
  if (state != nullptr) {
    state->energization = EnergizationState::Energized;
    state->voltage = config_.nominal_voltage;
    state->frequency = config_.nominal_frequency;
  }
  recompute_energization();
}

void SyntheticPlant::degrade_utility(const RefToken& feed, MilliVolts voltage,
                                     MilliHertz frequency) {
  auto* state = state_for(feed);
  if (state != nullptr) {
    state->energization = EnergizationState::Energized;
    state->voltage = voltage;
    state->frequency = frequency;
  }
  recompute_energization();
}

void SyntheticPlant::trip_breaker(const RefToken& breaker) {
  auto* state = state_for(breaker);
  if (state != nullptr) {
    state->breaker = BreakerPosition::Tripped;
  }
  recompute_energization();
}

void SyntheticPlant::close_breaker(const RefToken& breaker) {
  auto* state = state_for(breaker);
  if (state != nullptr) {
    state->breaker = BreakerPosition::Closed;
  }
  recompute_energization();
}

void SyntheticPlant::fail_ups(const RefToken& ups) {
  auto* state = state_for(ups);
  if (state != nullptr) {
    state->energization = EnergizationState::DeEnergized;
    state->reserve = milli_percent_from_value(0);
  }
  recompute_energization();
}

void SyntheticPlant::repair_ups(const RefToken& ups) {
  auto* state = state_for(ups);
  if (state != nullptr) {
    state->reserve = milli_percent_from_value(80000);
  }
  recompute_energization();
}

void SyntheticPlant::set_reserve(const RefToken& ups, MilliPercent reserve) {
  auto* state = state_for(ups);
  if (state != nullptr) {
    state->reserve = reserve;
  }
  recompute_energization();
}

void SyntheticPlant::fail_generator(const RefToken& generator) {
  auto* state = state_for(generator);
  if (state != nullptr) {
    state->generator = GeneratorState::Faulted;
  }
  recompute_energization();
}

void SyntheticPlant::repair_generator(const RefToken& generator) {
  auto* state = state_for(generator);
  if (state != nullptr) {
    state->generator = GeneratorState::Off;
  }
  recompute_energization();
}

void SyntheticPlant::set_generator_starts(bool succeed) { generator_starts_ = succeed; }

void SyntheticPlant::set_generator_synchronizes(bool succeed) {
  generator_synchronizes_ = succeed;
}

void SyntheticPlant::set_transfers(bool succeed) { transfers_ = succeed; }

void SyntheticPlant::set_evidence_suppressed(const RefToken& element, bool suppressed) {
  auto* state = state_for(element);
  if (state != nullptr) {
    state->suppressed = suppressed;
  }
}

void SyntheticPlant::set_quality(const RefToken& element, EvidenceQuality quality) {
  auto* state = state_for(element);
  if (state != nullptr) {
    state->quality = quality;
  }
}

std::vector<ElectricalObservation> SyntheticPlant::observe_stale(Timestamp now,
                                                                 Duration age) const {
  auto observations = observe(now);
  for (auto& observation : observations) {
    observation.observed_at = Timestamp::from_value(now.value() - age.value());
  }
  return observations;
}

std::vector<ElectricalObservation> SyntheticPlant::observe(Timestamp now) const {
  std::vector<ElectricalObservation> result;
  for (const auto& element : topology_.elements) {
    if (element.kind == ElementKind::LoadGroup || element.kind == ElementKind::Rack) {
      continue;  // load groups are reported through the branch that serves them
    }
    result.push_back(observe_element(element.element, now));
  }
  result.erase(std::remove_if(result.begin(), result.end(),
                              [](const ElectricalObservation& observation) {
                                return !observation.element.is_set();
                              }),
               result.end());
  return result;
}

ElectricalObservation SyntheticPlant::observe_element(const RefToken& element,
                                                      Timestamp now) const {
  ElectricalObservation observation;
  const ElementState* state = state_for(element);
  const TopologyElement* topology_element = nullptr;
  for (const auto& candidate : topology_.elements) {
    if (candidate.element == element) {
      topology_element = &candidate;
    }
  }
  if (state == nullptr || topology_element == nullptr || state->suppressed) {
    return observation;
  }
  observation.element = element;
  observation.kind = topology_element->kind;
  observation.origin = ObservationOrigin::SyntheticPlant;
  observation.quality = state->quality;
  observation.sequence = ObservationSequence::from_value(++sequence_);
  observation.observed_at = now;
  observation.provenance = "synthetic-plant:" + config_.site;
  observation.energization = state->energization;
  observation.breaker = state->breaker;
  observation.generator = state->generator;
  observation.transfer = state->transfer;
  if (state->energization == EnergizationState::DeEnergized) {
    observation.voltage = MilliVolts::from_value(0);
    observation.frequency = MilliHertz::from_value(0);
    observation.has_voltage = true;
    observation.has_frequency = true;
  } else {
    observation.voltage = state->voltage;
    observation.frequency = state->frequency;
    observation.has_voltage = true;
    observation.has_frequency = true;
  }
  observation.reserve = state->reserve;
  observation.has_reserve = state->has_reserve;
  observation.current = state->current;
  observation.has_current = state->has_current;

  codec::Writer writer;
  codec::encode(writer, element);
  codec::encode(writer, observation.sequence);
  codec::encode(writer, observation.observed_at);
  writer.u8(static_cast<std::uint8_t>(observation.energization));
  writer.u8(static_cast<std::uint8_t>(observation.breaker));
  writer.u8(static_cast<std::uint8_t>(observation.generator));
  writer.u8(static_cast<std::uint8_t>(observation.transfer));
  if (writer.ok()) {
    observation.evidence_fingerprint = codec::fnv1a128(writer.bytes());
  }
  return observation;
}

void SyntheticPlant::apply_request_effect(const ResponseRequest& request) {
  const auto target = request.target.element;
  switch (request.kind) {
    case RequestKind::IsolateElement: {
      RefToken point = target;
      for (const auto& element : topology_.elements) {
        if (element.element == target && element.has_isolation_point) {
          point = element.isolation_point;
        }
      }
      if (auto* state = state_for(point); state != nullptr) {
        state->breaker = BreakerPosition::Open;
      }
      break;
    }
    case RequestKind::BlockTransfer: {
      if (auto* state = state_for(target); state != nullptr) {
        state->transfer = TransferState::Isolated;
      }
      break;
    }
    case RequestKind::SelectFeed: {
      if (auto* state = state_for(target); state != nullptr) {
        state->energization = EnergizationState::Energized;
        state->voltage = config_.nominal_voltage;
        state->frequency = config_.nominal_frequency;
      }
      break;
    }
    case RequestKind::StartGenerator: {
      auto* state = state_for(target);
      if (state != nullptr) {
        if (generator_starts_) {
          state->generator = GeneratorState::Running;
          state->transfer = TransferState::Transferring;
        } else {
          state->generator = GeneratorState::Off;
          state->transfer = TransferState::Failed;
        }
      }
      break;
    }
    case RequestKind::SynchronizeGenerator: {
      auto* state = state_for(target);
      if (state != nullptr) {
        if (generator_synchronizes_ && transfers_) {
          state->generator = GeneratorState::Synchronized;
          state->transfer = TransferState::OnGenerator;
        } else if (!generator_synchronizes_) {
          state->generator = GeneratorState::Running;
          state->transfer = TransferState::Failed;
        } else {
          state->generator = GeneratorState::Synchronized;
          state->transfer = TransferState::Failed;
        }
      }
      break;
    }
    case RequestKind::TransferToGenerator: {
      auto* state = state_for(target);
      if (state != nullptr) {
        if (transfers_) {
          state->transfer = TransferState::OnGenerator;
        } else {
          state->transfer = TransferState::Failed;
        }
      }
      break;
    }
    case RequestKind::ShedLoad: {
      if (auto* state = state_for(target); state != nullptr) {
        state->current = MilliAmps::from_value(0);
        state->has_current = true;
      }
      break;
    }
    case RequestKind::DeferReclose: {
      deferred_.insert(target);
      break;
    }
    case RequestKind::Reclose: {
      if (deferred_.find(target) == deferred_.end()) {
        if (auto* state = state_for(target); state != nullptr) {
          state->breaker = BreakerPosition::Closed;
        }
      }
      break;
    }
    case RequestKind::RestoreNormalFeed: {
      if (auto* state = state_for(target); state != nullptr) {
        state->energization = EnergizationState::Energized;
        if (state->voltage.value() == 0) {
          state->voltage = config_.nominal_voltage;
        }
        if (state->frequency.value() == 0) {
          state->frequency = config_.nominal_frequency;
        }
      }
      break;
    }
    case RequestKind::CapPower:
    case RequestKind::PreserveReserve:
    case RequestKind::VerifyDeEnergization:
    case RequestKind::Unknown:
      break;
  }
  recompute_energization();
}

Result<DispatchOutcome> SyntheticPlant::dispatch(const ResponseRequest& request) {
  dispatched_.push_back(request);
  RefToken controller;
  auto controller_ref = RefToken::make(RefKind::Controller,
                                       std::string{"controller-"} +
                                           std::string{request_owner_name(request.target.owner)});
  if (controller_ref.ok()) {
    controller = controller_ref.value();
  }
  if (!config_.actuate_on_request) {
    return DispatchOutcome::accepted(controller,
                                     "recorded by the synthetic plant without an effect");
  }
  apply_request_effect(request);
  return DispatchOutcome::accepted(
      controller, std::string{"synthetic controller applied "} +
                      std::string{request_kind_name(request.kind)});
}

void SyntheticPlant::on_plan_published(const ResponsePlan& plan) { static_cast<void>(plan); }

std::vector<ResponseRequest> SyntheticPlant::dispatched() const { return dispatched_; }

// --- scenarios -------------------------------------------------------------

namespace {

inline unsigned& store_counter() {
  static unsigned counter = 0;
  return counter;
}

struct Driver {
  ScenarioOptions options{};
  SyntheticPlantConfig config{};
  std::unique_ptr<SyntheticPlant> plant{};
  std::shared_ptr<ManualClock> clock{};
  std::shared_ptr<ManualClock> runtime_clock{};
  std::unique_ptr<PowerFailureRuntime> runtime{};
  std::vector<std::string> notes{};
  std::vector<ProtectedObligation> obligations{};
  bool ok{true};
  std::size_t plan_generations{0};
  Timestamp now{};
  std::string store_directory{};
  bool owns_store{false};

  void note(std::string text) { notes.push_back(std::move(text)); }

  void expect(bool condition, std::string message) {
    if (!condition) {
      ok = false;
      notes.push_back("FAILED: " + std::move(message));
    } else {
      notes.push_back("ok: " + std::move(message));
    }
  }

  Result<void> setup() {
    plant = std::make_unique<SyntheticPlant>(config);
    clock = std::make_shared<ManualClock>(timestamp_from_unix_millis(1767225600000LL));
    runtime_clock = clock;
    now = clock->now().value();

    RuntimeOptions runtime_options;
    runtime_options.mode = options.durable ? StoreMode::Durable : StoreMode::Volatile;
    runtime_options.policy = default_policy();
    runtime_options.epoch = ControlEpoch::from_value(1);
    runtime_options.incarnation = ControllerIncarnation::from_value(1);
    if (options.durable) {
      store_directory = options.store_directory;
      if (store_directory.empty()) {
        std::error_code code;
        auto temp = std::filesystem::temp_directory_path(code);
        if (code) {
          return Status::error(StatusCode::StorePathInvalid, "no temporary directory")
              .with_context("scenario.store");
        }
        char suffix[64] = {};
        std::snprintf(suffix, sizeof(suffix), "pfm-scenario-store-%u",
                      static_cast<unsigned>(++store_counter()));
        temp /= suffix;
        std::filesystem::remove_all(temp, code);
        store_directory = temp.string();
        owns_store = true;
      }
      runtime_options.store_directory = store_directory;
    }
    auto opened = PowerFailureRuntime::open(
        runtime_options, runtime_clock,
        std::shared_ptr<ResponseTransport>{plant.get(), [](ResponseTransport*) {}});
    if (!opened.ok()) {
      return opened.status();
    }
    runtime = std::move(opened.value());
    return {};
  }

  Result<void> shutdown() {
    if (runtime) {
      auto closed = runtime->shutdown();
      runtime.reset();
      if (!closed.ok()) {
        return closed.status();
      }
    }
    if (owns_store && !options.keep_store) {
      std::error_code code;
      std::filesystem::remove_all(store_directory, code);
    }
    return {};
  }

  Result<AuthorityToken> token() { return runtime->current_authority(); }

  Result<void> publish_plant_topology() {
    auto current = token();
    if (!current.ok()) {
      return current.status();
    }
    return runtime->publish_topology(plant->topology(), current.value());
  }

  Result<void> publish_obligations(const std::vector<ProtectedObligation>& reported) {
    obligations = reported;
    auto current = token();
    if (!current.ok()) {
      return current.status();
    }
    return runtime->publish_obligations(obligations, current.value());
  }

  // A reporting authority re-reports: an obligation report is only current for
  // the freshness window, and a stale report blocks recovery by design.
  Result<void> refresh_obligations() {
    if (obligations.empty()) {
      return {};
    }
    for (auto& obligation : obligations) {
      if (obligation.has_report) {
        obligation.reported_at = now;
      }
    }
    auto current = token();
    if (!current.ok()) {
      return current.status();
    }
    return runtime->publish_obligations(obligations, current.value());
  }

  Result<void> open_incident() {
    auto current = token();
    if (!current.ok()) {
      return current.status();
    }
    current.value().incident = IncidentId::from_value(1);
    current.value().generation = IncidentGeneration::from_value(1);
    return runtime->open_incident(current.value());
  }

  // Admits current synthetic evidence and evaluates, without confirming any
  // effect afterwards: this is how work is deliberately left in flight.
  Result<EvaluationOutcome> evaluate_only(Duration step) {
    auto advanced = clock->advance(step);
    if (!advanced.ok()) {
      return advanced.status();
    }
    now = clock->now().value();
    auto current = token();
    if (!current.ok()) {
      return current.status();
    }
    if (auto r = runtime->admit_evidence(plant->observe(now), current.value()); !r.ok()) {
      return r.status();
    }
    if (auto r = refresh_obligations(); !r.ok()) {
      return r.status();
    }
    current = token();
    if (!current.ok()) {
      return current.status();
    }
    auto outcome = runtime->evaluate(current.value());
    if (outcome.ok()) {
      plan_generations += 1;
    }
    return outcome;
  }

  // Admits current synthetic evidence, evaluates, and then feeds back the
  // evidence that confirms the effects the plant actually performed.
  Result<EvaluationOutcome> tick(Duration step) {
    auto advanced = clock->advance(step);
    if (!advanced.ok()) {
      return advanced.status();
    }
    now = clock->now().value();

    auto current = token();
    if (!current.ok()) {
      return current.status();
    }
    auto observations = plant->observe(now);
    if (auto r = runtime->admit_evidence(observations, current.value()); !r.ok()) {
      if (r.code() != StatusCode::EvidenceUnknownTarget) {
        return r.status();
      }
    }
    // Admitting evidence advances the state revision, so the decision is
    // fenced against the revision the admission produced.
    current = token();
    if (!current.ok()) {
      return current.status();
    }
    if (auto r = refresh_obligations(); !r.ok()) {
      return r.status();
    }
    current = token();
    if (!current.ok()) {
      return current.status();
    }
    auto outcome = runtime->evaluate(current.value());
    if (!outcome.ok()) {
      return outcome.status();
    }
    plan_generations += 1;

    // Confirmation loop: any open request whose required proof the plant now
    // exhibits is confirmed with that evidence.
    auto requests = runtime->requests();
    if (requests.ok()) {
      for (const auto& request : requests.value()) {
        if (!request_state_is_open(request.state)) {
          continue;
        }
        auto evidence = plant->observe_element(request.target.element, now);
        if (!evidence.element.is_set()) {
          continue;
        }
        auto fresh = token();
        if (!fresh.ok()) {
          return fresh.status();
        }
        static_cast<void>(runtime->record_effect(request.id, evidence, fresh.value()));
      }
    }
    return outcome;
  }
};

ProtectedObligation make_obligation(ObligationId id, RefKind kind, const std::string& name,
                                    ObligationStatus status, Timestamp reported_at) {
  ProtectedObligation obligation;
  obligation.id = id;
  auto target = RefToken::make(kind, name);
  if (target.ok()) {
    obligation.target = target.value();
  }
  obligation.protection = ProtectionClass::ServiceLevel;
  obligation.status = status;
  obligation.has_report = true;
  obligation.reported_at = reported_at;
  auto authority = RefToken::make(RefKind::Controller, "facility-obligations");
  if (authority.ok()) {
    obligation.reporting_authority = authority.value();
  }
  obligation.has_reserve_floor = true;
  obligation.reserve_floor = milli_percent_from_value(50000);
  obligation.policy_generation = PolicyGeneration::from_value(1);
  return obligation;
}

constexpr std::string_view kScenarioNames[] = {
    "healthy-baseline",
    "feed-loss-recovery",
    "feed-degradation",
    "switchgear-fault",
    "breaker-trip-defer-reclose",
    "pdu-failure",
    "ups-on-battery",
    "ups-reserve-insufficient",
    "generator-start-failure",
    "generator-sync-failure",
    "generator-transfer-failure",
    "shared-domain-failure",
    "evidence-loss-fails-closed",
    "restart-recovers-authority-not-evidence",
};

}  // namespace

std::vector<std::string> scenario_names() {
  std::vector<std::string> names;
  for (const auto& name : kScenarioNames) {
    names.emplace_back(name);
  }
  return names;
}

Result<ScenarioOutcome> run_scenario(std::string_view name, const ScenarioOptions& options) {
  bool known = false;
  for (const auto& candidate : kScenarioNames) {
    if (candidate == name) {
      known = true;
    }
  }
  if (!known) {
    return Status::error(StatusCode::InvalidArgument, "no such scenario")
        .with_context(std::string{name});
  }

  Driver driver;
  driver.options = options;
  if (name == "restart-recovers-authority-not-evidence") {
    // This scenario proves authority handling across a restart, so it needs a
    // durable store even when the caller asked for a volatile run.
    driver.options.durable = true;
  }
  driver.config.site = "site-a";
  driver.config.bus_fanout = 2;

  auto setup = driver.setup();
  if (!setup.ok()) {
    return setup.status();
  }

  const auto finish = [&driver, name](FailureClass primary) {
    ScenarioOutcome outcome;
    outcome.name = std::string{name};
    outcome.ok = driver.ok;
    outcome.notes = driver.notes;
    outcome.primary_failure = primary;
    outcome.plan_generations = driver.plan_generations;
    auto requests = driver.runtime ? driver.runtime->requests() : Result<std::vector<ResponseRequest>>{std::vector<ResponseRequest>{}};
    if (requests.ok()) {
      outcome.requests_planned = requests.value().size();
      for (const auto& request : requests.value()) {
        if (request.state == RequestState::Verified) {
          outcome.requests_verified += 1;
        }
      }
    }
    if (driver.runtime) {
      auto status = driver.runtime->status();
      if (status.ok()) {
        outcome.recovered = status.value().lifecycle == IncidentLifecycle::Recovered ||
                            status.value().lifecycle == IncidentLifecycle::Closed;
      }
    }
    auto closed = driver.shutdown();
    if (!closed.ok()) {
      outcome.ok = false;
      outcome.notes.push_back(std::string{"FAILED: shutdown: "} +
                              std::string{closed.status().message()});
    }
    return outcome;
  };

  const auto fail = [&driver](const Status& status) {
    driver.shutdown();
    return status;
  };

  auto topology = driver.publish_plant_topology();
  if (!topology.ok()) {
    return fail(topology.status());
  }
  std::vector<ProtectedObligation> obligations;
  obligations.push_back(make_obligation(ObligationId::from_value(1), RefKind::LoadGroup, "load-0-0-0",
                                        ObligationStatus::Satisfied, driver.now));
  auto published = driver.publish_obligations(obligations);
  if (!published.ok()) {
    return fail(published.status());
  }
  auto incident = driver.open_incident();
  if (!incident.ok()) {
    return fail(incident.status());
  }

  // A first healthy evaluation establishes the baseline plan and starts the
  // stability dwell.
  auto baseline = driver.tick(duration_from_seconds(5));
  if (!baseline.ok()) {
    return fail(baseline.status());
  }

  auto feed = driver.plant->element_ref(RefKind::UtilityFeed, "feed-0");
  auto breaker = driver.plant->element_ref(RefKind::Breaker, "brk-bus-0-0");
  auto bus = driver.plant->element_ref(RefKind::Bus, "bus-0-0");
  auto ups = driver.plant->element_ref(RefKind::UpsUnit, "ups-0-0");
  auto pdu = driver.plant->element_ref(RefKind::Pdu, "pdu-0-0");
  auto generator = driver.plant->element_ref(RefKind::Generator, "gen-0");
  if (!feed.ok() || !breaker.ok() || !bus.ok() || !ups.ok() || !pdu.ok() || !generator.ok()) {
    return fail(Status::error(StatusCode::TopologyUnknownElement,
                              "the synthetic plant did not build the expected topology"));
  }

  const std::string scenario{name};

  if (scenario == "healthy-baseline") {
    auto plan = driver.runtime->plan();
    driver.expect(plan.ok() && plan.value().failures.empty(), "a healthy plant has no failures");
    driver.expect(plan.ok() && plan.value().requests.empty(),
                  "a healthy plant justifies no bounded requests");
    driver.expect(driver.plant->dispatched().empty(), "no request was dispatched");
  } else if (scenario == "feed-loss-recovery") {
    driver.plant->lose_utility(feed.value());
    auto outcome = driver.tick(duration_from_seconds(5));
    if (!outcome.ok()) {
      return fail(outcome.status());
    }
    driver.expect(outcome.value().primary_failure == FailureClass::UtilityFeedLoss,
                  "the feed loss is classified as a utility feed loss");
    driver.expect(outcome.value().requests_planned > 0, "bounded requests were planned");
    bool saw_isolation = false;
    bool saw_generator_start = false;
    for (const auto& request : driver.plant->dispatched()) {
      if (request.kind == RequestKind::IsolateElement) {
        saw_isolation = true;
      }
      if (request.kind == RequestKind::StartGenerator) {
        saw_generator_start = true;
      }
    }
    driver.expect(saw_isolation, "an isolation request reached the owning controller");
    driver.expect(saw_generator_start, "a generator start request reached the owning controller");
    for (int i = 0; i < 3; ++i) {
      auto step = driver.tick(duration_from_seconds(5));
      if (!step.ok()) {
        return fail(step.status());
      }
    }
    // The incident cannot be recovered while the fault is present.
    auto during = driver.runtime->recovery();
    driver.expect(during.ok() && !during.value().eligible,
                  "recovery is refused while the fault is still present");
    // Restore the feed; the isolated elements must be restored explicitly.
    driver.plant->restore_utility(feed.value());
    auto authorize_token = driver.token();
    if (!authorize_token.ok()) {
      return fail(authorize_token.status());
    }
    // Recovery requires an explicit authorization for this incident
    // generation; without it the authorization gate blocks even a healthy
    // plant.
    auto authorized = driver.runtime->authorize_recovery(authorize_token.value());
    driver.expect(authorized.ok(), "recovery authorization is accepted");
    for (int i = 0; i < 12; ++i) {
      auto step = driver.tick(duration_from_seconds(5));
      if (!step.ok()) {
        return fail(step.status());
      }
      auto assessment = driver.runtime->recovery();
      if (assessment.ok() && assessment.value().eligible) {
        break;
      }
    }
    auto assessment = driver.runtime->recovery();
    if (!assessment.ok()) {
      return fail(assessment.status());
    }
    driver.expect(assessment.value().eligible,
                  "recovery becomes eligible once the whole scope reports healthy");
    auto current = driver.token();
    if (!current.ok()) {
      return fail(current.status());
    }
    auto began = driver.runtime->begin_recovery(current.value());
    driver.expect(began.ok(), std::string{"recovery begins once it is eligible and authorized"} +
                                   (began.ok() ? std::string{}
                                               : std::string{" -- "} +
                                                     std::string{began.status().to_string()}));
    auto refreshed = driver.token();
    auto completed = driver.runtime->complete_recovery(refreshed.value());
    driver.expect(completed.ok(), "recovery completes");
    auto final_status = driver.runtime->status();
    driver.expect(final_status.ok() &&
                      (final_status.value().lifecycle == IncidentLifecycle::Recovered ||
                       final_status.value().lifecycle == IncidentLifecycle::Closed),
                  "the incident reaches the recovered state");
    auto final_token = driver.token();
    auto closed = final_token.ok() ? driver.runtime->close_incident(final_token.value())
                                   : Result<void>{final_token.status()};
    driver.expect(closed.ok(), "the incident closes");
  } else if (scenario == "feed-degradation") {
    driver.plant->degrade_utility(feed.value(), MilliVolts::from_value(180000),
                                  MilliHertz::from_value(50000));
    auto outcome = driver.tick(duration_from_seconds(5));
    if (!outcome.ok()) {
      return fail(outcome.status());
    }
    driver.expect(outcome.value().primary_failure == FailureClass::UtilityFeedDegradation,
                  "an out-of-band feed is classified as degradation");
  } else if (scenario == "switchgear-fault") {
    driver.plant->trip_breaker(breaker.value());
    auto outcome = driver.tick(duration_from_seconds(5));
    if (!outcome.ok()) {
      return fail(outcome.status());
    }
    bool saw_breaker_failure = false;
    auto classification = driver.runtime->classification();
    if (classification.ok()) {
      for (const auto& failure : classification.value().failures) {
        if (failure.klass == FailureClass::BreakerFailure &&
            failure.element == breaker.value()) {
          saw_breaker_failure = true;
        }
      }
    }
    driver.expect(saw_breaker_failure,
                  "a tripped bus breaker is classified as a breaker failure of its own");
    bool saw_defer = false;
    for (const auto& request : driver.plant->dispatched()) {
      if (request.kind == RequestKind::DeferReclose) {
        saw_defer = true;
      }
    }
    driver.expect(saw_defer, "a reclose is deferred until the fault is proven cleared");
  } else if (scenario == "breaker-trip-defer-reclose") {
    driver.plant->trip_breaker(breaker.value());
    for (int i = 0; i < 2; ++i) {
      auto step = driver.tick(duration_from_seconds(5));
      if (!step.ok()) {
        return fail(step.status());
      }
    }
    auto requests = driver.runtime->requests();
    if (!requests.ok()) {
      return fail(requests.status());
    }
    bool saw_reclose = false;
    for (const auto& request : requests.value()) {
      if (request.kind == RequestKind::Reclose) {
        saw_reclose = true;
      }
    }
    driver.expect(!saw_reclose, "no reclose is requested while the fault is present");
  } else if (scenario == "pdu-failure") {
    // The breaker on the PDU input opens without a trip: the PDU below it loses
    // supply while its own feed is still live, which is the branch failure the
    // boundary must classify rather than attribute upstream.
    driver.plant->trip_breaker(breaker.value());
    auto outcome = driver.tick(duration_from_seconds(5));
    if (!outcome.ok()) {
      return fail(outcome.status());
    }
    driver.expect(outcome.value().failures >= 1, "the failure is classified");
    driver.expect(outcome.value().primary_failure != FailureClass::None,
                  "a primary failure is named");
  } else if (scenario == "ups-on-battery") {
    driver.plant->lose_utility(feed.value());
    driver.plant->set_reserve(ups.value(), milli_percent_from_value(70000));
    auto outcome = driver.tick(duration_from_seconds(5));
    if (!outcome.ok()) {
      return fail(outcome.status());
    }
    auto classification = driver.runtime->classification();
    driver.expect(classification.ok(), "a classification is available");
  } else if (scenario == "ups-reserve-insufficient") {
    driver.plant->set_reserve(ups.value(), milli_percent_from_value(15000));
    auto outcome = driver.tick(duration_from_seconds(5));
    if (!outcome.ok()) {
      return fail(outcome.status());
    }
    driver.expect(outcome.value().primary_failure == FailureClass::UpsReserveInsufficient,
                  "a reserve below the floor is classified");
    bool saw_preserve = false;
    bool saw_shed = false;
    for (const auto& request : driver.plant->dispatched()) {
      if (request.kind == RequestKind::PreserveReserve) {
        saw_preserve = true;
      }
      if (request.kind == RequestKind::ShedLoad) {
        saw_shed = true;
      }
    }
    driver.expect(saw_preserve, "a reserve floor is requested");
    driver.expect(saw_shed, "load shedding is requested to hold the reserve floor");
  } else if (scenario == "generator-start-failure") {
    driver.plant->lose_utility(feed.value());
    driver.plant->set_generator_starts(false);
    for (int i = 0; i < 2; ++i) {
      auto step = driver.tick(duration_from_seconds(5));
      if (!step.ok()) {
        return fail(step.status());
      }
    }
    auto classification = driver.runtime->classification();
    if (!classification.ok()) {
      return fail(classification.status());
    }
    bool saw_start_failure = false;
    for (const auto& failure : classification.value().failures) {
      if (failure.klass == FailureClass::GeneratorStartFailure) {
        saw_start_failure = true;
      }
    }
    driver.expect(saw_start_failure, "a generator that did not start is classified");
  } else if (scenario == "generator-sync-failure") {
    driver.plant->lose_utility(feed.value());
    driver.plant->set_generator_synchronizes(false);
    for (int i = 0; i < 3; ++i) {
      auto step = driver.tick(duration_from_seconds(5));
      if (!step.ok()) {
        return fail(step.status());
      }
    }
    auto classification = driver.runtime->classification();
    if (!classification.ok()) {
      return fail(classification.status());
    }
    bool saw_sync_failure = false;
    for (const auto& failure : classification.value().failures) {
      if (failure.klass == FailureClass::GeneratorSyncFailure) {
        saw_sync_failure = true;
      }
    }
    driver.expect(saw_sync_failure, "a generator that will not synchronize is classified");
  } else if (scenario == "generator-transfer-failure") {
    driver.plant->lose_utility(feed.value());
    driver.plant->set_transfers(false);
    for (int i = 0; i < 3; ++i) {
      auto step = driver.tick(duration_from_seconds(5));
      if (!step.ok()) {
        return fail(step.status());
      }
    }
    auto classification = driver.runtime->classification();
    if (!classification.ok()) {
      return fail(classification.status());
    }
    bool saw_transfer_failure = false;
    for (const auto& failure : classification.value().failures) {
      if (failure.klass == FailureClass::GeneratorTransferFailure) {
        saw_transfer_failure = true;
      }
    }
    driver.expect(saw_transfer_failure, "a failed transfer is classified");
  } else if (scenario == "shared-domain-failure") {
    driver.plant->lose_utility(feed.value());
    auto outcome = driver.tick(duration_from_seconds(5));
    if (!outcome.ok()) {
      return fail(outcome.status());
    }
    auto scope = driver.runtime->scope();
    if (!scope.ok()) {
      return fail(scope.status());
    }
    driver.expect(scope.value().shared_domain_impacted,
                  "a shared upstream failure domain is recognised");
    driver.expect(scope.value().impacted_elements.size() > 1,
                  "the affected scope covers the whole shared domain");
    auto assessment = driver.runtime->recovery();
    driver.expect(assessment.ok() && !assessment.value().eligible,
                  "recovery is blocked while the shared domain is impacted");
  } else if (scenario == "evidence-loss-fails-closed") {
    driver.plant->set_evidence_suppressed(bus.value(), true);
    for (int i = 0; i < 4; ++i) {
      auto step = driver.tick(duration_from_seconds(5));
      if (!step.ok()) {
        return fail(step.status());
      }
    }
    auto classification = driver.runtime->classification();
    if (!classification.ok()) {
      return fail(classification.status());
    }
    bool saw_ambiguous = false;
    for (const auto& failure : classification.value().failures) {
      if (failure.klass == FailureClass::AmbiguousElectricalEvidence &&
          failure.element == bus.value()) {
        saw_ambiguous = true;
      }
    }
    driver.expect(saw_ambiguous, "lost evidence becomes an ambiguous failure, not health");
    auto assessment = driver.runtime->recovery();
    driver.expect(assessment.ok() && !assessment.value().eligible,
                  "recovery is blocked while an element no longer reports");
  } else if (scenario == "restart-recovers-authority-not-evidence") {
    driver.plant->lose_utility(feed.value());
    for (int i = 0; i < 2; ++i) {
      auto step = driver.tick(duration_from_seconds(5));
      if (!step.ok()) {
        return fail(step.status());
      }
    }
    // Leave a bounded request in flight: it is issued and acknowledged, but no
    // evidence of its effect has arrived.
    driver.plant->trip_breaker(breaker.value());
    auto in_flight = driver.evaluate_only(duration_from_seconds(5));
    if (!in_flight.ok()) {
      return fail(in_flight.status());
    }
    auto before = driver.runtime->status();
    driver.expect(before.ok() && before.value().incident_live, "the incident is live");
    driver.expect(before.ok() && before.value().open_requests > 0,
                  "a bounded request is in flight when the controller stops");

    auto closed = driver.runtime->shutdown();
    if (!closed.ok()) {
      return fail(closed.status());
    }
    // Reopen the same durable store with the same authority.
    RuntimeOptions runtime_options;
    runtime_options.mode = StoreMode::Durable;
    runtime_options.policy = default_policy();
    runtime_options.epoch = ControlEpoch::from_value(1);
    runtime_options.incarnation = ControllerIncarnation::from_value(1);
    runtime_options.store_directory = driver.store_directory;
    auto reopened = PowerFailureRuntime::open(
        runtime_options, driver.runtime_clock,
        std::shared_ptr<ResponseTransport>{driver.plant.get(), [](ResponseTransport*) {}});
    if (!reopened.ok()) {
      return fail(reopened.status());
    }
    driver.runtime = std::move(reopened.value());
    auto state = driver.runtime->state();
    if (!state.ok()) {
      return fail(state.status());
    }
    bool any_recovered = false;
    for (const auto& slot : state.value().observations) {
      if (slot.recovered) {
        any_recovered = true;
      }
    }
    driver.expect(any_recovered, "restored evidence is marked recovered, not current");
    driver.expect(!state.value().has_plan, "no plan survives a restart as current");
    auto status = driver.runtime->status();
    driver.expect(status.ok() && status.value().open_requests > 0,
                  "in-flight requests are retained as indeterminate after a restart");
    auto assessment = driver.runtime->recovery();
    driver.expect(!assessment.ok(), "recovery is refused until a plan is computed again");
  }

  auto outcome = finish(FailureClass::None);
  return outcome;
}

}  // namespace summon::pfm
