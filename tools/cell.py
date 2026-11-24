#!/usr/bin/env python3
# Original Cell Framework implementation; see LICENSE.
"""Create and check Cell Framework projects."""

import argparse
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import uuid
import xml.etree.ElementTree as ET
from urllib.parse import urlsplit


def template_files():
    script = Path(__file__).resolve()
    relative = "@CELL_CLI_TEMPLATE_RELATIVE@"
    if relative.startswith("@"):
        source = script.parent.parent
        return script.parent / "project", source / "examples" / "sdk_gain", source
    installed = script.parent / relative
    return installed, installed, installed


def designer_files(directory, html_name="cell-plugin-editor.html"):
    files = {}
    max_bytes = 8 * 1024 * 1024
    for name in ("cell-plugin-editor.html", "cell-ui-bindings.hpp"):
        path = directory / (html_name if name.endswith(".html") else name)
        if not path.is_file():
            raise ValueError(f"Designer export is missing: {path}")
        with path.open("rb") as stream:
            payload = stream.read(max_bytes + 1)
        if not payload or len(payload) > max_bytes or b"\0" in payload:
            raise ValueError(f"Designer export must contain 1 byte to 8 MiB of UTF-8 text: {path}")
        try:
            payload.decode("utf-8")
        except UnicodeDecodeError as error:
            raise ValueError(f"Designer export is not UTF-8: {path}") from error
        files[f"editor/{name}"] = payload
    return files


