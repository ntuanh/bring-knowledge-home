"""Reference interpreter: evaluates a Graph directly in pure Python.

Role: the "ground truth". It executes the graph one node at a time with the
most obvious code possible, so the compiled C (fused or not) can be checked
against it. passes.py also uses it to evaluate constant subgraphs.
Input: a Graph. Output: dict name -> flat list of floats (row-major order).
Called by driver.py and passes.py.
"""

from .graph import size_of


def input_values(seed, count):
    """Deterministic test data for inputs and params.

    codegen_c.py emits exactly the same formula in C, so both sides compute
    on the same numbers without any data files. Values lie in [-1, 1].
    """
    return [((i * 37 + seed * 101) % 201 - 100) / 100.0 for i in range(count)]


def eval_node(node, shapes, values):
    """Value of one operation node. `values[i]` / `shapes[i]` belong to node.inputs[i]."""
    op = node.op
    if op == "matmul":
        (n, k), (_, m) = shapes
        a, b = values
        out = [0.0] * (n * m)
        for i in range(n):
            for j in range(m):
                acc = 0.0
                for x in range(k):
                    acc += a[i * k + x] * b[x * m + j]
                out[i * m + j] = acc
        return out
    if op in ("add", "sub", "mul"):
        a, b = values
        count = max(len(a), len(b))
        # Broadcasting: the smaller operand repeats, because its shape is a
        # suffix of the larger shape in row-major order.
        fn = {"add": lambda x, y: x + y, "sub": lambda x, y: x - y, "mul": lambda x, y: x * y}[op]
        return [fn(a[i % len(a)], b[i % len(b)]) for i in range(count)]
    if op == "relu":
        return [v if v > 0.0 else 0.0 for v in values[0]]
    if op == "scale":
        factor = node.attrs["factor"]
        return [v * factor for v in values[0]]
    if op == "sum":
        shape, axis = shapes[0], node.attrs["axis"]
        inner = size_of(shape[axis + 1:])      # elements per step along the axis
        extent = shape[axis]
        outer = size_of(shape[:axis])
        out = [0.0] * (outer * inner)
        for o in range(outer):
            for e in range(extent):
                for i in range(inner):
                    out[o * inner + i] += values[0][(o * extent + e) * inner + i]
        return out
    if op == "transpose":
        n, m = shapes[0]
        out = [0.0] * (n * m)
        for i in range(n):
            for j in range(m):
                out[j * n + i] = values[0][i * m + j]
        return out
    raise ValueError(f"cannot evaluate op {op}")


def evaluate(graph):
    """Evaluates every node in definition (topological) order."""
    env = {}
    for node in graph.nodes.values():
        if node.op in ("input", "param"):
            env[node.name] = input_values(node.attrs["seed"], node.size)
        elif node.op == "const":
            env[node.name] = list(node.attrs["values"])
        else:
            env[node.name] = eval_node(node, [graph.nodes[i].shape for i in node.inputs],
                                       [env[i] for i in node.inputs])
    return env
