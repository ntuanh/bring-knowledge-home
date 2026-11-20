"""Stage 2: graph IR and shape inference.

Input: the AST from parser.py. Output: a Graph, a DAG whose nodes are tensors
(inputs, params, consts) and operations, each with an inferred shape.
Called by driver.py; the Graph is consumed by passes.py, interp.py and
loopir.py.

Unlike the C compiler's IR, there is no control flow here: the whole program
is one expression DAG, and the "types" that matter are tensor shapes.
"""

from .errors import MTError

ELEMENTWISE_BINARY = ("add", "sub", "mul")
ELEMENTWISE = ELEMENTWISE_BINARY + ("relu", "scale")
# op -> number of tensor arguments, number of constant arguments
OP_SIGNATURES = {"matmul": (2, 0), "add": (2, 0), "sub": (2, 0), "mul": (2, 0), "relu": (1, 0),
                 "scale": (1, 1), "sum": (1, 1), "transpose": (1, 0)}
LEAF_KINDS = ("input", "param", "const")

MAX_NODES = 200
MAX_RANK = 4
MAX_ELEMENTS = 65536          # per tensor
MAX_MATMUL_WORK = 2_000_000   # multiply-adds per matmul (the reference interpreter is pure Python)


def size_of(shape):
    n = 1
    for d in shape:
        n *= d
    return n


class Node:
    def __init__(self, name, op, inputs=(), attrs=None, shape=None, line=0, col=0):
        self.name = name
        self.op = op                  # "input", "param", "const", or an operation name
        self.inputs = list(inputs)    # names of the tensors this node reads
        self.attrs = dict(attrs or {})  # scale: factor; sum: axis; const: values; input/param: seed
        self.shape = list(shape or [])
        self.line = line
        self.col = col

    @property
    def is_leaf(self):
        return self.op in LEAF_KINDS

    @property
    def size(self):
        return size_of(self.shape)

    def copy(self):
        return Node(self.name, self.op, self.inputs, self.attrs, self.shape, self.line, self.col)

    def text(self):
        """e.g. `h = matmul(x, w) : [4, 16]`"""
        if self.op in ("input", "param"):
            return f"{self.op} {self.name} : {self.shape}"
        if self.op == "const":
            values = self.attrs["values"]
            shown = ", ".join(f"{v:g}" for v in values[:6]) + (", ..." if len(values) > 6 else "")
            return f"const {self.name} : {self.shape} = [{shown}]"
        args = list(self.inputs)
        if self.op == "scale":
            args.append(f"{self.attrs['factor']:g}")
        if self.op == "sum":
            args.append(str(self.attrs["axis"]))
        return f"{self.name} = {self.op}({', '.join(args)}) : {self.shape}"

    def to_dict(self):
        attrs = {k: v for k, v in self.attrs.items() if k != "values"}
        if self.op == "const":
            attrs["values"] = self.attrs["values"][:64]
            attrs["num_values"] = len(self.attrs["values"])
        return {"name": self.name, "op": self.op, "kind": self.op if self.is_leaf else "op",
                "inputs": self.inputs, "attrs": attrs, "shape": self.shape, "text": self.text(),
                "line": self.line, "col": self.col}


class Graph:
    def __init__(self):
        self.nodes = {}     # name -> Node, in definition order (which is a topological order)
        self.outputs = []   # names

    def add(self, node):
        self.nodes[node.name] = node

    def copy(self):
        g = Graph()
        for node in self.nodes.values():
            g.add(node.copy())
        g.outputs = list(self.outputs)
        return g

    def op_nodes(self):
        return [n for n in self.nodes.values() if not n.is_leaf]

    def consumers(self):
        """name -> set of node names that read it."""
        users = {name: set() for name in self.nodes}
        for node in self.nodes.values():
            for inp in node.inputs:
                users[inp].add(node.name)
        return users

    def text(self):
        lines = [node.text() for node in self.nodes.values()]
        lines += [f"output {name}" for name in self.outputs]
        return "\n".join(lines)

    def to_dict(self):
        return {"nodes": [n.to_dict() for n in self.nodes.values()], "outputs": list(self.outputs)}


def broadcast_shape(a, b):
    """Result shape of an element-wise op, or None if the shapes do not fit.

    Rule (simpler than NumPy's): the shapes are equal, or the smaller one
    equals the trailing dimensions of the larger one ([4,16] with [16]).
    """
    big, small = (a, b) if len(a) >= len(b) else (b, a)
    if big[len(big) - len(small):] == small:
        return list(big)
    return None


