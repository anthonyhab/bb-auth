# Troubleshooting

## 1) Service health

```bash
systemctl --user status bb-auth.service
journalctl --user -u bb-auth.service -n 200 --no-pager
```

Expected:

- service is `active (running)`
- no repeating startup/bind failures

## 2) Polkit prompt does not appear

Likely cause: another polkit agent is active.

Check logs:

```bash
journalctl --user -u bb-auth.service -n 200 --no-pager | grep -E "conflict|polkit|agent"
```

Then restart:

```bash
systemctl --user daemon-reload
systemctl --user restart bb-auth.service
```

## 3) GPG prompt still uses terminal pinentry

Run bootstrap:

```bash
bb-auth-bootstrap
```

Then:

```bash
gpg-connect-agent reloadagent /bye
```

Verify expected pinentry link:

```bash
ls -l /usr/libexec/pinentry-bb
```

## 4) Provider UI does not show

Fallback should launch automatically if provider is missing/crashed.

Check provider discovery and launch logs:

```bash
journalctl --user -u bb-auth.service -n 200 --no-pager | grep -E "Provider|fallback|manifest"
```

Check drop-in manifests:

```bash
ls -l ~/.config/bb-auth/providers.d
ls -l ~/.local/share/bb-auth/providers.d
```

## 5) Agent escalations don't show attribution

If agent-initiated `sudo` reaches PAM directly, bb-auth never sees it — check
whether the shim dir is on the agent's PATH:

```bash
bb-auth-declare --print-shim-dir        # e.g. /usr/libexec/bb-auth-shims
echo "$PATH" | tr ':' '\n' | grep shims # inside the agent's shell
```

The shims only intercept PATH-resolved `sudo`/`doas`/`pkexec` under a recognized
agent ancestor; `/usr/bin/sudo` and option-bearing commands pass through by
design. See README → Agent Supervision.

## 6) Local dev gating

Before merge/release:

```bash
./scripts/gate-local.sh
```

Strict daemon gate:

```bash
STRICT_DAEMON_SMOKE=1 ./scripts/gate-local.sh
```
