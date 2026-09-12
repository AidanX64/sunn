#Requires -Version 5.1
<#
  One-way mirror: sunn/forge (canonical) -> standalone forge checkout.
  Copies the exact Sunn commit's forge/ subtree, excluding Sunn provenance
  (forge/SOURCE.md) and preserving standalone-only data (.github, build
  outputs, examples, generated MIRROR.md).

  Default (no flags) is --Check: report drift, change nothing.
    powershell -ExecutionPolicy Bypass -File scripts/mirror-forge.ps1 --Check
    powershell -ExecutionPolicy Bypass -File scripts/mirror-forge.ps1 --Apply
    powershell -ExecutionPolicy Bypass -File scripts/mirror-forge.ps1 --Apply --Commit --Push

  Options:
    -MirrorPath <dir>   standalone checkout (default $env:FORGE_MIRROR or C:\Users\dooms\source\forge)
    -Ref <rev>          Sunn revision to mirror (default HEAD; must be clean for forge/)
    -Check              report differences only (default)
    -Apply              write deletions/copies + MIRROR.md into the mirror
    -Commit             stage + commit the mirror result (implies working tree changes only from mirror)
    -Push               push the mirror's current branch to origin
#>
param(
    [string]$MirrorPath = $(if ($env:FORGE_MIRROR) { $env:FORGE_MIRROR } else { "C:\Users\dooms\source\forge" }),
    [string]$Ref = "HEAD",
    [switch]$Check,
    [switch]$Apply,
    [switch]$Commit,
    [switch]$Push
)
$ErrorActionPreference = "Stop"

$SunnRoot = Split-Path -Parent $PSScriptRoot
$SourceSub = "forge"
if ($Apply -and $Check) { throw "Use only one of --Apply or --Check." }
$DoApply = $Apply.IsPresent
if (-not $DoApply -and -not $Check.IsPresent) { $DoApply = $false }

function Invoke-Git($dir, [string[]]$gitArgs) {
    $out = & git -C $dir @gitArgs 2>&1
    if ($LASTEXITCODE -ne 0) { throw "git $($gitArgs -join ' ') failed: $out" }
    return $out
}

function Test-Preserved($relPath) {    if ($relPath -eq "MIRROR.md") { return $true }
    if ($relPath -like ".github/*") { return $true }
    if ($relPath -like "build/*" -or $relPath -like "target/*" -or $relPath -like "test/target/*") { return $true }
    if ($relPath -like "examples/*" -or $relPath -like ".scratch/*" -or $relPath -like ".opencode/*") { return $true }
    if ($relPath -like "*.exe" -or $relPath -like "*.o" -or $relPath -like "*.obj" -or $relPath -like "*.a" -or $relPath -like "*.lib") { return $true }
    return $false
}

# Line-ending-insensitive content hash: checkouts may carry CRLF while
# git blobs carry LF (core.autocrlf / text=auto). Comparing raw bytes
# would flag every text file on every mirror run.
function Get-NormalizedHash([string]$LiteralPath) {
    $bytes = [System.IO.File]::ReadAllBytes($LiteralPath)
    $text = [System.Text.Encoding]::GetEncoding("iso-8859-1").GetString($bytes)
    $norm = $text.Replace("`r`n", "`n")
    $normBytes = [System.Text.Encoding]::GetEncoding("iso-8859-1").GetBytes($norm)
    return [System.BitConverter]::ToString(
        [System.Security.Cryptography.SHA256]::Create().ComputeHash($normBytes)
    ).Replace("-", "").ToLower()
}

# Resolve and guard the canonical revision.
$sha = ((Invoke-Git $SunnRoot @("rev-parse", "--verify", $Ref)) | Out-String).Trim()
$commitDate = ((Invoke-Git $SunnRoot @("show", "-s", "--format=%cI", $sha)) | Out-String).Trim()
$sunnBranch = ((Invoke-Git $SunnRoot @("rev-parse", "--abbrev-ref", "HEAD")) | Out-String).Trim()
$sunnDirty = Invoke-Git $SunnRoot @("status", "--porcelain", "--", $SourceSub)
if ($sunnDirty) { throw "Refusing mirror: sunn/$SourceSub has uncommitted changes. Commit them first.`n$sunnDirty" }

