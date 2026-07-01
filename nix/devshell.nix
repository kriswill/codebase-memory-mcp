# Dev shell: the package's own build inputs plus the tooling the pre-commit hook
# (scripts/hooks/pre-commit → `make -f Makefile.cbm lint`) shells out to.
#   - llvmPackages_20.clang-tools: clang-tidy (lint-tidy) + clang-format (lint-format).
#     Pinned to LLVM 20 to match CI (.github/workflows/_lint.yml installs clang-format-20),
#     so local formatting matches the CI gate exactly rather than drifting with the
#     newest clang-format.
#   - cppcheck: static analysis for lint-cppcheck (CI pins 2.20.0; nixpkgs tracks 2.21.x).
{
  mkShell,
  llvmPackages_20,
  cppcheck,
  codebase-memory-mcp,
}:
mkShell {
  inputsFrom = [ codebase-memory-mcp ];
  packages = [
    llvmPackages_20.clang-tools
    cppcheck
  ];
}
