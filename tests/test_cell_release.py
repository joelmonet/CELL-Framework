"""Qualify a generated banked CLAP release and make sure regressions fail."""
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def release(cell, project, build, *args, success=True):
    result = subprocess.run([cell, "release", project, "--build-dir", build, *args],
                            capture_output=True, text=True)
    assert (result.returncode == 0) == success, result.stdout + result.stderr
    path = Path(result.stdout.rsplit("Report: ", 1)[1].strip())
    return json.loads(path.read_text(encoding="utf-8"))


def test(cell, project):
    with tempfile.TemporaryDirectory(prefix="cell-release-test-") as temporary:
        root = Path(temporary)
        # Copy the project as a release consumer would. Retain the same ID.
        project = Path(shutil.copytree(project, root / "candidate",
                                      ignore=shutil.ignore_patterns(".cell-*", "build")))
        build = root / "build"
        baseline = root / "baseline"
        captured = release(cell, project, build, "--capture", baseline)
        assert captured["status"] == "captured"
        moved = baseline.rename(root / "relocated")
        verified = release(cell, project, build, "--previous", moved)
        assert verified["status"] == "passed" and verified["scope"] == "shipping-clap"
        assert verified["processor_identity"]["count"] == 1
        assert verified["referenced_assets"][0]["status"] == "passed"
        checks = {check["name"]: check["count"] for check in verified["host"]["checks"]}
        assert checks["factory-catalog"] == 36 and checks["automation"] == 2 and checks["preset-recall"] == 3
        assert all(render["within_tolerance"] for render in verified["host"]["renders"])
        assert [render["factory_key"] for render in verified["host"]["renders"] if "factory_key" in render] == ["2:4", "5:3"]

        waveform = project / "release/input.wav"
        original = waveform.read_bytes()
        waveform.unlink()
        failed = release(cell, project, build, "--previous", moved, success=False)
        assert failed["referenced_assets"][0]["status"] == "missing", failed
        waveform.write_bytes(original[:-1] + bytes([original[-1] ^ 1]))
        failed = release(cell, project, build, "--previous", moved, success=False)
        assert failed["referenced_assets"][0]["status"] == "changed", failed
        waveform.write_bytes(original)

        state = moved / "state-1.bin"
        original = state.read_bytes()
        state.write_bytes(bytes([original[0] ^ 1]) + original[1:])
        failed = release(cell, project, build, "--previous", moved, success=False)
        assert "baseline file changed: state-1.bin" in failed["error"], failed
        state.write_bytes(original)

        # Deliberate offline-only DSP change. The compiled project identity and
        # ordinary smoke tests still pass, so the release render must catch it.
        processor = project / "processor.cpp"
        text = processor.read_text(encoding="utf-8")
        old_signature = "const Transport&,\n                  std::span<const MidiEvent> events"
        old_call = "  renderBanked(audio, bank, events, &output);"
        assert old_signature in text and old_call in text
        text = text.replace(old_signature, "const Transport& transport,\n                  std::span<const MidiEvent> events", 1)
        text = text.replace(old_call, old_call + "\n  if (transport.offline)\n"
                            "   for (unsigned channel = 0; channel < audio.channelCount; ++channel)\n"
                            "    for (unsigned frame = 0; frame < audio.frames; ++frame)\n"
                            "     audio.channel(channel)[frame] += 0.001f;", 1)
        processor.write_text(text, encoding="utf-8")
        failed = release(cell, project, build, "--previous", moved, success=False)
        assert all(stage["status"] == "passed" for stage in failed["build_checks"]), failed
        assert "Offline render exceeds" in failed["error"], failed
        assert any(not item["within_tolerance"] and item["peak_absolute_error"] > 0.0009
                   for item in failed["host"]["renders"]), failed


if __name__ == "__main__":
    test(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
