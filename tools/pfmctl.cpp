// Power Failure Manager -- administration CLI.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// pfmctl is a thin, prompt-free command line over the public pfm API. It never
// reads stdin and never waits for input: every command runs to completion and
// reports its outcome through a deterministic exit code.
//
//   0  the command did what it says it did
//   1  the command ran and the library refused, or the required end state was
//      not reached
//   2  the command line itself was wrong; usage goes to stderr
//
// Normal output is plain text on stdout. Diagnostics go to stderr.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "pfm/clock.hpp"
#include "pfm/report.hpp"
#include "pfm/runtime.hpp"
#include "pfm/scenario.hpp"
#include "pfm/state.hpp"
#include "pfm/store.hpp"
#include "pfm/version.hpp"

namespace pfm = summon::pfm;

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;

// The fixed instant every command starts from, so that a run is reproducible.
constexpr std::int64_t kStartInstantMillis = 1767225600000LL;

constexpr const char* kUsageText =
    "usage: pfmctl <command> [options]\n"
    "\n"
    "commands:\n"
    "  version                        component identity, DCCP boundary, store format\n"
    "  boundaries                     owned boundary and adjacent non-owned authorities\n"
    "  scenarios                      list the synthetic scenario names\n"
    "  scenario --name NAME | --all   run a synthetic scenario and print its notes\n"
    "  demo [--store DIR] [--keep]    run the feed-loss lifecycle against a durable store\n"
    "  inspect --store DIR            open a durable store read-only and render it\n"
    "  verify --store DIR             prove the journal replays onto the checkpoint\n"
    "\n"
    "options:\n"
    "  --store DIR   durable store directory\n"
    "  --name NAME   scenario name\n"
    "  --all         run every scenario\n"
    "  --keep        keep a store directory this command created\n"
    "  --help        print this text\n";

struct Arguments {
  std::string command{};
  std::string store{};
  std::string name{};
  bool all{false};
  bool keep{false};
  bool help{false};
};

void write_out(const std::string& text) { std::fputs(text.c_str(), stdout); }
void write_err(const std::string& text) { std::fputs(text.c_str(), stderr); }

void line(const std::string& text) {
  write_out(text);
  write_out("\n");
}

void diagnostics(const std::string& text) {
  write_err("pfmctl: ");
  write_err(text);
  write_err("\n");
}

void usage() { write_err(kUsageText); }

// Reports a refused step and answers false so that a command can return the
// failure exit code without repeating the same three lines.
bool refused(std::string_view step, const pfm::Status& status) {
  diagnostics(std::string{step} + ": " + status.to_string());
  return false;
}

std::string number(std::uint64_t value) { return std::to_string(value); }

// A unique directory under the process temporary area. Nothing outside the
// temporary area is ever named by this program.
std::string unique_directory(const std::string& tag) {
  std::error_code code;
  std::filesystem::path base = std::filesystem::temp_directory_path(code);
  if (code) {
    base = std::filesystem::current_path(code);
  }
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  static unsigned counter = 0;
  const std::string name = "pfmctl-" + tag + "-" + std::to_string(stamp) + "-" +
                           std::to_string(counter++);
  return (base / name).string();
}

void remove_directory(const std::string& directory) {
  std::error_code code;
  std::filesystem::remove_all(directory, code);
}

bool directory_exists(const std::string& directory) {
  std::error_code code;
  const bool exists = std::filesystem::exists(directory, code);
  return exists && !code;
}

// Runs one authority-bearing call with a freshly read token. Every mutation
// advances the durable revision, so a token is never reused across calls.
template <class Call>
bool with_token(pfm::PowerFailureRuntime& runtime, std::string_view step, Call&& call) {
  auto token = runtime.current_authority();
  if (!token.ok()) {
    return refused(step, token.status());
  }
  auto result = call(token.value());
  if (!result.ok()) {
    return refused(step, result.status());
  }
  return true;
}

// --- version ---------------------------------------------------------------

int command_version() {
  line(std::string{"component: "} + std::string{pfm::component_name()});
  line(std::string{"version: "} + std::string{pfm::version_string()});
  line("dccp boundary: " + number(pfm::kDccpBoundary));
  line("store format version: " + number(pfm::kStoreFormatVersion));
  return kExitOk;
}

// --- boundaries ------------------------------------------------------------

