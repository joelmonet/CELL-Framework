"""Check a full Doctor run and failure reports from real CMake/CTest."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def doctor(script, root, tests, extra=()):
    root.mkdir()
    (root / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.24)\n"
        "project(DoctorCheck LANGUAGES NONE)\n"
        "enable_testing()\n" + tests, encoding="utf-8")
    result = subprocess.run(
        [sys.executable, str(script), "doctor", str(root), *extra],
        capture_output=True, encoding="utf-8")
    reports = list((root / ".cell-doctor").glob("run-*/report.json"))
    assert len(reports) == 1, result.stdout + result.stderr
    report_path = reports[0]
    report = json.loads(report_path.read_text(encoding="utf-8"))
    assert report["schema"] == "cell.doctor.v1"
    assert [stage["name"] for stage in report["stages"][:3]] == ["configure", "build", "test"]
    for stage in report["stages"]:
        if stage["status"] != "skipped":
            assert (report_path.parent / stage["log"]).is_file()
    return result, report


def test(script):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        passing = 'add_test(NAME smoke COMMAND "${CMAKE_COMMAND}" -E true)\n'
        result, report = doctor(script, root / "pass with spaces", passing)
        assert result.returncode == 0 and report["status"] == "passed", result.stdout
        assert report["stages"][-1]["tests"] == {"passed": 1, "failed": 0, "skipped": 0}

        result, report = doctor(script, root / "failing",
                                'add_test(NAME failure COMMAND "${CMAKE_COMMAND}" -E false)\n')
        assert result.returncode != 0 and report["stages"][-1]["tests"]["failed"] == 1

        result, report = doctor(script, root / "empty", "")
        assert result.returncode != 0 and report["stages"][-1]["status"] == "failed"

        disabled = passing + "set_tests_properties(smoke PROPERTIES DISABLED TRUE)\n"
        result, report = doctor(script, root / "disabled", disabled)
        assert result.returncode != 0 and report["stages"][-1]["tests"]["passed"] == 0

        result, report = doctor(script, root / "broken", "message(FATAL_ERROR broken)\n")
        assert result.returncode != 0
        assert [stage["status"] for stage in report["stages"]] == ["failed", "skipped", "skipped"]


        result, report = doctor(script, root / "needs validation", passing,
                                ("--validator", "clap", sys.executable))
        assert result.returncode != 0
        assert report["stages"][-1]["name"] == "validate-clap"
        assert report["stages"][-1]["exit_code"] is None

        # Test the command boundary separately from host-specific integration.
        # Python stands in for the validator and returns the payload's status.
        fixture = passing + (
            'file(WRITE "${CMAKE_BINARY_DIR}/fixture.vst3" "raise SystemExit(23)\n")\n'
            'file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/cell-artifacts/$<CONFIG>/vst3.txt" '
            'CONTENT "${CMAKE_BINARY_DIR}/fixture.vst3\n")\n')
        result, report = doctor(script, root / "validator failure", fixture,
                                ("--validator", "clap", sys.executable,
                                 "--validator", "vst3", sys.executable))
        assert result.returncode != 0
        assert [stage["status"] for stage in report["stages"]] == [
            "passed", "passed", "passed", "failed", "failed"]
        assert report["stages"][-1]["exit_code"] == 23

        result, report = doctor(script, root / "validator passed", fixture.replace("23", "0"),
                                ("--validator", "vst3", sys.executable))
        assert result.returncode == 0 and report["stages"][-1]["status"] == "passed"

        slow = fixture.replace("raise SystemExit(23)", "import time; time.sleep(10)")
        result, report = doctor(script, root / "validator timeout", slow,
                                ("--validator", "vst3", sys.executable, "--validator-timeout", "1"))
        assert result.returncode != 0 and "timed out" in report["stages"][-1]["reason"]


if __name__ == "__main__":
    test((Path(sys.argv[1]) if len(sys.argv) > 1 else
          Path(__file__).resolve().parents[1] / "tools" / "cell.py").resolve())
