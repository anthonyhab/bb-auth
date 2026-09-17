This directory defines the high-level concepts, business logic, and architecture of this project using markdown. It is managed by [lat.md](https://www.npmjs.com/package/lat.md) — a tool that anchors source code to these definitions. Install the `lat` command with `npm i -g lat.md` and run `lat --help`.

- [[architecture]] — daemon core, provider stack, fallback UI, entry modes
- [[protocol]] — provider↔daemon IPC contract (v3.0) and versioning policy
- [[provider-trust]] — daemon-launch attestation and fail-closed authorization
- [[omarchy-prompt]] — resident quickshell provider themed by omarchy's design system
- [[agent-intent]] — who+why attribution for agent-initiated privileged commands
- [[tests]] — security-critical test specifications mapped to Qt tests