constexpr const char* kOwnedBoundary[] = {
    "classification of typed electrical failures from current, attributable evidence",
    "the bounded response plan, its justification trace, and its request identities",
    "the request lifecycle and the verification state of every attempt",
    "the durable incident record: checkpoint, journal, replay, and fencing",
    "recovery gates and the refusal to treat acknowledgement or elapsed time as proof",
};

int command_boundaries() {
  line("owned boundary (DCCP boundary " + number(pfm::kDccpBoundary) + "):");
  for (const char* item : kOwnedBoundary) {
    line(std::string{"  owns: "} + item);
  }
  line("not owned -- adjacent authorities PFM requests and never performs:");
  for (std::uint8_t raw = 1; raw <= 9; ++raw) {
    const auto owner = static_cast<pfm::RequestOwner>(raw);
    line(std::string{"  requests only: "} + std::string{pfm::request_owner_name(owner)});
  }
  return kExitOk;
}

// --- scenarios -------------------------------------------------------------

int command_scenarios() {
  for (const auto& name : pfm::scenario_names()) {
    line(name);
  }
  return kExitOk;
}

int run_one_scenario(const std::string& name, bool keep) {
  const std::string directory = unique_directory("scenario");
  pfm::ScenarioOptions options;
  options.durable = true;
  options.store_directory = directory;
  options.keep_store = true;  // pfmctl owns the directory and removes it below.

  auto outcome = pfm::run_scenario(name, options);
  if (!outcome.ok()) {
    diagnostics("scenario " + name + ": " + outcome.status().to_string());
    remove_directory(directory);
    return kExitFailure;
  }
  line("scenario: " + outcome.value().name);
  for (const auto& note : outcome.value().notes) {
    line("  " + note);
  }
  line(std::string{"  primary_failure: "} +
       std::string{pfm::failure_class_code(outcome.value().primary_failure)});
  line("  plan_generations: " + number(outcome.value().plan_generations) +
       " requests_planned: " + number(outcome.value().requests_planned) +
       " requests_verified: " + number(outcome.value().requests_verified) +
       " recovered: " + (outcome.value().recovered ? "yes" : "no"));
  if (!keep) {
    remove_directory(directory);
  } else {
    line("  store kept at: " + directory);
  }
  line(std::string{"result: "} + (outcome.value().ok ? "ok" : "failed"));
  return outcome.value().ok ? kExitOk : kExitFailure;
}

int command_scenario(const Arguments& args) {
  if (args.help) {
    usage();
    return kExitOk;
  }
  if (!args.all && args.name.empty()) {
    diagnostics("scenario requires --name NAME or --all");
    usage();
    return kExitUsage;
  }
  if (args.all && !args.name.empty()) {
    diagnostics("scenario accepts either --name NAME or --all, not both");
    usage();
    return kExitUsage;
  }
  int result = kExitOk;
  const std::vector<std::string> names =
      args.all ? pfm::scenario_names() : std::vector<std::string>{args.name};
  for (const auto& name : names) {
    const int one = run_one_scenario(name, args.keep);
    if (one != kExitOk) {
      result = one;
    }
  }
  return result;
}

// --- demo ------------------------------------------------------------------

// One running demo session: a synthetic plant wired to a durable runtime, with
// the transition cursor used to print each durable state change exactly once.
struct DemoSession {
  pfm::SyntheticPlant* plant{nullptr};
  std::unique_ptr<pfm::PowerFailureRuntime> runtime{};
  std::shared_ptr<pfm::ManualClock> clock{};
  std::size_t printed_transitions{0};
  std::size_t tick{0};

  pfm::Result<pfm::AuthorityToken> token() { return runtime->current_authority(); }
};

void print_transitions(DemoSession& session) {
  auto transitions = session.runtime->transitions();
  if (!transitions.ok()) {
    return;
  }
  const auto& all = transitions.value();
  if (session.printed_transitions >= all.size()) {
    session.printed_transitions = all.size();
    return;
  }
  line("transitions:");
  for (std::size_t index = session.printed_transitions; index < all.size(); ++index) {
    const auto& record = all[index];
    std::string text = "  #" + number(record.sequence.value()) + " " +
                       std::string{pfm::journal_kind_name(record.kind)} + " revision " +
                       number(record.from_revision.value()) + "->" +
                       number(record.to_revision.value()) + " at " +
                       number(static_cast<std::uint64_t>(record.recorded_at.value()));
    if (record.reason != pfm::ReasonCode::None) {
      text += std::string{" reason="} + std::string{pfm::reason_code_name(record.reason)};
    }
    if (record.subject.is_set()) {
      text += " subject=" + record.subject.to_string();
    }
    line(text);
  }
  session.printed_transitions = all.size();
}

