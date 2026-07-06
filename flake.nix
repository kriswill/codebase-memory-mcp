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
          graph-ui = pkgs.callPackage ./nix/graph-ui.nix {
            inherit version;
            src = ./graph-ui;
          };
          codebase-memory-mcp = pkgs.callPackage ./nix/package.nix {
            inherit version graph-ui;
            src = ./.;
            platforms = systems;
          };
        in
        {
          inherit codebase-memory-mcp;
          default = codebase-memory-mcp;
          # cbm-tools: cbm-ctl / cbm-daemon supervision helpers (launchd user
          # agent on macOS, systemd user service on Linux).
          cbm-tools = pkgs.callPackage ./nix/tools/package.nix { inherit codebase-memory-mcp; };
        }
      );

      # nix-darwin / NixOS modules supervising the daemon (launchd user agent /
      # systemd user service; parameterized over `self` so package/tools default
      # to this flake's builds — no overlay needed).
      darwinModules.codebase-memory-mcp = import ./nix/darwin/module.nix self;
      darwinModules.default = self.darwinModules.codebase-memory-mcp;
      nixosModules.codebase-memory-mcp = import ./nix/nixos/module.nix self;
      nixosModules.default = self.nixosModules.codebase-memory-mcp;

      devShells = forAllSystems (pkgs: {
        default = pkgs.callPackage ./nix/devshell.nix {
          codebase-memory-mcp = self.packages.${pkgs.system}.default;
        };
      });
    };
}
