# AGENTS.md

Project guidance for coding agents and contributors in this repository.

## Primary Reference

- Read `PLAN.md` first.
- Align each session to one phase and a small set of checkboxes.

## Core Rules

1. Keep the core package minimal and deterministic.
2. Keep optional providers out of core dependencies.
3. Preserve provider runtime drop-in behavior through `providers.d`.
4. Treat `docs/PROVIDER_CONTRACT.md` as a compatibility contract.
5. Prefer simple, test-backed changes over broad refactors.

## Architecture Boundary

Core (`bb-auth`) owns:

- daemon/session/provider orchestration
- provider discovery/validation/launch
- built-in Qt fallback provider

Optional provider packages own:

- desktop-specific provider binaries (GTK, others)
- their dependency stacks and release cadence

## Change Process

For every non-trivial change:

1. State acceptance criteria before coding.
2. Implement the smallest coherent slice.
3. Add or update tests.
4. Run verification commands.
5. Update docs when behavior changes.

## Verification Baseline

Run before merge:

```bash
cmake -S . -B build-check -DCMAKE_BUILD_TYPE=Release
cmake --build build-check -j"$(nproc)"
ctest --test-dir build-check --output-on-failure
```

If packaging/build options changed, also test:

```bash
cmake -S . -B build-core -DCMAKE_BUILD_TYPE=Release
cmake --build build-core -j"$(nproc)"
ctest --test-dir build-core --output-on-failure
```

## Do Not

- Add optional UX stacks as hard dependencies in core packaging.
- Change provider protocol semantics without contract and tests.
- Merge untested UX behavior changes in auth-critical flows.
