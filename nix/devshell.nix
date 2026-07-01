# Dev shell: just the package's own build inputs.
{
  mkShell,
  codebase-memory-mcp,
}:
mkShell {
  inputsFrom = [ codebase-memory-mcp ];
}
