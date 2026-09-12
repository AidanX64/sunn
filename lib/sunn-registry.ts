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

const relativeSourceUrlSchema = z
  .string()
  .min(1)
  .max(2048)
  .regex(
    /^\/packages\/[A-Za-z0-9_-]+\/[A-Za-z0-9._-]+\.tar\.gz$/,
    "source url must be /packages/<name>/<file>.tar.gz or https://…"
  )

const sourceUrlSchema = z.union([relativeSourceUrlSchema, z.string().url().max(2048)])

const sha256Schema = z
  .string()
  .regex(/^[0-9a-f]{64}$/i, "sha256 must be 64 hex chars")

const revisionSchema = z.number().int().min(0).max(1000000).default(0)

const featureNameSchema = z
  .string()
  .min(1)
  .max(32)
  .regex(/^[A-Za-z0-9_-]+$/, "invalid feature name")

// Version-range requirement grammar (mirrors forge's C parser in
// src/manifest.c): comparators =, >=, >, <=, <, ^, ~, wildcards x/X/*,
// comma for AND, || for OR. Tight charset so a tampered recipe fails
// closed at parse time; semantic validity is checked client-side.
export const versionRangeSchema = z
  .string()
  .min(1)
  .max(512)
  .regex(
    /^[A-Za-z0-9.+*^~<>=|, \t-]+$/,
    "version-range uses comparators =, >=, >, <=, <, ^, ~, wildcards, ',' and '||'"
  )

// Optional transitive dependencies switched on by a feature.
// Registry-only (like the top-level `dependencies` names, which stay
// bare for backwards compatibility); at most one of
// version/min-version/version-range, neither meaning the baseline;
// max-version caps the range but never combines with an exact version.
const featureDepSchema = z
  .object({
    registry: packageNameSchema,
    version: packageVersionSchema.optional(),
    "min-version": packageVersionSchema.optional(),
    "version-range": versionRangeSchema.optional(),
    "max-version": packageVersionSchema.optional(),
  })
  .strict()
  .refine(
    (d) =>
      [d.version, d["min-version"], d["version-range"]].filter(
        (v) => v !== undefined
      ).length <= 1,
    {
      message:
        "feature dependency takes at most one of version/min-version/version-range",
    }
  )
  .refine((d) => d.version === undefined || d["max-version"] === undefined, {
    message: "feature dependency max-version cannot combine with version",
  })

const featureSchema = z
  .object({
    name: featureNameSchema,
    description: z.string().max(512).default(""),
    cflags: z.array(z.string().min(1).max(512)).max(8).default([]),
    dependencies: z.array(featureDepSchema).max(4).default([]),
  })
  .strict()

// Feature definitions as an array (not a map) so the C client's
// key-seeking JSON reader can walk them with its existing span pattern.
// Unique names enforced below; defaults must name defined features.
const featuresSchema = z.array(featureSchema).max(8).default([])

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

const patchNameSchema = z
  .string()
  .min(1)
  .max(128)
  .regex(/^[A-Za-z0-9][A-Za-z0-9._-]*\.patch$/, "invalid patch name")

const gitSourceSchema = z.object({
  kind: z.literal("git"),
  location: z.string().url().max(2048),
  ref: z.string().min(1).max(256),
}).strict()

const urlSourceSchema = z.object({
  kind: z.literal("url"),
  location: sourceUrlSchema,
  sha256: sha256Schema,
}).strict()

export const registrySourceSchema = z.discriminatedUnion("kind", [
  gitSourceSchema,
  urlSourceSchema,
])

export const nativePackageSchema = z
  .object({
    name: packageNameSchema,
    version: packageVersionSchema,
    revision: revisionSchema,
    description: z.string().max(1024).default(""),
    license: z.string().max(64).default(""),
    homepage: homepageSchema.default(""),
    lang: z.enum(["c", "c++", "asm"]),
    build: z.literal("forge").default("forge"),
    dependencies: z.array(packageNameSchema).max(100).default([]),
    source: registrySourceSchema,
    patches: z.array(patchNameSchema).max(32).default([]),
    features: featuresSchema,
    "default-features": z.array(featureNameSchema).max(8).default([]),
    forge: z.object({ manifest: manifestPathSchema }).strict().optional(),
  })
  .strict()
  .superRefine((pkg, ctx) => {
    const defined = new Set(pkg.features.map((f) => f.name))
    if (defined.size !== pkg.features.length) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        message: "feature names must be unique",
      })
    }
    for (const name of pkg["default-features"]) {
      if (!defined.has(name)) {
        ctx.addIssue({
          code: z.ZodIssueCode.custom,
          message: `default feature '${name}' is not defined in features`,
        })
      }
    }
  })

export type NativePackage = z.infer<typeof nativePackageSchema>

const indexEntrySchema = z
  .object({
    name: packageNameSchema,
    latest: packageVersionSchema,
    latest_revision: revisionSchema,
    description: z.string().max(1024).default(""),
    license: z.string().max(64).default(""),
    homepage: homepageSchema.default(""),
    index: indexPathSchema,
    versions: z
      .array(
        z
          .object({
            version: packageVersionSchema,
            revision: revisionSchema,
          })
          .strict()
      )
      .max(1000)
      .default([]),
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

const baselineEntrySchema = z
  .object({
    name: packageNameSchema,
    version: packageVersionSchema,
    revision: revisionSchema,
  })
  .strict()

// Registry-wide minimum floor: consumers without an exact pin resolve to
// at least these versions (vcpkg baseline semantics). Served statically at
// /baseline.json; the forge client validates it with this schema.
export const registryBaselineSchema = z
  .object({
    name: z.string().min(1).max(64),
    baseline: z.array(baselineEntrySchema).max(1000),
  })
  .strict()

export type RegistryBaseline = z.infer<typeof registryBaselineSchema>

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
