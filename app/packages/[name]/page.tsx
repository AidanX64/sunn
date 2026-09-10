import Link from "next/link"
import path from "path"
import { promises as fs } from "fs"
import { notFound } from "next/navigation"
import {
  nativePackageSchema,
  registryIndexSchema,
} from "@/lib/sunn-registry"
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card"

export default async function PackageDetailPage({
  params,
}: {
  params: Promise<{ name: string }>
}) {
  const { name } = await params
  try {
    const indexPath = path.join(
      process.cwd(),
      "public",
      "packages",
      "sunn.registry.json"
    )
    const index = registryIndexSchema.parse(
      JSON.parse(await fs.readFile(indexPath, "utf8"))
    )
    const entry = index.packages.find((p) => p.name === name)
    if (!entry) notFound()
    const pkgPath = path.join(process.cwd(), "public", entry.index.replace(/^\//, ""))
    const pkg = nativePackageSchema.parse(
      JSON.parse(await fs.readFile(pkgPath, "utf8"))
    )

    const forgeCmd = `forge add ${pkg.name} --git https://sunn.local/packages/${pkg.name}`
    const vcpkgCmd = pkg.vcpkg ? `vcpkg add port ${pkg.vcpkg.port}` : "# no vcpkg port mapped yet"
    const conanCmd = pkg.conan ? `conan install ${pkg.conan.ref}` : "# no conan ref mapped yet"

    return (
      <div className="max-w-3xl mx-auto flex flex-col min-h-svh px-4 py-8 gap-6">
        <Link href="/packages" className="text-sm underline underline-offset-4 w-fit">
          ← All packages
        </Link>
        <header className="flex flex-col gap-1">
          <h1 className="text-3xl font-bold tracking-tight">
            {pkg.name} {pkg.version}
          </h1>
          <p className="text-muted-foreground">{pkg.description}</p>
        </header>

        <Card>
          <CardHeader>
            <CardTitle>Install</CardTitle>
            <CardDescription>
              Static MVP — artifacts are placeholders until real tarballs land in{" "}
              <code>public/packages</code>.
            </CardDescription>
          </CardHeader>
          <CardContent className="flex flex-col gap-4">
            <div>
              <p className="text-sm font-medium mb-1">forge</p>
              <pre className="text-xs rounded-lg bg-muted p-3 overflow-x-auto">{forgeCmd}</pre>
            </div>
            <div>
              <p className="text-sm font-medium mb-1">vcpkg</p>
              <pre className="text-xs rounded-lg bg-muted p-3 overflow-x-auto">{vcpkgCmd}</pre>
            </div>
            <div>
              <p className="text-sm font-medium mb-1">conan</p>
              <pre className="text-xs rounded-lg bg-muted p-3 overflow-x-auto">{conanCmd}</pre>
            </div>
            <div>
              <p className="text-sm font-medium mb-1">CMake (FetchContent)</p>
              <pre className="text-xs rounded-lg bg-muted p-3 overflow-x-auto">
{`FetchContent_Declare(${pkg.name} URL https://sunn.local${pkg.artifacts[0]?.url ?? "/packages/…tar.gz"})`}
              </pre>
            </div>
          </CardContent>
        </Card>

        <Card>
          <CardHeader>
            <CardTitle>Metadata</CardTitle>
          </CardHeader>
          <CardContent className="text-sm flex flex-col gap-1">
            <p>lang: {pkg.lang}</p>
            <p>build: {pkg.build}</p>
            <p>license: {pkg.license || "—"}</p>
            <p>homepage: {pkg.homepage || "—"}</p>
            <p>triplets: {pkg.triplets.join(", ") || "—"}</p>
            <p>manifest: {pkg.forge?.manifest ?? "—"}</p>
          </CardContent>
        </Card>
      </div>
    )
  } catch {
    notFound()
  }
}
