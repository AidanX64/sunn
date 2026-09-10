import path from "path"
import { z } from "zod"

// Tight allowlists: registry JSON is a trust boundary (file paths, shell
// snippets, fetch URLs all derive from it). Keep these strict so a tampered
// index/package file fails closed at parse time instead of flowing into
// fs reads, fetch(), or copy-pasted shell commands.
export const packageNameSchema = z
  .string()
  .min(1)
  .max(64)
  .regex(/^[A-Za-z0-9][A-Za-z0-9_-]*$/, "invalid package name")

export const packageVersionSchema = z
  .string()
  .min(1)
  .max(32)
  .regex(
    /^[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?$/,
    "version must be MAJOR.MINOR.PATCH"
  )

export const tripletSchema = z
  .string()
  .min(1)
  .max(32)
  .regex(/^(x64|arm64)-(windows|linux|macos)$/, "unknown triplet")

const relativeArtifactUrlSchema = z
  .string()
  .min(1)
  .max(2048)
  .regex(
    /^\/packages\/[A-Za-z0-9_-]+\/[A-Za-z0-9._-]+\.tar\.gz$/,
    "artifact url must be /packages/<name>/<file>.tar.gz or https://…"
  )

const absoluteArtifactUrlSchema = z.string().url().max(2048)

const sha256Schema = z
  .string()
  .regex(/^[0-9a-f]{64}$/i, "sha256 must be 64 hex chars")

const homepageSchema = z
  .string()
  .max(2048)
  .refine((v) => v === "" || /^https?:\/\/\S+$/.test(v), {
    message: "homepage must be empty or an http(s) URL",
  })

const manifestPathSchema = z
  .string()
  .regex(/^\/packages\/[A-Za-z0-9_-]+\/Forge\.toml$/, "bad manifest path")

const indexPathSchema = z
  .string()
  .regex(
    /^\/packages\/[A-Za-z0-9_-]+\/[0-9]+\.[0-9]+\.[0-9]+(?:[-+][0-9A-Za-z.-]+)?\.json$/,
    "bad index pointer"
  )

export const artifactSchema = z
  .object({
    triplet: tripletSchema,
    url: z.union([relativeArtifactUrlSchema, absoluteArtifactUrlSchema]),
    sha256: sha256Schema,
  })
  .strict()

export const nativePackageSchema = z
  .object({
    name: packageNameSchema,
    version: packageVersionSchema,
    description: z.string().max(1024).default(""),
    license: z.string().max(64).default(""),
    homepage: homepageSchema.default(""),
    lang: z.enum(["c", "c++", "asm"]),
    build: z.literal("forge").default("forge"),
    triplets: z.array(tripletSchema).max(16).default([]),
    dependencies: z.array(packageNameSchema).max(100).default([]),
    artifacts: z.array(artifactSchema).max(16).default([]),
    vcpkg: z.object({ port: z.string().max(128), version: z.string().max(32) }).strict().optional(),
    conan: z.object({ ref: z.string().max(256), recipe: z.string().max(256) }).strict().optional(),
    forge: z.object({ manifest: manifestPathSchema }).strict().optional(),
  })
  .strict()
  .superRefine((pkg, ctx) => {
    const seen = new Set<string>()
    for (const t of pkg.triplets) {
      if (seen.has(t)) {
        ctx.addIssue({ code: z.ZodIssueCode.custom, message: `duplicate triplet ${t}` })
      }
      seen.add(t)
    }
    for (const a of pkg.artifacts) {
      if (!seen.has(a.triplet)) {
        ctx.addIssue({
          code: z.ZodIssueCode.custom,
          message: `artifact triplet ${a.triplet} not listed in triplets`,
        })
      }
    }
  })

export type NativePackage = z.infer<typeof nativePackageSchema>

const indexEntrySchema = z
  .object({
    name: packageNameSchema,
    latest: packageVersionSchema,
    description: z.string().max(1024).default(""),
    license: z.string().max(64).default(""),
    homepage: homepageSchema.default(""),
    index: indexPathSchema,
  })
  .strict()

export const registryIndexSchema = z
  .object({
    $schema: z.string().max(2048).optional(),
    name: z.string().min(1).max(64),
    description: z.string().max(1024).default(""),
    packages: z.array(indexEntrySchema).max(1000),
  })
  .strict()

export type RegistryIndex = z.infer<typeof registryIndexSchema>

/**
 * Resolve a registry `index` pointer (e.g. `/packages/hello-c/0.1.0.json`)
 * to an absolute file under `<cwd>/public`. Returns null when the pointer
 * is malformed or escapes the public dir — callers must fail closed.
 */
export function resolvePublicFile(
  indexPointer: string,
  publicRoot: string = path.resolve(process.cwd(), "public")
): string | null {
  if (!indexPointer.startsWith("/packages/")) return null
  if (indexPointer.includes("\0")) return null
  const root = path.resolve(publicRoot)
  const target = path.resolve(root, `.${indexPointer}`)
  if (target !== root && !target.startsWith(root + path.sep)) return null
  if (!target.toLowerCase().endsWith(".json") && !target.endsWith("Forge.toml")) {
    // Artifacts (.tar.gz) never resolve through this helper; manifests and
    // per-version JSON do.
    return null
  }
  return target
}
