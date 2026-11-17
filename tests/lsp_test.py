#!/usr/bin/env python3
"""end-to-end protocol tests for bearls, driving the server over stdio like an editor would

usage: tests/lsp_test.py [path/to/bearls]   (defaults to build/bearls)
"""

import atexit
import json
import os
import select
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / "tests" / "fixtures"
MAIN = FIXTURES / "main.br"
UTIL = FIXTURES / "util.br"


class Client:
    def __init__(
        self,
        server: Path,
        encodings: list[str],
        capabilities: dict | None = None,
        root: Path = FIXTURES,
    ):
        # a file rather than a pipe, a pipe nobody drains would eventually block the server
        self.stderr = tempfile.TemporaryFile()
        self.proc = subprocess.Popen(
            [str(server), "--verbose"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self.stderr,
            cwd="/",  # imports must not depend on the server's cwd
        )
        self.next_id = 1
        self.buffer = b""
        self.pending: list[dict] = []
        result = self.request(
            "initialize",
            {
                "processId": os.getpid(),
                "rootUri": root.as_uri(),
                "capabilities": {"general": {"positionEncodings": encodings}}
                | (capabilities or {}),
            },
        )
        self.encoding = result["capabilities"]["positionEncoding"]
        self.notify("initialized", {})

    def send(self, msg: dict) -> None:
        body = json.dumps(msg).encode()
        self.proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
        self.proc.stdin.flush()

    def notify(self, method: str, params: dict) -> None:
        self.send({"jsonrpc": "2.0", "method": method, "params": params})

    def request(self, method: str, params: dict):
        rid = self.next_id
        self.next_id += 1
        self.send({"jsonrpc": "2.0", "id": rid, "method": method, "params": params})
        msg = self.wait_for(lambda m: m.get("id") == rid)
        if "error" in msg:
            raise AssertionError(f"{method} failed: {msg['error']}")
        return msg["result"]

    def read_message(self, timeout: float) -> dict | None:
        deadline = time.monotonic() + timeout
        while True:
            header_end = self.buffer.find(b"\r\n\r\n")
            if header_end >= 0:
                headers = self.buffer[:header_end].decode()
                length = int(headers.split("Content-Length:")[1].split("\r\n")[0])
                start = header_end + 4
                if len(self.buffer) >= start + length:
                    body = self.buffer[start : start + length]
                    self.buffer = self.buffer[start + length :]
                    return json.loads(body)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            ready, _, _ = select.select([self.proc.stdout], [], [], remaining)
            if ready:
                chunk = os.read(self.proc.stdout.fileno(), 65536)
                if not chunk:
                    raise AssertionError("server closed stdout")
                self.buffer += chunk

    def wait_for(self, predicate, timeout: float = 5.0) -> dict:
        for i, msg in enumerate(self.pending):
            if predicate(msg):
                return self.pending.pop(i)
        deadline = time.monotonic() + timeout
        while (remaining := deadline - time.monotonic()) > 0:
            msg = self.read_message(remaining)
            if msg is None:
                break
            if predicate(msg):
                return msg
            self.pending.append(msg)
        raise AssertionError("timed out waiting for a message")

    def diagnostics_for(self, uri: str, version: int | None = None) -> list[dict]:
        def matches(m):
            return (
                m.get("method") == "textDocument/publishDiagnostics"
                and m["params"]["uri"] == uri
                and (version is None or m["params"].get("version") == version)
            )

        return self.wait_for(matches)["params"]["diagnostics"]

    def code_actions(self, uri: str, line: int, character: int) -> list[dict]:
        pos = {"line": line, "character": character}
        return self.request(
            "textDocument/codeAction",
            {
                "textDocument": {"uri": uri},
                "range": {"start": pos, "end": pos},
                "context": {"diagnostics": []},
            },
        )

    def definition(self, uri: str, line: int, character: int):
        return self.request(
            "textDocument/definition",
            {"textDocument": {"uri": uri}, "position": {"line": line, "character": character}},
        )

    def hover(self, uri: str, line: int, character: int):
        return self.request(
            "textDocument/hover",
            {"textDocument": {"uri": uri}, "position": {"line": line, "character": character}},
        )

    def log(self) -> str:
        """everything the server wrote to stderr so far"""
        self.stderr.seek(0)
        return self.stderr.read().decode()

    def change_doc(self, uri: str, version: int, text: str) -> None:
        self.notify(
            "textDocument/didChange",
            {"textDocument": {"uri": uri, "version": version}, "contentChanges": [{"text": text}]},
        )

    def shutdown(self) -> int:
        self.request("shutdown", None)
        self.notify("exit", None)
        return self.proc.wait(timeout=5)


def open_doc(client: Client, path: Path, text: str | None = None) -> str:
    uri = path.as_uri()
    client.notify(
        "textDocument/didOpen",
        {
            "textDocument": {
                "uri": uri,
                "languageId": "bear",
                "version": 1,
                "text": path.read_text() if text is None else text,
            }
        },
    )
    return uri


def position_of(text: str, needle: str, occurrence: int = 0) -> tuple[int, int]:
    """(line, byte column) of the start of the n-th occurrence of needle"""
    at = -1
    for _ in range(occurrence + 1):
        at = text.index(needle, at + 1)
    line = text.count("\n", 0, at)
    col = at - (text.rfind("\n", 0, at) + 1)
    return line, len(text[at - col : at].encode())


def single_location(result) -> dict:
    if isinstance(result, list):
        assert len(result) == 1, result
        return result[0]
    assert result is not None, "no definition found"
    return result


TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


@test
def clean_file_has_no_diagnostics(server):
    c = Client(server, ["utf-8", "utf-16"])
    assert c.encoding == "utf-8"
    uri = open_doc(c, MAIN)
    assert c.diagnostics_for(uri) == []
    assert c.shutdown() == 0


@test
def go_to_definition(server):
    c = Client(server, ["utf-8"])
    text = MAIN.read_text()
    uri = open_doc(c, MAIN)
    c.diagnostics_for(uri)

    cases = [
        # (needle in main.br, occurrence, target file, target text, target occurrence)
        ("add(GLOBAL", 0, MAIN, "add(i32", 0),  # function
        ("GLOBAL, math", 0, MAIN, "GLOBAL = 12", 0),  # global
        ("math..square", 0, UTIL, "math {", 0),  # module, cursor on the qualifier
        ("square(3)", 0, UTIL, "square(i32", 0),  # scoped function in another file
        ("Point p", 0, UTIL, "Point {", 0),  # type in a local declaration
        ("local + p.x", 0, MAIN, "local = add", 0),  # local variable
        ("a + b", 0, MAIN, "a, i32 b", 0),  # parameter
        ("b;", 0, MAIN, "b) -> i32", 0),  # parameter
    ]
    for needle, occ, target, target_needle, target_occ in cases:
        line, col = position_of(text, needle, occ)
        loc = single_location(c.definition(uri, line, col + 1))
        target_text = target.read_text()
        want = position_of(target_text, target_needle, target_occ)
        got = (loc["range"]["start"]["line"], loc["range"]["start"]["character"])
        assert loc["uri"] == target.as_uri(), (needle, loc)
        assert got == want, (needle, got, want)

    # member access and keywords resolve to nothing rather than to something wrong
    line, col = position_of(text, "x;", 0)
    assert c.definition(uri, line, col) is None
    line, col = position_of(text, "return a", 0)
    assert c.definition(uri, line, col) is None
    assert c.shutdown() == 0


@test
def hover_shows_preview_and_doc_comments(server):
    c = Client(server, ["utf-8"])
    text = MAIN.read_text()
    uri = open_doc(c, MAIN)
    c.diagnostics_for(uri)

    def check(hover, needle, canonical, decl, doc):
        """a bear code block previewing the definition, then its doc comments if it has any"""
        assert hover is not None, needle
        assert hover["contents"]["kind"] == "markdown", (needle, hover)
        value = hover["contents"]["value"]
        preview = f"```bear\n// canonical name: {canonical}\n{decl}"
        assert value.startswith(preview), (needle, value)
        assert value.endswith("\n```" + (f"\n\n{doc}" if doc else "")), (needle, value)

    cases = [
        # (needle in main.br, canonical name, start of the declaration, doc comment)
        ("add(GLOBAL", "add", "fn add(i32, i32) -> i32", "adds two numbers"),
        ("add(i32", "add", "fn add(i32, i32) -> i32", "adds two numbers"),  # the declaration
        (
            "square(3)",
            "math..square",
            "fn square(i32) -> i32",  # the canonical name is only in the comment
            "squares a number\n\noverflows like any other i32 multiplication",
        ),
        ("Point p", "Point", "struct Point {", "a point on the integer grid"),
        ("GLOBAL, math", "GLOBAL", "i32 GLOBAL", None),
        ("math..square", "math", "mod math", None),
        ("local + p.x", "local", "i32 local", None),
    ]
    for needle, canonical, decl, doc in cases:
        line, col = position_of(text, needle)
        hover = c.hover(uri, line, col + 1)
        check(hover, needle, canonical, decl, doc)
        name_len = len(needle.split("(")[0].split(" ")[0].split(",")[0].split(".")[0])
        assert hover["range"] == {
            "start": {"line": line, "character": col},
            "end": {"line": line, "character": col + name_len},
        }, (needle, hover)

    # member accesses and keywords resolve to nothing
    for needle in ("x;", "return a"):
        line, col = position_of(text, needle)
        assert c.hover(uri, line, col) is None, needle

    # a doc comment only documents the line right after it, and //// isn't one
    edited = text.replace("i32 GLOBAL", "/// separated\n\ni32 GLOBAL").replace(
        "/// adds", "//// adds"
    )
    c.change_doc(uri, 2, edited)
    c.diagnostics_for(uri, 2)
    for needle, canonical, decl in (
        ("GLOBAL, math", "GLOBAL", "i32 GLOBAL"),
        ("add(GLOBAL", "add", "fn add(i32, i32) -> i32"),
    ):
        line, col = position_of(edited, needle)
        check(c.hover(uri, line, col), needle, canonical, decl, None)
    assert c.shutdown() == 0


@test
def parser_and_hir_diagnostics_update_on_change(server):
    c = Client(server, ["utf-8"])
    text = MAIN.read_text()
    uri = open_doc(c, MAIN)
    assert c.diagnostics_for(uri, 1) == []

    # hir diagnostic: an undefined type
    broken = text.replace("i32 local", "Nope local")
    c.notify(
        "textDocument/didChange",
        {"textDocument": {"uri": uri, "version": 2}, "contentChanges": [{"text": broken}]},
    )
    diags = c.diagnostics_for(uri, 2)
    assert any(d["severity"] == 1 for d in diags), diags
    line, col = position_of(broken, "Nope")
    assert any(
        d["range"]["start"] == {"line": line, "character": col} for d in diags
    ), diags

    # parser diagnostic, sent as an incremental edit: delete the ';' after `return a + b`
    line, col = position_of(broken, "b;")
    c.notify(
        "textDocument/didChange",
        {
            "textDocument": {"uri": uri, "version": 3},
            "contentChanges": [
                {
                    "range": {
                        "start": {"line": line, "character": col + 1},
                        "end": {"line": line, "character": col + 2},
                    },
                    "text": "",
                }
            ],
        },
    )
    diags = c.diagnostics_for(uri, 3)
    assert any(d["severity"] == 1 for d in diags), diags

    # back to the original text, everything clears
    c.notify(
        "textDocument/didChange",
        {"textDocument": {"uri": uri, "version": 4}, "contentChanges": [{"text": text}]},
    )
    assert c.diagnostics_for(uri, 4) == []
    assert c.shutdown() == 0


@test
def unsaved_imported_buffer_is_used(server):
    c = Client(server, ["utf-8"])
    main_uri = open_doc(c, MAIN)
    assert c.diagnostics_for(main_uri) == []
    # rename Point in the (unsaved) util buffer: main.br's analysis must see it immediately
    util_uri = open_doc(c, UTIL, UTIL.read_text().replace("struct Point", "struct Pt"))
    diags = c.diagnostics_for(main_uri)
    assert any(d["severity"] == 1 for d in diags), diags
    c.diagnostics_for(util_uri)
    # closing the buffer falls back to the file on disk
    c.notify("textDocument/didClose", {"textDocument": {"uri": util_uri}})
    assert c.diagnostics_for(util_uri) == []
    assert c.diagnostics_for(main_uri) == []
    assert c.shutdown() == 0


@test
def related_information_across_files(server):
    c = Client(server, ["utf-8"])
    text = MAIN.read_text() + "\nstruct Point {\n    i32 z;\n}\n"
    uri = open_doc(c, MAIN, text)
    diags = c.diagnostics_for(uri)
    redefs = [d for d in diags if d.get("relatedInformation")]
    assert redefs, diags
    related = redefs[0]["relatedInformation"][0]
    assert related["location"]["uri"] == UTIL.as_uri(), related
    assert c.shutdown() == 0


@test
def chained_notes_are_only_related_information(server):
    c = Client(server, ["utf-8"])
    text = MAIN.read_text() + DUPLICATE_STRUCT
    uri = open_doc(c, MAIN, text)
    diags = c.diagnostics_for(uri)
    redefs = [d for d in diags if d.get("relatedInformation")]
    assert redefs, diags
    # the "previous definition" note points into this file, but isn't published on its own
    assert redefs[0]["relatedInformation"][0]["location"]["uri"] == uri, redefs
    assert all(d["severity"] in (1, 2) for d in diags), diags
    assert c.shutdown() == 0


DUPLICATE_STRUCT = "\nstruct Dup {\n    i32 a;\n}\nstruct Dup {\n    i32 b;\n}\n"
SHOW_DOCUMENT = {"window": {"showDocument": {"support": True}}}


@test
def code_action_goes_to_chained_diagnostic(server):
    c = Client(server, ["utf-8"], SHOW_DOCUMENT)
    text = MAIN.read_text() + DUPLICATE_STRUCT
    uri = open_doc(c, MAIN, text)
    diags = c.diagnostics_for(uri)
    [primary] = [d for d in diags if d.get("relatedInformation")]
    [related] = primary["relatedInformation"]
    start = primary["range"]["start"]

    actions = c.code_actions(uri, start["line"], start["character"])
    assert len(actions) == 1, actions
    command = actions[0]["command"]
    assert command["arguments"] == [related["location"]], actions
    assert related["message"] in actions[0]["title"], actions

    # running it asks the client to show the related location
    rid = c.next_id
    c.next_id += 1
    c.send({"jsonrpc": "2.0", "id": rid, "method": "workspace/executeCommand", "params": command})
    show = c.wait_for(lambda m: m.get("method") == "window/showDocument")
    assert show["params"]["uri"] == related["location"]["uri"], show
    assert show["params"]["selection"] == related["location"]["range"], show
    c.send({"jsonrpc": "2.0", "id": show["id"], "result": {"success": True}})
    assert c.wait_for(lambda m: m.get("id") == rid)["result"] is None

    # nothing to jump to from the note's own location, it's where the only target is
    note = related["location"]["range"]["start"]
    assert c.code_actions(uri, note["line"], note["character"]) == []
    # nor away from any diagnostic
    assert c.code_actions(uri, 0, 0) == []
    assert c.shutdown() == 0


@test
def no_code_actions_without_show_document(server):
    c = Client(server, ["utf-8"])
    uri = open_doc(c, MAIN, MAIN.read_text() + DUPLICATE_STRUCT)
    diags = c.diagnostics_for(uri)
    [primary] = [d for d in diags if d.get("relatedInformation")]
    start = primary["range"]["start"]
    assert c.code_actions(uri, start["line"], start["character"]) == []
    assert c.shutdown() == 0


def apply_edit(text: str, uri: str, edit: dict) -> str:
    """text with a WorkspaceEdit's (utf-8 positioned) changes to uri applied"""
    lines = text.encode().split(b"\n")

    def offset(pos: dict) -> int:
        return sum(len(line) + 1 for line in lines[: pos["line"]]) + pos["character"]

    data = text.encode()
    changes = sorted(edit["changes"][uri], key=lambda e: offset(e["range"]["start"]), reverse=True)
    for change in changes:
        start, end = offset(change["range"]["start"]), offset(change["range"]["end"])
        data = data[:start] + change["newText"].encode() + data[end:]
    return data.decode()


BAD_EXTERN = "\nextern oopsie {\n}\n"


@test
def code_action_fixes_from_chained_diagnostics(server):
    # edits don't need window/showDocument
    c = Client(server, ["utf-8"])
    text = MAIN.read_text() + BAD_EXTERN
    uri = open_doc(c, MAIN, text)
    [diag] = c.diagnostics_for(uri)
    start = diag["range"]["start"]
    actions = c.code_actions(uri, start["line"], start["character"])
    assert [a["title"] for a in actions] == ["replace with `C`", "remove `oopsie`"], actions
    assert all(a["kind"] == "quickfix" and a["diagnostics"] == [diag] for a in actions), actions

    replace, remove = (apply_edit(text, uri, a["edit"]) for a in actions)
    assert replace == MAIN.read_text() + "\nextern C {\n}\n", replace
    assert remove == MAIN.read_text() + "\nextern  {\n}\n", remove

    # the replacement actually fixes it
    c.change_doc(uri, 2, replace)
    assert c.diagnostics_for(uri, 2) == []
    assert c.code_actions(uri, start["line"], start["character"]) == []
    assert c.shutdown() == 0


@test
def code_action_fix_comes_before_go_to(server):
    c = Client(server, ["utf-8"], SHOW_DOCUMENT)
    text = MAIN.read_text() + "\ncompt i32 ARROW = Point{.x = 1, .y = 2}->x;\n"
    uri = open_doc(c, MAIN, text)
    [diag] = c.diagnostics_for(uri)
    start = diag["range"]["start"]
    fix, go_to = c.code_actions(uri, start["line"], start["character"])
    assert fix["title"] == "replace with `.`", fix
    assert "compile-time expressions" in go_to["title"] and "command" in go_to, go_to
    fixed = apply_edit(text, uri, fix["edit"])
    assert fixed.endswith("Point{.x = 1, .y = 2}.x;\n"), fixed
    assert c.shutdown() == 0


@test
def no_code_action_for_non_symbol_replacement(server):
    # "replace with `i32`" names a type rather than holding source text, so it has no edit
    c = Client(server, ["utf-8"])
    uri = open_doc(c, MAIN, MAIN.read_text() + "\nstruct S {\n    var x = 1;\n}\n")
    [diag] = c.diagnostics_for(uri)
    assert "replace with" in diag["relatedInformation"][0]["message"], diag
    start = diag["range"]["start"]
    assert c.code_actions(uri, start["line"], start["character"]) == []
    assert c.shutdown() == 0


def start_of(diag: dict) -> tuple[int, int]:
    start = diag["range"]["start"]
    return start["line"], start["character"]


@test
def compt_call_error_is_reported_at_the_call(server):
    c = Client(server, ["utf-8"])
    text = MAIN.read_text() + "\ncompt fn boom(i32 v) -> i32 => v / 0;\ncompt i32 BOOM = boom(3);\n"
    uri = open_doc(c, MAIN, text)
    diags = c.diagnostics_for(uri)
    [diag] = [d for d in diags if start_of(d) == position_of(text, "boom(3)")]
    assert diag["severity"] == 1, diag
    assert diag["message"].startswith("dividing by zero"), diag
    assert "called here" in diag["message"], diag
    # the error inside the function's body is still one jump away
    [body] = diag["relatedInformation"]
    assert (body["location"]["range"]["start"]["line"], body["location"]["range"]["start"]["character"]) == position_of(text, "0;"), body
    # and it's reported inside the body too, pointing back at the call
    [in_body] = [d for d in diags if start_of(d) == position_of(text, "0;")]
    assert in_body["message"].startswith("dividing by zero"), in_body
    assert in_body["severity"] == 1, in_body
    assert any(r["location"]["range"] == diag["range"] for r in in_body["relatedInformation"]), in_body
    assert len(diags) == 2, diags
    assert c.shutdown() == 0


GENERIC_ERROR = """
struct Arr<T> {
    T val;
    Nope bad;
}

struct Pair<T> {
    Arr<T> inner;
}

fn make_pair() {
    var a = Pair::<i32>{};
}
"""


@test
def generic_error_is_reported_where_it_was_instantiated(server):
    c = Client(server, ["utf-8"])
    text = MAIN.read_text() + GENERIC_ERROR
    uri = open_doc(c, MAIN, text)
    diags = c.diagnostics_for(uri)
    undefineds = [d for d in diags if d["message"].startswith("use of undefined type")]
    assert len(undefineds) == 2, diags
    # at the outermost instantiation, which is in the user's own code
    [undefined] = [d for d in undefineds if start_of(d) == position_of(text, "i32>{}")]
    related = [(r["location"]["range"]["start"]["line"], r["location"]["range"]["start"]["character"]) for r in undefined["relatedInformation"]]
    assert position_of(text, "Nope") in related, undefined
    assert position_of(text, "T> inner") in related, undefined
    # it's reported inside the generic's body too, pointing out to the instantiations
    [in_body] = [d for d in diags if start_of(d) == position_of(text, "Nope")]
    assert in_body["message"] == undefined["message"].split("\n")[0], in_body
    in_body_related = [r["location"]["range"] for r in in_body["relatedInformation"]]
    assert undefined["range"] in in_body_related, in_body
    assert c.shutdown() == 0


CONTRACT_MISMATCH = """
contract Shape {
    fn sides() -> i32;
    fn scale(i32 by) -> i32;
}

struct Square has Shape {
    i32 len;

    fn sides() -> u8 {
        return 4;
    }
    fn scale(u8 by) -> i32 {
        return 1;
    }
}
"""


@test
def help_elsewhere_is_also_reported_on_its_own(server):
    c = Client(server, ["utf-8"])
    text = MAIN.read_text() + CONTRACT_MISMATCH
    uri = open_doc(c, MAIN, text)
    diags = c.diagnostics_for(uri)
    errors = [d for d in diags if d["severity"] == 1]
    hints = [d for d in diags if d["severity"] == 4]
    assert len(errors) == 2 and len(hints) == 2, diags
    sides_error = next(d for d in errors if "`sides`" in d["message"])
    scale_error = next(d for d in errors if "`scale`" in d["message"])

    # each help sits where it applies, pointing back at the error it belongs to
    sides_help = next(d for d in hints if start_of(d) == position_of(text, "u8 {"))
    scale_help = next(d for d in hints if start_of(d) == position_of(text, "u8 by"))
    for help, error in ((sides_help, sides_error), (scale_help, scale_error)):
        assert help["message"] == "replace with `i32`", help
        [parent] = help["relatedInformation"]
        assert parent["location"]["range"] == error["range"], (help, error)
        assert parent["message"] == error["message"], (help, error)

    # and is still related information on that error
    assert any(
        r["message"] == "replace with `i32`"
        and r["location"]["range"] == sides_help["range"]
        for r in sides_error["relatedInformation"]
    ), sides_error
    assert c.shutdown() == 0


@test
def duplicate_diagnostic_chains_are_reported_once(server):
    # each instantiation of W re-emits the same chain
    c = Client(server, ["utf-8"])
    text = "struct W<T> { compt var v = W..v::<W::T>; }\n\nW<i32> _0 = {};\n"
    uri = open_doc(c, MAIN, text)
    diags = c.diagnostics_for(uri)
    assert diags, diags
    keys = [json.dumps(d, sort_keys=True) for d in diags]
    assert len(keys) == len(set(keys)), diags
    assert len(diags) < 10, diags
    assert c.shutdown() == 0


@test
def diagnostics_are_in_source_order(server):
    c = Client(server, ["utf-8"])
    text = MAIN.read_text() + CONTRACT_MISMATCH + "\nfn broken() -> i32 {\n    return 1\n}\n"
    uri = open_doc(c, MAIN, text)
    diags = c.diagnostics_for(uri)
    starts = [start_of(d) for d in diags]
    assert starts == sorted(starts), diags
    assert c.shutdown() == 0


def make_project(bearls: str | None) -> Path:
    """a copy of the fixtures plus lone.br (which nothing imports) and, optionally, a .bearls"""
    # resolved since the server logs canonical paths (macOS's /var is a symlink to /private/var)
    root = Path(tempfile.mkdtemp(prefix="bearls-project-")).resolve()
    atexit.register(shutil.rmtree, root, ignore_errors=True)
    for fixture in (MAIN, UTIL):
        shutil.copy(fixture, root / fixture.name)
    (root / "lone.br").write_text("fn lone() -> i32 {\n    return 1;\n}\n")
    if bearls is not None:
        (root / ".bearls").write_text(bearls)
    return root


def own_analyses(c: Client, path: Path) -> int:
    """how many times path was analyzed on its own rather than as part of a project"""
    return c.log().count(f"analyzed {path} (")


@test
def project_file_shares_one_analysis(server):
    # a leading "bearc" and relative paths (the server runs in /) are both handled
    root = make_project("# the whole project\nargs=bearc main.br -I . --warn-cyclic-import\n")
    main, util, lone = root / "main.br", root / "util.br", root / "lone.br"
    c = Client(server, ["utf-8"])

    # util.br is imported by the project's root, so it's covered by the project's analysis even
    # though main.br isn't open
    util_uri = open_doc(c, util)
    assert c.diagnostics_for(util_uri) == []
    assert "analyzed project" in c.log(), c.log()
    assert own_analyses(c, util) == 0, c.log()
    line, col = position_of(util.read_text(), "Point {")
    assert c.definition(util_uri, line, col + 1) is not None

    # lone.br isn't reachable from main.br, so it gets an analysis of its own
    lone_uri = open_doc(c, lone)
    assert c.diagnostics_for(lone_uri) == []
    assert own_analyses(c, lone) == 1, c.log()

    # until main.br imports it
    main_uri = open_doc(c, main)
    c.diagnostics_for(main_uri)
    c.change_doc(main_uri, 2, 'import "lone.br";\n' + main.read_text())
    c.diagnostics_for(main_uri, version=2)
    c.diagnostics_for(lone_uri)
    c.change_doc(lone_uri, 2, "fn lone() -> i32 {\n    Nope x = 1;\n    return x;\n}\n")
    assert c.diagnostics_for(lone_uri, version=2), "lone.br's error wasn't reported"
    assert own_analyses(c, lone) == 1, c.log()
    assert own_analyses(c, main) == 0, c.log()

    # and back on its own once it isn't
    c.change_doc(main_uri, 3, main.read_text())
    c.diagnostics_for(main_uri, version=3)
    assert c.diagnostics_for(lone_uri), "lone.br's error wasn't reported"
    assert own_analyses(c, lone) == 2, c.log()
    assert c.shutdown() == 0


def make_repo(bearls: str) -> tuple[Path, Path]:
    """(repo, project) laid out like bear-lang's tests/projects/04: bearc runs from the repository
    root and finds its input file through -I, so nothing in .bearls is relative to .bearls itself"""
    # resolved since the server logs canonical paths (macOS's /var is a symlink to /private/var)
    repo = Path(tempfile.mkdtemp(prefix="bearls-repo-")).resolve()
    atexit.register(shutil.rmtree, repo, ignore_errors=True)
    project = repo / "tests" / "projects" / "04"
    project.mkdir(parents=True)
    (repo / "lib").mkdir()
    shutil.copy(MAIN, project / "main.br")
    shutil.copy(UTIL, repo / "lib" / "util.br")
    (project / ".bearls").write_text(bearls)
    return repo, project


def assert_repo_project_works(c: Client, repo: Path, project: Path) -> None:
    uri = open_doc(c, project / "main.br")
    assert c.diagnostics_for(uri) == []
    assert f"(from {repo})" in c.log(), c.log()
    line, col = position_of((project / "main.br").read_text(), "square(3)")
    loc = single_location(c.definition(uri, line, col + 1))
    assert loc["uri"] == (repo / "lib" / "util.br").as_uri(), loc


@test
def project_paths_are_relative_to_the_workspace_folder(server):
    repo, project = make_repo("args = main.br -I tests/projects/04 -I lib\n")
    c = Client(server, ["utf-8"], root=repo)
    assert_repo_project_works(c, repo, project)
    assert c.shutdown() == 0


@test
def project_cwd_overrides_the_workspace_folder(server):
    # the workspace folder (FIXTURES) doesn't contain the project, cwd is relative to .bearls
    repo, project = make_repo("cwd = ../../..\nargs = main.br -I tests/projects/04 -I lib\n")
    c = Client(server, ["utf-8"])
    assert_repo_project_works(c, repo, project)
    assert c.shutdown() == 0


@test
def project_import_paths_apply_to_own_analyses(server):
    # no args, so no project analysis: main.br stands alone, finding lib/util.br through the
    # .bearls's import-paths (relative to the workspace folder, like args)
    repo, project = make_repo("import-paths = lib\n")
    main = project / "main.br"
    c = Client(server, ["utf-8"], root=repo)
    uri = open_doc(c, main)
    assert c.diagnostics_for(uri) == []
    assert "analyzed project" not in c.log(), c.log()
    assert own_analyses(c, main) == 1, c.log()
    line, col = position_of(main.read_text(), "square(3)")
    loc = single_location(c.definition(uri, line, col + 1))
    assert loc["uri"] == (repo / "lib" / "util.br").as_uri(), loc
    assert c.shutdown() == 0


@test
def missing_project_cwd_is_reported(server):
    repo, project = make_repo("args = main.br -I tests/projects/04 -I lib\ncwd = nowhere\n")
    c = Client(server, ["utf-8"], root=repo)
    open_doc(c, project / "main.br")
    message = c.wait_for(lambda m: m.get("method") == "window/showMessage")
    assert "is not a directory" in message["params"]["message"], message
    assert c.shutdown() == 0


@test
def without_project_file_each_document_stands_alone(server):
    root = make_project(None)
    c = Client(server, ["utf-8"])
    util_uri = open_doc(c, root / "util.br")
    assert c.diagnostics_for(util_uri) == []
    assert "analyzed project" not in c.log(), c.log()
    assert own_analyses(c, root / "util.br") == 1, c.log()
    assert c.shutdown() == 0


@test
def invalid_project_file_is_reported(server):
    root = make_project("args = main.br extra.br\n")
    c = Client(server, ["utf-8"])
    util_uri = open_doc(c, root / "util.br")
    message = c.wait_for(lambda m: m.get("method") == "window/showMessage")
    assert message["params"]["type"] == 1, message
    assert "more than one input file" in message["params"]["message"], message
    # its documents still get analyzed, on their own
    assert c.diagnostics_for(util_uri) == []
    assert own_analyses(c, root / "util.br") == 1, c.log()
    assert c.shutdown() == 0


@test
def utf16_positions(server):
    c = Client(server, ["utf-16"])
    assert c.encoding == "utf-16"
    # 'é' is 2 bytes / 1 utf-16 unit, '😀' is 4 bytes / 2 utf-16 units
    text = MAIN.read_text().replace(
        "    i32 local = add(GLOBAL,", '    str s = "é😀"; Nope local = add(GLOBAL,'
    )
    uri = open_doc(c, MAIN, text)
    diags = c.diagnostics_for(uri)
    line, _ = position_of(text, "Nope")
    line_text = text.splitlines()[line]
    want = len(line_text[: line_text.index("Nope")].encode("utf-16-le")) // 2
    assert any(
        d["range"]["start"] == {"line": line, "character": want} for d in diags
    ), (want, diags)

    # and the other direction: definition from a utf-16 position past the emoji
    col = len(line_text[: line_text.index("add(")].encode("utf-16-le")) // 2
    loc = single_location(c.definition(uri, line, col + 1))
    assert loc["range"]["start"]["line"] == position_of(text, "add(i32")[0], loc
    assert c.shutdown() == 0


@test
def lifecycle_errors(server):
    proc = subprocess.Popen(
        [str(server)], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL
    )
    c = Client.__new__(Client)
    c.proc, c.next_id, c.buffer, c.pending = proc, 1, b"", []
    # requests before initialize are rejected
    c.send({"jsonrpc": "2.0", "id": 7, "method": "textDocument/definition", "params": {}})
    assert c.wait_for(lambda m: m.get("id") == 7)["error"]["code"] == -32002
    # garbage JSON is a parse error, not a crash
    body = b"{nope"
    proc.stdin.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    proc.stdin.flush()
    assert c.wait_for(lambda m: "error" in m and m.get("id") is None)["error"]["code"] == -32700
    # exit without shutdown exits with 1
    c.notify("exit", None)
    assert proc.wait(timeout=5) == 1


@test
def unknown_request(server):
    c = Client(server, ["utf-8"])
    c.send({"jsonrpc": "2.0", "id": 99, "method": "textDocument/nope", "params": {}})
    assert c.wait_for(lambda m: m.get("id") == 99)["error"]["code"] == -32601
    c.notify("$/cancelRequest", {"id": 99})  # ignored
    assert c.shutdown() == 0


def main() -> int:
    server = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else ROOT / "build" / "bearls"
    failed = 0
    for fn in TESTS:
        try:
            fn(server)
            print(f"PASS {fn.__name__}")
        except Exception as e:  # noqa: BLE001
            failed += 1
            print(f"FAIL {fn.__name__}: {e!r}")
    print(f"{len(TESTS) - failed}/{len(TESTS)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
