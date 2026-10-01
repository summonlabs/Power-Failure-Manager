// Power Failure Manager -- electrical failure walkthrough.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// A complete, readable walkthrough of one electrical incident, using nothing
// but the public pfm API:
//
//   topology and a protected obligation -> incident open -> utility feed loss
//   -> classification and bounded requests -> acknowledgements and effect
//   evidence -> feed restoration and isolation closure -> authorized recovery
//   -> incident close -> reopen from a fresh runtime and read the replay back.
//
// The facility model is SYNTHETIC: an in-process plant that answers bounded
// requests the way a scripted adjacent controller would and produces the
// evidence the runtime classifies. Persistence is REAL: every mutation goes
// through the durable two-slot store.
//
// The program prints "example: ok" and returns 0 only when every step
// succeeded, including the byte-for-byte replay check of the reopened store.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "pfm/clock.hpp"
#include "pfm/report.hpp"
#include "pfm/runtime.hpp"
#include "pfm/scenario.hpp"
#include "pfm/state.hpp"
#include "pfm/store.hpp"

namespace pfm = summon::pfm;

namespace {

constexpr std::int64_t kStartInstantMillis = 1767225600000LL;
constexpr int kExitOk = 0;
constexpr int kExitFailure = 1;

void say(const std::string& text) {
  std::fputs(text.c_str(), stdout);
  std::fputs("\n", stdout);
}

void fail(std::string_view step, const pfm::Status& status) {
  std::fputs("example: failed at ", stderr);
  std::fputs(std::string{step}.c_str(), stderr);
  std::fputs(": ", stderr);
  std::fputs(status.to_string().c_str(), stderr);
  std::fputs("\n", stderr);
}

std::string count(std::uint64_t value) { return std::to_string(value); }

// The walkthrough keeps one runtime over one durable store, plus the cursor
// that makes each durable transition print exactly once.
struct Walkthrough {
  pfm::SyntheticPlant* plant{nullptr};
  std::unique_ptr<pfm::PowerFailureRuntime> runtime{};
  std::shared_ptr<pfm::ManualClock> clock{};
  std::string directory{};
  pfm::IncidentId incident{};
  pfm::IncidentGeneration generation{};
  std::size_t printed_transitions{0};
  std::size_t steps{0};
  bool ok{true};

  [[nodiscard]] pfm::Result<pfm::AuthorityToken> token() const {
    return runtime->current_authority();
  }

  [[nodiscard]] pfm::Timestamp now() const { return clock->now().value(); }

  void report_transitions() {
    auto transitions = runtime->transitions();
    if (!transitions.ok()) {
      fail("transitions", transitions.status());
      ok = false;
      return;
    }
    const auto& all = transitions.value();
    for (std::size_t index = printed_transitions; index < all.size(); ++index) {
      const auto& record = all[index];
      say("    transition #" + count(record.sequence.value()) + " " +
          std::string{pfm::journal_kind_name(record.kind)} + " revision " +
          count(record.from_revision.value()) + "->" + count(record.to_revision.value()));
    }
    printed_transitions = all.size();
  }

