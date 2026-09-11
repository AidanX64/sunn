#!/usr/bin/env bash
#
# Regression tests for sunn-registry dependencies:
#
#   R1  `forge add NAME --registry PKG --version VER` writes a pinned
#       manifest entry, downloads + verifies + unpacks the tarball, pins
#       {kind, version, location, sha256} in Forge.lock, and the dep builds.
#   R2  a bare entry pins the registry baseline (not newest); `forge
#       update` moves it to newest while the manifest stays bare.
#   R3  warm cache is offline-friendly; a cold cache + --offline fails
#       naming the dependency.
#   R4  a tampered lock sha is refused loudly (integrity gate).
#   R5  bare entries follow the baseline; exact pins stay put under
#       `forge update`; --locked refuses a pin move.
#   R6  transport policy: http outside localhost and bare file:// are
#       refused before any download (FORGE_ALLOW_UNSAFE_REGISTRY=1 opts
#       local file registries back in).
#   R7  manifest validation: registry+path in one entry, version on a git
#       dep, and unknown versions fail with readable errors (and `add`
#       rolls the manifest edit back).
#   R8  transitive diamond on one name with different registry versions
#       conflicts instead of first-one-wins.
#   R9  the same flow over http://127.0.0.1 (needs python3; skipped
#       with a notice when absent).
#   R10 `forge add --registry PKG --min-version VER` pins the floor;
#       satisfied pins stay, unsatisfiable minimums and exact-below-min
#       diamonds fail with a rollback.
#   R11 recipe revisions: repacks move only via `forge update`; plain
#       builds refuse stale pins loudly and record revision in Forge.lock.
#   R12 pre-revision lockfiles (no revision key) resolve byte-identically
#       and pass --locked.
#
# The stub registry is generated in a temp dir (fixture sources, tarballs,
# real sha256, index JSON) so the suite is hermetic: no network except
# loopback in R9.
set -u

replace_in_file() {
    if sed --version >/dev/null 2>&1; then
        sed -i "$1" "$2"
    else
        sed -i '' "$1" "$2"
    fi
}

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) FORGE="$root/build/forge.exe" ;;
    *)                    FORGE="$root/build/forge" ;;
esac
[ -x "$FORGE" ] || { echo "forge binary missing; run make first ($FORGE)" >&2; exit 1; }

if ! command -v git >/dev/null 2>&1; then
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            PATH="/mingw64/bin:/c/Program Files/Git/cmd:$PATH"
            ;;
    esac
fi
command -v git >/dev/null 2>&1 || { echo "git is required for these tests" >&2; exit 1; }
# forge.exe is a native Windows binary: MSYS converts PATH for it, keeping
# Git's /usr/bin (MSYS binaries) ahead of System32. MSYS tar cannot run as
# a grandchild of a native process (its gzip helper will not spawn), so put
# the native System32 curl/tar first. Bash keeps using its own entries.
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        PATH="/c/Windows/System32:$PATH"
        ;;
esac
command -v tar >/dev/null 2>&1 || { echo "tar is required for these tests" >&2; exit 1; }
if command -v sha256sum >/dev/null 2>&1; then
    sha256_of() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v python3 >/dev/null 2>&1; then
    sha256_of() { python3 -c "import hashlib,sys; print(hashlib.sha256(open(sys.argv[1],'rb').read()).hexdigest())" "$1"; }
else
    echo "sha256sum or python3 is required for these tests" >&2
    exit 1
fi

work="$(mktemp -d)"
if command -v cygpath >/dev/null 2>&1; then
    work_forge="$(cygpath -m "$work")"   # C:/... form for forge
else
    work_forge="$work"
