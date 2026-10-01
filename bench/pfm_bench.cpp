// Power Failure Manager -- benchmark.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// Measures COMPLETED useful operations only. The clock starts before an
// operation is submitted and stops after it has completed *and* its result has
// been checked, so no number below is submission latency. Warm-up operations
// are discarded, every benchmark runs the same workload, and a failed
// operation is counted as failed rather than folded into the rate.
//
//   usage: pfm_bench [--iterations=N] [--json]
//
// The facility model is SYNTHETIC: an in-process plant that produces evidence
// and answers bounded requests like a scripted adjacent controller. The
// persistence is REAL: every durable operation goes through the two-slot store
// (canonical encode, stage to the inactive slot, flush to the device, read the
// staged bytes back, then atomically replace the slot). No durable measurement
// excludes its commit.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "pfm/clock.hpp"
#include "pfm/plan.hpp"
#include "pfm/runtime.hpp"
#include "pfm/scenario.hpp"
#include "pfm/state.hpp"
#include "pfm/store.hpp"
#include "pfm/topology.hpp"

namespace pfm = summon::pfm;

namespace {

constexpr std::int64_t kStartInstantMillis = 1767225600000LL;
constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitFailure = 1;

using Stopwatch = std::chrono::steady_clock;

struct Options {
  std::uint64_t iterations{500};
  bool json{false};
};

struct Measurement {
  std::string name{};
  std::string unit{};
  std::string workload{};
  std::string durability{};
  std::uint64_t iterations{0};
  std::uint64_t completed{0};
  std::uint64_t failed{0};
  double seconds{0.0};
  std::uint64_t scale{0};
  std::string scale_unit{};
  // The first refusal observed, so a failed workload is diagnosable rather
  // than only countable.
  std::string failure_detail{};
};

void note_failure(Measurement& measurement, const pfm::Status& status) {
  if (measurement.failure_detail.empty()) {
    measurement.failure_detail = status.to_string();
  }
}

double elapsed_seconds(Stopwatch::time_point start, Stopwatch::time_point stop) {
  return std::chrono::duration<double>(stop - start).count();
}

std::string fixed(double value, int places) {
  char buffer[64] = {};
  std::snprintf(buffer, sizeof(buffer), "%.*f", places, value);
  return std::string{buffer};
}

std::string integer(std::uint64_t value) { return std::to_string(value); }

double rate(const Measurement& measurement) {
  if (measurement.seconds <= 0.0) {
    return 0.0;
  }
  return static_cast<double>(measurement.completed) / measurement.seconds;
}

double mean_latency_micros(const Measurement& measurement) {
  if (measurement.completed == 0) {
    return 0.0;
  }
  return measurement.seconds * 1.0e6 / static_cast<double>(measurement.completed);
}

std::string unique_directory(const std::string& tag) {
  std::error_code code;
  std::filesystem::path base = std::filesystem::temp_directory_path(code);
  if (code) {
    base = std::filesystem::current_path(code);
  }
  const auto stamp = Stopwatch::now().time_since_epoch().count();
  return (base / ("pfm-bench-" + tag + "-" + std::to_string(stamp))).string();
}

pfm::SyntheticPlantConfig workload_config() {
  pfm::SyntheticPlantConfig config;
  config.site = "bench-site";
  config.switchgear_fanout = 2;
  config.bus_fanout = 2;
  return config;
}

pfm::ProtectedObligation make_obligation(const pfm::RefToken& target, pfm::Timestamp reported_at) {
  pfm::ProtectedObligation obligation;
  obligation.id = pfm::ObligationId::from_value(1);
  obligation.target = target;
  obligation.protection = pfm::ProtectionClass::ServiceLevel;
  obligation.status = pfm::ObligationStatus::Satisfied;
  obligation.has_report = true;
  obligation.reported_at = reported_at;
  auto authority = pfm::RefToken::make(pfm::RefKind::Controller, "facility-obligations");
  if (authority.ok()) {
    obligation.reporting_authority = authority.value();
  }
  obligation.has_reserve_floor = true;
  obligation.reserve_floor = pfm::milli_percent_from_value(50000);
  obligation.policy_generation = pfm::PolicyGeneration::from_value(1);
  return obligation;
}

pfm::ElectricalPolicy workload_policy() {
  pfm::ElectricalPolicy policy = pfm::default_policy();
  policy.generation = pfm::PolicyGeneration::from_value(1);
  return policy;
}

std::uint64_t warmup_for(const Options& options) {
  const std::uint64_t proposed = options.iterations / 10;
  if (proposed < 1) {
    return 1;
  }
  return proposed > 100 ? 100 : proposed;
}

// --- 1. in-memory decision cycle -------------------------------------------

Measurement bench_decision_cycle(const Options& options) {
  Measurement measurement;
  measurement.name = "in-memory decision cycle";
  measurement.unit = "completed cycles/second";
  measurement.iterations = options.iterations;

  pfm::SyntheticPlant plant{workload_config()};
  auto feed = plant.element_ref(pfm::RefKind::UtilityFeed, "feed-0");
  if (feed.ok()) {
    plant.lose_utility(feed.value());
  }
  auto protected_group = plant.element_ref(pfm::RefKind::LoadGroup, "load-0-0-0");

  const pfm::Bounds bounds{};
  const pfm::ElectricalPolicy policy = workload_policy();
  auto topology = pfm::TopologyIndex::build(plant.topology(), bounds);
  if (!topology.ok()) {
    measurement.workload = "topology refused: " + topology.status().to_string();
    measurement.failed = options.iterations;
    return measurement;
  }

  measurement.scale = plant.topology().elements.size();
  measurement.scale_unit = "topology elements";
  measurement.workload =
      "classify + plan_response over " + integer(plant.observe(pfm::timestamp_from_unix_millis(
          kStartInstantMillis)).size()) +
      " current observations of a faulted synthetic plant (evidence assembly included)";

  pfm::Timestamp now = pfm::timestamp_from_unix_millis(kStartInstantMillis);
  std::uint64_t revision = 1;
  std::uint64_t plan_generation = 1;
  std::uint64_t request_id = 1;
  std::uint64_t attempt_id = 1;

  const auto cycle = [&]() -> bool {
    now = pfm::timestamp_from_unix_millis(now.value() + 1000);
    std::vector<pfm::ElectricalObservation> observations = plant.observe(now);
    auto classification = pfm::classify(observations, now, topology.value(), policy, bounds);
    if (!classification.ok()) {
      note_failure(measurement, classification.status());
      return false;
    }
    pfm::PlanInputs inputs;
    inputs.incident = pfm::IncidentId::from_value(1);
    inputs.incident_generation = pfm::IncidentGeneration::from_value(1);
    inputs.epoch = pfm::ControlEpoch::from_value(1);
    inputs.revision = pfm::StateRevision::from_value(revision++);
    inputs.next_plan_generation = pfm::PlanGeneration::from_value(plan_generation++);
    inputs.now = now;
    inputs.policy = policy;
    inputs.bounds = bounds;
    inputs.topology = &topology.value();
    inputs.current_observations = observations;
    if (protected_group.ok()) {
      inputs.obligations.push_back(make_obligation(protected_group.value(), now));
    }
    inputs.first_request_id = pfm::ResponseRequestId::from_value(request_id++);
    inputs.first_attempt_id = pfm::AttemptId::from_value(attempt_id++);
    auto plan = pfm::plan_response(inputs);
    if (!plan.ok()) {
      note_failure(measurement, plan.status());
    }
    return plan.ok();
  };

  const std::uint64_t warmup = warmup_for(options);
  for (std::uint64_t index = 0; index < warmup; ++index) {
    static_cast<void>(cycle());
  }
  std::uint64_t completed = 0;
  const auto start = Stopwatch::now();
  for (std::uint64_t index = 0; index < options.iterations; ++index) {
    if (cycle()) {
      completed += 1;
    }
  }
  const auto stop = Stopwatch::now();
  measurement.seconds = elapsed_seconds(start, stop);
  measurement.completed = completed;
  measurement.failed = options.iterations - completed;
  return measurement;
}

// --- 2. durable mutation ----------------------------------------------------

Measurement bench_durable_mutation(const Options& options) {
  Measurement measurement;
  measurement.name = "durable mutation";
  measurement.unit = "completed operations/second";
  measurement.iterations = options.iterations;
  measurement.workload =
      "durable evidence admission (one batch) + evaluate + checkpoint commit, over a live "
      "incident and a healthy synthetic plant";

  const std::string directory = unique_directory("durable");
  std::error_code code;
  std::filesystem::create_directories(directory, code);
  if (code) {
    measurement.workload = "the store directory could not be created";
    measurement.failed = options.iterations;
    return measurement;
  }

  pfm::SyntheticPlant plant{workload_config()};
  auto clock = std::make_shared<pfm::ManualClock>(
      pfm::timestamp_from_unix_millis(kStartInstantMillis));
  auto transport = std::shared_ptr<pfm::ResponseTransport>{
      &plant, [](pfm::ResponseTransport*) {}};
  pfm::RuntimeOptions runtime_options;
  runtime_options.mode = pfm::StoreMode::Durable;
  runtime_options.store_directory = directory;
  runtime_options.policy = workload_policy();
  runtime_options.epoch = pfm::ControlEpoch::from_value(1);
  runtime_options.incarnation = pfm::ControllerIncarnation::from_value(1);

  auto opened = pfm::PowerFailureRuntime::open(runtime_options, clock, transport);
  if (!opened.ok()) {
    measurement.workload = "runtime open refused: " + opened.status().to_string();
    measurement.failed = options.iterations;
    std::filesystem::remove_all(directory, code);
    return measurement;
  }
  auto runtime = std::move(opened.value());
  measurement.scale = plant.topology().elements.size();
  measurement.scale_unit = "topology elements";

  bool ready = true;
  {
    auto token = runtime->current_authority();
    ready = token.ok();
    if (ready) {
      ready = runtime->publish_topology(plant.topology(), token.value()).ok();
    }
    token = runtime->current_authority();
    auto group = plant.element_ref(pfm::RefKind::LoadGroup, "load-0-0-0");
    if (ready && token.ok() && group.ok()) {
      std::vector<pfm::ProtectedObligation> obligations;
      obligations.push_back(make_obligation(group.value(), clock->now().value()));
      ready = runtime->publish_obligations(obligations, token.value()).ok();
    }
    token = runtime->current_authority();
    if (ready && token.ok()) {
      token.value().incident = pfm::IncidentId::from_value(1);
      token.value().generation = pfm::IncidentGeneration::from_value(1);
      ready = runtime->open_incident(token.value()).ok();
    }
  }
  if (!ready) {
    measurement.workload = "the durable runtime could not be primed";
    measurement.failed = options.iterations;
    static_cast<void>(runtime->shutdown());
    std::filesystem::remove_all(directory, code);
    return measurement;
  }

  const auto operation = [&]() -> bool {
    auto advanced = clock->advance(pfm::duration_from_seconds(5));
    if (!advanced.ok()) {
      return false;
    }
    const pfm::Timestamp now = clock->now().value();
    auto token = runtime->current_authority();
    if (!token.ok()) {
      note_failure(measurement, token.status());
      return false;
    }
    if (auto admitted = runtime->admit_evidence(plant.observe(now), token.value());
        !admitted.ok()) {
      note_failure(measurement, admitted.status());
      return false;
    }
    token = runtime->current_authority();
    if (!token.ok()) {
      note_failure(measurement, token.status());
      return false;
    }
    if (auto outcome = runtime->evaluate(token.value()); !outcome.ok()) {
      note_failure(measurement, outcome.status());
      return false;
    }
    token = runtime->current_authority();
    if (!token.ok()) {
      note_failure(measurement, token.status());
      return false;
    }
    // The explicit commit: the retained journal is folded into the checkpoint
    // and the whole snapshot is published through the durable path again.
    auto committed = runtime->checkpoint(token.value());
    if (!committed.ok()) {
      note_failure(measurement, committed.status());
    }
    return committed.ok();
  };

  const std::uint64_t warmup = warmup_for(options);
  for (std::uint64_t index = 0; index < warmup; ++index) {
    static_cast<void>(operation());
  }

  auto status = runtime->store_status();
  const std::uint64_t commits_before = status.ok() ? status.value().commits : 0;

  std::uint64_t completed = 0;
  const auto start = Stopwatch::now();
  for (std::uint64_t index = 0; index < options.iterations; ++index) {
    if (operation()) {
      completed += 1;
    }
  }
  const auto stop = Stopwatch::now();

  status = runtime->store_status();
  const std::uint64_t commits_after = status.ok() ? status.value().commits : 0;
  const std::uint64_t commits = commits_after >= commits_before ? commits_after - commits_before : 0;

  measurement.seconds = elapsed_seconds(start, stop);
  measurement.completed = completed;
  measurement.failed = options.iterations - completed;
  const double per_operation =
      completed == 0 ? 0.0 : static_cast<double>(commits) / static_cast<double>(completed);
  measurement.durability = fixed(per_operation, 2) + " durable slot commits per completed operation (" +
                           integer(commits) + " commits observed)";
  static_cast<void>(runtime->shutdown());
  std::filesystem::remove_all(directory, code);
  return measurement;
}

// --- 3. durable full lifecycle ---------------------------------------------

struct LifecycleSession {
  std::unique_ptr<pfm::SyntheticPlant> plant{};
  std::shared_ptr<pfm::ManualClock> clock{};
  std::unique_ptr<pfm::PowerFailureRuntime> runtime{};
  std::string directory{};
  pfm::RefToken feed{};
  pfm::RefToken protected_group{};
  std::uint64_t ticks{0};

