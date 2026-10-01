// Power Failure Manager -- unit_tests (implementation).
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// Value-level unit tests for the library's stable contracts: exact integer
// arithmetic, typed identities, canonical reference tokens, the machine
// readable status contract, policy and obligation validation, the canonical
// binary codec, topology validation and indexing, request and journal
// fingerprints, and version identity.
//
// Every refusal is asserted with an exact StatusCode so a case can never pass
// because a different failure happened to be reported.

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "framework.hpp"
#include "pfm/codec.hpp"
#include "pfm/ids.hpp"
#include "pfm/journal.hpp"
#include "pfm/obligations.hpp"
#include "pfm/policy.hpp"
#include "pfm/refs.hpp"
#include "pfm/request.hpp"
#include "pfm/scenario.hpp"
#include "pfm/status.hpp"
#include "pfm/topology.hpp"
#include "pfm/units.hpp"
#include "pfm/version.hpp"

using namespace summon::pfm;

// Reports the two reference tokens instead of only "a != b".
#define PFM_CHECK_REF(actual, expected)                                             \
  do {                                                                              \
    const RefToken& pfm_ref_actual = (actual);                                      \
    const RefToken& pfm_ref_expected = (expected);                                  \
    if (!(pfm_ref_actual == pfm_ref_expected)) {                                    \
      ::pfmtest::report_failure(__FILE__, __LINE__,                                 \
                                std::string{"reference mismatch: "} +               \
                                    pfm_ref_actual.to_string() + " != " +           \
                                    pfm_ref_expected.to_string());                  \
    }                                                                               \
  } while (false)

