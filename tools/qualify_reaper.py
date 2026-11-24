#!/usr/bin/env python3
"""Save, reopen and render the reference CLAP, VST3 and JSFX in REAPER."""
import argparse
import hashlib
import io
import json
import math
import os
from pathlib import Path, PureWindowsPath
import platform
import re
import shutil
import struct
import subprocess
import sys
import wave

RATE = 48000
FRAMES = RATE // 2
GAIN_DB = -6
MAX_ERROR = 1e-6  # -120 dBFS peak tolerance for the complete DAW render path.
EXAMPLE = "Cell Gain Example"
AUTOMATION_STEPS = [
    {"frame": 0, "gain_db": GAIN_DB},
    {"frame": RATE * 16 // 100, "gain_db": 3},
    {"frame": RATE * 32 // 100, "gain_db": -12},
]
GUARD_FRAMES = RATE * 30 // 1000


def stage_stimulus(path, previous=None):
    """Use different signals on each channel to detect routing mistakes."""
    pcm = []
    for index in range(FRAMES):
        phase = 2 * math.pi * index / RATE
        pcm.extend((round(10000 * math.sin(phase * 701)),
                    round(7000 * math.cos(phase * 991))))
    reference = struct.pack(f"<{len(pcm)}h", *pcm)
    if previous is None:
        with wave.open(str(path), "wb") as stream:
            stream.setparams((2, 2, RATE, 0, "NONE", "not compressed"))
            stream.writeframes(reference)
    else:
        original = previous.read_bytes()
        with wave.open(io.BytesIO(original), "rb") as stream:
            shape = (stream.getnchannels(), stream.getsampwidth(), stream.getframerate(), stream.getnframes())
            if shape != (2, 2, RATE, FRAMES) or stream.readframes(FRAMES) != reference:
                raise ValueError(f"{previous} does not match the qualification stimulus")
        path.write_bytes(original)
    return pcm


def copy_project_for_render(original, destination):
    """Preserve the saved effect state while redirecting output to the copied project."""
    project = original.read_bytes()
    output = f'  RENDER_FILE "./{destination.stem}.wav"'.encode("ascii")
    staged, replacements = re.subn(rb"(?m)^  RENDER_FILE [^\r\n]*", output, project)
    if replacements != 1:
        raise ValueError(f"{original} has no unique REAPER render destination")
    destination.write_bytes(staged)
    return hashlib.sha256(project).hexdigest()


def verify_render(path, source, automation=False):
    with wave.open(str(path), "rb") as render:
        actual = (render.getframerate(), render.getnchannels(), render.getsampwidth(), render.getnframes())
        if actual != (RATE, 2, 3, FRAMES):
            raise ValueError(f"{path.name}: expected {FRAMES} stereo PCM24 frames at {RATE} Hz, got {actual}")
        data = render.readframes(FRAMES)
    if len(data) != FRAMES * 6:
        raise ValueError(f"{path.name}: incomplete render")
    # Avoid host scheduling and gain smoothing near the edges of each plateau.
    windows = [(RATE // 10, FRAMES - RATE // 20, GAIN_DB)]
    if automation:
        boundaries = [step["frame"] for step in AUTOMATION_STEPS[1:]] + [FRAMES]
        windows = [(step["frame"] + GUARD_FRAMES, end - GUARD_FRAMES, step["gain_db"])
                   for step, end in zip(AUTOMATION_STEPS, boundaries)]
    segments = []
    for start, end, gain_db in windows:
        gain = 10 ** (gain_db / 20)
        errors = [0.0, 0.0]
        for sample in range(2 * start, 2 * end):
            offset = 3 * sample
            rendered = int.from_bytes(data[offset:offset + 3], "little", signed=True) / (1 << 23)
            expected = source[sample] * gain / (1 << 15)
            channel = sample % 2
            errors[channel] = max(errors[channel], abs(rendered - expected))
        if max(errors) > MAX_ERROR:
            raise ValueError(f"{path.name}: expected {gain_db} dB on both channels at frames {start}–{end - 1}, "
                             f"peak errors {errors}")
        segments.append({"start_frame": start, "end_frame": end, "gain_db": gain_db,
                         "peak_error_by_channel": errors})
    result = {
        "frames": FRAMES,
        "checked_frames": sum(end - start for start, end, _ in windows),
        "peak_error_by_channel": [max(segment["peak_error_by_channel"][channel] for segment in segments)
                                  for channel in range(2)],
    }
    if automation:
        result["automation_segments"] = segments
    return result


def run_reaper(command, options, env, log):
    try:
        with log.open("w", encoding="utf-8") as output:
            process = subprocess.run([*command, *options], env=env, stdout=output, stderr=subprocess.STDOUT,
                                     timeout=45, check=False)
    except subprocess.TimeoutExpired as error:
        raise ValueError(f"REAPER timed out. See {log}") from error
    if process.returncode:
        raise ValueError(f"REAPER exited with status {process.returncode}. See {log}")


def map_host_paths(wine, paths):
    if wine is None:
        return {path: str(path) for path in paths}
    try:
        converted = subprocess.run([str(wine), "winepath", "-w", *(str(path) for path in paths)],
                                   capture_output=True, text=True, timeout=45, check=True)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        raise ValueError(f"Wine could not translate the qualification paths: {error.stderr}") from error
    windows_paths = converted.stdout.splitlines()
    if len(windows_paths) != len(paths) or not all(PureWindowsPath(path).is_absolute() for path in windows_paths):
        raise ValueError(f"Unexpected winepath output: {converted.stdout!r}")
    return dict(zip(paths, windows_paths))


def find_binaries(build, windows):
    clap = build / "gain-example" / f"{EXAMPLE}.clap"
    vst3_dir = build / "VST3"
    vst3 = vst3_dir / "Release" / f"{EXAMPLE}.vst3"
    if windows:
        release_clap = clap.parent / "Release" / clap.name
        if release_clap.is_file():
            clap = release_clap
        bundled = vst3 / "Contents" / "x86_64-win" / vst3.name
        if not bundled.is_file() and not vst3.is_file():
            vst3 = vst3_dir / vst3.name
            bundled = vst3 / "Contents" / "x86_64-win" / vst3.name
        vst3_binary = vst3 if vst3.is_file() else bundled
    else:
        vst3_binary = vst3 / "Contents" / f"{platform.machine()}-linux" / f"{EXAMPLE}.so"
    for artifact in (clap, vst3_binary):
        if not artifact.is_file():
            raise ValueError(f"Missing {artifact}")
    return clap, vst3, vst3_binary


def run_project_script(name, executable, env, run, project, script, *, inspect, phase):
    env = dict(env, CELL_REAPER_FORMAT=name, CELL_REAPER_INSPECT="1" if inspect else "0",
               CELL_REAPER_REPORT_PHASE=phase)
    setup = run / f"{name}-{phase}.txt"
    setup_log = run / f"{name}-{phase}.log"
    run_reaper(executable, [project if inspect else "-new", script], env, setup_log)
    if not setup.is_file():
        raise ValueError(f"REAPER did not run the {phase} script. See {setup_log}")
    status, host, detail = setup.read_text(encoding="utf-8").split("\n", 2)
    if status != "PASS":
        raise ValueError(f"{name} {phase} failed. See {setup}\n{detail.strip()}")
    return host, detail.strip()


def qualify_format(name, artifact, executable, env, run, stimulus, host_paths, script, origin, automation):
    project = run / f"{name}.rpp"
    provenance = None
    if origin:
        prior_run, prior_report = origin
        prior_result = prior_report["formats"].get(name)
        if not isinstance(prior_result, dict) or not prior_result.get("passed"):
            raise ValueError(f"{prior_run} has no passing {name} qualification")
        provenance = {
            "runtime": prior_report.get("runtime"),
            "host": prior_result.get("host"),
            "artifact_sha256": prior_result.get("artifact_sha256"),
            "project_sha256": copy_project_for_render(prior_run / project.name, project),
        }
    host, effect = run_project_script(name, executable, env, run, host_paths[project], host_paths[script],
                                      inspect=bool(origin), phase="setup")
    if not project.is_file():
        raise ValueError(f"Missing saved project {project}")
    if automation and not origin:
        host, effect = run_project_script(name, executable, env, run, host_paths[project], host_paths[script],
                                          inspect=True, phase="reopen")
    run_reaper(executable, ["-renderproject", host_paths[project]], env, run / f"{name}-render.log")
    measured = verify_render(run / f"{name}.wav", stimulus, automation)
    action = "imported, reopened" if origin else "saved, reopened"
    print(f"{name}: {action} and rendered {measured['frames']} frames in REAPER {host}", flush=True)
    result = dict(measured, passed=True, effect=effect, host=host,
                  artifact_sha256=hashlib.sha256(artifact.read_bytes()).hexdigest())
    if provenance:
        result["source"] = provenance
    return result


def qualify(reaper, build, run, wine=None, import_run=None, automation=False):
    script = Path(__file__).resolve().with_suffix(".lua")
    framework = script.parents[1]
    windows = sys.platform == "win32" or wine is not None
    clap, vst3, vst3_binary = find_binaries(build, windows)
    jsfx = framework / "examples/sdk_gain/Cell Gain.jsfx"
    for artifact in (reaper, script, jsfx):
        if not artifact.is_file():
            raise ValueError(f"Missing {artifact}")
    origin = None
    if import_run is not None:
        prior_report = json.loads((import_run / "report.json").read_text(encoding="utf-8"))
        if (not isinstance(prior_report, dict) or not isinstance(prior_report.get("formats"), dict)
                or prior_report.get("sample_rate_hz") != RATE or prior_report.get("gain_db") != GAIN_DB):
            raise ValueError(f"{import_run} is not a compatible REAPER qualification run")
        saved_steps = prior_report.get("automation_steps")
        if (automation or saved_steps is not None) and saved_steps != AUTOMATION_STEPS:
            raise ValueError(f"{import_run} does not contain the expected automation steps")
        automation = saved_steps is not None
        origin = import_run, prior_report
    run.mkdir(parents=True)
    clap_dir = run / "clap"
    vst3_dir = run / "vst3"
    effects = run / "profile/Effects/Cell"
    for directory in (clap_dir, vst3_dir, effects):
        directory.mkdir(parents=True)
    # Render from one set of artifacts even if the SDK build is rebuilt during the run.
    staged_clap = Path(shutil.copy2(clap, clap_dir))
    if vst3.is_file():
        # A flat Windows module can be staged in the standard x64 bundle layout.
        contents = vst3_dir / vst3.name / "Contents" / "x86_64-win"
        contents.mkdir(parents=True)
        staged_vst3_binary = Path(shutil.copy2(vst3_binary, contents))
    else:
        staged_vst3 = Path(shutil.copytree(vst3, vst3_dir / vst3.name))
        staged_vst3_binary = staged_vst3 / vst3_binary.relative_to(vst3)
    staged_jsfx = Path(shutil.copy2(jsfx, effects))
    profile = run / "profile/reaper.ini"
    artifacts = (("clap", staged_clap), ("vst3", staged_vst3_binary), ("jsfx", staged_jsfx))
    projects = tuple(run / f"{name}.rpp" for name, _ in artifacts)
    host_paths = map_host_paths(wine, (run, profile, script, clap_dir, vst3_dir, *projects))
    # REAPER can service parameter edits without physical audio hardware in Dummy Audio mode.
    if windows:
        settings = f"[audioconfig]\nmode=4\ndummy_srate={RATE}\n\n[REAPER]\n"
        vst_path_key = "vstpath64"
    else:
        settings = f"[REAPER]\nlinux_audio_mode=2\nlinux_audio_srate={RATE}\n"
        vst_path_key = "vstpath"
    profile.write_text(settings + f"{vst_path_key}={host_paths[vst3_dir]}\nclap_path={host_paths[clap_dir]}\n",
                       encoding="utf-8")
    executable = [str(wine)] if wine is not None else []
    executable.extend((str(reaper), "-newinst", "-nosplash", "-ignoreerrors", "-cfgfile", host_paths[profile]))
    env = dict(os.environ, CLAP_PATH=host_paths[clap_dir], CELL_REAPER_RUN=host_paths[run],
               CELL_REAPER_RATE=str(RATE), CELL_REAPER_DURATION=str(FRAMES / RATE), CELL_REAPER_GAIN_DB=str(GAIN_DB))
    env["CELL_REAPER_AUTOMATION"] = (
        ",".join(f"{step['frame']}:{step['gain_db']}" for step in AUTOMATION_STEPS) if automation else ""
    )
    stimulus = stage_stimulus(run / "input.wav", import_run / "input.wav" if origin else None)
    runtime = "wine" if wine is not None else sys.platform
    report = {"runtime": runtime, "mode": "import" if origin else "create", "sample_rate_hz": RATE, "gain_db": GAIN_DB,
              "max_peak_error": MAX_ERROR, "formats": {}}
    if automation:
        report.update(automation_steps=AUTOMATION_STEPS, automation_guard_frames=GUARD_FRAMES)
    for name, artifact in artifacts:
        try:
            result = qualify_format(name, artifact, executable, env, run, stimulus, host_paths, script,
                                    origin, automation)
        except (OSError, ValueError, wave.Error) as error:
            print(f"{name}: {error}", file=sys.stderr, flush=True)
            result = {"passed": False, "error": str(error)}
        report["formats"][name] = result
    report_path = run / "report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if not all(result["passed"] for result in report["formats"].values()):
        raise ValueError(f"REAPER qualification failed. See {report_path}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reaper", type=Path, required=True, help="Linux or Windows x64 REAPER executable")
    parser.add_argument("--wine", type=Path, help="Wine loader for running Windows REAPER on Linux")
    parser.add_argument("--build-dir", type=Path, required=True, help="Cell SDK Release build with the gain examples")
    parser.add_argument("--import-run", type=Path, help="previous qualification directory with saved REAPER projects")
    parser.add_argument("--automation", action="store_true", help="qualify a saved three-step gain envelope")
    parser.add_argument("--output-dir", type=Path, required=True, help="new directory for projects, renders and logs")
    args = parser.parse_args()
    linux = sys.platform.startswith("linux")
    if (linux and not os.environ.get("DISPLAY")) or (not linux and (sys.platform != "win32" or args.wine)):
        parser.error("Run on Windows or on Linux with an X11 display. --wine requires Linux.")
    try:
        qualify(args.reaper.resolve(), args.build_dir.resolve(), args.output_dir.resolve(),
                args.wine.absolute() if args.wine else None,
                args.import_run.resolve() if args.import_run else None, args.automation)
    except (OSError, ValueError, wave.Error) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
