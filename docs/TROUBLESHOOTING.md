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

## 5) Local dev gating

Before merge/release:

```bash
./scripts/gate-local.sh
```

Strict daemon gate:

```bash
STRICT_DAEMON_SMOKE=1 ./scripts/gate-local.sh
```
