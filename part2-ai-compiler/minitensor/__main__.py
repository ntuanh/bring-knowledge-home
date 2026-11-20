"""Command line for minitensor.

Usage (from part2-ai-compiler/):
    python3 -m minitensor [--emit=ast|graph|opt|loops|loops-unfused|c|c-unfused|metrics|run] [--json] file.mt

Without --emit it runs everything and prints a short report. With --json it
prints every stage as one JSON object (used by the visualizer).
Exit code: 0 on success, 1 if a stage failed.
"""

import json
import sys

from . import parser
from .driver import compile_source

EMITS = ("ast", "graph", "opt", "loops", "loops-unfused", "c", "c-unfused", "metrics", "run")


def metrics_text(m):
    rows = [("kernels", "kernels"), ("intermediate buffers", "intermediate_buffers"),
            ("intermediate bytes", "intermediate_bytes"), ("bytes read", "bytes_read"),
            ("bytes written", "bytes_written"), ("bytes read + written", "bytes_total")]
    lines = [f"{'':<22}{'unfused':>10}{'fused':>10}"]
    for label, key in rows:
        lines.append(f"{label:<22}{m['unfused'][key]:>10}{m['fused'][key]:>10}")
    return "\n".join(lines)


def run_text(run):
    lines = []
    for variant in ("unfused", "fused"):
        r = run[variant]
        lines.append(f"$ {r['compile_cmd']} && {r['run_cmd']}")
        if r["stderr"]:
            lines.append(r["stderr"])
        for name, check in r["outputs"].items():
            err = "missing" if check["max_abs_error"] is None else f"{check['max_abs_error']:.3g}"
            ref = run["reference"][name]
            lines.append(f"  {variant:<8} output {name} shape {ref['shape']}: max |C - reference| = {err} "
                         f"-> {'match' if check['ok'] else 'MISMATCH'}")
    first = next(iter(run["reference"].items()))
    lines.append(f"reference {first[0]}[0..{min(4, first[1]['count'])}] = "
                 + ", ".join(f"{v:.6g}" for v in first[1]["values"][:4]))
    lines.append("RESULT: " + ("compiled C matches the reference interpreter" if run["ok"] else "MISMATCH"))
    return "\n".join(lines)


def main(argv):
    emit, as_json, path = None, False, None
    for arg in argv:
        if arg == "--json":
            as_json = True
        elif arg.startswith("--emit=") and arg[7:] in EMITS:
            emit = arg[7:]
        elif arg.startswith("-") or path is not None:
            print(__doc__.strip(), file=sys.stderr)
            return 2
        else:
            path = arg
    if path is None:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    with open(path, encoding="utf-8") as f:
        source = f.read()

    need_run = as_json or emit in (None, "run")
    result = compile_source(source, filename=path, run=need_run)
    if as_json:
        print(json.dumps(result))
        return 1 if result["failed_stage"] else 0

    for d in result["diagnostics"]:
        print(f"{path}:{d['line']}:{d['col']}: error: {d['message']}", file=sys.stderr)
    if result["diagnostics"]:
        return 1

    if emit == "ast":
        print("\n".join(parser.statement_text(s) for s in result["ast"]))
    elif emit == "graph":
        print(result["graph"]["text"])
    elif emit == "opt":
        print(result["graph_opt"]["text"])
        print("\nfusion groups:")
        for g in result["graph_opt"]["groups"]:
            print(f"  {g['kernel']}: {', '.join(g['nodes'])}  ({g['kind']})")
        print("\noptimization log:")
        for e in result["opt_log"]:
            print(f"  [{e['pass']}] {e['before']}\n      -> {e['after']}")
        if not result["opt_log"]:
            print("  (no changes)")
    elif emit in ("loops", "loops-unfused"):
        print(result["loop_ir"]["fused" if emit == "loops" else "unfused"]["text"])
    elif emit in ("c", "c-unfused"):
        print(result["c_code"]["fused" if emit == "c" else "unfused"], end="")
    elif emit == "metrics":
        print(metrics_text(result["metrics"]))
    elif emit == "run":
        print(run_text(result["run"]))
    else:
        print(f"{path}: {len(result['graph']['nodes'])} tensors, outputs: {', '.join(result['graph']['outputs'])}")
        print(f"optimizations applied: {len(result['opt_log'])}")
        print(metrics_text(result["metrics"]))
        print(run_text(result["run"]))
    return 1 if result["failed_stage"] else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