bool print_classification(DemoSession& session, std::string_view step) {
  auto classification = session.runtime->classification();
  if (!classification.ok()) {
    return refused(step, classification.status());
  }
  line("classification:");
  write_out(pfm::render_classification(classification.value()));
  return true;
}

bool print_plan(DemoSession& session, std::string_view step) {
  auto plan = session.runtime->plan();
  if (!plan.ok()) {
    return refused(step, plan.status());
  }
  line("plan:");
  write_out(pfm::render_plan(plan.value()));
  return true;
}

void print_request_states(DemoSession& session, const std::string& heading) {
  auto requests = session.runtime->requests();
  if (!requests.ok()) {
    return;
  }
  line(heading + ": " + number(requests.value().size()));
  for (const auto& request : requests.value()) {
    line("  " + pfm::render_request(request));
  }
}

// One demo step: advance the clock, admit the evidence the plant produces, ask
// for a decision, print what was decided, then confirm the effects the plant
// actually performed with the evidence it actually exhibits.
bool step(DemoSession& session, pfm::Duration advance, std::string_view label) {
  const auto advanced = session.clock->advance(advance);
  if (!advanced.ok()) {
    return refused(label, advanced.status());
  }
  const pfm::Timestamp now = session.clock->now().value();
  session.tick += 1;

  auto token = session.token();
  if (!token.ok()) {
    return refused(label, token.status());
  }
  const auto observations = session.plant->observe(now);
  auto admitted = session.runtime->admit_evidence(observations, token.value());
  if (!admitted.ok()) {
    return refused(label, admitted.status());
  }

  token = session.token();
  if (!token.ok()) {
    return refused(label, token.status());
  }
  auto outcome = session.runtime->evaluate(token.value());
  if (!outcome.ok()) {
    return refused(label, outcome.status());
  }

  line("");
  line("--- step " + number(session.tick) + ": " + std::string{label} + " at t=" +
       number(static_cast<std::uint64_t>(now.value())) + " ---");
  if (!print_classification(session, label)) {
    return false;
  }
  if (!print_plan(session, label)) {
    return false;
  }
  line("dispatched requests: " + number(outcome.value().requests_dispatched) +
       " accepted: " + number(outcome.value().accepted) +
       " refused: " + number(outcome.value().refused) +
       " failed: " + number(outcome.value().failed) +
       " indeterminate: " + number(outcome.value().indeterminate));
  for (const auto id : outcome.value().dispatched_requests) {
    auto request = session.runtime->request(id);
    if (request.ok()) {
      line("  dispatched " + pfm::render_request(request.value()));
    }
  }
  print_transitions(session);

  // Evidence the plant exhibits for the elements it was asked to affect. The
  // runtime decides whether that evidence proves the requested effect; PFM
  // never asserts that it happened.
  auto requests = session.runtime->requests();
  if (requests.ok()) {
    for (const auto& request : requests.value()) {
      if (!pfm::request_state_is_open(request.state)) {
        continue;
      }
      const auto evidence = session.plant->observe_element(request.target.element, now);
      if (!evidence.element.is_set()) {
        continue;
      }
      auto fresh = session.token();
      if (!fresh.ok()) {
        return refused(label, fresh.status());
      }
      auto recorded = session.runtime->record_effect(request.id, evidence, fresh.value());
      // A request that already carries this evidence is refused by the request
      // state machine rather than recorded twice; that is not a failure here.
      if (!recorded.ok() && recorded.code() != pfm::StatusCode::RequestStateConflict) {
        return refused(label, recorded.status());
      }
      auto updated = session.runtime->request(request.id);
      if (updated.ok()) {
        line("  effect evidence for request#" + number(request.id.value()) + ": " +
             pfm::render_request(updated.value()));
      }
    }
  }
  print_transitions(session);
  return true;
}

