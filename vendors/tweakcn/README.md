# vendors/tweakcn (reference — consumed via npm, not vendored)

Decision (2026-09-10): do NOT copy the tweakcn repo here. It moves fast
and a snapshot would rot on day one. Consume it as a package dependency
and pin the version in the root `package.json` when first used.

If a patch to upstream is ever required, vendor the minimum needed
here following the `forge/SOURCE.md` pattern
(upstream URL + commit SHA + date + license), and record why the
npm dependency was insufficient.

JS vendors become `workspaces` entries in the root `package.json` only
when they contain their own `package.json`. Use Bun workspaces if a
second JavaScript package is added.
