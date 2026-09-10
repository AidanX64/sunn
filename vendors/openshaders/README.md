# vendors/openshaders (reserved — awaiting open-source)

Decision (2026-09-10): do NOT vendor anything yet. openshaders.com is
closed-source, so there is nothing to copy.

Integration seam (already in place, do not break it):

- Engine: `lib/custom-shaders.js` (`createShader(canvas, options)`)
- Wrapper: `components/sunn-shader-background.tsx` (theme sync via
  `next-themes`, WebGPU-missing CSS fallback, `destroy()` on unmount)

When openshaders opens:

1. Vendor it here following the `forge/SOURCE.md` pattern
   (upstream URL + commit SHA + date + license in a `SOURCE.md`).
2. Keep the wrapper's props (`darkBackground`, `lightBackground`,
   theme handling) stable — the gallery becomes a data source
   behind the same component, not a rewrite of every call site.
3. Only then consider promoting the wrapper to
   `registry/new-york/sunn-shader-background/` as a distributable.
