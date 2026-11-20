"""Pipeline contracts: folding preserves values and generated kernels match reference."""
from pathlib import Path
import unittest

from minitensor.driver import compile_source
from minitensor.graph import build_graph
from minitensor import interp, parser, passes

EXAMPLES = Path(__file__).resolve().parents[1] / "examples"


class PipelineTests(unittest.TestCase):
    def test_constant_folding_preserves_output_and_removes_dead_node(self):
        source = (EXAMPLES / "04_constant_folding.mt").read_text()
        original = build_graph(parser.parse(source))
        optimized, groups, log = passes.optimize(original)
        self.assertNotIn("unused", optimized.nodes)
        self.assertEqual(optimized.nodes["k2"].op, "const")
        self.assertEqual(optimized.nodes["k2"].attrs["values"], [3.0, 5.0, 7.0])
        self.assertEqual(interp.evaluate(original)["y"], interp.evaluate(optimized)["y"])

    def test_fused_and_unfused_generated_code_match_reference(self):
        result = compile_source((EXAMPLES / "01_mlp_layer.mt").read_text())
        self.assertIsNone(result["failed_stage"])
        self.assertTrue(result["run"]["ok"])
        for variant in ("fused", "unfused"):
            self.assertTrue(result["run"][variant]["outputs"]["y"]["ok"])
        self.assertLess(len(result["loop_ir"]["fused"]["kernels"]),
                        len(result["loop_ir"]["unfused"]["kernels"]))

    def test_invalid_graph_preserves_ast_and_stops_codegen(self):
        for name in ("err_shape.mt", "err_undefined.mt"):
            with self.subTest(name=name):
                result = compile_source((EXAMPLES / name).read_text())
                self.assertEqual(result["failed_stage"], "graph")
                self.assertIsNotNone(result["ast"])
                self.assertTrue(result["diagnostics"])
                self.assertIsNone(result["c_code"])
                self.assertIsNone(result["run"])
