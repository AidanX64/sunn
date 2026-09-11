#!/usr/bin/env bash
#
# Regression tests for general engine fixes:
#
#   R1  a [dependencies] inline table without path/git is a clean parse
#       error instead of crashing (it used to dereference NULL)
#   R2  -v detail lines render their formatted numbers correctly (the
#       terminal echo reused an already-consumed va_list on POSIX ABIs)
#   R3  depfiles written under paths containing spaces ("dir\ with\ spaces")
#       keep incremental builds incremental instead of recompiling forever
#   R4  a C++ path dependency links through the C++ driver even when the
#       consuming project is pure C
#   R5  a dependency whose own sources include headers from ITS dependencies
#       compiles (its sub-build receives those include directories)
#   R6  a [build] compiler override whose path merely CONTAINS "cl" stays a
#       GNU driver (POSIX only: needs symlinks)
#
# Like deps-regression.sh, everything runs inside a throwaway sandbox.
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

work="$(mktemp -d)"
if command -v cygpath >/dev/null 2>&1; then
    work_forge="$(cygpath -m "$work")"   # C:/... form for manifest strings
else
    work_forge="$work"
fi

export FORGE_HOME="$work_forge/home"

fail() {
    echo "FAIL: $*" >&2
    rm -rf "$work"
    exit 1
}
pass() { echo "ok - $*"; }

latest_log() { # <project-dir>
    ls -1t "$1/target/logs/"*.log | head -n 1
}

write_manifest() { # <path> <project-name> [extra sections passed on stdin appended]
    {
        echo '[project]'
        echo "name = \"$2\""
        echo ''
        echo '[sources]'
        echo 'c = ["src"]'
        echo 'cpp = []'
        echo 'asm = []'
        echo ''
        echo '[targets]'
        echo 'os = ["windows", "linux", "macos"]'
        echo 'arch = ["x86_64", "aarch64"]'
        if [ "$#" -gt 2 ]; then
            echo ''
            shift 2
            printf '%s\n' "$@"
        fi
    } >"$1"
}

# ----------------------------------------------------------------------
# R1: a dependency table with neither path nor git is a clean parse error
# ----------------------------------------------------------------------
proj="$work/r1"
mkdir -p "$proj/src"
write_manifest "$proj/Forge.toml" "r1"
printf '\n[dependencies]\nbroken = { tag = "v1" }\n' >>"$proj/Forge.toml"
echo 'int main(void) { return 0; }' >"$proj/src/main.c"

out="$("$FORGE" check --manifest "$proj/Forge.toml" 2>&1)"; rc=$?
[ "$rc" -ne 0 ] || fail "R1: a source-less dependency table was accepted"
case "$out" in
    *"needs exactly one source"*) : ;;
    *) fail "R1: refusal did not name the cause: $out" ;;
esac
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT) : ;;
    *) [ "$rc" -lt 128 ] || fail "R1: forge died on a signal (rc=$rc)" ;;
esac
pass "R1: source-less dependency tables are a clean parse error"

# ----------------------------------------------------------------------
# R2: -v renders formatted detail lines correctly
# ----------------------------------------------------------------------
proj="$work/r2"
mkdir -p "$proj/src"
write_manifest "$proj/Forge.toml" "r2"
echo 'int main(void) { return 0; }' >"$proj/src/main.c"

"$FORGE" build -v --manifest "$proj/Forge.toml" >"$work/r2.out" 2>&1 \
    || fail "R2: verbose build failed"
grep -q "compiled 1, up-to-date 0" "$work/r2.out" \
    || fail "R2: verbose counters are garbled: $(tail -n 5 "$work/r2.out")"
grep -q -- "----- compile 1 source file(s)" "$work/r2.out" \
    || fail "R2: verbose stage header is garbled"
pass "R2: verbose detail lines keep their formatted values"

# ----------------------------------------------------------------------
# R3: escaped spaces in depfiles keep incremental builds incremental
# ----------------------------------------------------------------------
proj="$work/dir with spaces/r3"
mkdir -p "$proj/src"
write_manifest "$proj/Forge.toml" "r3"
cat >"$proj/src/greet.h" <<'EOF'
int greet_value(void);
EOF
cat >"$proj/src/greet.c" <<'EOF'
#include "greet.h"
int greet_value(void) { return 7; }
EOF
cat >"$proj/src/main.c" <<'EOF'
#include <stdio.h>
#include "greet.h"
int main(void) { printf("%d\n", greet_value()); return 0; }
EOF

"$FORGE" build --manifest "$proj/Forge.toml" >/dev/null 2>&1 \
    || fail "R3: build under a spaced path failed"
