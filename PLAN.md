# bb-auth Plan

Last updated: 2026-02-18
Owner branch: `main`

## Vision

Build the best Linux authentication UX for polkit + keyring with:

- a small, deterministic core daemon
- one excellent built-in Qt fallback provider
- runtime drop-in provider extensibility

## Scope Strategy

Core package responsibilities:

- daemon lifecycle, session routing, policy boundaries
- provider discovery and launch from manifests
- well-tested Qt fallback provider

Out-of-core responsibilities:

- desktop-specific or experimental providers (GTK, shell-native, etc.)
- provider-specific dependency trees
- fast iteration without touching daemon internals

## Product Principles

- Minimize hard dependencies in the core package.
- Keep distro builds deterministic by default.
- Prefer runtime composition over compile-time feature branching.
- Treat provider protocol compatibility as a product contract.
- Ship fewer features, but with high reliability and UX quality.

## Target Architecture

1. `bb-auth` core
   - daemon, session model, provider registry/discovery/launcher
   - provider IPC contract + manifest validation
2. Built-in provider
   - Qt6 fallback provider (first-class quality target)
3. External providers
   - separate binaries and packages
   - registered through `providers.d/*.json`
4. Packaging
   - core package does not pull optional provider stacks
   - optional providers install independently

## Phase Roadmap

## Phase 1: Modular Foundation (completed)

Goal: make "minimal core + drop-in providers" the enforced default.

Tasks:

- [x] Remove in-tree GTK provider build/install from `CMakeLists.txt`.
- [x] Keep provider manifest/launcher runtime behavior unchanged.
- [x] Update `README.md` to document external-provider model first.
- [x] Keep `PKGBUILD` deterministic-minimal by default.
- [x] Ensure CI passes with no GTK provider in core build.

Exit criteria:

- Core builds and tests pass without GTK provider sources.
- No runtime regressions in daemon + Qt fallback flow.

## Phase 2: Provider Platform Hardening

Goal: make third-party provider integration safe and easy.

Tasks:

- [x] Version and freeze provider protocol contract (`docs/PROVIDER_CONTRACT.md`) (locked at IPC v2.0 with explicit framing/error conformance checks).
- [x] Add provider conformance test harness (initial coverage for discovery, registration, priority, and authorization boundaries).
- [x] Add a minimal external-provider template (single binary + manifest).
- [x] Add provider packaging guide for Arch/Nix/manual installs.
- [x] Validate precedence and conflict behavior across provider dirs.

Exit criteria:

- A non-C++ provider can pass conformance tests and run in production flow.
- Provider API changes require explicit versioning and migration notes.

## Phase 3: Best-in-Class Qt UX

Goal: polished, trustable prompts with clear context and low friction.

Tasks:

- [x] Standardize visual language and copy across polkit/keyring/pinentry.
- [x] Improve requestor identity and action clarity.
- [x] Tighten error, cancel, retry, and timeout UX states.
- [x] Add accessibility checks (keyboard-only flow, scaling, contrast).
- [ ] Add UX snapshot tests and flow regression tests.

Exit criteria:

- Prompt behavior is consistent across all auth sources.
- No ambiguous prompts or dead-end states in tested scenarios.

## Phase 4: Reliability and Security

Goal: production-grade behavior under failure and hostile inputs.

Tasks:

- [ ] Add integration tests for provider crash/restart/failover.
- [ ] Add stress tests for session queueing and rapid prompt churn.
- [ ] Fuzz manifest parsing and malformed IPC frames.
- [ ] Audit authorization boundaries for inactive providers.
- [ ] Add startup/shutdown race and conflict-agent test coverage.

Exit criteria:

- No known crashers in daemon startup/failure paths.
- Clear, enforced auth boundaries with test evidence.

## Phase 5: Release and Operations Discipline

Goal: repeatable releases with low regression risk.

Tasks:

- [x] Define release gates (`build`, `ctest`, packaging smoke, service smoke).
- [ ] Add migration notes and upgrade checks for each release.
- [ ] Publish compatibility matrix (core version vs provider protocol).
- [ ] Automate package validation in CI for core-only and optional-provider scenarios.

Exit criteria:

- Every release is reproducible and has a documented verification trail.

## Quality Bar

Functional:

- No blocking regressions in polkit, keyring, or pinentry paths.
- Provider failover returns to usable UI without manual restart.

UX:

- Clear source/action context before secret entry.
- Fast prompt appearance and predictable focus behavior.
- Consistent keyboard actions and cancellation behavior.

Security:

- Only active provider can submit interactive decisions.
- Manifest and IPC validation fails closed with explicit logs.

Packaging:

- Core package remains minimal and deterministic.
- Optional providers add functionality without changing core behavior.

## Near-Term Backlog (next 2-3 sessions)

- [x] Refactor out in-tree GTK provider code from core repo (external provider template now documented).
- [x] Add `docs/PROVIDER_PACKAGING.md`.
- [x] Add test target for provider conformance.
- [x] Add CI job that validates "core-only" installation path.
- [x] Add CI job that validates "core + optional provider" path.

## Session Workflow (for future contributors/agents)

Per session:

1. Read this `PLAN.md` and align work to one phase.
2. State the exact acceptance criteria before editing.
3. Implement smallest coherent slice with tests.
4. Run local verification (`cmake`, `ctest`, packaging checks as needed).
5. Update `PLAN.md` checkboxes and add short changelog note in PR.

Do not:

- expand core dependencies for optional UX experiments
- introduce provider-protocol changes without docs + tests
- merge UX changes without flow-level validation

## Changelog Notes

- 2026-02-18: AUR packaging switched to deterministic minimal default (`BB_AUTH_GTK_FALLBACK=OFF`), with optional GTK fallback build via explicit opt-in.
- 2026-02-18: Removed in-tree GTK provider build/install from core; core packaging/CI now validates minimal Qt-first architecture.
- 2026-02-18: Added provider conformance test suite and external provider packaging guide.
- 2026-02-18: Added external provider template and Arch CI split for core-only plus drop-in provider template validation.
- 2026-02-18: Removed remaining in-tree GTK provider source/assets from core repository.
- 2026-02-18: Reworked provider contract into lock-candidate spec and expanded conformance coverage for heartbeat/tie-break/stale-prune behavior.
- 2026-02-18: Locked provider contract at IPC v2.0 and added IPC conformance tests for invalid JSON, missing type, unknown type, and oversized buffered input disconnects.
- 2026-02-18: Unified fallback prompt copy/rendering through shared prompt-model logic (polkit/keyring/pinentry) and added source-level model regression tests.
- 2026-02-18: Improved fallback requestor/action clarity by surfacing polkit action context (`actionId`/user), strengthening weak requestor identity display, and adding model tests.
- 2026-02-18: Tightened fallback error/cancel/retry/timeout UX with pending-action timeouts, pinentry retry status copy, and closed-error auto-dismiss coverage.
- 2026-02-18: Added keyboard-first fallback UX checks (Enter submit, keyboard cancel activation, tab-order traversal where supported) and explicit pending-action focus recovery.
- 2026-02-18: Completed fallback accessibility checks by hardening keyboard-only cancel recovery during submit-pending, switching UI text styling to theme-aware palette defaults for contrast resilience, and adding scaling/contrast regression tests.
- 2026-02-18: Added `scripts/gate-local.sh` and `docs/LOCAL_RELEASE_WORKFLOW.md` to enforce local pre-main build/test/install/daemon gates before release-facing merges.