def infer_shape(op, shapes, attrs, where):
    """Shape of the result of `op` applied to tensors with the given shapes."""
    line, col = where
    if op == "matmul":
        a, b = shapes
        if len(a) != 2 or len(b) != 2:
            raise MTError("graph", f"matmul {a} x {b}: both operands must be 2-D matrices", line, col)
        if a[1] != b[0]:
            raise MTError("graph", f"matmul {a} x {b}: inner dimensions {a[1]} != {b[0]}", line, col)
        if a[0] * a[1] * b[1] > MAX_MATMUL_WORK:
            raise MTError("graph", f"matmul {a} x {b} is too large for this learning tool", line, col)
        return [a[0], b[1]]
    if op in ELEMENTWISE_BINARY:
        a, b = shapes
        result = broadcast_shape(a, b)
        if result is None:
            raise MTError("graph", f"{op} {a} and {b}: shapes are not compatible (they must be equal, or the "
                          "smaller shape must match the trailing dimensions of the larger one)", line, col)
        return result
    if op in ("relu", "scale"):
        return list(shapes[0])
    if op == "sum":
        a = shapes[0]
        axis = attrs["axis"]
        if not 0 <= axis < len(a):
            raise MTError("graph", f"sum over axis {axis} of a tensor with shape {a}: axis must be between 0 and "
                          f"{len(a) - 1}", line, col)
        return (a[:axis] + a[axis + 1:]) or [1]
    if op == "transpose":
        a = shapes[0]
        if len(a) != 2:
            raise MTError("graph", f"transpose {a}: operand must be a 2-D matrix", line, col)
        return [a[1], a[0]]
    raise MTError("graph", f"unknown operation '{op}'", line, col)


def check_shape(shape, name, line, col):
    if len(shape) > MAX_RANK:
        raise MTError("graph", f"tensor '{name}' has {len(shape)} dimensions; at most {MAX_RANK} are supported", line, col)
    if any(d < 1 for d in shape):
        raise MTError("graph", f"tensor '{name}' has shape {shape}: every dimension must be at least 1", line, col)
    if size_of(shape) > MAX_ELEMENTS:
        raise MTError("graph", f"tensor '{name}' has {size_of(shape)} elements; the limit is {MAX_ELEMENTS}", line, col)


def build_graph(statements):
    """AST -> Graph with shapes. Raises MTError on the first semantic error."""
    graph = Graph()
    seed = 0
    for stmt in statements:
        line, col = stmt["line"], stmt["col"]
        kind = stmt["kind"]
        if kind == "output":
            if stmt["name"] not in graph.nodes:
                raise MTError("graph", f"output '{stmt['name']}' is not defined", line, stmt.get("name_col", col))
            if stmt["name"] in graph.outputs:
                raise MTError("graph", f"'{stmt['name']}' is already an output", line, stmt.get("name_col", col))
            graph.outputs.append(stmt["name"])
            continue

        name = stmt["name"]
        if name in graph.nodes:
            prev = graph.nodes[name]
            raise MTError("graph", f"tensor '{name}' is already defined at line {prev.line} "
                          "(each tensor is assigned exactly once)", line, col)
        if len(graph.nodes) >= MAX_NODES:
            raise MTError("graph", f"too many tensors (limit {MAX_NODES})", line, col)

        if kind in ("input", "param"):
            check_shape(stmt["shape"], name, line, col)
            graph.add(Node(name, kind, attrs={"seed": seed}, shape=stmt["shape"], line=line, col=col))
            seed += 1
        elif kind == "const":
            check_shape(stmt["shape"], name, line, col)
            if len(stmt["values"]) != size_of(stmt["shape"]):
                raise MTError("graph", f"const '{name}' has shape {stmt['shape']} ({size_of(stmt['shape'])} values) "
                              f"but {len(stmt['values'])} values were given", line, col)
            graph.add(Node(name, "const", attrs={"values": list(stmt["values"])}, shape=stmt["shape"], line=line, col=col))
        else:
            op = stmt["op"]
            where = (stmt["op_line"], stmt["op_col"])
            if op not in OP_SIGNATURES:
                raise MTError("graph", f"unknown operation '{op}' (known: {', '.join(sorted(OP_SIGNATURES))})", *where)
            n_tensors, n_consts = OP_SIGNATURES[op]
            args = stmt["args"]
            if len(args) != n_tensors + n_consts:
                raise MTError("graph", f"{op} expects {n_tensors + n_consts} argument(s) but {len(args)} were given", *where)
            inputs, attrs = [], {}
            for arg in args[:n_tensors]:
                if arg["kind"] != "name":
                    raise MTError("graph", f"{op}: argument must be a tensor name, not a number", arg["line"], arg["col"])
                if arg["value"] not in graph.nodes:
                    raise MTError("graph", f"undefined tensor '{arg['value']}'", arg["line"], arg["col"])
                inputs.append(arg["value"])
            for arg in args[n_tensors:]:
                if arg["kind"] != "number":
                    raise MTError("graph", f"{op}: the last argument must be a number", arg["line"], arg["col"])
                if op == "sum":
                    if arg["value"] != int(arg["value"]):
                        raise MTError("graph", "sum: the axis must be an integer", arg["line"], arg["col"])
                    attrs["axis"] = int(arg["value"])
                else:
                    attrs["factor"] = arg["value"]
            shape = infer_shape(op, [graph.nodes[i].shape for i in inputs], attrs, where)
            check_shape(shape, name, line, col)
            graph.add(Node(name, op, inputs, attrs, shape, line, col))

    if not graph.outputs:
        raise MTError("graph", "the model has no 'output' statement", 0, 0)
    return graph
