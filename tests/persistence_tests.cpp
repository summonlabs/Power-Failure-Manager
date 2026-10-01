// Power Failure Manager -- durable persistence proofs.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// Every case in this file uses real files in a disposable directory and attacks
// the exact bytes the store itself wrote. Nothing here mocks the store, the
// filesystem, or the codec: a case passes only if the on-disk evidence proves
// it.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "framework.hpp"
#include "support.hpp"

#include "pfm/codec.hpp"
#include "pfm/journal.hpp"
#include "pfm/store.hpp"
#include "pfm/version.hpp"

namespace pfm = summon::pfm;

namespace {

using Bytes = std::vector<std::uint8_t>;

// Framing written by src/store.cpp: a 56-byte header, the canonical snapshot
// payload, then an 8-byte trailer. The field offsets below are the contract the
// corruption cases attack.
constexpr std::size_t kHeaderSize = 56;
constexpr std::size_t kTrailerSize = 8;
constexpr std::size_t kFormatOffset = 4;
constexpr std::size_t kReservedOffset = 8;
constexpr std::size_t kPayloadLengthOffset = 40;
constexpr std::size_t kHeaderCrcOffset = 48;

// --- small filesystem helpers ---------------------------------------------

std::string slot_file(const std::string& directory, unsigned index) {
  return (std::filesystem::path{directory} / ("state." + std::to_string(index))).string();
}

std::string staging_file(const std::string& directory, unsigned index) {
  return slot_file(directory, index) + ".staging";
}

Bytes read_bytes(const std::string& path) {
  std::ifstream stream{path, std::ios::binary};
  if (!stream) {
    return {};
  }
  stream.seekg(0, std::ios::end);
  const auto end = static_cast<std::streamoff>(stream.tellg());
  stream.seekg(0, std::ios::beg);
  Bytes bytes(static_cast<std::size_t>(end));
  if (!bytes.empty()) {
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  return bytes;
}

bool write_bytes(const std::string& path, const Bytes& bytes) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  if (!stream) {
    return false;
  }
  if (!bytes.empty()) {
    stream.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
  }
  stream.flush();
  return static_cast<bool>(stream);
}

std::uint32_t u32_at(const Bytes& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8 * i);
  }
  return value;
}

