import Link from "next/link"
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card"

export default function ForgePage() {
  return (
    <div className="max-w-3xl mx-auto flex flex-col min-h-svh px-4 py-8 gap-6">
      <header className="flex flex-col gap-1">
        <h1 className="text-3xl font-bold tracking-tight">forge</h1>
        <p className="text-muted-foreground">
          Cargo-like build orchestration for C, C++, and assembly — vendored
          in this monorepo at <code>forge/</code> (upstream stays source of
          truth, see <code>forge/SOURCE.md</code>).
        </p>
      </header>

      <Card>
        <CardHeader>
          <CardTitle>Build native Forge</CardTitle>
          <CardDescription>
            Per <code>forge/AGENTS.md</code>: mingw-w64 gcc on Windows, strict{" "}
            <code>-Wall -Wextra -Werror -std=c2x</code>.
          </CardDescription>
        </CardHeader>
        <CardContent className="flex flex-col gap-3">
          <pre className="text-xs rounded-lg bg-muted p-3 overflow-x-auto">
{`make -C forge CC=gcc
./forge/build/forge run --release --manifest forge/test/Forge.toml
# expect: Hello world! (rerun prints up-to-date:, no recompiles)`}
          </pre>
          <pre className="text-xs rounded-lg bg-muted p-3 overflow-x-auto">
{`bun run forge:build
bun run forge:test`}
          </pre>
        </CardContent>
      </Card>

      <Card>
        <CardHeader>
          <CardTitle>Registry attachment</CardTitle>
          <CardDescription>
            Native Forge stays a separate C CLI; Sunn serves its registry metadata.
          </CardDescription>
        </CardHeader>
        <CardContent className="text-sm flex flex-col gap-2">
          <p>
            <code>GET /api/forge/v1/resolve?name=hello-c&amp;version=0.1.0&amp;triplet=x64-windows</code>
          </p>
          <Link href="/packages" className="underline underline-offset-4 w-fit">
            Browse native packages →
          </Link>
        </CardContent>
      </Card>
    </div>
  )
}
