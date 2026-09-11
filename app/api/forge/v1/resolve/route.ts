import { NextResponse } from "next/server"
import path from "path"
import { promises as fs } from "fs"
import {
  nativePackageSchema,
  packageNameSchema,
  packageVersionSchema,
  registryIndexSchema,
  resolvePublicFile,
} from "@/lib/sunn-registry"

// GET /api/forge/v1/resolve?name=hello-c&version=0.1.0
// Resolve a native Forge recipe, not a Sunn-hosted artifact.
export async function GET(request: Request) {
  try {
    const { searchParams } = new URL(request.url)
    const name = searchParams.get("name")
    const version = searchParams.get("version")
    if (!name || !packageNameSchema.safeParse(name).success) {
      return NextResponse.json({ error: "Missing or invalid ?name=" }, { status: 400 })
    }
    if (version !== null && !packageVersionSchema.safeParse(version).success) {
      return NextResponse.json({ error: "Invalid ?version=" }, { status: 400 })
    }

    const indexPath = path.join(process.cwd(), "public", "packages", "sunn.registry.json")
    const index = registryIndexSchema.parse(
      JSON.parse(await fs.readFile(indexPath, "utf8"))
    )
    const entry = index.packages.find((p) => p.name === name)
    if (!entry) {
      return NextResponse.json({ error: "Package not found" }, { status: 404 })
    }
    if (version && entry.latest !== version) {
      return NextResponse.json(
        { error: `Only version ${entry.latest} hosted in static MVP` },
        { status: 404 }
      )
    }
    const pkgPath = resolvePublicFile(entry.index)
    if (!pkgPath) {
      console.error(`Corrupt registry index pointer for package ${entry.name}`)
      return NextResponse.json({ error: "Something went wrong" }, { status: 500 })
    }
    const pkg = nativePackageSchema.parse(JSON.parse(await fs.readFile(pkgPath, "utf8")))
    if (pkg.name !== entry.name || pkg.version !== entry.latest) {
      console.error(`Registry drift: index points ${entry.name}@${entry.latest} at ${entry.index}`)
      return NextResponse.json({ error: "Something went wrong" }, { status: 500 })
    }

    return NextResponse.json({
      version: pkg.version,
      source: pkg.source,
      patches: pkg.patches,
    })
  } catch (error) {
    console.error("Error resolving forge package:", error)
    return NextResponse.json({ error: "Something went wrong" }, { status: 500 })
  }
}
