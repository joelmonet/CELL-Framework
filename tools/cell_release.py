# Original Cell Framework implementation; see LICENSE.
"""Portable release fixtures and evidence for generated Cell projects."""

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import subprocess
import sys


FIXTURE_SCHEMA = "cell.release-fixture.v1"
BASELINE_SCHEMA = "cell.release-baseline.v1"
SCOPES = {"shipping-clap", "processor-reference"}
BASELINE_FILE = "baseline.json"
SESSION_FILE = "session.json"


def read_object(path):
    with path.open("rb") as stream:
        payload = stream.read(8 * 1024 * 1024 + 1)
    if len(payload) > 8 * 1024 * 1024:
        raise ValueError("Release JSON exceeds 8 MiB")
    data = json.loads(payload)
    if not isinstance(data, dict):
        raise ValueError(f"Expected a release JSON object in {path.name}")
    return data


def write_object(path, data, mode="x"):
    with path.open(mode, encoding="utf-8", newline="\n") as output:
        json.dump(data, output, indent=2, sort_keys=True, allow_nan=False)
        output.write("\n")


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(128 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def project_file(root, name):
    if not isinstance(name, str) or not name or "\\" in name or "\x00" in name or ":" in name:
        raise ValueError("Release assets need portable relative paths")
    relative = PurePosixPath(name)
    if relative.is_absolute() or ".." in relative.parts or relative.as_posix() != name:
        raise ValueError("Release assets need portable relative paths")
    path = (root / name).resolve()
    if not path.is_relative_to(root) or not path.is_file():
        raise ValueError(f"Referenced asset is missing or outside the project: {name}")
    return path


def fixture_file(project, explicit):
    path = explicit.resolve() if explicit else project / "release-fixture.json"
    if not path.is_file() and explicit is None:
        return {"schema": FIXTURE_SCHEMA, "tolerances": {"peak": 1e-6, "rms": 1e-7}}
    fixture = read_object(path)
    allowed = {"schema", "tolerances", "assets", "input", "session_state", "preset_state",
               "automation", "factory_presets"}
    if fixture.get("schema") != FIXTURE_SCHEMA or fixture.keys() - allowed:
        raise ValueError("Expected a cell.release-fixture.v1 file")
    limits = fixture.get("tolerances")
    if not isinstance(limits, dict) or limits.keys() != {"peak", "rms"} or any(
            type(value) not in (int, float) or not 0 <= value <= sys.float_info.max
            for value in limits.values()):
        raise ValueError("Declare finite, nonnegative peak and RMS tolerances")
    paths = fixture.get("assets", [])
    if not isinstance(paths, list) or len(paths) > 256 or any(not isinstance(name, str) for name in paths):
        raise ValueError("Declare up to 256 relative asset paths")
    if len(set(paths)) != len(paths):
        raise ValueError("Release fixture has duplicate asset paths")
    if "input" in fixture and (not isinstance(fixture["input"], str) or
                               PurePosixPath(fixture["input"]).suffix.lower() != ".wav"):
        raise ValueError("Release render input must be a relative WAVE path")
    return fixture


def artifact_receipt(build, config, previous=False):
    root = build / "cell-artifacts" / config
    receipt = root / "release.txt"
    if receipt.is_file() and receipt.stat().st_size:
        lines = receipt.read_text(encoding="utf-8").splitlines()
        if len(lines) != 3 or lines[0] not in SCOPES:
            raise ValueError("Invalid generated release build receipt")
        scope, bundle, identity = lines
        native = Path(identity).resolve()
        if not native.is_file():
            raise ValueError("The release build is missing its parameter identity executable")
    elif (root / "clap.txt").is_file():
        scope, bundle = "shipping-clap", (root / "clap.txt").read_text(encoding="utf-8").strip()
        candidates = (build / config, build)
        filename = "project_parameter_identity" + (".exe" if platform.system() == "Windows" else "")
        native = next((directory / filename for directory in candidates if (directory / filename).is_file()), None)
    else:
        raise ValueError("Expected a generated CLAP build or a release compatibility build")
    if not previous and native is None:
        raise ValueError("The release build is missing its parameter identity executable")
    if not bundle or not Path(bundle).is_absolute():
        raise ValueError("The build has no release compatibility plug-in")
    return scope, Path(bundle).resolve(), native


def processor_snapshot(executable, report_dir, run_stage, label):
    if executable is None:
        return None
    stage = run_stage(label, [str(executable), "--snapshot"], report_dir, timeout=30)
    if stage["status"] != "passed":
        raise ValueError("The parameter identity snapshot failed")
    return parse_identity((report_dir / stage["log"]).read_text(encoding="utf-8"))


def parse_identity(text):
    parameters = {}
    ids = set()
    for line in text.splitlines():
        parts = line.split("\t")
        if (len(parts) != 8 or not parts[0] or not parts[1].isascii() or not parts[1].isdigit()
                or parts[0] in parameters or parts[1] in ids):
            raise ValueError("Invalid processor parameter identity")
        parameters[parts[0]] = parts[1:]
        ids.add(parts[1])
    if not parameters:
        raise ValueError("No released processor parameter identities")
    return parameters


def require_identity(previous, current):
    if previous is None:
        return {"status": "unavailable", "reason": "Previous artifact has no processor identity snapshot"}
    if not isinstance(previous, dict) or not previous or not isinstance(current, dict):
        raise ValueError("Invalid released processor identity snapshot")
    for key, fields in previous.items():
        if current.get(key) != fields:
            raise ValueError(f"Released processor parameter mapping changed: {key}")
    return {"status": "passed", "count": len(previous)}


def artifact_fingerprint(bundle, module):
    result = {"sha256": digest(module)}
    if bundle.is_dir():
        result["bundle_files"] = {path.relative_to(bundle).as_posix(): digest(path)
                                  for path in sorted(bundle.rglob("*")) if path.is_file()}
    return result


def host_environment():
    # Opt-in development hooks must stay disabled during release qualification.
    return {key: value for key, value in os.environ.items()
            if not key.startswith(("CELL_DOCTOR_TRACE_", "CELL_DESIGNER_LIVE_"))}


def native_check(helper, mode, module, bundle, folder, payload, cwd, log, timeout):
    with log.open("wb") as stream:
        result = subprocess.run([str(helper), mode, str(module), str(bundle), str(folder), str(payload)],
                                stdout=stream, stderr=subprocess.STDOUT, cwd=cwd,
                                env=host_environment(), timeout=timeout)
    return result.returncode


def released_files(session):
    cases = session.get("cases")
    if not isinstance(cases, list) or not 2 <= len(cases) <= 34:
        raise ValueError("Invalid saved release cases")
    return {SESSION_FILE, "input.wav", "state-0.bin", "state-1.bin"} | {
        f"render-{index}.wav" for index in range(len(cases))
    }


def capture(helper, module, artifact, scope, identity, project, fixture, baseline, report_dir, timeout):
    baseline.mkdir(parents=True)
    assets = set(fixture.get("assets", []))
    if fixture.get("input"):
        assets.add(fixture["input"])
    assets = {name: digest(project_file(project, name)) for name in sorted(assets)}
    for field, name in (("input", "input.wav"), ("session_state", "provided-session.bin"),
                        ("preset_state", "provided-preset.bin")):
        if field in fixture:
            shutil.copyfile(project_file(project, fixture[field]), baseline / name)
    recipe = report_dir / "capture-fixture.json"
    write_object(recipe, fixture)
    log = report_dir / "capture.log"
    if native_check(helper, "capture", module, artifact, baseline, recipe, project, log, timeout):
        detail = log.read_text(encoding="utf-8", errors="replace").strip()
        raise ValueError("Previous plug-in could not save a reference" + (f": {detail}" if detail else ""))
    for name in ("provided-session.bin", "provided-preset.bin"):
        (baseline / name).unlink(missing_ok=True)
    published = released_files(read_object(baseline / SESSION_FILE))
    manifest = {"schema": BASELINE_SCHEMA, "scope": scope, "platform": platform_key(),
                "artifact": artifact_fingerprint(artifact, module), "processor_parameters": identity,
                "assets": assets, "files": {name: digest(baseline / name) for name in sorted(published)}}
    write_object(baseline / BASELINE_FILE, manifest)
    return manifest


def platform_key():
    arch = platform.machine().lower()
    return {"os": platform.system(), "arch": {"amd64": "x86_64", "aarch64": "arm64"}.get(arch, arch)}


def validate_baseline(baseline, scope, report):
    manifest = read_object(baseline / BASELINE_FILE)
    if manifest.get("schema") != BASELINE_SCHEMA or manifest.get("scope") != scope:
        raise ValueError("The previous release must use the same compatibility scope as the candidate")
    if manifest.get("platform") != platform_key():
        raise ValueError("Use a previous baseline for this operating system and architecture")
    expected = manifest.get("files")
    if not isinstance(expected, dict) or SESSION_FILE not in expected:
        raise ValueError("Release baseline has no file inventory")
    for name, checksum in expected.items():
        if digest(project_file(baseline, name)) != checksum:
            raise ValueError(f"Released baseline file changed: {name}")
    if set(expected) != released_files(read_object(baseline / SESSION_FILE)):
        raise ValueError("Incomplete released baseline file inventory")
    report["previous"] = manifest.get("artifact")
    report["baseline_sha256"] = digest(baseline / BASELINE_FILE)
    return manifest


def check_assets(assets, project, report):
    if not isinstance(assets, dict):
        raise ValueError("Release baseline has no asset inventory")
    results = []
    report["referenced_assets"] = results
    for name, expected in sorted(assets.items()):
        try:
            actual = digest(project_file(project, name))
        except ValueError:
            results.append({"path": name, "sha256": expected, "status": "missing"})
            continue
        status = "passed" if actual == expected else "changed"
        results.append({"path": name, "sha256": expected, "status": status})
    if any(item["status"] != "passed" for item in results):
        raise ValueError("A released project asset is missing or has changed")