fi
case "$work_forge" in
    /*) REG_FILE="file://$work_forge/stub" ;;
    *)  REG_FILE="file:///$work_forge/stub" ;;
esac

export FORGE_HOME="$work_forge/home"
export FORGE_ALLOW_UNSAFE_REGISTRY=1

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

pass() {
    echo "ok: $*"
}

# --- stub registry ----------------------------------------------------
# make_registry_pkg <name> <version> <retval> [revision]: (re)builds one
# stub package (lib sources + tarball + JSON), then refreshes the index.
make_registry_pkg() {
    local name="$1" version="$2" retval="$3" revision="${4:-0}"
    local src="$work/stub-src/$name" out="$work/stub/packages/$name"
    mkdir -p "$src/src" "$src/include" "$out"
    cat >"$src/Forge.toml" <<EOF
[project]
name = "$name"
version = "$version"

[sources]
c = ["src"]
cpp = []
asm = []

[targets]
os = ["windows", "linux", "macos"]
arch = ["x86_64", "aarch64"]

[profile.debug]
cflags = ["-g", "-O0"]

[profile.release]
cflags = ["-O2"]
EOF
    # Extra deps lines (e.g. transitive fixtures) come from $EXTRA_DEPS.
    if [ -n "${EXTRA_DEPS:-}" ]; then
        printf '[dependencies]\n%s\n' "$EXTRA_DEPS" >>"$src/Forge.toml"
    fi
    cat >"$src/include/$name.h" <<EOF
#ifndef ${name}_H
#define ${name}_H
int ${name}_value(void);
#endif
EOF
    # 'h' is not valid in the symbol above when name has '-'; keep names
    # simple (hello_c style) so the header guard stays a valid identifier.
    cat >"$src/src/$name.c" <<EOF
#include "$name.h"
int ${name}_value(void) { return $retval; }
EOF
    tar -czf "$out/$name-$version.tar.gz" -C "$src" Forge.toml include src
    local sha
    sha="$(sha256_of "$out/$name-$version.tar.gz")"
    cat >"$out/$version.json" <<EOF
{
  "name": "$name",
  "version": "$version",
  "revision": $revision,
  "description": "registry regression stub",
  "license": "MIT",
  "homepage": "https://example.com/$name",
  "lang": "c",
  "build": "forge",
  "dependencies": [],
  "source": {"kind": "url", "location": "/packages/$name/$name-$version.tar.gz", "sha256": "$sha"},
  "patches": [],
  "forge": {"manifest": "/packages/$name/Forge.toml"}
}
EOF
    cp "$src/Forge.toml" "$out/Forge.toml"
    echo "$version" >"$out/.latest"
    refresh_index
    echo "$sha"
}

refresh_index() {
    # Rebuilds packages/sunn.registry.json from whatever package dirs exist
    # (same layout as the real static hosting: index inside packages/).
    # The newest release per package comes from a .latest sidecar (written
    # at pack time) so no version-sort tool is needed portably.
    mkdir -p "$work/stub/packages"
    {
        echo '{'
        echo '  "name": "sunn-test",'
        echo '  "description": "regression stub index",'
        echo '  "packages": ['
        local first=1
        for dir in "$work"/stub/packages/*/; do
            [ -d "$dir" ] || continue
            local name latest
            name="$(basename "$dir")"
            latest="$(cat "$dir/.latest")"
            [ "$first" -eq 1 ] || echo ','
            first=0
            printf '    {"name": "%s", "latest": "%s", "description": "stub", "license": "MIT", "homepage": "", "index": "/packages/%s/%s.json"}' \
                "$name" "$latest" "$name" "$latest"
        done
        echo ''
        echo '  ]'
        echo '}'
    } >"$work/stub/packages/sunn.registry.json"
}

# make_consumer <dir> <main-body>: minimal project with a main.c.
make_consumer() {
    local dir="$1" body="$2"
    mkdir -p "$dir/src"
    cat >"$dir/Forge.toml" <<'EOF'
[project]
name = "consumer"
version = "0.1.0"

[sources]
c = ["src"]
cpp = []
asm = []

[targets]
os = ["windows", "linux", "macos"]
arch = ["x86_64", "aarch64"]

[profile.debug]
cflags = ["-g", "-O0"]

[profile.release]
cflags = ["-O2"]
EOF
    printf '%s\n' "$body" >"$dir/src/main.c"
}

