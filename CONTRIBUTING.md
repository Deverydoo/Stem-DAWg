# Contributing to Stem DAWg

Thanks for helping. Bug reports, test audio *descriptions* (please don't attach copyrighted music), ideas and pull requests are all welcome.

## Before you start

- For anything bigger than a small fix, open an issue first so we can agree on the approach.
- Read the [roadmap](docs/ROADMAP.md) and the [Milestone 1 specification](docs/milestone-1/M1-Technical-Specification.md). The specification explains the threading and real-time rules, which are strict.

## Build and test

See the [README](README.md#build). Every change must keep:

- **0 compiler warnings** at `/W4` (CI builds with the same flags),
- **all tests passing** in Debug and Release (`ctest --preset local-debug`, `ctest --preset local-release`),
- the **module boundaries**: no Qt outside `apps/`, no JUCE in public headers, no JUCE/Qt in `core`, `project` or `ai`.

## Code rules

- C++20, formatted with the repository's `.clang-format`.
- **The audio callback (`AudioEngine::process`) must never allocate, lock, block, log or throw.** `RealtimeSafetyTests` enforces this with a global allocation guard; add to it when you touch the engine.
- New behaviour comes with tests. Audio tests generate their signals in code (`tests/support/TestSignals.h`); no binary fixtures.
- Every source file starts with the SPDX header:
  ```cpp
  // SPDX-License-Identifier: AGPL-3.0-or-later
  // Copyright (C) 2026 Chaos Rift Entertainment, LLC and the Stem DAWg contributors
  ```
- User-facing controls get a tooltip (`helpTip(title, explanation)`) in plain language.

## Licence of contributions

Stem DAWg is licensed under the GNU Affero General Public License v3.0 or later. By submitting a contribution you agree that it is licensed under the same terms (inbound = outbound). New third-party dependencies must follow the rules in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