def new_project(args):
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,63}", args.name):
        raise ValueError("Use 1–64 ASCII letters, digits or underscores, starting with a letter")
    if re.fullmatch(r"CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9]", args.name, re.IGNORECASE):
        raise ValueError("Choose a project name that is valid on Windows")
    plugin_id = args.id or f"org.cell.generated.p{uuid.uuid4().hex}"
    if not re.fullmatch(r"[A-Za-z0-9_-]+(?:\.[A-Za-z0-9_-]+)+", plugin_id):
        raise ValueError("Use a stable reverse-domain plug-in ID")
    lv2_uri = args.lv2_uri if args.lv2_uri is not None else f"https://example.com/lv2/{plugin_id}"
    if not re.fullmatch(r"https?://[A-Za-z0-9._~:/#-]+", lv2_uri) or not urlsplit(lv2_uri).hostname:
        raise ValueError("Use a stable HTTP(S) LV2 URI, such as https://your-domain.com/lv2/my-gain")
    if args.ui and args.designer_dir is not None:
        raise ValueError("Choose either --ui or --designer-dir")

    formats = args.format or ["standalone"]
    android_app = "android" in formats
    if android_app and not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+", plugin_id):
        raise ValueError("Each Android app ID segment must start with a letter and contain only ASCII letters, digits, or underscores")
    has_au_identity = bool({"au", "auv3"}.intersection(formats)) or any(
        code is not None for code in (args.au_subtype, args.au_manufacturer))
    if has_au_identity:
        if not all(code is not None and re.fullmatch(r"[A-Za-z0-9]{4}", code)
                   for code in (args.au_subtype, args.au_manufacturer)):
            raise ValueError("Supply both --au-subtype and --au-manufacturer as four ASCII letters or digits")
        if "_" in plugin_id:
            raise ValueError("An Audio Unit bundle ID may contain letters, digits, hyphens, and periods")

    templates, gain, notices = template_files()
    replacements = {
        "{{PROJECT_NAME}}": args.name,
        "{{PLUGIN_ID}}": plugin_id,
        "{{BUILD_CLAP}}": "ON" if "clap" in formats else "OFF",
        "{{BUILD_VST3}}": "ON" if "vst3" in formats else "OFF",
        "{{BUILD_LV2}}": "ON" if "lv2" in formats else "OFF",
        "{{LV2_URI}}": lv2_uri,
        "{{BUILD_AU}}": "ON" if "au" in formats else "OFF",
        "{{BUILD_AUV3}}": "ON" if "auv3" in formats else "OFF",
        "{{BUILD_ANDROID}}": "ON" if android_app else "OFF",
        "{{AU_SUBTYPE}}": args.au_subtype or "",
        "{{AU_MANUFACTURER}}": args.au_manufacturer or "",
        "{{BUILD_UI}}": "ON" if args.ui else "OFF",
        "{{BUILD_DESIGNER}}": "ON" if args.designer_dir is not None else "OFF",
    }
    config = (templates / "CMakeLists.txt.in").read_text(encoding="utf-8")
    for marker, value in replacements.items():
        config = config.replace(marker, value)

    files = {
        "CMakeLists.txt": config.encode("utf-8"),
        "plugin-id.txt": (plugin_id + "\n").encode("ascii"),
        "lv2-uri.txt": (lv2_uri + "\n").encode("ascii"),
    }
    # The SDK configures this Python file on installation. Keep the Turtle
    # placeholder intact until CMake configures the generated project.
    ttl_uri_placeholder = b"@" + b"project_lv2_uri" + b"@"
    files["plugin.lv2.ttl.in"] = ((gain / "gain.lv2.ttl").read_bytes()
        .replace(b"https://example.com/cell/gain", ttl_uri_placeholder)
        .replace(b"Cell Gain Example", args.name.encode("ascii")))
    if args.designer_dir is not None:
        files.update(designer_files(args.designer_dir))
    if android_app:
        android_templates = templates / "android"
        android_example = notices / "examples" / "android_designer"
        if not android_templates.is_dir():
            android_templates = android_example
        for name in ("build.gradle", "src/main/AndroidManifest.xml",
                     "src/main/java/org/cell/examples/designer/MainActivity.java"):
            files[f"android/{name}"] = (android_templates / name).read_bytes()
        settings = (android_templates / "settings.gradle").read_text(encoding="utf-8")
        files["android/settings.gradle"] = settings.replace('"CellAndroidDesigner"', f'"{args.name}"').encode("utf-8")
        files["android/cell-app.properties"] = (
            f"identityFile=../plugin-id.txt\nname={args.name}\nnativeProject=../CMakeLists.txt\n"
            "versionCode=1\nversionName=0.1.0\n"
        ).encode("ascii")
        if args.designer_dir is None:
            default_editor = android_templates / "default-editor"
            if not default_editor.is_dir():
                default_editor = android_example.parent / "auv3_gain"
            files.update(designer_files(default_editor, html_name="Cell Gain Editor.html"))
    if has_au_identity:
        files["au-identity.txt"] = f"aufx\t{args.au_subtype}\t{args.au_manufacturer}\n".encode("ascii")
        files["check_au_bundle.py"] = (templates / "check_au_bundle.py").read_bytes()
        for name in ("au_gain_host.cpp", "au_cocoa_editor.mm"):
            source = templates / name
            if not source.is_file():
                source = notices / "tests" / name
            files[name] = source.read_bytes()
        for name, source_path in (("auv3_app.mm", "examples/auv3_gain/main.mm"),
                                  ("auv3-app.plist.in", "examples/auv3_gain/Info.plist.in"),
                                  ("auv3-app.entitlements", "framework/cmake/CellAUv3MacSandbox.entitlements")):
            source = templates / name
            if not source.is_file():
                source = notices / source_path
            files[name] = source.read_bytes()
    for name in ("gain.hpp", "processor.cpp", "main.cpp", "editor.cpp"):
        payload = (gain / name).read_bytes()
        files[name] = (payload.replace(b"com.example.cell.gain", plugin_id.encode("ascii"))
                              .replace(b"Cell Gain Example", args.name.encode("ascii"))
                              .replace(b"Cell SDK Example", b"Cell Project"))
    allocation_watch = templates / "allocation_watch.hpp"
    if not allocation_watch.is_file():
        allocation_watch = notices / "tests" / "allocation_watch.hpp"
    files["allocation_watch.hpp"] = allocation_watch.read_bytes()
    lv2_host = templates / "legacy_effect_host.cpp"
    if not lv2_host.is_file():
        lv2_host = notices / "tests" / "legacy_effect_host.cpp"
    files["legacy_effect_host.cpp"] = lv2_host.read_bytes()
    for name in ("render.cpp", "parameter_identity.cpp", "parameter-ids.tsv"):
        files[name] = (templates / name).read_bytes()
    files["README.md"] = (templates / "README.md").read_bytes()
    files["LICENSE"] = (notices / "LICENSE").read_bytes()
    notice = notices / "THIRD_PARTY_NOTICES.md"
    if not notice.is_file():
        notice = notices / "framework" / "THIRD_PARTY_NOTICES.md"
    files["THIRD_PARTY_NOTICES.md"] = notice.read_bytes()

    if args.github_ci:
        files[".github/workflows/cell.yml"] = (templates / "github-ci.yml").read_bytes()

    destination = args.output or Path(args.name)
    destination.mkdir()
    created = []
    directories = []
    try:
        for name, contents in files.items():
            path = destination / name
            if path.parent != destination:
                for parent in reversed(path.relative_to(destination).parents[:-1]):
                    directory = destination / parent
                    if directory not in directories:
                        directory.mkdir()
                        directories.append(directory)
            with path.open("xb") as stream:
                created.append(path)
                stream.write(contents)
    except OSError:
        for path in reversed(created):
            path.unlink()
        for directory in reversed(directories):
            directory.rmdir()
        try:
            destination.rmdir()
        except OSError:
            pass
        raise
    print(f"Created {destination} ({plugin_id})")