  [[nodiscard]] pfm::Result<void> tick(pfm::Duration step) {
    auto advanced = clock->advance(step);
    if (!advanced.ok()) {
      return advanced.status();
    }
    const pfm::Timestamp now = clock->now().value();
    ticks += 1;
    auto token = runtime->current_authority();
    if (!token.ok()) {
      return token.status();
    }
    if (auto admitted = runtime->admit_evidence(plant->observe(now), token.value());
        !admitted.ok()) {
      return admitted.status();
    }
    token = runtime->current_authority();
    if (!token.ok()) {
      return token.status();
    }
    if (auto outcome = runtime->evaluate(token.value()); !outcome.ok()) {
      return outcome.status();
    }
    // Confirm the effects the plant actually exhibits. An answer the runtime
    // refuses (a request that is already settled, for instance) is not a
    // benchmark failure: the phase goal below decides what completed means.
    auto requests = runtime->requests();
    if (!requests.ok()) {
      return requests.status();
    }
    for (const auto& request : requests.value()) {
      if (!pfm::request_state_is_open(request.state)) {
        continue;
      }
      const auto evidence = plant->observe_element(request.target.element, now);
      if (!evidence.element.is_set()) {
        continue;
      }
      auto fresh = runtime->current_authority();
      if (!fresh.ok()) {
        return fresh.status();
      }
      static_cast<void>(runtime->record_effect(request.id, evidence, fresh.value()));
    }
    return {};
  }

