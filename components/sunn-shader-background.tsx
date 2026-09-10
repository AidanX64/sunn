"use client"

import * as React from "react"
import { useTheme } from "next-themes"
import { createShader } from "@/lib/custom-shaders"

type ShaderHandle = {
  setTheme: (next: string) => void
  render: (time: number) => void
  destroy: () => void
}

export function SunnShaderBackground({
  className = "",
  darkBackground = "#090909",
  lightBackground = "#ffffff",
}: {
  className?: string
  darkBackground?: string
  lightBackground?: string
}) {
  const canvasRef = React.useRef<HTMLCanvasElement | null>(null)
  const handleRef = React.useRef<ShaderHandle | null>(null)
  const [failed, setFailed] = React.useState(false)
  const { resolvedTheme } = useTheme()

  React.useEffect(() => {
    const canvas = canvasRef.current
    if (!canvas) return
    const controller = new AbortController()
    let disposed = false

    ;(async () => {
      try {
        const gpu = (navigator as Navigator & { gpu?: unknown }).gpu
        if (!gpu) {
          if (!disposed) setFailed(true)
          return
        }
        const handle = (await createShader(canvas, {
          theme: resolvedTheme === "light" ? "light" : "dark",
          background: { dark: darkBackground, light: lightBackground },
          signal: controller.signal,
          onError: () => {
            if (!disposed) setFailed(true)
          },
        })) as unknown as ShaderHandle
        if (disposed) {
          handle.destroy()
          return
        }
        handleRef.current = handle
      } catch {
        if (!disposed) setFailed(true)
      }
    })()

    return () => {
      disposed = true
      controller.abort()
      handleRef.current?.destroy()
      handleRef.current = null
    }
    // Re-create only if backgrounds change; theme syncs separately below.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [darkBackground, lightBackground])

  React.useEffect(() => {
    handleRef.current?.setTheme(resolvedTheme === "light" ? "light" : "dark")
  }, [resolvedTheme])

  if (failed) {
    return (
      <div
        aria-hidden
        className={`bg-gradient-to-br from-violet-950 via-background to-background ${className}`}
      />
    )
  }

  return (
    <canvas
      ref={canvasRef}
      aria-hidden
      className={`block h-full w-full ${className}`}
    />
  )
}
