# Tasks

## 1. Translator

- [x] 1.1 Replace `_rewrite_sudo`/`_shim_rewrite` with a shared
  `_translate(launcher, args)` in `integrations/agent-cli/aisudo.in`
  implementing the supported `sudo` subset (`-n` drop, `-u`/`--user` map,
  `--` consume), declining on other flags, `VAR=`, metacharacters, and
  probe-only invocations. Verify: `python3 tests/test_declare_shim.py`
  translator unit rows pass.
- [x] 1.2 Wire both entry paths (`main` CLI remainder and `_shim_mode`) through
  the shared translator; CLI translates when the daemon answered
  (`bound is not None`), shim unchanged. Verify: shim + CLI rewrite tests pass.
- [x] 1.3 Align the harness-hook rewrite copies (`bb-auth-intent-hook.cpp`,
  `integrations/opencode/bb-auth-plugin.js`,
  `integrations/pi/bb-auth-extension.ts`) to the same option subset, and the
  live pi `tool-pipeline` guard to accept `aisudo`. Verify:
  `test_intent_hook.py` + `test_pi_extension.py` `-n`/`-u` cases pass.

## 2. CLI grammar

- [x] 2.1 `--reason`/`-r` optional with command-derived default; declare-only
  still errors requiring it. Verify: declare-only case exits non-zero with the
  required-reason message.
- [x] 2.2 `--json` (inline object or `-` stdin) with `argv`/`command`,
  optional `reason`/`agent`/`cwd`, strict unknown-key rejection with
  corrective errors. Verify: JSON happy path + each failure mode test passes.
- [x] 2.3 `--dry-run` prints `{"declare": ..., "exec": ..., "note": ...}` and
  exits without socket contact or exec. Verify: dry-run test asserts no daemon
  payload and no exec.
- [x] 2.4 Self-correcting errors + `--help` epilog documenting grammar, JSON
  schema, and the sudo option subset. Verify: `aisudo --help` output lists all
  three; misuse paths print the canonical form.

## 3. Install surface

- [x] 3.1 `CMakeLists.txt`: install the configured program a second time as
  `aisudo` in BINDIR. Verify: `cmake --install` staging shows both names.

## 4. Tests

- [x] 4.1 Update `tests/test_declare_shim.py` expectations (`sudo -u nobody
  id` now rewrites to `pkexec --user nobody id`; unsupported-option
  passthrough uses e.g. `sudo -E`). Add rows: `sudo -n CMD` rewrite (shim +
  CLI), probe-only decline, `--` handling, decline notes on stderr.
  Verify: full file green.
- [x] 4.2 Add CLI-mode cases: JSON inline/stdin, dry-run, default reason,
  corrective errors. Verify: full file green.

## 5. Docs

- [x] 5.1 `README.md` agent-supervision section: teach `aisudo` minimal form.
- [x] 5.2 `lat.md/agent-intent.md` declare-CLI section + `architecture.md`
  mention: new name, grammar, translation subset; `lat check` passes.
- [x] 5.3 `~/.agents/skills/bb-auth/SKILL.md` (outside repo): shrink to the
  `aisudo` one-liner, JSON form, `sudo -n` note, and `--help` pointer.

## 6. Gate and install

- [x] 6.1 `./scripts/gate-local.sh` (or its scoped equivalent for a
  python-only change) green.
- [x] 6.2 Install locally so `aisudo` and the updated shim land on the live
  system (`cmake --install` or local package flow). Verify:
  `aisudo --dry-run -- sudo -n true` prints the pkexec plan.
