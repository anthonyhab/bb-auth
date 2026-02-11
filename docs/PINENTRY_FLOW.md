# Pinentry Flow Contract

This document defines how pinentry authentication flows are represented in the daemon.

## Core Model

- One logical pinentry flow maps to one `session.id`
- Retries are emitted as `session.updated` on the same session id
- `session.closed` is terminal and emitted once

## State Model

For each session id, the daemon tracks:

- `pending_input`: waiting for UI response
- `awaiting_terminal`: password/confirmation sent to pinentry, waiting for terminal outcome
- `closed`: terminal state emitted

## Events

- New prompt: `session.updated` with `state:"prompting"` and `prompt`
- Retry: `session.updated` with `state:"prompting"` and `error`
- Terminal success/cancel/error: `session.closed`

## Security Rules

- Pinentry terminal results are accepted only from the owning peer pid
- Unknown sessions and invalid states are rejected
- Ambiguous flows fail closed (error), never inferred success

## Canonical Fields

Pinentry request payload uses canonical keys only:

- `prompt`
- `error` (optional)
- `repeat`
