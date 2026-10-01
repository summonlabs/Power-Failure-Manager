# Contributing to Power Failure Manager

Copyright 2026 Summon Software Labs. Licensed under the Apache License 2.0
(see `LICENSE`).

## Contribution terms

By submitting a contribution to this repository you agree that your
contribution is licensed to Summon Software Labs and to recipients of the
software distributed by Summon Software Labs under the Apache License,
Version 2.0, without additional terms or conditions, as described in
Section 5 of that license. There is no Contributor License Agreement (CLA)
to sign, and no copyright assignment is required.

Do not add `Co-authored-by` trailers or other attribution trailers to
commits. Commit authorship is recorded by Git.

## Code quality expectations

* C++20, portable where practical, Windows/MSVC first.
* All first-party code compiles with zero warnings. MSVC builds use
  `/W4 /WX /permissive-`. Do not silence a warning to make a build
  green; fix the cause or narrow the suppression with a comment explaining
  why it is correct.
* No new third-party runtime dependency may be introduced without an
  explicit decision recorded in the change description. The runtime is
  deliberately dependency-light.
* Physical quantities use exact integer units with explicit unit types.
  Floating point must not be used as an authority or accounting boundary.
* Missing, stale, unavailable, denied, indeterminate, and zero are distinct
  states. Do not encode any of them as another.
* Every authority-bearing mutation must carry the identity it was planned
  against (incident, generation, control epoch, incarnation, state
  revision) and must be refused deterministically when that identity is
  stale, future, or superseded.
* A conclusion about the electrical plant requires current evidence. An
  acknowledgement is not an observation, an observation is not a
  verification, and a locally healthy component does not imply recovery.

## Building and testing

The canonical build, test, install, and package-consumption commands are
documented in `README.md`. In short:

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Tests are proof obligations rather than decoration. A change that alters
electrical-failure semantics should come with a test that would fail if the
semantics regressed, including stale-evidence, contradictory-evidence, and
illegal-transition cases where they apply. Tests must not depend on
timeouts or wall-clock sleeps.

## Review checklist

* Does the change preserve the owned boundary described in `README.md`?
  Actuating electrical equipment, discovering topology, deriving capacity,
  executing load shedding, and owning generic incident state are explicitly
  out of scope.
* Are all new inputs bounded, and are declared sizes checked before
  allocation?
* Is every new persisted field integrity-checked and strictly decoded,
  including reserved fields and trailing bytes?
* Is anything persisted that is derived from the current instant rather
  than decided? Currency belongs to the moment it is asked about.
* Are new public results expressed as status codes rather than exception
  text?
* Is anything under a lock that can call back into user or adapter code?
