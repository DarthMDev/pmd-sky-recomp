#!/usr/bin/env python3
"""Unit test runner for Milestone 3 C++20 runtime subsystems."""

import os
import subprocess
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent


def resolve_recomp_ui_root() -> Path:
    """Resolve the recomp-ui root directory from environment variables or default checkout."""
    configured = os.environ.get("NDS_RECOMP_UI_ROOT") or os.environ.get("RECOMP_UI_ROOT")
    if configured:
        return Path(configured).resolve()
    return REPO_ROOT / "third_party" / "recomp-ui"


class TestMilestone3Runtime(unittest.TestCase):
    """Compiles and executes the Milestone 3 C++20 runtime verification suite."""

    def test_milestone3_cpp20_runtime_suite(self) -> None:
        """Compile and run the Milestone 3 Widescreen PPU, Camera/Interpolation, Shaders, and recomp-ui suite."""
        build_dir = REPO_ROOT / "build"
        build_dir.mkdir(parents=True, exist_ok=True)

        recomp_ui_root = resolve_recomp_ui_root()
        recomp_ui_inc = recomp_ui_root / "src"
        recomp_ui_src = recomp_ui_inc / "common" / "recomp_runtime_ui.c"

        recomp_ui_obj = build_dir / "recomp_runtime_ui_test.o"
        c_compile_cmd = [
            "clang",
            "-std=c11",
            f"-I{recomp_ui_inc}",
            "-c",
            str(recomp_ui_src),
            "-o",
            str(recomp_ui_obj),
        ]
        res_c = subprocess.run(c_compile_cmd, capture_output=True, text=True)
        self.assertEqual(res_c.returncode, 0, f"recomp-ui C compilation failed: {res_c.stderr}")

        bin_path = build_dir / "test_milestone3_runtime"
        cpp_compile_cmd = [
            "clang++",
            "-std=c++20",
            "-UNDEBUG",
            f"-I{REPO_ROOT / 'include'}",
            f"-I{REPO_ROOT / 'runtime'}",
            f"-I{recomp_ui_inc}",
            str(REPO_ROOT / "tests" / "milestone3_runtime_test.cpp"),
            str(REPO_ROOT / "runtime" / "gpu2d" / "widescreen_ppu.cpp"),
            str(REPO_ROOT / "runtime" / "camera" / "dungeon_camera.cpp"),
            str(REPO_ROOT / "runtime" / "camera" / "subpixel_interpolator.cpp"),
            str(REPO_ROOT / "runtime" / "shaders" / "weather_shader_pipeline.cpp"),
            str(REPO_ROOT / "runtime" / "ui" / "settings_overlay.cpp"),
            str(recomp_ui_obj),
            "-o",
            str(bin_path),
        ]
        res_cpp = subprocess.run(cpp_compile_cmd, capture_output=True, text=True)
        self.assertEqual(res_cpp.returncode, 0, f"Milestone 3 C++20 compilation failed: {res_cpp.stderr}")

        run_res = subprocess.run([str(bin_path)], capture_output=True, text=True)
        self.assertEqual(run_res.returncode, 0, f"Milestone 3 test execution failed: {run_res.stderr}")
        self.assertIn("ALL MILESTONE 3 RUNTIME TESTS PASSED", run_res.stdout)


if __name__ == "__main__":
    unittest.main()