void put_u32_at(Bytes& bytes, std::size_t offset, std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    bytes[offset + i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

// Repairs the header checksum after a deliberate edit of another header field,
// so that a length attack is proven against the length check itself rather than
// being caught earlier by the checksum. The store hashes exactly the 48 bytes
// that precede the checksum field.
void refresh_header_crc(Bytes& bytes) {
  const std::span<const std::uint8_t> prefix{bytes.data(), kHeaderCrcOffset};
  put_u32_at(bytes, kHeaderCrcOffset, pfm::codec::crc32c(prefix));
}

// --- fixtures --------------------------------------------------------------

std::unique_ptr<pfmtest::PlantFixture> open_fixture(
    const std::string& directory, pfm::StoreMode mode,
    pfm::ControlEpoch epoch = pfm::ControlEpoch::from_value(1),
    pfm::ControllerIncarnation incarnation = pfm::ControllerIncarnation::from_value(1),
    const pfm::SyntheticPlantConfig& config = pfm::SyntheticPlantConfig{}) {
  auto created = pfmtest::PlantFixture::make(config, mode, directory, epoch, incarnation);
  if (!created.ok()) {
    pfmtest::report_failure(__FILE__, __LINE__, "the plant fixture could not be opened: " +
                                                    std::string{created.status().to_string()});
    return nullptr;
  }
  return std::move(created.value());
}

pfm::StoreOptions durable_store_options(const std::string& directory) {
  pfm::StoreOptions options;
  options.directory = directory;
  options.mode = pfm::StoreMode::Durable;
  return options;
}

// A runtime with bounds of the caller's choosing, driving the synthetic plant.
struct ManualRuntime {
  std::unique_ptr<pfm::SyntheticPlant> plant{};
  std::shared_ptr<pfm::ManualClock> clock{};
  std::unique_ptr<pfm::PowerFailureRuntime> runtime{};

  static std::unique_ptr<ManualRuntime> make(const pfm::RuntimeOptions& options,
                                             pfm::SyntheticPlantConfig config = {}) {
    auto made = std::unique_ptr<ManualRuntime>{new ManualRuntime{}};
    made->plant = std::make_unique<pfm::SyntheticPlant>(std::move(config));
    made->clock = pfmtest::make_clock();
    auto opened = pfm::PowerFailureRuntime::open(
        options, made->clock,
        std::shared_ptr<pfm::ResponseTransport>{made->plant.get(),
                                               [](pfm::ResponseTransport*) {}});
    if (!opened.ok()) {
      pfmtest::report_failure(__FILE__, __LINE__, "the runtime could not be opened: " +
                                                      std::string{opened.status().to_string()});
      return nullptr;
    }
    made->runtime = std::move(opened.value());
    return made;
  }

  [[nodiscard]] pfm::Result<void> prime(
      pfm::IncidentId incident = pfm::IncidentId::from_value(1),
      pfm::IncidentGeneration generation = pfm::IncidentGeneration::from_value(1)) {
    auto token = runtime->current_authority();
    if (!token.ok()) {
      return token.status();
    }
    if (auto published = runtime->publish_topology(plant->topology(), token.value());
        !published.ok()) {
      return published;
    }
    token = runtime->current_authority();
    if (!token.ok()) {
      return token.status();
    }
    token.value().incident = incident;
    token.value().generation = generation;
    return runtime->open_incident(token.value());
  }
};

// Real, fully validated domain states, produced by driving a runtime that never
// touches the disk. Each state is exactly what a durable runtime would have
// committed at that instant.
std::vector<pfm::DomainState> make_states(const std::string& scratch, std::size_t count) {
  std::vector<pfm::DomainState> states;
  auto fixture = open_fixture(scratch, pfm::StoreMode::Volatile);
  if (fixture == nullptr) {
    return states;
  }
  if (auto primed = fixture->prime(); !primed.ok()) {
    pfmtest::report_failure(__FILE__, __LINE__, "the fixture could not be primed: " +
                                                    std::string{primed.status().to_string()});
    return states;
  }
  const auto capture = [&states, &fixture]() {
    auto state = fixture->runtime->state();
    if (!state.ok()) {
      pfmtest::report_failure(__FILE__, __LINE__, "the runtime state could not be read: " +
                                                      std::string{state.status().to_string()});
      return;
    }
    states.push_back(state.value());
  };
  capture();
  while (states.size() < count) {
    auto ticked = fixture->tick(pfm::duration_from_millis(1000));
    if (!ticked.ok()) {
      pfmtest::report_failure(__FILE__, __LINE__, "the fixture tick failed: " +
                                                      std::string{ticked.status().to_string()});
      break;
    }
    capture();
  }
  return states;
}

// Publishes complete states as consecutive generations of a fresh durable
// store. Two commits fill both slots and a third overwrites the older one,
// which is exactly the "newest slot plus previous complete generation" layout
// the corruption cases attack.
bool publish_generations(const std::string& directory,
                         const std::vector<pfm::DomainState>& states) {
  auto opened = pfm::DurableStore::open(durable_store_options(directory));
  if (!opened.ok()) {
    pfmtest::report_failure(__FILE__, __LINE__, "the durable store could not be opened: " +
                                                    std::string{opened.status().to_string()});
    return false;
  }
  auto store = std::move(opened.value());
  for (const auto& state : states) {
    pfm::StoreSnapshot snapshot;
    snapshot.checkpoint = state;
    snapshot.live = state;
    auto committed = store->commit(snapshot);
    if (!committed.ok()) {
      pfmtest::report_failure(__FILE__, __LINE__, "a generation could not be committed: " +
                                                      std::string{committed.status().to_string()});
      return false;
    }
  }
  auto closed = store->close();
  return closed.ok();
}

// Loads a store whose newest slot (state.0) was attacked: the slot must be
// refused with exactly the expected code and the previous complete generation
// must be loaded instead, byte for byte.
void expect_refused_newest_slot(const std::string& directory, const Bytes& previous_bytes,
                                pfm::StatusCode expected_code, const char* label) {
  const std::string prefix = std::string{"["} + label + "] ";
  auto opened = pfm::DurableStore::open(durable_store_options(directory));
  if (!opened.ok()) {
    pfmtest::report_failure(__FILE__, __LINE__, prefix + "the store did not open: " +
                                                    std::string{opened.status().to_string()});
    return;
  }
  auto store = std::move(opened.value());
  auto loaded = store->load();
  if (!loaded.ok()) {
    pfmtest::report_failure(__FILE__, __LINE__,
                            prefix + "the store refused the load instead of falling back: " +
                                std::string{loaded.status().to_string()});
    return;
  }
  const auto& status = store->status();
  PFM_CHECK_MSG(status.has_state, prefix + "the store reports no state");
  PFM_CHECK_MSG(status.fell_back, prefix + "the store did not report a fallback");
  PFM_CHECK_MSG(!status.fallback_detail.empty(), prefix + "no fallback detail was recorded");
  PFM_CHECK_EQ(status.slots.size(), std::size_t{2});
  PFM_CHECK_MSG(!status.slots[0].valid, prefix + "the attacked slot was reported usable");
  PFM_CHECK_MSG(status.slots[0].refusal == expected_code,
                prefix + "refusal was " + std::string{status.slots[0].detail} + " instead of " +
                    std::string{pfm::status_code_name(expected_code)});
  PFM_CHECK_MSG(status.slots[1].valid, prefix + "the previous complete generation was refused too");
  PFM_CHECK_EQ(status.commit_sequence.value(), std::uint64_t{2});
  PFM_CHECK_EQ(status.generation.value(), std::uint64_t{2});
  PFM_CHECK_EQ(status.commits, std::uint64_t{0});
  PFM_CHECK_MSG(pfm::encode_state(loaded.value().live) == previous_bytes,
                prefix + "the loaded live state is not the previous complete generation");
  PFM_CHECK_MSG(pfm::encode_state(loaded.value().checkpoint) == previous_bytes,
                prefix + "the loaded checkpoint is not the previous complete generation");
  PFM_CHECK_MSG(loaded.value().journal.empty(), prefix + "the fallback carried a journal");
  PFM_CHECK_OK(store->close());
}

}  // namespace

// ---------------------------------------------------------------------------
// A durable runtime commits, closes, reopens, and resolves the same generation.
// ---------------------------------------------------------------------------
PFM_TEST(durable_runtime_commits_closes_reopens_and_resolves_the_generation) {
  pfmtest::TempDirectory directory("persist-reopen");
  auto fixture = open_fixture(directory.path(), pfm::StoreMode::Durable);
  PFM_REQUIRE(fixture != nullptr);
  PFM_CHECK_OK(fixture->prime());
  PFM_CHECK_OK(fixture->tick(pfm::duration_from_millis(1000)));

  auto before = fixture->runtime->state();
  PFM_REQUIRE(before.ok());
  auto runtime_status = fixture->runtime->status();
  PFM_REQUIRE(runtime_status.ok());
  auto store_status = fixture->runtime->store_status();
  PFM_REQUIRE(store_status.ok());

  PFM_CHECK_EQ(store_status.value().mode, pfm::StoreMode::Durable);
  PFM_CHECK(!store_status.value().directory.empty());
  PFM_CHECK(store_status.value().has_state);
  PFM_CHECK(store_status.value().commits > 0);
  PFM_CHECK(store_status.value().commit_sequence.value() > 0);
  PFM_CHECK(store_status.value().generation.value() > 0);
  PFM_CHECK(store_status.value().committed_at.value() != 0);
  PFM_CHECK(!store_status.value().fell_back);
  PFM_CHECK(store_status.value().fallback_detail.empty());
  PFM_CHECK_EQ(store_status.value().slots.size(), std::size_t{2});
  PFM_CHECK_EQ(store_status.value().commit_sequence.value(),
               runtime_status.value().commit_sequence.value());
  PFM_CHECK_EQ(store_status.value().commits, runtime_status.value().commits);
  PFM_CHECK_EQ(store_status.value().retired_journal_entries,
               runtime_status.value().retired_journal_entries);
  // The very first generation is a complete checkpoint, never an empty
  // placeholder: the bootstrap entries are folded into it immediately.
  PFM_CHECK_MSG(store_status.value().retired_journal_entries >= 2,
                "the first durable generation was not published as a complete checkpoint");
  PFM_CHECK(runtime_status.value().journal_entries > 0);
  PFM_CHECK(runtime_status.value().incident_live);
  PFM_CHECK(runtime_status.value().revision.value() > 0);

  const auto sequence_before = store_status.value().commit_sequence.value();
  const auto generation_before = store_status.value().generation.value();
  const auto retired_before = store_status.value().retired_journal_entries;
  const auto revision_before = before.value().revision.value();

  PFM_CHECK_OK(fixture->runtime->shutdown());
  PFM_CHECK_OK(fixture->runtime->shutdown());  // shutdown is idempotent
  fixture->runtime.reset();

  auto reopened = open_fixture(directory.path(), pfm::StoreMode::Durable);
  PFM_REQUIRE(reopened != nullptr);
  auto status = reopened->runtime->status();
  PFM_REQUIRE(status.ok());
  PFM_CHECK_EQ(status.value().epoch.value(), std::uint64_t{1});
  PFM_CHECK_EQ(status.value().incarnation.value(), std::uint64_t{1});
  PFM_CHECK(status.value().revision.value() > revision_before);
  PFM_CHECK(status.value().incident_live);
  // The durable decision state survives, but a plan is not current across an
  // authority re-establishment: it is recomputed from evidence, never resumed.
  PFM_CHECK_MSG(!status.value().has_plan,
                "a plan was resumed across a re-open instead of being recomputed");
  PFM_CHECK_CODE(reopened->runtime->plan(), pfm::StatusCode::PlanNotCurrent);
  PFM_CHECK(status.value().commit_sequence.value() > sequence_before);

  auto store_after = reopened->runtime->store_status();
  PFM_REQUIRE(store_after.ok());
  PFM_CHECK(store_after.value().has_state);
  PFM_CHECK(!store_after.value().fell_back);
  PFM_CHECK(store_after.value().commits >= 1);
  PFM_CHECK_EQ(store_after.value().retired_journal_entries, retired_before);
  // Resuming the same controller under the same epoch adds exactly one durable
  // decision to the generation it resumed.
  PFM_CHECK_EQ(store_after.value().commit_sequence.value(),
               reopened->runtime->status().value().commit_sequence.value());
  PFM_CHECK(store_after.value().generation.value() > generation_before);

  auto after = reopened->runtime->state();
  PFM_REQUIRE(after.ok());
  PFM_CHECK_EQ(after.value().incident.incident.value(), before.value().incident.incident.value());
  PFM_CHECK_EQ(after.value().incident.generation.value(),
               before.value().incident.generation.value());
  PFM_CHECK_EQ(after.value().incident.lifecycle, before.value().incident.lifecycle);
  PFM_CHECK_EQ(after.value().topology_generation.value(),
               before.value().topology_generation.value());
  PFM_CHECK(after.value().has_topology);
  PFM_CHECK_EQ(after.value().topology.elements.size(), before.value().topology.elements.size());
  PFM_CHECK(after.value().epoch == before.value().epoch);
  PFM_CHECK(after.value().incarnation == before.value().incarnation);
  PFM_CHECK_EQ(after.value().observations.size(), before.value().observations.size());
  // The restored generation is usable: a fresh decision republishes a plan.
  PFM_CHECK_OK(reopened->tick(pfm::duration_from_millis(1000)));
  auto republished = reopened->runtime->plan();
  PFM_REQUIRE(republished.ok());
  PFM_CHECK(republished.value().generation.value() > 0);
  PFM_CHECK_OK(reopened->runtime->shutdown());
}

// ---------------------------------------------------------------------------
// Evidence that ages between two commits does not make a generation unreplayable.
// ---------------------------------------------------------------------------
PFM_TEST(evidence_that_ages_between_commits_stays_replayable) {
  pfmtest::TempDirectory directory("persist-aging");
  auto fixture = open_fixture(directory.path(), pfm::StoreMode::Durable);
  PFM_REQUIRE(fixture != nullptr);
  PFM_CHECK_OK(fixture->prime());
  auto buses = fixture->plant->elements_of_kind(pfm::ElementKind::Bus);
  PFM_REQUIRE(buses.size() >= 2);

  // One element reports at T+1.
  PFM_CHECK_OK(fixture->clock->advance(pfm::duration_from_millis(1000)));
  auto first_instant = fixture->clock->now();
  PFM_REQUIRE(first_instant.ok());
  auto first_observation = fixture->plant->observe_element(buses[0], first_instant.value());
  PFM_REQUIRE(first_observation.element.is_set());
  auto token = fixture->token();
  PFM_REQUIRE(token.ok());
  PFM_CHECK_OK(fixture->runtime->admit_evidence(first_observation, token.value()));

  // Time passes beyond the freshness window, and then a different element
  // reports: the first element's evidence is now stale while the generation is
  // still being committed.
  PFM_CHECK_OK(fixture->clock->advance(pfm::duration_from_millis(30000)));
  auto second_instant = fixture->clock->now();
  PFM_REQUIRE(second_instant.ok());
  auto second_observation = fixture->plant->observe_element(buses[1], second_instant.value());
  PFM_REQUIRE(second_observation.element.is_set());
  token = fixture->token();
  PFM_REQUIRE(token.ok());
  PFM_CHECK_OK(fixture->runtime->admit_evidence(second_observation, token.value()));

  auto before = fixture->runtime->state();
  PFM_REQUIRE(before.ok());
  PFM_CHECK(!before.value().observations.empty());
  PFM_CHECK(pfm::classify_freshness(first_observation.observed_at, second_instant.value(),
                                    before.value().policy) != pfm::Freshness::Fresh);
  // The stale element is not current, and the generation it was committed in
  // must still replay: a load that reported divergence here would mean the
  // committed live state was not the fold of its checkpoint and journal.
  {
    const auto current =
        pfm::current_observations(before.value(), second_instant.value(), before.value().policy);
    PFM_CHECK_MSG(std::none_of(current.begin(), current.end(),
                               [&buses](const pfm::ElectricalObservation& observation) {
                                 return observation.element == buses[0];
                               }),
                  "aged-out evidence was still treated as current");
  }
  const auto before_bytes = pfm::encode_state(before.value());

  PFM_CHECK_OK(fixture->runtime->shutdown());
  fixture->runtime.reset();

  // The generation that was written while the first element's evidence aged out
  // must still replay: a load that reported divergence here would mean the
  // committed live state was not the fold of its checkpoint and journal.
  auto reopened = open_fixture(directory.path(), pfm::StoreMode::Durable);
  PFM_REQUIRE(reopened != nullptr);
  auto after = reopened->runtime->state();
  PFM_REQUIRE(after.ok());
  PFM_CHECK_EQ(after.value().observations.size(), before.value().observations.size());
  PFM_CHECK_EQ(after.value().revision.value() > before.value().revision.value(), true);
  PFM_CHECK_OK(reopened->runtime->shutdown());
  static_cast<void>(before_bytes);
}

// ---------------------------------------------------------------------------
// The store itself reports commit sequence, generation, and commits.
// ---------------------------------------------------------------------------
PFM_TEST(durable_store_reports_commit_sequence_generation_and_commits) {
  pfmtest::TempDirectory scratch("persist-store-states");
  pfmtest::TempDirectory directory("persist-store");
  const auto states = make_states(scratch.path(), 1);
  PFM_REQUIRE(states.size() == 1);
  const auto expected = pfm::encode_state(states.front());
  PFM_REQUIRE(!expected.empty());

  auto canonical = pfm::canonical_store_directory(directory.path());
  PFM_REQUIRE(canonical.ok());

  auto opened = pfm::DurableStore::open(durable_store_options(directory.path()));
  PFM_REQUIRE(opened.ok());
  auto store = std::move(opened.value());
  PFM_CHECK(store->is_durable());
  PFM_CHECK_EQ(store->status().mode, pfm::StoreMode::Durable);
  PFM_CHECK_EQ(store->status().directory, canonical.value());
  PFM_CHECK(!store->status().has_state);
  PFM_CHECK_EQ(store->status().commits, std::uint64_t{0});
  PFM_CHECK_EQ(store->status().commit_sequence.value(), std::uint64_t{0});

  pfm::StoreSnapshot snapshot;
  snapshot.checkpoint = states.front();
  snapshot.live = states.front();
  PFM_CHECK_OK(store->commit(snapshot));
  PFM_CHECK_EQ(store->status().commits, std::uint64_t{1});
  PFM_CHECK_EQ(store->status().commit_sequence.value(), std::uint64_t{1});
  PFM_CHECK_EQ(store->status().generation.value(), std::uint64_t{1});
  PFM_CHECK(store->status().has_state);
  PFM_CHECK(!store->status().fell_back);

  auto reloaded = store->load();
  PFM_REQUIRE(reloaded.ok());
  PFM_CHECK(pfm::encode_state(reloaded.value().live) == expected);
  PFM_CHECK(pfm::encode_state(reloaded.value().checkpoint) == expected);
  PFM_CHECK(reloaded.value().journal.empty());
  PFM_CHECK_EQ(reloaded.value().retired_journal_entries, std::uint64_t{0});
  PFM_CHECK_EQ(store->status().commit_sequence.value(), std::uint64_t{1});
  PFM_CHECK(!store->status().fell_back);
  PFM_CHECK_OK(store->close());

  auto second_open = pfm::DurableStore::open(durable_store_options(directory.path()));
  PFM_REQUIRE(second_open.ok());
  auto second = std::move(second_open.value());
  PFM_CHECK_EQ(second->status().commits, std::uint64_t{0});  // nothing committed yet
  auto loaded = second->load();
  PFM_REQUIRE(loaded.ok());
  PFM_CHECK_EQ(second->status().commit_sequence.value(), std::uint64_t{1});
  PFM_CHECK_EQ(second->status().generation.value(), std::uint64_t{1});
  PFM_CHECK(second->status().has_state);
  PFM_CHECK(second->status().committed_at.value() != 0);
  PFM_CHECK(!second->status().fell_back);
  PFM_CHECK_EQ(second->status().slots.size(), std::size_t{2});
  PFM_CHECK(second->status().slots[0].valid || second->status().slots[1].valid);
  PFM_CHECK(pfm::encode_state(loaded.value().live) == expected);

  PFM_CHECK_OK(second->commit(snapshot));
  PFM_CHECK_EQ(second->status().commit_sequence.value(), std::uint64_t{2});
  PFM_CHECK_EQ(second->status().generation.value(), std::uint64_t{2});
  PFM_CHECK_EQ(second->status().commits, std::uint64_t{1});
  PFM_CHECK_OK(second->close());
}

// ---------------------------------------------------------------------------
// Canonical encoding determinism, and a full state round-tripping the store.
// ---------------------------------------------------------------------------
PFM_TEST(canonical_encoding_is_deterministic_and_round_trips_through_the_store) {
  pfmtest::TempDirectory scratch("persist-codec");
  pfmtest::TempDirectory directory("persist-codec-store");
  auto fixture = open_fixture(scratch.path(), pfm::StoreMode::Volatile);
  PFM_REQUIRE(fixture != nullptr);
  PFM_CHECK_OK(fixture->prime());

  // An obligation over a real element of the published topology.
  auto buses = fixture->plant->elements_of_kind(pfm::ElementKind::Bus);
  PFM_REQUIRE(!buses.empty());
  auto authority = pfm::RefToken::make(pfm::RefKind::Controller, "controller-facility");
  PFM_REQUIRE(authority.ok());
  auto now = fixture->clock->now();
  PFM_REQUIRE(now.ok());
  pfm::ProtectedObligation obligation;
  obligation.id = pfm::ObligationId::from_value(1);
  obligation.target = buses.front();
  obligation.protection = pfm::ProtectionClass::ServiceLevel;
  obligation.status = pfm::ObligationStatus::Satisfied;
  obligation.has_report = true;
  obligation.reported_at = now.value();
  obligation.reporting_authority = authority.value();
  obligation.has_reserve_floor = true;
  obligation.reserve_floor = pfm::milli_percent_from_value(40000);
  auto token = fixture->token();
  PFM_REQUIRE(token.ok());
  PFM_CHECK_OK(fixture->runtime->publish_obligations({obligation}, token.value()));

  // A real failure, so that the state carries a plan and several requests.
  auto feeds = fixture->plant->elements_of_kind(pfm::ElementKind::UtilityFeed);
  PFM_REQUIRE(!feeds.empty());
  fixture->plant->lose_utility(feeds.front());
  PFM_CHECK_OK(fixture->tick(pfm::duration_from_millis(1000)));
  PFM_CHECK_OK(fixture->tick(pfm::duration_from_millis(1000)));

  auto state = fixture->runtime->state();
  PFM_REQUIRE(state.ok());
  PFM_CHECK(!state.value().observations.empty());
  PFM_CHECK(!state.value().obligations.empty());
  PFM_CHECK(!state.value().requests.empty());
  PFM_CHECK(state.value().has_plan);
  PFM_CHECK(!state.value().plan.requests.empty());

  // Byte-for-byte determinism of the canonical encoding.
  const auto first = pfm::encode_state(state.value());
  const auto second = pfm::encode_state(state.value());
  PFM_REQUIRE(!first.empty());
  PFM_CHECK(first == second);

  // encode_state() is the canonical encoder and nothing else.
  pfm::codec::Writer writer;
  pfm::codec::encode(writer, state.value(), state.value().bounds);
  PFM_REQUIRE(writer.ok());
  PFM_CHECK(writer.bytes() == first);

  // Decoding and re-encoding reproduces the same bytes.
  auto decoded = pfm::decode_state(first, state.value().bounds);
  PFM_REQUIRE(decoded.ok());
  PFM_CHECK(pfm::encode_state(decoded.value()) == first);
  auto decoded_again = pfm::decode_state(first, state.value().bounds);
  PFM_REQUIRE(decoded_again.ok());
  PFM_CHECK(pfm::encode_state(decoded_again.value()) == pfm::encode_state(decoded.value()));

  // The same state survives a real store round trip byte for byte.
  auto opened = pfm::DurableStore::open(durable_store_options(directory.path()));
  PFM_REQUIRE(opened.ok());
  auto store = std::move(opened.value());
  pfm::StoreSnapshot snapshot;
  snapshot.checkpoint = state.value();
  snapshot.live = state.value();
  PFM_CHECK_OK(store->commit(snapshot));
  auto loaded = store->load();
  PFM_REQUIRE(loaded.ok());
  PFM_CHECK(pfm::encode_state(loaded.value().live) == first);
  PFM_CHECK(pfm::encode_state(loaded.value().checkpoint) == first);
  PFM_CHECK_EQ(loaded.value().live.observations.size(), state.value().observations.size());
  PFM_CHECK_EQ(loaded.value().live.requests.size(), state.value().requests.size());
  PFM_CHECK_EQ(loaded.value().live.obligations.size(), state.value().obligations.size());
  PFM_CHECK_EQ(loaded.value().live.plan.requests.size(), state.value().plan.requests.size());
  PFM_CHECK_OK(store->close());
}

// ---------------------------------------------------------------------------
// Corruption of the newest slot: refused with an exact code, previous
// complete generation loaded, never partially applied state.
// ---------------------------------------------------------------------------
PFM_TEST(corruption_of_the_newest_slot_falls_back_to_the_previous_generation) {
  pfmtest::TempDirectory scratch("persist-corrupt-states");
  pfmtest::TempDirectory directory("persist-corrupt");
  const auto states = make_states(scratch.path(), 3);
  PFM_REQUIRE(states.size() == 3);
  PFM_REQUIRE(publish_generations(directory.path(), states));

  // The third commit overwrites the older slot, so state.0 is the newest
  // generation and state.1 is the previous complete one.
  {
    auto opened = pfm::DurableStore::open(durable_store_options(directory.path()));
    PFM_REQUIRE(opened.ok());
    auto store = std::move(opened.value());
    auto loaded = store->load();
    PFM_REQUIRE(loaded.ok());
    PFM_CHECK(pfm::encode_state(loaded.value().live) == pfm::encode_state(states[2]));
    PFM_CHECK_EQ(store->status().commit_sequence.value(), std::uint64_t{3});
    PFM_CHECK_EQ(store->status().generation.value(), std::uint64_t{3});
    PFM_CHECK(!store->status().fell_back);
    PFM_CHECK(store->status().slots[0].valid);
    PFM_CHECK(store->status().slots[1].valid);
    PFM_CHECK_OK(store->close());
  }

  const Bytes previous_bytes = pfm::encode_state(states[1]);
  const Bytes pristine = read_bytes(slot_file(directory.path(), 0));
  PFM_REQUIRE(!pristine.empty());
  const Bytes previous_file = read_bytes(slot_file(directory.path(), 1));
  PFM_REQUIRE(!previous_file.empty());
  PFM_CHECK(previous_file.size() > kHeaderSize + kTrailerSize);

  const auto attack = [&directory, &pristine, &previous_bytes](const char* label,
                                                              const Bytes& corrupted,
                                                              pfm::StatusCode expected) {
    if (!write_bytes(slot_file(directory.path(), 0), corrupted)) {
      pfmtest::report_failure(__FILE__, __LINE__,
                              std::string{"could not write the attacked slot for "} + label);
      return;
    }
    expect_refused_newest_slot(directory.path(), previous_bytes, expected, label);
  };

  // One flipped byte in the header.
  {
    Bytes corrupted = pristine;
    corrupted[16] ^= 0xFFu;  // store generation, covered by the header checksum
    attack("header byte flip", corrupted, pfm::StatusCode::StoreIntegrityFailure);
  }
  {
    Bytes corrupted = pristine;
    corrupted[kHeaderCrcOffset] ^= 0xFFu;  // the checksum field itself
    attack("header checksum byte flip", corrupted, pfm::StatusCode::StoreIntegrityFailure);
  }
  {
    Bytes corrupted = pristine;
    corrupted[kReservedOffset] = 1u;
    attack("non-zero reserved header field", corrupted, pfm::StatusCode::ReservedFieldNotZero);
  }
  {
    Bytes corrupted = pristine;
    put_u32_at(corrupted, kFormatOffset, 99u);
    attack("unsupported format version", corrupted, pfm::StatusCode::StoreVersionUnsupported);
  }

  // One flipped byte in the payload.
  const auto payload_length = u32_at(pristine, kPayloadLengthOffset);
  PFM_REQUIRE(payload_length > 2);
  {
    Bytes corrupted = pristine;
    corrupted[kHeaderSize + (payload_length / 2)] ^= 0xFFu;
    attack("payload byte flip", corrupted, pfm::StatusCode::StoreIntegrityFailure);
  }
  {
    Bytes corrupted = pristine;
    corrupted[kHeaderSize] ^= 0xFFu;
    attack("first payload byte flip", corrupted, pfm::StatusCode::StoreIntegrityFailure);
  }

  // One flipped byte in the trailer.
  {
    Bytes corrupted = pristine;
    corrupted[corrupted.size() - kTrailerSize] ^= 0xFFu;  // trailer magic
    attack("trailer magic byte flip", corrupted, pfm::StatusCode::StoreCorrupt);
  }
  {
    Bytes corrupted = pristine;
    corrupted[corrupted.size() - 1] ^= 0xFFu;  // trailer length
    attack("trailer length byte flip", corrupted, pfm::StatusCode::StoreCorrupt);
  }

  // Truncation at several lengths, including shorter than the framing.
  for (const std::size_t length :
       {std::size_t{0}, std::size_t{1}, std::size_t{8}, std::size_t{55}, std::size_t{63},
        std::size_t{64}, std::size_t{kHeaderSize + kTrailerSize}}) {
    Bytes corrupted(pristine.begin(), pristine.begin() + static_cast<std::ptrdiff_t>(length));
    attack("truncation", corrupted, pfm::StatusCode::StoreTruncated);
  }
  {
    Bytes corrupted(pristine.begin(), pristine.end() - 1);
    attack("truncation by one byte", corrupted, pfm::StatusCode::StoreTruncated);
  }
  {
    Bytes corrupted(pristine.begin(), pristine.begin() + static_cast<std::ptrdiff_t>(kHeaderSize));
    attack("header only", corrupted, pfm::StatusCode::StoreTruncated);
  }

  // Appended trailing bytes.
  {
    Bytes corrupted = pristine;
    corrupted.push_back(0u);
    corrupted.push_back(0u);
    corrupted.push_back(0u);
    corrupted.push_back(0u);
    attack("appended trailing bytes", corrupted, pfm::StatusCode::StoreTruncated);
  }
  {
    Bytes corrupted = pristine;
    corrupted.push_back(0xFFu);
    attack("one appended byte", corrupted, pfm::StatusCode::StoreTruncated);
  }

  // A payload length that disagrees with the file length, with a repaired
  // header checksum so that the length check itself is what refuses the slot.
  {
    Bytes corrupted = pristine;
    put_u32_at(corrupted, kPayloadLengthOffset, payload_length + 8u);
    refresh_header_crc(corrupted);
    attack("payload length beyond the file", corrupted, pfm::StatusCode::StoreTruncated);
  }
  {
    Bytes corrupted = pristine;
    put_u32_at(corrupted, kPayloadLengthOffset, payload_length - 8u);
    refresh_header_crc(corrupted);
    attack("payload length short of the file", corrupted, pfm::StatusCode::StoreTruncated);
  }
  {
    Bytes corrupted = pristine;
    put_u32_at(corrupted, kPayloadLengthOffset, 0u);
    refresh_header_crc(corrupted);
    attack("zero payload length", corrupted, pfm::StatusCode::StoreTruncated);
  }

  // With the slot restored, the newest generation loads again without a
  // fallback: the fallback above was caused by the attack and nothing else.
  PFM_CHECK(write_bytes(slot_file(directory.path(), 0), pristine));
  {
    auto opened = pfm::DurableStore::open(durable_store_options(directory.path()));
    PFM_REQUIRE(opened.ok());
    auto store = std::move(opened.value());
    auto loaded = store->load();
    PFM_REQUIRE(loaded.ok());
    PFM_CHECK(!store->status().fell_back);
    PFM_CHECK(pfm::encode_state(loaded.value().live) == pfm::encode_state(states[2]));
    PFM_CHECK_EQ(store->status().commit_sequence.value(), std::uint64_t{3});
    PFM_CHECK(store->status().slots[0].valid);
    PFM_CHECK_OK(store->close());
  }
}

// ---------------------------------------------------------------------------
// A staging file is never authoritative, and a clean close removes it.
// ---------------------------------------------------------------------------
PFM_TEST(a_leftover_staging_file_is_never_authoritative) {
  pfmtest::TempDirectory scratch("persist-staging-states");
  pfmtest::TempDirectory directory("persist-staging");
  const auto states = make_states(scratch.path(), 2);
  PFM_REQUIRE(states.size() == 2);
  PFM_REQUIRE(publish_generations(directory.path(), states));
  const auto newest_bytes = pfm::encode_state(states[1]);

  // Garbage in both staging paths: the store must ignore it entirely.
  Bytes garbage(64, 0xABu);
  PFM_CHECK(write_bytes(staging_file(directory.path(), 0), garbage));
  PFM_CHECK(write_bytes(staging_file(directory.path(), 1), garbage));
  PFM_CHECK(std::filesystem::exists(staging_file(directory.path(), 0)));
  PFM_CHECK(std::filesystem::exists(staging_file(directory.path(), 1)));

  {
    auto opened = pfm::DurableStore::open(durable_store_options(directory.path()));
    PFM_REQUIRE(opened.ok());
    auto store = std::move(opened.value());
    auto loaded = store->load();
    PFM_REQUIRE(loaded.ok());
    PFM_CHECK(pfm::encode_state(loaded.value().live) == newest_bytes);
    PFM_CHECK(!store->status().fell_back);
    PFM_CHECK_EQ(store->status().commit_sequence.value(), std::uint64_t{2});
    PFM_CHECK_OK(store->close());
  }
  PFM_CHECK_MSG(!std::filesystem::exists(staging_file(directory.path(), 0)),
                "the leftover staging file was not removed by a clean close");
  PFM_CHECK_MSG(!std::filesystem::exists(staging_file(directory.path(), 1)),
                "the leftover staging file was not removed by a clean close");

  // A commit stopped before publication (the fault injection models death at
  // exactly that point) leaves staged bytes behind that are still invisible.
  {
    auto opened = pfm::DurableStore::open(durable_store_options(directory.path()));
    PFM_REQUIRE(opened.ok());
    auto store = std::move(opened.value());
    store->set_stage_only(true);
    pfm::StoreSnapshot staged;
    // A state that is one revision ahead of what the store already holds.
    staged.checkpoint = states[1];
    staged.live = states[1];
    staged.live.revision = pfm::StateRevision::from_value(states[1].revision.value() + 1);
    PFM_CHECK_OK(store->commit(staged));
    const bool staged_exists = std::filesystem::exists(staging_file(directory.path(), 0)) ||
                               std::filesystem::exists(staging_file(directory.path(), 1));
    PFM_CHECK_MSG(staged_exists, "the fault injection did not leave a staging file behind");
    PFM_CHECK_EQ(store->status().commits, std::uint64_t{0});
    PFM_CHECK_EQ(store->status().commit_sequence.value(), std::uint64_t{0});

    auto reloaded = store->load();
    PFM_REQUIRE(reloaded.ok());
    PFM_CHECK_MSG(pfm::encode_state(reloaded.value().live) == newest_bytes,
                  "an un-published staging file changed what the store resolves");
    PFM_CHECK_OK(store->close());
  }
  PFM_CHECK_MSG(!std::filesystem::exists(staging_file(directory.path(), 0)),
                "the un-published staging file was not removed by a clean close");
  PFM_CHECK_MSG(!std::filesystem::exists(staging_file(directory.path(), 1)),
                "the un-published staging file was not removed by a clean close");

  // The published generation is still the newest one after all of that.
  {
    auto opened = pfm::DurableStore::open(durable_store_options(directory.path()));
    PFM_REQUIRE(opened.ok());
    auto store = std::move(opened.value());
    auto loaded = store->load();
    PFM_REQUIRE(loaded.ok());
    PFM_CHECK(pfm::encode_state(loaded.value().live) == newest_bytes);
    PFM_CHECK(!store->status().fell_back);
    PFM_CHECK_OK(store->close());
  }
}

// ---------------------------------------------------------------------------
// Replay divergence: valid checksums over a snapshot whose checkpoint plus
// journal no longer reproduce the live state.
// ---------------------------------------------------------------------------
PFM_TEST(replay_divergence_is_reported_rather_than_accepted) {
  pfmtest::TempDirectory source("persist-replay-source");
  pfmtest::TempDirectory good_directory("persist-replay-good");
  pfmtest::TempDirectory bad_directory("persist-replay-bad");
  pfmtest::TempDirectory other_directory("persist-replay-bad-2");

  // A real durable runtime produces a real checkpoint plus journal pair.
  pfm::StoreSnapshot snapshot;
  {
    auto fixture = open_fixture(source.path(), pfm::StoreMode::Durable);
    PFM_REQUIRE(fixture != nullptr);
    PFM_CHECK_OK(fixture->prime());
    PFM_CHECK_OK(fixture->tick(pfm::duration_from_millis(1000)));
    PFM_CHECK_OK(fixture->runtime->shutdown());
    fixture->runtime.reset();
  }
  {
    auto opened = pfm::DurableStore::open(durable_store_options(source.path()));
    PFM_REQUIRE(opened.ok());
    auto store = std::move(opened.value());
    auto loaded = store->load();
    PFM_REQUIRE(loaded.ok());
    snapshot = loaded.value();
    PFM_CHECK(!snapshot.journal.empty());
    PFM_CHECK(snapshot.checkpoint.revision.value() > 0);
    PFM_CHECK(!(pfm::encode_state(snapshot.checkpoint) == pfm::encode_state(snapshot.live)));
    PFM_CHECK_OK(store->close());
  }
  {
    pfm::DomainState folded = snapshot.checkpoint;
    for (const auto& entry : snapshot.journal) {
      auto applied = pfm::apply_journal_entry(folded, entry);
      PFM_REQUIRE(applied.ok());
    }
    PFM_CHECK_MSG(pfm::encode_state(folded) == pfm::encode_state(snapshot.live),
                  "the journal does not reproduce the live state in the first place");
  }

  // The untouched snapshot is accepted: the divergence below is the only
  // difference between the two stores.
  {
    auto opened = pfm::DurableStore::open(durable_store_options(good_directory.path()));
    PFM_REQUIRE(opened.ok());
    auto store = std::move(opened.value());
    PFM_CHECK_OK(store->commit(snapshot));
    auto loaded = store->load();
    PFM_REQUIRE(loaded.ok());
    PFM_CHECK(pfm::encode_state(loaded.value().live) == pfm::encode_state(snapshot.live));
    PFM_CHECK(!loaded.value().journal.empty());
    PFM_CHECK_OK(store->close());
  }

  // The same snapshot with a modified live state: every checksum is recomputed
  // by the store, so the bytes are perfectly valid, but the journal no longer
  // folds onto the claimed live state.
  pfm::StoreSnapshot divergent = snapshot;
  divergent.live.revision = pfm::StateRevision::from_value(snapshot.live.revision.value() + 1);
  PFM_CHECK(pfm::encode_state(divergent.live) != pfm::encode_state(snapshot.live));
  {
    auto opened = pfm::DurableStore::open(durable_store_options(bad_directory.path()));
    PFM_REQUIRE(opened.ok());
    auto store = std::move(opened.value());
    PFM_CHECK_OK(store->commit(divergent));
    // The committed slot is structurally perfect: it parses, its checksums
    // match, and it is the only generation, so replay divergence is the only
    // thing left to report.
    PFM_CHECK_CODE(store->load(), pfm::StatusCode::ReplayDivergence);
    PFM_CHECK(store->status().fell_back);
    PFM_CHECK(!store->status().fallback_detail.empty());
    // The slot itself parsed, checksums and all: it is replay that a load
    // trusts, and a second attempt reports exactly the same refusal.
    PFM_CHECK(store->status().slots[0].valid);
    PFM_CHECK_CODE(store->load(), pfm::StatusCode::ReplayDivergence);
    PFM_CHECK_OK(store->close());
  }

  // A second, independent divergence: a live state that claims a lifecycle the
  // journal never produced.
  pfm::StoreSnapshot divergent_two = snapshot;
  divergent_two.live.incident.lifecycle = pfm::IncidentLifecycle::Closed;
  PFM_CHECK(pfm::encode_state(divergent_two.live) != pfm::encode_state(snapshot.live));
  {
    auto opened = pfm::DurableStore::open(durable_store_options(other_directory.path()));
    PFM_REQUIRE(opened.ok());
    auto store = std::move(opened.value());
    PFM_CHECK_OK(store->commit(divergent_two));
    PFM_CHECK_CODE(store->load(), pfm::StatusCode::ReplayDivergence);
    PFM_CHECK(store->status().fell_back);
    PFM_CHECK_OK(store->close());
  }
}

// ---------------------------------------------------------------------------
// Journal retention: the journal folds into the checkpoint, and replay still
// reproduces the live state byte for byte.
// ---------------------------------------------------------------------------
PFM_TEST(journal_retention_folds_into_the_checkpoint_and_replay_reproduces_live) {
  pfmtest::TempDirectory directory("persist-journal");
  pfm::RuntimeOptions options = pfmtest::durable_options(directory.path());
  options.bounds.max_journal_entries = 8;
  auto fixture = ManualRuntime::make(options);
  PFM_REQUIRE(fixture != nullptr);
  PFM_CHECK_OK(fixture->prime());

  const std::size_t iteration_bound = 12;
  for (std::size_t round = 0; round < iteration_bound; ++round) {
    PFM_CHECK_OK(fixture->clock->advance(pfm::duration_from_millis(1000)));
    auto now = fixture->clock->now();
    PFM_REQUIRE(now.ok());
    auto observations = fixture->plant->observe(now.value());
    PFM_REQUIRE(observations.size() > options.bounds.max_journal_entries);
    auto token = fixture->runtime->current_authority();
    PFM_REQUIRE(token.ok());
    PFM_CHECK_OK(fixture->runtime->admit_evidence(observations, token.value()));
    auto status = fixture->runtime->status();
    PFM_REQUIRE(status.ok());
    PFM_CHECK(status.value().journal_entries <= options.bounds.max_journal_entries);
  }

  auto status = fixture->runtime->status();
  PFM_REQUIRE(status.ok());
  PFM_CHECK_MSG(status.value().retired_journal_entries > 0,
                "the journal never folded into the checkpoint");
  PFM_CHECK_EQ(status.value().journal_entries, std::size_t{0});

  // An explicit checkpoint folds whatever is retained and republishes both.
  auto token = fixture->runtime->current_authority();
  PFM_REQUIRE(token.ok());
  PFM_CHECK_OK(fixture->runtime->checkpoint(token.value()));
  auto after_checkpoint = fixture->runtime->status();
  PFM_REQUIRE(after_checkpoint.ok());
  PFM_CHECK(after_checkpoint.value().retired_journal_entries >=
            status.value().retired_journal_entries);
  PFM_CHECK_EQ(after_checkpoint.value().journal_entries, std::size_t{0});

  const auto retired = after_checkpoint.value().retired_journal_entries;
  PFM_CHECK_OK(fixture->runtime->shutdown());
  fixture->runtime.reset();

  auto opened = pfm::DurableStore::open(durable_store_options(directory.path()));
  PFM_REQUIRE(opened.ok());
  auto store = std::move(opened.value());
  auto loaded = store->load();
  PFM_REQUIRE(loaded.ok());
  PFM_CHECK(loaded.value().journal.size() <= options.bounds.max_journal_entries);
  PFM_CHECK_EQ(loaded.value().retired_journal_entries, retired);
  PFM_CHECK_EQ(store->status().retired_journal_entries, retired);

  // The checkpoint is real content, not an empty state.
  PFM_CHECK(loaded.value().checkpoint.revision.value() > 0);
  PFM_CHECK(loaded.value().checkpoint.has_topology);
  PFM_CHECK(!loaded.value().checkpoint.observations.empty());
  PFM_CHECK(loaded.value().checkpoint.incident.lifecycle != pfm::IncidentLifecycle::None);

  // Replay: checkpoint plus the retained journal reproduces the live state
  // byte for byte.
  pfm::DomainState folded = loaded.value().checkpoint;
  for (const auto& entry : loaded.value().journal) {
    auto applied = pfm::apply_journal_entry(folded, entry);
    PFM_REQUIRE(applied.ok());
  }
  PFM_CHECK_MSG(pfm::encode_state(folded) == pfm::encode_state(loaded.value().live),
                "replay does not reproduce the persisted live state");
  PFM_CHECK(!pfm::encode_state(loaded.value().live).empty());
  PFM_CHECK_OK(store->close());
}

// ---------------------------------------------------------------------------
// Lock exclusivity inside one process.
// ---------------------------------------------------------------------------
PFM_TEST(the_store_lock_is_exclusive_within_one_process) {
  pfmtest::TempDirectory directory("persist-lock");
  auto first = pfm::StoreLock::acquire(directory.path());
  PFM_REQUIRE(first.ok());
  PFM_CHECK(first.value().held());
  PFM_CHECK_CODE(pfm::StoreLock::acquire(directory.path()), pfm::StatusCode::StoreLocked);
  PFM_CHECK_CODE(pfm::DurableStore::open(durable_store_options(directory.path())),
                 pfm::StatusCode::StoreLocked);
  first.value().release();
  PFM_CHECK(!first.value().held());

  auto second = pfm::StoreLock::acquire(directory.path());
  PFM_REQUIRE(second.ok());
  PFM_CHECK(second.value().held());
  second.value().release();

  auto store = pfm::DurableStore::open(durable_store_options(directory.path()));
  PFM_REQUIRE(store.ok());
  PFM_CHECK(store.value()->status().mode == pfm::StoreMode::Durable);
  PFM_CHECK_CODE(pfm::DurableStore::open(durable_store_options(directory.path())),
                 pfm::StatusCode::StoreLocked);
  PFM_CHECK_CODE(pfm::StoreLock::acquire(directory.path()), pfm::StatusCode::StoreLocked);
  PFM_CHECK_OK(store.value()->close());
  auto after = pfm::DurableStore::open(durable_store_options(directory.path()));
  PFM_REQUIRE(after.ok());
  PFM_CHECK_OK(after.value()->close());
}

// ---------------------------------------------------------------------------
// Canonical store directory refusals, and volatile mode holding nothing.
// ---------------------------------------------------------------------------
PFM_TEST(store_path_refusals_and_volatile_mode_without_durable_content) {
  PFM_CHECK_CODE(pfm::canonical_store_directory(""), pfm::StatusCode::StorePathInvalid);

  pfmtest::TempDirectory directory("persist-path");
  const auto file_path = directory.file("not-a-directory");
  PFM_CHECK(write_bytes(file_path, Bytes{1u, 2u, 3u}));
  PFM_CHECK(std::filesystem::is_regular_file(file_path));
  PFM_CHECK_CODE(pfm::DurableStore::open(durable_store_options(file_path)),
                 pfm::StatusCode::StorePathInvalid);
  // Canonicalisation itself accepts the path; it is opening the store that
  // refuses a path which cannot be a directory on this host.
  auto file_canonical = pfm::canonical_store_directory(file_path);
  PFM_CHECK(file_canonical.ok());
  auto absolute = pfm::canonical_store_directory(directory.path());
  PFM_REQUIRE(absolute.ok());
  PFM_CHECK(std::filesystem::path{absolute.value()}.is_absolute());
  auto relative = pfm::canonical_store_directory("pfm-relative-store-probe");
  PFM_REQUIRE(relative.ok());
  PFM_CHECK(std::filesystem::path{relative.value()}.is_absolute());

  // Volatile mode holds nothing durable, by construction.
  pfm::StoreOptions options;
  options.mode = pfm::StoreMode::Volatile;
  auto store = pfm::DurableStore::open(options);
  PFM_REQUIRE(store.ok());
  PFM_CHECK(!store.value()->is_durable());
  PFM_CHECK_EQ(store.value()->status().mode, pfm::StoreMode::Volatile);
  PFM_CHECK(store.value()->status().directory.empty());
  PFM_CHECK(!store.value()->status().has_state);
  PFM_CHECK_CODE(store.value()->load(), pfm::StatusCode::StoreNotFound);
  PFM_CHECK_OK(store.value()->commit(pfm::StoreSnapshot{}));
  PFM_CHECK_EQ(store.value()->status().commits, std::uint64_t{0});
  PFM_CHECK(!store.value()->status().has_state);
  PFM_CHECK_OK(store.value()->close());

  const std::string ignored_directory = directory.file("volatile-ignored");
  auto fixture = open_fixture(ignored_directory, pfm::StoreMode::Volatile);
  PFM_REQUIRE(fixture != nullptr);
  auto runtime_status = fixture->runtime->status();
  PFM_REQUIRE(runtime_status.ok());
  PFM_CHECK_EQ(runtime_status.value().mode, pfm::StoreMode::Volatile);
  PFM_CHECK(runtime_status.value().store_directory.empty());
  auto runtime_store = fixture->runtime->store_status();
  PFM_REQUIRE(runtime_store.ok());
  PFM_CHECK(!runtime_store.value().has_state);
  PFM_CHECK(runtime_store.value().directory.empty());
  PFM_CHECK(!runtime_store.value().fell_back);
  PFM_CHECK(!std::filesystem::exists(ignored_directory));
  PFM_CHECK_OK(fixture->runtime->shutdown());
}



PFM_TEST_MAIN()
