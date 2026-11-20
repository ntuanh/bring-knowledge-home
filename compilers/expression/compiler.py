#!/usr/bin/env python3
"""Standalone compiler adapter demo; no imports from the visualizer.

Compiles integer +, -, * expressions to stack bytecode. Emits protocol v1.
Python's parser supplies the AST; unsupported constructs are rejected.
"""
import ast
import io
import json
import sys
import tokenize


def compile_source(source):
    stages = [
        {"id": sid, "label": label, "help": help_text, "status": "skip", "events": [], "output": None}
        for sid, label, help_text in [
            ("tokens", "Tokens", "Các token được nhận diện theo thứ tự source."),
            ("ast", "AST", "Python parser dựng cây cú pháp; compiler này chỉ chấp nhận số nguyên và +, -, *."),
            ("bytecode", "Stack bytecode", "Duyệt cây postorder: đẩy toán hạng trước, thực hiện phép toán sau. Mỗi bước là một lệnh được sinh."),
            ("fold", "Constant folding", "Đánh giá biểu thức hằng từ dưới lên; mỗi bước ghi hai toán hạng và kết quả. Bytecode cuối chỉ cần PUSH kết quả."),
        ]]
    current = stages[0]
    try:
        tokens = []
        for tok in tokenize.generate_tokens(io.StringIO(source).readline):
            if tok.type in (tokenize.ENDMARKER, tokenize.NEWLINE, tokenize.NL, tokenize.ENCODING):
                continue
            item = {"kind": tokenize.tok_name[tok.type], "text": tok.string}
            tokens.append(item)
            current["events"].append({"label": f"{item['kind']}: {tok.string}", "data": item,
                                      "location": {"line": tok.start[0], "col": tok.start[1] + 1}})
        current["output"], current["status"] = tokens, "ok"
        current = stages[1]
        tree = ast.parse(source, mode="eval")
        current["output"] = ast.dump(tree, indent=2)
        pending = [(tree.body, 0)]
        while pending:
            node, depth = pending.pop()
            if depth > 100:
                raise ValueError("Expression nesting exceeds 100 levels")
            if isinstance(node, ast.Constant) and type(node.value) is int:
                if node.value.bit_length() > 4096:
                    raise ValueError("Integer exceeds 4096 bits")
            elif isinstance(node, ast.BinOp) and type(node.op) in (ast.Add, ast.Sub, ast.Mult):
                pending.extend([(node.right, depth + 1), (node.left, depth + 1)])
            else:
                raise ValueError("Only integer literals and binary +, -, * are supported")
        current["status"] = "ok"
        current = stages[2]
        instructions, folds = [], []

        def lower(node):
            loc = {"line": node.lineno, "col": node.col_offset + 1}
            if isinstance(node, ast.Constant):
                instruction, value = "PUSH " + str(node.value), node.value
            else:
                left, right = lower(node.left), lower(node.right)
                instruction, operation = {
                    ast.Add: ("ADD", lambda a, b: a + b),
                    ast.Sub: ("SUB", lambda a, b: a - b),
                    ast.Mult: ("MUL", lambda a, b: a * b),
                }[type(node.op)]
                value = operation(left, right)
                if value.bit_length() > 4096:
                    raise ValueError("Constant result exceeds 4096 bits")
                folds.append({"label": f"{left} {instruction} {right} → {value}", "location": loc,
                              "data": {"before": f"PUSH {left}; PUSH {right}; {instruction}",
                                       "after": f"PUSH {value}"}})
            instructions.append(instruction)
            current["events"].append({"label": instruction, "location": loc, "data": {"instruction": instruction}})
            return value

        value = lower(tree.body)
        current["output"], current["status"] = "\n".join(instructions), "ok"
        current = stages[3]
        current.update(status="ok", events=folds,
                       output={"before": "\n".join(instructions), "after": f"PUSH {value}", "result": value})
    except (SyntaxError, ValueError, tokenize.TokenError, IndentationError, RecursionError) as exc:
        current["status"] = "fail"
        current["events"].append({"label": str(exc), "data": {"message": str(exc)}})
    return {"version": 1, "stages": stages}


if __name__ == "__main__":
    session = compile_source(open(sys.argv[1], encoding="utf-8").read())
    print(json.dumps(session))
    sys.exit(1 if any(s["status"] == "fail" for s in session["stages"]) else 0)
