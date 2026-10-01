// Power Failure Manager -- minimal test framework.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0 (see LICENSE).
//
// Tests are plain functions registered at start-up and run in a fixed order.
// There is no timeout mechanism of any kind: a test that hangs is a defect to
// diagnose, not a test to abort.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "pfm/status.hpp"

namespace pfmtest {

class Registry {
 public:
  using TestFn = void (*)();

  static Registry& instance() {
    static Registry registry;
    return registry;
  }

  void add(std::string name, TestFn fn) {
    cases_.push_back(Case{std::move(name), fn});
  }

  void fail(const char* file, int line, const std::string& message) {
    failures_ += 1;
    current_failed_ = true;
    std::printf("  FAIL %s:%d in %s: %s\n", file, line, current_.c_str(), message.c_str());
    std::fflush(stdout);
  }

  [[nodiscard]] bool current_failed() const { return current_failed_; }
  [[nodiscard]] std::size_t failures() const { return failures_; }

  int run(int argc, char** argv) {
    std::string filter;
    if (argc > 1) {
      filter = argv[1];
    }
    std::size_t run_count = 0;
    for (const auto& item : cases_) {
      if (!filter.empty() && item.name.find(filter) == std::string::npos) {
        continue;
      }
      current_ = item.name;
      current_failed_ = false;
      std::printf("[ RUN  ] %s\n", item.name.c_str());
      std::fflush(stdout);
      item.fn();
      run_count += 1;
      std::printf("[ %s ] %s\n", current_failed_ ? "FAIL" : " OK ", item.name.c_str());
      std::fflush(stdout);
    }
    std::printf("%zu case(s) run, %zu assertion failure(s)\n", run_count, failures_);
    std::fflush(stdout);
    return failures_ == 0 ? 0 : 1;
  }

 private:
  struct Case {
    std::string name;
    TestFn fn;
  };

  std::vector<Case> cases_{};
  std::size_t failures_{0};
  bool current_failed_{false};
  std::string current_{};
};

inline void report_failure(const char* file, int line, const std::string& message) {
  Registry::instance().fail(file, line, message);
}

}  // namespace pfmtest

#define PFM_TEST(name)                                                              \
  static void name();                                                               \
  namespace {                                                                       \
  const bool name##_pfm_registered = []() {                                         \
    ::pfmtest::Registry::instance().add(#name, &name);                              \
    return true;                                                                    \
  }();                                                                              \
  }                                                                                 \
  static void name()

#define PFM_CHECK(condition)                                                        \
  do {                                                                              \
    if (!(condition)) {                                                             \
      ::pfmtest::report_failure(__FILE__, __LINE__, "expected: " #condition);       \
    }                                                                               \
  } while (false)

#define PFM_CHECK_MSG(condition, message)                                           \
  do {                                                                              \
    if (!(condition)) {                                                             \
      ::pfmtest::report_failure(__FILE__, __LINE__,                                 \
                                std::string{"expected: " #condition " -- "} +        \
                                    std::string{message});                          \
    }                                                                               \
  } while (false)

#define PFM_REQUIRE(condition)                                                      \
  do {                                                                              \
    if (!(condition)) {                                                             \
      ::pfmtest::report_failure(__FILE__, __LINE__, "required: " #condition);       \
      return;                                                                       \
    }                                                                               \
  } while (false)

#define PFM_CHECK_EQ(actual, expected)                                              \
  do {                                                                              \
    const auto& pfm_actual = (actual);                                              \
    const auto& pfm_expected = (expected);                                          \
    if (!(pfm_actual == pfm_expected)) {                                            \
      ::pfmtest::report_failure(__FILE__, __LINE__,                                 \
                                std::string{#actual " != " #expected});             \
    }                                                                               \
  } while (false)

// Refuses a call that was expected to succeed and reports the machine code.
#define PFM_CHECK_OK(expression)                                                    \
  do {                                                                              \
    const auto pfm_result = (expression);                                           \
    if (!pfm_result.ok()) {                                                         \
      ::pfmtest::report_failure(                                                    \
          __FILE__, __LINE__,                                                       \
          std::string{#expression " failed: "} +                                    \
              std::string{pfm_result.status().to_string()});                        \
    }                                                                               \
  } while (false)

// Requires an exact machine-readable status code, so a test never passes
// because a different failure happened to be reported.
#define PFM_CHECK_CODE(expression, expected_code)                                   \
  do {                                                                              \
    const auto pfm_result = (expression);                                           \
    if (pfm_result.ok()) {                                                          \
      ::pfmtest::report_failure(__FILE__, __LINE__,                                 \
                                #expression " unexpectedly succeeded");             \
    } else if (pfm_result.status().code() != (expected_code)) {                     \
      ::pfmtest::report_failure(                                                    \
          __FILE__, __LINE__,                                                       \
          std::string{#expression " produced "} +                                   \
              std::string{pfm_result.status().to_string()} + " instead of " +       \
              std::string{::summon::pfm::status_code_name(expected_code)});         \
    }                                                                               \
  } while (false)

#define PFM_TEST_MAIN()                                                             \
  int main(int argc, char** argv) {                                                 \
    return ::pfmtest::Registry::instance().run(argc, argv);                         \
  }
