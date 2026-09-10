# vendors/shadcn-ui (reference — consumed via npm/CLI, not vendored)

Decision (2026-09-10): do NOT copy the shadcn/ui repo here. The live
registry stays in `registry/` + `registry.json`, built/consumed with
the `shadcn` CLI (`shadcn` is already a dependency in the root
`package.json`; pin its version there on upgrade).

If a patch to upstream is ever required, vendor the minimum needed
here following the `forge/SOURCE.md` pattern
(upstream URL + commit SHA + date + license), and record why the
CLI dependency was insufficient.
