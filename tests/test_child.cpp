// Power Failure Manager -- helper process for the cross-process proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// This program is a test *helper*, not a test suite. Every subcommand is a
// real, independent operating-system process that process_tests.cpp starts,
// observes, and kills. It links the test framework only through nothing at
// all: it has its own main and it never unwinds on the paths that model a
// death, because those call std::_Exit so that no destructor, no flush, and no
// lock release runs.
//
// Subcommands (argv[1]), each returning 0 on success and non-zero on a failed
// expectation, printing short machine-readable lines:
//
//   hold-lock <dir>
//       Open a durable runtime on <dir> (the runtime owns the store, and the
//       store owns the OS single-writer lock). Print "locked" once the lock is
//       held, read ONE line from stdin with std::getline, then exit 0.
//
//   commit-loop <dir> <count>
//       Open a durable runtime and run <count> durable mutation/evaluation
//       cycles, printing one "cycle ..." progress line per cycle. The parent
//       uses those lines to choose the exact instant to kill this process.
//
//   stage-only <dir> <count>
//       Open the durable store directly, enable stage-only fault injection
//       before the first commit of this process, and run <count> commits that
//       write and flush a staging file and never publish it. Prints "staged"
//       and exits 0 *without unwinding*, so the flushed staging file survives
//       exactly as a killed writer would leave it.
//
//   dispatch-then-die <dir> <marker>
//       Drive a synthetic failure to a durably recorded, durably issued bounded
//       request, then die inside the transport's dispatch: the marker file is
//       written and flushed, the controller never answers, and std::_Exit(0)
//       runs with the dispatch in flight and the marker on disk.
//
//   reopen-and-report <dir> [--epoch N] [--incarnation N]
//       Reopen the existing durable runtime with the given identity (default
//       1/1, which is the identity the other subcommands use) and print one
//       line per retained request and one per observation slot.
//
//   verify <dir>
//       Open the store read-only through DurableStore and print the resolved
//       commit sequence, store generation, and whether the newest slot was
//       unusable and the previous generation was loaded instead.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "pfm/codec.hpp"
#include "pfm/runtime.hpp"
#include "pfm/scenario.hpp"
#include "pfm/state.hpp"
#include "pfm/store.hpp"
#include "pfm/transport.hpp"

