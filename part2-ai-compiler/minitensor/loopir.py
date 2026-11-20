"""Stage 4: lowering the graph to a loop IR.

Input: an optimized Graph and its fusion groups (from passes.py).
Output: a list of kernels. A kernel is one loop nest that reads some buffers
and writes exactly one output buffer. Called by driver.py; consumed by
codegen_c.py (to print C) and metrics.py (to count memory traffic).

This is where the graph ("what to compute") becomes explicit loops, buffers,
loads and stores ("how to compute it"), like Halide's lowering or TVM's TIR.

Loop IR, as plain tuples:

  statements
    ("for", var, extent, [statements])     for var in 0..extent
    ("assign", scalar, expr)               scalar = expr      (a register, not memory)
    ("store", buffer, index, expr)         buffer[index] = expr
  expressions
    ("load", buffer, index)                buffer[index]
    ("var", scalar) | ("const", number)
    ("bin", op, left, right)               op is + - *
    ("max0", expr)                         max(expr, 0)       (relu)

Indexes are strings over loop variables, e.g. "i*16 + j". Buffers are flat
arrays in row-major order.
"""

from .graph import size_of


def kernel_name(index, graph, group):
    return f"k{index}_" + "_".join(graph.nodes[n].op for n in group)


def flat_index(variables, shape):
    """Row-major index expression, e.g. vars (i, j), shape [4, 16] -> "i*16 + j"."""
    terms = []
    for d, var in enumerate(variables):
        stride = size_of(shape[d + 1:])
        terms.append(var if stride == 1 else f"{var}*{stride}")
    return " + ".join(terms) if terms else "0"


def node_expr(node, operand):
    """Expression for one element of an element-wise node. operand(name) gives an input's expression."""
    args = [operand(name) for name in node.inputs]
    if node.op == "add":
        return ("bin", "+", args[0], args[1])
    if node.op == "sub":
        return ("bin", "-", args[0], args[1])
    if node.op == "mul":
        return ("bin", "*", args[0], args[1])
    if node.op == "relu":
        return ("max0", args[0])
    if node.op == "scale":
        return ("bin", "*", args[0], ("const", node.attrs["factor"]))
    raise ValueError(node.op)


def lower_elementwise_group(graph, group):
    """One flat loop over all elements; fused members become scalars v_<name>."""
    root = graph.nodes[group[-1]]
    count = root.size

    def operand(name):
        if name in group:
            return ("var", f"v_{name}")
        size = graph.nodes[name].size
        # A smaller operand is broadcast: it repeats every `size` elements.
        return ("load", name, "i" if size == count else f"i % {size}")

    body = []
    for name in group:
        expr = node_expr(graph.nodes[name], operand)
        body.append(("store", name, "i", expr) if name == root.name else ("assign", f"v_{name}", expr))
    return [("for", "i", count, body)]


def lower_matmul_group(graph, group):
    """matmul plus an optional element-wise epilogue, all inside the (i, j) loops."""
    root = graph.nodes[group[-1]]
    mm = next(graph.nodes[n] for n in group if graph.nodes[n].op == "matmul")
    a, b = (graph.nodes[n] for n in mm.inputs)
    n, k = a.shape
    m = b.shape[1]
    out_index = flat_index(("i", "j"), [n, m])

    def operand(name):
        if name == mm.name:
            return ("var", "acc")
        if name in group:
            return ("var", f"v_{name}")
        size = graph.nodes[name].size
        if size == n * m:
            index = out_index
        elif size == m:
            index = "j"                      # e.g. a bias vector broadcast over rows
        else:
            index = f"({out_index}) % {size}"
        return ("load", name, index)

    inner = [
        ("assign", "acc", ("const", 0.0)),
        ("for", "k", k, [("assign", "acc", ("bin", "+", ("var", "acc"),
                                            ("bin", "*", ("load", a.name, flat_index(("i", "k"), [n, k])),
                                             ("load", b.name, flat_index(("k", "j"), [k, m])))))]),
    ]
    for name in group:
        if name == mm.name:
            if name == root.name:
                inner.append(("store", name, out_index, ("var", "acc")))
            continue
        expr = node_expr(graph.nodes[name], operand)
        inner.append(("store", name, out_index, expr) if name == root.name else ("assign", f"v_{name}", expr))
    return [("for", "i", n, [("for", "j", m, inner)])]


