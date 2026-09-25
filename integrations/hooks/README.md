# bb-auth agent intent hook

Make AI agents safer to run in auto-approve mode: when an agent runs a privileged
command (`sudo`/`pkexec`/`doas`), surface **who** is asking and **why** at the
bb-auth authentication prompt — and route the escalation through bb-auth's
supervised prompt instead of an unsupervised `sudo`.

`bb-auth-intent-hook.cpp` is a small compiled `Qt6::Core` console binary
(installed to the daemon's `libexec`) that auto-detects the harness from the
hook payload. It does two display/audit-only things, both fail-open:

1. **Declares who + why.** It POSTs `intent.declare` over the bb-auth socket
   (extracting the most recent assistant rationale when the harness exposes a
   transcript). The daemon attributes the request from process ancestry and
   shows the reason at the prompt.
2. **Routes `sudo` → `pkexec`.** A *simple* leading `sudo CMD` is rewritten to
   `pkexec CMD` via the harness's tool-input merge channel, so the escalation
   goes through bb-auth's polkit prompt — attributed and human-supervised —
   rather than sudo's PAM path, which bb-auth never sees.

## Trust model (important)

The declared reason and agent id are **self-asserted and display/audit only**.
bb-auth **never** uses them for the allow/deny decision — that stays with polkit
and the daemon's OS-resolved process identity (`docs/PROVIDER_CONTRACT.md` §3.1).
The hook **fails open**: malformed input, an unreachable daemon, or any error
leaves the command unchanged and lets it run. The `sudo`→`pkexec` rewrite is
deliberately conservative — only a clean leading `sudo CMD` with no options,
env assignments, or shell metacharacters is rewritten, and **only when the
daemon acknowledges** the declaration: with no supervised agent running,
`pkexec` could hard-fail where `sudo` would have worked, so a dead daemon means
the command runs on its original auth path. The rewrite shares the `aisudo`
option subset — `-n`/`--non-interactive` drops, `-u`/`--user` maps to
`pkexec --user`, `--` ends options — while other options, env assignments,
compound commands, and probe-only invocations run unchanged (still with
intent declared), because pkexec's flags and minimal environment differ from
sudo's.

On Claude-format harnesses the rewrite envelope carries
`permissionDecision: "allow"` — required for the harness to merge the updated
input. That "allow" skips the *harness's own* permission prompt (and Claude
Code's auto mode classifier) for the rewritten call, handing the decision to
the human at the bb-auth prompt. It never approves the original `sudo` — the
tool input is replaced.

Because "allow" removes the harness gate, the hook first runs `pkcheck
--action-id org.freedesktop.policykit.exec` (no interaction, ~3 ms) and
rewrites **only when polkit reports that it will challenge the user** (exit 2).
If a polkit rule would authorize pkexec silently (e.g. systemd's
`empower.rules`, a "wheel = YES" rule), or pkcheck is missing or fails, the
command stays on the harness's own permission path. Residual: unprivileged
callers cannot pass pkexec's `program` detail to pkcheck, so a rule that says
YES only for specific programs is not modeled.

The rewrite also carries `additionalContext` telling the model that a human
approves at the GUI prompt and that exit 126/127 means "declined — don't route
around it". For Claude Code, the same binary handles `PostToolUse` (a
`classifierContext` note for auto mode: authorized after authentication, or
without a prompt) and `PostToolUseFailure` (pkexec exit 126/127 →
`additionalContext`). Post events never declare intent and never relay tool
output.

The daemon correlates a declaration to the polkit request by **shared agent
process ancestry** (pid *and* start-time, so a recycled pid fails closed) — not
by trusting the declared `agent` field. If the declared agent disagrees with the
resolved one, the prompt shows a mismatch warning. The hook never sends the
agent's command as authoritative: what is actually authorized stays with
polkit's own message.

## Setup

```bash
bb-auth-agents install          # every detected harness
bb-auth-agents install claude   # or name them
bb-auth-agents status
```

The installer edits each harness's config idempotently (first edit saves
`<file>.bb-auth-backup`), touches only entries whose hook command is
`bb-auth-intent-hook` (plus Claude `autoMode` strings prefixed `bb-auth:`), and
symlinks the opencode plugin / pi extension so upgrades propagate. `status`
also reports the hook version, daemon reachability, and whether polkit will
prompt (i.e. whether rewrites are active). The manual snippets below are what
it writes.

## Per-harness setup (manual)

The hook binary lives at `<libexec>/bb-auth-intent-hook` (e.g.
`/usr/libexec/bb-auth-intent-hook`; `--version` prints its build). Hooks are
read at session start — restart the agent after editing.

### Claude Code

`PreToolUse` matcher keys on tool *name*; `if`-gating on the command prefix keeps
the hook from spawning on ordinary Bash calls — it only runs when an agent is
actually escalating privilege (`~/.claude/settings.json`):

