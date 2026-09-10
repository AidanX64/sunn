import { NextResponse } from "next/server"
import path from "path"
import { promises as fs } from "fs"
import {
  nativePackageSchema,
  registryIndexSchema,
} from "@/lib/sunn-registry"

// GET /api/forge/v1/resolve?name=hello-c&version=0.1.0&triplet=x64-windows
// Minimal contract for the separate forge CLI: pin name+version to an artifact.
export async function GET(request: Request) {
  try {
    const { searchParams } = new URL(request.url)
    const name = searchParams.get("name")
    const version = searchParams.get("version")
    const triplet = searchParams.get("triplet")
    if (!name) {
      return NextResponse.json({ error: "Missing ?name=" }, { status: 400 })
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
    const pkgPath = path.join(process.cwd(), "public", entry.index.replace(/^\//, ""))
    const pkg = nativePackageSchema.parse(JSON.parse(await fs.readFile(pkgPath, "utf8")))

    const artifact = triplet
      ? pkg.artifacts.find((a) => a.triplet === triplet) ?? null
      : null

    return NextResponse.json({
      name: pkg.name,
      version: pkg.version,
      triplet: triplet ?? null,
      artifact,
      manifest: pkg.forge?.manifest ?? null,
    })
  } catch (error) {
    console.error("Error resolving forge package:", error)
    return NextResponse.json({ error: "Something went wrong" }, { status: 500 })
  }
}
