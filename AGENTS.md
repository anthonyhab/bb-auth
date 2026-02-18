# AGENTS.md

Compact guide for coding agents in this repo.

## Mission

- Keep `bb-auth` core minimal, deterministic, and auth-safe.
- Keep optional UX/provider stacks out of core dependencies.
- Prefer small, test-backed diffs over broad rewrites.

## Read Order

1. `PLAN.md` (current phase + tasks)
2. `docs/PROVIDER_CONTRACT.md` (protocol boundary)
3. `README.md` (user-facing behavior)

## Repo Map

- `src/core/`: daemon, session, provider orchestration, IPC
- `src/fallback/`: built-in Qt fallback UI
- `src/modes/`: daemon/keyring/pinentry entry paths
- `tests/`: Qt tests and protocol/conformance checks
- `examples/provider-template/`: external provider template
- `docs/`: contract, packaging, troubleshooting, workflow

## One Command Before Main

```bash
./scripts/gate-local.sh
```

This runs:
- build + tests (`build-check`, `build-core`)
- install smoke
- daemon smoke (strict mode available)

Useful variants:

```bash
./scripts/gate-local.sh --quick
./scripts/gate-local.sh --aur-smoke
STRICT_DAEMON_SMOKE=1 ./scripts/gate-local.sh
```

## Change Rules

For non-trivial changes:

1. State acceptance criteria.
2. Implement smallest coherent slice.
3. Add/update tests.
4. Run local gates.
5. Update docs if behavior changed.

## Hard Boundaries

- Do not add optional provider/runtime deps to core package.
- Do not change provider protocol semantics without docs + tests.
- Do not merge auth-flow UX changes without keyboard-path validation.

## Release Discipline

- Treat `main` as release-facing.
- Work on feature/staging branches.
- Merge only when local gates + CI are green.
- Prefer squash merge to keep history compact.
