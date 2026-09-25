# bb-auth architecture

A Qt6 daemon that routes the user's secret (sudo/polkit password, keyring unlock, pinentry PIN) to a trusted UI provider over a same-UID Unix socket, with a built-in Widgets fallback.

## Daemon core

The daemon owns the IPC socket, the session lifecycle, and every trust decision; providers are thin UI surfaces that never see routing logic.

- `CAgent` (`src/core/Agent.{hpp,cpp}`) — top-level orchestrator: binds the IPC server, tracks the active provider, eager/on-demand provider launches ([[omarchy-prompt#Resident eager launch]]), fails closed when no trusted provider exists.
- `IpcServer` (`src/core/ipc/`) — `QLocalServer` with `UserAccessOption` (mode 0600): every peer is a same-UID process, which defines the adversary model in [[provider-trust#Provider trust model]].
- `Session` (`src/core/Session.{hpp,cpp}`) + `agent/SessionStore` — a session is one pending secret request; `session.created`/`session.respond`/`session.cancel` flow over the socket per [[protocol#Provider IPC contract]]. Sessions carry a creation timestamp and expire after `SESSION_TTL_MS` (10 min) — the provider-maintenance timer sweeps them via `SessionStore::expiredIds` + `CAgent::closeSession` so an abandoned polkit/pinentry request cannot pin a prompt provider alive forever.
- `agent/MessageRouter` / `agent/EventRouter` / `agent/EventQueue` — inbound provider messages vs. daemon→provider event delivery. Session events go only to the active provider, never broadcast ([[provider-trust#Event delivery boundary]]).

## Provider stack

Everything under `src/core/providers/` plus the trust stores decides *which* UI may receive the secret; see [[provider-trust#Provider trust model]] for the model itself.

- `ProviderDiscovery` + `ProviderManifest` — find and parse installed provider manifests (`autostart` flag, `resident` flag, priority, launch command).
- `ProviderLauncher` (`src/core/providers/`) — spawns providers (`QProcess::startDetached`), records `{pid, start-time}` launch attestations.
- `ProviderTrustStore` (`src/core/agent/`) — single-use attestation records matched at `ui.register`.
- `ProviderRegistry` (`src/core/agent/`) — registered providers, active-provider selection, `isAuthorized` gate for `session.respond`.

## Fallback UI

`src/fallback/` is the built-in Qt **Widgets** prompt the daemon launches when no external provider is configured or none is trusted.

- `FallbackWindow` / `FallbackClient` — the window and its socket client.
- `fallback/prompt/` — prompt-text extraction and heuristics (`PromptExtractors`, `PromptHeuristics`, `PromptModelBuilder`, `TextNormalize`) that shape polkit's raw action message into the displayed prompt, including the agent attribution band ([[agent-intent#Attribution band]]).

## Entry modes

`src/modes/` selects the daemon's personality at startup: `daemon` (polkit agent), `keyring`, `pinentry`. `src/keyring-prompter/` is the keyring-side prompter client.

## Agent integrations

`integrations/` holds agent-facing glue that is **not** core: the `bb-auth-agent` multi-call binary (hook, `aisudo`, PATH shims, installer) and the opencode/pi plugins ([[agent-intent#Agent intent surfacing]]).

`bb-auth-intent-hook` auto-detects Claude Code/Codex/Devin `PreToolUse` and Gemini `BeforeTool` payloads, declares intent, and — when polkit will challenge — rewrites clean `sudo` → `pkexec`; it also annotates Claude Code post-run events. The `aisudo` script is installed as `sudo`/`doas`/`pkexec` shim symlinks that pass through for humans and declare + translate for agents. The opencode plugin and pi extension hook their harness's tool call for the same effect; `bb-auth-agents` wires all of them.