# write_baseline <name:version:revision>...: rewrites the stub
# baseline.json from explicit triples (no args = no baseline file, i.e. a
# pre-baseline registry). Scenarios set it explicitly so each one controls
# the floor it resolves against.
write_baseline() {
    local spec name version revision first=1
    rm -f "$work/stub/baseline.json"
    [ "$#" -eq 0 ] && return 0
    {
        echo '{'
        echo '  "name": "sunn-test-baseline",'
        echo '  "baseline": ['
        for spec in "$@"; do
            IFS=: read -r name version revision <<<"$spec"
            [ "$first" -eq 1 ] || echo ','
            first=0
            printf '    {"name": "%s", "version": "%s", "revision": %s}' \
                "$name" "$version" "$revision"
        done
        echo ''
        echo '  ]'
        echo '}'
    } >"$work/stub/baseline.json"
}

export FORGE_REGISTRY_URL="$REG_FILE"

# --- R1: pinned add + build + run --------------------------------------
sha010="$(make_registry_pkg hello_c 0.1.0 42)"
make_consumer "$work/c1" '#include <stdio.h>
#include "hello_c.h"
int main(void) { printf("%d\n", hello_c_value()); return hello_c_value(); }'
(cd "$work/c1" && "$FORGE" add greeting --registry hello_c --version 0.1.0 >/dev/null 2>&1) \
    || fail "R1: add --registry failed"
grep -q 'greeting = { registry = "hello_c", version = "0.1.0" }' "$work/c1/Forge.toml" \
    || fail "R1: manifest entry not pinned"
grep -q "greeting = .*kind = \"url\".*version = \"0.1.0\".*sha256 = \"$sha010\"" "$work/c1/Forge.lock" \
    || fail "R1: lock pin missing/wrong"
(cd "$work/c1" && "$FORGE" run >/dev/null 2>&1); code=$?
[ "$code" -eq 42 ] || fail "R1: run exited $code, want 42 (dep link broken?)"
pass "R1 pinned add, build, run (exit 42)"

# --- R2: bare add pins the baseline, update tracks newest -------------
# Latest is 0.2.0 but the baseline lags at 0.1.0: a bare entry pins the
# floor (vcpkg semantics), and `forge update` moves it to newest while the
# manifest stays bare.
sha020="$(make_registry_pkg hello_c 0.2.0 43)"
write_baseline "hello_c:0.1.0:0"
make_consumer "$work/c2" 'int main(void) { return 0; }'
(cd "$work/c2" && "$FORGE" add greeting --registry hello_c >/dev/null 2>&1) \
    || fail "R2: unpinned add failed"
grep -q 'greeting = { registry = "hello_c" }' "$work/c2/Forge.toml" \
    || fail "R2: bare add should write no version"
grep -q "sha256 = \"$sha010\"" "$work/c2/Forge.lock" \
    || fail "R2: bare add did not pin the baseline (0.1.0)"
(cd "$work/c2" && "$FORGE" update greeting >/dev/null 2>&1) \
    || fail "R2: update failed"
grep -q "sha256 = \"$sha020\"" "$work/c2/Forge.lock" \
    || fail "R2: update did not move to newest (0.2.0)"
grep -q 'greeting = { registry = "hello_c" }' "$work/c2/Forge.toml" \
    || fail "R2: update should leave a bare entry bare"
pass "R2 bare add pins baseline, update tracks newest"

# --- R3: offline --------------------------------------------------------
(cd "$work/c1" && "$FORGE" build --offline >/dev/null 2>&1) \
    || fail "R3: warm cache + --offline should resolve"
rm -rf "$FORGE_HOME"
if (cd "$work/c1" && "$FORGE" build --offline >/dev/null 2>&1); then
    fail "R3: cold cache + --offline should fail"
fi
(cd "$work/c1" && "$FORGE" build --offline 2>&1 | grep -q "greeting") \
    || fail "R3: offline failure does not name the dependency"
pass "R3 offline reuse + cold-cache failure"

# --- R4: tampered pin ---------------------------------------------------
(cd "$work/c1" && "$FORGE" build >/dev/null 2>&1) \
    || fail "R4 setup: rebuild after cache wipe failed"