count="$(grep -c '\[compile\] source:' "$(latest_log "$proj")")"
[ "$count" -ge 2 ] || fail "R3: sanity: expected both TUs compiled, saw $count"

"$FORGE" build --manifest "$proj/Forge.toml" >/dev/null 2>&1 \
    || fail "R3: second build under a spaced path failed"
count="$(grep -c '\[compile\] source:' "$(latest_log "$proj")")"
[ "$count" -eq 0 ] \
    || fail "R3: spaced-path depfiles force recompiles ($count TU(s) rebuilt)"
pass "R3: projects under spaced paths stay incremental"

# ----------------------------------------------------------------------
# R4: a C++ path dependency links through the C++ driver from a C main
# ----------------------------------------------------------------------
cpplib="$work/cpp-lib"
mkdir -p "$cpplib/src"
write_manifest "$cpplib/Forge.toml" "cpplib"
replace_in_file 's/^cpp = \[\]$/cpp = ["src"]/' "$cpplib/Forge.toml"
cat >"$cpplib/src/lib.cpp" <<'EOF'
extern "C" int cpp_slot(void)
{
    int *slot = new int(41);
    *slot += 1;
    int value = *slot;
    delete slot;
    return value;
}
EOF

proj="$work/r4"
mkdir -p "$proj/src"
write_manifest "$proj/Forge.toml" "r4" \
    "[dependencies]" \
    "cpplib = { path = \"$work_forge/cpp-lib\" }"
cat >"$proj/src/main.c" <<'EOF'
extern int cpp_slot(void);
int main(void) { return cpp_slot() == 42 ? 0 : 1; }
EOF

"$FORGE" run --manifest "$proj/Forge.toml" >/dev/null 2>&1 \
    || fail "R4: C main + C++ path dependency failed to build or run"
pass "R4: C++ dependencies flip the link driver for pure-C consumers"

# ----------------------------------------------------------------------
# R5: a dependency that includes headers from its own dependencies compiles
# ----------------------------------------------------------------------
low="$work/lib-low"
mkdir -p "$low/include" "$low/src"
write_manifest "$low/Forge.toml" "liblow"
printf 'int low_value(void);\n' >"$low/include/low.h"
cat >"$low/src/low.c" <<'EOF'
#include "low.h"
int low_value(void) { return 5; }
EOF

mid="$work/lib-mid"
mkdir -p "$mid/src"
write_manifest "$mid/Forge.toml" "libmid" \
    "[dependencies]" \
    "low = { path = \"$work_forge/lib-low\" }"
cat >"$mid/src/mid.c" <<'EOF'
#include "low.h"
int mid_value(void) { return low_value() * 2; }
EOF

proj="$work/r5"
mkdir -p "$proj/src"
write_manifest "$proj/Forge.toml" "r5" \
    "[dependencies]" \
    "mid = { path = \"$work_forge/lib-mid\" }"
cat >"$proj/src/main.c" <<'EOF'
extern int mid_value(void);
int main(void) { return mid_value() == 10 ? 0 : 1; }
EOF

"$FORGE" run --manifest "$proj/Forge.toml" >/dev/null 2>&1 \
    || fail "R5: chained path dependencies lost their transitive headers"
pass "R5: dependency sub-builds receive their own dependencies' headers"

# ----------------------------------------------------------------------
# R6: a compiler override path containing "cl" stays a GNU driver (POSIX)
# ----------------------------------------------------------------------
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        pass "R6: skipped (needs POSIX symlinks)"
        ;;
    *)
        bindir="$work/tools-with-cl"
        mkdir -p "$bindir"
        host_cc="$(command -v gcc || command -v clang || command -v cc)" \
            || fail "R6: no host compiler found to alias"
        # Keep "cl" only in the parent path: Apple Clang's driver mode can
        # vary when invoked through a name ending in "gcc".
        ln -s "$host_cc" "$bindir/local-cluster-cc"

        proj="$work/r6"
        mkdir -p "$proj/src"
        write_manifest "$proj/Forge.toml" "r6" \
            "[build]" \
            "compiler = \"$bindir/local-cluster-cc\""
        echo 'int main(void) { return 0; }' >"$proj/src/main.c"

        "$FORGE" check --manifest "$proj/Forge.toml" >/dev/null 2>&1 \
            || fail "R6: override path containing 'cl' was misclassified as MSVC"
        grep -qF '(gcc)' "$(latest_log "$proj")" \
            || fail "R6: dispatch did not report a GCC-classified driver"
        pass "R6: 'cl' substrings in override paths no longer imply MSVC"
        ;;
esac

echo "all engine regressions passed"
rm -rf "$work"
