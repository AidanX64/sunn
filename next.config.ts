import type { NextConfig } from "next";

const nextConfig: NextConfig = {
  // Standalone server for the published @v1dxu/sunn package: `next build`
  // emits .next/standalone/server.js, which bin/sunn.js boots. The `files`
  // allowlist in package.json ships exactly that plus static + public/.
  output: "standalone",
  outputFileTracingIncludes: {
    registry: ["./registry/**/*"],
  },
  /* config options here */
};

export default nextConfig;
