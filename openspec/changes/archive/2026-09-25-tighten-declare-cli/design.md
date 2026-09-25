# Design

## Context

`integrations/agent-cli/aisudo.in` is one CMake-configured Python
script installed two ways: as the CLI in BINDIR and as `sudo`/`doas`/`pkexec`
shim symlinks under `<libexec>/bb-auth-shims/` (argv[0] dispatch). It declares
`intent.declare` over the daemon unix socket and execs a possibly-rewritten
command. Two near-identical translators exist today — `_rewrite_sudo` (CLI)
and `_shim_rewrite` (shim) — which already disagree in shape (one returns argv,
the other `None` on decline) and would drift further as the option subset
grows. The daemon payload is unchanged by this design; nothing past the socket
is touched.

## Goals / Non-Goals

**Goals:**

- One translator shared by CLI and shim, with an explicit supported `sudo`
  flag table.
- Grammar an agent can guess: minimal form is `aisudo CMD`, structured form is
  a single JSON object, and every failure prints the corrected invocation.
- `aisudo` name without breaking `bb-auth-declare`.

**Non-Goals:**

- No daemon, provider protocol, or `intent.declare` payload changes.
- No new dependencies; stdlib only (`argparse`, `json`, `shlex`).

## Decisions

### Dual name, not rename

The source is `aisudo.in`, configured and installed canonically as `aisudo`;
`bb-auth-declare` is installed via `install(PROGRAMS ... RENAME
bb-auth-declare)` and keeps working forever. Canonical docs and the skill
teach `aisudo`. Shim symlinks point at `bin/aisudo`; argv[0]-dispatch already
isolates behavior from the CLI name, so the alias costs nothing.
Alternative rejected: no new name — misses the tightness goal at zero cost to
add.

### Bare command ≡ `sudo` command

Because the name itself is the elevation verb, LAUNCHER is optional:
`aisudo make install` is normalized to `sudo make install` before anything
else runs, so the bare form inherits the existing contract wholesale —
`pkexec` when the daemon answers, real `sudo` when it does not. An explicit
`sudo`/`doas`/`pkexec` inside the command is still accepted and translated
per the shared rules; no launcher is required, but one is never wrong.

### One translator: `_translate(launcher, args) -> (argv | None, note)`

Walk `args` with a flag table: `-n`/`--non-interactive` drop,
`-u`/`--user USER` map to `--user USER`, `--` consumes end-of-options; first
non-flag token starts the command. Decline (return `None` + reason) on any
other flag, `VAR=val`, shell metacharacters in any argument, or no command
remaining (probe-only). The same table applies to `doas`; `pkexec` and
everything else never translate. Both entry paths call it:
`_shim_mode` on `argv[1:]`, CLI on the command remainder. Replaces
`_rewrite_sudo`/`_shim_rewrite` (private names, only tests reference them —
updated in the same change). The harness-hook copies (`bb-auth-intent-hook`
C++, `bb-auth-plugin.js`, `bb-auth-extension.ts`) were aligned to the same
subset so hook, shim, and CLI rewrite identically.

Why translate `-n` rather than decline: an agent's `-n` encodes "elevate
without stdin interaction"; the supervised GUI prompt is exactly that. The
decline path today produces a guaranteed `sudo: a password is required` —
strictly worse than a polkit prompt the user can answer or dismiss.

### Rewrite gate aligned to the shim's rule

CLI translates when the daemon *answered* (`bound is not None`), not only when
bound — matching shim behavior. An unbound (non-agent) CLI caller still gets
the supervised prompt rather than a terminal sudo it may not be able to
answer. Recorded for review; reverting to `bound is True` is a one-line gate
if the stricter rule is preferred.

### `--json` schema and errors

`--json` takes an inline object or `-` (stdin). `argv` (exact argv array) or
`command` (string, `shlex.split`) — `argv` wins if both given; `reason`,
`agent`, `cwd` optional strings. Unknown keys, non-object input, missing
command, and malformed JSON each exit non-zero naming the accepted keys.
`command`-as-string exists because models produce it naturally; `argv` is the
exact channel documented first.

### Reason defaulting and dry-run

`--reason` optional; default `" ".join(argv)` — informative at the prompt
since the band never renders `command` separately. Declare-only still requires
`--reason`. `--dry-run` resolves everything and prints
`{"declare": {...payload...}, "exec": [...], "note": "<translation note>"}`
to stdout, then exits zero without socket contact or exec — the
shape-assumption probe for agents and tests.

### Stderr notes

CLI translations and declines print one stderr line (`aisudo: sudo -n →
pkexec: GUI prompt replaces stdin auth`; `aisudo: sudo -E not translatable;
running via sudo`). Shim declines stay silent — shim passthrough must not
leak noise into agent transcripts.

## Risks / Trade-offs

- [`sudo -n CMD` as a credentials probe now pops a GUI prompt] → probe forms
  carry no command and decline; the note on stderr makes the translation
  visible in agent logs.
- [`command` string shlex-splitting misparses exotic quoting] → `argv` array
  is the exact path; `--help` and errors lead with it.
- [polkit vs sudoers authorization differs for `sudo -u` → `pkexec --user`]
  → both still require interactive auth; documented in `--help`.
- [installed copy lags the repo (observed today: deployed script predates
  shim mode)] → tasks end with a local install step so `aisudo` lands now.

## Migration Plan

`cmake --install` (or the local package build) adds `aisudo`; no removal of
`bb-auth-declare`, no PATH changes, no daemon restart needed beyond the
running service. Rollback is removing the extra installed name — the old CLI
grammar remains accepted throughout.
