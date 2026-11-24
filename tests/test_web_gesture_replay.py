"""Exercise the native reference engine with the editor's wire messages."""

import json
import math
import subprocess
import sys


def run(binary, mode, messages=()):
    return subprocess.run(
        [binary, mode],
        input="".join(json.dumps(message) + "\n" for message in messages),
        text=True,
        capture_output=True,
        timeout=5,
    )


def parameter_snapshot(result):
    assert result.returncode == 0, result.stderr
    snapshot = json.loads(result.stdout)
    assert snapshot["type"] == "parameters"
    return {parameter["key"]: parameter for parameter in snapshot["parameters"]}


def main():
    binary = sys.argv[1]
    gain = parameter_snapshot(run(binary, "--parameters"))["gain"]
    assert gain["minimum"] < gain["value"] < gain["maximum"]
    new_value = round(gain["value"] + gain["step"], 8)
    gesture = (
        {"type": "begin", "key": "gain"},
        {"type": "parameter", "key": "gain", "value": new_value},
        {"type": "end", "key": "gain"},
    )
    replayed = parameter_snapshot(run(binary, "--apply-gestures", gesture))
    assert math.isclose(replayed["gain"]["value"], new_value, abs_tol=1e-8)

    for invalid in (
        {"type": "parameter", "key": "unknown", "value": new_value},
        {"type": "parameter", "key": "gain", "value": gain["maximum"] + 1},
        {"type": "parameter", "key": "gain", "value": new_value + gain["step"] / 2},
    ):
        result = run(binary, "--apply-gestures", (gesture[0], invalid, gesture[2]))
        assert result.returncode != 0
        assert "Parameter message rejected on line 2" in result.stderr, result.stderr
        assert not result.stdout


if __name__ == "__main__":
    main()
