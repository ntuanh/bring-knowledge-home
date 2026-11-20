"""Check the standalone source layout through the public module entry point."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class CliTests(unittest.TestCase):
    def run_cli(self, example, *args):
        env = dict(os.environ, PYTHONPATH=str(ROOT / "src"))
        with tempfile.TemporaryDirectory() as directory:
            return subprocess.run([sys.executable, "-m", "minitensor", *args,
                                   str(ROOT / "examples" / example)],
                                  cwd=directory, env=env, capture_output=True, text=True, timeout=15)

    def test_json_pipeline_runs_without_visualizer(self):
        result = self.run_cli("01_mlp_layer.mt", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertIsNone(data["failed_stage"])
        self.assertTrue(data["run"]["ok"])

    def test_invalid_shape_returns_structured_diagnostic(self):
        result = self.run_cli("err_shape.mt", "--json")
        self.assertEqual(result.returncode, 1, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data["failed_stage"], "graph")
        self.assertTrue(data["diagnostics"])
        self.assertIsNone(data["run"])
