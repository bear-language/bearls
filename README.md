# bearls

Language server (LSP 3.18 over stdio) for [bear](https://github.com/bear-language/bear). C++23, built on `libbearc`'s `ContextDatabase`.

## Build / Install

### Requirements
- CMake >= 3.25, Ninja (or make)
- C++23 compiler with `<print>` and `<expected>`
- LLVM dev packages (same as [bearc]([bear](https://github.com/bear-language/bear)): `llvm-devel` / `llvm-dev` / `brew install llvm`)
- bear-lang sources: `libbearc` is built from source alongside bearls (same compiler + flags)
  - expected at `../bear-lang`; otherwise `-DBEARC_DIR=/path/to/bear-lang/bearc`

### Build
```bash
# Release (default)
cmake -S . -B build -G Ninja
cmake --build build

# Debug 
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug

# run
./build/bearls --version     
```

### Install
```bash
cmake --install build --prefix ~/.local    # installs ~/.local/bin/bearls
# or point your editor at `build/bearls` directly if you prefer not to install
```

## Running
```
bearls [--stdio] [--verbose] [-V|--version] [-h|--help]
```

- `--stdio`: accepted for compatibility (stdio is the only transport)
- `--verbose`: logs each analysis + timing to stderr (ends up in your editor's LSP log)
- note: fd 1 redirected to stderr at startup; protocol goes to a private dup of the original stdout, so
  stray libbearc output can't corrupt it

### Project files (`.bearls`)
bearc invocation for a whole project (one `key = value` per line):

```
# blank lines and lines starting with '#' are ignored
cwd = ../../..
args = main.br -I tests/projects/04 -I tests/lib --warn-cyclic-import
```

- `args` (optional): bearc command line
  - `args=...` and a leading `bearc` both fine; bearls always supplies `argv[0]` itself
  - whitespace-split, no quoting (yet)
  - omitted -> no project database; each file gets its own, `.bearls` only supplies `cwd` +
    `import-paths`
- `cwd` (optional): directory bearc runs from
  - relative -> relative to the `.bearls` dir; must exist
  - omitted -> innermost workspace folder containing the `.bearls` (e.g. repo root with Neovim's
    `root_markers = { '.git' }`), else the `.bearls` dir
  - relative paths in `args` resolve against it, as on the command line (the example above works
    from any workspace folder)
- `import-paths` (optional): whitespace-separated dirs, relative to the same dir as `args`
  - only for per-document databases (files outside the project, or all files if no `args`)
  - `import-paths = lib` alone ≈ a per-directory `importPaths`
- bearc resolution, same as the CLI:
  - input file: working dir, then `-I` paths
  - imports: `-I` paths only, never relative to the importing file
- `importPaths`, `extraArgs`, `import-paths` never apply to the project database
- invalid `.bearls` (unknown key, bad args, missing `cwd`) -> `window/showMessage`, files fall back
  to one database each
- changes picked up on next save or next edit in the project; added/removed `.bearls` on next save

### Settings (`initializationOptions`)
- `importPaths` = `[]` (extra `-I` dirs, relative to the workspace folder)
- `extraArgs` = `[]` (extra bearc flags, e.g. `["--warn-cyclic-import"]`)
- `debounceMs` = `30` (quiet period in ms after an edit before re-analyzing)

`importPaths` and `extraArgs` only apply to per-document databases, never a `.bearls` project
database, which is built from its `args` alone. Files under a `.bearls` that the project doesn't
reach (or every file, if it has no `args`) still get them.

Import path order for per-document analysis: document's dir -> `importPaths` -> `.bearls`
`import-paths` -> workspace folders. Document's dir first mirrors `bearc main.br -I <dir>`.

## TODOs 
- no go to definition for member accesses (`p.x`, `self.bar_`): needs the receiver's type, which
  `ContextDatabase` doesn't expose yet, so this resolves to nothing right now
- never-saved documents aren't analyzed (bearc needs the root on disk), but after first save, unsaved
  edits are used as usual

## Features
- diagnostics
  - parser + hir/sema diagnostics, published as you type (no save needed)
  - chained notes (e.g. *previous definition here*) attached as `relatedInformation`, not
    published separately; works across files
- go to definition
  - plain and scoped identifiers (`foo`, `foo..bar`) across files
  - cursor on a qualifier jumps to the qualifier (`math` in `math..square` -> `mod math`)
- code actions for chained diagnostics
  - one *Go to: previous definition here (util.br:3)* action per related location (skips duplicate locations)
  - uses `window/showDocument` via `bearls.showLocation`; only offered if the client supports it
- unsaved buffers: open documents (and open files they import) analyzed from in-editor contents
- UTF-8/16/32 position encodings (UTF-8 preferred, matches bearc's byte spans)

## Implementation
bearc does ~250k-1M LoC/s, so no incremental stuff is done:
1. document under a [`.bearls`](#project-files-bearls) -> shares the project's `ContextDatabase`
   (built from the project's bearc invocation), if reachable from the project's input file
2. anything else -> its own `ContextDatabase` rooted at itself
   - documents switch between project/own database as edits change reachability
3. edit -> marks stale the document + every open document/project that read the edited file;
   re-analyzed after `debounceMs` of quiet (default 30 ms)
4. requests re-analyze on demand if anything is stale, so positions always match current text
