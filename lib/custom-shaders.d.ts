export type ShaderTheme = "light" | "dark";

export type ShaderOptions = {
  theme?: ShaderTheme;
  background?: { dark?: string; light?: string };
  autoplay?: boolean;
  signal?: AbortSignal;
  onError?: (error: Error) => void;
};

export type ShaderHandle = {
  setTheme: (next: ShaderTheme) => void;
  render: (time: number) => void;
  destroy: () => void;
};

export function createShader(
  canvas: HTMLCanvasElement,
  options?: ShaderOptions
): Promise<ShaderHandle>;
