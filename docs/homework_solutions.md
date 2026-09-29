# Compiler Homework — Complete Solutions
## Ionic Language: Dragon Book + TAPL/ATAPL Mapping

---

## Part 1: Lexer

### HW 1.1 — Expression Arithmetic Tokenizer

The Ionic lexer is implemented in `src/lexer/lexer.ionic`. It uses a single-pass, character-by-character
approach with one-character lookahead (`lx_peek2`). All tokens are pushed into five parallel global
arrays: `g_tok_kinds`, `g_tok_ivals`, `g_tok_svals`, `g_tok_lines`, `g_tok_cols`.

**Token stream for:** `fn dot(tensor@cpu w, tensor@cpu x) -> float64 { ... }`

| # | Token Kind (constant) | Numeric | String Value | Line | Col |
|---|----------------------|---------|--------------|------|-----|
| 0 | TOK_FN | 3 | "fn" | 1 | 1 |
| 1 | TOK_IDENT | 29 | "dot" | 1 | 4 |
| 2 | TOK_LPAREN | 51 | | 1 | 7 |
| 3 | TOK_KW_TENSOR | 22 | "tensor" | 1 | 8 |
| 4 | TOK_AT_CPU | 25 | "cpu" | 1 | 15 |
| 5 | TOK_IDENT | 29 | "w" | 1 | 19 |
| 6 | TOK_COMMA | 50 | | 1 | 20 |
| 7 | TOK_KW_TENSOR | 22 | "tensor" | 1 | 22 |
| 8 | TOK_AT_CPU | 25 | "cpu" | 1 | 29 |
| 9 | TOK_IDENT | 29 | "x" | 1 | 33 |
| 10 | TOK_RPAREN | 52 | | 1 | 34 |
| 11 | TOK_ARROW | 45 | | 1 | 36 |
| 12 | TOK_KW_FLOAT64 | 19 | "float64" | 1 | 39 |
| 13 | TOK_LBRACE | 53 | | 1 | 47 |
| 14 | TOK_DOT_DOT | 47 | | 1 | 49 |
| 15 | TOK_DOT | 46 | | 1 | 51 |
| 16 | TOK_RBRACE | 54 | | 1 | 53 |
| 17 | TOK_EOF | 57 | | 1 | 54 |

**Key observations about the implementation:**

- `tensor@cpu` tokenizes as `TOK_KW_TENSOR` then `TOK_AT_CPU` — the `@` triggers a separate
  branch in `lx_advance` that reads the following identifier and maps `"gpu"` → `TOK_AT_GPU`,
  `"cpu"` → `TOK_AT_CPU`.
- Keywords are recognized by `keyword_kind(string)` which does exact-match string comparisons
  after reading the full identifier — satisfying TAPL's requirement that lexing is a
  *first-order syntax* transformation with no ambiguity.
- The lexer never emits a token for `@` itself; it is consumed as part of the hardware annotation.
- `->` is a two-character token: `CH_MINUS` followed by `CH_GT` produces `TOK_ARROW` (longest match).

**Regex patterns (Dragon Book Ch. 2 notation):**

```
KEYWORD     := let|mut|fn|if|else|while|for|return|break|continue|struct|...
INT_LIT     := [0-9]+  |  0[xX][0-9a-fA-F]+
FLOAT_LIT   := [0-9]+\.[0-9]+
IDENT       := [a-zA-Z_][a-zA-Z0-9_]*
STRING_LIT  := "[^"\\]*(\\[nt"\\][^"\\]*)*"
LINE_CMT    := \/\/[^\n]*
BLOCK_CMT   := \/\*([^*]|\*[^/])*\*\/
HW_ANNOT    := @(cpu|gpu)(\([0-9.]+\))?
WHITESPACE  := [ \t\n\r]+   (skipped, not emitted)
```

---

### HW 1.2 — Multi-character Token Ambiguity

**Division vs. Line Comment (`/` vs `//`):**

The lexer resolves this using **longest match (greedy)** in `lx_skip_ws`:

```ionic
// Inside lx_skip_ws — called before EVERY token is consumed:
if (lx_peek(lx) == CH_SLASH && lx_peek2(lx) == CH_SLASH) {
    // two consecutive '/' → line comment, skip to newline
    while (lx_peek(lx) != CH_NL && lx_peek(lx) != -1) { lx_advance(lx); }
}
```

