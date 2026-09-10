import { dirname } from "path";
import { fileURLToPath } from "url";
import { FlatCompat } from "@eslint/eslintrc";

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

const compat = new FlatCompat({
  baseDirectory: __dirname,
});

const eslintConfig = [
  {
    // Build output, generated registries, vendored C, and the shipped
    // launcher never get linted. (Raw `eslint .` — unlike the old
    // `next lint` — does not ignore .next. bin/ is plain-Node CJS,
    // validated by `node --check`, outside the TS ruleset.)
    ignores: [".next/**", "public/r/**", "public/packages/**", "forge/**", "bin/**"],
  },
  ...compat.extends("next/core-web-vitals", "next/typescript"),
];

export default eslintConfig;
