# Builds Forge from source and installs it to a user-level bin directory.
# Usage: powershell -ExecutionPolicy Bypass -File scripts/install.ps1

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$buildDir = Join-Path $root 'build'
$binDir = Join-Path $env:USERPROFILE 'bin'
$builtExe = Join-Path $buildDir 'forge.exe'
$installedExe = Join-Path $binDir 'forge.exe'

if (Get-Command gcc -ErrorAction SilentlyContinue) {
    $compiler = 'gcc'
} elseif (Get-Command clang -ErrorAction SilentlyContinue) {
    $compiler = 'clang'
} else {
    Write-Host "error: gcc or clang is required to build Forge" -ForegroundColor Red
    exit 1
}

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$sources = Get-ChildItem (Join-Path $root 'src') -Filter '*.c' | ForEach-Object { $_.FullName }

& $compiler -Wall -Wextra -Werror -std=c2x "-I$(Join-Path $root 'include')" $sources -o $builtExe
if ($LASTEXITCODE -ne 0) {
    Write-Host "error: build failed" -ForegroundColor Red
    exit $LASTEXITCODE
}

New-Item -ItemType Directory -Force -Path $binDir | Out-Null
Copy-Item $builtExe $installedExe -Force

Write-Host "Installed forge to $installedExe" -ForegroundColor Green
Write-Host ""
Write-Host "To use 'forge' from any directory, make sure the bin folder is on your PATH."
Write-Host "In the current PowerShell session, run:"
Write-Host ""
Write-Host "    `$env:Path += `"$binDir`""
Write-Host ""
Write-Host "To persist it for future sessions, run:"
Write-Host ""
Write-Host "    [Environment]::SetEnvironmentVariable('Path', [Environment]::GetEnvironmentVariable('Path', 'User') + `"$binDir`", 'User')"
Write-Host ""
Write-Host "Then open a new terminal and verify with:  forge --help"