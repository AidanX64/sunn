#Requires -Version 5.1
# Rebuilds fixture tarballs + metadata under public/packages/.
$ErrorActionPreference = "Stop"
$SunnRoot = Split-Path -Parent $PSScriptRoot
& python "$SunnRoot\scripts\package_registry_fixtures.py"
if ($LASTEXITCODE -ne 0) { throw "package_registry_fixtures.py failed with exit code $LASTEXITCODE" }
