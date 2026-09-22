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
a privileged command, bb-auth shows **who** is asking and — when declared —
**why**, with an "AI agent" badge on the prompt.

Three channels, in order of reliability:

- **PATH shims (zero cooperation):** `sudo`/`doas`/`pkexec` shims install inertly
  to `<libexec>/bb-auth-shims/`. They activate only under a recognized agent
  ancestor — a human's `sudo` passes through untouched. To enable, prepend the
  shim dir in your shell profile so agent processes inherit it:

  ```bash
  export PATH="$(aisudo --print-shim-dir):$PATH"
  ```

  Under an agent, `sudo CMD`/`doas CMD` are routed through the supervised polkit
  prompt even if the agent never declared anything.

- **Harness hooks:** a compiled `PreToolUse`/`BeforeTool` hook
  (`integrations/hooks/`) covers Claude Code, Codex CLI, Devin, and Gemini CLI;
  opencode and pi get a native plugin/extension (`integrations/`). These can
  carry the agent's real rationale to the prompt where the harness exposes a
  transcript or session state.

- **Skill / manual:** agents wrap commands directly — `aisudo [-r "why"] [--]
  sudo CMD`, or bare `aisudo CMD` (the name is the verb). Structured input via
  `aisudo --json '{"argv": [...], "reason": "…"}'`; `aisudo --help` carries the
  full grammar. `bb-auth-declare` remains as a compatibility alias.

Residual limits: absolute-path invocations (`/usr/bin/sudo`) bypass PATH shims;
attribution is display/audit only and never gates the allow/deny decision.
Requires `python3` for the declare/shim tooling (Arch: `python` optdepend).

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
