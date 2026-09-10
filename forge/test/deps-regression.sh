#!/usr/bin/env bash
#
# Regression tests for the Phase 1 dependency fixes:
#
#   K1  git URL allowlist: ext:: transports and option-shaped strings are
#       refused before git runs, at `forge add` time and at resolve time.
#   K2  a cold dependency cache honors the manifest ref / Forge.lock pin;
#       a moved remote default branch cannot silently bypass the lock.
#   K4  bare `forge update` re-resolves every dependency, while
#       `forge update NAME` moves only NAME past its pin.
#
# and the Phase 2 integrity/safety fixes:
#
#   S2  a hand-tampered Forge.lock commit pin is refused loudly instead of
#       being fed to `git checkout`.
#   S3  a dependency fetched into the shared cache may not use path
#       dependencies that escape its own checkout.
#   M5  two declarations of one dependency name pointing at different
#       sources conflict instead of first-one-wins.
#
# The scenarios are fully self-contained: temporary upstream repositories
# are cloned through plain local paths, which is why the script sets
# FORGE_ALLOW_UNSAFE_GIT=1 (exactly the opt-out real users would use).
set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"

case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) FORGE="$root/build/forge.exe" ;;
    *)                    FORGE="$root/build/forge" ;;
esac
[ -x "$FORGE" ] || { echo "forge binary missing; run make first ($FORGE)" >&2; exit 1; }

# MSYS2 shells may lack git entirely (it lives in /mingw64/bin or in a
# separate Git-for-Windows install); extend PATH before giving up.
if ! command -v git >/dev/null 2>&1; then
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            PATH="/mingw64/bin:/c/Program Files/Git/cmd:$PATH"
            ;;
    esac
fi
command -v git >/dev/null 2>&1 || { echo "git is required for these tests" >&2; exit 1; }

work="$(mktemp -d)"
if command -v cygpath >/dev/null 2>&1; then
    work_forge="$(cygpath -m "$work")"   # C:/... form for forge + git URLs
else
    work_forge="$work"
fi

export FORGE_HOME="$work_forge/home"
export FORGE_ALLOW_UNSAFE_GIT=1

