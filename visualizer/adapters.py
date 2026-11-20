"""Compiler Lab protocol and adapters. Compiler commands are server configuration."""

import json
import os
import sys
import tempfile
from pathlib import Path

import pipelines

VERSION = 1
STATUSES = {"ok", "fail", "skip"}
MAX_EVENTS = 20000


def registry():
    entries = [
        {"id": "minic", "name": "Mini C", "description": "C → tokens → AST → symbols → IR → assembly",
         "source": (pipelines.C_EXAMPLES / "01_minimal.c").read_text(encoding="utf-8")},
        {"id": "expression", "name": "Expression compiler (adapter demo)",
         "description": "Integer expressions → AST → stack bytecode → constant folding",
         "source": "2 + 3 * (4 - 1)",
         "command": ["{python}", "{root}/compilers/expression/compiler.py", "{source}"]},
    ]
    config = os.environ.get("COMPILER_ADAPTERS")
    if config:
        try:
            extra = json.loads(Path(config).read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise pipelines.PipelineError(f"Cannot read COMPILER_ADAPTERS: {exc}") from exc
        if not isinstance(extra, list):
            raise pipelines.PipelineError("COMPILER_ADAPTERS must contain a JSON array")
        entries.extend(extra)
    seen = set()
    for entry in entries:
        if not isinstance(entry, dict) or not all(isinstance(entry.get(k), str) and entry[k]
                                                 for k in ("id", "name")):
            raise pipelines.PipelineError("Each compiler needs a string id and name")
        if entry["id"] in seen:
            raise pipelines.PipelineError("Duplicate compiler id: " + entry["id"])
        seen.add(entry["id"])
        if entry["id"] != "minic":
            cmd = entry.get("command")
            if not isinstance(cmd, list) or not cmd or not all(isinstance(a, str) and a for a in cmd):
                raise pipelines.PipelineError("Compiler command must be a non-empty array of strings")
        for key in ("source", "description"):
            if key in entry and not isinstance(entry[key], str):
                raise pipelines.PipelineError(f"Compiler {key} must be a string")
    return entries


def catalog():
    return [{k: v for k, v in entry.items() if k in {"id", "name", "description", "source"}}
            for entry in registry()]


def validate_session(data):
    """Reject malformed adapters at the boundary, before the UI consumes them."""
    def bad(message):
        raise pipelines.PipelineError("Invalid compiler session: " + message)

    if not isinstance(data, dict) or type(data.get("version")) is not int or data["version"] != VERSION:
        bad("version must be 1")
    stages = data.get("stages")
    if not isinstance(stages, list) or not stages:
        bad("stages must be a non-empty array")
    seen, count = set(), 0
    for stage in stages:
        if not isinstance(stage, dict):
            bad("stage must be an object")
        for key in ("id", "label", "help"):
            if not isinstance(stage.get(key), str) or not stage[key]:
                bad(f"stage {key} must be a non-empty string")
        if stage["id"] in seen:
            bad("duplicate stage id")
        seen.add(stage["id"])
        if not isinstance(stage.get("status"), str) or stage["status"] not in STATUSES:
            bad("stage status must be ok, fail or skip")
        events = stage.setdefault("events", [])
        if not isinstance(events, list):
            bad("events must be an array")
        count += len(events)
        if count > MAX_EVENTS:
            bad(f"at most {MAX_EVENTS} events are supported")
        for event in events:
            if not isinstance(event, dict) or not isinstance(event.get("label"), str):
                bad("each event needs a string label")
            loc = event.get("location")
            if loc is not None:
                if not isinstance(loc, dict):
                    bad("location must be an object")
                for key in ("line", "col", "endLine", "endCol"):
                    if key in loc and (type(loc[key]) is not int or loc[key] < 1):
                        bad("source locations use positive 1-based integers")
                if "line" not in loc:
                    bad("location needs line")
    return data


def _event(label, data, line=None, col=None):
    event = {"label": label, "data": data}
    if isinstance(line, int) and line > 0:
        event["location"] = {"line": line}
        if isinstance(col, int) and col > 0:
            event["location"]["col"] = col
            event["location"]["endCol"] = col + max(1, data.get("len", 1))
    return event


def minic_session(source, opt):
    with tempfile.TemporaryDirectory(prefix="cv-lab-") as workdir:
        result = pipelines._run_minic(source, opt, workdir)
    specs = [
        ("lexer", "Lexer", "Nhận diện token từ các ký tự. Mỗi bước chọn một token theo thứ tự source.", "tokens"),
        ("parser", "Parser / AST", "Cấu trúc cú pháp biểu diễn độ ưu tiên và quan hệ cha–con. Các bước duyệt cây kết quả theo preorder, không phải trace nội bộ parser.", "ast"),
        ("sema", "Semantic analysis", "Kiểm tra tên, kiểu và scope. Các bước xem từng scope trong bảng symbol kết quả.", "symbols"),
        ("ir", "IR lowering", "Chuyển AST thành lệnh ba địa chỉ. Mỗi bước xem một lệnh IR đã sinh và dòng source tương ứng.", "ir"),
        ("opt", "Optimization", "Mỗi bước là một biến đổi thực tế được optimizer ghi vào opt_log; xem before và after. O0 bỏ qua pha này.", "ir_opt"),
        ("asm", "Code generation", "Sinh assembly x86-64 từ IR. Đây là kết quả codegen, chưa assemble, link hoặc chạy chương trình.", "asm"),
    ]
    stages = []
    failed = result.get("failed_stage")
    stopped = False
    for sid, label, help_text, key in specs:
        output = result.get(key)
        status = "skip" if stopped or output is None else "ok"
        if sid == failed:
            status, stopped = "fail", True
        events = []
        if sid == "lexer":
            events = [_event(t.get("display", t["kind"]), t, t.get("line"), t.get("col"))
                      for t in output or []]
        elif sid == "parser" and output:
            pending = [output]
            while pending:
                node = pending.pop()
                events.append(_event(node.get("label", node["kind"]),
                                     {k: v for k, v in node.items() if k != "children"}, node.get("line")))
                pending.extend(reversed(node.get("children", [])))
        elif sid == "sema":
            events = [_event(scope.get("label", "Scope"), scope) for scope in output or []]
        elif sid == "ir":
            events = [_event(fn["name"] + ": " + ins["text"], ins, ins.get("line"))
                      for fn in output or [] for ins in fn["instrs"]]
        elif sid == "opt":
            events = [_event(entry["pass"] + " / " + entry["function"], entry)
                      for entry in result.get("opt_log") or []]
        diags = [d for d in result.get("diagnostics", []) if d["stage"] == sid]
        events.extend(_event(d["message"], d, d.get("line"), d.get("col")) for d in diags)
        stages.append({"id": sid, "label": label, "help": help_text, "status": status,
                       "output": output, "events": events})
    return {"version": VERSION, "stages": stages}


def compile_session(compiler, source, opt=0):
    source = pipelines.check_source(source)
    entry = next((item for item in registry() if item["id"] == compiler), None)
    if entry is None:
        raise pipelines.PipelineError("Unknown compiler")
    opt = pipelines._check_opt(opt, {0, 1})
    if compiler == "minic":
        data = minic_session(source, opt)
    else:
        with tempfile.TemporaryDirectory(prefix="cv-adapter-") as workdir:
            path = Path(workdir) / "input.src"
            path.write_text(source, encoding="utf-8")
            replacements = {"{python}": sys.executable, "{root}": str(pipelines.ROOT),
                            "{source}": str(path), "{opt}": str(opt)}
            command = []
            for arg in entry["command"]:
                for token, value in replacements.items():
                    arg = arg.replace(token, value)
                command.append(arg)
            run = pipelines.run_tool(command, workdir, max_output=pipelines.MAX_JSON_BYTES,
                                     display=entry["name"])
            if run["timed_out"] or run["signal"] or run["truncated"]:
                raise pipelines.PipelineError("Compiler adapter timed out, was killed or exceeded output limit")
            try:
                data = json.loads(run["stdout"])
            except ValueError as exc:
                raise pipelines.PipelineError("Compiler adapter did not produce JSON: " + run["stderr"][:500]) from exc
            # Exit 1 may accompany a valid session containing compile diagnostics.
            if run["exit_code"] not in (0, 1):
                raise pipelines.PipelineError("Compiler adapter failed: " + run["stderr"][:500])
    data = validate_session(data)
    data["compiler"] = {"id": entry["id"], "name": entry["name"]}
    return data
