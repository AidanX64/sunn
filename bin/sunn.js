#!/usr/bin/env node
"use strict";

/* sunn package launcher (no dependencies, plain Node >= 20).
 *
 *   sunn serve [--port N]   boot the self-hosted registry (standalone build)
 *   sunn init <dir>         scaffold a working copy from this package
 *   sunn help               this text
 *
 * Installed via `npm i -g @v1dxu/sunn` (or `bunx @v1dxu/sunn serve`).
 */

const { spawn } = require("child_process");
const fs = require("fs");
const path = require("path");

const ROOT = path.resolve(__dirname, "..");
const STANDALONE = path.join(ROOT, ".next", "standalone");
const SERVER = path.join(STANDALONE, "server.js");

function fail(message) {
  console.error(`sunn: ${message}`);
  process.exit(1);
}

function usage() {
  console.log(`sunn — self-hostable component + native package registry

Usage:
  sunn serve [--port N]   start the registry (default port 3000)
  sunn init <dir>         scaffold a working copy from the GitHub release
  sunn help               show this text

Examples:
  npx @v1dxu/sunn serve --port 3100
  npx @v1dxu/sunn init ./my-registry`);
}

/* The standalone server resolves ./public and ./.next/static relative to
 * its working directory, so make sure both are inside the standalone dir
 * (copied from the package payload when absent) before booting. */
function ensureStandaloneAssets() {
  if (!fs.existsSync(SERVER)) {
    fail(
      "standalone server not found — this copy was not built with `next build` " +
        "(output: \"standalone\"). Reinstall @v1dxu/sunn from the registry."
    );
  }
  const pairs = [
    [path.join(ROOT, "public"), path.join(STANDALONE, "public")],
    [
      path.join(ROOT, ".next", "static"),
      path.join(STANDALONE, ".next", "static"),
    ],
  ];
  for (const [from, to] of pairs) {
    if (!fs.existsSync(to) && fs.existsSync(from)) {
      fs.cpSync(from, to, { recursive: true });
    }
  }
}

function portFromArgs(args) {
  const flag = args.indexOf("--port");
  if (flag !== -1) {
    const value = Number(args[flag + 1]);
    if (!Number.isInteger(value) || value <= 0 || value > 65535) {
      fail(`invalid --port (expected 1-65535, got "${args[flag + 1] ?? ""}")`);
    }
    return value;
  }
  const env = Number(process.env.PORT);
  return Number.isInteger(env) && env > 0 && env <= 65535 ? env : 3000;
}

function serve(args) {
  ensureStandaloneAssets();
  const port = portFromArgs(args);
  const child = spawn(process.execPath, [SERVER], {
    cwd: STANDALONE,
    env: { ...process.env, PORT: String(port) },
    stdio: "inherit",
  });
  child.on("error", (error) => fail(`could not start server: ${error.message}`));
  child.on("exit", (code) => process.exit(code ?? 1));
}

async function init(args) {
  const dir = args[0];
  if (!dir || args.includes("--help") || args.includes("-h")) {
    fail("usage: sunn init <dir>");
  }
  const dest = path.resolve(dir);
  if (fs.existsSync(dest) && fs.readdirSync(dest).length !== 0) {
    fail(`refusing to scaffold into non-empty directory: ${dest}`);
  }
  const sourceRoot = path.resolve(__dirname, "..");
  const sourcePaths = [
    "app",
    "components",
    "lib",
    "registry",
    "registry-fixtures",
    "forge",
    "vendors",
    "scripts",
    "public",
    "bin",
    "registry.json",
    "components.json",
    "next.config.ts",
    "postcss.config.mjs",
    "tailwind.config.ts",
    "tsconfig.json",
    "eslint.config.mjs",
    ".bun-version",
    ".nvmrc",
    "bun.lock",
    "package.json",
    "README.md",
    "LICENSE",
  ];

  fs.mkdirSync(dest, { recursive: true });
  for (const relativePath of sourcePaths) {
    const source = path.join(sourceRoot, relativePath);
    const target = path.join(dest, relativePath);
    fs.cpSync(source, target, { recursive: true });
  }
  console.log(`sunn: scaffolded ${dest}`);
  try {
    execFileSync("bun", ["install"], { cwd: dest, stdio: "inherit" });
  } catch {
    console.log("sunn: `bun install` not available — run it by hand, then `bun run dev`");
    return;
  }
  console.log("sunn: dependencies installed — start with `bun run dev`");
}

async function main() {
  const [command, ...rest] = process.argv.slice(2);
  if (!command || command === "help" || command === "--help" || command === "-h") {
    usage();
    return;
  }
  if (command === "serve") {
    serve(rest);
    return;
  }
  if (command === "init") {
    await init(rest);
    return;
  }
  if (command === "--version" || command === "-v") {
    console.log(require("../package.json").version);
    return;
  }
  fail(`unknown command "${command}" — see \`sunn help\``);
}

main().catch((error) => fail(error instanceof Error ? error.message : String(error)));
