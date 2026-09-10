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
    // Build output, generated registries, and vendored C: never lint these.
    // (Raw `eslint .` — unlike the old `next lint` — does not ignore .next.)
    ignores: [".next/**", "public/r/**", "public/packages/**", "forge/**"],
  },
  ...compat.extends("next/core-web-vitals", "next/typescript"),
];

export default eslintConfig;
