// Power Failure Manager -- public status/error contract.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace summon::pfm {

// Machine-readable outcome codes. The numeric value is part of the public
// machine contract and never changes between compatible releases. Validation
// precedence is documented per operation and is stable, so the same invalid
// request always produces the same primary code.
enum class StatusCode : std::uint16_t {
  Ok = 0,

  // --- generic input validation (1xx) -------------------------------------
  InvalidArgument = 100,
  EmptyField = 101,
  FieldTooLong = 102,
  InvalidCharacter = 103,
  ValueOutOfRange = 104,
  DuplicateElement = 105,
  TooManyElements = 106,
  MalformedEncoding = 107,
  ReservedFieldNotZero = 108,
  UnknownEnumValue = 109,

  // --- authority, generations, fencing (2xx) ------------------------------
  MissingAuthority = 200,
  StaleIncarnation = 201,
  StaleEpoch = 202,
  FutureEpoch = 203,
  StaleRevision = 204,
  FutureRevision = 205,
  StaleGeneration = 206,
  FutureGeneration = 207,
  CrossIncidentAuthority = 208,
  AuthorityFenced = 209,
  AuthorityRequired = 210,

  // --- lifecycle and decision state machine (3xx) -------------------------
  PreconditionFailed = 300,
  IllegalTransition = 301,
  NoActiveIncident = 302,
  IncidentNotFound = 303,
  IncidentClosed = 304,
  RecoveryNotEligible = 305,
  GateNotSatisfied = 306,
  EvidenceNotCurrent = 307,
  IncidentAlreadyOpen = 308,
  PlanNotCurrent = 309,

  // --- electrical evidence intake (4xx) -----------------------------------
  EvidenceRejected = 400,
  EvidenceStale = 401,
  EvidenceFuture = 402,
  EvidenceOutOfOrder = 403,
  EvidenceContradictory = 404,
  EvidenceUnavailable = 405,
  EvidenceUnknownTarget = 406,
  EvidenceDuplicate = 407,
  EvidenceOriginUntrusted = 408,

  // --- electrical topology and scope (5xx) --------------------------------
  TopologyUnknownElement = 500,
  TopologyCycle = 501,
  TopologyAmbiguousUpstream = 502,
  TopologyBoundsExceeded = 503,
  ScopeUnresolved = 504,
  FailureDomainUnknown = 505,
  SharedDomainUnresolved = 506,

  // --- protected obligations (6xx) ----------------------------------------
  ProtectedObligationViolation = 600,
  ObligationNotRelaxable = 601,
  ObligationUnknown = 602,
  ObligationViolated = 603,
  ObligationAlreadyRelaxed = 604,
  ObligationNotRelaxed = 605,

  // --- bounded response requests (7xx) ------------------------------------
  RequestNotFound = 700,
  RequestStateConflict = 701,
  RequestExpired = 702,
  IdempotencyConflict = 703,
  EffectUnverified = 704,
  RequestTargetMismatch = 705,
  RequestFailed = 706,
  TransportFailure = 707,
  DispatchIndeterminate = 708,
  AttemptExhausted = 709,

  // --- resource bounds (8xx) ----------------------------------------------
  ResourceExhausted = 800,
  BoundsExceeded = 801,
  CheckpointUnavailable = 802,
  ArithmeticOverflow = 803,

  // --- persistence (9xx) ---------------------------------------------------
  StoreNotFound = 900,
  StoreCorrupt = 901,
  StoreVersionUnsupported = 902,
  StoreTruncated = 903,
  StoreTrailingBytes = 904,
  StoreIntegrityFailure = 905,
  StoreLocked = 906,
  StorePathInvalid = 907,
  StoreIoError = 908,
  StoreReadbackMismatch = 909,
  NoAuthoritativeGeneration = 910,
  ReplayDivergence = 911,
  StoreAlreadyExists = 912,

  // --- runtime lifecycle and concurrency (10xx) ---------------------------
  RuntimeClosed = 1000,
  ShutdownInProgress = 1001,
  ReentrancyRefused = 1002,
  NotSupported = 1003,
  Internal = 1004,
  ConcurrencyConflict = 1005,
};