int command_demo(const Arguments& args) {
  const bool temporary = args.store.empty();
  const std::string directory = temporary ? unique_directory("demo") : args.store;
  const bool remove_at_end = temporary && !args.keep;

  line("pfmctl demo: synthetic feed-loss lifecycle against a durable store");
  line("  facility model: SYNTHETIC (in-process plant; no hardware, no BMS, no DCIM)");
  line("  persistence: REAL (durable two-slot store: encode, stage, flush, read back, replace)");
  line("  store: " + directory + (remove_at_end ? " (temporary, removed at exit)" : ""));

  std::error_code code;
  std::filesystem::create_directories(directory, code);
  if (code) {
    diagnostics("demo: cannot create the store directory: " + directory);
    return kExitFailure;
  }

  pfm::SyntheticPlantConfig config;
  config.site = "site-a";
  config.bus_fanout = 2;
  config.switchgear_fanout = 2;
  pfm::SyntheticPlant plant{config};

  DemoSession session;
  session.plant = &plant;
  session.clock = std::make_shared<pfm::ManualClock>(
      pfm::timestamp_from_unix_millis(kStartInstantMillis));
  auto transport = std::shared_ptr<pfm::ResponseTransport>{
      &plant, [](pfm::ResponseTransport*) {}};

  pfm::RuntimeOptions options;
  options.mode = pfm::StoreMode::Durable;
  options.store_directory = directory;
  options.policy = pfm::default_policy();
  options.policy.generation = pfm::PolicyGeneration::from_value(1);
  options.epoch = pfm::ControlEpoch::from_value(1);
  options.incarnation = pfm::ControllerIncarnation::from_value(1);

  auto opened = pfm::PowerFailureRuntime::open(options, session.clock, transport);
  if (!opened.ok()) {
    diagnostics("demo: open: " + opened.status().to_string());
    return kExitFailure;
  }
  session.runtime = std::move(opened.value());

  auto feed = plant.element_ref(pfm::RefKind::UtilityFeed, "feed-0");
  auto bus = plant.element_ref(pfm::RefKind::Bus, "bus-0-0");
  auto protected_group = plant.element_ref(pfm::RefKind::LoadGroup, "load-0-0-0");
  if (!feed.ok() || !bus.ok() || !protected_group.ok()) {
    diagnostics("demo: the synthetic plant did not build the expected topology");
    return kExitFailure;
  }

  bool ok = true;

  ok = ok && with_token(*session.runtime, "publish_topology",
                        [&](const pfm::AuthorityToken& token) {
                          return session.runtime->publish_topology(plant.topology(), token);
                        });

  // The obligation is owned by its authority; PFM holds the reference and the
  // rules that use it. The report is refreshed below whenever it would
  // otherwise age out, which is what a reporting authority does in reality.
  const auto make_obligation = [&]() {
    pfm::ProtectedObligation obligation;
    obligation.id = pfm::ObligationId::from_value(1);
    obligation.target = protected_group.value();
    obligation.protection = pfm::ProtectionClass::ServiceLevel;
    obligation.status = pfm::ObligationStatus::Satisfied;
    obligation.has_report = true;
    obligation.reported_at = session.clock->now().value();
    auto authority = pfm::RefToken::make(pfm::RefKind::Controller, "facility-obligations");
    if (authority.ok()) {
      obligation.reporting_authority = authority.value();
    }
    obligation.has_reserve_floor = true;
    obligation.reserve_floor = pfm::milli_percent_from_value(50000);
    obligation.policy_generation = pfm::PolicyGeneration::from_value(1);
    return obligation;
  };
  const pfm::ProtectedObligation obligation = make_obligation();
  auto protected_ref = pfm::RefToken::make(pfm::RefKind::Controller, "facility-obligations");
  line("obligation #1 protects " + obligation.target.to_string() + " (reporting authority " +
       (protected_ref.ok() ? protected_ref.value().to_string() : std::string{"unset"}) + ")");
  ok = ok && with_token(*session.runtime, "publish_obligations",
                        [&](const pfm::AuthorityToken& token) {
                          return session.runtime->publish_obligations({obligation}, token);
                        });

  {
    auto token = session.token();
    if (!token.ok()) {
      diagnostics("demo: current_authority: " + token.status().to_string());
      ok = false;
    } else {
      token.value().incident = pfm::IncidentId::from_value(1);
      token.value().generation = pfm::IncidentGeneration::from_value(1);
      auto incident = session.runtime->open_incident(token.value());
      if (!incident.ok()) {
        ok = refused("open_incident", incident.status());
      } else {
        line("");
        line("incident #1 generation 1 is open at t=" +
             number(static_cast<std::uint64_t>(session.clock->now().value().value())));
      }
    }
  }
  if (ok) {
    print_transitions(session);
  }

  // A healthy baseline establishes the plan and starts the stability dwell.
  ok = ok && step(session, pfm::duration_from_seconds(5), "healthy baseline");

  // The utility feed is lost: the plant de-energizes everything downstream.
  line("");
  line("fault: utility feed " + feed.value().to_string() + " is lost");
  plant.lose_utility(feed.value());
  ok = ok && step(session, pfm::duration_from_seconds(5), "utility feed loss");
  for (int i = 0; i < 3 && ok; ++i) {
    ok = step(session, pfm::duration_from_seconds(5), "isolation and transfer");
  }

  // The feed returns. The isolation the plan requested is still open, so the
  // restoration has to be requested and verified before recovery is eligible.
  line("");
  line("repair: utility feed " + feed.value().to_string() + " is restored");
  plant.restore_utility(feed.value());

  // The reporting authority keeps its report current: a report that ages out is
  // not a report, and recovery would then be blocked on an obligation nobody is
  // currently reporting rather than on the electrical facts.
  pfm::Timestamp last_report = obligation.reported_at;
  const auto refresh_obligation = [&]() -> bool {
    const pfm::Timestamp now = session.clock->now().value();
    if (now.value() - last_report.value() < options.policy.evidence_freshness_window.value()) {
      return true;
    }
    const pfm::ProtectedObligation refreshed = make_obligation();
    const bool published =
        with_token(*session.runtime, "publish_obligations (refresh)",
                   [&](const pfm::AuthorityToken& token) {
                     return session.runtime->publish_obligations({refreshed}, token);
                   });
    if (published) {
      last_report = refreshed.reported_at;
      line("obligation report refreshed at t=" +
           number(static_cast<std::uint64_t>(refreshed.reported_at.value())));
      print_transitions(session);
    }
    return published;
  };

  // Operator authorization is itself a gate, so the loop waits until every
  // other gate is satisfied by current evidence and reports what still blocks.
  bool facts_settled = false;
  for (int i = 0; i < 20 && ok && !facts_settled; ++i) {
    ok = refresh_obligation();
    if (!ok) {
      break;
    }
    ok = step(session, pfm::duration_from_seconds(5), "restoration and stabilization");
    if (!ok) {
      break;
    }
    auto assessment = session.runtime->recovery();
    if (!assessment.ok()) {
      ok = refused("recovery", assessment.status());
      break;
    }
    facts_settled = true;
    for (const auto& gate : assessment.value().gates) {
      if (!gate.satisfied && gate.gate != pfm::RecoveryGate::OperatorAuthorization) {
        facts_settled = false;
      }
    }
  }

  line("");
  line("recovery assessment:");
  auto assessment = session.runtime->recovery();
  if (!assessment.ok()) {
    ok = refused("recovery", assessment.status());
  } else {
    write_out(pfm::render_recovery(assessment.value()));
  }
  if (!facts_settled || !ok) {
    diagnostics("demo: the recovery gates were never satisfied by current evidence");
    print_request_states(session, "requests");
    if (remove_at_end) {
      remove_directory(directory);
    }
    return kExitFailure;
  }

  ok = ok && with_token(*session.runtime, "authorize_recovery",
                        [&](const pfm::AuthorityToken& token) {
                          return session.runtime->authorize_recovery(token);
                        });
  if (ok) {
    line("recovery authorized by the operator for incident generation 1");
    print_transitions(session);
  }

  line("");
  line("recovery assessment after authorization:");
  bool eligible = false;
  auto authorized_assessment = session.runtime->recovery();
  if (!authorized_assessment.ok()) {
    ok = refused("recovery", authorized_assessment.status());
  } else {
    eligible = authorized_assessment.value().eligible;
    write_out(pfm::render_recovery(authorized_assessment.value()));
  }
  if (!ok || !eligible) {
    diagnostics("demo: recovery did not become eligible after operator authorization");
    if (remove_at_end) {
      remove_directory(directory);
    }
    return kExitFailure;
  }

  ok = ok && with_token(*session.runtime, "begin_recovery",
                        [&](const pfm::AuthorityToken& token) {
                          return session.runtime->begin_recovery(token);
                        });
  if (ok) {
    line("recovery begun: the incident is Recovering");
    print_transitions(session);
  }
  ok = ok && with_token(*session.runtime, "complete_recovery",
                        [&](const pfm::AuthorityToken& token) {
                          return session.runtime->complete_recovery(token);
                        });
  if (ok) {
    line("recovery completed: the incident is Recovered");
    print_transitions(session);
  }

  line("");
  line("final recovery assessment:");
  auto final_assessment = session.runtime->recovery();
  if (final_assessment.ok()) {
    write_out(pfm::render_recovery(final_assessment.value()));
  }
  print_request_states(session, "requests");

  auto status = session.runtime->status();
  bool recovered = false;
  if (!status.ok()) {
    ok = refused("status", status.status());
  } else {
    recovered = status.value().lifecycle == pfm::IncidentLifecycle::Recovered ||
                status.value().lifecycle == pfm::IncidentLifecycle::Closed;
    line("lifecycle: " + std::string{pfm::incident_lifecycle_name(status.value().lifecycle)});
    line("revision: " + number(status.value().revision.value()) + " commits: " +
         number(status.value().commits) + " journal entries: " +
         number(status.value().journal_entries));
  }
  auto store_status = session.runtime->store_status();
  if (store_status.ok()) {
    write_out(pfm::render_store_status(store_status.value()));
  }

  auto closed = session.runtime->shutdown();
  if (!closed.ok()) {
    ok = refused("shutdown", closed.status());
  }
  session.runtime.reset();
  if (remove_at_end) {
    remove_directory(directory);
  } else {
    line("store kept at: " + directory);
  }

  if (!ok || !recovered) {
    diagnostics("demo: the lifecycle did not reach Recovered");
    return kExitFailure;
  }
  line("demo: ok");
  return kExitOk;
}