  // One authority-bearing call with a freshly read token: every mutation
  // advances the revision, so a token is never reused across calls.
  template <class Call>
  bool call(std::string_view what, Call&& body) {
    if (!ok) {
      return false;
    }
    auto current = token();
    if (!current.ok()) {
      fail(what, current.status());
      ok = false;
      return false;
    }
    auto result = body(current.value());
    if (!result.ok()) {
      fail(what, result.status());
      ok = false;
      return false;
    }
    report_transitions();
    return true;
  }
};

// Advances time, admits the evidence the plant produces, evaluates, and then
// feeds back the evidence the plant actually exhibits for every open request.
bool evaluate(Walkthrough& walk, pfm::Duration advance, const std::string& label,
              bool verbose) {
  if (!walk.ok) {
    return false;
  }
  const auto advanced = walk.clock->advance(advance);
  if (!advanced.ok()) {
    fail(label, advanced.status());
    walk.ok = false;
    return false;
  }
  const pfm::Timestamp now = walk.now();
  walk.steps += 1;

  auto token = walk.token();
  if (!token.ok()) {
    fail(label, token.status());
    walk.ok = false;
    return false;
  }
  const auto observations = walk.plant->observe(now);
  auto admitted = walk.runtime->admit_evidence(observations, token.value());
  if (!admitted.ok()) {
    fail(label, admitted.status());
    walk.ok = false;
    return false;
  }
  token = walk.token();
  auto outcome = walk.runtime->evaluate(token.value());
  if (!outcome.ok()) {
    fail(label, outcome.status());
    walk.ok = false;
    return false;
  }

  auto status = walk.runtime->status();
  if (!status.ok()) {
    fail(label, status.status());
    walk.ok = false;
    return false;
  }
  say("  [step " + count(walk.steps) + "] " + label + " at t=" +
      count(static_cast<std::uint64_t>(now.value())) + " -> lifecycle=" +
      std::string{pfm::incident_lifecycle_name(status.value().lifecycle)} + " failures=" +
      count(status.value().failures) + " requests=" + count(status.value().requests) +
      " open=" + count(status.value().open_requests) + " verified=" +
      count(status.value().verified_requests));

  if (verbose) {
    auto classification = walk.runtime->classification();
    if (classification.ok()) {
      say("  classification:");
      std::fputs(("    " + pfm::render_classification(classification.value())).c_str(), stdout);
    }
    auto plan = walk.runtime->plan();
    if (plan.ok()) {
      say("  plan:");
      std::fputs(("    " + pfm::render_plan(plan.value())).c_str(), stdout);
    }
  }
  if (outcome.value().requests_dispatched > 0) {
    // The owning controller answered, so the runtime has already journaled the
    // acknowledgement: each line below is durable state, not a local note. An
    // acknowledgement is never proof of the effect, which is why the effect
    // evidence is recorded separately underneath.
    say("  bounded requests dispatched and acknowledged: " +
        count(outcome.value().requests_dispatched));
    for (const auto id : outcome.value().dispatched_requests) {
      auto request = walk.runtime->request(id);
      if (request.ok()) {
        say("    " + pfm::render_request(request.value()));
      }
    }
  }
  walk.report_transitions();

  auto requests = walk.runtime->requests();
  if (!requests.ok()) {
    fail(label, requests.status());
    walk.ok = false;
    return false;
  }
  for (const auto& request : requests.value()) {
    if (!pfm::request_state_is_open(request.state)) {
      continue;
    }
    const auto evidence = walk.plant->observe_element(request.target.element, now);
    if (!evidence.element.is_set()) {
      continue;
    }
    auto fresh = walk.token();
    if (!fresh.ok()) {
      fail(label, fresh.status());
      walk.ok = false;
      return false;
    }
    auto recorded = walk.runtime->record_effect(request.id, evidence, fresh.value());
    // A request that already carries this evidence is refused by the request
    // state machine rather than recorded twice; that is not a failure here.
    if (!recorded.ok() && recorded.code() != pfm::StatusCode::RequestStateConflict) {
      fail(label, recorded.status());
      walk.ok = false;
      return false;
    }
    auto updated = walk.runtime->request(request.id);
    if (updated.ok() && updated.value().state != request.state) {
      say("    effect evidence: " + pfm::render_request(updated.value()));
    }
  }
  walk.report_transitions();
  return true;
}

std::string temporary_directory() {
  std::error_code code;
  std::filesystem::path base = std::filesystem::temp_directory_path(code);
  if (code) {
    base = std::filesystem::current_path(code);
  }
  return (base / "pfm-example-electrical-failure").string();
}

}  // namespace

