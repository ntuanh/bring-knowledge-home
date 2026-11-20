"""Runs the compilers and the GNU toolchain on user-supplied source, safely.

Role: called by server.py for every API request that compiles or runs code.
Input: source text from the browser. Output: plain dicts that server.py
serializes to JSON.

Safety rules implemented here (the server runs untrusted code):
  * every request works in its own temporary directory, deleted afterwards;
  * every subprocess has a timeout and is started without a shell;
  * user programs additionally run under CPU-time and file-size limits;
  * captured output is capped, and source size is limited.

Tool boundaries (kept visible in the returned data): `minic` only produces
assembly text. `as` produces the object file, and `gcc` (driving `ld`) links
it against libc into the ELF executable.
"""

import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MINIC = ROOT / "build" / "minic"
C_EXAMPLES = ROOT / "part1-c-compiler" / "examples"
AI_DIR = ROOT / "part2-ai-compiler"
AI_EXAMPLES = AI_DIR / "examples"

MAX_SOURCE_BYTES = 64 * 1024
COMPILE_TIMEOUT = 5.0        # seconds, for compilers and binutils
RUN_TIMEOUT = 2.0            # seconds, for the compiled user program
AI_TIMEOUT = 30.0            # seconds, for the whole minitensor pipeline
MAX_OUTPUT_BYTES = 64 * 1024         # cap for program / tool output sent to the UI
MAX_JSON_BYTES = 32 * 1024 * 1024    # cap for compiler JSON (tokens + AST can be large)
RUN_FILE_LIMIT = 1024 * 1024         # RLIMIT_FSIZE for user programs

PRLIMIT = shutil.which("prlimit")


class PipelineError(Exception):
    """A problem the user should see as a readable message (HTTP 400)."""


def check_source(source):
    if not isinstance(source, str):
        raise PipelineError("'source' must be a string")
    if len(source.encode("utf-8")) > MAX_SOURCE_BYTES:
        raise PipelineError(f"source is larger than {MAX_SOURCE_BYTES // 1024} KB")
    if "\x00" in source:
        raise PipelineError("source contains a NUL byte")
    return source


def _read_capped(path, cap):
    with open(path, "rb") as f:
        data = f.read(cap + 1)
    truncated = len(data) > cap
    return data[:cap].decode("utf-8", errors="replace"), truncated


def run_tool(argv, cwd, timeout=COMPILE_TIMEOUT, limit_user_program=False,
             max_output=MAX_OUTPUT_BYTES, env=None, display=None):
    """Run one command (never through a shell) and capture its result.

    stdout and stderr are written to files inside `cwd` rather than pipes, so
    a program that prints forever cannot fill the server's memory; the files
    are read back up to `max_output` bytes.
    """
    argv = [str(a) for a in argv]
    shown = display or " ".join(os.path.basename(argv[0]) if i == 0 and "/" in a and not a.startswith("./") else a
                                for i, a in enumerate(argv))
    real_argv = argv
    if limit_user_program and PRLIMIT:
        real_argv = [PRLIMIT, f"--fsize={RUN_FILE_LIMIT}", f"--cpu={int(timeout) + 1}", "--core=0", "--"] + argv

    out_path = os.path.join(cwd, ".stdout.txt")
    err_path = os.path.join(cwd, ".stderr.txt")
    timed_out = False
    try:
        with open(out_path, "wb") as fo, open(err_path, "wb") as fe:
            proc = subprocess.Popen(real_argv, cwd=cwd, stdin=subprocess.DEVNULL, stdout=fo, stderr=fe,
                                    start_new_session=True, env=env)
            try:
                proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait()
    except FileNotFoundError:
        return {"cmd": shown, "exit_code": None, "stdout": "", "stderr": f"command not found: {argv[0]}",
                "timed_out": False, "signal": None, "truncated": False, "ok": False}

    stdout, t1 = _read_capped(out_path, max_output)
    stderr, t2 = _read_capped(err_path, max_output)
    code = proc.returncode
    sig = None
    if code is not None and code < 0:
        try:
            sig = signal.Signals(-code).name
        except ValueError:
            sig = f"signal {-code}"
    return {
        "cmd": shown,
        "exit_code": None if (timed_out or sig) else code,
        "stdout": stdout,
        "stderr": stderr,
        "timed_out": timed_out,
        "timeout_seconds": timeout if timed_out else None,
        "signal": None if timed_out else sig,
        "truncated": t1 or t2,
        "ok": (code == 0 and not timed_out),
    }


