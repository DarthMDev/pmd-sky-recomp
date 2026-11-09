#!/usr/bin/env python3
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
GEN_SCRIPT = REPO_ROOT / "translator" / "gen_pmdsky_bindings.py"
CPP_TEST = REPO_ROOT / "tests" / "pmdsky_bindings_test.cpp"


class TestPMDSkyBindings(unittest.TestCase):
    def test_generator_writes_default_header(self):
        """Generate a fresh header using the default input and output paths."""
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            script = root / "translator" / GEN_SCRIPT.name
            script.parent.mkdir()
            shutil.copyfile(GEN_SCRIPT, script)
            symbols = root / "third_party" / "pmdsky-debug" / "symbols"
            symbols.mkdir(parents=True)
            (symbols / "arm9.yml").write_text(
                "- name: NitroMain\n  address:\n    NA: 0x02123456\n",
                encoding="utf-8",
            )

            result = subprocess.run([sys.executable, str(script)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, f"Generator failed: {result.stderr}")
            header = root / "include" / "skyrecomp" / "pmdsky_addrs.hpp"
            self.assertIn(".NitroMain = 0x02123456,", header.read_text(encoding="utf-8"))

    def test_cpp20_compilation_and_assertions(self):
        """Compile the bindings as C++20 and run their address assertions."""
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
