# graph-ui — the Vite/React/Three.js 3D graph visualizer, built offline so its
# dist/ can be embedded into the binary. `npm run build` needs network, which the
# build sandbox lacks, so we build it in a separate fixed-output buildNpmPackage and
# drop the prebuilt dist in place before `embed` (see package.nix's buildPhase).
# npmDepsHash pins the vendored npm closure (graph-ui/package-lock.json) — recompute
# with `prefetch-npm-deps graph-ui/package-lock.json` on a bump.
{
  buildNpmPackage,
  version,
  src,
}:
buildNpmPackage {
  pname = "codebase-memory-mcp-graph-ui";
  inherit version src;
  npmDepsHash = "sha256-P1JVo+GFr+Gsq88dnn9OsedZqrTaj3DDGbej+nHbp4U=";
  installPhase = ''
    runHook preInstall
    cp -r dist $out
    runHook postInstall
  '';
}
