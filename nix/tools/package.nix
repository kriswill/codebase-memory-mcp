{
  lib,
  stdenv,
  git,
  codebase-memory-mcp,
  # Linux-only (unused on darwin, where the Apple system tools are baked in).
  lsof ? null,
  systemd ? null,
}:
# cbm-tools — two tiny C programs that supervise and drive the
# codebase-memory-mcp daemon:
#   cbm-daemon  supervisor-friendly foreground wrapper (holds stdin open via a FIFO)
#   cbm-ctl     control CLI (status / flush / commit / start|stop|restart / logs)
# The supervisor backend is launchd (user agent) on macOS and a systemd user
# service on Linux; cbm-ctl selects its backend at compile time.
# Tool paths are baked in at compile time, so the binaries need nothing on PATH.
let
  backendDefs =
    if stdenv.hostPlatform.isDarwin then
      ''
        -DLAUNCHCTL='"/bin/launchctl"' \
        -DLSOF='"/usr/sbin/lsof"' \
        -DTAIL='"/usr/bin/tail"' \
      ''
    else
      ''
        -DSYSTEMCTL='"${systemd}/bin/systemctl"' \
        -DJOURNALCTL='"${systemd}/bin/journalctl"' \
        -DLSOF='"${lib.getExe lsof}"' \
      '';
in
stdenv.mkDerivation {
  pname = "cbm-tools";
  inherit (codebase-memory-mcp) version;

  # Just the C sources in this directory (keeps the compile sandbox minimal and
  # avoids rebuilds when unrelated files in the tree change).
  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./cbm-ctl.c
      ./cbm-daemon.c
    ];
  };

  dontConfigure = true;

  buildPhase = ''
    runHook preBuild

    cc="''${CC:-cc}"
    cflags="-O2 -std=gnu11${lib.optionalString stdenv.hostPlatform.isDarwin " -D_DARWIN_C_SOURCE"} -Wall -Wextra"

    $cc $cflags \
      -DCBM_BIN_DEFAULT='"${lib.getExe codebase-memory-mcp}"' \
      -o cbm-daemon cbm-daemon.c

    $cc $cflags \
      -DCBM_BIN='"${lib.getExe codebase-memory-mcp}"' \
      -DGIT='"${lib.getExe git}"' \
      ${backendDefs} -o cbm-ctl cbm-ctl.c

    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 cbm-daemon $out/bin/cbm-daemon
    install -Dm755 cbm-ctl $out/bin/cbm-ctl
    runHook postInstall
  '';

  meta = {
    description = "Control CLI (cbm-ctl) and daemon wrapper (cbm-daemon) for codebase-memory-mcp";
    mainProgram = "cbm-ctl";
    platforms = lib.platforms.unix;
  };
}
