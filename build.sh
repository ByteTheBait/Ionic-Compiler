#!/bin/sh
# Build the Ionic compiler from the split source tree.
#
# Usage:
#   ./build.sh                       — build ionic_new using ionic_self
#                                       (the committed bootstrap binary that
#                                        ships with the repo). Works on any
#                                        OS without any toolchain installed.
#   ./build.sh --verify-self-hosting — build ionic_new, then verify it
#                                       rebuilds itself byte-for-byte (catches
#                                       regressions in the self-hosted code
#                                       path). Exits 0 on match, 1 otherwise.
#   IONIC=./some_binary ./build.sh   — use a specific Ionic binary instead of
#                                       the default ./ionic_self
#
# End users install the released tarball (./ionic + lib/std). To rebuild from
# source they just need ./ionic_self from the same tarball; no toolchain
# required.
#
# The Rust source tree (src/*.rs + Cargo.toml) is for the dev-only Rust
# compiler. It's used by CI as a cross-OS bootstrap — `cargo build --release`
# produces target/release/ionic which can compile user programs on any OS
# without needing the committed ionic_self. But target/release/ionic is
# missing bitwise operator support (it predates that addition), so it
# CANNOT compile the split .ionic sources themselves — use ionic_self for
# that. End users don't need Rust or cargo; releases ship self-hosted.

set -e

IONIC="${IONIC:-./ionic_self}"
OUT="${OUT:-ionic_new}"

SOURCES="
  src/lexer/tokens.ionic
  src/lexer/lexer.ionic
  src/diagnostics.ionic
  src/parser/ast.ionic
  src/parser/parser.ionic
  src/imports.ionic
  src/semantic/checker.ionic
  src/opt/opt.ionic
  src/codegen/native.ionic
  src/codegen/elf.ionic
  src/main.ionic
"

# ── Mode: build then verify self-hosting ────────────────────────────────────
# First build ionic_new via whatever IONIC points at (default: ionic_self),
# then run ionic_new to rebuild itself into a temp file and `cmp -s` compare.
# Exits non-zero if the second build differs — catches determinism bugs and
# self-hosting regressions in one shot.
if [ "$1" = "--verify-self-hosting" ]; then
    echo "==> Building $OUT from split source using $IONIC..."
    $IONIC $SOURCES -o "$OUT"
    echo "==> Verifying self-hosting: $OUT rebuilds itself byte-identical..."
    cp "$OUT" /tmp/_ionic_verify_a
    ./"$OUT" $SOURCES -o /tmp/_ionic_verify_b
    if cmp -s /tmp/_ionic_verify_a /tmp/_ionic_verify_b; then
        echo "    ✓ byte-identical"
    else
        echo "    ✗ DIFFER — self-hosting is BROKEN"
        echo "    Run: diff <(xxd /tmp/_ionic_verify_a) <(xxd /tmp/_ionic_verify_b) | head"
        exit 1
    fi
    echo "==> Done: $OUT"
    exit 0
fi

# ── Default: build via existing ionic (self-hosted loop) ────────────────────
echo "==> Compiling $OUT from split source using $IONIC..."
$IONIC $SOURCES -o "$OUT"
echo "==> Done: $OUT"
