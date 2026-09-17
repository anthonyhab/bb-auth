# Provider IPC contract

v3.0 protocol over a mode-0600 Unix stream socket: provider↔daemon messages for registration, session delivery, and intent declaration. Normative source: `docs/PROVIDER_CONTRACT.md`.

## Transport and roles

`QLocalSocket`/`QLocalServer` with `UserAccessOption`; the daemon serves, providers connect and `ui.register`, and exactly one provider is *active* at a time.

The active provider is the sole recipient of `session.created` and the only socket authorized to `session.respond`/`session.cancel`.

## v3.0 authorization semantics

Major breaking change: free-standing providers may connect and register but can never become active — active status requires daemon-launch attestation per [[provider-trust#Provider trust model]].

- The legacy empty-registry allow was removed; authorization fails closed ([[provider-trust#Fail-closed authorization]]).
- v2.x free-standing providers must migrate to daemon-launched packaging (`docs/PROVIDER_PACKAGING.md`).

## v2.1 agent attribution additions

`session.created` carries `context.requestor {isAgent, agentKind, …}` (OS-resolved) and `context.intent {reason, declaredAgent, channel, mismatch}` (self-asserted) — see [[agent-intent#Agent intent surfacing]].

Normative boundary: `context.intent` and the declared agent id are **display/audit only** — providers and daemon MUST NOT use them in any authorization decision; `mismatch=true` SHOULD be surfaced, not hidden.

## Versioning policy

Minor (`x.y`) changes are additive and backward-compatible; removing fields, changing meaning, or changing authorization behavior requires a major bump.

Providers MUST ignore unknown daemon fields; unknown provider message types get an `error` reply (`Unknown type`).