  [[nodiscard]] bool has_verified_request(pfm::RequestKind kind) const {
    auto requests = runtime->requests();
    if (!requests.ok()) {
      return false;
    }
    for (const auto& request : requests.value()) {
      if (request.kind == kind && request.state == pfm::RequestState::Verified) {
        return true;
      }
    }
    return false;
  }
};

pfm::Result<std::unique_ptr<LifecycleSession>> open_lifecycle(const std::string& directory) {
  auto session = std::unique_ptr<LifecycleSession>{new LifecycleSession{}};
  session->directory = directory;
  session->plant = std::make_unique<pfm::SyntheticPlant>(workload_config());
  session->clock = std::make_shared<pfm::ManualClock>(
      pfm::timestamp_from_unix_millis(kStartInstantMillis));

  auto feed = session->plant->element_ref(pfm::RefKind::UtilityFeed, "feed-0");
  auto group = session->plant->element_ref(pfm::RefKind::LoadGroup, "load-0-0-0");
  if (!feed.ok() || !group.ok()) {
    return pfm::Status::error(pfm::StatusCode::TopologyUnknownElement,
                              "the synthetic plant did not build the expected topology");
  }
  session->feed = feed.value();
  session->protected_group = group.value();

  auto transport = std::shared_ptr<pfm::ResponseTransport>{
      session->plant.get(), [](pfm::ResponseTransport*) {}};
  pfm::RuntimeOptions runtime_options;
  runtime_options.mode = pfm::StoreMode::Durable;
  runtime_options.store_directory = directory;
  runtime_options.policy = workload_policy();
  runtime_options.epoch = pfm::ControlEpoch::from_value(1);
  runtime_options.incarnation = pfm::ControllerIncarnation::from_value(1);
  auto opened = pfm::PowerFailureRuntime::open(runtime_options, session->clock, transport);
  if (!opened.ok()) {
    return opened.status();
  }
  session->runtime = std::move(opened.value());

  auto token = session->runtime->current_authority();
  if (!token.ok()) {
    return token.status();
  }
  if (auto published = session->runtime->publish_topology(session->plant->topology(),
                                                          token.value());
      !published.ok()) {
    return published.status();
  }
  token = session->runtime->current_authority();
  if (!token.ok()) {
    return token.status();
  }
  std::vector<pfm::ProtectedObligation> obligations;
  obligations.push_back(make_obligation(session->protected_group, session->clock->now().value()));
  if (auto published = session->runtime->publish_obligations(obligations, token.value());
      !published.ok()) {
    return published.status();
  }
  token = session->runtime->current_authority();
  if (!token.ok()) {
    return token.status();
  }
  token.value().incident = pfm::IncidentId::from_value(1);
  token.value().generation = pfm::IncidentGeneration::from_value(1);
  if (auto incident = session->runtime->open_incident(token.value()); !incident.ok()) {
    return incident.status();
  }
  return session;
}

// The timed region of one lifecycle: failure -> isolation -> restoration ->
// authorized recovery -> close. Store creation and topology publication are
// outside it.
pfm::Result<void> run_lifecycle_phases(LifecycleSession& session) {
  // Phase 1: the failure, the isolation it justifies, and the generator
  // transfer it starts. The same number of ticks is used every time, so every
  // lifecycle does the same work.
  session.plant->lose_utility(session.feed);
  for (int index = 0; index < 4; ++index) {
    if (auto stepped = session.tick(pfm::duration_from_seconds(5)); !stepped.ok()) {
      return stepped.status();
    }
  }
  if (!session.has_verified_request(pfm::RequestKind::IsolateElement)) {
    return pfm::Status::error(pfm::StatusCode::PreconditionFailed,
                              "no isolation request was verified");
  }

  session.plant->restore_utility(session.feed);
  pfm::Timestamp last_report{};
  bool has_last_report = false;
  const auto refresh_obligation = [&session, &last_report, &has_last_report]() -> pfm::Result<void> {
    const pfm::Timestamp now = session.clock->now().value();
    if (has_last_report &&
        now.value() - last_report.value() <
            session.runtime->state().value().policy.evidence_freshness_window.value()) {
      return {};
    }
    auto token = session.runtime->current_authority();
    if (!token.ok()) {
      return token.status();
    }
    std::vector<pfm::ProtectedObligation> obligations;
    obligations.push_back(make_obligation(session.protected_group, now));
    auto published = session.runtime->publish_obligations(obligations, token.value());
    if (!published.ok()) {
      return published.status();
    }
    last_report = now;
    has_last_report = true;
    return {};
  };
  if (auto refreshed = refresh_obligation(); !refreshed.ok()) {
    return refreshed.status();
  }

  // Operator authorization is itself a gate, so wait until every other gate is
  // satisfied before asking for it.
  bool facts_settled = false;
  for (int index = 0; index < 16 && !facts_settled; ++index) {
    if (auto refreshed = refresh_obligation(); !refreshed.ok()) {
      return refreshed.status();
    }
    if (auto stepped = session.tick(pfm::duration_from_seconds(5)); !stepped.ok()) {
      return stepped.status();
    }
    auto assessment = session.runtime->recovery();
    if (!assessment.ok()) {
      return assessment.status();
    }
    facts_settled = true;
    for (const auto& gate : assessment.value().gates) {
      if (!gate.satisfied && gate.gate != pfm::RecoveryGate::OperatorAuthorization) {
        facts_settled = false;
      }
    }
  }
  if (!facts_settled) {
    return pfm::Status::error(pfm::StatusCode::RecoveryNotEligible,
                              "the recovery gates were never satisfied by current evidence");
  }

  const auto call = [&session](auto&& body) -> pfm::Result<void> {
    auto current = session.runtime->current_authority();
    if (!current.ok()) {
      return current.status();
    }
    return body(current.value());
  };
  if (auto authorized = call([&](const pfm::AuthorityToken& current) {
        return session.runtime->authorize_recovery(current);
      });
      !authorized.ok()) {
    return authorized.status();
  }
  auto authorized_assessment = session.runtime->recovery();
  if (!authorized_assessment.ok()) {
    return authorized_assessment.status();
  }
  if (!authorized_assessment.value().eligible) {
    return pfm::Status::error(pfm::StatusCode::RecoveryNotEligible,
                              "recovery did not become eligible after operator authorization");
  }

  if (auto began = call([&](const pfm::AuthorityToken& current) {
        return session.runtime->begin_recovery(current);
      });
      !began.ok()) {
    return began.status();
  }
  if (auto completed = call([&](const pfm::AuthorityToken& current) {
        return session.runtime->complete_recovery(current);
      });
      !completed.ok()) {
    return completed.status();
  }
  if (auto closed = call([&](const pfm::AuthorityToken& current) {
        return session.runtime->close_incident(current);
      });
      !closed.ok()) {
    return closed.status();
  }
  auto status = session.runtime->status();
  if (!status.ok()) {
    return status.status();
  }
  if (status.value().lifecycle != pfm::IncidentLifecycle::Closed) {
    return pfm::Status::error(pfm::StatusCode::IllegalTransition,
                              "the lifecycle did not reach the closed state");
  }
  return {};
}

Measurement bench_durable_lifecycle(const Options& options) {
  Measurement measurement;
  measurement.name = "durable full lifecycle";
  measurement.unit = "completed lifecycles/second";
  measurement.iterations = options.iterations;
  measurement.workload =
      "failure -> isolation verified -> feed restoration -> recovery authorized, begun and "
      "completed -> incident closed, on a durable store created fresh for each lifecycle";
  measurement.scale = 0;
  measurement.scale_unit = "ticks per lifecycle";

  const std::string root = unique_directory("lifecycle");
  std::error_code code;
  std::filesystem::create_directories(root, code);
  if (code) {
    measurement.workload = "the benchmark directory could not be created";
    measurement.failed = options.iterations;
    return measurement;
  }

  const std::uint64_t warmup = warmup_for(options);
  std::vector<std::string> pending;
  pending.reserve(static_cast<std::size_t>(warmup + options.iterations));
  for (std::uint64_t index = 0; index < warmup + options.iterations; ++index) {
    pending.push_back((std::filesystem::path{root} / ("store-" + integer(index))).string());
  }

  std::uint64_t completed = 0;
  std::uint64_t failed = 0;
  std::uint64_t ticks = 0;
  std::uint64_t commits = 0;
  double seconds = 0.0;
  for (std::uint64_t index = 0; index < warmup + options.iterations; ++index) {
    const std::string& directory = pending[static_cast<std::size_t>(index)];
    std::filesystem::create_directories(directory, code);
    if (code) {
      failed += 1;
      continue;
    }
    auto opened = open_lifecycle(directory);
    bool ok = opened.ok();
    if (!ok) {
      note_failure(measurement, opened.status());
    }
    double lifecycle_seconds = 0.0;
    std::uint64_t lifecycle_ticks = 0;
    std::uint64_t lifecycle_commits = 0;
    if (ok) {
      auto status_before = opened.value()->runtime->store_status();
      const std::uint64_t before = status_before.ok() ? status_before.value().commits : 0;
      const auto start = Stopwatch::now();
      const auto phases = run_lifecycle_phases(*opened.value());
      const auto stop = Stopwatch::now();
      ok = phases.ok();
      if (!ok) {
        note_failure(measurement, phases.status());
      }
      lifecycle_seconds = elapsed_seconds(start, stop);
      lifecycle_ticks = opened.value()->ticks;
      auto status_after = opened.value()->runtime->store_status();
      const std::uint64_t after = status_after.ok() ? status_after.value().commits : 0;
      lifecycle_commits = after >= before ? after - before : 0;
      static_cast<void>(opened.value()->runtime->shutdown());
    }
    std::filesystem::remove_all(directory, code);
    if (index < warmup) {
      continue;  // warm-up: discarded
    }
    if (ok) {
      completed += 1;
      ticks += lifecycle_ticks;
      commits += lifecycle_commits;
      seconds += lifecycle_seconds;
    } else {
      failed += 1;
    }
  }

  measurement.seconds = seconds;
  measurement.completed = completed;
  measurement.failed = failed;
  measurement.scale = completed == 0 ? 0 : ticks / completed;
  const double per_lifecycle =
      completed == 0 ? 0.0 : static_cast<double>(commits) / static_cast<double>(completed);
  measurement.durability = fixed(per_lifecycle, 2) +
                           " durable slot commits per completed lifecycle (" + integer(commits) +
                           " commits observed)";
  std::filesystem::remove_all(root, code);
  return measurement;
}

// --- reporting --------------------------------------------------------------

// The scale of the workload every benchmark runs, described once.
struct WorkloadScale {
  std::size_t elements{0};
  std::size_t observations{0};
  std::string config{};
};

WorkloadScale describe_workload() {
  pfm::SyntheticPlant plant{workload_config()};
  WorkloadScale scale;
  scale.elements = plant.topology().elements.size();
  scale.observations =
      plant.observe(pfm::timestamp_from_unix_millis(kStartInstantMillis)).size();
  scale.config = "site=" + workload_config().site + " switchgear_fanout=" +
                 integer(workload_config().switchgear_fanout) + " bus_fanout=" +
                 integer(workload_config().bus_fanout);
  return scale;
}

void print_text(const Options& options, const std::vector<Measurement>& results,
                const WorkloadScale& scale) {
  std::printf("pfm_bench: Power Failure Manager benchmarks\n");
  std::printf("  facility model: SYNTHETIC (%s elements, %s observations per decision; %s)\n",
              integer(scale.elements).c_str(), integer(scale.observations).c_str(),
              scale.config.c_str());
  std::printf("  persistence: REAL (durable two-slot store: encode, stage, flush to the device,\n");
  std::printf("               read back, atomically replace the inactive slot)\n");
  std::printf("  iterations: %s per benchmark; warm-up operations discarded: %s\n",
              integer(options.iterations).c_str(), integer(warmup_for(options)).c_str());
  std::printf("  measurement: completed operations only; failed operations are reported\n");
  std::printf("\n");
  for (const auto& measurement : results) {
    std::printf("%s\n", measurement.name.c_str());
    std::printf("  workload: %s\n", measurement.workload.c_str());
    if (measurement.scale != 0) {
      std::printf("  scale: %s %s\n", integer(measurement.scale).c_str(),
                  measurement.scale_unit.c_str());
    }
    std::printf("  completed: %s of %s in %s s\n", integer(measurement.completed).c_str(),
                integer(measurement.iterations).c_str(), fixed(measurement.seconds, 6).c_str());
    std::printf("  rate: %s %s\n", fixed(rate(measurement), 3).c_str(), measurement.unit.c_str());
    std::printf("  mean latency: %s microseconds\n",
                fixed(mean_latency_micros(measurement), 3).c_str());
    if (!measurement.durability.empty()) {
      std::printf("  durability: %s\n", measurement.durability.c_str());
    }
    if (measurement.failed != 0) {
      std::printf("  failed: %s\n", integer(measurement.failed).c_str());
    }
    if (!measurement.failure_detail.empty()) {
      std::printf("  first refusal: %s\n", measurement.failure_detail.c_str());
    }
    std::printf("\n");
  }
}

void print_json(const Options& options, const std::vector<Measurement>& results,
                const WorkloadScale& scale) {
  std::printf("{\n");
  std::printf("  \"facility_model\": \"synthetic\",\n");
  std::printf("  \"persistence\": \"real\",\n");
  std::printf("  \"facility_config\": \"%s\",\n", scale.config.c_str());
  std::printf("  \"facility_elements\": %s,\n", integer(scale.elements).c_str());
  std::printf("  \"observations_per_decision\": %s,\n", integer(scale.observations).c_str());
  std::printf("  \"iterations\": %s,\n", integer(options.iterations).c_str());
  std::printf("  \"warmup\": %s,\n", integer(warmup_for(options)).c_str());
  std::printf("  \"measurement\": \"completed operations only\",\n");
  std::printf("  \"results\": [\n");
  for (std::size_t index = 0; index < results.size(); ++index) {
    const auto& measurement = results[index];
    std::printf("    {\n");
    std::printf("      \"name\": \"%s\",\n", measurement.name.c_str());
    std::printf("      \"workload\": \"%s\",\n", measurement.workload.c_str());
    std::printf("      \"unit\": \"%s\",\n", measurement.unit.c_str());
    std::printf("      \"scale\": %s,\n", integer(measurement.scale).c_str());
    std::printf("      \"scale_unit\": \"%s\",\n", measurement.scale_unit.c_str());
    std::printf("      \"iterations\": %s,\n", integer(measurement.iterations).c_str());
    std::printf("      \"completed\": %s,\n", integer(measurement.completed).c_str());
    std::printf("      \"failed\": %s,\n", integer(measurement.failed).c_str());
    std::printf("      \"seconds\": %s,\n", fixed(measurement.seconds, 6).c_str());
    std::printf("      \"per_second\": %s,\n", fixed(rate(measurement), 3).c_str());
    std::printf("      \"mean_latency_microseconds\": %s,\n",
                fixed(mean_latency_micros(measurement), 3).c_str());
    std::printf("      \"durability\": \"%s\",\n", measurement.durability.c_str());
    std::printf("      \"failure_detail\": \"%s\"\n", measurement.failure_detail.c_str());
    std::printf("    }%s\n", index + 1 == results.size() ? "" : ",");
  }
  std::printf("  ]\n");
  std::printf("}\n");
}

bool parse_arguments(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--json") {
      options.json = true;
      continue;
    }
    const std::string prefix = "--iterations=";
    if (argument.rfind(prefix, 0) == 0) {
      const std::string text = argument.substr(prefix.size());
      if (text.empty()) {
        std::fprintf(stderr, "pfm_bench: --iterations requires a value\n");
        return false;
      }
      char* end = nullptr;
      const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
      if (end == nullptr || *end != '\0' || value == 0) {
        std::fprintf(stderr, "pfm_bench: --iterations must be a positive integer\n");
        return false;
      }
      options.iterations = static_cast<std::uint64_t>(value);
      continue;
    }
    if (argument == "--help" || argument == "-h") {
      std::printf("usage: pfm_bench [--iterations=N] [--json]\n");
      std::exit(kExitOk);
    }
    std::fprintf(stderr, "pfm_bench: unknown argument: %s\n", argument.c_str());
    std::fprintf(stderr, "usage: pfm_bench [--iterations=N] [--json]\n");
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_arguments(argc, argv, options)) {
    return kExitUsage;
  }

  const WorkloadScale scale = describe_workload();
  std::vector<Measurement> results;
  results.push_back(bench_decision_cycle(options));
  results.push_back(bench_durable_mutation(options));
  results.push_back(bench_durable_lifecycle(options));

  if (options.json) {
    print_json(options, results, scale);
  } else {
    print_text(options, results, scale);
  }

  for (const auto& measurement : results) {
    if (measurement.completed != measurement.iterations) {
      return kExitFailure;
    }
  }
  return kExitOk;
}
