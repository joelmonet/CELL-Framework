"""Record a built banked plug-in through CLAP and replay its host traffic."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def run(*command, success=True):
    result = subprocess.run(command, capture_output=True, encoding="utf-8")
    report = Path(result.stdout.rsplit("Report: ", 1)[1].strip())
    stage = json.loads(report.read_text(encoding="utf-8"))["stages"][-1]
    log = report.parent / stage["log"]
    assert (result.returncode == 0) == success, result.stdout + result.stderr + log.read_text(encoding="utf-8")
    return stage, report.parent


def test(cell, build, instrumented):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        bundle = Path((build / "cell-artifacts/Release/clap.txt").read_text(encoding="utf-8").strip())
        host = build / ("project_banked_clap_check.exe" if sys.platform == "win32" else "project_banked_clap_check")
        module = bundle if bundle.is_file() else bundle / "Contents/MacOS" / bundle.stem
        recorded, _ = run(cell, "doctor", root, "--plugin", bundle, "--record", root / "trace",
                          "--record-audio", "--record-host", host, f"--host-arg={module}", f"--host-arg={bundle}")
        assert len(recorded["traces"]) == 1
        trace = Path(recorded["traces"][0])
        plain, _ = run(cell, "doctor", root, "--plugin", bundle, "--replay", trace)
        assert plain["replay"] == {
            "schema": "cell.doctor.clap.v1", "status": "passed", "callbacks": 2,
            "process_errors": 0, "editor_edits": 3, "input_events": 5, "output_events": 8,
            "transport_blocks": 2, "flushes": 1, "state_loads": 1, "state_saves": 2,
            "activation_configurations": [
                {"sample_rate": 48000, "min_frames": 1, "max_frames": 32, "output_channels": 2},
            ],
        }, plain
        profiled, _ = run(cell, "doctor", root, "--plugin", bundle, "--replay", trace, "--realtime")
        rt = profiled["replay"]["realtime"]
        assert rt["callback_count"] == rt["paced_callback_count"] == 2, rt
        assert 0 < rt["p99_9_callback_ns"] <= rt["worst_callback_ns"], rt
        assert rt["allocations"] == (0 if instrumented else None), rt
        assert isinstance(rt["underruns"], int) and rt["device_underruns"] is None, rt
        assert isinstance(rt["deadline_misses"], int), rt
        invalid = root / "truncated.celltrace"
        invalid.write_bytes(trace.read_bytes()[:-1])
        failed, logs = run(cell, "doctor", root, "--plugin", bundle, "--replay", invalid, success=False)
        assert failed["status"] == "failed"
        assert "Incomplete CLAP recording" in (logs / failed["log"]).read_text(encoding="utf-8")


if __name__ == "__main__":
    test(Path(sys.argv[1]), Path(sys.argv[2]), "--instrumented" in sys.argv)
