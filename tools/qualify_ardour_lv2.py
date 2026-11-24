#!/usr/bin/env python3
"""Qualify the Cell Gain LV2 bundle through Ardour's session and export tools."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import wave

PLUGIN_URI = "https://example.com/cell/gain"
RATE = 48000
FRAMES = RATE // 2
SETTLE_FRAMES = RATE // 50
MAX_ERROR = 1e-6
SNAPSHOTS = {"dry": 0, "attenuated": -6, "boosted": 3}


def run_host(command, env, log):
    try:
        with log.open("wb") as stream:
            result = subprocess.run(command, env=env, stdout=stream, stderr=subprocess.STDOUT,
                                    timeout=45, check=False)
    except subprocess.TimeoutExpired as error:
        raise ValueError(f"Ardour timed out. See {log}") from error
    if result.returncode:
        raise ValueError(f"Ardour exited with status {result.returncode}. See {log}")


def verify_render(path, gain_db):
    with wave.open(str(path), "rb") as render:
        actual = (render.getframerate(), render.getnchannels(), render.getsampwidth(), render.getnframes())
        if actual != (RATE, 2, 3, FRAMES):
            raise ValueError(f"{path.name}: expected {FRAMES} stereo PCM24 frames at {RATE} Hz, got {actual}")
        data = render.readframes(FRAMES)
    if len(data) != FRAMES * 6:
        raise ValueError(f"{path.name}: incomplete render")

    gain = 10 ** (gain_db / 20)
    expected = (0.25 * gain, -0.125 * gain)
    errors = [0.0, 0.0]
    for offset in range(SETTLE_FRAMES * 6, len(data), 6):
        for channel, target in enumerate(expected):
            start = offset + channel * 3
            sample = int.from_bytes(data[start:start + 3], "little", signed=True) / (1 << 23)
            errors[channel] = max(errors[channel], abs(sample - target))
    if max(errors) > MAX_ERROR:
        raise ValueError(f"{path.name}: expected {gain_db} dB on both channels, peak errors {errors}")
    return {
        "gain_db": gain_db,
        "frames": FRAMES,
        "checked_frames": FRAMES - SETTLE_FRAMES,
        "peak_error_by_channel": errors,
    }


def qualify(lua, exporter, bundle, output):
    if bundle.suffix != ".lv2" or not (bundle / "manifest.ttl").is_file():
        raise ValueError(f"Expected a built LV2 bundle directory, got {bundle}")
    if bundle == output or bundle in output.parents:
        raise ValueError("Choose an output directory outside the LV2 bundle")
    output.mkdir(parents=True)
    staged = Path(shutil.copytree(bundle, output / "lv2" / bundle.name))
    config, cache, data = (output / kind for kind in ("config", "cache", "data"))
    env = dict(os.environ, LV2_PATH=str(staged.parent), XDG_CONFIG_HOME=str(config),
               XDG_CACHE_HOME=str(cache), XDG_DATA_HOME=str(data))
    for directory in (config, cache, data):
        directory.mkdir()

    lua_log, export_log = output / "lua-version.log", output / "export-version.log"
    run_host([lua, "--version"], env, lua_log)
    run_host([exporter, "--version"], env, export_log)
    lua_version = lua_log.read_text(encoding="utf-8", errors="replace")
    export_version = export_log.read_text(encoding="utf-8", errors="replace")
    lua_release = re.search(r"\bversion\s+((\d+)\.\S+)", lua_version)
    export_release = re.search(r"\bversion\s+(\d+\.\S+)", export_version)
    if not lua_release or not export_release or lua_release[1] != export_release[1]:
        raise ValueError(f"Could not confirm matching Ardour tools. See version logs in {output}")
    version, major = lua_release.groups()
    scripts = config / f"ardour{major}" / "scripts"
    scripts.mkdir(parents=True)
    tool = Path(__file__).resolve()
    stimulus = tool.with_name("qualify_ardour_lv2_stimulus.lua")
    shutil.copyfile(stimulus, scripts / stimulus.name)

    session = output / "CellLV2"
    settings = [str(session), str(RATE), str(FRAMES), PLUGIN_URI]
    for name, gain in SNAPSHOTS.items():
        if name != "dry":
            settings.extend((name, str(gain)))
    run_host([lua, "--", str(tool.with_suffix(".lua")), *settings], env, output / "create.log")
    report = {
        "ardour_version": version,
        "plugin_uri": PLUGIN_URI,
        "sample_rate_hz": RATE,
        "max_peak_error": MAX_ERROR,
        "bundle_hashes": {
            str(path.relative_to(staged)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sorted(staged.rglob("*")) if path.is_file()
        },
        "snapshots": {},
    }
    for name, gain in SNAPSHOTS.items():
        if not (session / f"{name}.ardour").is_file():
            raise ValueError(f"Ardour did not save {name}. See {output / 'create.log'}")
        rendered = output / f"{name}.wav"
        run_host([exporter, "-b", "24", "-s", str(RATE), "-o", str(rendered), str(session), name],
                 env, output / f"{name}-export.log")
        report["snapshots"][name] = verify_render(rendered, gain)
        print(f"Ardour {version}: reopened and rendered {name} ({gain:+} dB)", flush=True)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ardour-lua", default="ardour8-lua", help="Ardour Lua executable")
    parser.add_argument("--ardour-export", default="ardour8-export", help="matching Ardour export executable")
    parser.add_argument("--bundle", type=Path, required=True, help="built CellGain_LV2.lv2 directory")
    parser.add_argument("--output-dir", type=Path, required=True, help="new directory for session and evidence")
    args = parser.parse_args()
    try:
        qualify(args.ardour_lua, args.ardour_export, args.bundle.resolve(), args.output_dir.resolve())
    except (OSError, ValueError, wave.Error) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
