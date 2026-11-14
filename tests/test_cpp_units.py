"""Compile et exécute les tests unitaires C++ (indépendants du matériel)."""

import shutil
import subprocess

import pytest


@pytest.mark.skipif(shutil.which("cmake") is None, reason="cmake absent")
def test_cpp_unit_tests(repo_root, tmp_path):
    src = repo_root / "tests" / "cpp"
    build = tmp_path / "build"
    generator = ["-G", "Ninja"] if shutil.which("ninja") else []
    subprocess.run(
        ["cmake", "-S", str(src), "-B", str(build), *generator],
        check=True,
        capture_output=True,
    )
    subprocess.run(["cmake", "--build", str(build)], check=True, capture_output=True)
    result = subprocess.run(
        [str(build / "visslink_tests")], check=False, capture_output=True, text=True
    )
    print(result.stdout)
    assert result.returncode == 0, result.stdout + result.stderr