The key insight: `lx_skip_ws` is called before the main token dispatch. It consumes `//` comments
entirely. When control returns to the main dispatch loop, the next character is whatever follows
the comment — not `/`. Therefore the token dispatch's `if (c == CH_SLASH)` branch only fires
for actual division operators (single `/`).

**Block Comment `/* ... */` (implemented in HW 1.2):**

```ionic
if (lx_peek(lx) == CH_SLASH && lx_peek2(lx) == CH_STAR) {
    lx_advance(lx);   // consume '/'
    lx_advance(lx);   // consume '*'
    mut in_block = 1;
    while (in_block && lx_peek(lx) != -1) {
        let bc = lx_advance(lx);
        if (bc == CH_STAR && lx_peek(lx) == CH_SLASH) {
            lx_advance(lx);   // consume '/'
            in_block = 0;
        }
    }
}
```

Dragon Book rule: **no nesting** — the first `*/` terminates the comment regardless of inner `/*`.
A string literal `"/*"` is handled correctly because `lx_skip_ws` only runs between tokens,
never inside a string-scanning loop.

**Compound assignment operators (implemented):**

`+=`, `-=`, `*=`, `/=` are new two-character tokens (TOK_PLUS_EQ = 66, TOK_MINUS_EQ = 67,
TOK_STAR_EQ = 68, TOK_SLASH_EQ = 69). They are recognized by lookahead in the operator
dispatch section:

```ionic
if (c == CH_PLUS) {
    if (lx_peek(lx) == CH_EQ) { lx_advance(lx); kind = TOK_PLUS_EQ; }
    else                       { kind = TOK_PLUS; }
}
```

The parser **desugars** `x += e` to `x = x + e` immediately during parsing (no new AST node
is needed):

```ionic
if (is_compound_assign(assign_tok)) {
    let op = compound_assign_op(assign_tok);   // TOK_PLUS_EQ → OP_ADD
    p_advance(p);
    let rhs = parse_expr(p);
    let expanded = new_node(ND_BINOP, "", op, target, rhs, 0);
    return new_node(ND_ASSIGN, "", 0, target, expanded, 0);
}
```

This matches the Dragon Book's recommended approach: introduce the compound operator at the
lexer/parser boundary and reduce it to primitive operations before semantic analysis.

---

### HW 1.3 — Error Recovery in Lexing

**Strategy: Panic-mode character-skip (implemented)**

When the lexer encounters a character that matches no pattern, it:
1. Emits a `TOK_BAD_CHAR` token (kind = 70) carrying the unrecognized character in `g_tok_svals`
2. Prints a lexer-level diagnostic with full source-location caret
3. **Continues lexing** — does not abort

```ionic
if (kind == 0) {
    kind = TOK_BAD_CHAR;
    print_span(tk_line, tk_col,
        str_concat("unrecognized character '", str_concat(char_to_str(c), "'")));
}
```

The parser then handles `TOK_BAD_CHAR` tokens: they are unrecognized in any grammar position,
so the parser's `p_expect` or `parse_primary` fails and triggers `p_sync_stmt`, which skips
forward to the next statement-level synchronization token (`let`, `mut`, `return`, `if`, etc.).

**Unclosed string literal:**

The string-scanning loop checks for `sc == -1` (EOF):
```ionic
if (sc == -1) { in_str = 0; running = 0; }
```
Setting `running = 0` causes the outer `lex_global` loop to terminate. The last token
pushed is `TOK_STRING_LIT` with the partial string. The parser will then see `TOK_EOF`
in an unexpected position and report "unexpected EOF."

**Token stream for broken input** `mut x = 10 $ 5;`:
```
TOK_MUT(2)  TOK_IDENT("x")  TOK_EQ(44)  TOK_INT_LIT(10)
TOK_BAD_CHAR('$')  ← lexer prints: [error] 1:12: unrecognized character '$'
TOK_INT_LIT(5)  TOK_SEMI(49)  TOK_EOF
```
The parser sees `TOK_BAD_CHAR` after `10` where it expects `TOK_SEMI`, emits a parse error,
and recovers at the next `TOK_SEMI`, reporting just one additional parse error — not a crash.