namespace {

using namespace summon::pfm;

constexpr std::uint64_t kEpoch = 1;
constexpr std::uint64_t kIncarnation = 1;
constexpr std::int64_t kBaseUnixMillis = 1767225600000LL;  // 2026-01-01T00:00:00Z

// Exit codes. They are part of the machine contract the parent asserts on.
constexpr int kExitOk = 0;
constexpr int kExitExpectationFailed = 1;
constexpr int kExitUsage = 2;
constexpr int kExitOpenFailed = 3;
constexpr int kExitUnusableStore = 4;

void emit(const std::string& text) {
  std::printf("%s\n", text.c_str());
  std::fflush(stdout);
}

int fail(const std::string& detail) {
  emit("child-failure " + detail);
  return kExitExpectationFailed;
}

int open_failed(const std::string& what, const Status& status) {
  emit("open-failed what=" + what + " code=" + std::string{status.code_name()} +
       " message=" + std::string{status.message()});
  return kExitOpenFailed;
}

std::string code_of(const Status& status) { return std::string{status.code_name()}; }

std::string number(std::uint64_t value) { return std::to_string(value); }

std::shared_ptr<ManualClock> make_clock() {
  return std::make_shared<ManualClock>(timestamp_from_unix_millis(kBaseUnixMillis));
}

RuntimeOptions durable_options(const std::string& directory, std::uint64_t epoch,
                               std::uint64_t incarnation) {
  RuntimeOptions options;
  options.mode = StoreMode::Durable;
  options.store_directory = directory;
  options.policy = default_policy();
  options.policy.generation = PolicyGeneration::from_value(1);
  options.epoch = ControlEpoch::from_value(epoch);
  options.incarnation = ControllerIncarnation::from_value(incarnation);
  return options;
}

// The synthetic plant and the fault every process in this proof drives. The
// parent re-derives the same operations after the child dies, so the config and
// the injected fault have to be identical on both sides.
SyntheticPlantConfig plant_config() {
  SyntheticPlantConfig config;
  config.site = "site-a";
  return config;
}

Result<RefToken> first_utility_feed(const SyntheticPlant& plant) {
  const auto feeds = plant.elements_of_kind(ElementKind::UtilityFeed);
  if (feeds.empty()) {
    return Status::error(StatusCode::Internal, "the synthetic plant has no utility feed")
        .with_context("scenario.feed");
  }
  return feeds.front();
}

// Publishes the topology, opens incident 1/1, loses one utility feed, advances
// the clock by one second, and admits the resulting evidence. Identical in the
// child and in the parent, which is what makes the operation fingerprints
// reproducible after a death around dispatch.
Result<void> drive_to_failure(PowerFailureRuntime& runtime, SyntheticPlant& plant,
                              ManualClock& clock) {
  auto token = runtime.current_authority();
  if (!token.ok()) {
    return token.status();
  }
  if (auto published = runtime.publish_topology(plant.topology(), token.value());
      !published.ok()) {
    return published.status();
  }
  auto feed = first_utility_feed(plant);
  if (!feed.ok()) {
    return feed.status();
  }
  plant.lose_utility(feed.value());
  if (auto advanced = clock.advance(duration_from_seconds(1)); !advanced.ok()) {
    return advanced.status();
  }
  auto incident = runtime.current_authority();
  if (!incident.ok()) {
    return incident.status();
  }
  incident.value().incident = IncidentId::from_value(1);
  incident.value().generation = IncidentGeneration::from_value(1);
  if (auto opened = runtime.open_incident(incident.value()); !opened.ok()) {
    return opened.status();
  }
  auto current = runtime.current_authority();
  if (!current.ok()) {
    return current.status();
  }
  auto instant = clock.now();
  if (!instant.ok()) {
    return instant.status();
  }
  const auto observations = plant.observe(instant.value());
  if (auto admitted = runtime.admit_evidence(observations, current.value()); !admitted.ok()) {
    return admitted.status();
  }
  return {};
}

// One durable cycle: current evidence in, one decision, then external evidence
// for every open request, exactly as the shared fixture does.
Result<void> tick_once(PowerFailureRuntime& runtime, SyntheticPlant& plant, ManualClock& clock) {
  if (auto advanced = clock.advance(duration_from_seconds(1)); !advanced.ok()) {
    return advanced.status();
  }
  const Timestamp now = clock.now().value();
  auto token = runtime.current_authority();
  if (!token.ok()) {
    return token.status();
  }
  if (auto admitted = runtime.admit_evidence(plant.observe(now), token.value()); !admitted.ok()) {
    return admitted.status();
  }
  auto decided = runtime.current_authority();
  if (!decided.ok()) {
    return decided.status();
  }
  if (auto outcome = runtime.evaluate(decided.value()); !outcome.ok()) {
    return outcome.status();
  }
  auto requests = runtime.requests();
  if (!requests.ok()) {
    return requests.status();
  }
  for (const auto& request : requests.value()) {
    if (!request_state_is_open(request.state)) {
      continue;
    }
    const auto evidence = plant.observe_element(request.target.element, now);
    if (!evidence.element.is_set()) {
      continue;
    }
    auto fresh = runtime.current_authority();
    if (!fresh.ok()) {
      return fresh.status();
    }
    static_cast<void>(runtime.record_effect(request.id, evidence, fresh.value()));
  }
  return {};
}

// A transport that records the dispatch and then dies without answering. The
// record is flushed and closed before the death, so the parent can prove which
// idempotency key reached a controller and that it reached it exactly once.
class DispatchThenDieTransport final : public ResponseTransport {
 public:
  DispatchThenDieTransport(std::string marker, int exit_code)
      : marker_(std::move(marker)), exit_code_(exit_code) {}

  Result<DispatchOutcome> dispatch(const ResponseRequest& request) override {
    {
      std::ofstream record{marker_, std::ios::app};
      if (record) {
        record << "dispatch id=" << request.id.value() << " attempt=" << request.attempt.value()
               << " ordinal=" << request.attempt_ordinal
               << " key=" << request.idempotency.to_hex()
               << " fingerprint=" << request.fingerprint.to_hex()
               << " kind=" << request_kind_name(request.kind) << "\n";
        record.flush();
        record.close();
      }
    }
    // The controller never answers. The process dies with the dispatch in
    // flight: no unwinding, no runtime flush, no answer recorded anywhere.
    std::_Exit(exit_code_);
  }

