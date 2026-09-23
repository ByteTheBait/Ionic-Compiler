# Ionic

A statically-typed, self-hosting compiled language targeting native ARM64 (macOS). Ionic compiles directly to Mach-O object files — no LLVM required at runtime — and enforces hardware placement at the type level: `tensor@cpu` and `tensor@gpu` are distinct types and the compiler rejects code that crosses the boundary without an explicit transfer.

```ionic
fn fibonacci(int64 n) -> int64 {
    if (n <= 1) { return n; }
    mut a = 0; mut b = 1; mut i = 2;
    while (i <= n) { let c = a + b; a = b; b = c; i = i + 1; }
    return b;
}

mut i = 0;
while (i <= 10) {
    println(format("fib({}) = {}", int64_to_str(i), int64_to_str(fibonacci(i))));
    i = i + 1;
}
```

```
$ ./ionic_new fib.ionic -o fib && ./fib
fib(0) = 0
fib(1) = 1
fib(2) = 1
fib(3) = 2
...
fib(10) = 55
```

---

## Features

- **Self-hosting** — `ionic_new` is compiled by itself; the entire compiler is written in Ionic
- **Direct ARM64 code generation** — emits Mach-O `.o` files directly, linked with `ld`; no LLVM at runtime
- **Static types, inferred where obvious** — `let x = 42` is `int64`; `let f = 3.14` is `float64`
- **Full float64 support** — arithmetic, `sqrt`, `pow`, `floor`, `ceil`, `fabs`, `int64_to_float64`, `float64_to_str`
- **Rich string builtins** — `format`, `str_concat`, `str_len`, `str_slice`, `str_replace`, `str_contains`, `str_starts_with`, `str_ends_with`, `int64_to_str`
- **Arrays** — `[int64]`/`[float64]`/`[string]` types, `.push`, `.len`, `arr_reset`, indexing and element assignment; element types are tracked through the checker
- **Import system** — `import std.math.*;` and selective `import std.str.{contains, trim};` pull in standard-library modules transitively with dedup
- **Standard library** — `std.math`, `std.str`, `std.array`, `std.io`, `std.data`, `std.text`, `std.http` live under `lib/std/`
- **Web request primitives** — `http_get`, `http_post`, `http_status`, `http_body`, `http_urlencode` (HTTP/1.1 client via raw sockets, no TLS) plus a `std.http` wrapper module
- **Hardware-aware types** — `tensor@cpu` and `tensor@gpu` prevent accidental cross-device ops
- **Real ML backends** — GGUF models via llama.cpp with Metal GPU; ONNX/CoreML; Piper TTS
- **Human-readable errors** — multi-error reporting, source-line carets, column tracking, panic-mode recovery
- **Optimizer** — a self-hosted optimization pipeline (fold, inline, unroll, const-propagate) that runs on every compilation before codegen
- **Modern syntax** — compound assignment (`+=`, `-=`, `*=`, `/=`), block comments `/* ... */`, and panic-mode lexer recovery

---

## Build

### Prerequisites

- Rust toolchain + `cargo` (bootstrap only — not needed once `ionic_self` exists)
- Clang / `ld` (for linking)

### Quick start

```sh
git clone <repo>
cd AILANG
cargo build --release          # builds the bootstrap Rust compiler
./build.sh --bootstrap         # compiles ionic_self (Ionic→ARM64) and ionic_new
```

After that, `ionic_self` and `ionic_new` are both native ARM64 binaries. `ionic_new` is the primary compiler.

### Rebuild after source changes

```sh
./build.sh          # recompile ionic_new from split sources using ionic_self (~3s)
```

---

## Usage

```
./ionic_new <file.ionic> -o <output>
```

**Compile and run:**
```sh
./ionic_new hello.ionic -o hello && ./hello
```

**No `fn main` needed** — top-level statements run directly:
```ionic
let x = 6 * 7;
println(int64_to_str(x));   // prints 42
```

---

## Language quick reference

### Variables

```ionic
let x = 42;           // immutable int64
mut count = 0;        // mutable int64
let pi = 3.14159;     // float64
let msg = "hello";    // string
```

### Functions

```ionic
fn gcd(int64 a, int64 b) -> int64 {
    mut aa = a; mut bb = b;
    while (bb != 0) { let t = bb; bb = aa - (aa / bb) * bb; aa = t; }
    return aa;
}
```

### Control flow

```ionic
if (x > 0) { println("positive"); }
else { println("non-positive"); }

while (i < 10) { i = i + 1; }
```

### Arrays

```ionic
mut data = [0]; arr_reset(data);
data.push(10); data.push(20); data.push(30);
println(int64_to_str(data.len));     // 3
println(int64_to_str(data[1]));      // 20
data[1] = 99;
```

### String formatting

```ionic
let s = format("x={}, y={}", int64_to_str(x), float64_to_str(y));
println(s);
```