# --------------------------------------------------------------------------
# Health and examples
# --------------------------------------------------------------------------

_tool_versions = None


def tool_versions():
    """First line of `<tool> --version` for every tool the project uses."""
    global _tool_versions
    if _tool_versions is None:
        versions = {}
        with tempfile.TemporaryDirectory(prefix="cv-") as d:
            for tool in ["gcc", "as", "ld", "make", "readelf", "objdump", "nm", "size", "strings", "file"]:
                if not shutil.which(tool):
                    versions[tool] = None
                    continue
                r = run_tool([tool, "--version"], d)
                text = (r["stdout"] or r["stderr"]).strip().splitlines()
                versions[tool] = text[0] if text else "unknown"
        versions["python3"] = "Python " + sys.version.split()[0]
        _tool_versions = versions
    result = dict(_tool_versions)
    result["minic"] = "built" if MINIC.exists() else None
    return result


def list_examples(part):
    """Example programs for the UI dropdown. The title is the first comment line."""
    if part == "c":
        directory, suffix, comment = C_EXAMPLES, ".c", "//"
    elif part == "ai":
        directory, suffix, comment = AI_EXAMPLES, ".mt", "#"
    else:
        raise PipelineError("part must be 'c' or 'ai'")
    examples = []
    if directory.is_dir():
        for path in sorted(directory.glob("*" + suffix)):
            source = path.read_text(encoding="utf-8")
            first = source.splitlines()[0].strip() if source.strip() else ""
            title = first[len(comment):].strip() if first.startswith(comment) else path.stem
            examples.append({"name": path.name, "title": title, "source": source})
    return examples


# --------------------------------------------------------------------------
# Part 1: minic + assemble + link + run
# --------------------------------------------------------------------------

def _run_minic(source, opt, workdir):
    if not MINIC.exists():
        raise PipelineError("build/minic does not exist. Run `make build` first.")
    with open(os.path.join(workdir, "prog.c"), "w", encoding="utf-8") as f:
        f.write(source)
    r = run_tool([MINIC, "--json", f"-O{opt}", "prog.c"], workdir, max_output=MAX_JSON_BYTES,
                 display=f"minic --json -O{opt} prog.c")
    if r["timed_out"]:
        raise PipelineError("minic timed out")
    try:
        data = json.loads(r["stdout"])
    except json.JSONDecodeError:
        raise PipelineError("minic did not produce valid JSON: " + (r["stderr"] or r["stdout"])[:500])
    return data


def _check_opt(opt, allowed):
    try:
        opt = int(opt)
    except (TypeError, ValueError):
        raise PipelineError("'opt' must be a number")
    if opt not in allowed:
        raise PipelineError(f"'opt' must be one of {sorted(allowed)}")
    return opt


def compile_c(source, opt):
    """All minic stages, then assemble (as), link (gcc/ld) and run."""
    source = check_source(source)
    opt = _check_opt(opt, {0, 1})
    with tempfile.TemporaryDirectory(prefix="cv-") as d:
        data = _run_minic(source, opt, d)
        data["minic_cmd"] = f"minic -O{opt} -o prog.s prog.c"
        data["assemble"] = data["link"] = data["run"] = None
        if data.get("failed_stage") is None and data.get("asm"):
            with open(os.path.join(d, "prog.s"), "w", encoding="utf-8") as f:
                f.write(data["asm"])
            data["assemble"] = run_tool(["as", "prog.s", "-o", "prog.o"], d)
            if not data["assemble"]["ok"]:
                data["failed_stage"] = "assemble"
            else:
                data["link"] = run_tool(["gcc", "prog.o", "-o", "prog"], d)
                if not data["link"]["ok"]:
                    data["failed_stage"] = "link"
                else:
                    data["run"] = run_tool(["./prog"], d, timeout=RUN_TIMEOUT, limit_user_program=True)
        return data


# --------------------------------------------------------------------------
# Part 1: the real GCC pipeline inspector
# --------------------------------------------------------------------------

PRINT_SHIM = '#include <stdio.h>\nstatic void print(int x){printf("%d\\n",x);}\n'

