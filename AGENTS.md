# AGENTS.md

Compact guide for coding agents in this repo.

## Mission

- Keep `bb-auth` core minimal, deterministic, and auth-safe.
- Keep optional UX/provider stacks out of core dependencies.
- Prefer small, test-backed diffs over broad rewrites.

## Read Order

1. `PLAN.md` (current phase + tasks)
2. `docs/PROVIDER_CONTRACT.md` (protocol boundary)
3. `README.md` (user-facing behavior)
4. `lat.md/` (knowledge graph — `lat search` for relevant sections)

## Repo Map

- `src/core/`: daemon, session, provider orchestration, IPC
- `src/fallback/`: built-in Qt fallback UI
- `src/modes/`: daemon/keyring/pinentry entry paths
- `tests/`: Qt tests and protocol/conformance checks
- `examples/provider-template/`: external provider template
- `docs/`: contract, packaging, troubleshooting, workflow

## One Command Before Main

```bash
./scripts/gate-local.sh
```

This runs:
- build + tests (`build-check`, `build-core`)
- install smoke
- daemon smoke (strict mode available)

Useful variants:

```bash
./scripts/gate-local.sh --quick
./scripts/gate-local.sh --aur-smoke
STRICT_DAEMON_SMOKE=1 ./scripts/gate-local.sh
```

## Change Rules

For non-trivial changes:

1. State acceptance criteria.
2. Implement smallest coherent slice.
3. Add/update tests.
4. Run local gates.
5. Update docs if behavior changed.

## Hard Boundaries

- Do not add optional provider/runtime deps to core package.
- Do not change provider protocol semantics without docs + tests.
- Do not merge auth-flow UX changes without keyboard-path validation.

## Release Discipline

- Treat `main` as release-facing.
- Work on feature/staging branches.
- Merge only when local gates + CI are green.
- Prefer squash merge to keep history compact.

## Docs Ownership

One source of truth per fact.

- `lat.md/` — living knowledge graph: architecture, domain concepts, trust model, test specs. Update sections in the same change that alters behavior; `lat check` must pass. (C++ symbol links `[[x.cpp#sym]]` are unsupported — use `// @lat:` comments for code→spec edges and plain `code` spans for spec→code.)
- `openspec/` — change workflow: `openspec/changes/` are proposals in flight, `openspec/specs/` are current capability specs. Drive non-trivial changes via the opsx propose → apply → verify → archive flow.
- `docs/adr/` — frozen decision history (0001–0003). Read for context; do not add new ADRs — new decisions land in openspec changes and lat.md sections.
- `docs/PROVIDER_CONTRACT.md` — normative IPC contract; `lat.md/protocol.md` summarizes, never restates it.

%% lat:begin %%
# Before starting work

- Run `lat search` to find sections relevant to your task. Read them to understand the design intent before writing code.
- Run `lat expand` on user prompts to expand any `[[refs]]` — this resolves section names to file locations and provides context.

# Post-task checklist (REQUIRED — do not skip)

After EVERY task, before responding to the user:

- [ ] Update `lat.md/` if you added or changed any functionality, architecture, tests, or behavior
- [ ] Run `lat check` — all wiki links and code refs must pass
- [ ] Do not skip these steps. Do not consider your task done until both are complete.

---

# What is lat.md?

This project uses [lat.md](https://www.npmjs.com/package/lat.md) to maintain a structured knowledge graph of its architecture, design decisions, and test specs in the `lat.md/` directory. It is a set of cross-linked markdown files that describe **what** this project does and **why** — the domain concepts, key design decisions, business logic, and test specifications. Use it to ground your work in the actual architecture rather than guessing.

# Commands

```bash
lat locate "Section Name"      # find a section by name (exact, fuzzy)
lat refs "file#Section"        # find what references a section
lat search "natural language"  # semantic search across all sections
lat expand "user prompt text"  # expand [[refs]] to resolved locations
lat check                      # validate all links and code refs
```

Run `lat --help` when in doubt about available commands or options.

If `lat search` fails because no API key is configured, explain to the user that semantic search requires a key provided via `LAT_LLM_KEY` (direct value), `LAT_LLM_KEY_FILE` (path to key file), or `LAT_LLM_KEY_HELPER` (command that prints the key). Supported key prefixes: `sk-...` (OpenAI) or `vck_...` (Vercel). If the user doesn't want to set it up, use `lat locate` for direct lookups instead.

# Syntax primer

- **Section ids**: `lat.md/path/to/file#Heading#SubHeading` — full form uses project-root-relative path (e.g. `lat.md/tests/search#RAG Replay Tests`). Short form uses bare file name when unique (e.g. `search#RAG Replay Tests`, `cli#search#Indexing`).
- **Wiki links**: `[[target]]` or `[[target|alias]]` — cross-references between sections. Can also reference source code: `[[src/foo.ts#myFunction]]`.
- **Source code links**: Wiki links in `lat.md/` files can reference functions, classes, constants, and methods in TypeScript/JavaScript/Python/Rust/Go/C files. Use the full path: `[[src/config.ts#getConfigDir]]`, `[[src/server.ts#App#listen]]` (class method), `[[lib/utils.py#parse_args]]`, `[[src/lib.rs#Greeter#greet]]` (Rust impl method), `[[src/app.go#Greeter#Greet]]` (Go method), `[[src/app.h#Greeter]]` (C struct). `lat check` validates these exist.
- **Code refs**: `// @lat: [[section-id]]` (JS/TS/Rust/Go/C) or `# @lat: [[section-id]]` (Python) — ties source code to concepts

# Test specs

Key tests can be described as sections in `lat.md/` files (e.g. `tests.md`). Add frontmatter to require that every leaf section is referenced by a `// @lat:` or `# @lat:` comment in test code:

```markdown
---
lat:
  require-code-mention: true
---
# Tests

Authentication and authorization test specifications.

## User login

Verify credential validation and error handling for the login endpoint.

### Rejects expired tokens
Tokens past their expiry timestamp are rejected with 401, even if otherwise valid.

### Handles missing password
Login request without a password field returns 400 with a descriptive error.
```

Every section MUST have a description — at least one sentence explaining what the test verifies and why. Empty sections with just a heading are not acceptable. (This is a specific case of the general leading paragraph rule below.)

Each test in code should reference its spec with exactly one comment placed next to the relevant test — not at the top of the file:

```python
# @lat: [[tests#User login#Rejects expired tokens]]
def test_rejects_expired_tokens():
    ...

# @lat: [[tests#User login#Handles missing password]]
def test_handles_missing_password():
    ...
```

Do not duplicate refs. One `@lat:` comment per spec section, placed at the test that covers it. `lat check` will flag any spec section not covered by a code reference, and any code reference pointing to a nonexistent section.

# Section structure

Every section in `lat.md/` **must** have a leading paragraph — at least one sentence immediately after the heading, before any child headings or other block content. The first paragraph must be ≤250 characters (excluding `[[wiki link]]` content). This paragraph serves as the section's overview and is used in search results, command output, and RAG context — keeping it concise guarantees the section's essence is always captured.

```markdown
# Good Section

Brief overview of what this section documents and why it matters.

More detail can go in subsequent paragraphs, code blocks, or lists.

## Child heading

Details about this child topic.
```

```markdown
# Bad Section

## Child heading

Details about this child topic.
```

The second example is invalid because `Bad Section` has no leading paragraph. `lat check` validates this rule and reports errors for missing or overly long leading paragraphs.
%% lat:end %%
