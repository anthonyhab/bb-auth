# bb-auth

`bb-auth` gives you one consistent authentication prompt on Linux for:

- polkit (`pkexec`)
- GNOME Keyring system prompts
- GPG pinentry

It runs as a user daemon and routes requests to a UI provider.
If no external provider is available, it automatically uses a built-in Qt fallback prompt.

![Fallback prompt](assets/screenshot.png)

## Why Use It

- One prompt style across system/admin/password requests.
- Better context in prompts (what is requesting auth, and why).
- Keyboard-friendly fallback UI.
- Works even if an optional external provider is missing or crashes.

## Who It Is For

Use `bb-auth` if you want one auth UX across your Linux session.
This is especially useful if you are tired of mixed prompt styles from different agents.

You may want to wait if you prefer the exact default KDE/GNOME behavior and do not want to switch prompt agents yet.

## What It Changes

`bb-auth` replaces prompt agents in your user session:

- polkit auth agent process in your session
- keyring system prompter for interactive prompts
- pinentry frontend process for interactive passphrase entry

`bb-auth` does not replace backend security systems:

- polkit authority/policies
- secret storage backend semantics
- GPG cryptography/agent model

## Quick Start

### 1. Install

Arch:

```bash
yay -S bb-auth-git
```

Nix:

```bash
nix profile install github:anthonyhab/bb-auth#bb-auth
```

Manual:

```bash
git clone https://github.com/anthonyhab/bb-auth
cd bb-auth
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
cmake --install build
```

### 2. Enable

```bash
systemctl --user daemon-reload
systemctl --user enable --now bb-auth.service
systemctl --user status bb-auth.service
```

### 3. Verify

```bash
pkexec echo ok
```

## Common Setup Problems

Check service and recent logs:

```bash
systemctl --user status bb-auth.service
journalctl --user -u bb-auth.service -n 200 --no-pager
```

GPG still opens terminal pinentry:

```bash
bb-auth-bootstrap
```

If polkit prompts do not appear, another polkit agent is usually active.
Stop the conflicting agent, then restart:

```bash
systemctl --user daemon-reload
systemctl --user restart bb-auth.service
```

More troubleshooting:

- `docs/TROUBLESHOOTING.md`

## Agent Supervision (AI agents)

When an AI agent (Claude Code, Codex, Gemini CLI, opencode, pi, Devin, …) runs
a privileged command, bb-auth shows **who** is asking and **why**, with an
"AI agent" badge on the prompt — and the agent's `sudo` becomes a handoff: the
harness steps aside and *you* approve the escalation at the bb-auth prompt.

Wire every harness you have installed:

```bash
bb-auth-agents install     # idempotent; backs up each config it edits
bb-auth-agents status      # per-harness wiring + hook, daemon, and polkit state
bb-auth-agents uninstall   # removes only bb-auth's entries
```

Restart agent sessions afterwards (Codex: trust the hook via `/hooks`).

What the hooks do, per privileged tool call:

- **Declare** who + why to the daemon. The reason is the agent's last message
  where the harness exposes a transcript (Claude Code, Codex, Devin, pi).
- **Hand off**: a clean `sudo CMD`/`doas CMD` is rewritten to `pkexec CMD` and
  approved at the harness level — but **only when polkit will prompt you**
  (`pkcheck` says a challenge is required). If a polkit rule would authorize
  pkexec silently, the command stays on the harness's own permission path.
- **Tell the agent** a human owns the decision, and (Claude Code) annotate the
  result for the auto mode classifier; a dismissed prompt (exit 126) is
  reported as "the user declined — don't route around it".

For Claude Code, `install` also adds two `bb-auth:` entries to `autoMode`
(`environment` and `soft_deny`, keeping `$defaults`) so the classifier treats
workarounds after a declined prompt as going around you.

Hookless agents (aider, cursor-agent, …) use one of:

- **PATH shims (zero cooperation):** `sudo`/`doas`/`pkexec` shims in
  `<libexec>/bb-auth-shims/` activate only under a recognized agent ancestor —
  a human's `sudo` passes through untouched:

  ```bash
  export PATH="$(aisudo --print-shim-dir):$PATH"
  ```

- **`aisudo` (voluntary):** `aisudo [-r "why"] [--] sudo CMD`, or bare
  `aisudo CMD`. `aisudo --help` carries the full grammar (`--json`,
  `--dry-run`).

Residual limits: absolute-path invocations (`/usr/bin/sudo`) bypass PATH shims;
attribution is display/audit only and never gates the allow/deny decision.
Requires `python3` for `aisudo`/`bb-auth-agents` (Arch: `python` optdepend).
Details: `integrations/hooks/README.md`.

## Provider Model (Advanced)

- UI providers are runtime drop-ins via manifests in `providers.d`.
- If no provider is available, the built-in Qt fallback is used automatically.

Provider and integration docs:

- `docs/PROVIDER_CONTRACT.md`
- `docs/PROVIDER_PACKAGING.md`
- `examples/provider-template/`

## Development

Build and test:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Pre-main local gates:

```bash
./scripts/gate-local.sh
```

Fast loop:

```bash
./scripts/gate-local.sh --quick
```

Replace local Arch install with your working tree build:

```bash
STRICT_DAEMON_SMOKE=1 ./scripts/gate-local.sh --deploy-local

# fastest packaging/install loop while iterating:
./scripts/gate-local.sh --deploy-only
```

Workflow:

- `docs/LOCAL_RELEASE_WORKFLOW.md`
- `PLAN.md`
- `AGENTS.md`

## License

MIT
