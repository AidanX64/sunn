#Requires -Version 5.1
<#
  One-way vendor sync: C:\Users\dooms\source\forge -> sunn/forge
  Preserves sunn/forge/SOURCE.md. Excludes build outputs, .git, and .github
  (CI is owned per-repo: upstream keeps its own, sunn tests the vendored
  tree via sunn/.github/workflows/ci.yml).
#>
$ErrorActionPreference = "Stop"
$Upstream = "C:\Users\dooms\source\forge"
$SunnRoot = Split-Path -Parent $PSScriptRoot
$Dest = Join-Path $SunnRoot "forge"
$SourceMd = Join-Path $Dest "SOURCE.md"
$Backup = $null
if (Test-Path -LiteralPath $SourceMd) { $Backup = Get-Content -LiteralPath $SourceMd -Raw }

if (-not (Test-Path -LiteralPath $Upstream)) { throw "Upstream not found: $Upstream" }
& robocopy $Upstream $Dest /E /XD .git .github build target .scratch .opencode examples /XF *.exe *.o *.obj *.a *.lib | Out-String | Write-Output
$code = $LASTEXITCODE
if ($code -ge 8) { throw "robocopy failed with exit code $code" }
if ($Backup -ne $null) { Set-Content -LiteralPath $SourceMd -Value $Backup -NoNewline }
Write-Output "sync-forge: done (robocopy exit $code). Update forge/SOURCE.md HEAD/date manually."