if command -v sed >/dev/null 2>&1; then
    replace_in_file "s/$sha010/0000000000000000000000000000000000000000000000000000000000000000/" \
        "$work/c1/Forge.lock"
else
    python3 - "$work/c1/Forge.lock" "$sha010" <<'PYEOF'
import sys
p, old = sys.argv[1], sys.argv[2]
s = open(p).read().replace(old, "0" * 64)
open(p, "w").write(s)
PYEOF
fi
if (cd "$work/c1" && "$FORGE" build >/dev/null 2>&1); then
    fail "R4: tampered sha256 should fail"
fi
(cd "$work/c1" && "$FORGE" build 2>&1 | grep -qi "sha256\|malformed\|changed") \
    || fail "R4: tamper failure is not loud about integrity"
# Recovery: dropping the lock regenerates a good pin from the manifest.
rm "$work/c1/Forge.lock"
(cd "$work/c1" && "$FORGE" build >/dev/null 2>&1) \
    || fail "R4: lock regeneration failed"
grep -q "sha256 = \"$sha010\"" "$work/c1/Forge.lock" \
    || fail "R4: regenerated pin differs"
pass "R4 tampered pin refused"

# --- R5: bare entries follow the baseline; exact pins stay; locked refuses
# A new release appears after the pins above were made; the baseline moves
# with it here, so fresh bare adds pin 0.3.0 (R2 covers a lagging baseline).
sha030="$(make_registry_pkg hello_c 0.3.0 44)"
write_baseline "hello_c:0.3.0:0"
# Unversioned entries resolve the baseline: fresh add pins 0.3.0 ...
make_consumer "$work/c5" 'int main(void) { return 0; }'
(cd "$work/c5" && "$FORGE" add greeting --registry hello_c >/dev/null 2>&1) \
    || fail "R5: unpinned add failed"
grep -q 'greeting = { registry = "hello_c" }' "$work/c5/Forge.toml" \
    || fail "R5: bare add should write no version"
grep -q "sha256 = \"$sha030\"" "$work/c5/Forge.lock" \
    || fail "R5: bare add did not pin the baseline (0.3.0)"
# ... and a lagging entry moves past its pin on re-add.
(cd "$work/c2" && "$FORGE" remove greeting >/dev/null 2>&1) \
    || fail "R5 setup: remove failed"
(cd "$work/c2" && "$FORGE" add greeting --registry hello_c >/dev/null 2>&1) \
    || fail "R5 setup: re-add failed"
grep -q "sha256 = \"$sha030\"" "$work/c2/Forge.lock" \
    || fail "R5 setup: re-add did not pin 0.3.0"
# An exact pin never moves under `forge update` (only the manifest moves it).
(cd "$work/c1" && "$FORGE" update greeting >/dev/null 2>&1) \
    || fail "R5: update of an exact pin should succeed quietly"
grep -q "sha256 = \"$sha010\"" "$work/c1/Forge.lock" \
    || fail "R5: update moved an exact pin"
# --locked refuses a manifest edit that would move a pin.
replace_in_file 's/registry = "hello_c", version = "0.1.0"/registry = "hello_c", version = "0.3.0"/' "$work/c1/Forge.toml"
if (cd "$work/c1" && "$FORGE" build --locked >/dev/null 2>&1); then
    fail "R5: --locked should refuse a pin move"
fi
grep -q "sha256 = \"$sha010\"" "$work/c1/Forge.lock" \
    || fail "R5: --locked run rewrote the pin"
pass "R5 baseline tracking, exact pins stay, locked refuses"

# --- R6: transport policy -----------------------------------------------
make_consumer "$work/c6" 'int main(void) { return 0; }'
if (cd "$work/c6" && FORGE_REGISTRY_URL="http://example.com/registry" "$FORGE" add x --registry hello_c --version 0.1.0 >/dev/null 2>&1); then
    fail "R6: plain-http registry should be refused"
fi
(cd "$work/c6" && FORGE_REGISTRY_URL="http://example.com/registry" "$FORGE" add x --registry hello_c --version 0.1.0 2>&1 | grep -qi "https\|http") \
    || fail "R6: http refusal does not explain itself"