 private:
  std::string marker_{};
  int exit_code_{0};
};

// --- subcommands -----------------------------------------------------------

int hold_lock(const std::string& directory) {
  auto clock = make_clock();
  auto opened = PowerFailureRuntime::open(durable_options(directory, kEpoch, kIncarnation), clock,
                                          std::shared_ptr<ResponseTransport>{});
  if (!opened.ok()) {
    return open_failed("hold-lock", opened.status());
  }
  emit("locked");
  // The parent releases this process by writing one line to its stdin. The
  // store is held for as long as this read blocks.
  std::string release;
  std::getline(std::cin, release);
  auto closed = opened.value()->shutdown();
  if (!closed.ok()) {
    return fail("shutdown refused: " + code_of(closed.status()));
  }
  emit("released");
  return kExitOk;
}

int commit_loop(const std::string& directory, std::uint32_t count) {
  auto clock = make_clock();
  auto plant = std::make_unique<SyntheticPlant>(plant_config());
  auto opened = PowerFailureRuntime::open(
      durable_options(directory, kEpoch, kIncarnation), clock,
      std::shared_ptr<ResponseTransport>{plant.get(), [](ResponseTransport*) {}});
  if (!opened.ok()) {
    return open_failed("commit-loop", opened.status());
  }
  auto& runtime = *opened.value();
  if (auto driven = drive_to_failure(runtime, *plant, *clock); !driven.ok()) {
    return fail("drive failed: " + code_of(driven.status()));
  }
  auto primed = runtime.status();
  if (!primed.ok()) {
    return fail("status refused: " + code_of(primed.status()));
  }
  emit("primed commit=" + number(primed.value().commit_sequence.value()) +
       " requests=" + number(primed.value().requests));
  for (std::uint32_t cycle = 1; cycle <= count; ++cycle) {
    if (auto ticked = tick_once(runtime, *plant, *clock); !ticked.ok()) {
      return fail("cycle " + number(cycle) + " failed: " + code_of(ticked.status()));
    }
    auto status = runtime.status();
    auto store = runtime.store_status();
    if (!status.ok() || !store.ok()) {
      return fail("cycle " + number(cycle) + " status refused");
    }
    emit("cycle " + number(cycle) + " commit=" + number(status.value().commit_sequence.value()) +
         " generation=" + number(store.value().generation.value()) +
         " revision=" + number(status.value().revision.value()) +
         " requests=" + number(status.value().requests) +
         " open=" + number(status.value().open_requests));
  }
  emit("commit-loop complete cycles=" + number(count));
  return kExitOk;
}

int stage_only(const std::string& directory, std::uint32_t count) {
  StoreOptions options;
  options.directory = directory;
  options.mode = StoreMode::Durable;
  auto store_result = DurableStore::open(options);
  if (!store_result.ok()) {
    return open_failed("stage-only store", store_result.status());
  }
  auto store = std::move(store_result.value());

  DomainState base;
  JournalSequence sequence = JournalSequence::from_value(1);
  std::uint64_t retired = 0;
  auto loaded = store->load();
  if (loaded.ok()) {
    base = loaded.value().live;
    retired = loaded.value().retired_journal_entries;
    sequence = loaded.value().journal.empty()
                   ? JournalSequence::from_value(1)
                   : JournalSequence::from_value(loaded.value().journal.back().sequence.value() + 1);
    emit("loaded commit=" + number(store->status().commit_sequence.value()) +
         " generation=" + number(store->status().generation.value()) + " fresh=0");
  } else if (loaded.status().code() == StatusCode::StoreNotFound ||
             loaded.status().code() == StatusCode::NoAuthoritativeGeneration) {
    // A fresh directory: the same identity a runtime would establish on its
    // first commit, so the staged generation is a complete, valid snapshot.
    base.epoch = ControlEpoch::from_value(kEpoch);
    base.incarnation = ControllerIncarnation::from_value(kIncarnation);
    base.revision = StateRevision::from_value(1);
    base.policy = default_policy();
    base.policy.generation = PolicyGeneration::from_value(1);
    base.bounds = options.bounds;
    emit("loaded commit=0 generation=0 fresh=1");
  } else {
    return fail("load refused: " + code_of(loaded.status()));
  }

  const std::uint64_t start_commit = store->status().commit_sequence.value();
  const std::uint64_t start_generation = store->status().generation.value();
  // Fault injection is enabled before the first commit of this process, so
  // every commit below writes, flushes, and reads back a staging file and then
  // stops short of the publication point.
  store->set_stage_only(true);
  emit("stage-only commit=" + number(start_commit) + " generation=" + number(start_generation));
  for (std::uint32_t cycle = 1; cycle <= count; ++cycle) {
    DomainState live = base;
    JournalEntry entry;
    entry.sequence = sequence;
    entry.kind = JournalKind::EpochRolled;
    entry.recorded_at = timestamp_from_unix_millis(kBaseUnixMillis + 1000LL * cycle);
    entry.revision = StateRevision::from_value(live.revision.value() + 1);
    entry.payload.epoch = live.epoch;
    entry.payload.incarnation = live.incarnation;
    entry.payload.epoch_rollover = false;
    entry.fingerprint = journal_entry_fingerprint(entry);
    if (auto applied = apply_journal_entry(live, entry); !applied.ok()) {
      return fail("staged entry refused: " + code_of(applied.status()));
    }
    StoreSnapshot snapshot;
    snapshot.checkpoint = live;
    snapshot.live = live;
    snapshot.retired_journal_entries = retired + cycle;
    if (auto committed = store->commit(snapshot); !committed.ok()) {
      return fail("staged commit refused: " + code_of(committed.status()));
    }
    base = std::move(live);
    sequence = JournalSequence::from_value(sequence.value() + 1);
    emit("staged cycle=" + number(cycle) + " intended_commit=" + number(start_commit + cycle) +
         " intended_generation=" + number(start_generation + cycle));
  }
  emit("staged");
  // Deliberately does not unwind: DurableStore::close() would delete the
  // staging files, and a leftover staging file is precisely what this process
  // exists to leave behind for a fresh process to interpret.
  std::_Exit(kExitOk);
  return kExitOk;
}

int dispatch_then_die(const std::string& directory, const std::string& marker) {
  std::remove(marker.c_str());
  auto clock = make_clock();
  auto plant = std::make_unique<SyntheticPlant>(plant_config());
  auto transport = std::make_shared<DispatchThenDieTransport>(marker, kExitOk);
  auto opened = PowerFailureRuntime::open(durable_options(directory, kEpoch, kIncarnation), clock,
                                          transport);
  if (!opened.ok()) {
    return open_failed("dispatch-then-die", opened.status());
  }
  auto& runtime = *opened.value();
  if (auto driven = drive_to_failure(runtime, *plant, *clock); !driven.ok()) {
    return fail("drive failed: " + code_of(driven.status()));
  }
  auto token = runtime.current_authority();
  if (!token.ok()) {
    return fail("authority refused: " + code_of(token.status()));
  }
  auto outcome = runtime.evaluate(token.value());
  if (!outcome.ok()) {
    return fail("evaluate refused: " + code_of(outcome.status()));
  }
  return fail("evaluate returned without ever dispatching a bounded request");
}

int reopen_and_report(const std::string& directory, std::uint64_t epoch,
                      std::uint64_t incarnation) {
  auto clock = make_clock();
  auto opened = PowerFailureRuntime::open(durable_options(directory, epoch, incarnation), clock,
                                          std::shared_ptr<ResponseTransport>{});
  if (!opened.ok()) {
    return open_failed("reopen-and-report", opened.status());
  }
  auto& runtime = *opened.value();
  auto status = runtime.status();
  if (!status.ok()) {
    return fail("status refused: " + code_of(status.status()));
  }
  auto state = runtime.state();
  if (!state.ok()) {
    return fail("state refused: " + code_of(state.status()));
  }
  emit("authority epoch=" + number(status.value().epoch.value()) +
       " incarnation=" + number(status.value().incarnation.value()) +
       " revision=" + number(status.value().revision.value()) +
       " commit=" + number(status.value().commit_sequence.value()) +
       " lifecycle=" + std::string{incident_lifecycle_name(status.value().lifecycle)});
  for (const auto& slot : state.value().requests) {
    emit("request " + number(slot.request.id.value()) +
         " state=" + std::string{request_state_name(slot.request.state)} +
         " idempotency=" + slot.request.idempotency.to_hex());
  }
  for (const auto& slot : state.value().observations) {
    emit("observation " + slot.element.to_string() +
         " recovered=" + (slot.recovered ? "1" : "0"));
  }
  emit("recovery authorized=" + std::string{state.value().incident.operator_authorized ? "1" : "0"} +
       " stable=" + std::string{state.value().incident.has_stable_since ? "1" : "0"} +
       " steps=" + number(state.value().incident.recovery_steps) +
       " has_plan=" + std::string{state.value().has_plan ? "1" : "0"} +
       " records=" + number(state.value().requests.size()) +
       " slots=" + number(state.value().observations.size()));
  auto closed = runtime.shutdown();
  if (!closed.ok()) {
    return fail("shutdown refused: " + code_of(closed.status()));
  }
  return kExitOk;
}

std::string hex32(std::uint32_t value) {
  char buffer[16] = {};
  std::snprintf(buffer, sizeof(buffer), "%08x", value);
  return std::string{buffer};
}

int verify(const std::string& directory) {
  StoreOptions options;
  options.directory = directory;
  options.mode = StoreMode::Durable;
  auto store_result = DurableStore::open(options);
  if (!store_result.ok()) {
    return open_failed("verify store", store_result.status());
  }
  auto store = std::move(store_result.value());
  emit("store directory=" + store->status().directory);
  auto loaded = store->load();
  const auto& status = store->status();
  for (std::size_t index = 0; index < status.slots.size(); ++index) {
    const auto& slot = status.slots[index];
    emit("slot " + number(index) + " valid=" + (slot.valid ? "1" : "0") +
         " commit=" + number(slot.commit_sequence.value()) +
         " generation=" + number(slot.generation.value()));
  }
  if (!loaded.ok()) {
    emit("resolved commit=" + number(status.commit_sequence.value()) +
         " generation=" + number(status.generation.value()) +
         " fell_back=" + (status.fell_back ? "1" : "0") + " has_state=0 code=" +
         code_of(loaded.status()) + " detail=[" + std::string{loaded.status().message()} + "]");
    const auto code = loaded.status().code();
    if (code == StatusCode::StoreNotFound || code == StatusCode::NoAuthoritativeGeneration) {
      return kExitOk;
    }
    return kExitUnusableStore;
  }
  const auto digest = codec::crc32c(encode_state(loaded.value().live));
  emit("resolved commit=" + number(status.commit_sequence.value()) +
       " generation=" + number(status.generation.value()) +
       " fell_back=" + (status.fell_back ? "1" : "0") +
       " has_state=1 code=pfm.ok digest=" + hex32(digest) +
       " revision=" + number(loaded.value().live.revision.value()) +
       " requests=" + number(loaded.value().live.requests.size()) +
       " observations=" + number(loaded.value().live.observations.size()));
  return kExitOk;
}
std::uint64_t parse_u64(const char* text, bool& ok) {
  ok = false;
  if (text == nullptr || *text == '\0') {
    return 0;
  }
  std::uint64_t value = 0;
  for (const char* cursor = text; *cursor != '\0'; ++cursor) {
    if (*cursor < '0' || *cursor > '9') {
      return 0;
    }
    value = value * 10u + static_cast<std::uint64_t>(*cursor - '0');
  }
  ok = true;
  return value;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    emit("usage: test_child <subcommand> [args]");
    return kExitUsage;
  }
  const std::string command{argv[1]};

