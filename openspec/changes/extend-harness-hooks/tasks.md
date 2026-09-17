# Tasks

## 1. Codex detection in intent hook

- [x] 1.1 Detect codex payloads via `turn_id`/`tool_use_id` discriminators;
  accept `tool_name` `Bash`/`exec_command` or `matcher_aliases` containing
  `Bash`; emit `agent: "codex"`.
- [x] 1.2 Verify emitted `hookSpecificOutput` envelope matches codex's
  `permissionDecision: "allow"` + `updatedInput` contract (already does).

## 2. pi extension

- [x] 2.1 `integrations/pi/bb-auth-extension.ts`: `tool_call` handler — declare
  + in-place `event.input.command` rewrite gated on daemon `ok`; fail-open.
- [x] 2.2 Add `pi-coding-agent` segment signature to daemon detector
  (`RequestContext.cpp`) and shim (`bb-auth-declare.in`); NOT bare `pi`.

## 3. Docs

- [x] 3.1 `integrations/hooks/README.md`: codex `hooks.json` example + `/hooks`
  trust-review caveat; pi extension install path.
- [x] 3.2 `lat.md/agent-intent.md`: harness coverage list + pi signature note.

## 4. Tests & gate

- [x] 4.1 `test_intent_hook.py`: codex payload (rewrite + `agent: "codex"`),
  claude-without-turn_id stays claude-code.
- [x] 4.2 Signature test rows for `pi-coding-agent` detection and bare `pi`
  rejection (daemon C++ test + shim python test).
- [x] 4.3 `./scripts/gate-local.sh` green.
