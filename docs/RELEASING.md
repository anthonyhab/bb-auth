# Releasing

Forgejo (`git.hab.rip/habibe/bb-auth`) is the CI and release authority; the
GitHub repository is a public mirror and runs no Actions. `main` is
release-facing for AUR and Nix users.

The same gate runs in three places, so their results mean the same thing:
`./scripts/gate-local.sh` locally, the `gate` job of `.forgejo/workflows/ci.yml`
on every push, and the release job on a tag.

## Branch Model

1. Create a feature branch from `main`.
2. Commit locally on the feature branch.
3. Run local gates.
4. Push the feature branch to Forgejo (`hab`) and wait for CI:
   `fj watch --sha HEAD`.
5. Fast-forward `main` only after local gates + Forgejo CI are green, then
   mirror `main` to GitHub (fast-forward only on both; never force-push).

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

This builds from local git HEAD and installs via `pacman -U`.
If your working tree is dirty, `gate-local` also builds `build-check/bb-auth-fallback`
from your current edits and sets a runtime override:

```bash
systemctl --user set-environment BB_AUTH_FALLBACK_PATH=...
```

so fallback UI changes are exercised immediately without requiring a commit.

Fastest deploy loop while iterating on daemon/UI behavior:

```bash
./scripts/gate-local.sh --deploy-only
```

To force a clean package work dir before building:

```bash
CLEAN_LOCAL_PKG=1 ./scripts/gate-local.sh --deploy-only
```

If you want to clear the runtime fallback override manually:

```bash
systemctl --user unset-environment BB_AUTH_FALLBACK_PATH
systemctl --user restart bb-auth.service
```

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
- Keep branches small; before merging, rewrite *unpublished* history into one
  commit per logical change with a message that says why. Never rewrite
  history that has reached either remote.

Command aliases:

- `make gate-local` -> full local gate run
- `make gate-fast` -> quick gate run
- `make gate-release` -> strict daemon + local AUR smoke
- `make deploy-local` -> strict gates + install local package over AUR install

## Cutting a release

1. On the release branch: bump `VERSION`, move `[Unreleased]` in
   `CHANGELOG.md` to `[X.Y.Z] - date`, update `docs/COMPATIBILITY.md`. CI's
   "Release artifacts render" step fails if the tag, `VERSION`, or the
   changelog section disagree.
2. Fast-forward `main` once Forgejo CI is green; mirror to GitHub.
3. Tag the exact `main` commit and push the tag to Forgejo first:
   `git tag -a vX.Y.Z -m vX.Y.Z && git push hab vX.Y.Z`.
4. `.forgejo/workflows/release.yml` re-runs the full gate on that commit, then
   publishes a Forgejo release with the notes, a source tarball,
   `SHA256SUMS`, and the rendered AUR `PKGBUILD`/`SRCINFO`. That release is
   the authorization record.
5. Push the tag to GitHub (`git push origin vX.Y.Z`) — the AUR `PKGBUILD`
   fetches `git+https://github.com/anthonyhab/bb-auth.git#commit=<SHA>`, so
   the public build is pinned to the verified commit, not to a mutable ref.
6. Publish to the AUR: copy the release's `PKGBUILD` and `SRCINFO` (as
   `.SRCINFO`) into the `bb-auth` AUR repo, commit, push.

If a post-release fix is needed, cut a patch release (`vX.Y.(Z+1)`); never
move or re-push a published tag.
