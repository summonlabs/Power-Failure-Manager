// Power Failure Manager -- out-of-tree downstream consumer.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// This program consumes the installed Power Failure Manager package exactly as
// a downstream facility controller would. It owns a disposable directory, opens
// a durable runtime inside it, publishes the topology of a synthetic plant,
// opens an incident, admits the evidence the plant reports, evaluates it,
// confirms the effects the plant actually applied, prints one machine-readable
// summary line, and then proves that the durable store it leaves behind
// replays into the same state. It leaves nothing behind and returns 0 only
// when every step succeeded.

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include "pfm/classification.hpp"
#include "pfm/evidence.hpp"
#include "pfm/policy.hpp"
#include "pfm/runtime.hpp"
#include "pfm/scenario.hpp"
#include "pfm/state.hpp"
#include "pfm/store.hpp"
#include "pfm/transport.hpp"

namespace pfm = summon::pfm;

namespace {

// A directory this program creates and removes itself.
class DisposableDirectory {
 public:
  explicit DisposableDirectory(const std::string& tag) {
    std::error_code code;
    auto base = std::filesystem::temp_directory_path(code);
    if (code) {
      base = std::filesystem::current_path(code);
    }
    path_ = (base / ("pfm-consumer-" + tag)).string();
    std::filesystem::remove_all(path_, code);
    std::filesystem::create_directories(path_, code);
  }

  ~DisposableDirectory() {
    std::error_code code;
    std::filesystem::remove_all(path_, code);
  }

  DisposableDirectory(const DisposableDirectory&) = delete;
  DisposableDirectory& operator=(const DisposableDirectory&) = delete;

  [[nodiscard]] const std::string& path() const { return path_; }

 private:
  std::string path_{};
};

int report_failure(const pfm::Status& status) {
  std::printf("PFM_CONSUMER ok=0 code=%s detail=%s\n",
              std::string{status.code_name()}.c_str(),
              std::string{status.message()}.c_str());
  std::fflush(stdout);
  return 1;
}

int report_failure(const char* code, const char* detail) {
  std::printf("PFM_CONSUMER ok=0 code=%s detail=%s\n", code, detail);
  std::fflush(stdout);
  return 1;
}

}  // namespace