if (cd "$work/c6" && FORGE_ALLOW_UNSAFE_REGISTRY= FORGE_REGISTRY_URL="$REG_FILE" "$FORGE" add x --registry hello_c --version 0.1.0 >/dev/null 2>&1); then
    fail "R6: file:// without the opt-in should be refused"
fi
(cd "$work/c6" && FORGE_ALLOW_UNSAFE_REGISTRY= FORGE_REGISTRY_URL="$REG_FILE" "$FORGE" add x --registry hello_c --version 0.1.0 2>&1 | grep -q "FORGE_ALLOW_UNSAFE_REGISTRY") \
    || fail "R6: file refusal does not name the opt-in"
grep -q "x = " "$work/c6/Forge.toml" && fail "R6: refused add left a manifest entry"
pass "R6 transport policy"

# --- R7: manifest validation --------------------------------------------
make_consumer "$work/c7" 'int main(void) { return 0; }'
printf '\n[dependencies]\nbad = { registry = "hello_c", path = "x" }\n' >>"$work/c7/Forge.toml"
if (cd "$work/c7" && "$FORGE" check >/dev/null 2>&1); then
    fail "R7: registry+path should not parse"
fi
(cd "$work/c7" && "$FORGE" check 2>&1 | grep -qi "exactly one source") \
    || fail "R7: mixed-source error is unclear"
make_consumer "$work/c7b" 'int main(void) { return 0; }'
if (cd "$work/c7b" && "$FORGE" add x --registry hello_c --version nope-nope! >/dev/null 2>&1); then
    fail "R7: bad version should fail add"
fi
grep -q "x = " "$work/c7b/Forge.toml" && fail "R7: failed add left a manifest entry"
# min-version and version exclude each other, on the CLI and in manifests.
if (cd "$work/c7b" && "$FORGE" add y --registry hello_c --version 0.1.0 --min-version 0.1.0 >/dev/null 2>&1); then
    fail "R7: --version/--min-version together should fail add"
fi
if (cd "$work/c7b" && "$FORGE" add z --path "$work/c7" --min-version 0.1.0 >/dev/null 2>&1); then
    fail "R7: --min-version on a path dep should fail add"
fi
make_consumer "$work/c7c" 'int main(void) { return 0; }'
printf '\n[dependencies]\nboth = { registry = "hello_c", version = "0.1.0", min-version = "0.1.0" }\n' >>"$work/c7c/Forge.toml"
if (cd "$work/c7c" && "$FORGE" check >/dev/null 2>&1); then
    fail "R7: version+min-version should not parse"
fi
(cd "$work/c7c" && "$FORGE" check 2>&1 | grep -qi "only one of") \
    || fail "R7: version/min-version error is unclear"
make_consumer "$work/c7d" 'int main(void) { return 0; }'
printf '\n[dependencies]\nfloor = { registry = "hello_c", min-version = "bogus" }\n' >>"$work/c7d/Forge.toml"
if (cd "$work/c7d" && "$FORGE" check >/dev/null 2>&1); then
    fail "R7: bad min-version should not parse"
fi
(cd "$work/c7d" && "$FORGE" check 2>&1 | grep -qi "min-version") \
    || fail "R7: bad min-version error is unclear"
pass "R7 manifest validation + add rollback"

# --- R8: transitive diamond conflicts -----------------------------------
# Same local name, different versions: consumer pins hello_c 0.1.0 while
# mid_lib's manifest wants hello_c 0.2.0. (Different local names are
# different nodes, same as git deps — only same-name conflicts are
# refused, per the M5 rule.)
EXTRA_DEPS='hello_c = { registry = "hello_c", version = "0.2.0" }' make_registry_pkg mid_lib 0.1.0 7 >/dev/null
make_consumer "$work/c8" 'int main(void) { return 0; }'
(cd "$work/c8" && "$FORGE" add via_mid --registry mid_lib --version 0.1.0 >/dev/null 2>&1) \
    || fail "R8 setup: mid_lib add failed"
