import Link from "next/link"
import path from "path"
import { promises as fs } from "fs"
import { notFound } from "next/navigation"
import {
  nativePackageSchema,
  packageNameSchema,
  registryIndexSchema,
  resolvePublicFile,
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
  if (!packageNameSchema.safeParse(name).success) notFound()
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
    const pkgPath = resolvePublicFile(entry.index)
    if (!pkgPath) throw new Error(`bad index pointer for ${entry.name}`)
    const pkg = nativePackageSchema.parse(
      JSON.parse(await fs.readFile(pkgPath, "utf8"))
    )
    if (pkg.name !== entry.name || pkg.version !== entry.latest) {
      throw new Error(`registry drift for ${entry.name}`)
    }

    const forgeCmd = `forge add ${pkg.name} --registry ${pkg.name} --version ${pkg.version}`
    const sourceDescription =
      pkg.source.kind === "git"
        ? `${pkg.source.location} @ ${pkg.source.ref}`
        : `${pkg.source.location} (sha256 ${pkg.source.sha256})`

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
              Forge resolves this recipe, fetches the upstream source, applies
              any patches, and builds it locally.
            </CardDescription>
          </CardHeader>
          <CardContent className="flex flex-col gap-4">
            <div>
              <p className="text-sm font-medium mb-1">forge</p>
              <pre className="text-xs rounded-lg bg-muted p-3 overflow-x-auto">{forgeCmd}</pre>
            </div>
            <div>
              <p className="text-sm font-medium mb-1">recipe source</p>
              <pre className="text-xs rounded-lg bg-muted p-3 overflow-x-auto">{sourceDescription}</pre>
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
            <p>patches: {pkg.patches.join(", ") || "none"}</p>
            <p>manifest: {pkg.forge?.manifest ?? "—"}</p>
          </CardContent>
        </Card>
      </div>
    )
  } catch {
    notFound()
  }
}
