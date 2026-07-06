# NixOS module for codebase-memory-mcp, exported as
# `nixosModules.codebase-memory-mcp` (and `.default`). Parameterized over this
# flake's `self` so `package`/`tools` default to the flake's own builds for the
# host system — consumers need no overlay or callPackage wiring.
#
# Linux twin of nix/darwin/module.nix: the same cbm-daemon wrapper runs as a
# systemd *user* service. The binary has no daemon mode (its HTTP UI +
# git-watcher are background threads of the stdio MCP server, which exits on
# stdin EOF), so cbm-daemon hands it a never-EOF stdin via a FIFO and execs it
# in the foreground. systemd then tracks the real PID directly —
# Restart=always restarts on crash, SIGTERM (systemctl --user stop/restart)
# shuts it down through the server's own graceful handler.
self:
{
  lib,
  config,
  pkgs,
  ...
}:
let
  cfg = config.services.codebase-memory-mcp;
  sys = pkgs.stdenv.hostPlatform.system;
  cbmTools = self.packages.${sys}.cbm-tools;
in
{
  options.services.codebase-memory-mcp = {
    enable = lib.mkEnableOption "systemd-user-supervised codebase-memory-mcp daemon + control CLI (cbm-ctl)";

    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${sys}.codebase-memory-mcp;
      defaultText = lib.literalExpression "codebase-memory-mcp.packages.\${system}.codebase-memory-mcp";
      description = "The codebase-memory-mcp package to supervise.";
    };

    port = lib.mkOption {
      type = lib.types.port;
      default = 9749;
      description = "TCP port for the codebase-memory-mcp HTTP UI / daemon.";
    };
  };

  config = lib.mkIf cfg.enable {
    environment.systemPackages = [
      cfg.package # codebase-memory-mcp (also the bare stdio MCP server for .mcp.json)
      cbmTools # cbm-ctl + cbm-daemon
    ];

    # cbm-ctl targets exactly this unit name (systemctl --user / journalctl
    # --user -u codebase-memory-mcp). Nice + best-effort/7 I/O scheduling keep
    # the watcher's reindexing off concurrent clients (the launchd module's
    # ProcessType=Background + LowPriorityIO + Nice analogue); RestartSec
    # mirrors ThrottleInterval. Logs go to the user journal.
    systemd.user.services.codebase-memory-mcp = {
      description = "codebase-memory-mcp daemon (HTTP graph UI + git watcher)";
      wantedBy = [ "default.target" ];
      environment = {
        CBM_BIN = lib.getExe cfg.package;
        CBM_PORT = toString cfg.port;
      };
      serviceConfig = {
        ExecStart = "${cbmTools}/bin/cbm-daemon";
        Restart = "always";
        RestartSec = 10;
        Nice = 5;
        IOSchedulingClass = "best-effort";
        IOSchedulingPriority = 7;
      };
    };
  };
}
