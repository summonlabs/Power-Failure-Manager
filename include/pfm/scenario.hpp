// Power Failure Manager -- synthetic electrical plant and scenarios.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "pfm/plan.hpp"
#include "pfm/policy.hpp"
#include "pfm/runtime.hpp"
#include "pfm/state.hpp"
#include "pfm/topology.hpp"
#include "pfm/transport.hpp"

namespace summon::pfm {

// The synthetic plant is a deterministic in-process model of an electrical
// distribution path: utility feeds, switchgear, buses, breakers, PDUs, branch
// circuits, UPS units, a generator, and load groups.
//
// It is SYNTHETIC. It contacts no hardware, no BMS, and no DCIM, and it never
// actuates anything. It answers bounded requests the way a scripted adjacent
// controller would, and it produces the evidence the runtime classifies.
struct SyntheticPlantConfig {
  std::string site{"site-a"};
  // Number of PDU/branch pairs under each bus.
  std::uint32_t bus_fanout{2};
  // Number of buses under each switchgear.
  std::uint32_t switchgear_fanout{2};
  std::uint32_t topology_generation{1};
  MilliVolts nominal_voltage{230000};
  MilliHertz nominal_frequency{50000};
  // When false the plant records requests but changes nothing, which models a
  // controller that answers without an effect.
  bool actuate_on_request{true};
};

class SyntheticPlant final : public ResponseTransport {
 public:
  explicit SyntheticPlant(SyntheticPlantConfig config);

  [[nodiscard]] const TopologySnapshot& topology() const noexcept { return topology_; }
  [[nodiscard]] const SyntheticPlantConfig& config() const noexcept { return config_; }

  // Current electrical evidence for every element, at one instant.
  [[nodiscard]] std::vector<ElectricalObservation> observe(Timestamp now) const;
  [[nodiscard]] ElectricalObservation observe_element(const RefToken& element,
                                                      Timestamp now) const;
  // Evidence that is deliberately stale, for evidence-currency scenarios.
  [[nodiscard]] std::vector<ElectricalObservation> observe_stale(Timestamp now,
                                                                 Duration age) const;
  [[nodiscard]] Result<RefToken> element_ref(RefKind kind, std::string_view name) const;

  // --- fault injection (all synthetic) ------------------------------------

  void lose_utility(const RefToken& feed);
  void restore_utility(const RefToken& feed);
  void degrade_utility(const RefToken& feed, MilliVolts voltage, MilliHertz frequency);
  void trip_breaker(const RefToken& breaker);
  void close_breaker(const RefToken& breaker);
  void fail_ups(const RefToken& ups);
  void repair_ups(const RefToken& ups);
  void set_reserve(const RefToken& ups, MilliPercent reserve);
  void fail_generator(const RefToken& generator);
  void repair_generator(const RefToken& generator);
  // Makes every generator start attempt fail, which is a distinct failure from
  // a generator that is faulted in place.
  void set_generator_starts(bool succeed);
  void set_generator_synchronizes(bool succeed);
  void set_transfers(bool succeed);
  // Suppresses evidence for an element, so the runtime sees no current
  // observation rather than a healthy one.
  void set_evidence_suppressed(const RefToken& element, bool suppressed);
  void set_quality(const RefToken& element, EvidenceQuality quality);

  [[nodiscard]] std::vector<RefToken> elements_of_kind(ElementKind kind) const;

  // --- ResponseTransport --------------------------------------------------

  [[nodiscard]] Result<DispatchOutcome> dispatch(const ResponseRequest& request) override;
  void on_plan_published(const ResponsePlan& plan) override;

  [[nodiscard]] std::vector<ResponseRequest> dispatched() const;

 private:
  struct ElementState {
    EnergizationState energization{EnergizationState::Energized};
    BreakerPosition breaker{BreakerPosition::Closed};
    GeneratorState generator{GeneratorState::Off};
    TransferState transfer{TransferState::OnUtility};
    MilliVolts voltage{};
    bool has_voltage{true};
    MilliHertz frequency{};
    bool has_frequency{true};
    MilliPercent reserve{};
    bool has_reserve{false};
    MilliAmps current{};
    bool has_current{false};
    bool suppressed{false};
    EvidenceQuality quality{EvidenceQuality::Good};
  };

  void build_topology();
  void recompute_energization();
  void apply_request_effect(const ResponseRequest& request);
  [[nodiscard]] ElementState* state_for(const RefToken& element);
  [[nodiscard]] const ElementState* state_for(const RefToken& element) const;

  SyntheticPlantConfig config_{};
  TopologySnapshot topology_{};
  std::vector<std::pair<RefToken, ElementState>> states_{};
  std::set<RefToken> deferred_{};
  mutable std::vector<ResponseRequest> dispatched_{};
  mutable std::uint64_t sequence_{0};
  Timestamp dispatch_instant_{};
  bool generator_starts_{true};
  bool generator_synchronizes_{true};
  bool transfers_{true};
};

struct ScenarioOptions {
  std::string store_directory{};
  bool durable{true};
  bool keep_store{false};
  bool verbose{false};
};

struct ScenarioOutcome {
  std::string name{};
  bool ok{false};
  std::vector<std::string> notes{};
  FailureClass primary_failure{FailureClass::None};
  std::size_t plan_generations{0};
  std::size_t requests_planned{0};
  std::size_t requests_verified{0};
  bool recovered{false};
};

[[nodiscard]] std::vector<std::string> scenario_names();
[[nodiscard]] Result<ScenarioOutcome> run_scenario(std::string_view name,
                                                   const ScenarioOptions& options);

}  // namespace summon::pfm