def run_stage(name, command, report_dir, timeout=None):
    log = report_dir / f"{name}.log"
    started = time.monotonic()
    error = None
    with log.open("wb") as output:
        try:
            exit_code = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                                       timeout=timeout).returncode
        except (OSError, subprocess.TimeoutExpired) as failure:
            error = str(failure)
            output.write((error + "\n").encode("utf-8"))
            exit_code = None
    result = {
        "name": name,
        "status": "passed" if exit_code == 0 else "failed",
        "exit_code": exit_code,
        "elapsed_seconds": round(time.monotonic() - started, 3),
        "log": log.name,
    }
    if error:
        result["reason"] = error
    return result


def test_summary(stage, junit):
    try:
        cases = ET.parse(junit).findall(".//testcase")
    except (OSError, ET.ParseError):
        if stage["status"] == "passed":
            stage.update(status="failed", reason="CTest did not produce a readable test report")
        return

    counts = {"passed": 0, "failed": 0, "skipped": 0}
    for case in cases:
        if case.find("failure") is not None or case.find("error") is not None:
            counts["failed"] += 1
        elif case.find("skipped") is not None or case.get("status") in {"disabled", "notrun", "skipped"}:
            counts["skipped"] += 1
        else:
            counts["passed"] += 1
    stage["tests"] = counts
    if stage["status"] == "passed" and (counts["failed"] or not counts["passed"]):
        stage.update(status="failed", reason="CTest did not report a successful test run")


def validate_plugin(format_name, executable, build, config, report_dir, timeout):
    name = f"validate-{format_name}"
    receipt = build / "cell-artifacts" / config / f"{format_name}.txt"
    try:
        value = receipt.read_text(encoding="utf-8").rstrip("\r\n")
        artifact = Path(value)
        if (not artifact.is_absolute() or artifact.suffix.lower() != f".{format_name}" or
                not artifact.exists()):
            raise ValueError(f"No built {format_name.upper()} artifact in {receipt}")
    except (OSError, ValueError) as error:
        log = report_dir / f"{name}.log"
        log.write_text(str(error) + "\n", encoding="utf-8")
        return {"name": name, "status": "failed", "exit_code": None,
                "reason": str(error), "log": log.name}

    command = [executable, "validate"] if format_name == "clap" else [executable]
    stage = run_stage(name, [*command, str(artifact)], report_dir, timeout=timeout)
    stage["artifact"] = str(artifact)
    return stage


