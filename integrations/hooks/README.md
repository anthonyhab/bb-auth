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
env assignments, or shell metacharacters is rewritten; anything compound or
option-bearing runs unchanged (still with intent declared), because pkexec's
flags and minimal environment differ from sudo's.

The daemon correlates a declaration to the polkit request by **shared agent
process ancestry** (pid *and* start-time, so a recycled pid fails closed) — not
by trusting the declared `agent` field. If the declared agent disagrees with the
resolved one, the prompt shows a mismatch warning. The hook never sends the
agent's command as authoritative: what is actually authorized stays with
polkit's own message.

## Per-harness setup

The hook binary lives at `<libexec>/bb-auth-intent-hook` (e.g.
`/usr/libexec/bb-auth-intent-hook`). Hooks are read at session start — restart
the agent after editing.

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
    ]
  }
}
```

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

### Others (opencode, codex, aider, …)

No hook format — use the PATH shims (`bb-auth-declare --print-shim-dir`, see
README → Agent Supervision) or the voluntary `bb-auth-declare` CLI. The opencode
plugin in `../opencode/` additionally covers command-string rewriting.

## Verify

In a fresh agent session, ask it to run a `sudo` command (e.g. `sudo pacman
-Syu`). It should be rewritten to `pkexec` and surface the bb-auth prompt showing
the agent badge and the declared reason. Run the same command directly in a
terminal (no agent) — it resolves normally with no reason, and the decision is
unaffected either way.
