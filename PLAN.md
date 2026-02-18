# bb-auth Plan

Last updated: 2026-02-18
Owner branch: `dev/jules-vet`

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

## Phase 1: Modular Foundation (current)

Goal: make "minimal core + drop-in providers" the enforced default.

Tasks:

- [ ] Remove in-tree GTK provider build/install from `CMakeLists.txt`.
- [ ] Keep provider manifest/launcher runtime behavior unchanged.
- [ ] Update `README.md` to document external-provider model first.
- [ ] Keep `PKGBUILD` deterministic-minimal by default.
- [ ] Ensure CI passes with no GTK provider in core build.

Exit criteria:

- Core builds and tests pass without GTK provider sources.
- No runtime regressions in daemon + Qt fallback flow.

## Phase 2: Provider Platform Hardening

Goal: make third-party provider integration safe and easy.

Tasks:

- [ ] Version and freeze provider protocol contract (`docs/PROVIDER_CONTRACT.md`).
- [ ] Add provider conformance test harness (register/heartbeat/subscribe/respond/cancel/error handling).
- [ ] Add a minimal external-provider template (single binary + manifest).
- [ ] Add provider packaging guide for Arch/Nix/manual installs.
- [ ] Validate precedence and conflict behavior across provider dirs.

Exit criteria:

- A non-C++ provider can pass conformance tests and run in production flow.
- Provider API changes require explicit versioning and migration notes.

## Phase 3: Best-in-Class Qt UX

Goal: polished, trustable prompts with clear context and low friction.

Tasks:

- [ ] Standardize visual language and copy across polkit/keyring/pinentry.
- [ ] Improve requestor identity and action clarity.
- [ ] Tighten error, cancel, retry, and timeout UX states.
- [ ] Add accessibility checks (keyboard-only flow, scaling, contrast).
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

- [ ] Define release gates (`build`, `ctest`, packaging smoke, service smoke).
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

- [ ] Refactor out in-tree GTK provider code into external reference package/repo.
- [ ] Add `docs/PROVIDER_PACKAGING.md`.
- [ ] Add test target for provider conformance.
- [ ] Add CI job that validates "core-only" installation path.
- [ ] Add CI job that validates "core + optional provider" path.

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