namespace {

constexpr std::int64_t kI64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kI64Min = std::numeric_limits<std::int64_t>::min();
constexpr std::uint64_t kU64Max = std::numeric_limits<std::uint64_t>::max();

std::span<const std::uint8_t> as_bytes(const std::string& text) {
  return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// Canonical literals only: a rejection here would surface as an "element has no
// reference" failure inside the topology cases rather than silently passing.
RefToken ref_of(RefKind kind, std::string_view name) {
  auto made = RefToken::make(kind, name);
  return made.ok() ? made.value() : RefToken{};
}

bool contains_ref(const std::vector<RefToken>& values, const RefToken& value) {
  return std::find(values.begin(), values.end(), value) != values.end();
}

bool is_canonical_order(const std::vector<RefToken>& values) {
  return std::is_sorted(values.begin(), values.end());
}

bool has_duplicates(const std::vector<RefToken>& values) {
  return std::adjacent_find(values.begin(), values.end()) != values.end();
}

// --- shared fixtures -------------------------------------------------------

ResponseRequest make_valid_request() {
  ResponseRequest request;
  request.id = ResponseRequestId::from_value(1);
  request.kind = RequestKind::IsolateElement;
  request.target.element = ref_of(RefKind::Breaker, "brk-bus-0-0");
  request.target.kind = ElementKind::Breaker;
  request.target.owner = RequestOwner::PowerControlPlane;
  request.target.controller = ref_of(RefKind::Controller, "power-control-plane");
  request.plan_generation = PlanGeneration::from_value(1);
  request.incident_generation = IncidentGeneration::from_value(1);
  request.attempt = AttemptId::from_value(1);
  request.attempt_ordinal = 1;
  request.required_proof = ProofKind::Isolation;
  request.created_at = Timestamp::from_value(1'700'000'000'000LL);
  request.deadline = Timestamp::from_value(1'700'000'060'000LL);
  request.fingerprint = response_request_fingerprint(request);
  request.idempotency = IdempotencyKey::from_fingerprint(request.fingerprint);
  return request;
}

JournalEntry make_valid_entry() {
  JournalEntry entry;
  entry.sequence = JournalSequence::from_value(7);
  entry.kind = JournalKind::IncidentOpened;
  entry.recorded_at = Timestamp::from_value(1'700'000'000'000LL);
  entry.revision = StateRevision::from_value(3);
  entry.payload.has_incident = true;
  entry.payload.incident = IncidentId::from_value(1);
  entry.payload.incident_generation = IncidentGeneration::from_value(1);
  entry.payload.note = "incident opened";
  entry.fingerprint = journal_entry_fingerprint(entry);
  return entry;
}

ProtectedObligation make_obligation() {
  ProtectedObligation obligation;
  obligation.id = ObligationId::from_value(1);
  obligation.target = ref_of(RefKind::LoadGroup, "load-0-0-0");
  obligation.protection = ProtectionClass::ServiceLevel;
  obligation.status = ObligationStatus::Satisfied;
  obligation.has_report = true;
  obligation.reported_at = Timestamp::from_value(1'000'000);
  obligation.reporting_authority = ref_of(RefKind::Controller, "facility-obligations");
  obligation.policy_generation = PolicyGeneration::from_value(1);
  return obligation;
}

// A small but structurally complete plant: feed -> breaker -> bus -> branch ->
// load group, with one failure domain and one power domain.
TopologySnapshot small_topology() {
  TopologySnapshot snapshot;
  snapshot.generation = TopologyGeneration::from_value(1);

  TopologyElement feed;
  feed.element = ref_of(RefKind::UtilityFeed, "feed-a");
  feed.kind = ElementKind::UtilityFeed;
  feed.domain = ref_of(RefKind::PowerDomain, "pd-a");

  TopologyElement breaker;
  breaker.element = ref_of(RefKind::Breaker, "brk-a");
  breaker.kind = ElementKind::Breaker;
  breaker.upstream = feed.element;

  TopologyElement bus;
  bus.element = ref_of(RefKind::Bus, "bus-a");
  bus.kind = ElementKind::Bus;
  bus.upstream = breaker.element;
  bus.domain = ref_of(RefKind::PowerDomain, "pd-a");
  bus.has_isolation_point = true;
  bus.isolation_point = breaker.element;

  TopologyElement branch;
  branch.element = ref_of(RefKind::PduBranch, "branch-a");
  branch.kind = ElementKind::PduBranch;
  branch.upstream = bus.element;
  branch.load_group = ref_of(RefKind::LoadGroup, "load-a");

  TopologyElement load;
  load.element = ref_of(RefKind::LoadGroup, "load-a");
  load.kind = ElementKind::LoadGroup;
  load.upstream = branch.element;

  snapshot.elements = {feed, breaker, bus, branch, load};

  FailureDomain domain;
  domain.domain = ref_of(RefKind::FailureDomain, "fd-a");
  domain.members = {feed.element, breaker.element, bus.element, branch.element, load.element};
  domain.shared = false;
  snapshot.domains = {domain};
  return snapshot;
}

// Every assigned status code with the numeric value that is part of the public
// machine contract. A code that is renumbered, renamed, or duplicated fails.
struct AssignedCode {
  StatusCode code;
  std::uint16_t value;
};

constexpr AssignedCode kAssignedCodes[] = {
    {StatusCode::Ok, 0},
    {StatusCode::InvalidArgument, 100},
    {StatusCode::EmptyField, 101},
    {StatusCode::FieldTooLong, 102},
    {StatusCode::InvalidCharacter, 103},
    {StatusCode::ValueOutOfRange, 104},
    {StatusCode::DuplicateElement, 105},
    {StatusCode::TooManyElements, 106},
    {StatusCode::MalformedEncoding, 107},
    {StatusCode::ReservedFieldNotZero, 108},
    {StatusCode::UnknownEnumValue, 109},
    {StatusCode::MissingAuthority, 200},
    {StatusCode::StaleIncarnation, 201},
    {StatusCode::StaleEpoch, 202},
    {StatusCode::FutureEpoch, 203},
    {StatusCode::StaleRevision, 204},
    {StatusCode::FutureRevision, 205},
    {StatusCode::StaleGeneration, 206},
    {StatusCode::FutureGeneration, 207},
    {StatusCode::CrossIncidentAuthority, 208},
    {StatusCode::AuthorityFenced, 209},
    {StatusCode::AuthorityRequired, 210},
    {StatusCode::PreconditionFailed, 300},
    {StatusCode::IllegalTransition, 301},
    {StatusCode::NoActiveIncident, 302},
    {StatusCode::IncidentNotFound, 303},
    {StatusCode::IncidentClosed, 304},
    {StatusCode::RecoveryNotEligible, 305},
    {StatusCode::GateNotSatisfied, 306},
    {StatusCode::EvidenceNotCurrent, 307},
    {StatusCode::IncidentAlreadyOpen, 308},
    {StatusCode::PlanNotCurrent, 309},
    {StatusCode::EvidenceRejected, 400},
    {StatusCode::EvidenceStale, 401},
    {StatusCode::EvidenceFuture, 402},
    {StatusCode::EvidenceOutOfOrder, 403},
    {StatusCode::EvidenceContradictory, 404},
    {StatusCode::EvidenceUnavailable, 405},
    {StatusCode::EvidenceUnknownTarget, 406},
    {StatusCode::EvidenceDuplicate, 407},
    {StatusCode::EvidenceOriginUntrusted, 408},
    {StatusCode::TopologyUnknownElement, 500},
    {StatusCode::TopologyCycle, 501},
    {StatusCode::TopologyAmbiguousUpstream, 502},
    {StatusCode::TopologyBoundsExceeded, 503},
    {StatusCode::ScopeUnresolved, 504},
    {StatusCode::FailureDomainUnknown, 505},
    {StatusCode::SharedDomainUnresolved, 506},
    {StatusCode::ProtectedObligationViolation, 600},
    {StatusCode::ObligationNotRelaxable, 601},
    {StatusCode::ObligationUnknown, 602},
    {StatusCode::ObligationViolated, 603},
    {StatusCode::ObligationAlreadyRelaxed, 604},
    {StatusCode::ObligationNotRelaxed, 605},
    {StatusCode::RequestNotFound, 700},
    {StatusCode::RequestStateConflict, 701},
    {StatusCode::RequestExpired, 702},
    {StatusCode::IdempotencyConflict, 703},
    {StatusCode::EffectUnverified, 704},
    {StatusCode::RequestTargetMismatch, 705},
    {StatusCode::RequestFailed, 706},
    {StatusCode::TransportFailure, 707},
    {StatusCode::DispatchIndeterminate, 708},
    {StatusCode::AttemptExhausted, 709},
    {StatusCode::ResourceExhausted, 800},
    {StatusCode::BoundsExceeded, 801},
    {StatusCode::CheckpointUnavailable, 802},
    {StatusCode::ArithmeticOverflow, 803},
    {StatusCode::StoreNotFound, 900},
    {StatusCode::StoreCorrupt, 901},
    {StatusCode::StoreVersionUnsupported, 902},
    {StatusCode::StoreTruncated, 903},
    {StatusCode::StoreTrailingBytes, 904},
    {StatusCode::StoreIntegrityFailure, 905},
    {StatusCode::StoreLocked, 906},
    {StatusCode::StorePathInvalid, 907},
    {StatusCode::StoreIoError, 908},
    {StatusCode::StoreReadbackMismatch, 909},
    {StatusCode::NoAuthoritativeGeneration, 910},
    {StatusCode::ReplayDivergence, 911},
    {StatusCode::StoreAlreadyExists, 912},
    {StatusCode::RuntimeClosed, 1000},
    {StatusCode::ShutdownInProgress, 1001},
    {StatusCode::ReentrancyRefused, 1002},
    {StatusCode::NotSupported, 1003},
    {StatusCode::Internal, 1004},
    {StatusCode::ConcurrencyConflict, 1005},
};

}  // namespace

// ===========================================================================
// units.hpp -- checked arithmetic, time, ranges
// ===========================================================================

PFM_TEST(units_checked_add_i64_refuses_overflow_and_underflow) {
  PFM_CHECK_CODE(checked_add_i64(kI64Max, 1), StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_add_i64(kI64Max, kI64Max), StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_add_i64(1, kI64Max), StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_add_i64(kI64Min, -1), StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_add_i64(kI64Min, kI64Min), StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_add_i64(-2, kI64Min), StatusCode::ArithmeticOverflow);

  auto sum = checked_add_i64(kI64Max, 0);
  PFM_REQUIRE(sum.ok());
  PFM_CHECK_EQ(sum.value(), kI64Max);

  sum = checked_add_i64(kI64Max, -1);
  PFM_REQUIRE(sum.ok());
  PFM_CHECK_EQ(sum.value(), kI64Max - 1);

  sum = checked_add_i64(kI64Min, 0);
  PFM_REQUIRE(sum.ok());
  PFM_CHECK_EQ(sum.value(), kI64Min);

  sum = checked_add_i64(kI64Min, 1);
  PFM_REQUIRE(sum.ok());
  PFM_CHECK_EQ(sum.value(), kI64Min + 1);

  sum = checked_add_i64(-5, 12);
  PFM_REQUIRE(sum.ok());
  PFM_CHECK_EQ(sum.value(), 7);
}

PFM_TEST(units_checked_sub_i64_refuses_overflow_and_underflow) {
  PFM_CHECK_CODE(checked_sub_i64(kI64Min, 1), StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_sub_i64(kI64Max, -1), StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_sub_i64(0, kI64Min), StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_sub_i64(kI64Min, kI64Max), StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_sub_i64(1, kI64Min), StatusCode::ArithmeticOverflow);

  auto difference = checked_sub_i64(kI64Max, kI64Max);
  PFM_REQUIRE(difference.ok());
  PFM_CHECK_EQ(difference.value(), 0);

  // Subtracting INT64_MIN is refused even where the mathematical result is
  // representable, because the negation of the operand is not.
  PFM_CHECK_CODE(checked_sub_i64(kI64Min, kI64Min), StatusCode::ArithmeticOverflow);

  difference = checked_sub_i64(kI64Min, 0);
  PFM_REQUIRE(difference.ok());
  PFM_CHECK_EQ(difference.value(), kI64Min);

  difference = checked_sub_i64(3, 10);
  PFM_REQUIRE(difference.ok());
  PFM_CHECK_EQ(difference.value(), -7);

  difference = checked_sub_i64(kI64Max, 1);
  PFM_REQUIRE(difference.ok());
  PFM_CHECK_EQ(difference.value(), kI64Max - 1);
}

PFM_TEST(units_checked_mul_i64_refuses_overflow_and_underflow) {
  PFM_CHECK_CODE(checked_mul_i64(kI64Max, 2), StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_mul_i64(kI64Min, 2), StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_mul_i64(kI64Min, -1), StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_mul_i64(-1, kI64Min), StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_mul_i64(kI64Max, kI64Max), StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_mul_i64(kI64Min, kI64Min), StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_mul_i64(kI64Max, 3), StatusCode::ArithmeticOverflow);

  auto product = checked_mul_i64(kI64Min, 1);
  PFM_REQUIRE(product.ok());
  PFM_CHECK_EQ(product.value(), kI64Min);

  product = checked_mul_i64(kI64Max, 1);
  PFM_REQUIRE(product.ok());
  PFM_CHECK_EQ(product.value(), kI64Max);

  product = checked_mul_i64(kI64Max, 0);
  PFM_REQUIRE(product.ok());
  PFM_CHECK_EQ(product.value(), 0);

  product = checked_mul_i64(0, kI64Min);
  PFM_REQUIRE(product.ok());
  PFM_CHECK_EQ(product.value(), 0);

  product = checked_mul_i64(-3, -4);
  PFM_REQUIRE(product.ok());
  PFM_CHECK_EQ(product.value(), 12);

  product = checked_mul_i64(kI64Max, -1);
  PFM_REQUIRE(product.ok());
  PFM_CHECK_EQ(product.value(), kI64Min + 1);

  product = checked_mul_i64(3037000499LL, 3037000499LL);
  PFM_REQUIRE(product.ok());
  PFM_CHECK_EQ(product.value(), 9223372030926249001LL);
}

PFM_TEST(units_checked_u64_refuses_overflow) {
  PFM_CHECK_CODE(checked_add_u64(kU64Max, 1), StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_add_u64(kU64Max, kU64Max), StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_mul_u64(kU64Max, 2), StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_mul_u64(std::uint64_t{1} << 32, std::uint64_t{1} << 32),
                 StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_mul_u64(std::uint64_t{1} << 63, 2), StatusCode::ArithmeticOverflow);

  auto sum = checked_add_u64(kU64Max - 1, 1);
  PFM_REQUIRE(sum.ok());
  PFM_CHECK_EQ(sum.value(), kU64Max);

  sum = checked_add_u64(0, kU64Max);
  PFM_REQUIRE(sum.ok());
  PFM_CHECK_EQ(sum.value(), kU64Max);

  auto product = checked_mul_u64(kU64Max, 1);
  PFM_REQUIRE(product.ok());
  PFM_CHECK_EQ(product.value(), kU64Max);

  product = checked_mul_u64(0, kU64Max);
  PFM_REQUIRE(product.ok());
  PFM_CHECK_EQ(product.value(), std::uint64_t{0});

  product = checked_mul_u64(123456789, 1000000);
  PFM_REQUIRE(product.ok());
  PFM_CHECK_EQ(product.value(), std::uint64_t{123456789000000});
}

PFM_TEST(units_quantity_arithmetic_is_checked) {
  auto later = checked_add(Timestamp::from_value(1000), duration_from_millis(500));
  PFM_REQUIRE(later.ok());
  PFM_CHECK_EQ(later.value().value(), 1500);

  auto earlier = checked_sub(Timestamp::from_value(1000), duration_from_millis(500));
  PFM_REQUIRE(earlier.ok());
  PFM_CHECK_EQ(earlier.value().value(), 500);

  PFM_CHECK_CODE(checked_add(Timestamp::from_value(kI64Max), duration_from_millis(1)),
                 StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_sub(Timestamp::from_value(kI64Min), duration_from_millis(1)),
                 StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_sub(Timestamp::from_value(0), duration_from_millis(kI64Min)),
                 StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_add(Timestamp::from_value(kI64Min), Duration::from_value(-1)),
                 StatusCode::ArithmeticOverflow);

  auto total = checked_add(duration_from_seconds(2), duration_from_seconds(3));
  PFM_REQUIRE(total.ok());
  PFM_CHECK_EQ(total.value().value(), 5000);

  PFM_CHECK_CODE(checked_add(Duration::from_value(kI64Max), Duration::from_value(1)),
                 StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_sub(Duration::from_value(kI64Min), Duration::from_value(1)),
                 StatusCode::ArithmeticOverflow);

  auto scaled = checked_scale(duration_from_seconds(5), 3);
  PFM_REQUIRE(scaled.ok());
  PFM_CHECK_EQ(scaled.value().value(), 15000);

  PFM_CHECK_CODE(checked_scale(Duration::from_value(kI64Max), 2),
                 StatusCode::ArithmeticOverflow);
  PFM_CHECK_CODE(checked_scale(BasisPoints::from_value(-1), kI64Min),
                 StatusCode::ArithmeticOverflow);
  auto minimal = checked_scale(BasisPoints::from_value(1), kI64Min);
  PFM_REQUIRE(minimal.ok());
  PFM_CHECK_EQ(minimal.value().value(), kI64Min);
}

PFM_TEST(units_checked_difference_refuses_out_of_order_instants) {
  auto zero = checked_difference(Timestamp::from_value(500), Timestamp::from_value(500));
  PFM_REQUIRE(zero.ok());
  PFM_CHECK_EQ(zero.value().value(), 0);

  auto span = checked_difference(Timestamp::from_value(2000), Timestamp::from_value(1200));
  PFM_REQUIRE(span.ok());
  PFM_CHECK_EQ(span.value().value(), 800);

  PFM_CHECK_CODE(checked_difference(Timestamp::from_value(100), Timestamp::from_value(200)),
                 StatusCode::InvalidArgument);
  PFM_CHECK_CODE(checked_difference(Timestamp::from_value(kI64Min), Timestamp::from_value(kI64Max)),
                 StatusCode::ArithmeticOverflow);
}

PFM_TEST(units_duration_seconds_ceil_never_rounds_down_to_zero) {
  PFM_CHECK_EQ(duration_seconds_ceil(duration_from_millis(0)), 0);
  PFM_CHECK_EQ(duration_seconds_ceil(duration_from_millis(1)), 1);
  PFM_CHECK_EQ(duration_seconds_ceil(duration_from_millis(2)), 1);
  PFM_CHECK_EQ(duration_seconds_ceil(duration_from_millis(999)), 1);
  PFM_CHECK_EQ(duration_seconds_ceil(duration_from_millis(1000)), 1);
  PFM_CHECK_EQ(duration_seconds_ceil(duration_from_millis(1001)), 2);
  PFM_CHECK_EQ(duration_seconds_ceil(duration_from_millis(1500)), 2);
  PFM_CHECK_EQ(duration_seconds_ceil(duration_from_seconds(15)), 15);
  PFM_CHECK_EQ(duration_seconds_ceil(duration_from_seconds(60)), 60);
}

PFM_TEST(units_within_window_boundaries) {
  const Timestamp now = Timestamp::from_value(1'000'000);
  const Duration window = duration_from_millis(500);

  PFM_CHECK(within_window(now, now, window));
  PFM_CHECK(within_window(now, now, Duration::from_value(0)));
  PFM_CHECK(within_window(Timestamp::from_value(999'500), now, window));
  PFM_CHECK(!within_window(Timestamp::from_value(999'499), now, window));
  PFM_CHECK(within_window(Timestamp::from_value(1'000'500), now, window));
  PFM_CHECK(!within_window(Timestamp::from_value(1'000'501), now, window));
  PFM_CHECK(!within_window(now, now, duration_from_millis(-1)));
  PFM_CHECK(!within_window(Timestamp::from_value(kI64Min), Timestamp::from_value(kI64Max),
                           Duration::from_value(kI64Max)));
}

PFM_TEST(units_range_helpers_cover_the_full_scale_only) {
  PFM_CHECK_EQ(kBasisPointsFull, 10000);
  PFM_CHECK_EQ(kMilliPercentFull, 100000);
  PFM_CHECK_EQ(basis_points_full().value(), kBasisPointsFull);

  PFM_CHECK(!basis_points_in_range(basis_points_from_value(-1)));
  PFM_CHECK(basis_points_in_range(basis_points_from_value(0)));
  PFM_CHECK(basis_points_in_range(basis_points_from_value(1)));
  PFM_CHECK(basis_points_in_range(basis_points_from_value(9999)));
  PFM_CHECK(basis_points_in_range(basis_points_from_value(10000)));
  PFM_CHECK(!basis_points_in_range(basis_points_from_value(10001)));

  PFM_CHECK(!milli_percent_in_range(milli_percent_from_value(-1)));
  PFM_CHECK(milli_percent_in_range(milli_percent_from_value(0)));
  PFM_CHECK(milli_percent_in_range(milli_percent_from_value(50000)));
  PFM_CHECK(milli_percent_in_range(milli_percent_from_value(100000)));
  PFM_CHECK(!milli_percent_in_range(milli_percent_from_value(100001)));
}

// ===========================================================================
// ids.hpp -- typed identities and fingerprint rendering
// ===========================================================================

PFM_TEST(ids_tags_do_not_interoperate) {
  static_assert(!std::is_same_v<IncidentId, PolicyGeneration>);
  static_assert(!std::is_convertible_v<PolicyGeneration, IncidentId>);
  static_assert(!std::is_constructible_v<IncidentId, PolicyGeneration>);
  static_assert(!std::is_assignable_v<IncidentId&, PolicyGeneration>);
  static_assert(std::is_same_v<IncidentId::rep_type, std::uint64_t>);
  static_assert(!std::is_constructible_v<Fingerprint, IdempotencyKey>);
  static_assert(!std::is_convertible_v<IdempotencyKey, Fingerprint>);
  static_assert(!std::is_constructible_v<Timestamp, Duration>);
  static_assert(!std::is_constructible_v<Duration, Timestamp>);
  static_assert(!std::is_convertible_v<MilliPercent, BasisPoints>);
  static_assert(!std::is_constructible_v<BasisPoints, MilliPercent>);

  // Same numeric value, deliberately different types.
  const IncidentId incident = IncidentId::from_value(7);
  const PolicyGeneration policy_generation = PolicyGeneration::from_value(7);
  PFM_CHECK_EQ(incident.value(), policy_generation.value());
  static_assert(!std::is_same_v<decltype(incident), decltype(policy_generation)>);
}

PFM_TEST(ids_is_set_and_is_absent) {
  const IncidentId absent{};
  PFM_CHECK(absent.is_absent());
  PFM_CHECK(!absent.is_set());
  PFM_CHECK_EQ(absent.value(), std::uint64_t{0});

  const IncidentId present = IncidentId::from_value(42);
  PFM_CHECK(present.is_set());
  PFM_CHECK(!present.is_absent());
  PFM_CHECK_EQ(present.value(), std::uint64_t{42});

  PFM_CHECK(IncidentId::from_value(0).is_absent());
  PFM_CHECK(IncidentId::from_value(1).is_set());
  PFM_CHECK(IncidentId::from_value(7) == IncidentId::from_value(7));
  PFM_CHECK(IncidentId::from_value(7) < IncidentId::from_value(8));
  PFM_CHECK(IncidentId::from_value(8) > IncidentId::from_value(7));

  PFM_CHECK(!Fingerprint{}.is_set());
  PFM_CHECK((Fingerprint{0, 1}.is_set()));
  PFM_CHECK((Fingerprint{1, 0}.is_set()));
  PFM_CHECK(!IdempotencyKey{}.is_set());
  PFM_CHECK((IdempotencyKey{0, 1}.is_set()));
  PFM_CHECK((IdempotencyKey{1, 0}.is_set()));
}

PFM_TEST(ids_fingerprint_to_hex_is_canonical_32_lowercase_hex) {
  const Fingerprint fingerprint{0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
  const std::string hex = fingerprint.to_hex();
  PFM_CHECK_EQ(hex.size(), std::size_t{32});
  PFM_CHECK_EQ(hex, std::string{"0123456789abcdeffedcba9876543210"});
  for (const char character : hex) {
    PFM_CHECK((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'));
  }
  PFM_CHECK_EQ(Fingerprint{}.to_hex(), std::string(32, '0'));
  PFM_CHECK_EQ((Fingerprint{0, 0xFF}.to_hex()),
               std::string{"000000000000000000000000000000ff"});
  PFM_CHECK_EQ((Fingerprint{0xFF, 0}.to_hex()),
               std::string{"00000000000000ff0000000000000000"});
}

PFM_TEST(ids_idempotency_key_round_trips_through_a_fingerprint) {
  const Fingerprint fingerprint{0x1111222233334444ULL, 0x5555666677778888ULL};
  const IdempotencyKey key = IdempotencyKey::from_fingerprint(fingerprint);
  PFM_CHECK_EQ(key.high(), fingerprint.high());
  PFM_CHECK_EQ(key.low(), fingerprint.low());
  PFM_CHECK_EQ(key.to_hex(), fingerprint.to_hex());
  PFM_CHECK(key.as_fingerprint() == fingerprint);
  PFM_CHECK_EQ(IdempotencyKey{}.to_hex(), std::string(32, '0'));
  PFM_CHECK((IdempotencyKey{0, 1} < IdempotencyKey{0, 2}));
  PFM_CHECK((IdempotencyKey{0, 1} < IdempotencyKey{1, 0}));
  PFM_CHECK(less_by_value(IdempotencyKey{0, 1}, IdempotencyKey{0, 2}));
  PFM_CHECK(!less_by_value(IdempotencyKey{0, 2}, IdempotencyKey{0, 1}));
}

// ===========================================================================
// refs.hpp -- canonical opaque reference tokens
// ===========================================================================

PFM_TEST(refs_make_rejects_names_that_are_not_opaque_tokens) {
  PFM_CHECK_CODE(RefToken::make(RefKind::UpsUnit, ""), StatusCode::EmptyField);
  PFM_CHECK_CODE(RefToken::make(RefKind::UpsUnit, std::string(97, 'a')),
                 StatusCode::FieldTooLong);
  PFM_CHECK_CODE(RefToken::make(RefKind::Unknown, "ups-0-0"), StatusCode::InvalidArgument);

  for (const std::string_view name : {"a/b", "a\\b", "a b", "a;b", "a,b", "a|b", "a*b",
                                      "a?b", "a<b", "a\"b", "a'b", "..", "a..b", "../a",
                                      "-lead", "_lead", ".lead", ":lead", "trail-", "trail_",
                                      "trail.", "trail:", "\x01""a", "a\n", "a\t",
                                      "\xC3\xA9"}) {
    const auto made = RefToken::make(RefKind::UpsUnit, name);
    if (made.ok()) {
      ::pfmtest::report_failure(__FILE__, __LINE__,
                                std::string{"accepted a non-canonical name: "} +
                                    std::string{name});
      continue;
    }
    if (made.status().code() != StatusCode::InvalidCharacter) {
      ::pfmtest::report_failure(
          __FILE__, __LINE__,
          std::string{"name "} + std::string{name} + " produced " +
              std::string{made.status().to_string()} + " instead of pfm.invalid_character");
    }
  }
}

PFM_TEST(refs_make_accepts_canonical_names_including_the_length_bound) {
  const auto ups = RefToken::make(RefKind::UpsUnit, "ups-0-0");
  PFM_REQUIRE(ups.ok());
  PFM_CHECK(ups.value().is_set());
  PFM_CHECK_EQ(ups.value().kind(), RefKind::UpsUnit);
  PFM_CHECK_EQ(ups.value().name(), std::string{"ups-0-0"});

  const auto upper = RefToken::make(RefKind::Pdu, "PDU.Main_A");
  PFM_CHECK(upper.ok());

  const auto punctuation = RefToken::make(RefKind::Circuit, "a@b#c:d");
  PFM_CHECK(punctuation.ok());

  const auto single = RefToken::make(RefKind::Rack, "a");
  PFM_CHECK(single.ok());

  const std::string longest(kMaxRefNameLength, 'z');
  const auto at_bound = RefToken::make(RefKind::Rack, longest);
  PFM_REQUIRE(at_bound.ok());
  PFM_CHECK_EQ(at_bound.value().name().size(), kMaxRefNameLength);

  const std::string too_long(kMaxRefNameLength + 1, 'z');
  PFM_CHECK_CODE(RefToken::make(RefKind::Rack, too_long), StatusCode::FieldTooLong);

  const RefToken unset{};
  PFM_CHECK(!unset.is_set());
  PFM_CHECK_EQ(std::string{unset.to_string()}, std::string{"unknown:"});
  PFM_CHECK(unset.name().empty());
  PFM_CHECK_EQ(unset.kind(), RefKind::Unknown);
}

PFM_TEST(refs_parse_round_trips_to_string) {
  const auto made = RefToken::make(RefKind::PduBranch, "branch-0-0-1");
  PFM_REQUIRE(made.ok());
  PFM_CHECK_EQ(std::string{made.value().to_string()}, std::string{"pdu-branch:branch-0-0-1"});

  const auto parsed = RefToken::parse(made.value().to_string());
  PFM_REQUIRE(parsed.ok());
  PFM_CHECK(parsed.value() == made.value());
  PFM_CHECK_EQ(parsed.value().kind(), RefKind::PduBranch);
  PFM_CHECK_EQ(parsed.value().name(), std::string{"branch-0-0-1"});

  const auto minimal = RefToken::parse("rack:a");
  PFM_REQUIRE(minimal.ok());
  PFM_CHECK_EQ(minimal.value().kind(), RefKind::Rack);
  PFM_CHECK_EQ(minimal.value().name(), std::string{"a"});

  PFM_CHECK_CODE(RefToken::parse(""), StatusCode::MalformedEncoding);
  PFM_CHECK_CODE(RefToken::parse("ups"), StatusCode::MalformedEncoding);
  PFM_CHECK_CODE(RefToken::parse(":ups-0-0"), StatusCode::MalformedEncoding);
  PFM_CHECK_CODE(RefToken::parse("ups:"), StatusCode::MalformedEncoding);
  PFM_CHECK_CODE(RefToken::parse("ups-0-0:"), StatusCode::MalformedEncoding);
  PFM_CHECK_CODE(RefToken::parse("bogus:name"), StatusCode::UnknownEnumValue);
  PFM_CHECK_CODE(RefToken::parse("UPS:name"), StatusCode::UnknownEnumValue);
  PFM_CHECK_CODE(RefToken::parse("unknown:name"), StatusCode::InvalidArgument);
  PFM_CHECK_CODE(RefToken::parse("ups:a/b"), StatusCode::InvalidCharacter);
  PFM_CHECK_CODE(RefToken::parse("ups:" + std::string(kMaxRefNameLength + 1, 'a')),
                 StatusCode::FieldTooLong);
  PFM_CHECK_CODE(RefToken::parse(std::string(kMaxRefTextLength + 1, 'a')),
                 StatusCode::FieldTooLong);
}

PFM_TEST(refs_order_by_kind_then_name) {
  const RefToken bus_a = ref_of(RefKind::Bus, "a");
  const RefToken bus_z = ref_of(RefKind::Bus, "z");
  const RefToken pdu_a = ref_of(RefKind::Pdu, "a");
  const RefToken pdu_b = ref_of(RefKind::Pdu, "b");
  const RefToken feed = ref_of(RefKind::UtilityFeed, "a");

  PFM_CHECK(bus_a < bus_z);
  PFM_CHECK(bus_a < pdu_a);
  PFM_CHECK(pdu_a < pdu_b);
  PFM_CHECK(feed < bus_a);
  PFM_CHECK(pdu_b > bus_z);
  PFM_CHECK(bus_a <= bus_a);
  PFM_CHECK(bus_a >= bus_a);
  PFM_CHECK(!(bus_a > bus_a));
  PFM_CHECK(bus_a == ref_of(RefKind::Bus, "a"));
  PFM_CHECK(!(bus_a == bus_z));
  PFM_CHECK(!(bus_a == ref_of(RefKind::Breaker, "a")));

  std::vector<RefToken> values{pdu_b, bus_z, pdu_a, feed, bus_a};
  std::sort(values.begin(), values.end());
  PFM_CHECK(is_canonical_order(values));
  PFM_CHECK_REF(values[0], feed);
  PFM_CHECK_REF(values[1], bus_a);
  PFM_CHECK_REF(values[2], bus_z);
  PFM_CHECK_REF(values[3], pdu_a);
  PFM_CHECK_REF(values[4], pdu_b);
}

PFM_TEST(refs_kind_names_are_stable_and_classified) {
  PFM_CHECK_EQ(std::string{ref_kind_name(RefKind::UpsUnit)}, std::string{"ups"});
  PFM_CHECK_EQ(std::string{ref_kind_name(RefKind::PduBranch)}, std::string{"pdu-branch"});
  PFM_CHECK_EQ(std::string{ref_kind_name(RefKind::FailureDomain)},
               std::string{"failure-domain"});
  PFM_CHECK_EQ(std::string{ref_kind_name(RefKind::Unknown)}, std::string{"unknown"});

  RefKind kind = RefKind::Unknown;
  PFM_CHECK(ref_kind_from_name("generator", kind));
  PFM_CHECK_EQ(kind, RefKind::Generator);
  PFM_CHECK(ref_kind_from_name("load-group", kind));
  PFM_CHECK_EQ(kind, RefKind::LoadGroup);

  RefKind untouched = RefKind::Site;
  PFM_CHECK(!ref_kind_from_name("bogus", untouched));
  PFM_CHECK_EQ(untouched, RefKind::Site);

  PFM_CHECK(ref_kind_is_element(RefKind::Bus));
  PFM_CHECK(ref_kind_is_element(RefKind::Breaker));
  PFM_CHECK(ref_kind_is_element(RefKind::Generator));
  PFM_CHECK(!ref_kind_is_element(RefKind::Site));
  PFM_CHECK(!ref_kind_is_element(RefKind::FailureDomain));
  PFM_CHECK(!ref_kind_is_element(RefKind::LoadGroup));
  PFM_CHECK(!ref_kind_is_element(RefKind::Unknown));

  PFM_CHECK(ref_name_is_canonical("ups-0-0"));
  PFM_CHECK(!ref_name_is_canonical(""));
  PFM_CHECK(!ref_name_is_canonical("a/b"));
  PFM_CHECK(!ref_name_is_canonical(".."));
  PFM_CHECK(!ref_name_is_canonical(std::string(kMaxRefNameLength + 1, 'a')));
}

// ===========================================================================
// status.hpp -- the machine-readable contract
// ===========================================================================

PFM_TEST(status_names_are_stable_and_unique) {
  std::set<std::string> names;
  std::set<std::uint16_t> values;
  for (const auto& entry : kAssignedCodes) {
    PFM_CHECK_EQ(static_cast<std::uint16_t>(entry.code), entry.value);
    const std::string name{status_code_name(entry.code)};
    PFM_CHECK(name.rfind("pfm.", 0) == 0);
    PFM_CHECK_EQ(names.count(name), std::size_t{0});
    PFM_CHECK_EQ(values.count(entry.value), std::size_t{0});
    names.insert(name);
    values.insert(entry.value);
  }
  PFM_CHECK_EQ(names.size(), std::size(kAssignedCodes));
  PFM_CHECK_EQ(values.size(), std::size(kAssignedCodes));
  static_assert(std::size(kAssignedCodes) >= 80,
                "the table must enumerate every assigned status code");

  PFM_CHECK_EQ(std::string{status_code_name(StatusCode::Ok)}, std::string{"pfm.ok"});
  PFM_CHECK_EQ(std::string{status_code_name(StatusCode::ArithmeticOverflow)},
               std::string{"pfm.arithmetic_overflow"});
  PFM_CHECK_EQ(std::string{status_code_name(StatusCode::StaleEpoch)},
               std::string{"pfm.stale_epoch"});
  PFM_CHECK_EQ(std::string{status_code_name(StatusCode::TopologyCycle)},
               std::string{"pfm.topology_cycle"});
  PFM_CHECK_EQ(std::string{status_code_name(StatusCode::IdempotencyConflict)},
               std::string{"pfm.idempotency_conflict"});
  PFM_CHECK_EQ(std::string{status_code_name(StatusCode::StoreIntegrityFailure)},
               std::string{"pfm.store_integrity_failure"});
  PFM_CHECK_EQ(std::string{status_code_name(static_cast<StatusCode>(2999))},
               std::string{"pfm.unassigned"});
}

PFM_TEST(status_code_from_value_round_trips_and_refuses_unassigned_values) {
  for (const auto& entry : kAssignedCodes) {
    const auto decoded = status_code_from_value(entry.value);
    PFM_REQUIRE(decoded.has_value());
    PFM_CHECK(decoded.value() == entry.code);
    PFM_CHECK_EQ(static_cast<std::uint16_t>(decoded.value()), entry.value);
  }

  const std::array<std::uint16_t, 14> unassigned{
      {1, 4, 50, 110, 199, 211, 310, 409, 507, 606, 710, 804, 913, 65535}};
  for (const std::uint16_t value : unassigned) {
    PFM_CHECK(!status_code_from_value(value).has_value());
  }
  PFM_CHECK(!status_code_from_value(static_cast<std::uint16_t>(StatusCode::Ok) + 1).has_value());
}

PFM_TEST(status_to_string_carries_the_code_name_and_context) {
  const Status success = Status::success();
  PFM_CHECK(success.ok());
  PFM_CHECK(status_ok(success.code()));
  PFM_CHECK_EQ(success.to_string(), std::string{"pfm.ok"});
  PFM_CHECK(success.message().empty());
  PFM_CHECK(success.context().empty());

  const Status failure =
      Status::error(StatusCode::StaleEpoch, "epoch is older than the controller");
  PFM_CHECK(!failure.ok());
  PFM_CHECK(!status_ok(failure.code()));
  PFM_CHECK_EQ(failure.code(), StatusCode::StaleEpoch);
  PFM_CHECK_EQ(std::string{failure.code_name()}, std::string{"pfm.stale_epoch"});
  const std::string rendered = failure.to_string();
  PFM_CHECK(rendered.find("pfm.stale_epoch") != std::string::npos);
  PFM_CHECK(rendered.find("epoch is older than the controller") != std::string::npos);

  Status contextual = Status::error(StatusCode::EmptyField, "reference name is empty");
  contextual.with_context("ref.name");
  PFM_CHECK_EQ(std::string{contextual.context()}, std::string{"ref.name"});
  PFM_CHECK(contextual.to_string().find("[ref.name]") != std::string::npos);

  PFM_CHECK(status_ok(StatusCode::Ok));
  PFM_CHECK(!status_ok(StatusCode::Internal));
}

// ===========================================================================
// policy.hpp -- validation and tolerance bands
// ===========================================================================

PFM_TEST(policy_defaults_and_default_bounds_are_valid) {
  const ElectricalPolicy policy = default_policy();
  PFM_CHECK_OK(validate(policy));
  PFM_CHECK_OK(validate(Bounds{}));

  PFM_CHECK_EQ(policy.evidence_freshness_window.value(), 15'000);
  PFM_CHECK_EQ(policy.evidence_expiry_window.value(), 60'000);
  PFM_CHECK(policy.evidence_freshness_window < policy.evidence_expiry_window);
  PFM_CHECK(policy.reserve_critical_floor < policy.reserve_floor);
  PFM_CHECK(milli_percent_in_range(policy.reserve_floor));
  PFM_CHECK(milli_percent_in_range(policy.reserve_critical_floor));
  PFM_CHECK(policy.nominal_voltage.value() > 0);
  PFM_CHECK(policy.nominal_frequency.value() > 0);
  PFM_CHECK_EQ(policy.max_attempts_per_request, 3u);
  PFM_CHECK(policy.require_verified_isolation_for_recovery);
}

PFM_TEST(policy_rejects_negative_durations) {
  ElectricalPolicy policy = default_policy();
  policy.evidence_freshness_window = duration_from_millis(-1);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.evidence_expiry_window = Duration::from_value(kI64Min);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.acknowledgement_window = duration_from_millis(-1);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.verification_window = duration_from_millis(-1);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.stability_dwell = duration_from_millis(-1);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.generator_start_window = duration_from_millis(-1);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.synchronization_window = duration_from_millis(-1);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.retransfer_hold = duration_from_millis(-1);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.reclose_dwell = duration_from_millis(-1);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.verification_validity = duration_from_millis(-1);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);
}

PFM_TEST(policy_rejects_freshness_beyond_expiry) {
  ElectricalPolicy policy = default_policy();
  policy.evidence_freshness_window = duration_from_seconds(61);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.evidence_expiry_window = duration_from_seconds(15);
  PFM_CHECK_OK(validate(policy));

  policy.evidence_expiry_window = duration_from_seconds(14);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);
}

PFM_TEST(policy_rejects_invalid_reserve_floors) {
  ElectricalPolicy policy = default_policy();
  policy.reserve_critical_floor = milli_percent_from_value(50001);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.reserve_floor = milli_percent_from_value(100001);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.reserve_critical_floor = milli_percent_from_value(-1);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.reserve_floor = milli_percent_from_value(100000);
  policy.reserve_critical_floor = milli_percent_from_value(100000);
  PFM_CHECK_OK(validate(policy));

  policy = default_policy();
  policy.reserve_floor = milli_percent_from_value(0);
  policy.reserve_critical_floor = milli_percent_from_value(0);
  PFM_CHECK_OK(validate(policy));
}

PFM_TEST(policy_rejects_zero_nominal_values_and_bad_tolerances) {
  ElectricalPolicy policy = default_policy();
  policy.nominal_voltage = MilliVolts::from_value(0);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.nominal_voltage = MilliVolts::from_value(-230000);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.nominal_frequency = MilliHertz::from_value(0);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.nominal_frequency = MilliHertz::from_value(-1);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.voltage_tolerance = milli_percent_from_value(0);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.voltage_tolerance = milli_percent_from_value(50000);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.voltage_tolerance = milli_percent_from_value(49999);
  PFM_CHECK_OK(validate(policy));

  policy = default_policy();
  policy.voltage_tolerance = milli_percent_from_value(100001);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.frequency_tolerance = milli_percent_from_value(0);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.frequency_tolerance = milli_percent_from_value(50000);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy = default_policy();
  policy.frequency_tolerance = milli_percent_from_value(100000);
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);
}

PFM_TEST(policy_rejects_an_unusable_attempt_bound) {
  ElectricalPolicy policy = default_policy();
  policy.max_attempts_per_request = 0;
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy.max_attempts_per_request = 17;
  PFM_CHECK_CODE(validate(policy), StatusCode::InvalidArgument);

  policy.max_attempts_per_request = 1;
  PFM_CHECK_OK(validate(policy));

  policy.max_attempts_per_request = 16;
  PFM_CHECK_OK(validate(policy));
}

PFM_TEST(policy_bounds_validation_refusals) {
  Bounds bounds{};
  PFM_CHECK_OK(validate(bounds));

  bounds.max_topology_elements = 0;
  PFM_CHECK_CODE(validate(bounds), StatusCode::InvalidArgument);

  bounds = Bounds{};
  bounds.max_observations = 0;
  PFM_CHECK_CODE(validate(bounds), StatusCode::InvalidArgument);

  bounds = Bounds{};
  bounds.max_note_length = 7;
  PFM_CHECK_CODE(validate(bounds), StatusCode::InvalidArgument);

  bounds = Bounds{};
  bounds.max_note_length = 4097;
  PFM_CHECK_CODE(validate(bounds), StatusCode::InvalidArgument);

  bounds = Bounds{};
  bounds.max_label_length = 8;
  bounds.max_provenance_length = 4096;
  PFM_CHECK_OK(validate(bounds));
}

PFM_TEST(policy_tolerance_band_helpers_at_exact_boundaries) {
  const ElectricalPolicy policy = default_policy();

  auto voltage_limit = voltage_deviation_limit(policy);
  PFM_REQUIRE(voltage_limit.ok());
  PFM_CHECK_EQ(voltage_limit.value().value(), 23'000);

  auto frequency_limit = frequency_deviation_limit(policy);
  PFM_REQUIRE(frequency_limit.ok());
  PFM_CHECK_EQ(frequency_limit.value().value(), 500);

  const auto voltage_ok = [&policy](std::int64_t reading) {
    const auto result = voltage_within_tolerance(policy, MilliVolts::from_value(reading));
    return result.ok() && result.value();
  };
  PFM_CHECK(voltage_ok(230'000));
  PFM_CHECK(voltage_ok(253'000));
  PFM_CHECK(!voltage_ok(253'001));
  PFM_CHECK(voltage_ok(207'000));
  PFM_CHECK(!voltage_ok(206'999));
  PFM_CHECK(!voltage_ok(0));
  PFM_CHECK(!voltage_ok(-1));

  const auto frequency_ok = [&policy](std::int64_t reading) {
    const auto result = frequency_within_tolerance(policy, MilliHertz::from_value(reading));
    return result.ok() && result.value();
  };
  PFM_CHECK(frequency_ok(50'000));
  PFM_CHECK(frequency_ok(50'500));
  PFM_CHECK(!frequency_ok(50'501));
  PFM_CHECK(frequency_ok(49'500));
  PFM_CHECK(!frequency_ok(49'499));
  PFM_CHECK(!frequency_ok(0));

  // The deviation limit is truncated toward zero, never rounded up.
  ElectricalPolicy odd = default_policy();
  odd.nominal_voltage = MilliVolts::from_value(230'001);
  auto odd_limit = voltage_deviation_limit(odd);
  PFM_REQUIRE(odd_limit.ok());
  PFM_CHECK_EQ(odd_limit.value().value(), 23'000);

  // An invalid policy is refused by the helpers rather than silently applied.
  ElectricalPolicy broken = default_policy();
  broken.voltage_tolerance = milli_percent_from_value(0);
  PFM_CHECK_CODE(voltage_deviation_limit(broken), StatusCode::InvalidArgument);
  PFM_CHECK_CODE(voltage_within_tolerance(broken, MilliVolts::from_value(230'000)),
                 StatusCode::InvalidArgument);
  PFM_CHECK_CODE(frequency_deviation_limit(broken), StatusCode::InvalidArgument);
}

// ===========================================================================
// obligations.hpp -- validation and recovery blocking
// ===========================================================================

PFM_TEST(obligations_validate_refusals) {
  const Bounds bounds{};

  ProtectedObligation obligation = make_obligation();
  PFM_CHECK_OK(validate(obligation, bounds));

  obligation = make_obligation();
  obligation.target = ref_of(RefKind::Breaker, "brk-bus-0-0");
  PFM_CHECK_OK(validate(obligation, bounds));

  obligation = make_obligation();
  obligation.target = ref_of(RefKind::Rack, "rack-0-0");
  PFM_CHECK_OK(validate(obligation, bounds));

  obligation = make_obligation();
  obligation.id = ObligationId{};
  PFM_CHECK_CODE(validate(obligation, bounds), StatusCode::InvalidArgument);

  obligation = make_obligation();
  obligation.target = RefToken{};
  PFM_CHECK_CODE(validate(obligation, bounds), StatusCode::InvalidArgument);

  obligation = make_obligation();
  obligation.target = ref_of(RefKind::Site, "site-a");
  PFM_CHECK_CODE(validate(obligation, bounds), StatusCode::InvalidArgument);

  obligation = make_obligation();
  obligation.target = ref_of(RefKind::FailureDomain, "fd-a");
  PFM_CHECK_CODE(validate(obligation, bounds), StatusCode::InvalidArgument);

  obligation = make_obligation();
  obligation.reporting_authority = RefToken{};
  PFM_CHECK_CODE(validate(obligation, bounds), StatusCode::InvalidArgument);

  obligation = make_obligation();
  obligation.has_reserve_floor = true;
  obligation.reserve_floor = milli_percent_from_value(100001);
  PFM_CHECK_CODE(validate(obligation, bounds), StatusCode::ValueOutOfRange);

  obligation = make_obligation();
  obligation.has_max_outage = true;
  obligation.max_outage = Duration::from_value(-1);
  PFM_CHECK_CODE(validate(obligation, bounds), StatusCode::ValueOutOfRange);

  obligation = make_obligation();
  obligation.reported_at = Timestamp{};
  PFM_CHECK_CODE(validate(obligation, bounds), StatusCode::InvalidArgument);

  obligation = make_obligation();
  obligation.status = ObligationStatus::Unknown;
  PFM_CHECK_OK(validate(obligation, bounds));

  obligation = make_obligation();
  obligation.has_report = false;
  obligation.reported_at = Timestamp{};
  PFM_CHECK_OK(validate(obligation, bounds));
}

PFM_TEST(obligations_block_recovery_for_unreported_unknown_at_risk_and_violated) {
  const ElectricalPolicy policy = default_policy();
  const Timestamp now = Timestamp::from_value(1'000'000);

  ProtectedObligation obligation = make_obligation();
  obligation.reported_at = now;
  obligation.status = ObligationStatus::Satisfied;
  PFM_CHECK(!obligation_blocks_recovery(obligation, now, policy));

  obligation.status = ObligationStatus::AtRisk;
  PFM_CHECK(obligation_blocks_recovery(obligation, now, policy));

  obligation.status = ObligationStatus::Violated;
  PFM_CHECK(obligation_blocks_recovery(obligation, now, policy));

  obligation.status = ObligationStatus::Unknown;
  PFM_CHECK(obligation_blocks_recovery(obligation, now, policy));

  obligation.status = ObligationStatus::Unreported;
  PFM_CHECK(obligation_blocks_recovery(obligation, now, policy));

  obligation = make_obligation();
  obligation.has_report = false;
  obligation.status = ObligationStatus::Satisfied;
  PFM_CHECK(obligation_blocks_recovery(obligation, now, policy));
}

PFM_TEST(obligations_block_recovery_when_the_report_is_expired_or_future) {
  const ElectricalPolicy policy = default_policy();
  const Timestamp now = Timestamp::from_value(1'000'000);

  ProtectedObligation obligation = make_obligation();
  obligation.status = ObligationStatus::Satisfied;

  obligation.reported_at = Timestamp::from_value(now.value() - 10'000);
  PFM_CHECK(!obligation_blocks_recovery(obligation, now, policy));

  // Exactly at the freshness window the report is still current.
  obligation.reported_at =
      Timestamp::from_value(now.value() - policy.evidence_freshness_window.value());
  PFM_CHECK(!obligation_blocks_recovery(obligation, now, policy));

  // Past the freshness window the report is stale, and a stale report is not a
  // report: recovery waits for the owning authority to reaffirm it.
  obligation.reported_at = Timestamp::from_value(now.value() - 30'000);
  PFM_CHECK(obligation_blocks_recovery(obligation, now, policy));

  // At the expiry boundary it is still stale rather than expired, and it blocks
  // either way.
  obligation.reported_at = Timestamp::from_value(now.value() - 60'000);
  PFM_CHECK(obligation_blocks_recovery(obligation, now, policy));

  obligation.reported_at = Timestamp::from_value(now.value() - 60'001);
  PFM_CHECK(obligation_blocks_recovery(obligation, now, policy));

  obligation.reported_at = Timestamp::from_value(now.value() - 120'000);
  PFM_CHECK(obligation_blocks_recovery(obligation, now, policy));

  obligation.reported_at = Timestamp::from_value(now.value() + 1);
  PFM_CHECK(obligation_blocks_recovery(obligation, now, policy));
}

PFM_TEST(obligations_relaxable_only_below_regulatory) {
  PFM_CHECK(!obligation_relaxable(ProtectionClass::HardSafetyInterlock));
  PFM_CHECK(!obligation_relaxable(ProtectionClass::Regulatory));
  PFM_CHECK(obligation_relaxable(ProtectionClass::ServiceLevel));
  PFM_CHECK(obligation_relaxable(ProtectionClass::AdvisoryOptimization));

  PFM_CHECK_EQ(std::string{protection_class_name(ProtectionClass::HardSafetyInterlock)},
               std::string{"hard-safety-interlock"});
  PFM_CHECK_EQ(std::string{protection_class_name(ProtectionClass::Regulatory)},
               std::string{"regulatory"});
  PFM_CHECK_EQ(std::string{protection_class_name(ProtectionClass::ServiceLevel)},
               std::string{"service-level"});
  PFM_CHECK_EQ(std::string{protection_class_name(ProtectionClass::AdvisoryOptimization)},
               std::string{"advisory-optimization"});

  PFM_CHECK_EQ(std::string{obligation_status_name(ObligationStatus::Unreported)},
               std::string{"unreported"});
  PFM_CHECK_EQ(std::string{obligation_status_name(ObligationStatus::Satisfied)},
               std::string{"satisfied"});
  PFM_CHECK_EQ(std::string{obligation_status_name(ObligationStatus::AtRisk)},
               std::string{"at-risk"});
  PFM_CHECK_EQ(std::string{obligation_status_name(ObligationStatus::Violated)},
               std::string{"violated"});
  PFM_CHECK_EQ(std::string{obligation_status_name(ObligationStatus::Unknown)},
               std::string{"unknown"});
}

// ===========================================================================
// codec.hpp -- canonical encoding primitives
// ===========================================================================

PFM_TEST(codec_crc32c_known_vectors) {
  PFM_CHECK_EQ(codec::crc32c(as_bytes(std::string{"123456789"})),
               std::uint32_t{0xE3069283});
  PFM_CHECK_EQ(codec::crc32c(as_bytes(std::string{})), std::uint32_t{0});
  PFM_CHECK_EQ(codec::crc32c(as_bytes(std::string{"The quick brown fox jumps over the lazy dog"})),
               std::uint32_t{0x22620404});
  PFM_CHECK_EQ(codec::crc32c(as_bytes(std::string{"a"})), std::uint32_t{0xC1D04330});
}

PFM_TEST(codec_crc32c_incremental_seed_matches_one_shot) {
  const std::string text = "power-failure-manager";
  const std::uint32_t one_shot = codec::crc32c(as_bytes(text));
  for (std::size_t split = 0; split <= text.size(); ++split) {
    const std::string head = text.substr(0, split);
    const std::string tail = text.substr(split);
    const std::uint32_t seed = codec::crc32c(0xFFFFFFFFu, as_bytes(head));
    const std::uint32_t combined = codec::crc32c(seed, as_bytes(tail)) ^ 0xFFFFFFFFu;
    PFM_CHECK_EQ(combined, one_shot);
  }
  PFM_CHECK_EQ(codec::crc32c(0xFFFFFFFFu, as_bytes(std::string{})) ^ 0xFFFFFFFFu,
               std::uint32_t{0});
}

PFM_TEST(codec_fnv1a128_is_deterministic_and_sensitive) {
  const Fingerprint empty = codec::fnv1a128(as_bytes(std::string{}));
  PFM_CHECK_EQ(empty.high(), 0x6C62272E07BB0142ULL);
  PFM_CHECK_EQ(empty.low(), 0x62B821756295C58DULL);

  const Fingerprint single = codec::fnv1a128(as_bytes(std::string{"a"}));
  PFM_CHECK_EQ(single.high(), 0xD228CB696F1A8CAFULL);
  PFM_CHECK_EQ(single.low(), 0x78912B704E4A8964ULL);

  const Fingerprint digits = codec::fnv1a128(as_bytes(std::string{"123456789"}));
  PFM_CHECK_EQ(digits.high(), 0xDA2D42A08D04E458ULL);
  PFM_CHECK_EQ(digits.low(), 0x5DD325117F71D504ULL);

  const Fingerprint first = codec::fnv1a128(as_bytes(std::string{"pfm"}));
  const Fingerprint again = codec::fnv1a128(as_bytes(std::string{"pfm"}));
  const Fingerprint changed = codec::fnv1a128(as_bytes(std::string{"pfn"}));
  const Fingerprint extended = codec::fnv1a128(as_bytes(std::string{"pfm "}));
  PFM_CHECK(first == again);
  PFM_CHECK(!(first == changed));
  PFM_CHECK(!(first == extended));
  PFM_CHECK(!(changed == extended));
  PFM_CHECK(first.is_set());

  const Fingerprint prefix = codec::fnv1a128(as_bytes(std::string{"a"}));
  const Fingerprint with_zero = codec::fnv1a128(as_bytes(std::string{"a\0", 2}));
  PFM_CHECK(!(prefix == with_zero));
}

PFM_TEST(codec_writer_and_reader_round_trip_little_endian) {
  codec::Writer writer;
  writer.u8(0xAB);
  writer.u16(0x1234);
  writer.u32(0xDEADBEEFu);
  writer.u64(0x0123456789ABCDEFULL);
  writer.i64(-42);
  writer.flag(true);
  writer.flag(false);
  PFM_REQUIRE(writer.ok());
  PFM_CHECK(writer.text("hello", 16));
  PFM_REQUIRE(writer.ok());

  const std::vector<std::uint8_t>& bytes = writer.bytes();
  PFM_CHECK_EQ(bytes.size(), std::size_t{34});
  PFM_CHECK_EQ(bytes[0], std::uint8_t{0xAB});
  PFM_CHECK_EQ(bytes[1], std::uint8_t{0x34});
  PFM_CHECK_EQ(bytes[2], std::uint8_t{0x12});
  PFM_CHECK_EQ(bytes[3], std::uint8_t{0xEF});
  PFM_CHECK_EQ(bytes[4], std::uint8_t{0xBE});
  PFM_CHECK_EQ(bytes[5], std::uint8_t{0xAD});
  PFM_CHECK_EQ(bytes[6], std::uint8_t{0xDE});
  PFM_CHECK_EQ(bytes[7], std::uint8_t{0xEF});
  PFM_CHECK_EQ(bytes[15], std::uint8_t{0xD6});
  PFM_CHECK_EQ(bytes[22], std::uint8_t{0xFF});
  PFM_CHECK_EQ(bytes[23], std::uint8_t{1});
  PFM_CHECK_EQ(bytes[24], std::uint8_t{0});
  PFM_CHECK_EQ(bytes[25], std::uint8_t{5});
  PFM_CHECK_EQ(bytes[26], std::uint8_t{0});
  PFM_CHECK_EQ(bytes[29], std::uint8_t{'h'});

  codec::Reader reader{writer.bytes()};
  PFM_CHECK_EQ(reader.u8(), std::uint8_t{0xAB});
  PFM_CHECK_EQ(reader.u16(), std::uint16_t{0x1234});
  PFM_CHECK_EQ(reader.u32(), std::uint32_t{0xDEADBEEF});
  PFM_CHECK_EQ(reader.u64(), std::uint64_t{0x0123456789ABCDEF});
  PFM_CHECK_EQ(reader.i64(), std::int64_t{-42});
  PFM_CHECK(reader.flag());
  PFM_CHECK(!reader.flag());
  std::string text;
  PFM_REQUIRE(reader.ok());
  PFM_CHECK(reader.text(text, 16));
  PFM_CHECK_EQ(text, std::string{"hello"});
  PFM_CHECK(reader.ok());
  PFM_CHECK(reader.at_end());
  PFM_CHECK_EQ(reader.remaining(), std::size_t{0});
}

PFM_TEST(codec_writer_text_refuses_an_oversized_field) {
  codec::Writer writer;
  PFM_CHECK(writer.text("abc", 3));
  PFM_REQUIRE(writer.ok());
  PFM_CHECK_EQ(writer.bytes().size(), std::size_t{7});
  PFM_CHECK_EQ(writer.bytes()[0], std::uint8_t{3});
  PFM_CHECK_EQ(writer.bytes()[1], std::uint8_t{0});
  PFM_CHECK_EQ(writer.bytes()[2], std::uint8_t{0});
  PFM_CHECK_EQ(writer.bytes()[3], std::uint8_t{0});
  PFM_CHECK_EQ(writer.bytes()[4], std::uint8_t{'a'});

  const std::string too_long(9, 'x');
  PFM_CHECK(!writer.text(too_long, 8));
  PFM_CHECK(!writer.ok());
  PFM_CHECK_EQ(writer.error().code(), StatusCode::FieldTooLong);
  PFM_CHECK_EQ(writer.bytes().size(), std::size_t{7});
  PFM_CHECK_EQ(writer.error().context(), std::string{"codec.text"});

  // The failure is latched: a later oversize write keeps the first reason.
  PFM_CHECK(!writer.text(std::string(50, 'y'), 8));
  PFM_CHECK_EQ(writer.error().code(), StatusCode::FieldTooLong);
}

PFM_TEST(codec_reader_refuses_a_declared_length_above_the_bound_before_reading) {
  const std::array<std::uint8_t, 4> absurd{{0xFF, 0xFF, 0xFF, 0xFF}};
  codec::Reader absurd_reader{absurd};
  std::string out{"unchanged"};
  PFM_CHECK(!absurd_reader.text(out, 8));
  PFM_CHECK_EQ(absurd_reader.error().code(), StatusCode::FieldTooLong);
  PFM_CHECK_EQ(out, std::string{"unchanged"});
  PFM_CHECK_EQ(absurd_reader.position(), std::size_t{4});
  PFM_CHECK(absurd_reader.at_end());

  std::vector<std::uint8_t> over{std::uint8_t{9}, std::uint8_t{0}, std::uint8_t{0},
                                 std::uint8_t{0}};
  over.insert(over.end(), 9, std::uint8_t{'y'});
  codec::Reader over_reader{over};
  out = "unchanged";
  PFM_CHECK(!over_reader.text(out, 8));
  PFM_CHECK_EQ(over_reader.error().code(), StatusCode::FieldTooLong);
  PFM_CHECK_EQ(out, std::string{"unchanged"});
  PFM_CHECK_EQ(over_reader.remaining(), std::size_t{9});

  const std::vector<std::uint8_t> exact{std::uint8_t{4}, std::uint8_t{0}, std::uint8_t{0},
                                        std::uint8_t{0}, std::uint8_t{'p'}, std::uint8_t{'f'},
                                        std::uint8_t{'m'}, std::uint8_t{'!'}};
  codec::Reader exact_reader{exact};
  out.clear();
  PFM_CHECK(exact_reader.text(out, 4));
  PFM_CHECK_EQ(out, std::string{"pfm!"});
  PFM_CHECK(exact_reader.ok());
  PFM_CHECK(exact_reader.at_end());

  const std::vector<std::uint8_t> short_field{std::uint8_t{4}, std::uint8_t{0}, std::uint8_t{0},
                                              std::uint8_t{0}, std::uint8_t{'p'},
                                              std::uint8_t{'f'}};
  codec::Reader short_reader{short_field};
  out = "unchanged";
  PFM_CHECK(!short_reader.text(out, 8));
  PFM_CHECK_EQ(short_reader.error().code(), StatusCode::StoreTruncated);
  PFM_CHECK_EQ(out, std::string{"unchanged"});
}

PFM_TEST(codec_reader_flag_refuses_values_above_one_and_detects_trailing_bytes) {
  const std::array<std::uint8_t, 1> invalid{{2}};
  codec::Reader invalid_reader{invalid};
  PFM_CHECK(!invalid_reader.flag());
  PFM_CHECK_EQ(invalid_reader.error().code(), StatusCode::MalformedEncoding);
  PFM_CHECK(!invalid_reader.ok());
  PFM_CHECK(!invalid_reader.flag());

  const std::array<std::uint8_t, 2> flags{{0, 1}};
  codec::Reader flag_reader{flags};
  PFM_CHECK(!flag_reader.flag());
  PFM_CHECK(flag_reader.flag());
  PFM_CHECK(flag_reader.ok());
  PFM_CHECK(flag_reader.at_end());

  const std::array<std::uint8_t, 3> trailing{{1, 2, 3}};
  codec::Reader trailing_reader{trailing};
  static_cast<void>(trailing_reader.u8());
  PFM_CHECK(!trailing_reader.at_end());
  PFM_CHECK_EQ(trailing_reader.remaining(), std::size_t{2});
  static_cast<void>(trailing_reader.u8());
  PFM_CHECK_EQ(trailing_reader.position(), std::size_t{2});
  PFM_CHECK(!trailing_reader.at_end());
  static_cast<void>(trailing_reader.u8());
  PFM_CHECK(trailing_reader.at_end());
  PFM_CHECK(trailing_reader.ok());

  static_cast<void>(trailing_reader.u8());
  PFM_CHECK(!trailing_reader.ok());
  PFM_CHECK_EQ(trailing_reader.error().code(), StatusCode::StoreTruncated);
  PFM_CHECK(trailing_reader.at_end());
}

PFM_TEST(codec_decode_refuses_unknown_enum_bytes_and_round_trips_references) {
  const std::array<std::uint8_t, 1> unknown{{0xFF}};
  codec::Reader enum_reader{unknown};
  RequestState state = RequestState::Planned;
  codec::decode(enum_reader, state);
  PFM_CHECK(!enum_reader.ok());
  PFM_CHECK_EQ(enum_reader.error().code(), StatusCode::UnknownEnumValue);
  PFM_CHECK_EQ(state, RequestState::Planned);

  const std::array<std::uint8_t, 1> known{
      {static_cast<std::uint8_t>(RequestState::Indeterminate)}};
  codec::Reader known_reader{known};
  codec::decode(known_reader, state);
  PFM_CHECK(known_reader.ok());
  PFM_CHECK_EQ(state, RequestState::Indeterminate);

  // A zero kind byte with no name at all is a truncated encoding.
  const std::array<std::uint8_t, 1> zero_kind{{0}};
  codec::Reader kind_reader{zero_kind};
  RefToken token;
  codec::decode(kind_reader, token);
  PFM_CHECK(!kind_reader.ok());
  PFM_CHECK_EQ(kind_reader.error().code(), StatusCode::StoreTruncated);
  PFM_CHECK(!token.is_set());

  // A zero kind byte carrying an empty name is the canonical absent reference,
  // and it round-trips as one rather than being refused.
  const std::array<std::uint8_t, 5> absent_kind{{0, 0, 0, 0, 0}};
  codec::Reader absent_reader{absent_kind};
  RefToken absent;
  codec::decode(absent_reader, absent);
  PFM_CHECK(absent_reader.ok());
  PFM_CHECK(absent_reader.at_end());
  PFM_CHECK(!absent.is_set());

  // A zero kind byte carrying a name is malformed: absence has one encoding.
  const std::array<std::uint8_t, 6> absent_with_name{{0, 1, 0, 0, 0, 'x'}};
  codec::Reader malformed_reader{absent_with_name};
  RefToken malformed;
  codec::decode(malformed_reader, malformed);
  PFM_CHECK(!malformed_reader.ok());
  PFM_CHECK_EQ(malformed_reader.error().code(), StatusCode::MalformedEncoding);

  // A kind byte that names no kind at all is an unknown enum value.
  const std::array<std::uint8_t, 1> unassigned_kind{{0xC8}};
  codec::Reader unassigned_reader{unassigned_kind};
  RefToken unassigned;
  codec::decode(unassigned_reader, unassigned);
  PFM_CHECK(!unassigned_reader.ok());
  PFM_CHECK_EQ(unassigned_reader.error().code(), StatusCode::UnknownEnumValue);

  const RefToken ups = ref_of(RefKind::UpsUnit, "ups-0-0");
  codec::Writer writer;
  codec::encode(writer, ups);
  PFM_REQUIRE(writer.ok());
  codec::Reader round_reader{writer.bytes()};
  RefToken decoded;
  codec::decode(round_reader, decoded);
  PFM_REQUIRE(round_reader.ok());
  PFM_CHECK(decoded == ups);
  PFM_CHECK_EQ(std::string{decoded.to_string()}, std::string{"ups:ups-0-0"});
  PFM_CHECK(round_reader.at_end());
}

// ===========================================================================
// topology.hpp -- validation, canonicalisation, indexing
// ===========================================================================

PFM_TEST(topology_validate_accepts_a_valid_snapshot) {
  const TopologySnapshot snapshot = small_topology();
  PFM_CHECK_OK(validate(snapshot, Bounds{}));

  auto built = TopologyIndex::build(snapshot, Bounds{});
  PFM_REQUIRE(built.ok());
  const TopologyIndex& index = built.value();
  PFM_CHECK(!index.empty());
  PFM_CHECK_EQ(index.size(), std::size_t{5});
  PFM_CHECK_EQ(index.generation().value(), std::uint64_t{1});
  PFM_CHECK(index.find(ref_of(RefKind::Bus, "bus-a")) != nullptr);
  PFM_CHECK(index.find(ref_of(RefKind::Bus, "bus-zzz")) == nullptr);
  PFM_CHECK(index.domain(ref_of(RefKind::FailureDomain, "fd-a")) != nullptr);
  PFM_CHECK(index.domain(ref_of(RefKind::FailureDomain, "fd-zzz")) == nullptr);
  PFM_CHECK_EQ(index.find(ref_of(RefKind::PduBranch, "branch-a"))->load_group.name(),
               std::string{"load-a"});
}

PFM_TEST(topology_validate_refuses_duplicate_elements_and_domains) {
  TopologySnapshot snapshot = small_topology();
  const TopologyElement duplicate_element = snapshot.elements.front();
  snapshot.elements.push_back(duplicate_element);
  PFM_CHECK_CODE(validate(snapshot, Bounds{}), StatusCode::DuplicateElement);

  snapshot = small_topology();
  const FailureDomain duplicate_domain = snapshot.domains.front();
  snapshot.domains.push_back(duplicate_domain);
  PFM_CHECK_CODE(validate(snapshot, Bounds{}), StatusCode::DuplicateElement);
}

PFM_TEST(topology_validate_refuses_cycles) {
  TopologySnapshot snapshot = small_topology();
  snapshot.elements[2].upstream = snapshot.elements[2].element;
  PFM_CHECK_CODE(validate(snapshot, Bounds{}), StatusCode::TopologyCycle);

  snapshot = small_topology();
  snapshot.elements[0].upstream = snapshot.elements[2].element;
  PFM_CHECK_CODE(validate(snapshot, Bounds{}), StatusCode::TopologyCycle);

  snapshot = small_topology();
  snapshot.elements[0].upstream = snapshot.elements[4].element;
  PFM_CHECK_CODE(validate(snapshot, Bounds{}), StatusCode::TopologyCycle);
}

PFM_TEST(topology_validate_refuses_unknown_references) {
  TopologySnapshot snapshot = small_topology();
  snapshot.elements[2].upstream = ref_of(RefKind::Bus, "bus-zzz");
  PFM_CHECK_CODE(validate(snapshot, Bounds{}), StatusCode::TopologyUnknownElement);

  snapshot = small_topology();
  snapshot.elements[2].isolation_point = ref_of(RefKind::Breaker, "brk-zzz");
  PFM_CHECK_CODE(validate(snapshot, Bounds{}), StatusCode::TopologyUnknownElement);

  snapshot = small_topology();
  snapshot.domains[0].members.push_back(ref_of(RefKind::Rack, "rack-zzz"));
  PFM_CHECK_CODE(validate(snapshot, Bounds{}), StatusCode::TopologyUnknownElement);

  snapshot = small_topology();
  snapshot.elements[2].domain = ref_of(RefKind::FailureDomain, "fd-zzz");
  PFM_CHECK_CODE(validate(snapshot, Bounds{}), StatusCode::FailureDomainUnknown);
}

PFM_TEST(topology_validate_refuses_a_non_switching_isolation_point) {
  TopologySnapshot snapshot = small_topology();
  snapshot.elements[2].isolation_point = ref_of(RefKind::UtilityFeed, "feed-a");
  PFM_CHECK_CODE(validate(snapshot, Bounds{}), StatusCode::InvalidArgument);

  snapshot = small_topology();
  snapshot.elements[2].isolation_point = snapshot.elements[2].element;
  PFM_CHECK_CODE(validate(snapshot, Bounds{}), StatusCode::InvalidArgument);

  // A switching device is accepted as an isolation point.
  snapshot = small_topology();
  snapshot.elements[3].has_isolation_point = true;
  snapshot.elements[3].isolation_point = snapshot.elements[1].element;
  PFM_CHECK_OK(validate(snapshot, Bounds{}));
}

PFM_TEST(topology_validate_refuses_unset_and_mismatched_identities) {
  TopologySnapshot snapshot = small_topology();
  snapshot.generation = TopologyGeneration{};
  PFM_CHECK_CODE(validate(snapshot, Bounds{}), StatusCode::InvalidArgument);

  snapshot = small_topology();
  snapshot.elements[0].element = RefToken{};
  PFM_CHECK_CODE(validate(snapshot, Bounds{}), StatusCode::InvalidArgument);

  snapshot = small_topology();
  snapshot.elements[0].kind = ElementKind::Unknown;
  PFM_CHECK_CODE(validate(snapshot, Bounds{}), StatusCode::InvalidArgument);

  snapshot = small_topology();
  snapshot.elements[0].kind = ElementKind::Bus;
  PFM_CHECK_CODE(validate(snapshot, Bounds{}), StatusCode::InvalidArgument);

  snapshot = small_topology();
  snapshot.elements[0].has_isolation_point = true;
  snapshot.elements[0].isolation_point = RefToken{};
  PFM_CHECK_CODE(validate(snapshot, Bounds{}), StatusCode::InvalidArgument);

  snapshot = small_topology();
  snapshot.elements[2].load_group = ref_of(RefKind::Bus, "bus-a");
  PFM_CHECK_CODE(validate(snapshot, Bounds{}), StatusCode::InvalidArgument);

  snapshot = small_topology();
  snapshot.elements[0].element = ref_of(RefKind::LoadGroup, "load-a");
  snapshot.elements[0].kind = ElementKind::UtilityFeed;
  PFM_CHECK_CODE(validate(snapshot, Bounds{}), StatusCode::InvalidArgument);
}

PFM_TEST(topology_validate_enforces_bounds) {
  Bounds bounds{};
  bounds.max_topology_elements = 4;
  PFM_CHECK_CODE(validate(small_topology(), bounds), StatusCode::TopologyBoundsExceeded);

  bounds = Bounds{};
  bounds.max_topology_elements = 5;
  PFM_CHECK_OK(validate(small_topology(), bounds));

  bounds = Bounds{};
  bounds.max_failure_domains = 0;
  PFM_CHECK_CODE(validate(small_topology(), bounds), StatusCode::TopologyBoundsExceeded);

  bounds = Bounds{};
  bounds.max_label_length = 0;
  TopologySnapshot snapshot = small_topology();
  snapshot.elements[0].label = "x";
  PFM_CHECK_CODE(validate(snapshot, bounds), StatusCode::FieldTooLong);
  PFM_CHECK_OK(validate(small_topology(), bounds));
}

PFM_TEST(topology_canonicalise_sorts_elements_domains_and_members) {
  TopologySnapshot snapshot = small_topology();
  std::reverse(snapshot.elements.begin(), snapshot.elements.end());
  FailureDomain later;
  later.domain = ref_of(RefKind::FailureDomain, "fd-z");
  later.members = {snapshot.elements.front().element, snapshot.elements.front().element};
  snapshot.domains.insert(snapshot.domains.begin(), later);
  snapshot.domains[1].members.push_back(snapshot.domains[1].members.front());
  std::reverse(snapshot.domains[1].members.begin(), snapshot.domains[1].members.end());

  canonicalise(snapshot);

  PFM_CHECK(std::is_sorted(
      snapshot.elements.begin(), snapshot.elements.end(),
      [](const TopologyElement& a, const TopologyElement& b) { return a.element < b.element; }));
  PFM_CHECK_EQ(snapshot.elements[0].kind, ElementKind::UtilityFeed);
  PFM_CHECK_EQ(snapshot.elements[1].kind, ElementKind::Bus);
  PFM_CHECK_EQ(snapshot.elements[2].kind, ElementKind::Breaker);
  PFM_CHECK_EQ(snapshot.elements[3].kind, ElementKind::PduBranch);
  PFM_CHECK_EQ(snapshot.elements[4].kind, ElementKind::LoadGroup);

  PFM_CHECK_EQ(std::string{snapshot.domains[0].domain.name()}, std::string{"fd-a"});
  PFM_CHECK_EQ(std::string{snapshot.domains[1].domain.name()}, std::string{"fd-z"});
  PFM_CHECK_EQ(snapshot.domains[0].members.size(), std::size_t{5});
  PFM_CHECK(is_canonical_order(snapshot.domains[0].members));
  PFM_CHECK(!has_duplicates(snapshot.domains[0].members));
  PFM_CHECK_EQ(snapshot.domains[1].members.size(), std::size_t{1});

  // The canonical snapshot is still valid.
  PFM_CHECK_OK(validate(snapshot, Bounds{}));
}

PFM_TEST(topology_index_ancestors_and_descendants) {
  auto built = TopologyIndex::build(small_topology(), Bounds{});
  PFM_REQUIRE(built.ok());
  const TopologyIndex& index = built.value();

  const RefToken feed = ref_of(RefKind::UtilityFeed, "feed-a");
  const RefToken breaker = ref_of(RefKind::Breaker, "brk-a");
  const RefToken bus = ref_of(RefKind::Bus, "bus-a");
  const RefToken branch = ref_of(RefKind::PduBranch, "branch-a");
  const RefToken load = ref_of(RefKind::LoadGroup, "load-a");

  const std::vector<RefToken> bus_ancestors = index.ancestors(bus);
  PFM_CHECK_EQ(bus_ancestors.size(), std::size_t{3});
  PFM_CHECK_REF(bus_ancestors[0], bus);
  PFM_CHECK_REF(bus_ancestors[1], breaker);
  PFM_CHECK_REF(bus_ancestors[2], feed);

  const std::vector<RefToken> feed_ancestors = index.ancestors(feed);
  PFM_CHECK_EQ(feed_ancestors.size(), std::size_t{1});
  PFM_CHECK_REF(feed_ancestors[0], feed);

  const std::vector<RefToken> load_ancestors = index.ancestors(load);
  PFM_CHECK_EQ(load_ancestors.size(), std::size_t{5});
  PFM_CHECK_REF(load_ancestors[0], load);
  PFM_CHECK_REF(load_ancestors[4], feed);

  const std::vector<RefToken> all = index.descendants(feed);
  PFM_CHECK_EQ(all.size(), std::size_t{5});
  PFM_CHECK(is_canonical_order(all));
  PFM_CHECK_REF(all[0], feed);
  PFM_CHECK_REF(all[1], bus);
  PFM_CHECK_REF(all[2], breaker);
  PFM_CHECK_REF(all[3], branch);
  PFM_CHECK_REF(all[4], load);

  const std::vector<RefToken> under_bus = index.descendants(bus);
  PFM_CHECK_EQ(under_bus.size(), std::size_t{3});
  PFM_CHECK_REF(under_bus[0], bus);
  PFM_CHECK_REF(under_bus[1], branch);
  PFM_CHECK_REF(under_bus[2], load);

  const std::vector<RefToken> under_load = index.descendants(load);
  PFM_CHECK_EQ(under_load.size(), std::size_t{1});
  PFM_CHECK_REF(under_load[0], load);

  PFM_CHECK(index.ancestors(ref_of(RefKind::Bus, "bus-zzz")).empty());
  PFM_CHECK(index.descendants(ref_of(RefKind::Bus, "bus-zzz")).empty());
}

PFM_TEST(topology_index_load_groups_and_domains) {
  auto built = TopologyIndex::build(small_topology(), Bounds{});
  PFM_REQUIRE(built.ok());
  const TopologyIndex& index = built.value();

  const RefToken feed = ref_of(RefKind::UtilityFeed, "feed-a");
  const RefToken bus = ref_of(RefKind::Bus, "bus-a");
  const RefToken branch = ref_of(RefKind::PduBranch, "branch-a");
  const RefToken load = ref_of(RefKind::LoadGroup, "load-a");
  const RefToken power_domain = ref_of(RefKind::PowerDomain, "pd-a");
  const RefToken failure_domain = ref_of(RefKind::FailureDomain, "fd-a");

  const std::vector<RefToken> loads = index.load_groups_under(feed);
  PFM_CHECK_EQ(loads.size(), std::size_t{1});
  PFM_CHECK_REF(loads[0], load);

  const std::vector<RefToken> under_branch = index.load_groups_under(branch);
  PFM_CHECK_EQ(under_branch.size(), std::size_t{1});
  PFM_CHECK_REF(under_branch[0], load);

  const std::vector<RefToken> under_breaker = index.load_groups_under(ref_of(RefKind::Breaker, "brk-a"));
  PFM_CHECK_EQ(under_breaker.size(), std::size_t{1});
  PFM_CHECK_REF(under_breaker[0], load);

  const std::vector<RefToken> bus_domains = index.domains_of(bus);
  PFM_CHECK_EQ(bus_domains.size(), std::size_t{2});
  PFM_CHECK_REF(bus_domains[0], power_domain);
  PFM_CHECK_REF(bus_domains[1], failure_domain);

  const std::vector<RefToken> branch_domains = index.domains_of(branch);
  PFM_CHECK_EQ(branch_domains.size(), std::size_t{1});
  PFM_CHECK_REF(branch_domains[0], failure_domain);

  PFM_CHECK(index.domains_of(RefToken{}).empty());

  const std::vector<RefToken> combined =
      index.domains_of(std::vector<RefToken>{bus, branch, ref_of(RefKind::Bus, "bus-zzz")});
  PFM_CHECK_EQ(combined.size(), std::size_t{2});
  PFM_CHECK_REF(combined[0], power_domain);
  PFM_CHECK_REF(combined[1], failure_domain);
}

PFM_TEST(topology_index_isolation_point_queries) {
  auto built = TopologyIndex::build(small_topology(), Bounds{});
  PFM_REQUIRE(built.ok());
  const TopologyIndex& index = built.value();

  const RefToken breaker = ref_of(RefKind::Breaker, "brk-a");
  const RefToken bus = ref_of(RefKind::Bus, "bus-a");
  const RefToken branch = ref_of(RefKind::PduBranch, "branch-a");
  const RefToken feed = ref_of(RefKind::UtilityFeed, "feed-a");

  RefToken point;
  PFM_CHECK(index.isolation_point_for(bus, point));
  PFM_CHECK_REF(point, breaker);

  point = RefToken{};
  PFM_CHECK(index.isolation_point_for(branch, point));
  PFM_CHECK_REF(point, breaker);

  const RefToken load = ref_of(RefKind::LoadGroup, "load-a");
  point = RefToken{};
  PFM_CHECK(index.isolation_point_for(load, point));
  PFM_CHECK_REF(point, breaker);

  point = RefToken{};
  PFM_CHECK(!index.isolation_point_for(feed, point));
  PFM_CHECK(!point.is_set());

  point = RefToken{};
  PFM_CHECK(!index.isolation_point_for(ref_of(RefKind::Bus, "bus-zzz"), point));

  const std::vector<RefToken> isolated = index.isolated_by(breaker);
  PFM_CHECK_EQ(isolated.size(), std::size_t{1});
  PFM_CHECK_REF(isolated[0], bus);

  PFM_CHECK(index.isolated_by(ref_of(RefKind::Breaker, "brk-zzz")).empty());

  PFM_CHECK(!index.domain_is_shared(ref_of(RefKind::FailureDomain, "fd-zzz")));
}

PFM_TEST(topology_index_domain_is_shared_detects_multiple_downstream_domains) {
  TopologySnapshot snapshot = small_topology();
  auto built = TopologyIndex::build(snapshot, Bounds{});
  PFM_REQUIRE(built.ok());
  PFM_CHECK(!built.value().domain_is_shared(ref_of(RefKind::FailureDomain, "fd-a")));

  TopologyElement second_pdu;
  second_pdu.element = ref_of(RefKind::Pdu, "pdu-b");
  second_pdu.kind = ElementKind::Pdu;
  second_pdu.upstream = ref_of(RefKind::Bus, "bus-a");
  second_pdu.domain = ref_of(RefKind::PowerDomain, "pd-b");
  snapshot.elements.push_back(second_pdu);
  snapshot.domains[0].members.push_back(second_pdu.element);

  auto shared = TopologyIndex::build(snapshot, Bounds{});
  PFM_REQUIRE(shared.ok());
  PFM_CHECK(shared.value().domain_is_shared(ref_of(RefKind::FailureDomain, "fd-a")));

  // An explicitly flagged shared domain is shared whatever its members say.
  TopologySnapshot flagged = small_topology();
  flagged.domains[0].shared = true;
  auto flagged_index = TopologyIndex::build(flagged, Bounds{});
  PFM_REQUIRE(flagged_index.ok());
  PFM_CHECK(flagged_index.value().domain_is_shared(ref_of(RefKind::FailureDomain, "fd-a")));
}

PFM_TEST(topology_index_queries_answer_over_the_synthetic_plant) {
  const SyntheticPlant plant{SyntheticPlantConfig{}};
  auto built = TopologyIndex::build(plant.topology(), Bounds{});
  PFM_REQUIRE(built.ok());
  const TopologyIndex& index = built.value();

  const RefToken feed = ref_of(RefKind::UtilityFeed, "feed-0");
  const RefToken breaker = ref_of(RefKind::Breaker, "brk-feed-0");
  const RefToken switchgear = ref_of(RefKind::Switchgear, "sg-0");
  const RefToken bus = ref_of(RefKind::Bus, "bus-0-0");
  const RefToken bus_breaker = ref_of(RefKind::Breaker, "brk-bus-0-0");
  const RefToken ups = ref_of(RefKind::UpsUnit, "ups-0-0");
  const RefToken pdu = ref_of(RefKind::Pdu, "pdu-0-0");
  const RefToken pdu_breaker = ref_of(RefKind::Breaker, "brk-pdu-0-0");
  const RefToken branch = ref_of(RefKind::PduBranch, "branch-0-0-0");
  const RefToken load = ref_of(RefKind::LoadGroup, "load-0-0-0");
  const RefToken generator = ref_of(RefKind::Generator, "gen-0");
  const RefToken power_domain = ref_of(RefKind::PowerDomain, "pd-0");
  const RefToken failure_domain = ref_of(RefKind::FailureDomain, "fd-0");

  PFM_CHECK(index.find(feed) != nullptr);
  PFM_CHECK(index.find(ups) != nullptr);
  PFM_CHECK(index.find(ref_of(RefKind::UpsUnit, "ups-9-9")) == nullptr);

  const std::vector<RefToken> chain = index.ancestors(ups);
  PFM_CHECK_EQ(chain.size(), std::size_t{6});
  PFM_CHECK_REF(chain[0], ups);
  PFM_CHECK_REF(chain[1], bus_breaker);
  PFM_CHECK_REF(chain[2], bus);
  PFM_CHECK_REF(chain[3], switchgear);
  PFM_CHECK_REF(chain[4], breaker);
  PFM_CHECK_REF(chain[5], feed);

  const std::vector<RefToken> under_switchgear = index.descendants(switchgear);
  PFM_CHECK(is_canonical_order(under_switchgear));
  PFM_CHECK(contains_ref(under_switchgear, bus));
  PFM_CHECK(contains_ref(under_switchgear, ups));
  PFM_CHECK(contains_ref(under_switchgear, pdu));
  PFM_CHECK(contains_ref(under_switchgear, branch));
  PFM_CHECK(contains_ref(under_switchgear, load));
  PFM_CHECK(contains_ref(under_switchgear, generator));
  PFM_CHECK(!contains_ref(under_switchgear, feed));

  const std::vector<RefToken> loads = index.load_groups_under(switchgear);
  PFM_CHECK_EQ(loads.size(), std::size_t{4});
  PFM_CHECK(contains_ref(loads, load));
  PFM_CHECK(contains_ref(loads, ref_of(RefKind::LoadGroup, "load-0-0-1")));
  PFM_CHECK(contains_ref(loads, ref_of(RefKind::LoadGroup, "load-0-1-0")));
  PFM_CHECK(contains_ref(loads, ref_of(RefKind::LoadGroup, "load-0-1-1")));
  PFM_CHECK(is_canonical_order(loads));

  const std::vector<RefToken> bus_loads = index.load_groups_under(bus);
  PFM_CHECK_EQ(bus_loads.size(), std::size_t{2});
  PFM_CHECK(contains_ref(bus_loads, load));
  PFM_CHECK(contains_ref(bus_loads, ref_of(RefKind::LoadGroup, "load-0-0-1")));

  const std::vector<RefToken> ups_domains = index.domains_of(ups);
  PFM_CHECK(contains_ref(ups_domains, power_domain));
  PFM_CHECK(contains_ref(ups_domains, failure_domain));
  PFM_CHECK(is_canonical_order(ups_domains));

  PFM_CHECK(index.domain_is_shared(failure_domain));
  PFM_CHECK(!index.domain_is_shared(ref_of(RefKind::FailureDomain, "fd-999-zzz")));

  RefToken point;
  PFM_CHECK(index.isolation_point_for(branch, point));
  PFM_CHECK_REF(point, pdu_breaker);

  point = RefToken{};
  PFM_CHECK(index.isolation_point_for(generator, point));
  PFM_CHECK_REF(point, breaker);

  const std::vector<RefToken> isolated = index.isolated_by(bus_breaker);
  PFM_CHECK_EQ(isolated.size(), std::size_t{1});
  PFM_CHECK_REF(isolated[0], bus);

  const std::vector<RefToken> isolated_by_pdu = index.isolated_by(pdu_breaker);
  PFM_CHECK_EQ(isolated_by_pdu.size(), std::size_t{1});
  PFM_CHECK_REF(isolated_by_pdu[0], pdu);
}

// ===========================================================================
// request.hpp -- lifecycle, fingerprint, validation
// ===========================================================================

PFM_TEST(request_state_transition_legal_pairs) {
  PFM_CHECK(request_state_transition_legal(RequestState::Planned, RequestState::Issued));
  PFM_CHECK(request_state_transition_legal(RequestState::Planned, RequestState::Refused));
  PFM_CHECK(request_state_transition_legal(RequestState::Planned, RequestState::Abandoned));
  PFM_CHECK(request_state_transition_legal(RequestState::Planned, RequestState::Superseded));
  PFM_CHECK(request_state_transition_legal(RequestState::Planned, RequestState::Failed));

  PFM_CHECK(request_state_transition_legal(RequestState::Issued, RequestState::Acknowledged));
  PFM_CHECK(request_state_transition_legal(RequestState::Issued, RequestState::Observed));
  PFM_CHECK(request_state_transition_legal(RequestState::Issued, RequestState::Verified));
  PFM_CHECK(request_state_transition_legal(RequestState::Issued, RequestState::Failed));
  PFM_CHECK(request_state_transition_legal(RequestState::Issued, RequestState::Expired));
  PFM_CHECK(request_state_transition_legal(RequestState::Issued, RequestState::Indeterminate));
  PFM_CHECK(request_state_transition_legal(RequestState::Issued, RequestState::Superseded));
  PFM_CHECK(request_state_transition_legal(RequestState::Issued, RequestState::Abandoned));
  PFM_CHECK(request_state_transition_legal(RequestState::Issued, RequestState::Refused));

  PFM_CHECK(request_state_transition_legal(RequestState::Acknowledged, RequestState::Observed));
  PFM_CHECK(request_state_transition_legal(RequestState::Acknowledged, RequestState::Verified));
  PFM_CHECK(request_state_transition_legal(RequestState::Acknowledged, RequestState::Failed));
  PFM_CHECK(request_state_transition_legal(RequestState::Acknowledged, RequestState::Expired));
  PFM_CHECK(
      request_state_transition_legal(RequestState::Acknowledged, RequestState::Indeterminate));
  PFM_CHECK(request_state_transition_legal(RequestState::Acknowledged, RequestState::Superseded));
  PFM_CHECK(request_state_transition_legal(RequestState::Acknowledged, RequestState::Abandoned));
  PFM_CHECK(!request_state_transition_legal(RequestState::Acknowledged, RequestState::Refused));
  PFM_CHECK(!request_state_transition_legal(RequestState::Acknowledged, RequestState::Planned));
  PFM_CHECK(!request_state_transition_legal(RequestState::Acknowledged, RequestState::Issued));

  PFM_CHECK(request_state_transition_legal(RequestState::Observed, RequestState::Verified));
  PFM_CHECK(request_state_transition_legal(RequestState::Observed, RequestState::Failed));
  PFM_CHECK(request_state_transition_legal(RequestState::Observed, RequestState::Expired));
  PFM_CHECK(request_state_transition_legal(RequestState::Observed, RequestState::Superseded));
  PFM_CHECK(request_state_transition_legal(RequestState::Observed, RequestState::Abandoned));
  PFM_CHECK(request_state_transition_legal(RequestState::Observed, RequestState::Indeterminate));
  PFM_CHECK(!request_state_transition_legal(RequestState::Observed, RequestState::Acknowledged));

  PFM_CHECK(request_state_transition_legal(RequestState::Indeterminate, RequestState::Verified));
  PFM_CHECK(request_state_transition_legal(RequestState::Indeterminate, RequestState::Failed));
  PFM_CHECK(
      request_state_transition_legal(RequestState::Indeterminate, RequestState::Superseded));
  PFM_CHECK(request_state_transition_legal(RequestState::Indeterminate, RequestState::Abandoned));
  PFM_CHECK(request_state_transition_legal(RequestState::Indeterminate, RequestState::Observed));
  PFM_CHECK(
      !request_state_transition_legal(RequestState::Indeterminate, RequestState::Issued));

  PFM_CHECK(!request_state_transition_legal(RequestState::Planned, RequestState::Verified));
  PFM_CHECK(!request_state_transition_legal(RequestState::Planned, RequestState::Acknowledged));
  PFM_CHECK(!request_state_transition_legal(RequestState::Planned, RequestState::Observed));
  PFM_CHECK(!request_state_transition_legal(RequestState::Planned, RequestState::Indeterminate));
  PFM_CHECK(!request_state_transition_legal(RequestState::Planned, RequestState::Expired));
  PFM_CHECK(!request_state_transition_legal(RequestState::Issued, RequestState::Planned));
  PFM_CHECK(!request_state_transition_legal(RequestState::Verified, RequestState::Failed));
  PFM_CHECK(!request_state_transition_legal(RequestState::Failed, RequestState::Verified));
  PFM_CHECK(!request_state_transition_legal(RequestState::Expired, RequestState::Issued));
  PFM_CHECK(!request_state_transition_legal(RequestState::Refused, RequestState::Issued));

  for (const RequestState state : {RequestState::Planned, RequestState::Issued,
                                  RequestState::Acknowledged, RequestState::Observed,
                                  RequestState::Verified, RequestState::Failed,
                                  RequestState::Superseded, RequestState::Abandoned,
                                  RequestState::Refused, RequestState::Expired,
                                  RequestState::Indeterminate}) {
    PFM_CHECK(!request_state_transition_legal(state, state));
  }
}

PFM_TEST(request_state_terminal_and_open_classification) {
  PFM_CHECK(request_state_is_terminal(RequestState::Verified));
  PFM_CHECK(request_state_is_terminal(RequestState::Failed));
  PFM_CHECK(request_state_is_terminal(RequestState::Superseded));
  PFM_CHECK(request_state_is_terminal(RequestState::Abandoned));
  PFM_CHECK(request_state_is_terminal(RequestState::Refused));
  PFM_CHECK(request_state_is_terminal(RequestState::Expired));
  PFM_CHECK(!request_state_is_terminal(RequestState::Planned));
  PFM_CHECK(!request_state_is_terminal(RequestState::Issued));
  PFM_CHECK(!request_state_is_terminal(RequestState::Acknowledged));
  PFM_CHECK(!request_state_is_terminal(RequestState::Observed));
  PFM_CHECK(!request_state_is_terminal(RequestState::Indeterminate));

  PFM_CHECK(request_state_is_open(RequestState::Planned));
  PFM_CHECK(request_state_is_open(RequestState::Issued));
  PFM_CHECK(request_state_is_open(RequestState::Acknowledged));
  PFM_CHECK(request_state_is_open(RequestState::Observed));
  PFM_CHECK(request_state_is_open(RequestState::Indeterminate));
  PFM_CHECK(!request_state_is_open(RequestState::Verified));
  PFM_CHECK(!request_state_is_open(RequestState::Failed));
  PFM_CHECK(!request_state_is_open(RequestState::Superseded));
  PFM_CHECK(!request_state_is_open(RequestState::Abandoned));
  PFM_CHECK(!request_state_is_open(RequestState::Refused));
  PFM_CHECK(!request_state_is_open(RequestState::Expired));

  std::set<std::string> names;
  for (const RequestState state :
       {RequestState::Planned, RequestState::Issued, RequestState::Acknowledged,
        RequestState::Observed, RequestState::Verified, RequestState::Failed,
        RequestState::Superseded, RequestState::Abandoned, RequestState::Refused,
        RequestState::Expired, RequestState::Indeterminate}) {
    const std::string name{request_state_name(state)};
    PFM_CHECK(name != "unknown");
    PFM_CHECK_EQ(names.count(name), std::size_t{0});
    names.insert(name);
    PFM_CHECK(request_state_is_terminal(state) != request_state_is_open(state));
  }

  ResponseRequest request = make_valid_request();
  request.state = RequestState::Verified;
  request.required_proof = ProofKind::Isolation;
  PFM_CHECK(request.satisfies(ProofKind::Isolation));
  PFM_CHECK(request.satisfies(ProofKind::None));
  PFM_CHECK(!request.satisfies(ProofKind::DeEnergization));
  request.state = RequestState::Observed;
  PFM_CHECK(!request.satisfies(ProofKind::Isolation));
  PFM_CHECK(request.is_open());
  request.state = RequestState::Failed;
  PFM_CHECK(!request.is_open());
  PFM_CHECK(!request.satisfies(ProofKind::None));
}

PFM_TEST(request_fingerprint_is_stable_and_covers_the_consequential_fields) {
  const ResponseRequest request = make_valid_request();
  PFM_CHECK(request.fingerprint.is_set());
  PFM_CHECK(response_request_fingerprint(request) == request.fingerprint);
  PFM_CHECK(response_request_fingerprint(request) == response_request_fingerprint(request));

  ResponseRequest changed = request;
  changed.kind = RequestKind::VerifyDeEnergization;
  PFM_CHECK(!(response_request_fingerprint(changed) == request.fingerprint));

  changed = request;
  changed.target.element = ref_of(RefKind::Breaker, "brk-bus-0-1");
  PFM_CHECK(!(response_request_fingerprint(changed) == request.fingerprint));

  changed = request;
  changed.target.kind = ElementKind::Bus;
  PFM_CHECK(!(response_request_fingerprint(changed) == request.fingerprint));

  changed = request;
  changed.target.owner = RequestOwner::UpsControl;
  PFM_CHECK(!(response_request_fingerprint(changed) == request.fingerprint));

  changed = request;
  changed.target.controller = ref_of(RefKind::Controller, "another-controller");
  PFM_CHECK(!(response_request_fingerprint(changed) == request.fingerprint));

  changed = request;
  changed.has_magnitude = true;
  changed.magnitude = basis_points_from_value(5000);
  PFM_CHECK(!(response_request_fingerprint(changed) == request.fingerprint));

  ResponseRequest other = changed;
  other.magnitude = basis_points_from_value(5001);
  PFM_CHECK(!(response_request_fingerprint(other) == response_request_fingerprint(changed)));

  changed = request;
  changed.incident_generation = IncidentGeneration::from_value(2);
  PFM_CHECK(!(response_request_fingerprint(changed) == request.fingerprint));

  // Attempt identity, plan generation, lifecycle state, and instants are
  // deliberately outside the fingerprint: a retry reuses the same operation.
  changed = request;
  changed.attempt = AttemptId::from_value(99);
  changed.attempt_ordinal = 4;
  changed.plan_generation = PlanGeneration::from_value(7);
  changed.state = RequestState::Indeterminate;
  changed.created_at = Timestamp::from_value(1'700'000'500'000LL);
  changed.deadline = Timestamp::from_value(1'700'000'900'000LL);
  changed.has_issued_at = true;
  changed.issued_at = Timestamp::from_value(1'700'000'500'001LL);
  changed.owner_note = "a different note";
  PFM_CHECK(response_request_fingerprint(changed) == request.fingerprint);
}

PFM_TEST(request_validate_accepts_a_fingerprinted_request) {
  const ResponseRequest request = make_valid_request();
  PFM_CHECK_OK(validate(request, Bounds{}));
  PFM_CHECK(request.id.is_set());
  PFM_CHECK(request.attempt.is_set());
  PFM_CHECK(request.is_open());
  PFM_CHECK_EQ(request.idempotency.high(), request.fingerprint.high());
  PFM_CHECK_EQ(request.idempotency.low(), request.fingerprint.low());
}

PFM_TEST(request_validate_refuses_incomplete_and_mismatched_requests) {
  const Bounds bounds{};

  ResponseRequest request = make_valid_request();
  request.id = ResponseRequestId{};
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::InvalidArgument);

  request = make_valid_request();
  request.kind = RequestKind::Unknown;
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::InvalidArgument);

  request = make_valid_request();
  request.target.element = RefToken{};
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::RequestTargetMismatch);

  request = make_valid_request();
  request.target.kind = ElementKind::Unknown;
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::RequestTargetMismatch);

  request = make_valid_request();
  request.target.kind = ElementKind::Bus;
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::RequestTargetMismatch);

  request = make_valid_request();
  request.target.owner = RequestOwner::Unknown;
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::RequestTargetMismatch);

  request = make_valid_request();
  request.idempotency = IdempotencyKey{};
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::InvalidArgument);

  request = make_valid_request();
  request.fingerprint = Fingerprint{};
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::InvalidArgument);
}

PFM_TEST(request_validate_refuses_a_tampered_fingerprint_or_key) {
  const Bounds bounds{};

  ResponseRequest request = make_valid_request();
  request.fingerprint = Fingerprint{request.fingerprint.high(), request.fingerprint.low() ^ 1ULL};
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::IdempotencyConflict);

  request = make_valid_request();
  request.fingerprint = Fingerprint{0xDEADBEEFULL, 0xFEEDFACEULL};
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::IdempotencyConflict);

  request = make_valid_request();
  request.idempotency = IdempotencyKey{1, 2};
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::IdempotencyConflict);

  request = make_valid_request();
  request.kind = RequestKind::Reclose;
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::IdempotencyConflict);
}

PFM_TEST(request_validate_refuses_bad_identity_instant_and_bounds) {
  const Bounds bounds{};

  ResponseRequest request = make_valid_request();
  request.attempt = AttemptId{};
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::InvalidArgument);

  request = make_valid_request();
  request.attempt_ordinal = 0;
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::InvalidArgument);

  request = make_valid_request();
  request.created_at = Timestamp{};
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::InvalidArgument);

  request = make_valid_request();
  request.has_magnitude = true;
  request.magnitude = basis_points_from_value(10001);
  request.fingerprint = response_request_fingerprint(request);
  request.idempotency = IdempotencyKey::from_fingerprint(request.fingerprint);
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::ValueOutOfRange);

  request = make_valid_request();
  request.owner_note.assign(bounds.max_note_length + 1, 'n');
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::FieldTooLong);

  request = make_valid_request();
  request.evidence.assign(bounds.max_reason_evidence_refs + 1, Fingerprint{1, 1});
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::BoundsExceeded);

  request = make_valid_request();
  request.state = RequestState::Verified;
  request.required_proof = ProofKind::Isolation;
  PFM_CHECK_CODE(validate(request, bounds), StatusCode::EffectUnverified);

  request.verification_source = ref_of(RefKind::Controller, "power-control-plane");
  PFM_CHECK_OK(validate(request, bounds));

  request = make_valid_request();
  request.state = RequestState::Verified;
  request.required_proof = ProofKind::None;
  PFM_CHECK_OK(validate(request, bounds));
}

// ===========================================================================
// journal.hpp -- fingerprints and validation
// ===========================================================================

PFM_TEST(journal_fingerprint_is_stable_and_covers_entry_identity) {
  const JournalEntry entry = make_valid_entry();
  PFM_CHECK(entry.fingerprint.is_set());
  PFM_CHECK(journal_entry_fingerprint(entry) == entry.fingerprint);
  PFM_CHECK(journal_entry_fingerprint(entry) == journal_entry_fingerprint(entry));
  PFM_CHECK_OK(validate(entry, Bounds{}));

  JournalEntry changed = entry;
  changed.kind = JournalKind::IncidentClosed;
  PFM_CHECK(!(journal_entry_fingerprint(changed) == entry.fingerprint));

  changed = entry;
  changed.sequence = JournalSequence::from_value(8);
  PFM_CHECK(!(journal_entry_fingerprint(changed) == entry.fingerprint));

  changed = entry;
  changed.revision = StateRevision::from_value(4);
  PFM_CHECK(!(journal_entry_fingerprint(changed) == entry.fingerprint));

  changed = entry;
  changed.recorded_at = Timestamp::from_value(1'700'000'000'001LL);
  PFM_CHECK(!(journal_entry_fingerprint(changed) == entry.fingerprint));

  changed = entry;
  changed.payload.note = "incident closed";
  PFM_CHECK(!(journal_entry_fingerprint(changed) == entry.fingerprint));

  changed = entry;
  changed.payload.has_incident = false;
  PFM_CHECK(!(journal_entry_fingerprint(changed) == entry.fingerprint));
}

PFM_TEST(journal_validate_refuses_a_tampered_fingerprint) {
  const Bounds bounds{};

  JournalEntry entry = make_valid_entry();
  entry.fingerprint = Fingerprint{0xDEADBEEFULL, 0xFEEDFACEULL};
  PFM_CHECK_CODE(validate(entry, bounds), StatusCode::StoreIntegrityFailure);

  entry = make_valid_entry();
  entry.sequence = JournalSequence::from_value(8);
  PFM_CHECK_CODE(validate(entry, bounds), StatusCode::StoreIntegrityFailure);

  entry = make_valid_entry();
  entry.payload.note = "tampered";
  PFM_CHECK_CODE(validate(entry, bounds), StatusCode::StoreIntegrityFailure);
}

PFM_TEST(journal_validate_refuses_incomplete_entries) {
  const Bounds bounds{};

  JournalEntry entry = make_valid_entry();
  entry.kind = JournalKind::None;
  PFM_CHECK_CODE(validate(entry, bounds), StatusCode::InvalidArgument);

  entry = make_valid_entry();
  entry.sequence = JournalSequence{};
  PFM_CHECK_CODE(validate(entry, bounds), StatusCode::InvalidArgument);

  entry = make_valid_entry();
  entry.revision = StateRevision{};
  PFM_CHECK_CODE(validate(entry, bounds), StatusCode::InvalidArgument);

  entry = make_valid_entry();
  entry.recorded_at = Timestamp{};
  PFM_CHECK_CODE(validate(entry, bounds), StatusCode::InvalidArgument);

  entry = make_valid_entry();
  entry.fingerprint = Fingerprint{};
  PFM_CHECK_CODE(validate(entry, bounds), StatusCode::InvalidArgument);

  PFM_CHECK_EQ(std::string{journal_kind_name(JournalKind::RecoveryCompleted)},
               std::string{"recovery-completed"});
  PFM_CHECK_EQ(std::string{journal_kind_name(static_cast<JournalKind>(200))},
               std::string{"unknown"});
}

PFM_TEST(journal_validate_refuses_an_oversized_note) {
  const Bounds bounds{};
  JournalEntry entry = make_valid_entry();
  entry.payload.note.assign(bounds.max_note_length + 44, 'n');
  entry.fingerprint = journal_entry_fingerprint(entry);
  PFM_REQUIRE(entry.fingerprint.is_set());
  PFM_CHECK_CODE(validate(entry, bounds), StatusCode::FieldTooLong);

  entry.payload.note.assign(bounds.max_note_length, 'n');
  entry.fingerprint = journal_entry_fingerprint(entry);
  PFM_CHECK_OK(validate(entry, bounds));
}

// ===========================================================================
// version.hpp -- identity
// ===========================================================================

PFM_TEST(version_identity_is_stable) {
  PFM_CHECK_EQ(version_string(), std::string_view{"1.0.0"});
  PFM_CHECK_EQ(kStoreFormatVersion, 1u);
  PFM_CHECK_EQ(kVersionMajor, 1u);
  PFM_CHECK_EQ(kVersionMinor, 0u);
  PFM_CHECK_EQ(kVersionPatch, 0u);
  PFM_CHECK_EQ(kDccpBoundary, 53u);
  PFM_CHECK(!component_name().empty());
  PFM_CHECK_EQ(component_name(), std::string_view{"Power Failure Manager"});
}

PFM_TEST_MAIN()
