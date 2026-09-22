# bearls: synopsis

I built `bearls` in `~/dev/bearls`: a C++23 language server built on `ContextDatabase`. It publishes
parser and HIR diagnostics as you type, without saving, and supports go-to-definition. It works end
to end in Neovim 0.12. The main caveat: bearc sometimes hangs or crashes on half-typed code, and
because it runs inside the server, that takes the server down with it.

**Testing.** 8 end-to-end protocol tests pass on GCC Release (with LTO), GCC Debug and Clang. A
headless Neovim run attached the server, showed a diagnostic on an unsaved edit, and
`vim.lsp.buf.definition()` jumped from `math..square` into `util.br`. `clang-tidy` with your config
and `clang-format` are clean. Your own bearc test suite passes 334/334 with my changes, same as
before them.

**Speed.** A generated 72k-line file takes about 58 ms per full parse + sema (≈1.2M lines/sec), so
the server simply re-analyzes after edits. Edit to diagnostics is about 100 ms including a 30 ms
wait for typing to pause, and go-to-definition takes about 0.7 ms.

**Features:**
- **Diagnostics:** chained notes like "previous definition here" show at their own location and are
  also attached to the error, even when they're in another file.
- **Go to definition:** works for plain and scoped names (`math..square`, `Foo..do_work`) across
  files, including locals, parameters, globals, types and modules. With the cursor on `math`, it
  jumps to `mod math`.
- **Unsaved buffers:** any open file is read from the editor, including open files that the current
  file imports.
- **Positions:** UTF-8 is preferred since it matches bearc's byte offsets, with UTF-16 as the
  fallback.

**Changes to libbearc (please review):** `ContextDatabase` wasn't quite enough on its own, so I made
small, additive changes (4 files, clang-format clean, uncommitted):
- **Accessors:** `file_id_for_path`, `file_name`, `file_source`, `file_is_intrinsic` and `symbol`.
  Without these the server can't map LSP paths to `FileId`s.
- **Source overlays:** an optional parameter so a `Context` can read open buffers from memory
  instead of disk.
- **Bug fix in `Context::scope_for_span`:** its binary search assumed the span list was sorted by
  position, but function scopes are registered before any function body's scope. As a result, local
  variables were never found. It also read out of bounds for positions outside every registered
  span, such as top-level code. It now sorts once before the first lookup and falls back to the root
  scope. `ContextDatabase` is its only caller.

**Crashes and hangs on half-typed code.** I added a fuzzer that feeds the server every 5th prefix of
each file in `bear-lang/tests`. It found dozens of prefixes that crash or hang the server (partial
run). Every one I checked with the plain `bearc` CLI reproduces there too, so they're compiler bugs,
not server bugs. Most are infinite loops in the parser at an unexpected end of file, plus a few
segfaults in the parser and in sema. Some trigger early, e.g. `tests/hir/63.br` after 110
characters. When one hits, `:lsp restart bearls` brings the server back. Run
`python3 tests/prefix_fuzz.py ../bear-lang/tests --out fuzz-failures` to get a repro file for each.
If you'd rather not wait on compiler fixes, I can run the analysis in a separate process so a crash
doesn't kill the server, at roughly double the CPU cost per analysis.

**Other limits:**
- Go-to-definition on member access (`p.x`, `self.bar_`) returns nothing, since that needs the
  receiver's type.
- A file that has never been saved isn't analyzed, because bearc looks for the root file on disk.
- bearc itself doesn't yet report undeclared names inside function bodies (`i32 x = NOPE;`), so the
  server can't either.

## Build and run

```bash
cd ~/dev/bearls
cmake -S . -B build -G Ninja      # Release (-O3 + LTO) by default; builds libbearc from ../bear-lang
cmake --build build
./build/bearls --version          # bearls 0.1.0 (bearc 0.0.1)
python3 tests/lsp_test.py         # protocol tests
cmake --install build --prefix ~/.local   # optional: puts bearls in ~/.local/bin
```

You need CMake 3.25+, Ninja, GCC 14+ or Clang 18+, and the LLVM dev package. If bear-lang isn't at
`../bear-lang`, add `-DBEARC_DIR=/path/to/bear-lang/bearc`. The server talks over stdin/stdout. Add
`--verbose` to log each analysis and its time to your editor's LSP log.

Imports: bearc only looks up `import "x.br"` on its `-I` paths, not next to the importing file. So
each analysis searches the file's own directory first, then any `importPaths` you set, then the
project root.

## Neovim (0.11+)

Add to `init.lua`:

```lua
vim.opt.rtp:append(vim.fn.expand('~/dev/bearls/editors/nvim'))  -- *.br filetype + server config
vim.lsp.config('bearls', {
  -- cmd = { vim.fn.expand('~/dev/bearls/build/bearls') },  -- only if bearls isn't on PATH
  init_options = { importPaths = {} },                       -- also: extraArgs, debounceMs
})
vim.lsp.enable('bearls')
vim.diagnostic.config({ virtual_text = true })  -- diagnostic text is off by default since 0.11
vim.api.nvim_create_autocmd('LspAttach', { callback = function(a)
  vim.keymap.set('n', 'gd', vim.lsp.buf.definition, { buffer = a.buf })
end })
```

Then open any `.br` file:
- `gd` or `CTRL-]` goes to definition, and `CTRL-O` jumps back.
- `]d` and `[d` move between diagnostics, and `<C-w>d` shows the full message.
- `:checkhealth vim.lsp` confirms the server is attached, and `:lsp restart bearls` restarts it
  after a rebuild.

The README also has an inline config that doesn't use the runtimepath, a Neovim 0.10 version, the
settings table, and a code overview. `~/dev/bearls` isn't a git repo yet.