# Guard the mirror checkout.
if (-not (Test-Path -LiteralPath $MirrorPath)) { throw "Mirror checkout not found: $MirrorPath" }
Invoke-Git $MirrorPath @("rev-parse", "--git-dir") | Out-Null
$mirrorDirty = Invoke-Git $MirrorPath @("status", "--porcelain")
if ($mirrorDirty) { throw "Refusing mirror: destination checkout is dirty. Commit or stash there first.`n$mirrorDirty" }
$mirrorBranch = ((Invoke-Git $MirrorPath @("rev-parse", "--abbrev-ref", "HEAD")) | Out-String).Trim()

# Export the exact canonical subtree to staging.
$staging = Join-Path ([System.IO.Path]::GetTempPath()) ("sunn-forge-mirror-" + [System.Guid]::NewGuid().ToString("N"))
$tarball = "$staging.tar"
try {
    New-Item -ItemType Directory -Path $staging | Out-Null
    & git -C $SunnRoot archive --format=tar --output=$tarball $sha $SourceSub 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "git archive of $sha failed." }
    & tar -xf $tarball -C $staging 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "tar extraction of the canonical subtree failed." }

    $sourceFiles = @((Invoke-Git $SunnRoot @("ls-tree", "-r", "--name-only", $sha, "--", $SourceSub)) |
        ForEach-Object { $_.Trim() -replace "^forge/", "" } |
        Where-Object { $_ -ne "" -and $_ -ne "SOURCE.md" })

    $destTracked = @(Invoke-Git $MirrorPath @("ls-files") | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" })
    $destManaged = @($destTracked | Where-Object { -not (Test-Preserved $_) })
    # Case-sensitive destination set (same reason as $sourceSet below):
    # Test-Path alone follows Windows case-insensitivity and would miss
    # case-only renames.
    $destSet = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($rel in $destTracked) { [void]$destSet.Add($rel) }

    $missing = @()
    $changed = @()
    foreach ($rel in $sourceFiles) {
        $src = Join-Path (Join-Path $staging $SourceSub) $rel
        $dst = Join-Path $MirrorPath $rel
        if (-not $destSet.Contains($rel)) { $missing += $rel; continue }
        if (-not (Test-Path -LiteralPath $dst -PathType Leaf)) { $missing += $rel; continue }
        $a = Get-NormalizedHash -LiteralPath $src
        $b = Get-NormalizedHash -LiteralPath $dst
        if ($a -ne $b) { $changed += $rel }
    }
    # Case-sensitive set: Windows paths are case-insensitive, but git
    # tracks case (Forge.toml vs forge.toml), so a plain hashtable
    # (case-insensitive) would miss case-only renames.
    $sourceSet = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($rel in $sourceFiles) { [void]$sourceSet.Add($rel) }
    $removed = @($destManaged | Where-Object { -not $sourceSet.Contains($_) })

    # Generated standalone provenance (standalone-only; never mirrored back).
    $hashLines = @()
    foreach ($rel in ($sourceFiles | Sort-Object)) {
        $src = Join-Path (Join-Path $staging $SourceSub) $rel
        $hashLines += "$(Get-NormalizedHash -LiteralPath $src)  $rel"
    }
    $rootHash = [System.BitConverter]::ToString(
        [System.Security.Cryptography.SHA256]::Create().ComputeHash(
            [System.Text.Encoding]::UTF8.GetBytes(($hashLines -join "`n")))).Replace("-", "").ToLower()
    $mirrorMd = @(
        "# forge - downstream mirror",
        "",
        "This checkout is a read-only mirror. sunn/forge is canonical - do not edit sources here.",
        "",
        "- Canonical repo: https://github.com/AidanX64/sunn.git (forge/ subtree, branch $sunnBranch)",
        "- Canonical commit: $sha ($commitDate)",
        "- Canonical files: $($sourceFiles.Count)",
        "- Canonical tree hash: $rootHash",
        "- Mirrored: $([DateTime]::UtcNow.ToString("yyyy-MM-dd")) (mirror-forge)",
        ""
    ) -join "`n"
    $mirrorNotePath = Join-Path $MirrorPath "MIRROR.md"
    $mirrorDiffers = $true
    if (Test-Path -LiteralPath $mirrorNotePath -PathType Leaf) {
        $existing = [System.IO.File]::ReadAllText($mirrorNotePath)
        if ($existing -eq $mirrorMd) { $mirrorDiffers = $false }
    }

    $noteDrift = 0
    if ($mirrorDiffers) { $noteDrift = 1 }
    $totalDrift = $missing.Count + $changed.Count + $removed.Count + $noteDrift
    $contentDrift = $missing.Count + $changed.Count + $removed.Count
    Write-Output "canonical: sunn@$sha ($($sourceFiles.Count) files, tree $rootHash)"
    Write-Output "mirror:    $MirrorPath (branch $mirrorBranch)"
    Write-Output "missing-in-mirror: $($missing.Count); changed: $($changed.Count); removed-upstream: $($removed.Count); mirror-note-differs: $mirrorDiffers"
    foreach ($rel in ($missing | Select-Object -First 20)) { Write-Output "  + $rel" }
    foreach ($rel in ($changed | Select-Object -First 20)) { Write-Output "  ~ $rel" }
    foreach ($rel in ($removed | Select-Object -First 20)) { Write-Output "  - $rel" }

    if (-not $DoApply) {
        # A stale mirror note alone is informational (it records which
        # canonical commit was last mirrored); only content drift fails.
        if ($contentDrift -ne 0) { exit 1 }
        if ($totalDrift -ne 0) { Write-Output "mirror-forge: content matches; mirror note is stale."; return }
        Write-Output "mirror-forge: clean."
        return
    }

    foreach ($rel in $removed) {
        $dst = Join-Path $MirrorPath $rel
        if (Test-Path -LiteralPath $dst) { Remove-Item -LiteralPath $dst -Force }
    }
    foreach ($rel in ($missing + $changed)) {
        $src = Join-Path (Join-Path $staging $SourceSub) $rel
        $dst = Join-Path $MirrorPath $rel
        $parent = Split-Path -Parent $dst
        if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent | Out-Null }
        Copy-Item -LiteralPath $src -Destination $dst -Force
    }
    [System.IO.File]::WriteAllText($mirrorNotePath, $mirrorMd, [System.Text.UTF8Encoding]::new($false))

    if ($Commit -or $Push) {
        Invoke-Git $MirrorPath @("add", "-A") | Out-Null
        $after = Invoke-Git $MirrorPath @("status", "--porcelain")
        if (-not $after) {
            Write-Output "mirror-forge: applied; nothing new to commit."
        } else {
            $msgFile = Join-Path ([System.IO.Path]::GetTempPath()) ("forge-mirror-msg-" + [System.Guid]::NewGuid().ToString("N") + ".txt")
            $msg = "Mirror sunn/forge from sunn@$sha`n`nCanonical: sunn $sunnBranch@$sha ($commitDate).`nFiles: $($sourceFiles.Count); tree $rootHash.`n"
            [System.IO.File]::WriteAllText($msgFile, $msg, [System.Text.UTF8Encoding]::new($false))
            try {
                Invoke-Git $MirrorPath @("commit", "-F", $msgFile) | Write-Output
            } finally {
                Remove-Item -LiteralPath $msgFile -Force -ErrorAction SilentlyContinue
            }
        }
        if ($Push) {
            Invoke-Git $MirrorPath @("push", "origin", $mirrorBranch) | Write-Output
        }
    }
    Write-Output "mirror-forge: applied."
} finally {
    Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $tarball -Force -ErrorAction SilentlyContinue
}
