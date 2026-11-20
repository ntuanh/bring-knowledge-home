import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import adapters
import pipelines
import server


class AdapterTests(unittest.TestCase):
    def test_minic_trace_and_source_positions(self):
        session = adapters.compile_session("minic", "int main() { return 2 + 3 * 4; }", 1)
        stages = {s["id"]: s for s in session["stages"]}
        self.assertTrue(all(s["status"] == "ok" for s in stages.values()))
        self.assertTrue(stages["lexer"]["events"])
        self.assertEqual(stages["lexer"]["events"][0]["location"],
                         {"line": 1, "col": 1, "endCol": 4})
        self.assertTrue(any("before" in e["data"] and "after" in e["data"]
                            for e in stages["opt"]["events"]))
        self.assertIn("main:", stages["asm"]["output"])

    def test_minic_error_preserves_completed_and_partial_results(self):
        session = adapters.compile_session("minic", "int main() { return missing; }")
        self.assertEqual([s["status"] for s in session["stages"]],
                         ["ok", "ok", "fail", "skip", "skip", "skip"])
        self.assertTrue(any("undeclared" in e["label"] for e in session["stages"][2]["events"]))

    def test_o0_skips_optimization_but_still_generates_assembly(self):
        session = adapters.compile_session("minic", "int main() { return 0; }")
        self.assertEqual(session["stages"][4]["status"], "skip")
        self.assertEqual(session["stages"][5]["status"], "ok")

    def test_c_examples_and_full_toolchain(self):
        examples = pipelines.list_examples("c")
        self.assertIn("01_minimal.c", [e["name"] for e in examples])
        result = pipelines.compile_c("int main() { print(42); return 0; }", 1)
        self.assertIsNone(result["failed_stage"])
        self.assertTrue(result["assemble"]["ok"])
        self.assertTrue(result["link"]["ok"])
        self.assertEqual(result["run"]["stdout"].strip(), "42")

    def test_independent_expression_compiler(self):
        session = adapters.compile_session("expression", "2 + 3 * (4 - 1)")
        self.assertEqual(session["stages"][-1]["output"]["result"], 11)
        self.assertEqual([e["label"] for e in session["stages"][2]["events"]],
                         ["PUSH 2", "PUSH 3", "PUSH 4", "PUSH 1", "SUB", "MUL", "ADD"])
        self.assertEqual(len(session["stages"][-1]["events"]), 3)

    def test_expression_error_exit_one_is_a_valid_session(self):
        session = adapters.compile_session("expression", "danger()")
        self.assertEqual([s["status"] for s in session["stages"]], ["ok", "fail", "skip", "skip"])

    def test_rejects_bad_requests_and_bad_protocol(self):
        for compiler, source in [("unknown", "1"), ("expression", None),
                                 ("expression", "a\x00"), ("expression", "x" * 65537)]:
            with self.subTest(compiler=compiler, source_type=type(source)):
                with self.assertRaises(pipelines.PipelineError):
                    adapters.compile_session(compiler, source)
        valid = {"version": 1, "stages": [{"id": "x", "label": "X", "help": "X", "status": "ok",
                                           "events": [{"label": "X", "location": {"line": 1}}]}]}
        for mutate in [lambda d: d.update(version=2),
                       lambda d: d["stages"].append(copy.deepcopy(d["stages"][0])),
                       lambda d: d["stages"][0].update(status=[]),
                       lambda d: d["stages"][0]["events"][0].update(location={"line": 0})]:
            data = copy.deepcopy(valid)
            mutate(data)
            with self.assertRaises(pipelines.PipelineError):
                adapters.validate_session(data)

    def test_external_registration_and_commands_not_exposed(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "adapters.json"
            config.write_text(json.dumps([{"id": "custom", "name": "Custom compiler",
                "command": ["{python}", "{root}/compilers/expression/compiler.py", "{source}"]}]))
            with patch.dict(os.environ, {"COMPILER_ADAPTERS": str(config)}):
                self.assertTrue(all("command" not in c for c in adapters.catalog()))
                result = adapters.compile_session("custom", "10 - 4")
                self.assertEqual(result["compiler"]["id"], "custom")
                self.assertEqual(result["stages"][-1]["output"]["result"], 6)

    def test_invalid_adapter_output_and_timeout(self):
        for result in [dict(timed_out=True, signal=None, truncated=False),
                       dict(timed_out=False, signal=None, truncated=False, stdout="oops", stderr="bad"),
                       dict(timed_out=False, signal=None, truncated=False, stdout='{}', stderr="", exit_code=0)]:
            with patch.object(pipelines, "run_tool", return_value=result):
                with self.assertRaises(pipelines.PipelineError):
                    adapters.compile_session("expression", "2")

    def test_api_routes(self):
        # Run the real routing methods with in-memory request/response boundaries.
        handler = object.__new__(server.Handler)
        handler.send_json = lambda data, status=200: responses.append((status, data))
        responses = []
        handler.path = "/api/compilers"
        handler.do_GET()
        self.assertEqual(responses[-1][0], 200)
        self.assertTrue(responses[-1][1]["compilers"])
        handler.path = "/api/compiler/compile"
        handler.read_json_body = lambda: {"compiler": "expression", "source": "5 * 2"}
        handler.do_POST()
        self.assertEqual(responses[-1][1]["stages"][-1]["output"]["result"], 10)
        handler.read_json_body = lambda: {"compiler": "missing", "source": "1"}
        handler.do_POST()
        self.assertEqual(responses[-1][0], 400)

    def test_removed_ai_routes_do_not_depend_on_external_source(self):
        handler = object.__new__(server.Handler)
        responses = []
        handler.send_json = lambda data, status=200: responses.append((status, data))
        handler.path = "/api/ai/compile"
        handler.read_json_body = lambda: {"source": "input x : [1]"}
        handler.do_POST()
        self.assertEqual(responses[-1][0], 404)
        handler.path = "/api/examples?part=ai"
        handler.do_GET()
        self.assertEqual(responses[-1][0], 400)


if __name__ == "__main__":
    unittest.main()