// --- inspect ---------------------------------------------------------------

pfm::Result<std::unique_ptr<pfm::DurableStore>> open_store(const std::string& directory) {
  pfm::StoreOptions options;
  options.directory = directory;
  options.mode = pfm::StoreMode::Durable;
  options.bounds = pfm::Bounds{};
  return pfm::DurableStore::open(options);
}

int command_inspect(const Arguments& args) {
  if (args.store.empty()) {
    diagnostics("inspect requires --store DIR");
    usage();
    return kExitUsage;
  }
  if (!directory_exists(args.store)) {
    diagnostics("inspect: no store directory at " + args.store);
    return kExitFailure;
  }
  auto store = open_store(args.store);
  if (!store.ok()) {
    refused("inspect: open", store.status());
    return kExitFailure;
  }
  // The store is opened for reading only: nothing below ever commits.
  auto snapshot = store.value()->load();
  line("pfmctl inspect: read-only view of a durable store");
  write_out(pfm::render_store_status(store.value()->status()));
  if (!snapshot.ok()) {
    diagnostics("inspect: no loadable generation: " + snapshot.status().to_string());
    auto closed = store.value()->close();
    if (!closed.ok()) {
      diagnostics("inspect: close: " + closed.status().to_string());
    }
    return kExitFailure;
  }
  write_out(pfm::render_state(snapshot.value().live));
  line("checkpoint: observations=" + number(snapshot.value().checkpoint.observations.size()) +
       " requests=" + number(snapshot.value().checkpoint.requests.size()) +
       " revision=" + number(snapshot.value().checkpoint.revision.value()));
  line("journal: entries=" + number(snapshot.value().journal.size()) +
       " retired=" + number(snapshot.value().retired_journal_entries));
  auto closed = store.value()->close();
  if (!closed.ok()) {
    diagnostics("inspect: close: " + closed.status().to_string());
    return kExitFailure;
  }
  return kExitOk;
}