int main() {
  const DisposableDirectory directory{"smoke"};

  pfm::SyntheticPlant plant{pfm::SyntheticPlantConfig{}};
  auto clock = std::make_shared<pfm::SystemClock>();
  auto transport =
      std::shared_ptr<pfm::ResponseTransport>{&plant, [](pfm::ResponseTransport*) {}};

  pfm::RuntimeOptions options;
  options.mode = pfm::StoreMode::Durable;
  options.store_directory = directory.path();
  options.policy = pfm::default_policy();
  options.policy.generation = pfm::PolicyGeneration::from_value(1);
  options.epoch = pfm::ControlEpoch::from_value(1);
  options.incarnation = pfm::ControllerIncarnation::from_value(1);

  auto opened = pfm::PowerFailureRuntime::open(options, clock, transport);
  if (!opened.ok()) {
    return report_failure(opened.status());
  }
  std::unique_ptr<pfm::PowerFailureRuntime> runtime = std::move(opened.value());

  // The convenience token carries the current revision; every mutation is
  // fenced against the revision the previous mutation produced.
  const auto token = [&runtime]() { return runtime->current_authority(); };

  {
    auto current = token();
    if (!current.ok()) {
      return report_failure(current.status());
    }
    auto published = runtime->publish_topology(plant.topology(), current.value());
    if (!published.ok()) {
      return report_failure(published.status());
    }
  }

  {
    auto current = token();
    if (!current.ok()) {
      return report_failure(current.status());
    }
    current.value().incident = pfm::IncidentId::from_value(1);
    current.value().generation = pfm::IncidentGeneration::from_value(1);
    auto incident = runtime->open_incident(current.value());
    if (!incident.ok()) {
      return report_failure(incident.status());
    }
  }

  // A utility feed is lost, and the plant is driven to its settled state
  // before the runtime is told anything at all.
  auto feed = plant.element_ref(pfm::RefKind::UtilityFeed, "feed-0");
  if (!feed.ok()) {
    return report_failure(feed.status());
  }
  for (int pass = 0; pass < 8; ++pass) {
    plant.lose_utility(feed.value());
  }

  pfm::EvaluationOutcome outcome{};
  // The failure the incident was raised for, kept separately from the newest
  // classification: a response that succeeds removes the failure from the
  // current plan, and the summary should still say what happened.
  pfm::FailureClass classified_failure = pfm::FailureClass::None;
  for (int round = 0; round < 3; ++round) {
    auto instant = clock->now();
    if (!instant.ok()) {
      return report_failure(instant.status());
    }
    auto current = token();
    if (!current.ok()) {
      return report_failure(current.status());
    }
    auto admitted = runtime->admit_evidence(plant.observe(instant.value()), current.value());
    if (!admitted.ok()) {
      return report_failure(admitted.status());
    }

    current = token();
    if (!current.ok()) {
      return report_failure(current.status());
    }
    auto evaluated = runtime->evaluate(current.value());
    if (!evaluated.ok()) {
      return report_failure(evaluated.status());
    }
    outcome = evaluated.value();
    if (classified_failure == pfm::FailureClass::None &&
        outcome.primary_failure != pfm::FailureClass::None) {
      classified_failure = outcome.primary_failure;
    }

    // Confirm only what the plant now exhibits. An acknowledgement proves
    // nothing, and evidence that does not show the requested effect is
    // refused, which is a normal outcome rather than an error.
    auto requests = runtime->requests();
    if (!requests.ok()) {
      return report_failure(requests.status());
    }
    for (const auto& request : requests.value()) {
      if (!pfm::request_state_is_open(request.state)) {
        continue;
      }
      auto evidence = plant.observe_element(request.target.element, instant.value());
      if (!evidence.element.is_set()) {
        continue;
      }
      auto fresh = token();
      if (!fresh.ok()) {
        return report_failure(fresh.status());
      }
      static_cast<void>(runtime->record_effect(request.id, evidence, fresh.value()));
    }
  }

  auto status = runtime->status();
  if (!status.ok()) {
    return report_failure(status.status());
  }
  const std::size_t request_count = status.value().requests;
  const std::size_t verified_count = status.value().verified_requests;
  const std::uint64_t plan_generation = status.value().plan_generation.value();
  const std::uint64_t incident_generation = status.value().incident_generation.value();
  const std::string lifecycle{pfm::incident_lifecycle_name(status.value().lifecycle)};
  const std::string failure_code{pfm::failure_class_code(classified_failure)};
  const std::string current_code{pfm::failure_class_code(outcome.primary_failure)};

  auto stopped = runtime->shutdown();
  if (!stopped.ok()) {
    return report_failure(stopped.status());
  }
  runtime.reset();

  // The store must replay: folding the retained journal onto its checkpoint has
  // to reproduce the persisted live state byte for byte.
  {
    pfm::StoreOptions store_options;
    store_options.directory = directory.path();
    store_options.mode = pfm::StoreMode::Durable;
    auto store = pfm::DurableStore::open(store_options);
    if (!store.ok()) {
      return report_failure(store.status());
    }
    auto snapshot = store.value()->load();
    if (!snapshot.ok()) {
      return report_failure(snapshot.status());
    }
    pfm::DomainState folded = snapshot.value().checkpoint;
    for (const auto& entry : snapshot.value().journal) {
      auto applied = pfm::apply_journal_entry(folded, entry);
      if (!applied.ok()) {
        return report_failure(applied.status());
      }
    }
    if (pfm::encode_state(folded) != pfm::encode_state(snapshot.value().live)) {
      return report_failure("pfm.replay_divergence",
                            "the retained journal does not replay onto its checkpoint");
    }
    auto store_closed = store.value()->close();
    if (!store_closed.ok()) {
      return report_failure(store_closed.status());
    }
    store.value().reset();
  }

  // The durable state must also come back as a live controller, with restored
  // evidence marked recovered rather than current.
  {
    auto reopened = pfm::PowerFailureRuntime::open(options, clock, transport);
    if (!reopened.ok()) {
      return report_failure(reopened.status());
    }
    auto replayed = reopened.value()->status();
    if (!replayed.ok()) {
      return report_failure(replayed.status());
    }
    if (!replayed.value().incident_live) {
      return report_failure("pfm.no_active_incident", "the incident did not survive the restart");
    }
    auto state = reopened.value()->state();
    if (!state.ok()) {
      return report_failure(state.status());
    }
    bool any_recovered = false;
    for (const auto& slot : state.value().observations) {
      if (slot.recovered) {
        any_recovered = true;
      }
    }
    if (!any_recovered) {
      return report_failure("pfm.evidence_not_recovered",
                            "restored evidence was not marked recovered");
    }
    auto closed = reopened.value()->shutdown();
    if (!closed.ok()) {
      return report_failure(closed.status());
    }
  }

  std::printf(
      "PFM_CONSUMER ok=1 plan_generation=%llu incident_generation=%llu primary_failure=%s "
      "current_failure=%s requests=%zu verified=%zu lifecycle=%s replay=ok\n",
      static_cast<unsigned long long>(plan_generation),
      static_cast<unsigned long long>(incident_generation), failure_code.c_str(),
      current_code.c_str(), request_count, verified_count, lifecycle.c_str());
  std::fflush(stdout);
  return 0;
}