---

## Part 2: Parser

### HW 2.1 — Recursive Descent for Ionic Expressions

**Left-recursion removal (Dragon Book §4.1):**

The original grammar for expressions has left-recursive production rules:

```
expr → expr addop term   ← LEFT RECURSIVE
term → term mulop factor ← LEFT RECURSIVE
```

Left recursion is fatal for recursive-descent parsers because `parse_expr` would immediately
call itself without consuming any input, causing infinite recursion.

**Elimination algorithm:** For each rule of form `A → Aα | β`, replace with:
```
A  → β A'
A' → α A' | ε
```

**Applied to Ionic's full operator hierarchy (lowest to highest precedence):**

```
expr    → and_expr  { "||" and_expr }          // left-assoc via iteration
and_expr → eq_expr  { "&&" eq_expr  }
eq_expr → cmp_expr  { ("==" | "!=") cmp_expr  }
cmp_expr → shift_expr { ("<" | ">" | "<=" | ">=") shift_expr }
shift_expr → add_expr { ("<<" | ">>") add_expr }
add_expr → mul_expr  { ("+" | "-") mul_expr   }
mul_expr → unary     { ("*" | "/" | "%") unary }
unary   → "-" unary | "!" unary | "~" unary | postfix
postfix → primary { "." IDENT ["(" arglist ")"] | "[" expr "]" }
primary → INT | FLOAT | STRING | BOOL | "(" expr ")" | "[" arglist "]"
        | IDENT ["(" arglist ")" | "{" fieldinits "}"]
```

Each `{ ... }` is an iteration (while loop) — this eliminates left recursion while preserving
left associativity. The Ionic parser in `src/parser/parser.ionic` implements exactly this
structure: `parse_or` → `parse_and` → `parse_bitor` → `parse_bitxor` → `parse_bitand` →
`parse_eq` → `parse_cmp` → `parse_shift` → `parse_add` → `parse_mul` → `parse_unary` →
`parse_postfix` → `parse_primary`.

**Operator precedence table (tightest at bottom):**

| Level | Operators | Associativity |
|-------|-----------|---------------|
| 1 | `\|\|` | left |
| 2 | `&&` | left |
| 3 | `\|` | left |
| 4 | `^` | left |
| 5 | `&` | left |
| 6 | `==` `!=` | left |
| 7 | `<` `>` `<=` `>=` | left |
| 8 | `<<` `>>` | left |
| 9 | `+` `-` | left |
| 10 | `*` `/` `%` | left |
| 11 | unary `-` `!` `~` | right (prefix) |
| 12 | postfix `.` `[]` | left |

---

### HW 2.2 — Parser AST and Position Tracking

Ionic's AST is stored as a **parallel array representation** (Structure of Arrays, not Array of
Structures). Every node is identified by an integer index into these global arrays
(`src/parser/ast.ionic`):

```
nd_kind[i]  — node type (ND_* constant)
nd_sv[i]    — string payload (identifier, literal text)
nd_iv[i]    — integer payload (literal value, operator kind, mutability flag)
nd_i2[i]    — first child index
nd_i3[i]    — second child index
nd_i4[i]    — third child index
nd_line[i]  — source line (set from g_cur_line at node creation)
nd_col[i]   — source column (set from g_cur_col at node creation)
```

**Mapping to the homework's type definitions:**

```
struct Program   → [imports, structs, fns, top_stmts] array (parse_program return value)

enum Definition:
  FuncDecl      → ND_FN_DEF:  sv=name, iv=line, i2=params_list, i3=ret_ty, i4=body
  LetBind       → ND_LET:     sv=name, iv=mutable(0/1), i2=ty_node(0=inferred), i3=init_expr, i4=hw

enum Expr:
  LitInt(i64)   → ND_INT:          iv=value
  LitFloat(f64) → ND_FLOAT:        sv=text, iv=ieee754_bits
  LitStr(String)→ ND_STR:          sv=text
  Var(Ident)    → ND_IDENT:        sv=name
  BinOp(op,l,r) → ND_BINOP:        iv=OP_* constant, i2=lhs, i3=rhs
  Call(f,args)  → ND_CALL:         sv=name, i2=CONS list of args
  ArrayLit(es)  → ND_ARRAY_LIT:    i2=CONS list of elements
  ArrayIndex    → ND_INDEX:         i2=array_expr, i3=index_expr
  FieldAccess   → ND_FIELD_ACCESS:  sv=field_name, i2=object_expr
  MethodCall    → ND_METHOD_CALL:   sv=method_name, i2=object_expr, i3=CONS arg list
  TypeApp       → tensor@cpu/gpu expressed as ND_TY_TENSOR with iv=0(cpu)/1(gpu)
                  — not a separate expression node; appears only in type position
  ToGpu/ToCpu   → ND_TO_GPU / ND_TO_CPU: i2=operand_expr
```

