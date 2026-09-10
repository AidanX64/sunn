import * as React from "react"
import Link from "next/link"
import { OpenInV0Button } from "@/components/open-in-v0-button"
import { ModeToggle } from "@/components/mode-toggle"
import { SunnShaderBackground } from "@/components/sunn-shader-background"
import { HelloWorld } from "@/registry/new-york/hello-world/hello-world"
import { ExampleForm } from "@/registry/new-york/example-form/example-form"
import PokemonPage from "@/registry/new-york/complex-component/page"

// This page displays items from the custom registry.
// You are free to implement this with your own design as needed.

export default function Home() {
  return (
    <div className="max-w-3xl mx-auto flex flex-col min-h-svh px-4 py-8 gap-8">
      <div className="relative overflow-hidden border rounded-lg min-h-[320px]">
        <div className="absolute inset-0">
          <SunnShaderBackground />
        </div>
        <div className="relative flex flex-col gap-3 p-6 min-h-[320px] justify-end bg-gradient-to-t from-background/90 via-background/20 to-transparent">
          <div className="flex items-center justify-between">
            <p className="text-xs uppercase tracking-widest text-muted-foreground">
              sunn monorepo
            </p>
            <ModeToggle />
          </div>
          <h1 className="text-3xl font-bold tracking-tight">sunn</h1>
          <p className="text-muted-foreground max-w-xl">
            Half shadcn registry, half vcpkg/conan-style registry for
            C/C++/ASM — powered by forge. Web UI above, native packages
            below.
          </p>
          <div className="flex gap-2 text-sm">
            <Link href="/packages" className="underline underline-offset-4">
              Browse native packages
            </Link>
            <span className="text-muted-foreground">·</span>
            <Link href="/forge" className="underline underline-offset-4">
              forge
            </Link>
          </div>
        </div>
      </div>
      <header className="flex flex-col gap-1">
        <h2 className="text-3xl font-bold tracking-tight">Custom Registry</h2>
        <p className="text-muted-foreground">
          A custom registry for distributing code using shadcn.
        </p>
      </header>
      <main className="flex flex-col flex-1 gap-8">
        <div className="flex flex-col gap-4 border rounded-lg p-4 min-h-[450px] relative">
          <div className="flex items-center justify-between">
            <h2 className="text-sm text-muted-foreground sm:pl-3">
              A simple hello world component
            </h2>
            <OpenInV0Button name="hello-world" className="w-fit" />
          </div>
          <div className="flex items-center justify-center min-h-[400px] relative">
            <HelloWorld />
          </div>
        </div>

        <div className="flex flex-col gap-4 border rounded-lg p-4 min-h-[450px] relative">
          <div className="flex items-center justify-between">
            <h2 className="text-sm text-muted-foreground sm:pl-3">
              A contact form with Zod validation.
            </h2>
            <OpenInV0Button name="example-form" className="w-fit" />
          </div>
          <div className="flex items-center justify-center min-h-[500px] relative">
            <ExampleForm />
          </div>
        </div>

        <div className="flex flex-col gap-4 border rounded-lg p-4 min-h-[450px] relative">
          <div className="flex items-center justify-between">
            <h2 className="text-sm text-muted-foreground sm:pl-3">
              A complex component showing hooks, libs and components.
            </h2>
            <OpenInV0Button name="complex-component" className="w-fit" />
          </div>
          <div className="flex items-center justify-center min-h-[400px] relative">
            <PokemonPage />
          </div>
        </div>
      </main>
    </div>
  )
}