def doctor(args):
    validators = dict(args.validator)
    if len(validators) != len(args.validator) or validators.keys() - {"clap", "vst3"}:
        raise ValueError("Select each validator format at most once (clap or vst3)")
    if validators and not re.fullmatch(r"[A-Za-z0-9_-]+", args.config):
        raise ValueError("Use a standard CMake configuration name for validator runs")
    if args.validator_timeout <= 0:
        raise ValueError("Validator timeout must be positive")
    project = args.project.resolve()
    if not (project / "CMakeLists.txt").is_file():
        raise ValueError(f"No CMakeLists.txt in {project}")
    build = args.build_dir.resolve() if args.build_dir else None
    if build == project:
        raise ValueError("Use an out-of-source build directory")
    report_root = project / ".cell-doctor"
    report_root.mkdir(exist_ok=True)
    report_dir = Path(tempfile.mkdtemp(prefix="run-", dir=report_root))
    build = build or report_dir / "build"

    configure = ["cmake", "-S", str(project), "-B", str(build),
                 f"-DCMAKE_BUILD_TYPE={args.config}"]
    if args.sdk:
        configure.append(f"-DCMAKE_PREFIX_PATH={args.sdk.resolve()}")
    configure.extend(args.cmake_arg)
    junit = report_dir / "tests.xml"
    pipeline = [
        ("configure", configure),
        ("build", ["cmake", "--build", str(build), "--config", args.config]),
        ("test", ["ctest", "--test-dir", str(build), "-C", args.config,
                  "--output-on-failure", "--no-tests=error", "--output-junit", str(junit)]),
    ]
    print(f"Doctor report directory: {report_dir}", flush=True)
    stages = []
    passed = True
    for name, command in pipeline:
        if passed:
            stage = run_stage(name, command, report_dir)
            if name == "test":
                test_summary(stage, junit)
            passed = stage["status"] == "passed"
        else:
            stage = {"name": name, "status": "skipped", "reason": "Earlier stage failed"}
        stages.append(stage)
        print(f"{name}: {stage['status']}", flush=True)

    tests_passed = passed
    for format_name, executable in validators.items():
        if tests_passed:
            stage = validate_plugin(format_name, executable, build, args.config, report_dir,
                                    args.validator_timeout)
        else:
            stage = {"name": f"validate-{format_name}", "status": "skipped",
                     "reason": "Earlier stage failed"}
        stages.append(stage)
        passed = passed and stage["status"] == "passed"
        print(f"{stage['name']}: {stage['status']}", flush=True)

    report = {
        "schema": "cell.doctor.v1",
        "status": "passed" if passed else "failed",
        "project": str(project),
        "build_directory": str(build),
        "configuration": args.config,
        "stages": stages,
    }
    path = report_dir / "report.json"
    path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Report: {path}")
    return 0 if passed else 1


def main():
    parser = argparse.ArgumentParser(prog="cell", description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    new = commands.add_parser("new", help="Generate a gain effect and smoke test")
    new.add_argument("name")
    new.add_argument("--id", help="Stable reverse-domain plug-in ID")
    new.add_argument("--output", type=Path, help="New directory to create")
    new.add_argument("--format", choices=("standalone", "clap", "vst3", "lv2", "au", "auv3", "android"), action="append",
                     help="Enable a plug-in format or add the Android app. Repeat for multiple formats.")
    new.add_argument("--lv2-uri", help="Stable HTTP(S) identifier for the LV2 plug-in")
    new.add_argument("--au-subtype", help="Stable four-character Audio Unit subtype")
    new.add_argument("--au-manufacturer", help="Stable four-character Audio Unit manufacturer code")
    new.add_argument("--github-ci", action="store_true", help="Include the GitHub Actions build and test workflow")
    new.add_argument("--ui", action="store_true", help="Enable the Cell native plug-in editor")
    new.add_argument("--designer-dir", type=Path, help="Bundle the HTML and C++ bindings exported by Cell UI Designer")
    check = commands.add_parser("doctor", help="Configure, build, and test a project")
    check.add_argument("project", nargs="?", type=Path, default=Path("."))
    check.add_argument("--sdk", type=Path, help="Installed Cell SDK prefix")
    check.add_argument("--build-dir", type=Path, help="Optional reusable build directory")
    check.add_argument("--config", default="Release", help="Build configuration")
    check.add_argument("--cmake-arg", action="append", default=[],
                       help="Additional configure argument. Use --cmake-arg=-DNAME=VALUE.")
    check.add_argument("--validator", nargs=2, action="append", default=[],
                       metavar=("FORMAT", "EXECUTABLE"),
                       help="Run clap-validator or Steinberg validator on a built bundle (repeatable)")
    check.add_argument("--validator-timeout", type=int, default=120, metavar="SECONDS")
    args = parser.parse_args()
    try:
        if args.command == "doctor":
            return doctor(args)
        new_project(args)
    except (OSError, ValueError) as error:
        print(f"cell: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
