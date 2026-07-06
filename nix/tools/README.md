# codebase-memory-mcp — supervised daemon (nix-darwin / NixOS)

Two small C tools plus a module per OS that run
[`codebase-memory-mcp`](https://github.com/DeusData/codebase-memory-mcp) as a
supervised background daemon — a launchd user agent on macOS, a systemd user
service on Linux — with a CLI to drive it.

The MCP server itself — the C binary with its embedded 3D graph UI — is built by
`../package.nix`. **This directory is only the supervision layer.**

## Why a wrapper is needed

`codebase-memory-mcp` has no daemon mode. Its HTTP graph UI (default port 9749)
and git-watcher run as background threads of the **stdio MCP server**, which
blocks until stdin hits EOF (or SIGTERM). Under launchd/systemd stdin is
`/dev/null` → instant EOF → the process exits at once.

`cbm-daemon` fixes that: it opens a FIFO read-write (so a writer is always held
and the read loop never sees EOF), dup2's it onto stdin, and execs the server in
the **foreground**. The supervisor then tracks the real PID directly —
`KeepAlive` / `Restart=always` restarts it on crash, and stop/restart reach the
server's own graceful SIGTERM handler.

## Contents

- **`cbm-daemon.c`** — the foreground wrapper above (portable POSIX).
- **`cbm-ctl.c`** — control CLI (below); selects its supervisor backend at
  compile time (`launchctl` on macOS, `systemctl --user` on Linux).
- **`package.nix`** — compiles both into the `cbm-tools` package; tool paths
  (`codebase-memory-mcp`, `git`, `launchctl`/`systemctl`, …) are baked in via
  `-D`, so the binaries rely on nothing in `PATH`.
- **`../darwin/module.nix`** — the nix-darwin module, exported as
  `darwinModules.codebase-memory-mcp`.
- **`../nixos/module.nix`** — the NixOS module (systemd user service), exported
  as `nixosModules.codebase-memory-mcp`.

## Usage

Enable per host (same option set on both OSes):

```nix
# in a host module, e.g. modules/hosts/<host>.nix
services.codebase-memory-mcp.enable = true;
```

This puts `codebase-memory-mcp` + `cbm-ctl` on `PATH` and registers the daemon
with the OS supervisor, background / low-priority I/O in both cases:

- **macOS:** launchd user agent **`org.nixos.codebase-memory-mcp`**
  (`KeepAlive`, runs at load). Logs:
  `~/Library/Logs/org.nixos.codebase-memory-mcp.{out,err}.log`.
  Requires `system.primaryUser` (used for the per-user log paths); the module's
  `managedBy` back-reference auto-registers it in `system.requiresPrimaryUser`,
  so a null `primaryUser` surfaces nix-darwin's migration assertion rather than
  a raw coercion error.
- **Linux:** systemd user service **`codebase-memory-mcp.service`**
  (`Restart=always`, wanted by `default.target`). Logs: the user journal
  (`journalctl --user -u codebase-memory-mcp`).

| Option | Default | |
|---|---|---|
| `services.codebase-memory-mcp.enable` | `false` | the daemon + tools |
| `services.codebase-memory-mcp.package` | this flake's build | package to supervise |
| `services.codebase-memory-mcp.port` | `9749` | HTTP UI / daemon port |

### `cbm-ctl`

| Command | Does |
|---|---|
| `status` | daemon state, port listener, indexed projects |
| `flush [path]` | persist the index artifact for a repo |
| `commit [-m msg] [path]` | flush, then `git add`/`commit` `.codebase-memory` |
| `start` · `stop` · `restart` | control the user agent / user service |
| `logs` | follow the daemon logs |

`flush`/`commit` take a `mkdir`-atomic advisory lock so concurrent sessions
serialize heavy reindexing instead of piling on. (DB integrity is already
handled by the daemon's SQLite WAL + `busy_timeout`; the lock only avoids
redundant work and guards the git commit.)

## Build

```
nix build .#cbm-tools
```
