"""Stage 3: graph optimizations.

Input: a Graph from graph.py. Output: an optimized Graph plus the list of
fusion groups, and a log entry (before -> after) for every change.
Called by driver.py; the result is consumed by loopir.py.

Passes, in order:
  1. dead_node_elimination  - drop nodes that do not reach any output
  2. constant_folding       - evaluate operations whose inputs are all consts
  3. fuse_operators         - decide which operations share one loop nest

A C compiler mostly saves instructions. An AI compiler mostly saves *memory
traffic*: every unfused operation writes a whole intermediate tensor that the
next operation reads back. Fusion keeps that value in a register instead.
"""

from . import interp
from .graph import ELEMENTWISE, Node


def log_entry(log, pass_name, before, after):
    log.append({"pass": pass_name, "before": before, "after": after})


def dead_node_elimination(graph, log):
    """Removes operations and consts that no output depends on.

    Unused inputs and params are kept: they are part of the model's interface.
    """
    live = set()
    stack = list(graph.outputs)
    while stack:
        name = stack.pop()
        if name in live:
            continue
        live.add(name)
        stack.extend(graph.nodes[name].inputs)
    result = graph.copy()
    for name, node in graph.nodes.items():
        if name not in live and node.op not in ("input", "param"):
            log_entry(log, "dead node elimination", node.text(), "(removed: no output depends on it)")
            del result.nodes[name]
    return result


def constant_folding(graph, log):
    """Replaces every operation whose inputs are all consts by a const node."""
    result = graph.copy()
    for node in list(result.nodes.values()):
        if node.is_leaf or not all(result.nodes[i].op == "const" for i in node.inputs):
            continue
        values = interp.eval_node(node, [result.nodes[i].shape for i in node.inputs],
                                  [result.nodes[i].attrs["values"] for i in node.inputs])
        folded = Node(node.name, "const", attrs={"values": values}, shape=node.shape, line=node.line, col=node.col)
        log_entry(log, "constant folding", node.text(), folded.text())
        result.nodes[node.name] = folded   # same key: the definition order is preserved
    return result


def singleton_groups(graph):
    """No fusion: every operation is its own kernel."""
    return [[node.name] for node in graph.op_nodes()]


def fuse_operators(graph, log):
    """Groups operations that can run inside one loop nest.

    Returns a list of groups; each group is a list of node names in
    topological order and its last node (the "root") is the only one whose
    value is written to memory.

    A producer p is fused into its consumer c when all of these hold:
      * c is element-wise (add, sub, mul, relu, scale);
      * p is element-wise, or a matmul (then c becomes its "epilogue");
      * c is the only consumer of p, and p is not a model output, so nobody
        else needs p's buffer;
      * p and c have the same shape, so they share one iteration space;
      * the merged group contains at most one matmul.
    """
    consumers = graph.consumers()
    group_of = {}
    for node in graph.op_nodes():
        group = [node.name]
        if node.op in ELEMENTWISE:
            for inp in dict.fromkeys(node.inputs):   # unique, in order
                producer = graph.nodes[inp]
                if producer.is_leaf or producer.op not in ELEMENTWISE + ("matmul",):
                    continue
                if consumers[inp] != {node.name} or inp in graph.outputs or producer.shape != node.shape:
                    continue
                merged = group_of[inp] + group
                if sum(graph.nodes[n].op == "matmul" for n in merged) > 1:
                    continue
                group = merged
        order = {name: i for i, name in enumerate(graph.nodes)}
        group.sort(key=order.get)
        for name in group:
            group_of[name] = group

    groups = []
    for node in graph.op_nodes():
        group = group_of[node.name]
        if group[-1] == node.name:     # emit each group once, at its root
            groups.append(group)
            if len(group) > 1:
                hidden = group[:-1]
                log_entry(log, "operator fusion",
                          f"{len(group)} kernels: " + "; ".join(graph.nodes[n].text() for n in group),
                          f"1 kernel computing {group[-1]}; intermediate tensor(s) {', '.join(hidden)} "
                          "become scalars inside the loop and are never stored")
    return groups


def group_kind(graph, group):
    ops = [graph.nodes[n].op for n in group]
    if len(group) == 1:
        return "single"
    return "matmul + element-wise epilogue" if "matmul" in ops else "element-wise chain"


def optimize(graph):
    """Runs all passes. Returns (optimized graph, fusion groups, log)."""
    log = []
    g = dead_node_elimination(graph, log)
    g = constant_folding(g, log)
    g = dead_node_elimination(g, log)    # consts that were only used by folded nodes
    groups = fuse_operators(g, log)
    return g, groups, log