// Stable machine-readable identifier, e.g. "pfm.stale_epoch".
[[nodiscard]] std::string_view status_code_name(StatusCode code) noexcept;

[[nodiscard]] constexpr bool status_ok(StatusCode code) noexcept { return code == StatusCode::Ok; }

// Decodes a wire value; returns nullopt for values that are not assigned codes.
[[nodiscard]] std::optional<StatusCode> status_code_from_value(std::uint16_t value) noexcept;

// A machine-readable code plus a human-readable explanation. Explanations are
// diagnostics only: callers branch on the code, never on the text.
class Status {
 public:
  Status() noexcept = default;
  Status(StatusCode code, std::string message) : code_(code), message_(std::move(message)) {}

  [[nodiscard]] static Status success() noexcept { return Status{}; }
  [[nodiscard]] static Status error(StatusCode code, std::string message) {
    return Status{code, std::move(message)};
  }

  [[nodiscard]] StatusCode code() const noexcept { return code_; }
  [[nodiscard]] bool ok() const noexcept { return code_ == StatusCode::Ok; }
  [[nodiscard]] std::string_view message() const noexcept { return message_; }
  [[nodiscard]] std::string_view code_name() const noexcept { return status_code_name(code_); }

  // Bounded structured context: the reference, id, or field the code applies
  // to, used by diagnostics and audit output.
  [[nodiscard]] const std::string& context() const noexcept { return context_; }
  Status& with_context(std::string context) {
    context_ = std::move(context);
    return *this;
  }

  [[nodiscard]] std::string to_string() const;

  [[nodiscard]] friend bool operator==(const Status& a, const Status& b) noexcept {
    return a.code_ == b.code_ && a.message_ == b.message_;
  }

 private:
  StatusCode code_{StatusCode::Ok};
  std::string message_{};
  std::string context_{};
};

// Programmer-error trap for impossible states. Aborts rather than throwing so
// that no exception ever crosses the public API boundary.
[[noreturn]] void trap(const char* expression, const char* file, int line);

#define PFM_TRAP(expr) ::summon::pfm::trap((expr), __FILE__, __LINE__)

// Result<T>: either a value or a Status.
template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(Status error) : value_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<T>(value_); }
  [[nodiscard]] StatusCode code() const noexcept {
    return ok() ? StatusCode::Ok : std::get<Status>(value_).code();
  }
  // Precondition: !ok(). Violating it is a programmer error and traps.
  [[nodiscard]] const Status& status() const noexcept {
    if (ok()) {
      PFM_TRAP("Result::status() called on a successful result");
    }
    return std::get<Status>(value_);
  }
  [[nodiscard]] Status status_or_ok() const {
    return ok() ? Status::success() : std::get<Status>(value_);
  }
  // Precondition: ok(). Violating it is a programmer error and traps.
  [[nodiscard]] T& value() & noexcept {
    if (!ok()) {
      PFM_TRAP("Result::value() called on an error result");
    }
    return std::get<T>(value_);
  }
  [[nodiscard]] const T& value() const& noexcept {
    if (!ok()) {
      PFM_TRAP("Result::value() called on an error result");
    }
    return std::get<T>(value_);
  }
  [[nodiscard]] T&& value() && noexcept {
    if (!ok()) {
      PFM_TRAP("Result::value() called on an error result");
    }
    return std::move(std::get<T>(value_));
  }
  [[nodiscard]] T value_or(T fallback) const {
    return ok() ? std::get<T>(value_) : std::move(fallback);
  }

 private:
  std::variant<T, Status> value_;
};

template <>
class Result<void> {
 public:
  Result() = default;
  Result(Status error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] bool ok() const noexcept { return error_.ok(); }
  [[nodiscard]] StatusCode code() const noexcept { return error_.code(); }
  [[nodiscard]] const Status& status() const noexcept { return error_; }
  [[nodiscard]] Status status_or_ok() const { return error_; }

 private:
  Status error_{};
};

using VoidResult = Result<void>;

}  // namespace summon::pfm
