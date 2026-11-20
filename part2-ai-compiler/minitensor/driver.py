"""The minitensor driver: runs every stage and collects the results.

Input: .mt source text. Output: one dict with every stage's result (this is
what `python3 -m minitensor --json` prints and what the visualizer shows).
Called by __main__.py and by the tests.

Stage 6 lives here: the generated C is compiled with `gcc -O2`, run, and its
printed outputs are compared with the reference interpreter. Two variants are
built so their numbers can be compared: "unfused" (one kernel per operation)
and "fused" (after operator fusion).
"""

import os
import subprocess
import tempfile

from . import codegen_c, interp, loopir, metrics, parser, passes
from .errors import MTError
from .graph import build_graph

TOLERANCE = 1e-4
COMPILE_TIMEOUT = 10
RUN_TIMEOUT = 5
SHOWN_VALUES = 64     # values per tensor included in the result


def compile_and_run(c_source, name, workdir):
    """gcc -O2, run, parse `output <name> <count>` blocks. Returns a dict."""
    src = os.path.join(workdir, f"{name}.c")
    exe = os.path.join(workdir, name)
    with open(src, "w", encoding="utf-8") as f:
        f.write(c_source)
    result = {"compile_cmd": f"gcc -O2 -o {name} {name}.c", "run_cmd": f"./{name}", "compiled": False,
              "ran": False, "stderr": "", "outputs": {}}
    try:
        cc = subprocess.run(["gcc", "-O2", "-o", exe, src], capture_output=True, text=True, timeout=COMPILE_TIMEOUT)
    except (OSError, subprocess.TimeoutExpired) as e:
        result["stderr"] = f"gcc failed to run: {e}"
        return result
    if cc.returncode != 0:
        result["stderr"] = cc.stderr[-4000:]
        return result
    result["compiled"] = True
    try:
        run = subprocess.run([exe], capture_output=True, text=True, timeout=RUN_TIMEOUT)
    except (OSError, subprocess.TimeoutExpired) as e:
        result["stderr"] = f"program failed to run: {e}"
        return result
    if run.returncode != 0:
        result["stderr"] = f"program exited with code {run.returncode}: {run.stderr[-2000:]}"
        return result
    result["ran"] = True
    lines = run.stdout.split("\n")
    i = 0
    while i < len(lines):
        parts = lines[i].split()
        if len(parts) == 3 and parts[0] == "output":
            count = int(parts[2])
            result["outputs"][parts[1]] = [float(v) for v in lines[i + 1:i + 1 + count]]
            i += 1 + count
        else:
            i += 1
    return result


def compare(actual, reference):
    """Largest absolute error, and whether every value is within tolerance."""
    if len(actual) != len(reference):
        return float("inf"), False
    worst, ok = 0.0, True
    for a, r in zip(actual, reference):
        err = abs(a - r)
        worst = max(worst, err)
        if err > TOLERANCE * max(1.0, abs(r)):
            ok = False
    return worst, ok


def compile_source(source, filename="model.mt", run=True, workdir=None):
    """Runs the pipeline. Never raises MTError: errors land in result["diagnostics"]."""
    result = {"file": filename, "ast": None, "graph": None, "graph_opt": None, "opt_log": None, "loop_ir": None,
              "c_code": None, "run": None, "metrics": None, "diagnostics": [], "failed_stage": None}
    try:
        # Stage 1: text -> AST
        ast = parser.parse(source)
        result["ast"] = ast

        # Stage 2: AST -> graph IR with shapes
        graph = build_graph(ast)
        result["graph"] = graph.to_dict()
        result["graph"]["text"] = graph.text()

        # Stage 3: graph optimizations
        opt_graph, groups, log = passes.optimize(graph)
        unfused_groups = passes.singleton_groups(opt_graph)
        result["opt_log"] = log

        # Stage 4: lowering to loops, without and with fusion
        lowered = {"unfused": loopir.lower(opt_graph, unfused_groups), "fused": loopir.lower(opt_graph, groups)}
        kernel_of = {name: k["name"] for k in lowered["fused"]["kernels"] for name in k["nodes"]}
        result["graph_opt"] = opt_graph.to_dict()
        result["graph_opt"]["text"] = opt_graph.text()
        result["graph_opt"]["groups"] = [
            {"id": i, "nodes": group, "root": group[-1], "kind": passes.group_kind(opt_graph, group),
             "kernel": kernel_of[group[-1]]} for i, group in enumerate(groups)]
        fused_names = {b["name"] for b in lowered["fused"]["buffers"]}
        result["loop_ir"] = {
            variant: {"text": loopir.program_text(low, opt_graph), "buffers": low["buffers"],
                      "kernels": [{"name": k["name"], "nodes": k["nodes"], "inputs": k["inputs"], "output": k["output"],
                                   "text": loopir.kernel_text(k, opt_graph)} for k in low["kernels"]]}
            for variant, low in lowered.items()}
        result["loop_ir"]["removed_buffers"] = [b for b in lowered["unfused"]["buffers"] if b["name"] not in fused_names]

        # Stage 5: C code
        result["c_code"] = {variant: codegen_c.generate(opt_graph, low, f"{filename}, {variant}")
                            for variant, low in lowered.items()}

        # Stage 7 (static, so it does not need the run): metrics
        result["metrics"] = {variant: metrics.compute(low) for variant, low in lowered.items()}

        # Stage 6: compile, run, and verify against the reference interpreter.
        # The reference runs on the *original* graph, so it also checks the passes.
        if run:
            reference = interp.evaluate(graph)
            ref_outputs = {name: reference[name] for name in graph.outputs}
            run_result = {"tolerance": TOLERANCE, "ok": True,
                          "reference": {name: {"shape": graph.nodes[name].shape, "values": vals[:SHOWN_VALUES],
                                               "count": len(vals)} for name, vals in ref_outputs.items()}}
            ctx = tempfile.TemporaryDirectory(prefix="mt-") if workdir is None else None
            try:
                wd = workdir or ctx.name
                for variant in ("unfused", "fused"):
                    r = compile_and_run(result["c_code"][variant], f"model_{variant}", wd)
                    checks = {}
                    ok = r["ran"]
                    for name, ref in ref_outputs.items():
                        actual = r["outputs"].get(name)
                        if actual is None:
                            checks[name] = {"max_abs_error": None, "ok": False, "values": []}
                            ok = False
                            continue
                        worst, good = compare(actual, ref)
                        checks[name] = {"max_abs_error": worst, "ok": good, "values": actual[:SHOWN_VALUES]}
                        ok = ok and good
                    run_result[variant] = {"compile_cmd": r["compile_cmd"], "run_cmd": r["run_cmd"],
                                           "compiled": r["compiled"], "ran": r["ran"], "stderr": r["stderr"],
                                           "outputs": checks, "ok": ok}
                    run_result["ok"] = run_result["ok"] and ok
            finally:
                if ctx:
                    ctx.cleanup()
            result["run"] = run_result
            if not run_result["ok"]:
                result["failed_stage"] = "run"
    except MTError as e:
        result["diagnostics"].append(e.to_dict())
        result["failed_stage"] = e.stage
    return result