### Float math

```ionic
let pi = 3.14159265358979;
println(float64_to_str(sqrt(2.0)));          // 1.41421
println(float64_to_str(pow(pi, 2.0)));        // 9.8696
println(float64_to_str(int64_to_float64(n))); // cast int→float
```

### Compound assignment

```ionic
mut x = 10;
x += 5;    // x = 15
x -= 3;    // 12
x *= 2;    // 24
x /= 4;    // 6
```

### Block comments

```ionic
/* line one
   line two */
let y = 1 + 2;   // 3
```

### Imports & the standard library

Import a whole module with a wildcard, or select specific symbols:

```ionic
import std.math.*;                      // everything in lib/std/math.ionic
import std.str.{contains, trim};        // just those two functions
import std.array.*;
import std.io.*;
```

The import resolver (`src/imports.ionic`) loads the referenced module files from
`lib/` during compilation, transitively expanding any imports inside them, and
deduplicates so each module is included exactly once. Imported functions and
constants become ordinary program declarations.

Available std modules: `std.math` (constants + float helpers), `std.str`
(string utils), `std.array` (array utils over `[int64]`/`[float64]`),
`std.io` (readline, eprint, print_hr), `std.data` (lower/upper/clean,
normalize_ws, CSV parse/split, mean/stdev/min/max over `[float64]`,
to_csv_line), `std.text` (tokenize, words, count_substr, replace_all, join),
and `std.http` (web client: get / post / status / body / urlencode over the
raw-socket HTTP/1.1 runtime).

```ionic
import std.http.*;
let raw = get("http://example.com/");
if (status(raw) == 200) {
    let html = body(raw);
    println(int64_to_str(str_len(html)));
}
```

---

## Optimizations

`src/opt/opt.ionic` is a self-hosted, single-pass optimizer that runs on every
compilation between semantic checking and codegen. It rewrites the AST in place
and always preserves the stable self-hosting fixed point — the compiler is able
to optimize its own source.

- **Constant folding + algebraic simplification + DCE** — folds integer ops on
  constant operands (including comparisons and bitwise ops), simplifies
  `x + 0`, `x * 1`, `x / 1`, `x | 0`, `x << 0`, folds unary neg/not/bitnot,
  eliminates dead `if` branches on constant conditions and truncates unreachable
  code after `return`.
- **Float constant folding** — folds `+ - * /` on float (or int-promoted)
  literals and the single-arg builtins `sqrt`, `fabs`, `floor`, `ceil`, plus
  float negation, computing IEEE-754 bit patterns at compile time so the emitted
  object contains no runtime float calls.
- **Loop unrolling** — flattens `for` loops whose bounds are compile-time
  integer constants and whose trip count is small (`≤ 8`), with no
  `break`/`continue`, by deeply cloning the body per iteration and substituting
  the induction variable with its literal index.
- **Function inlining** — inlines "leaf" pure functions (a body that is exactly
  a single `return <expr>` with no calls): parameters are substituted by the
  optimized call-site arguments and the resulting expression is re-optimized.
  Recursion and multi-statement functions are left intact.
- **Scoped constant propagation (mem2reg subset)** — locally replaces references
  to names declared `let x = <literal>` and never reassigned anywhere in the
  function with their literal values, eliminating redundant store/loads so later
  expressions can fold further.

Combined, these passes work together — e.g. a constant `let` is propagated into
arguments so a leaf call inlines, and the substituted body then folds; an
inlined small loop body inside a constant-trip `for` is unrolled. Folding is
always conservative, so optimization never changes program semantics.

---

## Project layout

```
src/
  codegen/         Code generator (ARM64 Mach-O emitter) — Ionic source
  parser/          Parser — Ionic source
  lexer/           Lexer — Ionic source
  semantic/        Type checker — Ionic source
  main.ionic       Compiler entry point
  imports.ionic    Import resolver (loads std modules, transitive dedup)
  diagnostics.ionic  Error reporting
  compiler.ionic   Monolithic source (bootstrap only)
  codegen.ionic    Monolithic codegen (bootstrap only)
  codegen.rs       Rust LLVM backend (bootstrap only)
  lexer.rs / parser.rs / semantic.rs   Rust frontend (bootstrap only)
  ionic_model_runtime.c   Native runtime: I/O, arrays, strings, math, ML
build.sh           Build script (--bootstrap for full rebuild)
ionic_new          Primary compiler binary (self-hosted)
ionic_self         Previous-generation compiler (used to build ionic_new)
```

---

## Self-hosting cycle

```
Rust compiler ──bootstrap──> ionic_self
ionic_self    ──build──────> ionic_new
ionic_new     ──build──────> ionic_new  (stable fixed point)
```

The Rust source (`src/*.rs`) is only needed for the initial bootstrap. Once `ionic_self` exists it is not required again unless you change the bootstrap compiler.
