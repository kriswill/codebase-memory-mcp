# `.cbmignore` — Excluding Files from Indexing

`.cbmignore` is a project-specific ignore file that controls which files the
indexer sees. It uses gitignore-style syntax and is read from the **root of
the indexed directory** (`<repo>/.cbmignore`). Nested `.cbmignore` files in
subdirectories are not read.

It applies at **file discovery time** — the directory walk that selects files
for parsing. Every indexing path uses the same discovery: the initial
`index_repository`, manual re-indexing, and background auto-sync. A path
matched by `.cbmignore` never enters the graph. Changes to `.cbmignore` take
effect on the next (re-)index.

Unlike `.gitignore`, it has no effect on git itself — it only shapes what the
indexer sees. Commit it to share indexing excludes with your team, or list it
in `.gitignore` to keep personal excludes untracked.

To verify it works: directory subtrees skipped during discovery are reported
in the `index_repository` response under `excluded`
(`{"dirs": [up to 25 paths], "count": <total>, "truncated": <bool>}`).

## Syntax

One pattern per line. Blank lines are ignored, lines starting with `#` are
comments, and trailing whitespace is trimmed.

| Feature | Meaning |
|---|---|
| `*` | matches any run of characters, except `/` |
| `?` | matches exactly one character, except `/` |
| `**` | matches across directory boundaries (`**/name`, `dir/**`, `a/**/b`) |
| `[abc]`, `[a-z]` | character classes; `[!a-z]` / `[^a-z]` negate the class |
| trailing `/` | pattern matches **directories only** |
| `/` anywhere else | anchors the pattern to the repo root |
| no `/` in pattern | matches the file/directory name at **any depth** |
| leading `!` | negation — re-includes a previously matched path; the **last matching pattern wins** |

Examples:

```gitignore
# Generated protobuf output, anywhere in the tree
*.pb.go

# A specific top-level directory (leading / anchors to the repo root)
/third_party/

# Any directory named "snapshots", at any depth (trailing / = directories only)
snapshots/

# Everything under any fixtures directory
**/fixtures/**

# Anchored glob: generated clients for any single-character API version
/api/v?/generated/

# Character class: yearly log folders 2020-2029
/logs/202[0-9]/

# Ignore all YAML, but keep CI configs (negation — last match wins)
*.yaml
!ci.yaml
```

## Precedence

Discovery applies its filters in a fixed order — the first layer that rejects
a path wins. For directories:

1. **Built-in skip list** — `.git`, `node_modules`, `dist`, `target`,
   `vendor`, tool caches, etc. (60+ names). The **fast/moderate** index modes
   skip additional directories that usually hold non-core code — including
   `scripts`, `tools`, `bin`, `build`, `out`, `docs`, `examples`, `testdata`,
   `fixtures`, `migrations`, `e2e` — while **full** mode indexes them. A
   `.cbmignore` negation whose rule is the last match for the directory
   (e.g. `!scripts/`) un-skips a built-in entry, except the non-negatable
   safety core (`.git`, `node_modules`, `.worktrees`, `.claude-worktrees`).
   Directory names starting with `_` are NOT skipped (only the literal
   `_opam` cache is).
2. **Repo `.gitignore`** — `<repo>/.gitignore` merged with
   `<git-common-dir>/info/exclude` (worktree-aware); later patterns win on
   conflict. Honored even when the indexed directory is not a git repo root.
3. **Nested `.gitignore` files** — picked up during the walk and matched
   relative to their own directory.
4. **`.cbmignore`** — a positive match skips the path; a negated match can
   only rescue paths from layer 5.
5. **Git global excludes** — `core.excludesFile` from `~/.gitconfig` or the
   XDG git config (default `$XDG_CONFIG_HOME/git/ignore`); consulted only
   when the project is a git repo with a config.

For files, built-in suffix filters (`.png`, `.o`, `.db`, …; fast modes add
archives, media, lockfiles, `.min.js`, …) and fast-mode filename/substring
filters run **before** the ignore files, and a maximum-file-size cap runs
after them; none of these are overridable from `.cbmignore`. Symlinks are
always skipped.

## Negation (`!`) — current behavior

- **Within `.cbmignore`**: standard gitignore semantics. Patterns are
  evaluated top to bottom and the last matching pattern wins, so
  `!pattern` re-includes something an earlier line excluded.
- **Parent pruning** (same caveat as git): when a directory is excluded, the
  walk never descends into it — you cannot re-include a file whose parent
  directory is excluded. Negate the directory itself if you need its
  contents.
- **Across layers**: a `.cbmignore` negation overrides two layers:
  - the **git global excludes** layer. Example: your `~/.config/git/ignore`
    ignores `*.sql`, but this project's SQL should be indexed — add `!*.sql`
    to `.cbmignore`;
  - the **ordinary built-in skip directories** (#500) — `!scripts/`,
    `!obj/`, `!dist/`, … un-skip a built-in (or fast/moderate-mode) skip-list
    directory, so build-output-like directories that actually contain source
    can be indexed. A small safety core stays non-negatable by design —
    `.git`, `node_modules`, and worktree-internal directories — because
    indexing them risks OOM and correctness issues (see issues #489/#802).

  Negation cannot override the repo `.gitignore`/`info/exclude`, nested
  `.gitignore` files, the built-in suffix/filename filters, or the size cap.

### Planned (not yet implemented)

- Auxiliary filesystem walkers will honor the same ignore predicate as
  discovery, so every code path sees an identical ignore decision
  (unification tracked in a follow-up issue).
