# Proposal

## Why

Agent supervision works in tests but was dormant in practice: no harness on the
author's own machine had the hook wired, because wiring meant hand-pasting JSON
from a README. Once wired, the Claude/Codex rewrite answers the harness with
`permissionDecision: "allow"`, which skips the harness prompt *and* the auto
mode classifier on the assumption that polkit will ask the human. That
assumption fails silently under any polkit rule returning `YES` for
`org.freedesktop.policykit.exec` (e.g. systemd's `empower.rules`, common
"wheel = yes" rules): the result would be harness-approved, unprompted root.
Finally, the handoff is invisible to the model — it never learns that a human
now owns the decision, so after a dismissed prompt it tends to route around it
(`sudo -S`, askpass, `su`).

## What Changes

- **Challenge-gated allow**: the hook rewrites `sudo`→`pkexec` (and emits
  `allow`) only when `pkcheck` reports that polkit will challenge the user
  (exit 2). Silent authorization, denial, or a missing/failed `pkcheck` keeps
  the original command on the harness's own permission path.
- **Visible handoff**: the rewrite carries `additionalContext` telling the
  model a human approves at the GUI prompt and that a decline means no.
- **Post-run annotation (Claude Code)**: the same hook binary handles
  `PostToolUse` (`classifierContext` for auto mode: authorized by polkit after
  authentication vs without prompting) and `PostToolUseFailure` (pkexec exit
  126/127 → `additionalContext`: the user declined; do not circumvent).
- **Harness installer** `bb-auth-agents status|install|uninstall`: idempotent,
  backed-up edits of Claude Code, Codex, Gemini CLI and Devin hook configs;
  symlinks the opencode plugin and pi extension; adds bb-auth entries to
  Claude's `autoMode.environment`/`soft_deny` (preserving `$defaults`).
- **Shared translation vectors**: one fixture file
  (`tests/fixtures/escalation-translation.json`) run against every translator
  copy (C++ hook, Python aisudo, pi TS, opencode JS).
- **Transcript tail scan**: the hook reads the transcript backwards instead of
  parsing the whole JSONL on every escalation.
- **Drop `bb-auth-declare`**: the alias has no users outside this machine and
  doubles the name surface agents see.

## Impact

- `integrations/hooks/bb-auth-intent-hook.cpp`, new
  `integrations/agents/bb-auth-agents.in`, `CMakeLists.txt`, tests, docs,
  `lat.md/agent-intent.md`, skill (outside repo).
- No daemon, IPC protocol, or `intent.declare` payload changes.
