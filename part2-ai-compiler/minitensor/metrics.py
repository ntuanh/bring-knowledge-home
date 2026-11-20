"""Stage 7: metrics that show what fusion buys.

Input: the loop IR from loopir.py (once without fusion, once with).
Output: a dict of counts. Called by driver.py.

The numbers are static estimates computed from the loop structure, not
measurements: every "load" in the loop IR counts as 4 bytes read and every
"store" as 4 bytes written, multiplied by how often its loops run. Scalars
(acc, v_*) cost nothing because they live in registers.
"""


def count_loads(expr):
    kind = expr[0]
    if kind == "load":
        return 1
    if kind == "max0":
        return count_loads(expr[1])
    if kind == "bin":
        return count_loads(expr[2]) + count_loads(expr[3])
    return 0


def count_traffic(statements, times=1):
    """Returns (loads, stores) executed by the statements."""
    loads = stores = 0
    for stmt in statements:
        if stmt[0] == "for":
            inner_loads, inner_stores = count_traffic(stmt[3], times * stmt[2])
            loads += inner_loads
            stores += inner_stores
        elif stmt[0] == "assign":
            loads += times * count_loads(stmt[2])
        else:
            loads += times * count_loads(stmt[3])
            stores += times
    return loads, stores


def compute(lowered):
    loads = stores = 0
    per_kernel = []
    for kernel in lowered["kernels"]:
        k_loads, k_stores = count_traffic(kernel["body"])
        per_kernel.append({"name": kernel["name"], "bytes_read": 4 * k_loads, "bytes_written": 4 * k_stores})
        loads += k_loads
        stores += k_stores
    intermediates = [b for b in lowered["buffers"] if b["kind"] == "intermediate"]
    return {
        "kernels": len(lowered["kernels"]),
        "intermediate_buffers": len(intermediates),
        "intermediate_bytes": sum(b["bytes"] for b in intermediates),
        "bytes_read": 4 * loads,
        "bytes_written": 4 * stores,
        "bytes_total": 4 * (loads + stores),
        "per_kernel": per_kernel,
    }