  if (command == "hold-lock") {
    if (argc < 3) {
      return kExitUsage;
    }
    return hold_lock(argv[2]);
  }
  if (command == "commit-loop") {
    bool ok = false;
    const std::uint64_t count = argc >= 4 ? parse_u64(argv[3], ok) : 0;
    if (argc < 4 || !ok) {
      return kExitUsage;
    }
    return commit_loop(argv[2], static_cast<std::uint32_t>(count));
  }
  if (command == "stage-only") {
    bool ok = false;
    const std::uint64_t count = argc >= 4 ? parse_u64(argv[3], ok) : 0;
    if (argc < 4 || !ok) {
      return kExitUsage;
    }
    return stage_only(argv[2], static_cast<std::uint32_t>(count));
  }
  if (command == "dispatch-then-die") {
    if (argc < 4) {
      return kExitUsage;
    }
    return dispatch_then_die(argv[2], argv[3]);
  }
  if (command == "reopen-and-report") {
    if (argc < 3) {
      return kExitUsage;
    }
    std::uint64_t epoch = kEpoch;
    std::uint64_t incarnation = kIncarnation;
    for (int index = 3; index + 1 < argc; index += 2) {
      bool ok = false;
      const std::uint64_t value = parse_u64(argv[index + 1], ok);
      if (!ok) {
        return kExitUsage;
      }
      const std::string flag{argv[index]};
      if (flag == "--epoch") {
        epoch = value;
      } else if (flag == "--incarnation") {
        incarnation = value;
      } else {
        return kExitUsage;
      }
    }
    return reopen_and_report(argv[2], epoch, incarnation);
  }
  if (command == "verify") {
    if (argc < 3) {
      return kExitUsage;
    }
    return verify(argv[2]);
  }
  emit("usage: unknown subcommand " + command);
  return kExitUsage;
}
