#!/bin/sh
# Build the Ionic compiler from the split source tree.
#
# Usage:
#   ./build.sh                       — build ionic_new using ionic_self
#                                       (the committed bootstrap binary that
#                                        ships with the repo). Works on any
#                                        OS without any toolchain installed.
#   ./build.sh --bootstrap-rust      — alternative path: cargo build --release,
#                                       then use target/release/ionic to build
#                                       ionic_new. This is the CI/Linux path:
#                                       works from a clean checkout with only
#                                       Rust stable + clang available, no need
#                                       for the committed ionic_self binary.
#   ./build.sh --verify-self-hosting — build ionic_new using the default path,
#                                       then verify it rebuilds itself
#                                       byte-for-byte (catches regressions in
#                                       the self-hosted code path).
#   IONIC=./some_binary ./build.sh   — use a specific Ionic binary instead of
#                                       the default ./ionic_self
#
# End users install the released tarball (./ionic + lib/std). To rebuild from
# source they just need ./ionic_self from the same tarball; no Rust toolchain
# required.
#
# The Rust bootstrap is for development / cross-OS CI only.

set -e

IONIC="${IONIC:-./ionic_self}"
OUT="${OUT:-ionic_new}"
RUST_IONIC="${RUST_IONIC:-target/release/ionic}"

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

# ── Mode: bootstrap via Rust ────────────────────────────────────────────────
# The Rust compiler (cargo build --release) only accepts a single source file.
# Concatenate the split sources into one big file, build via cargo, then use
# the resulting compiler to produce ionic_new. Useful for fresh clones on
# Linux ARM64 / Windows where the user doesn't have an ionic_self binary yet.
if [ "$1" = "--bootstrap-rust" ]; then
    echo "==> Bootstrapping via Cargo..."
    cargo build --release

    COMBINED="/tmp/_ionic_combined.ionic"
    : > "$COMBINED"
    for src in $SOURCES; do
        cat "$src" >> "$COMBINED"
        printf '\n' >> "$COMBINED"
    done

    echo "==> Compiling $OUT from concatenated split source..."
    "$RUST_IONIC" "$COMBINED" -o "$OUT"
    echo "==> Done: $OUT"
    exit 0
fi

# ── Mode: build then verify self-hosting ────────────────────────────────────
# First build ionic_new via whatever IONIC points at (default: ionic_self),
# then run ionic_new to rebuild itself and compare to the first build.
# Exits non-zero if the second build differs from the first — this catches
# determinism bugs and self-hosting regressions in one shot.
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