// --- verify ----------------------------------------------------------------

int command_verify(const Arguments& args) {
  if (args.store.empty()) {
    diagnostics("verify requires --store DIR");
    usage();
    return kExitUsage;
  }
  if (!directory_exists(args.store)) {
    diagnostics("verify: no store directory at " + args.store);
    return kExitFailure;
  }
  auto store = open_store(args.store);
  if (!store.ok()) {
    refused("verify: open", store.status());
    return kExitFailure;
  }
  auto snapshot = store.value()->load();
  if (!snapshot.ok()) {
    diagnostics("verify: the store refused to load: " + snapshot.status().to_string());
    write_err(pfm::render_store_status(store.value()->status()));
    auto closed = store.value()->close();
    if (!closed.ok()) {
      diagnostics("verify: close: " + closed.status().to_string());
    }
    return kExitFailure;
  }

  // The proof: folding the retained journal onto the persisted checkpoint must
  // reproduce the persisted live state byte for byte, in the canonical
  // encoding. A mismatch is reported, never repaired.
  pfm::DomainState folded = snapshot.value().checkpoint;
  std::size_t applied = 0;
  for (const auto& entry : snapshot.value().journal) {
    auto result = pfm::apply_journal_entry(folded, entry);
    if (!result.ok()) {
      diagnostics("verify: replay refused after " + number(applied) + " entries at sequence " +
                  number(entry.sequence.value()) + ": " + result.status().to_string());
      auto closed = store.value()->close();
      if (!closed.ok()) {
        diagnostics("verify: close: " + closed.status().to_string());
      }
      return kExitFailure;
    }
    applied += 1;
  }

  const std::vector<std::uint8_t> replayed = pfm::encode_state(folded);
  const std::vector<std::uint8_t> live = pfm::encode_state(snapshot.value().live);
  const bool identical = replayed == live;
  const auto status = store.value()->status();
  auto closed = store.value()->close();
  const bool closed_ok = closed.ok();
  if (!closed_ok) {
    diagnostics("verify: close: " + closed.status().to_string());
  }

  if (!identical) {
    diagnostics("verify: replay diverged: checkpoint+journal re-encodes to " +
                number(replayed.size()) + " bytes but the live state is " + number(live.size()) +
                " bytes");
    return kExitFailure;
  }
  line("replay: ok");
  line("  directory: " + status.directory);
  line("  commit_sequence: " + number(status.commit_sequence.value()) + " store_generation: " +
       number(status.generation.value()) + " commits: " + number(status.commits));
  line("  checkpoint revision: " + number(snapshot.value().checkpoint.revision.value()) +
       " live revision: " + number(snapshot.value().live.revision.value()));
  line("  journal entries replayed: " + number(applied) + " retired: " +
       number(snapshot.value().retired_journal_entries));
  line("  replayed bytes: " + number(replayed.size()) + " live bytes: " + number(live.size()));
  return closed_ok ? kExitOk : kExitFailure;
}

