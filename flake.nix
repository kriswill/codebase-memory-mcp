{
  description = "codebase-memory-mcp — C11 MCP server for codebase indexing (kriswill nix fork)";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "aarch64-darwin"
        "x86_64-darwin"
        "aarch64-linux"
        "x86_64-linux"
      ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});

      # Ahead of the v0.8.1 tag on upstream main; suffixed to mark this as the nix fork
      # build (also injected as CBM_VERSION so `--version` doesn't report "dev").
      version = "0.8.1-nix";
    in
    {
      packages = forAllSystems (
        pkgs:
        let
          lib = pkgs.lib;

          # graph-ui — the Vite/React/Three.js 3D graph visualizer, built offline so
          # its dist/ can be embedded into the binary. `npm run build` needs network,
          # which the build sandbox lacks, so we build it in a separate fixed-output
          # buildNpmPackage and drop the prebuilt dist in place before `embed`.
          # npmDepsHash pins the vendored npm closure (graph-ui/package-lock.json) —
          # recompute with `prefetch-npm-deps graph-ui/package-lock.json` on a bump.
          graph-ui = pkgs.buildNpmPackage {
            pname = "codebase-memory-mcp-graph-ui";
            inherit version;
            src = ./graph-ui;
            npmDepsHash = "sha256-P1JVo+GFr+Gsq88dnn9OsedZqrTaj3DDGbej+nHbp4U=";
            installPhase = ''
              runHook preInstall
              cp -r dist $out
              runHook postInstall
            '';
          };

          codebase-memory-mcp = pkgs.stdenv.mkDerivation {
            pname = "codebase-memory-mcp";
            inherit version;
            src = ./.;

            nativeBuildInputs = [
              pkgs.gnumake
              pkgs.git
              pkgs.makeWrapper
            ];
            buildInputs = [ pkgs.zlib ] ++ lib.optionals pkgs.stdenv.isLinux [ pkgs.zlib.static ];

            # Neutralize the npm-driven `frontend` Makefile target — the sandbox has no
            # network, so we supply graph-ui's prebuilt dist instead (see buildPhase).
            # The `embed` step still runs on that dist (pure shell + cc).
            postPatch = ''
              substituteInPlace Makefile.cbm \
                --replace-fail 'cd graph-ui && npm ci && npm run build' 'true'
            '';

            # Build cbm-with-ui (links the graph visualizer, reachable via --ui=true).
            # Drop the prebuilt frontend into graph-ui/dist first; inject the real
            # version via CFLAGS_EXTRA (the escaped quotes survive make's recipe shell
            # so the compiler sees a string literal) instead of the "dev" default.
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

            # The flake-topology indexing passes (pass_flakelock / pass_nix_eval) shell
            # out to `nix` — and `git`, which `nix flake` needs for path inputs, plus
            # `coreutils` for the `timeout` that bounds pass_nix_eval. Guarantee all
            # three on the indexer's runtime PATH so that capability is first-class.
            postInstall = ''
              wrapProgram $out/bin/codebase-memory-mcp \
                --prefix PATH : ${
                  lib.makeBinPath [
                    pkgs.nix
                    pkgs.git
                    pkgs.coreutils
                  ]
                }
            '';

            meta = {
              description = "MCP server that builds and queries a semantic graph of your codebase";
              homepage = "https://github.com/DeusData/codebase-memory-mcp";
              license = lib.licenses.mit;
              mainProgram = "codebase-memory-mcp";
              platforms = systems;
            };
          };
        in
        {
          inherit codebase-memory-mcp;
          default = codebase-memory-mcp;
        }
        # cbm-tools (cbm-ctl / cbm-daemon launchd supervision) is macOS-only.
        // lib.optionalAttrs pkgs.stdenv.isDarwin {
          cbm-tools = pkgs.callPackage ./nix/cbm-tools.nix { inherit codebase-memory-mcp; };
        }
      );

      # nix-darwin module supervising the daemon under launchd (parameterized over
      # `self` so package/tools default to this flake's builds — no overlay needed).
      darwinModules.codebase-memory-mcp = import ./nix/darwin-module.nix self;
      darwinModules.default = self.darwinModules.codebase-memory-mcp;

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.system}.default ];
        };
      });
    };
}
