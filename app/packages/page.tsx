"use client"

import * as React from "react"
import Link from "next/link"
import { Input } from "@/components/ui/input"
import { Button } from "@/components/ui/button"
import {
  Card,
  CardContent,
  CardDescription,
  CardHeader,
  CardTitle,
} from "@/components/ui/card"
import type { NativePackage } from "@/lib/sunn-registry"

export default function PackagesPage() {
  const [q, setQ] = React.useState("")
  const [lang, setLang] = React.useState("")
  const [triplet, setTriplet] = React.useState("")
  const [pkgs, setPkgs] = React.useState<NativePackage[]>([])
  const [loading, setLoading] = React.useState(true)
  const [error, setError] = React.useState("")

  const load = React.useCallback(async () => {
    setLoading(true)
    setError("")
    try {
      const params = new URLSearchParams()
      if (q) params.set("q", q)
      if (lang) params.set("lang", lang)
      if (triplet) params.set("triplet", triplet)
      const res = await fetch(`/api/packages?${params.toString()}`)
      if (!res.ok) throw new Error(`HTTP ${res.status}`)
      const data = await res.json()
      setPkgs(data.packages ?? [])
    } catch (e) {
      setError(e instanceof Error ? e.message : "Failed to load")
    } finally {
      setLoading(false)
    }
  }, [q, lang, triplet])

  React.useEffect(() => {
    void load()
  }, [load])

  return (
    <div className="max-w-3xl mx-auto flex flex-col min-h-svh px-4 py-8 gap-6">
      <header className="flex flex-col gap-1">
        <h1 className="text-3xl font-bold tracking-tight">Native packages</h1>
        <p className="text-muted-foreground">
          C / C++ / ASM registry skeleton — static hosting under{" "}
          <code>public/packages</code> for now.
        </p>
      </header>

      <div className="flex flex-col sm:flex-row gap-2">
        <Input
          placeholder="Search packages…"
          value={q}
          onChange={(e) => setQ(e.target.value)}
        />
        <select
          aria-label="Language"
          className="h-8 rounded-lg border border-input bg-transparent px-2 text-sm"
          value={lang}
          onChange={(e) => setLang(e.target.value)}
        >
          <option value="">all langs</option>
          <option value="c">c</option>
          <option value="c++">c++</option>
          <option value="asm">asm</option>
        </select>
        <Input
          placeholder="triplet (e.g. x64-linux)"
          value={triplet}
          onChange={(e) => setTriplet(e.target.value)}
        />
        <Button onClick={() => void load()}>Search</Button>
      </div>

      {loading && <p className="text-sm text-muted-foreground">Loading…</p>}
      {error && <p className="text-sm text-destructive">{error}</p>}
      {!loading && !error && pkgs.length === 0 && (
        <p className="text-sm text-muted-foreground">No packages found.</p>
      )}

      <div className="grid gap-4">
        {pkgs.map((p) => (
          <Card key={`${p.name}@${p.version}`}>
            <CardHeader>
              <CardTitle>
                <Link
                  href={`/packages/${p.name}`}
                  className="underline underline-offset-4"
                >
                  {p.name} {p.version}
                </Link>
              </CardTitle>
              <CardDescription>{p.description}</CardDescription>
            </CardHeader>
            <CardContent className="flex flex-wrap gap-2 text-xs text-muted-foreground">
              <span>lang: {p.lang}</span>
              <span>·</span>
              <span>license: {p.license || "—"}</span>
              <span>·</span>
              <span>triplets: {p.triplets.join(", ") || "—"}</span>
            </CardContent>
          </Card>
        ))}
      </div>
    </div>
  )
}