**Position tracking:** `g_cur_line` and `g_cur_col` are updated by `p_advance` from the current
token's stored position. `new_node` stamps them into `nd_line[i]` and `nd_col[i]` at allocation
time, so every node carries its source location for error reporting.

---

### HW 2.3 — Backtracking Parser for Type Inference

The type inference function walks the AST **post-order** (children before parent) to infer types.
The semantic checker `sc_expr` in `src/semantic/checker.ionic` implements this. Here is the
design mapped to the homework spec:

```
type_infer(node) → Type:

  ND_INT   → TY_INT64
  ND_FLOAT → TY_FLOAT64
  ND_BOOL  → TY_BOOL   (bool and int64 are compatible at runtime in Ionic)
  ND_STR   → TY_STR

  ND_IDENT(name) → env_lookup(name)  // returns TY_UNKNOWN if not in scope

  ND_BINOP(op, lhs, rhs):
    lt = type_infer(lhs)
    rt = type_infer(rhs)
    if op ∈ {ADD, SUB, MUL, DIV, MOD}:
      if lt == TY_FLOAT64 || rt == TY_FLOAT64:
        // Promote: rewrite op to OP_FADD/FSUB/FMUL/FDIV in AST node
        return TY_FLOAT64
      return TY_INT64
    if op ∈ {EQEQ, NEQ, LT, GT, LTEQ, GTEQ, AND, OR}:
      return TY_BOOL

  ND_CALL(name, args):
    fi = fn_find(name)
    // check arg count, check arg types against declared param types
    return fn_ret_tys[fi]

  ND_ARRAY_LIT(elements):
    elem_ty = TY_UNKNOWN
    for each element:
      et = type_infer(element)
      if elem_ty == TY_UNKNOWN: elem_ty = et
      else if et != elem_ty: ERROR("mixed element types")
    return TY_ARRAY   // elem type tracked separately in sc_elem_ty
```

**Type inference for `let` binding:**

```
ND_LET(name, ty_annotation=0, init_expr):
  init_ty = type_infer(init_expr)
  if ty_annotation != 0:
    ann_ty = node_ty_to_int(ty_annotation)
    if !compatible(init_ty, ann_ty): ERROR
    bind(name, ann_ty)
  else:
    bind(name, init_ty)     // pure inference — no annotation needed
```

