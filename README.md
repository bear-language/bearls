# bearls

Language server for [bear](../bear-lang), written in C++23 on top of `libbearc`'s
`ContextDatabase`

It speaks LSP 3.18 over stdio and provides:

- **diagnostics**: parser diagnostics (from each file's AST) and hir/sema diagnostics (from the
  `hir::Context`), published as you type, with no need to save. Chained notes (e.g. *previous
  definition here*) aren't published as diagnostics of their own. They're attached to the
  diagnostic they belong to as `relatedInformation`, including across files.
- **go to definition** for plain and scoped identifiers (`foo`, `math..square`, `Foo..do_work`)
  across files. Putting the cursor on a qualifier jumps to the qualifier: `math` in
  `math..square` goes to `mod math`.
- **code actions to jump to chained diagnostics**: on a diagnostic, each related location gets a
  *Go to: previous definition here (util.br:3)* action. Locations that are already under the cursor,
  or that another action already goes to, are skipped. Running one sends the client a
  `window/showDocument` request (through the `bearls.showLocation` command), so they're only offered
  to clients that support it.
- unsaved buffers: every open document is analyzed from its in-editor contents, and so is any
  open file it imports.
- incremental text sync and UTF-8/UTF-16/UTF-32 position encodings. UTF-8 is preferred because it
  matches bearc's byte spans, so no transcoding is needed.

## How it works

bearc parses and type-checks at roughly 1M LoC/s, so bearls skips incremental bookkeeping:

1. If there's a [`.bearls` project file](#project-files-bearls) in a document's directory or any
   directory above it, that project gets one `ContextDatabase`, built from the project's bearc
   invocation. It's shared by every open document it covers, meaning every file reachable from the
   project's input file.
2. Every other open document owns a `ContextDatabase` rooted at itself. That includes documents in
   a project that the project doesn't (yet) reach. As soon as one becomes reachable, e.g. because
   an edit imports it, it switches to the project's database. When it stops being reachable, it
   goes back to its own. Either way, that's a full parse + sema of the root and everything it
   transitively imports.
3. An edit marks the document stale, along with every other open document or project whose
   analysis read the edited file. Once the client has been quiet for `debounceMs` (default 30 ms), stale
   documents are re-analyzed and their diagnostics republished.
4. Requests (go to definition, code actions) re-analyze on demand if anything is stale, so positions
   always line up with the current text.

Any file with an open buffer is read from memory through source overlays, never from disk.

Measured on a generated 72k-line file (Release build): **~58 ms** per full analysis, **~0.7 ms**
per go-to-definition.

```
src/
  main.cpp        entry point: flags, stdout protection, SIGPIPE
  transport.*     JSON-RPC framing over raw fds (poll-based, for debouncing)
  json.*          non-throwing accessors, parse and dump over nlohmann::json
  protocol.*      LSP structs (Position, Range, Location, Diagnostic) and their serialization
  text.*          LineIndex (byte offset <-> utf-8/16/32 positions), file:// URIs
  analysis.*      one ContextDatabase + the diagnostics/definition queries against it
  project.*       finding and reading .bearls project files
  server.*        lifecycle, document store, scheduling, request/notification handlers
  log.hpp         stderr logging (stdout belongs to the protocol)
third_party/
  nlohmann/       nlohmann/json 3.12.0, the single-header release (MIT)
```

