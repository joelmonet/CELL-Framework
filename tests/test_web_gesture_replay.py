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
    exported = run(binary, "--parameters")
    gain = parameter_snapshot(exported)["gain"]
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

    catalog = json.loads(exported.stdout)["programs"]
    assert catalog["type"] == "program-memory"
    assert [len(bank["programs"]) for bank in catalog["banks"]] == [6] * 6
    recall = {"type": "recall-program", "bank": 2, "program": 4}
    recalled = run(binary, "--apply-gestures", (recall,))
    assert parameter_snapshot(recalled)["gain"]["value"] == -28
    assert json.loads(recalled.stdout)["programs"]["active"] == {"bank": 2, "program": 4}
    modified = run(binary, "--apply-gestures", (recall, {"type": "parameter", "key": "gain", "value": -27.5}))
    assert parameter_snapshot(modified)["gain"]["value"] == -27.5
    assert json.loads(modified.stdout)["programs"]["active"] is None
    assert {"key": "factory-recall", "mode": "invoke"} in json.loads(exported.stdout)["actions"]
    factory = {"type": "action", "key": "factory-recall", "phase": "invoke"}
    restored = run(binary, "--apply-gestures", (recall, factory))
    assert parameter_snapshot(restored)["gain"]["value"] == 0
    assert json.loads(restored.stdout)["programs"]["active"] == {"bank": 5, "program": 0}
    momentary = ({"type": "momentary-press", "key": "gain"}, {"type": "momentary-release", "key": "gain"})
    assert parameter_snapshot(run(binary, "--apply-gestures", momentary))["gain"]["value"] == gain["minimum"]
    for messages in (
        ({**recall, "bank": 6},),
        ({**recall, "program": 6},),
        ({**recall, "program": 4.5},),
        ({**recall, "bank": "2"},),
        (gesture[0], recall),
        ({**factory, "key": "store"},),
        ({**factory, "phase": "press"},),
        (momentary[1],),
    ):
        rejected = run(binary, "--apply-gestures", messages)
        assert rejected.returncode != 0 and not rejected.stdout
        assert "Parameter message rejected" in rejected.stderr


if __name__ == "__main__":
    main()