fail() {
    echo "FAIL: $*" >&2
    if ls target/logs/*.log >/dev/null 2>&1; then
        echo "--- last dependency log lines ---" >&2
        tail -n 12 "$(ls -1t target/logs/*.log | head -1)" >&2
    fi
    rm -rf "$work"
    exit 1
}
# Sections share $work (K1 reuses dep repos made in K4), so this must not
# exit or clean up — only the final line tears the sandbox down.
pass() {
    echo "ok - $*"
}

git_commit_all() { # <repo-dir> <message>
    git -C "$1" add -A
    git -C "$1" -c user.email=forge@example.com -c user.name=forge \
        commit -qm "$2" >/dev/null
}

make_dep_repo() { # <repo-dir> <project-name>
    mkdir -p "$1/src"
    printf '[project]\nname = "%s"\n\n[sources]\nc = ["src"]\ncpp = []\nasm = []\n\n[targets]\nos = ["windows", "linux", "macos"]\narch = ["x86_64", "aarch64"]\n' \
        "$2" >"$1/Forge.toml"
    echo "int lib_value(void) { return 1; }" >"$1/src/lib.c"
    git -C "$1" -c init.defaultBranch=master init -q
    git_commit_all "$1" "initial commit"
}

default_branch_of() {
    git -C "$1" symbolic-ref --short HEAD
}

write_project_manifest() { # <path> <project-name> [dependency-lines...]
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
            echo '[dependencies]'
            shift 2
            printf '%s\n' "$@"
        fi
    } >"$1"
}

# ----------------------------------------------------------------------
# K2: cold-cache clone honors the pinned tag
# ----------------------------------------------------------------------
dep="$work/dep-one"
make_dep_repo "$dep" "libone"
pinned="$(git -C "$dep" rev-parse HEAD)"
git -C "$dep" tag v1
echo "int moved_marker;" >>"$dep/src/lib.c"
git_commit_all "$dep" "move default tip past v1"
tip="$(git -C "$dep" rev-parse HEAD)"
[ "$pinned" != "$tip" ] || fail "setup: upstream default branch did not move"

proj="$work/proj-k2"
mkdir -p "$proj/src"
# Manifest URLs must be native paths: forge spawns git directly, so MSYS
# never translates /tmp/... on its behalf.
write_project_manifest "$proj/Forge.toml" "k2" \
    "libone = { git = \"$work_forge/dep-one\", tag = \"v1\" }"
echo 'int main(void) { return 0; }' >"$proj/src/main.c"
cd "$proj" || exit 1

"$FORGE" update >/dev/null 2>&1 \
    || fail "K2: initial warm-cache update failed"
grep -q "commit = \"$pinned\"" Forge.lock \
    || fail "K2: warm cache did not record the tag pin"

# Fresh machine / CI: wipe the shared cache entirely and re-resolve.
rm -rf "${FORGE_HOME:?}/git"
"$FORGE" update >/dev/null 2>&1 \
    || fail "K2: cold-cache update failed"
grep -q "commit = \"$pinned\"" Forge.lock \
    || fail "K2: cold cache checked out the moved default tip ($tip) instead of the pinned tag ($pinned)"
pass "K2: cold-cache clone honors the pinned tag"

# ----------------------------------------------------------------------
# K4: bare update re-resolves all; named update moves only its target
# ----------------------------------------------------------------------
dep_a="$work/dep-a"
dep_b="$work/dep-b"
make_dep_repo "$dep_a" "liba"
make_dep_repo "$dep_b" "libb"
branch_a="$(default_branch_of "$dep_a")"
branch_b="$(default_branch_of "$dep_b")"
base_a="$(git -C "$dep_a" rev-parse HEAD)"
base_b="$(git -C "$dep_b" rev-parse HEAD)"

proj="$work/proj-k4"
mkdir -p "$proj/src"
write_project_manifest "$proj/Forge.toml" "k4" \
    "liba = { git = \"$work_forge/dep-a\", branch = \"$branch_a\" }" \
    "libb = { git = \"$work_forge/dep-b\", branch = \"$branch_b\" }"
echo 'int main(void) { return 0; }' >"$proj/src/main.c"
cd "$proj" || exit 1

"$FORGE" update >/dev/null 2>&1 \
    || fail "K4: initial resolve failed"
grep -q "liba = .*commit = \"$base_a\"" Forge.lock \
    || fail "K4: liba was not pinned at its base commit"
grep -q "libb = .*commit = \"$base_b\"" Forge.lock \
    || fail "K4: libb was not pinned at its base commit"

# Advance both upstreams past the pins.
echo "int moved_a;" >>"$dep_a/src/lib.c"
git_commit_all "$dep_a" "advance a"
head_a="$(git -C "$dep_a" rev-parse HEAD)"
echo "int moved_b;" >>"$dep_b/src/lib.c"
git_commit_all "$dep_b" "advance b"
head_b="$(git -C "$dep_b" rev-parse HEAD)"

"$FORGE" update liba >/dev/null 2>&1 \
    || fail "K4: named update of liba failed"
grep -q "liba = .*commit = \"$head_a\"" Forge.lock \
    || fail "K4: 'update liba' did not move liba past its pin"
grep -q "libb = .*commit = \"$base_b\"" Forge.lock \
    || fail "K4: 'update liba' dragged libb along (or reset it)"
pass "K4: named update moves only the named dependency"

"$FORGE" update >/dev/null 2>&1 \
    || fail "K4: bare update failed"
grep -q "liba = .*commit = \"$head_a\"" Forge.lock \
    || fail "K4: bare update lost liba's resolution"
grep -q "libb = .*commit = \"$head_b\"" Forge.lock \
    || fail "K4: bare update did not move libb past its pin"
pass "K4: bare update re-resolves every dependency"

# ----------------------------------------------------------------------
# S2: a tampered Forge.lock commit pin is refused loudly
# ----------------------------------------------------------------------
dep="$work/dep-s2"
make_dep_repo "$dep" "libs2"

proj="$work/proj-s2"
mkdir -p "$proj/src"
write_project_manifest "$proj/Forge.toml" "s2" \
    "libs2 = { git = \"$work_forge/dep-s2\", branch = \"master\" }"
echo 'int main(void) { return 0; }' >"$proj/src/main.c"
cd "$proj" || exit 1

"$FORGE" update >/dev/null 2>&1 \
    || fail "S2: initial resolve failed"
pinned_s2="$(git -C "$dep" rev-parse HEAD)"
grep -q "commit = \"$pinned_s2\"" Forge.lock \
    || fail "S2: pin was not recorded"

sed -i "s/commit = \"$pinned_s2\"/commit = \"deadbeefdeadbeefdeadbeefdeadbeefdeadbe\"/" Forge.lock
if "$FORGE" build >"$work/out.txt" 2>&1; then
    fail "S2: tampered commit pin was accepted"
fi
grep -q "malformed commit pin" "$work/out.txt" \
    || fail "S2: tampered lock failed without naming the cause (see $work/out.txt)"
pass "S2: tampered lockfile commit pins are refused"

# ----------------------------------------------------------------------
# S3: cached dependencies may not path-dep out of their checkout
# ----------------------------------------------------------------------
# The escape target is planted INSIDE the shared cache tree, next to where
# cloned checkouts live: it must exist for resolution to reach the
# containment gate instead of failing earlier with "does not exist".
mkdir -p "$work/home/git/outside-lib/src"
write_project_manifest "$work/home/git/outside-lib/Forge.toml" "outside"
echo "int outside_value(void) { return 1; }" >"$work/home/git/outside-lib/src/lib.c"

dep="$work/dep-outer"
make_dep_repo "$dep" "libouter"
{
    echo ''
    echo '[dependencies]'
    echo "outside = { path = \"../outside-lib\" }"
} >>"$dep/Forge.toml"
git_commit_all "$dep" "add escaping path dependency"

dep_inner="$work/dep-inner"
make_dep_repo "$dep_inner" "libinner"
mkdir -p "$dep_inner/sub/src"
write_project_manifest "$dep_inner/sub/Forge.toml" "inside"
echo "int inside_value(void) { return 2; }" >"$dep_inner/sub/src/lib.c"
{
    echo ''
    echo '[dependencies]'
    echo "inside = { path = \"./sub\" }"
} >>"$dep_inner/Forge.toml"
git_commit_all "$dep_inner" "add contained path dependency"

proj="$work/proj-s3"
mkdir -p "$proj/src"
write_project_manifest "$proj/Forge.toml" "s3" \
    "libouter = { git = \"$work_forge/dep-outer\", branch = \"master\" }"
echo 'int main(void) { return 0; }' >"$proj/src/main.c"
cd "$proj" || exit 1

if "$FORGE" update >"$work/out.txt" 2>&1; then
    fail "S3: cache escape via ../ path dependency was allowed"
fi
grep -q "outside its own checkout" "$work/out.txt" \
    || fail "S3: escape failed without naming the cause (see $work/out.txt)"

# Positive control: a path dependency that stays inside the checkout works.
rm -f Forge.lock
write_project_manifest "$proj/Forge.toml" "s3b" \
    "libinner = { git = \"$work_forge/dep-inner\", branch = \"master\" }"
"$FORGE" update >/dev/null 2>&1 \
    || fail "S3: contained ./sub path dependency was rejected"
pass "S3: escaping path dependencies refused, contained ones still work"

# ----------------------------------------------------------------------
# M5: one name pointing at two different sources is an error
# ----------------------------------------------------------------------
dep_a="$work/dep-m5a"
dep_b="$work/dep-m5b"
make_dep_repo "$dep_a" "sharedm5"
make_dep_repo "$dep_b" "sharedm5too"

consumer="$work/lib-consumer"
make_dep_repo "$consumer" "consumer"
{
    echo ''
    echo '[dependencies]'
    echo "sharedm5 = { git = \"$work_forge/dep-m5b\", branch = \"master\" }"
} >>"$consumer/Forge.toml"
git_commit_all "$consumer" "declare conflicting shared dependency"

proj="$work/proj-m5"
mkdir -p "$proj/src"
write_project_manifest "$proj/Forge.toml" "m5" \
    "sharedm5 = { git = \"$work_forge/dep-m5a\", branch = \"master\" }" \
    "consumer = { path = \"$work_forge/lib-consumer\" }"
echo 'int main(void) { return 0; }' >"$proj/src/main.c"
cd "$proj" || exit 1

if "$FORGE" update >"$work/out.txt" 2>&1; then
    fail "M5: conflicting sources for one name were accepted silently"
fi
grep -q "declared twice with conflicting sources" "$work/out.txt" \
    || fail "M5: conflict failed without naming the cause (see $work/out.txt)"

# Positive control: same name from the SAME source stays a legal diamond.
rm -f Forge.lock
write_project_manifest "$proj/Forge.toml" "m5b" \
    "sharedm5 = { git = \"$work_forge/dep-m5a\", branch = \"master\" }" \
    "consumer = { path = \"$work_forge/lib-consumer\" }"
sed -i "s|dep-m5b|dep-m5a|" "$consumer/Forge.toml"
git_commit_all "$consumer" "agree on the shared source"
"$FORGE" update >/dev/null 2>&1 \
    || fail "M5: agreeing diamond was rejected"
pass "M5: conflicting dep identities refused, agreeing diamonds still dedupe"

# ----------------------------------------------------------------------
# K1: hostile git URLs are refused before git ever runs
# ----------------------------------------------------------------------
proj="$work/proj-k1"
mkdir -p "$proj/src"
write_project_manifest "$proj/Forge.toml" "k1"
echo 'int main(void) { return 0; }' >"$proj/src/main.c"
cd "$proj" || exit 1

if env -u FORGE_ALLOW_UNSAFE_GIT "$FORGE" add evil \
        --git 'ext::sh -c touch%20pwned' >/dev/null 2>&1; then
    fail "K1: ext:: transport URL was accepted by 'forge add'"
fi
[ ! -e pwned ] || fail "K1: ext:: URL executed a shell command"
grep -q evil Forge.toml \
    && fail "K1: rejected ext:: dependency was still written to the manifest"

if env -u FORGE_ALLOW_UNSAFE_GIT "$FORGE" add shady \
        --git '--upload-pack=/bin/sh' >/dev/null 2>&1; then
    fail "K1: option-shaped git URL was accepted"
fi
grep -q shady Forge.toml \
    && fail "K1: rejected option-shaped dependency was written to the manifest"

if env -u FORGE_ALLOW_UNSAFE_GIT "$FORGE" add sneaky \
        --git 'file:///etc/passwd' >/dev/null 2>&1; then
    fail "K1: file:// URL was accepted"
fi

# Positive control: the documented escape hatch makes local-path clones work.
"$FORGE" add libb --git "$dep_b" >/dev/null 2>&1 \
    || fail "K1: FORGE_ALLOW_UNSAFE_GIT=1 local-path add failed"
grep -q "git = " Forge.toml \
    || fail "K1: successful add left no dependency line behind"
pass "K1: hostile URLs rejected, escape hatch works"

# ----------------------------------------------------------------------
# K7: naming a path dependency updates nothing else (and works offline)
# ----------------------------------------------------------------------
dep="$work/dep-k7"
make_dep_repo "$dep" "libk"
branch_k="$(default_branch_of "$dep")"
base_k="$(git -C "$dep" rev-parse HEAD)"

pathlib="$work/k7-pathlib"
mkdir -p "$pathlib"
write_project_manifest "$pathlib/Forge.toml" "k7lib"

proj="$work/proj-k7"
mkdir -p "$proj/src"
write_project_manifest "$proj/Forge.toml" "k7" \
    "libk = { git = \"$work_forge/dep-k7\", branch = \"$branch_k\" }" \
    "plib = { path = \"$work_forge/k7-pathlib\" }"
echo 'int main(void) { return 0; }' >"$proj/src/main.c"
cd "$proj" || exit 1

"$FORGE" update >/dev/null 2>&1 \
    || fail "K7: initial resolve failed"
grep -q "libk = .*commit = \"$base_k\"" Forge.lock \
    || fail "K7: initial pin for libk is missing"

echo "int moved_k;" >>"$dep/src/lib.c"
git_commit_all "$dep" "advance k past the pin"

"$FORGE" update plib >/dev/null 2>&1 \
    || fail "K7: named update of a path dependency failed"
grep -q "libk = .*commit = \"$base_k\"" Forge.lock \
    || fail "K7: updating a path dependency dragged libk past its pin"

"$FORGE" update plib --offline >/dev/null 2>&1 \
    || fail "K7: named path-dependency update must work offline"
pass "K7: named path-dependency updates touch nothing else, even offline"

echo "all dependency regressions passed"
rm -rf "$work"