# The conflict surfaces at add time too, rolling the entry back.
if (cd "$work/c8" && "$FORGE" add hello_c --registry hello_c --version 0.1.0 >/dev/null 2>&1); then
    fail "R8: conflicting add should fail"
fi
grep -q "hello_c = " "$work/c8/Forge.toml" && fail "R8: conflicting add left a manifest entry"
# And at resolve time with a hand-written manifest (fresh consumer: append
# the whole section, since `add` already created one in c8).
make_consumer "$work/c8b" 'int main(void) { return 0; }'
printf '\n[dependencies]\nvia_mid = { registry = "mid_lib", version = "0.1.0" }\nhello_c = { registry = "hello_c", version = "0.1.0" }\n' >>"$work/c8b/Forge.toml"
if (cd "$work/c8b" && "$FORGE" check >/dev/null 2>&1); then
    fail "R8: diamond (0.1.0 vs 0.2.0) should conflict"
fi
(cd "$work/c8b" && "$FORGE" check 2>&1 | grep -qi "conflict") \
    || fail "R8: diamond failure does not say conflict"
pass "R8 transitive diamond conflicts"

# --- R10: min-version floors --------------------------------------------
# Baseline lags at 0.1.0 with 0.3.0 newest: a 0.2.0 minimum pins exactly
# the floor, satisfied pins stay put, and anything unsatisfiable fails
# loudly with a rollback.
write_baseline "hello_c:0.1.0:0"
make_consumer "$work/c10" 'int main(void) { return 0; }'
(cd "$work/c10" && "$FORGE" add needy --registry hello_c --min-version 0.2.0 >/dev/null 2>&1) \
    || fail "R10: add --min-version failed"
grep -q 'needy = { registry = "hello_c", min-version = "0.2.0" }' "$work/c10/Forge.toml" \
    || fail "R10: manifest minimum not written"
grep -q "sha256 = \"$sha020\"" "$work/c10/Forge.lock" \
    || fail "R10: minimum did not pin the floor (0.2.0)"
(cd "$work/c10" && "$FORGE" build >/dev/null 2>&1) \
    || fail "R10: rebuild above the minimum failed"
grep -q "sha256 = \"$sha020\"" "$work/c10/Forge.lock" \
    || fail "R10: rebuild moved a satisfied pin"
# Above newest: nothing can satisfy it.
make_consumer "$work/c10b" 'int main(void) { return 0; }'
if (cd "$work/c10b" && "$FORGE" add needy --registry hello_c --min-version 9.9.9 >/dev/null 2>&1); then
    fail "R10: unsatisfiable minimum should fail add"
fi
grep -q "needy = " "$work/c10b/Forge.toml" && fail "R10: failed add left a manifest entry"
# Same-name exact below the minimum conflicts instead of silently winning.
EXTRA_DEPS='hello_c = { registry = "hello_c", min-version = "0.2.0" }' make_registry_pkg mid_min 0.1.0 7 >/dev/null
make_consumer "$work/c10c" 'int main(void) { return 0; }'
(cd "$work/c10c" && "$FORGE" add via_mid --registry mid_min --version 0.1.0 >/dev/null 2>&1) \
    || fail "R10 setup: mid_min add failed"
if (cd "$work/c10c" && "$FORGE" add hello_c --registry hello_c --version 0.1.0 >/dev/null 2>&1); then
    fail "R10: exact below the minimum should conflict"
fi
grep -q "hello_c = " "$work/c10c/Forge.toml" && fail "R10: conflicting add left a manifest entry"
pass "R10 min-version floors"

# --- R11: recipe revisions ----------------------------------------------
# 0.1.0 repacked with new bytes at revision 1: plain builds never ambush
# a locked pin (same loud refusal repacks always got), while `forge
# update` moves to the new revision.
sha011="$(make_registry_pkg hello_c 0.1.0 45 1)"
write_baseline "hello_c:0.1.0:1"
make_consumer "$work/c11" 'int main(void) { return 0; }'
(cd "$work/c11" && "$FORGE" add greeting --registry hello_c --version 0.1.0 >/dev/null 2>&1) \
    || fail "R11: pinned add failed"