bearls builds with `-fno-exceptions` (bearc's Release flags), so nlohmann::json calls
`std::abort()` wherever it would throw. Client messages are therefore only read through `json.hpp`'s
accessors (never `at()`, `get<T>()`, or a const `operator[]`), parsing uses
`allow_exceptions = false`, and dumping replaces invalid UTF-8 instead of rejecting it. Also note
that `json::Value{nullptr}` is `[null]`, while `json::Value(nullptr)` is `null`.

## Building

### Requirements

- CMake >= 3.25 and Ninja (or make)
- a C++23 compiler with `<print>` and `<expected>`: GCC >= 14 or Clang >= 18
- LLVM development packages (same as bearc: `llvm-devel` / `llvm-dev` / `brew install llvm`)
- the bear-lang sources. bearls builds `libbearc` from source as part of its own build, so both
  always use the same compiler and flags.

By default the bear-lang checkout is expected next to this one (`../bear-lang`). Otherwise, pass
`-DBEARC_DIR=/path/to/bear-lang/bearc`.

### Build

```bash
cd ~/dev/bearls

# Release (-O3 + LTO) is the default build type. Use it for day-to-day editing.
cmake -S . -B build -G Ninja
cmake --build build

# or a Debug build (asserts on, -O0 -g). Useful when debugging bearc itself.
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug

./build/bearls --version     # bearls 0.1.0 (bearc 0.0.1)
```

### Install

```bash
cmake --install build --prefix ~/.local    # installs ~/.local/bin/bearls
which bearls                               # make sure ~/.local/bin is on your PATH
```

Or skip installing and point your editor at the absolute path of `build/bearls`.

### Test

```bash
python3 tests/lsp_test.py                  # uses build/bearls
python3 tests/lsp_test.py build-debug/bearls
```

## Running

bearls is started by your editor and talks LSP on stdin/stdout:

```
bearls [--stdio] [--verbose] [-V|--version] [-h|--help]
```

- `--stdio` is accepted for compatibility with clients that always pass it. stdio is the only
  transport.
- `--verbose` logs every analysis and its timing to stderr. Your editor collects it into its LSP
  log.

To keep stray output from libbearc from corrupting the protocol stream, bearls points file
descriptor 1 at stderr at startup and writes protocol messages to a private duplicate of the
original stdout.

### Project files (`.bearls`)

A `.bearls` file holds the bearc invocation for a whole project, one `key = value` per line:

```
# blank lines and lines starting with '#' are ignored
cwd = ../../..
args = main.br -I tests/projects/04 -I tests/lib --warn-cyclic-import
```

- `args` (optional) is the bearc command line. `args=...` works too, and so does spelling out the
  whole command (`args = bearc main.br ...`). bearc's arg parser skips `argv[0]`, so bearls always
  puts `bearc` there itself. Args are split on whitespace, and quoting isn't supported yet. Without
  `args`, there's no project database: every file under the `.bearls` gets its own, and the
  `.bearls` only sets `cwd` and `import-paths` for them.
- `cwd` (optional) is the directory bearc runs from. A relative `cwd` is relative to the `.bearls`
  file's directory, and it has to exist. Without `cwd`, bearc runs from the innermost workspace
  folder containing the `.bearls` (with Neovim's `root_markers = { '.git' }`, the repository root),
  or from the `.bearls` directory if it's outside every workspace folder.
- Args are passed to bearc exactly as written, so their relative paths are relative to that
  directory, as on the command line. The example above is what you'd type at the root of
  bear-lang, and `cwd` makes it work no matter which folder the editor treats as the workspace.
- As with the CLI, bearc looks up the input file in its working directory and then on its `-I`
  paths. It resolves imports against the `-I` paths only, not against the importing file.
- `import-paths` (optional) is a whitespace-separated list of directories. They're added to
  `importPaths` from `initializationOptions` for the files under the `.bearls` that get a
  per-document database: the files outside the project, or every file if there's no `args`.
  Relative ones are relative to the same directory as `args`. A `.bearls` with only
  `import-paths = lib` works like a per-directory `importPaths`.
- `importPaths` and `extraArgs` from `initializationOptions` don't apply to the project's
  database, only to the per-document databases of files outside it. The same goes for
  `import-paths`.
- An invalid `.bearls` (unknown key, bad args, a `cwd` that doesn't exist) is reported through
  `window/showMessage`, and its files fall back to one database each. Changes to `.bearls` are
  picked up on the next save, or on the next edit to a file in the project. A `.bearls` that is
  added or removed is picked up on the next save.

Without a `.bearls`, every open document gets its own database, as described above.

### Settings (`initializationOptions`)

| key | default | meaning |
| --- | --- | --- |
| `importPaths` | `[]` | extra `-I` directories. Relative paths resolve against the workspace folder. Not used for `.bearls` projects |
| `extraArgs` | `[]` | extra bearc flags, e.g. `["--warn-cyclic-import"]`. Not used for `.bearls` projects |
| `debounceMs` | `30` | quiet period after an edit before re-analyzing |

The import path for a document's analysis is: the document's own directory, then `importPaths`, then
its `.bearls` file's `import-paths`, then the workspace folders. bearc resolves `import "x.br"` against its import paths, not against the
importing file. The document's own directory comes first to mirror the usual `bearc main.br -I <dir>`
invocation.

## Limitations

- Go to definition doesn't resolve member accesses (`p.x`, `self.bar_`). That needs the
  receiver's type, which `ContextDatabase` doesn't expose. These resolve to nothing rather than to
  something wrong
- A document that has never been saved (so doesn't exist on disk) isn't analyzed, since bearc
  resolves its root file on disk. Once it's saved, its unsaved edits are used as usual

## Fuzzing with half-typed input

```bash
python3 tests/prefix_fuzz.py ../bear-lang/tests --step 5 --out fuzz-failures
# hang  .../tests/hir/63.br @ 110 chars: bearc also hangs in the parser (repro: fuzz-failures/hir_63_prefix110.br)
bearc fuzz-failures/hir_63_prefix110.br -I ../bear-lang/tests/hir    # reproduce directly
```
