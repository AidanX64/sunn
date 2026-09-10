import { NextResponse } from "next/server"
import path from "path"
import { promises as fs } from "fs"
import {
  nativePackageSchema,
  registryIndexSchema,
} from "@/lib/sunn-registry"

async function loadIndex() {
  const filePath = path.join(process.cwd(), "public", "packages", "sunn.registry.json")
  const raw = await fs.readFile(filePath, "utf8")
  return registryIndexSchema.parse(JSON.parse(raw))
}

async function loadPackage(indexPath: string) {
  // indexPath looks like /packages/hello-c/0.1.0.json
  const rel = indexPath.replace(/^\//, "")
  const filePath = path.join(process.cwd(), "public", rel)
  const raw = await fs.readFile(filePath, "utf8")
  return nativePackageSchema.parse(JSON.parse(raw))
}

// GET /api/packages?q=&lang=c&triplet=x64-windows
export async function GET(request: Request) {
  try {
    const { searchParams } = new URL(request.url)
    const q = (searchParams.get("q") ?? "").toLowerCase()
    const lang = searchParams.get("lang")
    const triplet = searchParams.get("triplet")

    const index = await loadIndex()
    const pkgs = await Promise.all(index.packages.map((p) => loadPackage(p.index)))

    const filtered = pkgs.filter((p) => {
      if (q && !`${p.name} ${p.description}`.toLowerCase().includes(q)) return false
      if (lang && p.lang !== lang) return false
      if (triplet && !p.triplets.includes(triplet)) return false
      return true
    })

    return NextResponse.json({ packages: filtered })
  } catch (error) {
    console.error("Error listing native packages:", error)
    return NextResponse.json({ error: "Something went wrong" }, { status: 500 })
  }
}
