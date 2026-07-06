# codebase-memory-mcp — the C11 MCP server, built as cbm-with-ui so the graph
# visualizer is linked in and reachable via --ui=true. graph-ui is built separately
# (see graph-ui.nix) and its prebuilt dist embedded during buildPhase, since the
# build sandbox has no network for its npm step.
{
  lib,
  stdenv,
  gnumake,
  git,
  makeWrapper,
  zlib,
  nix,
  coreutils,
  version,
  src,
  graph-ui,
  platforms,
}:
stdenv.mkDerivation {
  pname = "codebase-memory-mcp";
  inherit version src;

  nativeBuildInputs = [
    gnumake
    git
    makeWrapper
  ];
  buildInputs = [ zlib ] ++ lib.optionals stdenv.isLinux [ zlib.static ];

  # Neutralize the npm-driven `frontend` Makefile target — the sandbox has no
  # network, so we supply graph-ui's prebuilt dist instead (see buildPhase).
  # The `embed` step still runs on that dist (pure shell + cc). patchShebangs:
  # the Linux sandbox has no /usr/bin/env for the scripts' shebangs (the darwin
  # sandbox exposes /usr/bin, which is why this was never needed there).
  postPatch = ''
    substituteInPlace Makefile.cbm \
      --replace-fail 'cd graph-ui && npm ci && npm run build' 'true'
    patchShebangs scripts/
  '';

  # Drop the prebuilt frontend into graph-ui/dist first; inject the real version
  # via CFLAGS_EXTRA (the escaped quotes survive make's recipe shell so the compiler
  # sees a string literal) instead of the "dev" default.
  buildPhase = ''
    runHook preBuild
    mkdir -p graph-ui/dist
    cp -r ${graph-ui}/. graph-ui/dist/
    make -j$NIX_BUILD_CORES -f Makefile.cbm cbm-with-ui \
      CFLAGS_EXTRA='-DCBM_VERSION=\"${version}\"'
    runHook postBuild
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 build/c/codebase-memory-mcp $out/bin/codebase-memory-mcp
    runHook postInstall
  '';

  # The flake-topology indexing passes (pass_flakelock / pass_nix_eval) shell out
  # to `nix` — and `git`, which `nix flake` needs for path inputs, plus `coreutils`
  # for the `timeout` that bounds pass_nix_eval. Guarantee all three on the indexer's
  # runtime PATH so that capability is first-class.
  postInstall = ''
    wrapProgram $out/bin/codebase-memory-mcp \
      --prefix PATH : ${
        lib.makeBinPath [
          nix
          git
          coreutils
        ]
      }
  '';

  meta = {
    description = "MCP server that builds and queries a semantic graph of your codebase";
    homepage = "https://github.com/DeusData/codebase-memory-mcp";
    license = lib.licenses.mit;
    mainProgram = "codebase-memory-mcp";
    inherit platforms;
  };
}