grep -q "sha256 = \"$sha011\"" "$work/c11/Forge.lock" \
    || fail "R11: lock does not pin the repacked bytes"
grep -q 'revision = "1"' "$work/c11/Forge.lock" \
    || fail "R11: lock does not pin the revision"
(cd "$work/c11" && "$FORGE" build >/dev/null 2>&1) \
    || fail "R11: rebuild failed"
# A lock pinning the old revision refuses a plain build loudly...
make_consumer "$work/c11b" 'int main(void) { return 0; }'
printf '\n[dependencies]\ngreeting = { registry = "hello_c", version = "0.1.0" }\n' >>"$work/c11b/Forge.toml"
printf '# Generated by forge. Do not edit.\n[dependencies]\ngreeting = { kind = "url", version = "0.1.0", location = "%s/packages/hello_c/hello-c-0.1.0.tar.gz", sha256 = "%s" }\n' \
    "$REG_FILE" "$sha010" >"$work/c11b/Forge.lock"
if (cd "$work/c11b" && "$FORGE" build >/dev/null 2>&1); then
    fail "R11: stale-revision pin should fail a plain build"
fi
(cd "$work/c11b" && "$FORGE" build 2>&1 | grep -qi "changed\|sha256") \
    || fail "R11: stale-revision failure is not loud about integrity"
# ...but `forge update` moves it to the new revision.
(cd "$work/c11b" && "$FORGE" update greeting >/dev/null 2>&1) \
    || fail "R11: update failed"
grep -q "sha256 = \"$sha011\"" "$work/c11b/Forge.lock" \
    || fail "R11: update did not move to revision 1"
pass "R11 recipe revisions"

# --- R12: old lockfiles (no revision) keep working ----------------------
# A hand-written pre-revision pin resolves byte-identically (no forced
# migration) and passes --locked once current.
sha_old="$(make_registry_pkg oldie 0.5.0 11)"
make_consumer "$work/c12" 'int main(void) { return 0; }'
printf '\n[dependencies]\nold = { registry = "oldie", version = "0.5.0" }\n' >>"$work/c12/Forge.toml"
printf '# Generated by forge. Do not edit.\n[dependencies]\nold = { kind = "url", version = "0.5.0", location = "%s/packages/oldie/oldie-0.5.0.tar.gz", sha256 = "%s" }\n' \
    "$REG_FILE" "$sha_old" >"$work/c12/Forge.lock"
cp "$work/c12/Forge.lock" "$work/c12/Forge.lock.orig"
(cd "$work/c12" && "$FORGE" build >/dev/null 2>&1) \
    || fail "R12: build with a revision-less lock failed"
cmp -s "$work/c12/Forge.lock" "$work/c12/Forge.lock.orig" \
    || fail "R12: build rewrote a current revision-less lock"
(cd "$work/c12" && "$FORGE" build --locked >/dev/null 2>&1) \
    || fail "R12: --locked should pass once the pin is current"
pass "R12 old lockfiles keep working"

# --- R9: http loopback (needs python3) ----------------------------------
if command -v python3 >/dev/null 2>&1; then
    port=8471
    (cd "$work/stub" && exec python3 -m http.server "$port" >/dev/null 2>&1) &
    server=$!
    trap 'kill $server 2>/dev/null' EXIT
    sleep 1
    make_consumer "$work/c9" 'int main(void) { return 0; }'
    # A bare-bones file server has no /api/* routes, so the query 404s —
    # which still proves loopback http passed the transport policy (a
    # policy refusal names the transport instead of attempting curl).
    if (cd "$work/c9" && FORGE_ALLOW_UNSAFE_REGISTRY= FORGE_REGISTRY_URL="http://127.0.0.1:$port" "$FORGE" add x --registry hello_c --version 0.1.0 2>&1 | grep -qi "does not allow\|outside localhost"); then
        fail "R9: loopback http should pass the transport policy"
    fi
    kill $server 2>/dev/null
    trap - EXIT
    pass "R9 loopback http passes policy (server 404s as expected)"
else
    echo "skip: R9 (no python3 for the loopback stub)"
fi

echo "registry regression: all scenarios passed"
