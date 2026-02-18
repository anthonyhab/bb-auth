# Local Release Workflow (Main Guardrails)

Use this workflow to validate daemon changes before anything lands on `main`.
`main` is release-facing for AUR users, so avoid direct pushes without gates.

## Branch Model

1. Create a feature branch from `main`.
2. Commit locally on the feature branch.
3. Run local gates.
4. Push the feature branch and open a PR.
5. Merge to `main` only after local gates + CI are green.

Example:

```bash
git switch main
git pull --ff-only origin main
git switch -c fix/<short-name>
```

## Local Gates (One Command)

```bash
./scripts/gate-local.sh
```

This runs:

- `build-check`: configure/build/test
- `build-core`: configure/build/test
- install smoke (`cmake --install` into a temp prefix)
- daemon smoke (`BB_AUTH_SKIP_POLKIT=1` + socket ping)

Optional local AUR packaging smoke:

```bash
./scripts/gate-local.sh --aur-smoke
```

`--aur-smoke` rewrites PKGBUILD source to `git+file://<local-repo>` in a temp directory and runs `makepkg --nocheck`.

Install-test locally by replacing your current Arch package:

```bash
STRICT_DAEMON_SMOKE=1 ./scripts/gate-local.sh --deploy-local
```

This builds from your working tree and installs via `pacman -U`.

If daemon smoke cannot bind IPC in your current desktop/session policy, the gate is skipped by default.
To make that a hard failure, run with:

```bash
STRICT_DAEMON_SMOKE=1 ./scripts/gate-local.sh
```

## Fast Iteration

For quick inner-loop checks while coding:

```bash
./scripts/gate-local.sh --quick
```

Then run the full gates before opening/merging PR.

## Merge Discipline

- Do not push feature work directly to `main`.
- Keep PRs small and focused.
- Prefer squash merge for stacked/iterative AI-generated branches to reduce history clutter.

Command aliases:

- `make gate-local` -> full local gate run
- `make gate-fast` -> quick gate run
- `make gate-release` -> strict daemon + local AUR smoke
- `make deploy-local` -> strict gates + install local package over AUR install

## Release Discipline

After merge to `main`:

1. Bump `VERSION`.
2. Tag the release (`vX.Y.Z`) on the final commit.
3. Publish release notes.

If a post-release fix is needed, cut a patch release (`vX.Y.(Z+1)`), do not rewrite published release history.