# Flags for the side-by-side comparison: they remove unwind tables, stack
# protector code and CET `endbr64` instructions so the listing shows only the
# code that corresponds to the source.
CLEAN_ASM_FLAGS = ["-fno-asynchronous-unwind-tables", "-fno-stack-protector", "-fcf-protection=none"]


def _prototypes(minic_data):
    """Forward declarations for every function, taken from minic's AST.

    minic resolves function names in two passes, so a function may be called
    before it is defined. Real C needs a declaration first, so the shim adds
    prototypes to make the same source acceptable to GCC.
    """
    ast = (minic_data or {}).get("ast")
    if not ast:
        return ""
    lines = []
    for fn in ast.get("children", []):
        if fn.get("kind") != "Function" or fn.get("name") in ("main", "print"):
            continue
        nparams = sum(1 for c in fn.get("children", []) if c.get("kind") == "Param")
        params = ", ".join(["int"] * nparams) or "void"
        lines.append(f"{fn.get('returns', 'int')} {fn['name']}({params});\n")
    return "".join(lines)


def _parse_elf(header, sections, segments, relocs, undefined):
    """Turn readelf text into a small structure for the ELF diagram."""
    info = {"type": None, "entry": None, "machine": None, "class": None, "data": None,
            "num_program_headers": None, "num_section_headers": None,
            "sections": [], "segments": [], "segment_sections": [], "num_relocations": 0,
            "undefined_symbols": []}
    fields = {"Type:": "type", "Entry point address:": "entry", "Machine:": "machine", "Class:": "class",
              "Data:": "data", "Number of program headers:": "num_program_headers",
              "Number of section headers:": "num_section_headers"}
    for line in header.splitlines():
        line = line.strip()
        for prefix, key in fields.items():
            if line.startswith(prefix):
                info[key] = line[len(prefix):].strip()
    sec_re = re.compile(r"^\s*\[\s*(\d+)\]\s+(\S*)\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+"
                        r"([0-9a-f]+)\s+([A-Za-z]*)\s+(\d+)\s+(\d+)\s+(\d+)\s*$")
    for line in sections.splitlines():
        m = sec_re.match(line)
        if m and m.group(3) != "NULL":
            info["sections"].append({"index": int(m.group(1)), "name": m.group(2), "type": m.group(3),
                                     "addr": int(m.group(4), 16), "offset": int(m.group(5), 16),
                                     "size": int(m.group(6), 16), "flags": m.group(8)})
    seg_re = re.compile(r"^\s+([A-Z_]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+"
                        r"0x([0-9a-f]+)\s+([RWE ]{3})\s+0x([0-9a-f]+)")
    in_mapping = False
    for line in segments.splitlines():
        if "Section to Segment mapping" in line:
            in_mapping = True
            continue
        if in_mapping:
            m = re.match(r"^\s+(\d+)\s*(.*)$", line)
            if m:
                info["segment_sections"].append(m.group(2).split())
            continue
        m = seg_re.match(line)
        if m:
            info["segments"].append({"type": m.group(1), "offset": int(m.group(2), 16),
                                     "vaddr": int(m.group(3), 16), "filesz": int(m.group(5), 16),
                                     "memsz": int(m.group(6), 16), "flags": m.group(7).replace(" ", "")})
    info["num_relocations"] = sum(1 for line in relocs.splitlines() if re.match(r"^[0-9a-f]{12,16}\s", line))
    info["undefined_symbols"] = [line.split()[-1] for line in undefined.splitlines() if line.strip()]
    return info


def _inspect_binary(name, workdir):
    """Run the binutils inspection tools on one file (object or executable)."""
    tools = [
        ("file", ["file", name]),
        ("size", ["size", name]),
        ("nm", ["nm", name]),
        ("readelf -h", ["readelf", "-h", name]),
        ("readelf -S", ["readelf", "-S", "-W", name]),
        ("readelf -l", ["readelf", "-l", "-W", name]),
        ("readelf -s", ["readelf", "-s", "-W", name]),
        ("readelf -r", ["readelf", "-r", "-W", name]),
        ("objdump -d", ["objdump", "-d", "-M", "intel", name]),
        ("strings", ["strings", name]),
    ]
    outputs = {}
    for key, argv in tools:
        outputs[key] = run_tool(argv, workdir, max_output=256 * 1024)
    undefined = run_tool(["nm", "-u", name], workdir)
    elf = _parse_elf(outputs["readelf -h"]["stdout"], outputs["readelf -S"]["stdout"],
                     outputs["readelf -l"]["stdout"], outputs["readelf -r"]["stdout"], undefined["stdout"])
    elf["file_size"] = os.path.getsize(os.path.join(workdir, name))
    return {"tools": outputs, "elf": elf}