// --- argument handling -----------------------------------------------------

bool parse_arguments(int argc, char** argv, Arguments& args) {
  if (argc < 2) {
    diagnostics("no command was given");
    usage();
    return false;
  }
  args.command = argv[1];
  for (int index = 2; index < argc; ++index) {
    const std::string_view argument = argv[index];
    if (argument == "--store" || argument == "--name") {
      if (index + 1 >= argc) {
        diagnostics(std::string{argument} + " requires a value");
        usage();
        return false;
      }
      const std::string value = argv[++index];
      if (argument == "--store") {
        args.store = value;
      } else {
        args.name = value;
      }
    } else if (argument == "--all") {
      args.all = true;
    } else if (argument == "--keep") {
      args.keep = true;
    } else if (argument == "--help" || argument == "-h") {
      args.help = true;
    } else {
      diagnostics("unknown option: " + std::string{argument});
      usage();
      return false;
    }
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Arguments args;
  if (!parse_arguments(argc, argv, args)) {
    return kExitUsage;
  }
  if (args.help) {
    usage();
    return kExitOk;
  }

  if (args.command == "version") {
    return command_version();
  }
  if (args.command == "boundaries") {
    return command_boundaries();
  }
  if (args.command == "scenarios") {
    return command_scenarios();
  }
  if (args.command == "scenario") {
    return command_scenario(args);
  }
  if (args.command == "demo") {
    return command_demo(args);
  }
  if (args.command == "inspect") {
    return command_inspect(args);
  }
  if (args.command == "verify") {
    return command_verify(args);
  }

  diagnostics("unknown command: " + args.command);
  usage();
  return kExitUsage;
}