int main() {
  const std::string directory = temporary_directory();
  {
    std::error_code code;
    std::filesystem::remove_all(directory, code);
    std::filesystem::create_directories(directory, code);
    if (code) {
      std::fputs("example: the temporary store directory could not be created\n", stderr);
      return kExitFailure;
    }
  }

  say("electrical failure walkthrough");
  say("  facility model: SYNTHETIC (in-process plant; no hardware is contacted)");
  say("  persistence: REAL (durable store at " + directory + ")");

  pfm::SyntheticPlantConfig config;
  config.site = "site-a";
  config.bus_fanout = 2;
  config.switchgear_fanout = 2;
  pfm::SyntheticPlant plant{config};

  auto feed = plant.element_ref(pfm::RefKind::UtilityFeed, "feed-0");
  auto bus = plant.element_ref(pfm::RefKind::Bus, "bus-0-0");
  auto breaker = plant.element_ref(pfm::RefKind::Breaker, "brk-bus-0-0");
  auto protected_group = plant.element_ref(pfm::RefKind::LoadGroup, "load-0-0-0");
  if (!feed.ok() || !bus.ok() || !breaker.ok() || !protected_group.ok()) {
    std::fputs("example: the synthetic plant did not build the expected topology\n", stderr);
    return kExitFailure;
  }

  Walkthrough walk;
  walk.plant = &plant;
  walk.directory = directory;
  walk.clock = std::make_shared<pfm::ManualClock>(
      pfm::timestamp_from_unix_millis(kStartInstantMillis));
  walk.incident = pfm::IncidentId::from_value(1);
  walk.generation = pfm::IncidentGeneration::from_value(1);

  auto transport = std::shared_ptr<pfm::ResponseTransport>{
      &plant, [](pfm::ResponseTransport*) {}};
  pfm::RuntimeOptions options;
  options.mode = pfm::StoreMode::Durable;
  options.store_directory = directory;
  options.policy = pfm::default_policy();
  options.policy.generation = pfm::PolicyGeneration::from_value(1);
  options.epoch = pfm::ControlEpoch::from_value(1);
  options.incarnation = pfm::ControllerIncarnation::from_value(1);

  auto opened = pfm::PowerFailureRuntime::open(options, walk.clock, transport);
  if (!opened.ok()) {
    fail("open runtime", opened.status());
    return kExitFailure;
  }
  walk.runtime = std::move(opened.value());

  // --- topology -----------------------------------------------------------
  say("");
  say("1. publish the electrical topology supplied by its authority");
  walk.call("publish_topology", [&](const pfm::AuthorityToken& token) {
    return walk.runtime->publish_topology(plant.topology(), token);
  });
  say("  topology generation " + count(plant.topology().generation.value()) + " with " +
      count(plant.topology().elements.size()) + " elements and " +
      count(plant.topology().domains.size()) + " failure domains");

  // --- protected obligation ----------------------------------------------
  say("");
  say("2. publish a protected obligation owned by another authority");
  const auto make_obligation = [&]() {
    pfm::ProtectedObligation obligation;
    obligation.id = pfm::ObligationId::from_value(1);
    obligation.target = protected_group.value();
    obligation.protection = pfm::ProtectionClass::ServiceLevel;
    obligation.status = pfm::ObligationStatus::Satisfied;
    obligation.has_report = true;
    obligation.reported_at = walk.now();
    auto authority = pfm::RefToken::make(pfm::RefKind::Controller, "facility-obligations");
    if (authority.ok()) {
      obligation.reporting_authority = authority.value();
    }
    obligation.has_reserve_floor = true;
    obligation.reserve_floor = pfm::milli_percent_from_value(50000);
    obligation.policy_generation = pfm::PolicyGeneration::from_value(1);
    return obligation;
  };
  walk.call("publish_obligations", [&](const pfm::AuthorityToken& token) {
    return walk.runtime->publish_obligations({make_obligation()}, token);
  });
  say("  obligation #1 protects " + protected_group.value().to_string() +
      " at class service-level");

  // --- incident -----------------------------------------------------------
  say("");
  say("3. open the incident under an explicit identity and generation");
  {
    auto token = walk.token();
    if (!token.ok()) {
      fail("open_incident", token.status());
      walk.ok = false;
    } else {
      token.value().incident = walk.incident;
      token.value().generation = walk.generation;
      auto result = walk.runtime->open_incident(token.value());
      if (!result.ok()) {
        fail("open_incident", result.status());
        walk.ok = false;
      } else {
        say("  incident #" + count(walk.incident.value()) + " generation " +
            count(walk.generation.value()) + " is open");
        walk.report_transitions();
      }
    }
  }

  // --- baseline -----------------------------------------------------------
  say("");
  say("4. evaluate the healthy baseline");
  evaluate(walk, pfm::duration_from_seconds(5), "healthy baseline", false);

  // --- fault --------------------------------------------------------------
  say("");
  say("5. the utility feed " + feed.value().to_string() + " is lost");
  plant.lose_utility(feed.value());
  evaluate(walk, pfm::duration_from_seconds(5), "utility feed loss", true);

  say("");
  say("6. the plant performs the requested effects; the runtime records the");
  say("   evidence the plant actually exhibits, never the acknowledgement alone");
  for (int index = 0; index < 3 && walk.ok; ++index) {
    evaluate(walk, pfm::duration_from_seconds(5), "isolation and transfer", false);
  }

  // --- restoration --------------------------------------------------------
  say("");
  say("7. the feed is restored and the isolation is closed again");
  plant.restore_utility(feed.value());
  walk.call("publish_obligations (refresh)", [&](const pfm::AuthorityToken& token) {
    return walk.runtime->publish_obligations({make_obligation()}, token);
  });

  // The reporting authority keeps its report current, and the operator
  // authorization is itself a gate: the loop waits until every other gate is
  // satisfied by current, independent evidence.
  pfm::Timestamp last_report = walk.now();
  bool facts_settled = false;
  for (int index = 0; index < 20 && walk.ok && !facts_settled; ++index) {
    if (walk.now().value() - last_report.value() >=
        walk.runtime->state().value().policy.evidence_freshness_window.value()) {
      const pfm::ProtectedObligation refreshed = make_obligation();
      walk.call("publish_obligations (refresh)", [&](const pfm::AuthorityToken& token) {
        return walk.runtime->publish_obligations({refreshed}, token);
      });
      last_report = refreshed.reported_at;
    }
    evaluate(walk, pfm::duration_from_seconds(5), "restoration", false);
    auto assessment = walk.runtime->recovery();
    if (!assessment.ok()) {
      fail("recovery", assessment.status());
      walk.ok = false;
      break;
    }
    facts_settled = true;
    for (const auto& gate : assessment.value().gates) {
      if (!gate.satisfied && gate.gate != pfm::RecoveryGate::OperatorAuthorization) {
        facts_settled = false;
      }
    }
  }
  say("  isolation point " + breaker.value().to_string() + " is " +
      std::string{pfm::breaker_position_name(
          plant.observe_element(breaker.value(), walk.now()).breaker)});

  // --- recovery -----------------------------------------------------------
  say("");
  say("8. recovery assessment: every gate is answered by current evidence");
  auto assessment = walk.runtime->recovery();
  if (!assessment.ok()) {
    fail("recovery", assessment.status());
    walk.ok = false;
  } else {
    std::fputs(("  " + pfm::render_recovery(assessment.value())).c_str(), stdout);
    if (!facts_settled) {
      say("  the electrical gates were never satisfied by current, independent");
      say("  evidence, so recovery may not even be considered");
      walk.ok = false;
    }
  }

  say("");
  say("9. the operator authorizes recovery for this incident generation");
  walk.call("authorize_recovery", [&](const pfm::AuthorityToken& token) {
    return walk.runtime->authorize_recovery(token);
  });
  say("  recovery is authorized; the authorization is a durable decision, not a");
  say("  substitute for any electrical gate");
  bool eligible = false;
  auto authorized = walk.runtime->recovery();
  if (!authorized.ok()) {
    fail("recovery", authorized.status());
    walk.ok = false;
  } else {
    eligible = authorized.value().eligible;
    std::fputs(("  " + pfm::render_recovery(authorized.value())).c_str(), stdout);
  }
  if (walk.ok && !eligible) {
    say("  recovery is still not eligible after authorization");
    walk.ok = false;
  }

  say("");
  say("10. begin and complete recovery");
  walk.call("begin_recovery", [&](const pfm::AuthorityToken& token) {
    return walk.runtime->begin_recovery(token);
  });
  walk.call("complete_recovery", [&](const pfm::AuthorityToken& token) {
    return walk.runtime->complete_recovery(token);
  });

  say("");
  say("11. close the incident");
  walk.call("close_incident", [&](const pfm::AuthorityToken& token) {
    return walk.runtime->close_incident(token);
  });

  auto final_status = walk.runtime->status();
  if (!final_status.ok()) {
    fail("status", final_status.status());
    walk.ok = false;
  } else {
    say("  incident #" + count(final_status.value().incident.value()) + " lifecycle=" +
        std::string{pfm::incident_lifecycle_name(final_status.value().lifecycle)} +
        " revision=" + count(final_status.value().revision.value()) +
        " commits=" + count(final_status.value().commits) +
        " journal entries=" + count(final_status.value().journal_entries));
    if (final_status.value().lifecycle != pfm::IncidentLifecycle::Closed) {
      say("  the incident did not reach the closed state");
      walk.ok = false;
    }
  }

  const pfm::StateRevision committed_revision =
      final_status.ok() ? final_status.value().revision : pfm::StateRevision{};

  // --- shutdown -----------------------------------------------------------
  say("");
  say("12. shut the runtime down and prove the durable record replays");
  auto closed = walk.runtime->shutdown();
  if (!closed.ok()) {
    fail("shutdown", closed.status());
    walk.ok = false;
  }
  walk.runtime.reset();

  pfm::StoreOptions store_options;
  store_options.directory = directory;
  store_options.mode = pfm::StoreMode::Durable;
  store_options.bounds = pfm::Bounds{};
  auto store = pfm::DurableStore::open(store_options);
  if (!store.ok()) {
    fail("open store", store.status());
    walk.ok = false;
  } else {
    auto snapshot = store.value()->load();
    if (!snapshot.ok()) {
      fail("load store", snapshot.status());
      walk.ok = false;
    } else {
      pfm::DomainState folded = snapshot.value().checkpoint;
      std::size_t replayed = 0;
      for (const auto& entry : snapshot.value().journal) {
        auto applied = pfm::apply_journal_entry(folded, entry);
        if (!applied.ok()) {
          fail("replay journal", applied.status());
          walk.ok = false;
          break;
        }
        replayed += 1;
      }
      const std::vector<std::uint8_t> replayed_bytes = pfm::encode_state(folded);
      const std::vector<std::uint8_t> live_bytes = pfm::encode_state(snapshot.value().live);
      say("  journal entries replayed: " + count(replayed) + ", retired: " +
          count(snapshot.value().retired_journal_entries));
      say("  checkpoint + journal re-encodes to " + count(replayed_bytes.size()) +
          " bytes; the stored live state is " + count(live_bytes.size()) + " bytes");
      if (!(replayed_bytes == live_bytes)) {
        say("  replay diverged: the durable record does not reproduce itself");
        walk.ok = false;
      } else {
        say("  replay: ok (byte for byte)");
      }
      std::fputs(("  " + pfm::render_store_status(store.value()->status())).c_str(), stdout);
    }
    auto store_closed = store.value()->close();
    if (!store_closed.ok()) {
      fail("close store", store_closed.status());
      walk.ok = false;
    }
  }

  // --- reopen -------------------------------------------------------------
  say("");
  say("13. reopen the store from a fresh runtime and read the state back");
  auto reopened = pfm::PowerFailureRuntime::open(options, walk.clock, transport);
  if (!reopened.ok()) {
    fail("reopen runtime", reopened.status());
    walk.ok = false;
  } else {
    auto replayed_state = reopened.value()->state();
    auto replayed_status = reopened.value()->status();
    if (!replayed_state.ok() || !replayed_status.ok()) {
      fail("read replayed state", replayed_state.ok() ? replayed_status.status()
                                                     : replayed_state.status());
      walk.ok = false;
    } else {
      const auto& state = replayed_state.value();
      std::size_t recovered_slots = 0;
      std::size_t present_slots = 0;
      for (const auto& slot : state.observations) {
        if (slot.present) {
          present_slots += 1;
        }
        if (slot.recovered) {
          recovered_slots += 1;
        }
      }
      say("  replayed state summary:");
      say("    epoch=" + count(state.epoch.value()) + " incarnation=" +
          count(state.incarnation.value()) + " revision=" + count(state.revision.value()) +
          " topology_generation=" + count(state.topology_generation.value()) +
          " policy_generation=" + count(state.policy_generation.value()));
      say("    incident=" + count(state.incident.incident.value()) + " generation=" +
          count(state.incident.generation.value()) + " lifecycle=" +
          std::string{pfm::incident_lifecycle_name(state.incident.lifecycle)} +
          " primary=" + std::string{pfm::failure_class_code(state.incident.primary_failure)});
      say("    observations=" + count(state.observations.size()) + " (present=" +
          count(present_slots) + ", marked recovered=" + count(recovered_slots) + ")");
      say("    requests=" + count(state.requests.size()) + " obligations=" +
          count(state.obligations.size()) + " transitions=" + count(state.transitions.size()) +
          " plan_retained=" + (state.has_plan ? std::string{"yes"} : std::string{"no"}));
      say("    commit_sequence=" + count(replayed_status.value().commit_sequence.value()) +
          " commits=" + count(replayed_status.value().commits));

      // The replayed record must agree with what was closed, and restored
      // evidence must be marked recovered rather than current: a restart
      // recovers authority, never evidence.
      if (state.incident.lifecycle != pfm::IncidentLifecycle::Closed) {
        say("  the reopened incident is not the closed one");
        walk.ok = false;
      }
      if (state.revision.value() < committed_revision.value()) {
        say("  the reopened revision is behind the committed revision");
        walk.ok = false;
      }
      if (recovered_slots != present_slots || present_slots == 0) {
        say("  restored evidence was not marked recovered");
        walk.ok = false;
      }
      if (state.has_plan) {
        say("  a plan survived the restart as current, which it must not");
        walk.ok = false;
      }
    }
    auto reclosed = reopened.value()->shutdown();
    if (!reclosed.ok()) {
      fail("shutdown reopened runtime", reclosed.status());
      walk.ok = false;
    }
  }

  // --- cleanup ------------------------------------------------------------
  {
    std::error_code code;
    std::filesystem::remove_all(directory, code);
    if (code) {
      say("  the temporary store directory could not be removed");
      walk.ok = false;
    }
  }

  if (!walk.ok) {
    say("example: failed");
    return kExitFailure;
  }
  say("");
  say("example: ok");
  return kExitOk;
}
