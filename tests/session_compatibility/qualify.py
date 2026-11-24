#!/usr/bin/env python3
"""Capture, reopen, and package sessions from separately installed SDK builds."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import zipfile


def run(*args):
    print("+", *args, flush=True)
    subprocess.run(args, check=True)


def revision(source):
    return subprocess.check_output(["git", "-C", source, "rev-parse", "HEAD"], text=True).strip()


def build(source, work, args):
    sdk_build = work / "sdk-build"
    sdk = work / "sdk"
    native_build = work / "reference-build"
    install = work / "reference"
    flags = ["-DCMAKE_BUILD_TYPE=Release"]
    if args.fast_float:
        flags.append(f"-DCELL_FAST_FLOAT_INCLUDE_DIR={args.fast_float / 'include'}")
    run("cmake", "-S", source / "framework", "-B", sdk_build, *flags)
    run("cmake", "--build", sdk_build, "--config", "Release", "--parallel", str(args.jobs))
    run("cmake", "--install", sdk_build, "--config", "Release", "--prefix", sdk)

    run("cmake", "-S", args.candidate / "tests/session_compatibility", "-B", native_build,
        "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_PREFIX_PATH={sdk}",
        f"-DCELL_REFERENCE_SOURCE={source}", f"-DCELL_REFERENCE_CLAP_SDK={args.clap}")
    run("cmake", "--build", native_build, "--config", "Release", "--parallel", str(args.jobs))
    run("cmake", "--install", native_build, "--config", "Release", "--prefix", install)
    paths = [install / part for part in (install / "paths.txt").read_text().splitlines()]
    if len(paths) != 2 or not all(path.is_file() for path in paths):
        raise RuntimeError(f"Expected the installed host and CLAP binary under {install}")
    return install, paths


def check_rejection(candidate, project, expected):
    host, plugin = candidate
    result = subprocess.run([host, "verify", plugin, project],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode == 0 or expected not in result.stdout:
        raise RuntimeError(f"Invalid fixture {project.name} did not fail at {expected!r}\n{result.stdout}")
    return {"case": project.name, "diagnostic": expected}


def add_directory(archive, path, prefix):
    for file in sorted(path.rglob("*")):
        if file.is_file():
            archive.write(file, (Path(prefix) / file.relative_to(path)).as_posix())


def qualify(args):
    expected = json.loads((args.candidate / "tests/session_compatibility/baseline.json").read_text())
    previous_commit = revision(args.previous)
    candidate_commit = revision(args.candidate)
    if previous_commit != expected["commit"]:
        raise RuntimeError(f"Expected baseline {expected['commit']}, got {previous_commit}")
    if previous_commit == candidate_commit:
        raise RuntimeError("Session qualification needs different previous and candidate revisions")

    project = args.output / "capture" / "project"
    project.mkdir(parents=True)
    previous_install, previous = build(args.previous, args.output / "previous", args)
    examples = args.previous / "examples/portable_graph"
    run(previous[0], "capture", previous[1], project, examples)
    moved = args.output / "relocated" / "material" / "project"
    moved.parent.mkdir(parents=True)
    project.rename(moved)

    candidate_install, candidate = build(args.candidate, args.output / "candidate", args)

    negatives = args.output / "rejections"
    missing = negatives / "missing-asset"
    shutil.copytree(moved, missing)
    (missing / "assets/input.wav").unlink()
    rejected = [check_rejection(candidate, missing, "Referenced asset is missing")]
    remapped = negatives / "lost-automation"
    shutil.copytree(moved, remapped)
    manifest = remapped / "session.json"
    session = json.loads(manifest.read_text(encoding="utf-8"))
    session["automation"][0]["parameter"] = (session["gain"] + 1) % (2**32 - 1)
    manifest.write_text(json.dumps(session), encoding="utf-8")
    rejected.append(check_rejection(candidate, remapped, "CLAP offline reference is outside its tolerance"))
    corrupted = negatives / "corrupted-preset"
    shutil.copytree(moved, corrupted)
    preset = corrupted / "factory.preset"
    data = preset.read_bytes()
    preset.write_bytes(bytes([data[0] ^ 1]) + data[1:])
    rejected.append(check_rejection(candidate, corrupted, "Candidate could not read the saved state or preset"))

    result = subprocess.run([candidate[0], "verify", candidate[1], moved], check=True,
                            stdout=subprocess.PIPE, text=True)
    print(result.stdout, end="")
    report = {
        "schema": "cell.session-qualification.v1",
        "previous": expected,
        "candidate": candidate_commit,
        "platform": args.platform,
        "audio": {"sample_rate": 48000, "channels": 2, "peak_absolute_limit": 1e-6, "rms_limit": 1e-7},
        "verified": result.stdout.splitlines(),
        "negative_controls": rejected,
    }
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    destination = args.output / f"cell-session-reference-{args.platform}.zip"
    with zipfile.ZipFile(destination, "x", compression=zipfile.ZIP_DEFLATED) as archive:
        add_directory(archive, previous_install, "previous")
        add_directory(archive, candidate_install, "candidate")
        add_directory(archive, moved, "session")
        archive.write(args.output / "report.json", "report.json")
        archive.write(args.candidate / "framework/SESSION-COMPATIBILITY.md", "SESSION-COMPATIBILITY.md")
        archive.write(args.candidate / "LICENSE", "LICENSE")
        archive.write(args.candidate / "NOTICE", "NOTICE")
    print(destination)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("previous", "candidate", "clap", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--fast-float", type=Path)
    parser.add_argument("--platform", required=True, choices=("linux-x64", "linux-arm64", "windows-x64",
                                                              "macos-arm64", "macos-x64"))
    parser.add_argument("--jobs", type=int, default=3)
    args = parser.parse_args()
    for name in ("previous", "candidate", "clap", "output", "fast_float"):
        value = getattr(args, name)
        if value is not None:
            setattr(args, name, value.resolve())
    qualify(args)
