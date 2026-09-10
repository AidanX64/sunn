import { NextResponse } from "next/server"
import path from "path"
import { promises as fs } from "fs"
import {
  nativePackageSchema,
  packageNameSchema,
  registryIndexSchema,
  resolvePublicFile,
} from "@/lib/sunn-registry"

// GET /api/packages/[name]
export async function GET(
  _request: Request,
  { params }: { params: Promise<{ name: string }> }
) {
  try {
    const { name } = await params
    if (!packageNameSchema.safeParse(name).success) {
      return NextResponse.json({ error: "Invalid package name" }, { status: 400 })
    }
    const indexPath = path.join(process.cwd(), "public", "packages", "sunn.registry.json")
    const indexRaw = await fs.readFile(indexPath, "utf8")
    const index = registryIndexSchema.parse(JSON.parse(indexRaw))
    const entry = index.packages.find((p) => p.name === name)
    if (!entry) {
      return NextResponse.json({ error: "Package not found" }, { status: 404 })
    }
    const filePath = resolvePublicFile(entry.index)
    if (!filePath) {
      console.error(`Corrupt registry index pointer for package ${entry.name}`)
      return NextResponse.json({ error: "Something went wrong" }, { status: 500 })
    }
    const raw = await fs.readFile(filePath, "utf8")
    const pkg = nativePackageSchema.parse(JSON.parse(raw))
    if (pkg.name !== entry.name || pkg.version !== entry.latest) {
      console.error(`Registry drift: index points ${entry.name}@${entry.latest} at ${entry.index}`)
      return NextResponse.json({ error: "Something went wrong" }, { status: 500 })
    }
    return NextResponse.json(pkg)
  } catch (error) {
    console.error("Error fetching native package:", error)
    return NextResponse.json({ error: "Something went wrong" }, { status: 500 })
  }
}