def gcc_pipeline(source, opt):
    """The real GCC toolchain on the same source: -E, -S, -c, link, inspect, run."""
    source = check_source(source)
    opt = _check_opt(opt, {0, 1, 2, 3})
    with tempfile.TemporaryDirectory(prefix="cv-") as d:
        minic_data = None
        try:
            minic_data = _run_minic(source, 0, d)
        except PipelineError:
            pass
        shim = PRINT_SHIM + _prototypes(minic_data)
        with open(os.path.join(d, "prog.c"), "w", encoding="utf-8") as f:
            f.write(shim + '#line 1 "prog.c"\n' + source)

        result = {"opt": opt, "shim": shim, "ok": False, "failed_step": None,
                  "preprocess": None, "preprocessed": None, "compile": None, "asm": None,
                  "assemble": None, "link": None, "object": None, "executable": None, "run": None,
                  "compare": None}

        def read(name, cap=256 * 1024):
            text, _ = _read_capped(os.path.join(d, name), cap)
            return text

        # A syntax check first, so an invalid program gives one clear error.
        step = run_tool(["gcc", "-E", "prog.c", "-o", "prog.i"], d)
        result["preprocess"] = step
        if not step["ok"]:
            result["failed_step"] = "preprocess"
            return result
        result["preprocessed"] = read("prog.i")

        step = run_tool(["gcc", "-S", "-masm=intel", f"-O{opt}", "prog.c", "-o", "prog.s"], d)
        result["compile"] = step
        if not step["ok"]:
            result["failed_step"] = "compile"
            return result
        result["asm"] = read("prog.s")

        step = run_tool(["gcc", "-c", f"-O{opt}", "prog.c", "-o", "prog.o"], d)
        result["assemble"] = step
        if not step["ok"]:
            result["failed_step"] = "assemble"
            return result
        result["object"] = _inspect_binary("prog.o", d)

        step = run_tool(["gcc", "prog.o", "-o", "prog"], d)
        result["link"] = step
        if not step["ok"]:
            result["failed_step"] = "link"
            return result
        result["executable"] = _inspect_binary("prog", d)
        result["run"] = run_tool(["./prog"], d, timeout=RUN_TIMEOUT, limit_user_program=True)

        # Side-by-side comparison listings at -O0 and -O2 with "clean" flags.
        compare = {"flags": " ".join(CLEAN_ASM_FLAGS)}
        for level in (0, 2):
            out = f"cmp_O{level}.s"
            step = run_tool(["gcc", "-S", "-masm=intel", f"-O{level}"] + CLEAN_ASM_FLAGS + ["prog.c", "-o", out], d)
            compare[f"O{level}"] = {"cmd": step["cmd"], "asm": read(out) if step["ok"] else step["stderr"]}
        result["compare"] = compare
        result["ok"] = True
        return result


# --------------------------------------------------------------------------
# Part 2: minitensor
# --------------------------------------------------------------------------

def compile_ai(source):
    """Run the whole minitensor pipeline in a subprocess (so it can be timed out)."""
    source = check_source(source)
    with tempfile.TemporaryDirectory(prefix="cv-") as d:
        with open(os.path.join(d, "model.mt"), "w", encoding="utf-8") as f:
            f.write(source)
        env = dict(os.environ)
        env["PYTHONPATH"] = str(AI_DIR)
        env["PYTHONDONTWRITEBYTECODE"] = "1"
        r = run_tool([sys.executable, "-m", "minitensor", "--json", "model.mt"], d, timeout=AI_TIMEOUT,
                     max_output=MAX_JSON_BYTES, env=env, display="python3 -m minitensor --json model.mt")
        if r["timed_out"]:
            raise PipelineError(f"minitensor timed out after {AI_TIMEOUT:.0f} s")
        try:
            return json.loads(r["stdout"])
        except json.JSONDecodeError:
            raise PipelineError("minitensor did not produce valid JSON: " + (r["stderr"] or r["stdout"])[-800:])