**Edge case: `[1, true]`**
`sc_expr` for `ND_ARRAY_LIT` infers element 1 as `TY_INT64`, then element 2 as `TY_BOOL`.
`ty_compat(TY_BOOL, TY_INT64)` returns `true` (because `bool` and `int64` are the same at
runtime in Ionic's current design). For truly incompatible types like `[1, "hello"]`, the
checker emits: `"mixed element types: int64 and string"`.

---

### HW 2.4 — Error Recovery in Parsing

Ionic implements **panic-mode recovery** with two synchronization functions:

**`p_sync_stmt`** — statement-level recovery inside function bodies:
```ionic
// Skip forward until we see a reliable statement-start keyword
// or a '}' that ends the enclosing block
sync tokens: let, mut, return, if, while, for, break, continue, }, fn, struct
```
If a `;` is encountered first, it is consumed (common case: malformed expression statement).

**`p_sync_top`** — top-level recovery between definitions:
```ionic
sync tokens: fn, struct, let, mut
```

**Multi-error design:** `g_parse_error` acts as a gating flag. When set:
- `p_expect` returns immediately without consuming any token
- `parse_error` increments `g_parse_error_count` but only prints diagnostics for the FIRST
  error per parse attempt

After `p_sync_stmt` runs, `g_parse_error` is cleared to 0, allowing parsing to resume.
This produces multiple diagnostics in one pass without stop-first behavior.

`parse_block` drives this loop:
```ionic
while (p_kind(p) != TOK_RBRACE && p_kind(p) != TOK_EOF) {
    push(stmts, parse_stmt(p));
    n = n + 1;
    if (g_parse_error != 0) {
        p_sync_stmt(p);
        g_parse_error = 0;   // clear and continue
    }
}
```

---

## Part 3: Type System

### HW 3.1 — Type Environment and Substitution

Ionic's type environment is implemented as a **flat stack with frame markers**
(`src/semantic/checker.ionic`, lines 93–159). This corresponds to the TAPL §19 substitution model.

```
type Env  ≅ (sv_names[], sv_tys[], sv_svs[], sv_muts[], sv_count)
type Frame ≅ (frame_stk[], frame_count)   // stack of sv_count values at each scope entry

env_declare(x, τ) ≡ sv_declare(x, ty, sv, mut_flag)   // extend env
env_lookup(x)     ≡ sv_lookup_ty(x)                    // scan sv_names[sv_count..1] backwards
push_frame()      ≡ sv_push_frame()                     // save current sv_count
pop_frame()       ≡ sv_pop_frame()                      // restore sv_count (drops inner bindings)
```

The backward scan in `sv_lookup_idx` (line 133) implements **innermost-scope wins**: the most
recently declared binding with a given name shadows all outer ones.

**Hardware placement annotations in the type system:**

```
tensor@cpu → TY_TENSOR_CPU = 9
tensor@gpu → TY_TENSOR_GPU = 10
model@cpu  → TY_MODEL_CPU  = 11
model@gpu  → TY_MODEL_GPU  = 12
```

These are **distinct types** — `ty_compat(TY_TENSOR_CPU, TY_TENSOR_GPU)` returns `false`.
Substitution (`apply_sub` in TAPL §19 terms) is handled by `node_ty_to_int(ty_node)` which
maps AST type nodes to integer TY_* constants — there are no unification variables in Ionic's
current type system (it uses a simpler propagation-based inference, not full HM).

**Environment lookup:**
```
env[x] = sv_tys[i]   where i = first i from sv_count down to 1 where sv_names[i] == x
env[x] not_found → TY_UNKNOWN  (treated as "compatible with anything" to avoid error cascades)
```

---

### HW 3.2 — Hardware-Placement Subtyping

**Subtyping rules (TAPL §21 notation):**

```
──────────────── [REFL]
   τ ⊑ τ

   τ₁ ⊑ τ₂    τ₂ ⊑ τ₃
──────────────────────── [TRANS]
        τ₁ ⊑ τ₃

   tensor@cpu ⊄ tensor@gpu    tensor@gpu ⊄ tensor@cpu    [NO-CROSS]
```

The NO-CROSS rule is the key design decision: hardware placement is **invariant**, not covariant.
This prevents accidental cross-device data movement (which would require a PCIe/NVLink transfer)
from appearing in well-typed programs.

**The transfer operation as a type:**

```
move(t: tensor@gpu, target=CPU) : tensor@gpu → tensor@cpu

In Ionic syntax: t.toCpu()   [ND_TO_CPU node]
                 t.toGpu()   [ND_TO_GPU node]

Typing rule MOVE-TO-CPU:
   Γ ⊢ e : tensor@gpu
   ────────────────────────
   Γ ⊢ e.toCpu() : tensor@cpu

Typing rule MOVE-TO-GPU:
   Γ ⊢ e : tensor@cpu
   ────────────────────────
   Γ ⊢ e.toGpu() : tensor@gpu
```

The checker enforces this in `sc_expr` for `ND_TO_GPU` and `ND_TO_CPU`:
```ionic
if (k == ND_TO_GPU) {
    sc_in_conversion = 1;    // suppress false-positive "cpu used in gpu block"
    sc_expr(nd_i2[node]);
    sc_in_conversion = 0;
    if (sc_ty != TY_TENSOR_CPU && sc_ty != TY_UNKNOWN) {
        sc_error_at("toGpu", "operand must be tensor@cpu");
    }
    sc_ty = TY_TENSOR_GPU; sc_sv = ""; return 0;
}
```

**Type error example:**
```ionic
fn apply_model(tensor@cpu w, tensor@cpu x) -> tensor@cpu { ... }

let g = some_tensor.toGpu();  // g : tensor@gpu
apply_model(g, x);            // TYPE ERROR
```
The checker calls `sc_expr` on each argument. For `g`, it sees `TY_TENSOR_GPU`. The expected
param type is `TY_TENSOR_CPU`. `ty_compat(TY_TENSOR_GPU, TY_TENSOR_CPU)` is `false`, so:
```
[error] 3:14: type error in arg 1 of 'apply_model': expected tensor@cpu, got tensor@gpu
        (call .toCpu() to convert)
```
The hint is generated by `tensor_convert_hint(got, want)`.

---

### HW 3.3 — Type Inference Algorithm (Hindley-Milner trace)

Ionic uses **propagation-based inference** (a subset of Algorithm W) — there are no unification
variables or occurs checks because all types are monomorphic at the expression level.

**Algorithm W sketch for Ionic (`sc_expr` in checker.ionic):**

```
W(expr, Γ) → (τ, Γ'):

  W(LitInt, Γ)        = (TY_INT64, Γ)
  W(LitFloat, Γ)      = (TY_FLOAT64, Γ)
  W(Var(x), Γ)        = (Γ(x), Γ)                         if x ∈ dom(Γ)
                       = (TY_UNKNOWN, Γ)                    otherwise (error already reported)

  W(BinOp(+, e1, e2), Γ):
    (τ1, Γ') = W(e1, Γ)
    (τ2, Γ'') = W(e2, Γ')
    if τ1 = TY_FLOAT64 ∨ τ2 = TY_FLOAT64:
      REWRITE op from OP_ADD to OP_FADD in AST   ← key: AST mutation during inference
      return (TY_FLOAT64, Γ'')
    return (TY_INT64, Γ'')

  W(Let(x, e), Γ):
    (τ, Γ') = W(e, Γ)
    return (TY_VOID, Γ'[x ↦ τ])
```

**Trace for the example:**

```
// Input:
mut x = 42 + 1;       // infer x : int64
let y = 3.14 * x;     // float64 * int64 → ?

Statement 1: W(Let("x", BinOp(+, 42, 1)))
  W(BinOp(+, 42, 1)):
    W(42) = TY_INT64
    W(1)  = TY_INT64
    neither is float → result TY_INT64, op stays OP_ADD
  bind x ↦ TY_INT64
  Γ' = {x: TY_INT64}

Statement 2: W(Let("y", BinOp(*, 3.14, x)))
  W(BinOp(*, 3.14, x)):
    W(3.14) = TY_FLOAT64
    W(x)    = TY_INT64  (from Γ')
    TY_FLOAT64 is present → result TY_FLOAT64, op rewritten to OP_FMUL
  bind y ↦ TY_FLOAT64
  Γ'' = {x: TY_INT64, y: TY_FLOAT64}
```

**Design choice: auto-widen int→float for mixed arithmetic**

Ionic DOES auto-widen: `3.14 * x` is legal (x is int64). The AST op is rewritten from
`OP_MUL` to `OP_FMUL`. At codegen time, `OP_FMUL` emits an integer-to-float conversion
instruction before the `FMUL` FP instruction.

This matches the ATAPL §5 approach: type inference mutates the AST to make the coercion
explicit before code generation.

---

### HW 3.4 — Array Type Inference

**Least Upper Bound (LUB) table for Ionic:**

```
LUB(τ, τ)             = τ               [reflexivity]
LUB(TY_INT64, TY_BOOL)  = TY_INT64      [bool ≡ int64 at runtime]
LUB(TY_INT64, TY_FLOAT64) = TY_FLOAT64  [widening — Ionic allows this]
LUB(TY_INT64, TY_STR)    = ⊥ (Bottom)  [ERROR: mixed element types]
LUB(TY_FLOAT64, TY_STR)  = ⊥           [ERROR]
LUB(TY_ARRAY, τ)          = ⊥           [ERROR: nested arrays not inferred]
LUB(TY_UNKNOWN, τ)        = τ           [TY_UNKNOWN propagates upward, resolved later]
```

**Implementation in `sc_expr` for `ND_ARRAY_LIT`:**

```ionic
if (k == ND_ARRAY_LIT) {
    mut elem_ty = TY_UNKNOWN;
    mut enode = nd_i2[node];
    while (enode != 0) {
        sc_expr(nd_i2[enode]);                          // infer element type
        if (elem_ty == TY_UNKNOWN && sc_ty != TY_UNKNOWN) {
            elem_ty = sc_ty;                            // first concrete element sets the type
        } else if (!ty_compat(sc_ty, elem_ty) && sc_ty != TY_UNKNOWN) {
            sc_error_at("array literal",               // LUB = ⊥ → error
                str_concat("mixed element types: ",
                    str_concat(ty_name(elem_ty, ""), str_concat(" and ", ty_name(sc_ty, "")))));
        }
        enode = nd_i3[enode];
    }
    sc_elem_ty = elem_ty;
    sc_ty = TY_ARRAY; sc_sv = ""; return 0;
}
```

**Example: `push` validation**

When `arr_push(arr, elem)` is called, the checker could (future work) validate that `elem`'s
type matches `arr`'s inferred element type. Currently Ionic's `push` builtin accepts any types
(both params are `TY_UNKNOWN` in the builtin registry).

**`[1, true]`:** `ty_compat(TY_BOOL, TY_INT64)` returns `true` (line 88 in checker.ionic),
so Ionic accepts `[1, true]` and infers `arr[int64]`. The rationale: `bool` is just `int64`
at runtime (no distinct boolean machine type).

**`[1, "hello"]`:** `ty_compat(TY_STR, TY_INT64)` returns `false` →
`ERROR: mixed element types: int64 and string`.

---

## Part 4: Code Generation

### HW 4.1 — SSA Form and Basic Block Construction

Ionic's current codegen (`src/codegen/native.ionic`) performs **direct one-pass AST-to-ARM64**
translation with a simple register allocator — it does NOT generate SSA form. This section
designs what SSA would look like for Ionic.

**SSA Instruction Set:**

```
enum Op {
    Const(i64),              // dest = constant
    Copy(src: Reg),          // dest = src
    Add(l: Reg, r: Reg),
    Sub(l: Reg, r: Reg),
    Mul(l: Reg, r: Reg),
    Div(l: Reg, r: Reg),
    Cmp(l: Reg, r: Reg, cc: CondCode),  // sets condition code
    Phi(srcs: Vec<(BlockId, Reg)>),     // φ-function
    Branch(cond: Reg, true_bb: BlockId, false_bb: BlockId),
    Jump(bb: BlockId),
    Call(name: string, args: Vec<Reg>),
    Ret(val: Option<Reg>),
    Param(idx: i64),         // function parameter
}

struct Instr { dest: Option<Reg>, op: Op, line: i64 }
struct BasicBlock { id: BlockId, instrs: Vec<Instr>, preds: Vec<BlockId> }
struct SSAFunc { name: string, blocks: Vec<BasicBlock>, entry: BlockId }
```

**SSA representation of Fibonacci:**

```
fn fibonacci(int64 n) -> int64 {
    if (n <= 1) { return n; }
    mut a = 0;
    mut b = 1;
    mut i = 2;
    while (i <= n) { let c = a + b; a = b; b = c; i = i + 1; }
    return b;
}
```

```
; Parameters
%n_0  = param(0)

BB_entry:
  %cond_0 = cmp(%n_0, const(1), LE)
  branch %cond_0 → BB_base_case, BB_init

BB_base_case:
  ret %n_0

BB_init:
  %a_0 = const(0)
  %b_0 = const(1)
  %i_0 = const(2)
  jump → BB_loop_head

BB_loop_head:                    ; preds: BB_init, BB_loop_body
  %a_1 = phi(BB_init: %a_0,     BB_loop_body: %a_2)
  %b_1 = phi(BB_init: %b_0,     BB_loop_body: %b_2)
  %i_1 = phi(BB_init: %i_0,     BB_loop_body: %i_2)
  %cond_1 = cmp(%i_1, %n_0, LE)
  branch %cond_1 → BB_loop_body, BB_exit

BB_loop_body:
  %c_0  = add(%a_1, %b_1)
  %a_2  = copy(%b_1)           ; a = b
  %b_2  = copy(%c_0)           ; b = c
  %i_2  = add(%i_1, const(1))  ; i = i + 1
  jump → BB_loop_head

BB_exit:
  ret %b_1
```

**φ-function placement rule** (Cytron et al. / SSABook Ch. 3):

A φ-function for variable `v` is needed at basic block `B` if and only if `B` is in the
**dominance frontier** of some block that defines `v`. In the fibonacci example:
- `a`, `b`, `i` are defined in `BB_init` (initial values) and `BB_loop_body` (updates)
- `BB_loop_head` is in the dominance frontier of both → φ-functions placed there

**Dominance frontier computation** (Dragon Book Ch. 6):

```
DF(BB_init)      = {BB_loop_head}   // BB_init strictly dominates BB_loop_body
                                    // but BB_loop_head has pred BB_loop_body which
                                    // BB_init does not dominate
DF(BB_loop_body) = {BB_loop_head}   // same argument
```

**Why SSA enables optimizations:**

1. **Constant propagation:** If `%cond_0 = cmp(const(0), const(1), LE)` → `%cond_0 = const(1)`,
   the branch to `BB_base_case` is known at compile time and can be eliminated (DCE).

2. **Register allocation by coloring:** In SSA form, every `%v_i` is defined exactly once.
   Two SSA values can share a physical register iff their live ranges do not interfere
   (Chaitin graph coloring). The φ-function at `BB_loop_head` tells the allocator that
   `%a_0` and `%a_2` feed the same slot, requiring a consistent register assignment.

3. **GVN / CSE:** Duplicate computations (same op, same SSA operands) are trivially identified
   because names are unique — no aliasing to worry about.

**Ionic's actual codegen (without SSA):**

The ARM64 codegen in `src/codegen/native.ionic` uses a simpler strategy:
- Function parameters → callee-saved registers X19..X(19+n-1)
- Local variables (`ND_LET`, `ND_ASSIGN`) → spill slots at `[X29 + offset*8]`
- Temporaries → X9 (scratch), with X10/X11 for two-operand operations
- Loop variables (for `while` / `for`) → same local slots, re-read each iteration

This is equivalent to SSA with all variables immediately spilled to memory — correct but
suboptimal. True SSA with graph-coloring allocation would keep loop variables in callee-saved
registers across iterations, eliminating load/store traffic in the hot path.

---

## Summary of Implemented Features

| Homework | Feature | Location | Status |
|----------|---------|----------|--------|
| HW 1.1 | Token stream analysis | `src/lexer/lexer.ionic` | Existing |
| HW 1.2 | Block comments `/* */` | `src/lexer/lexer.ionic:lx_skip_ws` | **Implemented** |
| HW 1.2 | Compound assignments `+=` `-=` `*=` `/=` | `src/lexer/tokens.ionic`, `src/lexer/lexer.ionic`, `src/parser/parser.ionic` | **Implemented** |
| HW 1.3 | Bad-char error recovery | `src/lexer/lexer.ionic`, `src/compiler.ionic` | **Implemented** |
| HW 2.1 | Left-recursion removal proof | `src/parser/parser.ionic` | Existing (documented above) |
| HW 2.2 | AST node types with position | `src/parser/ast.ionic` | Existing |
| HW 2.3 | Post-order type inference | `src/semantic/checker.ionic:sc_expr` | Existing |
| HW 2.4 | Panic-mode parse recovery | `src/parser/parser.ionic:p_sync_stmt` | Existing |
| HW 3.1 | Type environment + lookup | `src/semantic/checker.ionic:sv_*` | Existing |
| HW 3.2 | Hardware-placement subtyping | `src/semantic/checker.ionic:ty_compat` | Existing |
| HW 3.3 | Type inference trace | `src/semantic/checker.ionic` | Documented above |
| HW 3.4 | Array LUB inference | `src/semantic/checker.ionic:ND_ARRAY_LIT` | Existing |
| HW 4.1 | SSA form design | (Design answer — not yet in codegen) | Documented above |
