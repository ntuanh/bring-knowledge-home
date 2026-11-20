"""Stage 1: parse the .mt DSL into an AST (a list of statements).

Input: source text. Output: list of statement dicts. Called by driver.py;
the AST is consumed by graph.py (build_graph).

Grammar (one statement per line, '#' starts a comment):

    statement := "input" NAME ":" shape
               | "param" NAME ":" shape
               | "const" NAME ":" shape "=" literal
               | NAME "=" OP "(" arg ("," arg)* ")"
               | "output" NAME
    shape     := "[" INT ("," INT)* "]"
    literal   := NUMBER | "[" literal ("," literal)* "]"
    arg       := NAME | NUMBER

The parser only checks syntax. Whether names exist and shapes fit is the job
of graph.py.
"""

import re

from .errors import MTError

TOKEN_RE = re.compile(r"""
    (?P<ws>[ \t\r]+)
  | (?P<comment>\#[^\n]*)
  | (?P<newline>\n)
  | (?P<number>[-+]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?)
  | (?P<name>[A-Za-z_][A-Za-z0-9_]*)
  | (?P<punct>[:\[\],=()])
""", re.VERBOSE)

KEYWORDS = {"input", "param", "const", "output"}


def tokenize(source):
    """Returns a list of (kind, text, line, col). Newlines inside [...] are dropped."""
    tokens = []
    pos, line, line_start, depth = 0, 1, 0, 0
    while pos < len(source):
        m = TOKEN_RE.match(source, pos)
        col = pos - line_start + 1
        if not m:
            raise MTError("parse", f"unexpected character {source[pos]!r}", line, col)
        kind, text = m.lastgroup, m.group()
        pos = m.end()
        if kind == "newline":
            if depth == 0:
                tokens.append(("newline", "\\n", line, col))
            line += 1
            line_start = pos
        elif kind in ("ws", "comment"):
            pass
        else:
            if text == "[":
                depth += 1
            elif text == "]":
                depth = max(0, depth - 1)
            tokens.append((kind, text, line, col))
    tokens.append(("newline", "\\n", line, len(source) - line_start + 1))
    tokens.append(("eof", "end of file", line, len(source) - line_start + 1))
    return tokens


class Parser:
    def __init__(self, tokens):
        self.tokens = tokens
        self.pos = 0

    def peek(self):
        return self.tokens[self.pos]

    def next(self):
        tok = self.tokens[self.pos]
        self.pos += 1
        return tok

    def error(self, message, tok=None):
        tok = tok or self.peek()
        raise MTError("parse", message, tok[2], tok[3])

    def describe(self, tok):
        return "end of line" if tok[0] == "newline" else tok[1] if tok[0] == "eof" else f"'{tok[1]}'"

    def expect(self, text):
        tok = self.peek()
        if tok[1] != text or tok[0] not in ("punct", "name"):
            self.error(f"expected '{text}' but found {self.describe(tok)}")
        return self.next()

    def expect_name(self, what):
        tok = self.peek()
        if tok[0] != "name":
            self.error(f"expected {what} but found {self.describe(tok)}")
        if tok[1] in KEYWORDS:
            self.error(f"'{tok[1]}' is a keyword and cannot be used as {what}")
        return self.next()

    def end_of_statement(self):
        tok = self.peek()
        if tok[0] != "newline":
            self.error(f"expected end of line but found {self.describe(tok)}")
        self.next()

    # shape := "[" INT ("," INT)* "]"
    def parse_shape(self):
        self.expect("[")
        dims = []
        while True:
            tok = self.peek()
            if tok[0] != "number" or not re.fullmatch(r"\d+", tok[1]):
                self.error(f"expected a dimension (positive integer) but found {self.describe(tok)}")
            dims.append(int(self.next()[1]))
            if self.peek()[1] == ",":
                self.next()
                continue
            break
        self.expect("]")
        return dims

    # literal := NUMBER | "[" literal ("," literal)* "]"   (returned flattened)
    def parse_literal(self, out):
        tok = self.peek()
        if tok[0] == "number":
            out.append(float(self.next()[1]))
        elif tok[1] == "[" and tok[0] == "punct":
            self.next()
            while True:
                self.parse_literal(out)
                if self.peek()[1] == ",":
                    self.next()
                    continue
                break
            self.expect("]")
        else:
            self.error(f"expected a number or '[' but found {self.describe(tok)}")

    def parse_statement(self):
        tok = self.peek()
        if tok[0] != "name":
            self.error(f"expected a statement but found {self.describe(tok)}")
        line, col = tok[2], tok[3]
        if tok[1] in ("input", "param", "const"):
            self.next()
            name = self.expect_name("a tensor name")
            self.expect(":")
            stmt = {"kind": tok[1], "name": name[1], "shape": self.parse_shape(), "line": line, "col": col}
            if tok[1] == "const":
                self.expect("=")
                values = []
                self.parse_literal(values)
                stmt["values"] = values
            return stmt
        if tok[1] == "output":
            self.next()
            name = self.expect_name("a tensor name")
            return {"kind": "output", "name": name[1], "line": line, "col": col, "name_col": name[3]}
        # NAME "=" OP "(" arg ("," arg)* ")"
        self.next()
        self.expect("=")
        op = self.expect_name("an operation name")
        self.expect("(")
        args = []
        while True:
            arg = self.peek()
            if arg[0] == "name":
                args.append({"kind": "name", "value": arg[1], "line": arg[2], "col": arg[3]})
            elif arg[0] == "number":
                args.append({"kind": "number", "value": float(arg[1]), "text": arg[1], "line": arg[2], "col": arg[3]})
            else:
                self.error(f"expected an argument but found {self.describe(arg)}")
            self.next()
            if self.peek()[1] == ",":
                self.next()
                continue
            break
        self.expect(")")
        return {"kind": "assign", "name": tok[1], "op": op[1], "args": args, "line": line, "col": col,
                "op_line": op[2], "op_col": op[3]}

    def parse_program(self):
        statements = []
        while self.peek()[0] != "eof":
            if self.peek()[0] == "newline":
                self.next()
                continue
            statements.append(self.parse_statement())
            self.end_of_statement()
        return statements


def parse(source):
    """Source text -> list of statement dicts. Raises MTError on the first syntax error."""
    return Parser(tokenize(source)).parse_program()


def statement_text(stmt):
    """One-line description of a statement, used by `--emit=ast`."""
    if stmt["kind"] in ("input", "param"):
        return f"{stmt['kind'].capitalize()}({stmt['name']}, shape={stmt['shape']})"
    if stmt["kind"] == "const":
        return f"Const({stmt['name']}, shape={stmt['shape']}, {len(stmt['values'])} values)"
    if stmt["kind"] == "output":
        return f"Output({stmt['name']})"
    args = ", ".join(str(a["value"]) if a["kind"] == "name" else a["text"] for a in stmt["args"])
    return f"Assign({stmt['name']} = Call({stmt['op']}; {args}))"
