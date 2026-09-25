# Tasks

## 1. Hook

- [x] 1.1 `pkcheck` challenge gate (bounded 1 s) before any rewrite/allow.
- [x] 1.2 `additionalContext` on the rewrite envelope.
- [x] 1.3 `hook_event_name` dispatch: Claude `PostToolUse` → `classifierContext`;
  `PostToolUseFailure` exit 126/127 → `additionalContext`; no declare.
- [x] 1.4 Backward transcript scan (tail chunks, 8 MiB cap).
- [x] 1.5 `--version` prints the build version.

## 2. Installer

- [x] 2.1 `integrations/agents/bb-auth-agents.in` with status/install/uninstall,
  `--dry-run`, backups, `$defaults`-preserving autoMode edits.
- [x] 2.2 CMake configure + install to BINDIR; ctest entry.

## 3. Translation vectors

- [x] 3.1 `tests/fixtures/escalation-translation.json`.
- [x] 3.2 Hook, aisudo, pi, opencode tests iterate the fixture (fix drift found).

## 4. Surface cleanup

- [x] 4.1 Remove `bb-auth-declare` install + doc/test references.

## 5. Docs

- [x] 5.1 `integrations/hooks/README.md`: installer first, manual JSON second,
  challenge gate + post events.
- [x] 5.2 `README.md`, `lat.md/agent-intent.md`, `lat.md/architecture.md`,
  `lat.md/tests.md` if new specs; `lat check`.
- [x] 5.3 `PLAN.md` Phase 6 (agent supervision).
- [x] 5.4 Skill (`~/.agents/skills/bb-auth/SKILL.md`): "state why first".

## 6. Gate and live install

- [x] 6.1 `./scripts/gate-local.sh` green.
- [x] 6.2 Install to `~/.local`, run `bb-auth-agents install`, verify `status`
  and a fresh-process hook run.
