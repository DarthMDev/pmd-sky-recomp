#!/usr/bin/env python3
import subprocess
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
GEN_SCRIPT = REPO_ROOT / "translator" / "gen_pmdsky_bindings.py"
HEADER_PATH = REPO_ROOT / "include" / "skyrecomp" / "pmdsky_addrs.hpp"
CPP_TEST = REPO_ROOT / "tests" / "pmdsky_bindings_test.cpp"


class TestPMDSkyBindings(unittest.TestCase):
    def test_generator_runs_cleanly(self):
        res = subprocess.run([sys.executable, str(GEN_SCRIPT)], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"Generator failed: {res.stderr}")
        self.assertTrue(HEADER_PATH.exists(), "Header file was not created")

    def test_cpp20_compilation_and_assertions(self):
        # Compile with clang++ -std=c++20
        bin_path = REPO_ROOT / "build" / "test_pmdsky_bindings"
        bin_path.parent.mkdir(parents=True, exist_ok=True)
        compile_cmd = [
            "clang++",
            "-std=c++20",
            f"-I{REPO_ROOT / 'include'}",
            str(CPP_TEST),
            "-o", str(bin_path),
        ]
        res = subprocess.run(compile_cmd, capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"Compilation failed: {res.stderr}")

        # Run test executable
        run_res = subprocess.run([str(bin_path)], capture_output=True, text=True)
        self.assertEqual(run_res.returncode, 0, f"Test execution failed: {run_res.stderr}")
        self.assertIn("ALL C++20 ASSERTIONS PASSED", run_res.stdout)


if __name__ == "__main__":
    unittest.main()