```json
{
  "hooks": {
    "PreToolUse": [
      { "matcher": "Bash",
        "hooks": [
          { "type": "command", "if": "Bash(sudo *)",   "command": "/usr/libexec/bb-auth-intent-hook" },
          { "type": "command", "if": "Bash(pkexec *)", "command": "/usr/libexec/bb-auth-intent-hook" },
          { "type": "command", "if": "Bash(doas *)",   "command": "/usr/libexec/bb-auth-intent-hook" }
        ] }
    ],
    "PostToolUse": [
      { "matcher": "Bash",
        "hooks": [ { "type": "command", "if": "Bash(pkexec *)", "command": "/usr/libexec/bb-auth-intent-hook" } ] }
    ],
    "PostToolUseFailure": [
      { "matcher": "Bash",
        "hooks": [ { "type": "command", "if": "Bash(pkexec *)", "command": "/usr/libexec/bb-auth-intent-hook" } ] }
    ]
  }
}
```

`bb-auth-agents install claude` additionally appends a `bb-auth:` entry to
`autoMode.environment` (how escalation is supervised here) and
`autoMode.soft_deny` (retrying via `sudo -S`/askpass/`su`/`run0`/sudoers edits
after a declined prompt), creating either list with `"$defaults"` first so the
built-in rules stay in effect.

Claude Code provides `transcript_path`, so the prompt gets the agent's real
rationale.

### Devin CLI

Same Claude-format hook protocol (`.devin/hooks.v1.json` or
`~/.config/devin/config.json`). Devin's shell tool is `exec` and its matcher is
a regex on tool name only — no `if` gate — so the hook spawns on every exec and
self-filters privileged prefixes inside (~1 ms):

```json
{
  "hooks": {
    "PreToolUse": [
      { "matcher": "^exec$",
        "hooks": [
          { "type": "command", "command": "/usr/libexec/bb-auth-intent-hook" }
        ] }
    ]
  }
}
```

### Gemini CLI

`BeforeTool` hook on the `run_shell_command` tool in `~/.gemini/settings.json`
(or project `.gemini/settings.json`). Matcher is a regex on the tool name; the
hook rewrites via `hookSpecificOutput.tool_input`:

```json
{
  "hooks": {
    "BeforeTool": [
      { "matcher": "^run_shell_command$",
        "hooks": [
          { "name": "bb-auth-intent", "type": "command",
            "command": "/usr/libexec/bb-auth-intent-hook" }
        ] }
    ]
  }
}
```

Gemini does not expose a transcript path, so the reason falls back to
"(no rationale captured)" — the escalation is still attributed, routed through
pkexec, and the agent is identified by ancestry.

### Codex CLI

Codex's `PreToolUse` hooks reuse the Claude payload shape and add codex-specific
fields (`turn_id`, `matcher_aliases`), which the hook uses to tell them apart.
`~/.codex/hooks.json` (or `<repo>/.codex/hooks.json`):

```json
{
  "hooks": {
    "PreToolUse": [
      { "matcher": "Bash",
        "hooks": [
          { "type": "command", "command": "/usr/libexec/bb-auth-intent-hook" }
        ] }
    ]
  }
}
```

The matcher also covers unified `exec_command` calls via `matcher_aliases`.
Rewrites merge through `hookSpecificOutput.updatedInput` gated on
`permissionDecision: "allow"`.

**Trust review:** codex skips non-managed hooks until you approve them — after
installing, run `/hooks` in the CLI and trust the hook definition (changed
hook content re-triggers review). For vetted automation,
`codex --dangerously-bypass-hook-trust` skips the check for that invocation.

### opencode

opencode has a plugin API instead of hook commands. Install the plugin into
`~/.config/opencode/plugins/` (or the project's `.opencode/plugin/`):

```bash
bb-auth-agents install opencode   # symlinks the packaged plugin
```

The plugin hooks `tool.execute.before` on the `bash` tool: it declares intent
over the bb-auth socket and rewrites a clean leading `sudo`/`doas` to `pkexec`
via the mutable `output.args.command`. Note: some opencode versions have an
upstream bug where `output.args` mutations do not propagate — if the rewrite
does not take effect, the declaration still lands and the PATH shims remain
the reliable fallback.

### pi

pi extensions are TypeScript/JavaScript modules loaded by jiti. Install the
extension into `~/.pi/agent/extensions/` (global) or `.pi/extensions/`
(project-local), then `/reload`:

```bash
bb-auth-agents install pi   # symlinks the packaged extension
```

The extension hooks `tool_call` on the `bash` tool: it declares intent
(extracting the last assistant message via `ctx.sessionManager` for a real
reason) and rewrites a clean leading `sudo`/`doas` by mutating
`event.input.command` in place — only when the daemon acknowledges.

Note on detection: pi is recognized by the `pi-coding-agent` npm package dir
in the process cmdline, not the bare `pi` binary name (which collides with an
unrelated Debian utility). If pi runs under a different wrapper, its ancestor
name may not resolve — the declare still binds to whatever agent ancestry the
daemon sees.

### Others (aider, cursor-agent, …)

No hook format — use the PATH shims (`aisudo --print-shim-dir`, see
README → Agent Supervision) or the voluntary `aisudo` CLI.

## Verify

In a fresh agent session, ask it to run a `sudo` command (e.g. `sudo pacman
-Syu`). It should be rewritten to `pkexec` and surface the bb-auth prompt showing
the agent badge and the declared reason. Run the same command directly in a
terminal (no agent) — it resolves normally with no reason, and the decision is
unaffected either way.
