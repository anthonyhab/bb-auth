# Tasks

## 1. Shim mode in bb-auth-declare

- [x] 1.1 Split `_detect_agent()` into "found agent kind or None" vs. the CLI's
  `"agent"` fallback, so shim mode can distinguish agent trees from human ones.
  Verify: `_detect_agent` returns `None` under a plain shell; unit-level check
  via a small python -c harness.
- [x] 1.2 Add argv0 dispatch: when `os.path.basename(sys.argv[0])` is
  `sudo`/`doas`/`pkexec`, run shim mode instead of the CLI parser. Verify:
  symlink the script to a temp name `sudo` and confirm shim path is taken.
- [x] 1.3 Implement `_resolve_real(name)`: scan `PATH` in order, return first
  executable whose realpath differs from the shim's own realpath; on failure
  exec basename with shim dir stripped from PATH env. Verify: under a test PATH
  containing only the shim dir + a stub dir, resolution returns the stub.
- [x] 1.4 Implement shim behavior: no agent ancestor → exec real binary verbatim;
  agent ancestor → `_rewrite_sudo`-style rewrite for clean `sudo CMD`/`doas CMD`
  to `pkexec CMD` + declare with `channel="shim"` and fixed generic reason; all
  other argv → passthrough with no declare. Verify: manual run under a faked
  agent ancestry (e.g. renamed `bash`→`agy` won't work; use a parent process
  named per signature, or stub `_detect_agent` in a test harness).
- [x] 1.5 Add `--print-shim-dir` CLI flag printing the installed shim directory.
  Verify: flag prints absolute path matching the CMake install destination.

## 2. Install + packaging

- [x] 2.1 CMake install: `libexec/bb-auth/shims/{sudo,doas,pkexec}` as absolute
  symlinks to `<bindir>/bb-auth-declare`. Verify: `cmake --install` into a DESTDIR
  produces the three links and they exec the script.
- [x] 2.2 PKGBUILD/nix: confirm shim dir lands in package (no new deps — python3
  already required). Verify: `gate-local.sh` install smoke passes.

## 3. Docs + knowledge graph

- [x] 3.1 README/TROUBLESHOOTING: shim activation (`export PATH="$(bb-auth-declare
  --print-shim-dir):$PATH"`), residual limits (absolute paths bypass), Python
  startup note. Verify: docs render and command works as written.
- [x] 3.2 lat.md: add `agent-intent#PATH shim channel` section + `@lat:` anchor in
  `bb-auth-declare`; update `agent-intent` channel list (hook, cli, shim).
  Verify: `lat check` passes.

## 4. Verification

- [x] 4.1 `./scripts/gate-local.sh` green (build + tests + install + daemon
  smoke). Verify: script exits 0.
- [x] 4.2 Manual smoke on live session: prepend shim dir, run `sudo true` as a
  human (transparent), then under a simulated agent ancestor run `sudo true`
  (bb-auth prompt with generic reason + `channel: "shim"`). Verify: both paths
  behave per spec.