def lower_sum(graph, node):
    """Loops over the kept dimensions; the summed axis is the innermost loop."""
    src = graph.nodes[node.inputs[0]]
    axis = node.attrs["axis"]
    variables = [("k" if d == axis else f"d{d}") for d in range(len(src.shape))]
    kept = [v for d, v in enumerate(variables) if d != axis]
    kept_shape = [e for d, e in enumerate(src.shape) if d != axis]
    body = [
        ("assign", "acc", ("const", 0.0)),
        ("for", "k", src.shape[axis], [("assign", "acc", ("bin", "+", ("var", "acc"),
                                                            ("load", src.name, flat_index(variables, src.shape))))]),
        ("store", node.name, flat_index(kept, kept_shape), ("var", "acc")),
    ]
    for var, extent in reversed(list(zip(kept, kept_shape))):
        body = [("for", var, extent, body)]
    return body


def lower_transpose(graph, node):
    src = graph.nodes[node.inputs[0]]
    n, m = src.shape
    return [("for", "i", n, [("for", "j", m, [
        ("store", node.name, flat_index(("j", "i"), [m, n]), ("load", src.name, flat_index(("i", "j"), [n, m])))])])]


def lower(graph, groups):
    """Graph + groups -> {"kernels": [...], "buffers": [...]}."""
    kernels = []
    for index, group in enumerate(groups):
        root = graph.nodes[group[-1]]
        ops = [graph.nodes[n].op for n in group]
        if "matmul" in ops:
            body = lower_matmul_group(graph, group)
        elif root.op == "sum":
            body = lower_sum(graph, root)
        elif root.op == "transpose":
            body = lower_transpose(graph, root)
        else:
            body = lower_elementwise_group(graph, group)
        inputs = []
        for name in group:
            for inp in graph.nodes[name].inputs:
                if inp not in group and inp not in inputs:
                    inputs.append(inp)
        kernels.append({"name": kernel_name(index, graph, group), "nodes": list(group), "inputs": inputs,
                        "output": root.name, "body": body})

    # Every leaf and every kernel output needs a real buffer in memory.
    buffers = []
    materialized = [n.name for n in graph.nodes.values() if n.is_leaf] + [k["output"] for k in kernels]
    for name in materialized:
        node = graph.nodes[name]
        if node.is_leaf:
            kind = node.op
        else:
            kind = "output" if name in graph.outputs else "intermediate"
        buffers.append({"name": name, "kind": kind, "shape": node.shape, "elements": node.size, "bytes": 4 * node.size,
                        "is_output": name in graph.outputs})
    return {"kernels": kernels, "buffers": buffers}


# ---------------------------------------------------------------- printing

def expr_text(expr, nested=False):
    kind = expr[0]
    if kind == "load":
        return f"{expr[1]}[{expr[2]}]"
    if kind == "var":
        return expr[1]
    if kind == "const":
        return f"{expr[1]:g}" if expr[1] != int(expr[1]) else f"{expr[1]:.1f}"
    if kind == "max0":
        return f"max({expr_text(expr[1])}, 0.0)"
    text = f"{expr_text(expr[2], True)} {expr[1]} {expr_text(expr[3], True)}"
    return f"({text})" if nested else text


def body_text(statements, indent, lines):
    pad = "  " * indent
    for stmt in statements:
        if stmt[0] == "for":
            lines.append(f"{pad}for {stmt[1]} in 0..{stmt[2]}:")
            body_text(stmt[3], indent + 1, lines)
        elif stmt[0] == "assign":
            lines.append(f"{pad}{stmt[1]} = {expr_text(stmt[2])}")
        else:
            lines.append(f"{pad}{stmt[1]}[{stmt[2]}] = {expr_text(stmt[3])}")


def kernel_text(kernel, graph):
    shape = lambda name: f"{name}{graph.nodes[name].shape}"
    lines = [f"kernel {kernel['name']}({', '.join(shape(n) for n in kernel['inputs'])}) -> {shape(kernel['output'])}:"]
    body_text(kernel["body"], 1, lines)
    return "\n".join(lines)


def program_text(lowered, graph):
    lines = ["buffers:"]
    for buf in lowered["buffers"]:
        lines.append(f"  {buf['name']:<10} {buf['kind']:<12} shape {str(buf['shape']):<12} {buf['bytes']} bytes")
    return "\n".join(lines) + "\n\n" + "\n\n".join(kernel_text(k, graph) for k in lowered["kernels"])
