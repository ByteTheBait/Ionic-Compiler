# tree-sitter-ionic

A [tree-sitter](https://tree-sitter.github.io/) grammar for the Ionic language,
for editor syntax highlighting and structural queries.

The grammar is derived from the language the compiler itself accepts —
`src/lexer/tokens.ionic`, `src/lexer/lexer.ionic`, and
`src/parser/parser.ionic`. Keep it in sync when the syntax changes.

## Contents

```
tree-sitter.json        Grammar metadata (name, scope, file-types, query paths)
grammar.js              The grammar (source of truth for the query node names)
queries/highlights.scm  Highlight captures
src/                    Generated parser.c + node-types.json (committed)
```

`src/` is checked in so editors and downstream users can build the parser
without needing the tree-sitter CLI or Node. Regenerate it with
`tree-sitter generate` after editing `grammar.js`.

## Developing

Install the CLI (`cargo install tree-sitter-cli`, or
`npm install -g tree-sitter-cli`), then:

```sh
tree-sitter generate                 # rebuild src/ from grammar.js

tree-sitter parse ../examples/hw_tags.ionic      # show the tree (watch for ERROR/MISSING)
tree-sitter query queries/highlights.scm ../examples/hw_tags.ionic
tree-sitter build --wasm -o /tmp/ionic.wasm      # compile a parser
```

A quick sanity pass over everything in the repo:

```sh
for f in ../examples/*.ionic ../lib/std/*.ionic ../src/*.ionic ../src/*/*.ionic; do
    tree-sitter parse "$f" 2>/dev/null | grep -qE 'ERROR|MISSING' && echo "ERR $f" || echo "ok  $f"
done
```

## Query notes

In tree-sitter, when several patterns match the same node the **last** matching
pattern wins (and the innermost node wins for nested matches). The generic
`(identifier) @variable` capture is therefore placed *before* the specific ones
(`@function`, `@type`, `@property`, `@module`) so the specific captures take
precedence.

## Installing in an editor

This grammar lives in a subdirectory of the compiler repo
(`tree-sitter-ionic/`), so the paths below point into that subdirectory.

### Neovim (nvim-treesitter)

Register the parser so `:TSInstall ionic` can fetch and build it. Queries are
not fetched from the parser repo — put `highlights.scm` on Neovim's
`runtimepath` yourself.

```lua
-- ~/.config/nvim/init.lua  (after nvim-treesitter is set up)
local function register_ionic()
  require('nvim-treesitter.parsers').ionic = {
    install_info = {
      url = 'https://github.com/ByteTheBait/Ionic-Compiler',
      location = 'tree-sitter-ionic',   -- build from this subdirectory
      files = { 'src/parser.c' },       -- pre-generated; no `generate` needed
    },
    filetype = 'ionic',
  }
end

register_ionic()
vim.api.nvim_create_autocmd('User', { pattern = 'TSUpdate', callback = register_ionic })

-- Treat *.ionic as its own filetype and turn on highlighting.
vim.filetype.add({ extension = { ionic = 'ionic' } })
vim.api.nvim_create_autocmd('FileType', {
  pattern = 'ionic',
  callback = function() vim.treesitter.start() end,
})
```

Install the parser and the query:

```sh
# then, inside Neovim:
#   :TSInstall ionic
mkdir -p ~/.config/nvim/queries/ionic
cp tree-sitter-ionic/queries/highlights.scm ~/.config/nvim/queries/ionic/
```

(The `nvim-treesitter` *`main`* branch uses a newer Lua API; consult its README
if you are on that branch.)

### Helix

Grammar *and* language config are required (`highlights.scm` is enough for
highlighting; `indents.scm`/`textobjects.scm` are optional).

```toml
# ~/.config/helix/languages.toml
[[language]]
name = "ionic"
scope = "source.ionic"
file-types = ["ionic"]
comment-tokens = "//"
block-comment-tokens = { start = "/*", end = "*/" }
indent = { tab-width = 4, unit = "    " }
grammar = "ionic"

[[grammar]]
name = "ionic"
source = { git = "https://github.com/ByteTheBait/Ionic-Compiler", rev = "main", subpath = "tree-sitter-ionic" }
```

Copy the query files into Helix's runtime, then fetch/build:

```sh
mkdir -p ~/.config/helix/runtime/queries/ionic
cp tree-sitter-ionic/queries/*.scm ~/.config/helix/runtime/queries/ionic/
hx --grammar fetch && hx --grammar build
```

If you prefer, pin `rev` to a commit hash instead of `main`.

### VS Code

There is no published extension, but the grammar (or its compiled WASM) can be
pointed at from an extension that registers `source.ionic` as a language and
lists `tree-sitter-ionic/queries/highlights.scm`. Contributions welcome.

### Manual WASM build

Some editors load a prebuilt parser:

```sh
cd tree-sitter-ionic
tree-sitter build --wasm -o ionic.wasm
```
