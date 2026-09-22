#!/usr/bin/env python3
"""feeds bearc every n-th prefix of each .br file, the kind of half-typed input an editor produces

each prefix goes through bearls exactly like an edit would (didChange, then a request that forces
the analysis). a server that dies or stops answering is re-checked against the bearc CLI to tell
compiler bugs (which take the in-process server down with them) from server bugs, and every failing
input is written to the output directory so it can be reproduced with `bearc <file>`

usage: tests/prefix_fuzz.py <dir with .br files> [--step N] [--bearls PATH] [--bearc PATH] [--out DIR]
"""

import argparse
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lsp_test import ROOT, Client, open_doc  # noqa: E402

TIMEOUT_S = 5


def classify_with_bearc(bearc: Path, src: Path, import_dir: Path) -> str:
    """what the plain CLI does with the same input"""
    def run(*flags: str) -> str:
        try:
            proc = subprocess.run(
                [str(bearc), str(src), "-I", str(import_dir), "--silent", *flags],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
                timeout=TIMEOUT_S,
            )
        except subprocess.TimeoutExpired:
            return "hang"
        return "crash" if proc.returncode < 0 else "ok"

    full = run()
    if full == "ok":
        return "bearc ok -> bearls bug?"
    parse_only = run("--parse-only")
    stage = "parser" if parse_only != "ok" else "sema"
    verb = {"crash": "crashes", "hang": "hangs"}[full]
    return f"bearc also {verb} in the {stage}"


def fuzz_file(bearls: Path, path: Path, step: int) -> tuple[int, int | None, str]:
    """(analyses run, failing prefix length or None, failure)"""
    text = path.read_text()
    client = Client(bearls, ["utf-8"])
    uri = open_doc(client, path, "")
    client.diagnostics_for(uri, 1)
    runs = 0
    for version, n in enumerate([*range(0, len(text), step), len(text)], start=2):
        try:
            client.notify(
                "textDocument/didChange",
                {
                    "textDocument": {"uri": uri, "version": version},
                    "contentChanges": [{"text": text[:n]}],
                },
            )
            # definition forces an analysis of exactly this version
            client.definition(uri, text[:n].count("\n"), 0)
            runs += 1
        except AssertionError:
            died = client.proc.poll() is not None
            client.proc.kill()
            return runs, n, "crash" if died else "hang"
    client.shutdown()
    return runs, None, ""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("dir", type=Path)
    parser.add_argument("--step", type=int, default=5)
    parser.add_argument("--bearls", type=Path, default=ROOT / "build" / "bearls")
    parser.add_argument("--bearc", type=Path, default=ROOT / "build" / "bearc" / "bearc")
    parser.add_argument("--out", type=Path, default=Path("fuzz-failures"))
    args = parser.parse_args()

    files = sorted(args.dir.resolve().rglob("*.br"))
    total_runs = 0
    failures = 0
    for path in files:
        runs, n, what = fuzz_file(args.bearls.resolve(), path, args.step)
        total_runs += runs
        if n is None:
            continue
        failures += 1
        args.out.mkdir(parents=True, exist_ok=True)
        repro = (args.out / f"{path.parent.name}_{path.stem}_prefix{n}.br").resolve()
        repro.write_text(path.read_text()[:n])
        # imports still resolve against the original file's directory
        verdict = classify_with_bearc(args.bearc.resolve(), repro, path.parent)
        print(f"{what:5} {path} @ {n} chars: {verdict} (repro: {repro})", flush=True)

    print(f"{total_runs} analyses over {len(files)} files, {failures} failing files")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
