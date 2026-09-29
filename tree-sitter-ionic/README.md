# tree-sitter-ionic

A [tree-sitter](https://tree-sitter.github.io/) grammar for the Ionic language,
used for editor syntax highlighting and structural queries.

The grammar is derived from the language the compiler itself accepts —
`src/lexer/tokens.ionic`, `src/lexer/lexer.ionic`, and
`src/parser/parser.ionic`. Keep it in sync when the syntax changes.

## Layout

```
grammar.js            Grammar definition (source of truth for the query node names)
queries/highlights.scm  Highlight captures
```

`src/` (generated `parser.c`, `node-types.json`, …) is produced by
`tree-sitter generate` and is not checked in.

## Generating & testing

Requires the [tree-sitter CLI](https://github.com/tree-sitter/tree-sitter/blob/master/cli/README.md)
(`npm install -g tree-sitter-cli`, or `cargo install tree-sitter-cli`).

```sh
# Regenerate the parser from grammar.js
tree-sitter generate

# Parse a file and show the tree (look for ERROR / MISSING nodes)
tree-sitter parse ../examples/hw_tags.ionic

# Check the highlight query compiles and see its captures
tree-sitter query queries/highlights.scm ../examples/hw_tags.ionic
```

A quick sanity pass over the whole tree:

```sh
for f in ../examples/*.ionic ../lib/std/*.ionic; do
    tree-sitter parse "$f" | grep -qE 'ERROR|MISSING' && echo "ERR $f" || echo "ok  $f"
done
```

## Notes on the query

In tree-sitter, when multiple patterns match the same node the **last** matching
pattern wins. The generic `(identifier) @variable` capture is therefore placed
before the specific ones (`@function`, `@type`, `@property`, `@module`) so the
specific captures take precedence.
