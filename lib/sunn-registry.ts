import { z } from "zod"

export const tripletSchema = z.string().min(1)

export const artifactSchema = z.object({
  triplet: tripletSchema,
  url: z.string().min(1),
  sha256: z.string().min(1),
})

export const nativePackageSchema = z.object({
  name: z.string().min(1),
  version: z.string().min(1),
  description: z.string().default(""),
  license: z.string().default(""),
  homepage: z.string().default(""),
  lang: z.enum(["c", "c++", "asm"]),
  build: z.string().default("forge"),
  triplets: z.array(tripletSchema).default([]),
  dependencies: z.array(z.string()).default([]),
  artifacts: z.array(artifactSchema).default([]),
  vcpkg: z.object({ port: z.string(), version: z.string() }).optional(),
  conan: z.object({ ref: z.string(), recipe: z.string() }).optional(),
  forge: z.object({ manifest: z.string() }).optional(),
})

export type NativePackage = z.infer<typeof nativePackageSchema>

export const registryIndexSchema = z.object({
  name: z.string(),
  description: z.string().default(""),
  packages: z.array(
    z.object({
      name: z.string(),
      latest: z.string(),
      description: z.string().default(""),
      license: z.string().default(""),
      homepage: z.string().default(""),
      index: z.string(),
    })
  ),
})

export type RegistryIndex = z.infer<typeof registryIndexSchema>
