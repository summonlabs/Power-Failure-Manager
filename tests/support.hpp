// Power Failure Manager -- shared test fixtures.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "pfm/runtime.hpp"
#include "pfm/scenario.hpp"
#include "pfm/state.hpp"

namespace pfmtest {

// A disposable directory that removes itself. It never touches anything
// outside the process temporary area.
class TempDirectory {
 public:
  explicit TempDirectory(const std::string& tag) {
    std::error_code code;
    auto base = std::filesystem::temp_directory_path(code);
    if (code) {
      base = std::filesystem::current_path(code);
    }
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "pfm-%s-%u", tag.c_str(),
                  static_cast<unsigned>(counter_++));
    path_ = (base / buffer).string();
    std::filesystem::remove_all(path_, code);
    std::filesystem::create_directories(path_, code);
  }

  ~TempDirectory() {
    std::error_code code;
    std::filesystem::remove_all(path_, code);
  }

  TempDirectory(const TempDirectory&) = delete;
  TempDirectory& operator=(const TempDirectory&) = delete;

  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] std::string file(const std::string& name) const {
    return (std::filesystem::path{path_} / name).string();
  }

 private:
  std::string path_{};
  static inline unsigned counter_{0};
};

inline std::shared_ptr<summon::pfm::ManualClock> make_clock() {
  return std::make_shared<summon::pfm::ManualClock>(
      summon::pfm::timestamp_from_unix_millis(1767225600000LL));
}

inline summon::pfm::RuntimeOptions durable_options(const std::string& directory,
                                                   summon::pfm::ControlEpoch epoch =
                                                       summon::pfm::ControlEpoch::from_value(1),
                                                   summon::pfm::ControllerIncarnation incarnation =
                                                       summon::pfm::ControllerIncarnation::from_value(1)) {
  summon::pfm::RuntimeOptions options;
  options.mode = summon::pfm::StoreMode::Durable;
  options.store_directory = directory;
  options.policy = summon::pfm::default_policy();
  options.policy.generation = summon::pfm::PolicyGeneration::from_value(1);
  options.epoch = epoch;
  options.incarnation = incarnation;
  return options;
}

inline summon::pfm::RuntimeOptions volatile_options() {
  summon::pfm::RuntimeOptions options = summon::pfm::RuntimeOptions::volatile_memory();
  options.policy = summon::pfm::default_policy();
  options.policy.generation = summon::pfm::PolicyGeneration::from_value(1);
  return options;
}

// A runtime wired to the synthetic plant: the plant supplies evidence and
// answers bounded requests, and nothing else is involved.
struct PlantFixture {
  std::unique_ptr<summon::pfm::SyntheticPlant> plant{};
  std::unique_ptr<summon::pfm::PowerFailureRuntime> runtime{};
  std::shared_ptr<summon::pfm::ManualClock> clock{};
  std::string directory{};

  static summon::pfm::Result<std::unique_ptr<PlantFixture>> make(
      summon::pfm::SyntheticPlantConfig config, summon::pfm::StoreMode mode,
      const std::string& directory = std::string{},
      summon::pfm::ControlEpoch epoch = summon::pfm::ControlEpoch::from_value(1),
      summon::pfm::ControllerIncarnation incarnation =
          summon::pfm::ControllerIncarnation::from_value(1)) {
    auto fixture = std::unique_ptr<PlantFixture>{new PlantFixture{}};
    fixture->plant = std::make_unique<summon::pfm::SyntheticPlant>(config);
    fixture->clock = make_clock();
    fixture->directory = directory;
    summon::pfm::RuntimeOptions options =
        mode == summon::pfm::StoreMode::Durable ? durable_options(directory, epoch, incarnation)
                                                : volatile_options();
    options.epoch = epoch;
    options.incarnation = incarnation;
    auto opened = summon::pfm::PowerFailureRuntime::open(
        options, fixture->clock,
        std::shared_ptr<summon::pfm::ResponseTransport>{fixture->plant.get(),
                                                        [](summon::pfm::ResponseTransport*) {}});
    if (!opened.ok()) {
      return opened.status();
    }
    fixture->runtime = std::move(opened.value());
    return fixture;
  }

  [[nodiscard]] summon::pfm::Result<summon::pfm::AuthorityToken> token() const {
    return runtime->current_authority();
  }

  // Publishes topology and obligations and opens the first incident, leaving
  // the runtime ready for an evaluation.
  [[nodiscard]] summon::pfm::Result<void> prime(
      summon::pfm::IncidentId incident = summon::pfm::IncidentId::from_value(1),
      summon::pfm::IncidentGeneration generation =
          summon::pfm::IncidentGeneration::from_value(1)) {
    auto current = token();
    if (!current.ok()) {
      return current.status();
    }
    if (auto r = runtime->publish_topology(plant->topology(), current.value()); !r.ok()) {
      return r;
    }
    current = token();
    if (!current.ok()) {
      return current.status();
    }
    current.value().incident = incident;
    current.value().generation = generation;
    return runtime->open_incident(current.value());
  }

  [[nodiscard]] summon::pfm::Result<summon::pfm::EvaluationOutcome> tick(
      summon::pfm::Duration step) {
    auto advanced = clock->advance(step);
    if (!advanced.ok()) {
      return advanced.status();
    }
    const auto now = clock->now().value();
    auto current = token();
    if (!current.ok()) {
      return current.status();
    }
    auto observations = plant->observe(now);
    if (auto r = runtime->admit_evidence(observations, current.value()); !r.ok()) {
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
    auto requests = runtime->requests();
    if (requests.ok()) {
      for (const auto& request : requests.value()) {
        if (!summon::pfm::request_state_is_open(request.state)) {
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

}  // namespace pfmtest
