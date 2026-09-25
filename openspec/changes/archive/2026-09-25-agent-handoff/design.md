# Design

## Context

The hook (`bb-auth-intent-hook`, Qt6::Core console binary) runs per privileged
tool call. Claude Code evaluates `permissionDecision: "allow"` as "skip the
permission prompt and the auto mode classifier"; deny/ask *rules* still apply.
Codex accepts `updatedInput` only together with `allow` and rejects `ask`.

## Decisions

### Gate on `pkcheck` exit 2, not on "not YES"

`pkcheck --action-id org.freedesktop.policykit.exec --process <hook pid>`
without `--allow-user-interaction` exits 0 (authorized silently), 1 (not
authorized), 2 (challenge), other on error. Only 2 proves a human will see a
prompt, so only 2 unlocks the rewrite. Unprivileged callers cannot pass
`--detail program …`, so rules keyed on the program are not modeled — a rule
that says YES for one program but challenges generically would still get the
rewrite; documented residual. Subject is the hook process: same uid/session as
the later pkexec, which is what typical rules inspect. Bounded at 1 s; ~3 ms
measured.

Alternative rejected: emit `"ask"` when not challenged. Codex rejects `ask`,
and "don't rewrite" is uniform across harnesses and keeps the harness gate.

### One binary, event-dispatched

`hook_event_name` selects behavior: `PostToolUse`/`PostToolUseFailure` only
for Claude-shaped payloads (no `turn_id`) on a leading `pkexec`; absent or
pre-events keep today's path. One install path, one version.

`classifierContext` is truthful by construction: after a successful pkexec the
hook re-runs `pkcheck`; exit 2 means no retained authorization exists, so the
run must have authenticated. Exit 0 is reported as "authorized without a
prompt". Notes never relay tool output.

### Installer in Python, config-file surgery by marker

`bb-auth-agents` is a stdlib Python script (like aisudo). bb-auth entries are
identified by the hook command's basename `bb-auth-intent-hook` (hooks) or the
`bb-auth:` prefix (autoMode prose), so install = remove ours + add current,
and uninstall = remove ours. First modification of a file writes a
`<file>.bb-auth-backup` copy. Plugins/extensions are symlinked so package
upgrades propagate. `autoMode` arrays that do not exist are created with
`"$defaults"` first — creating them bare would discard the built-in rules.

Alternative rejected: a Claude Code plugin. Needs a marketplace manifest and
still leaves the other five harnesses manual; the installer covers all.

### Shared vectors, not shared code

Four languages remain (each harness loads its own runtime), but they now read
one fixture: `{command, rewrite|null}` rows exercised by each test file.

## Risks

- Settings files are user-owned; the installer rewrites JSON (formatting
  normalized to 2-space indent). Backup mitigates.
- Removing `bb-auth-declare` breaks anything still calling it; grep showed only
  docs/tests in-repo.
