#!/usr/bin/env python3
# Original Cell Framework implementation; see LICENSE.
"""Create and check Cell Framework projects."""

import argparse
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
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


def installed_sdk():
    relative = "@CELL_CLI_SDK_RELATIVE@"
    if not relative.startswith("@"):
        return (Path(__file__).resolve().parent / relative).resolve()
    return None


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

    defaults = {"effect": "clap", "banked": "clap", "instrument": "clap", "mobile": "android"}
    formats = args.format or [defaults.get(args.template, "standalone")]
    banked = args.template == "banked"
    instrument = args.template == "instrument"
    if (banked or instrument) and (set(formats) != {"clap"} or args.ui or args.designer_dir is not None
                                  or args.au_subtype or args.au_manufacturer or args.lv2_uri):
        raise ValueError("Use CLAP for the banked and instrument templates, with their supplied controls")
    if args.template == "mobile" and "android" not in formats:
        raise ValueError("The mobile template requires the Android format")
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
    editor = args.designer_dir
    if banked:
        editor = templates / "banked" if templates == notices else notices / "examples/sdk_banked"
    if instrument:
        instrument_source = templates / "instrument" if templates == notices else notices / "examples/sdk_instrument"
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
        "{{BUILD_DESIGNER}}": "ON" if editor is not None else "OFF",
        "{{BANKED_FIXTURE}}": "ON" if banked else "OFF",
        "{{INSTRUMENT_TEMPLATE}}": "ON" if instrument else "OFF",
    }
    config = (templates / "CMakeLists.txt.in").read_text(encoding="utf-8")
    for marker, value in replacements.items():
        config = config.replace(marker, value)

    files = {
        "CMakeLists.txt": config.encode("utf-8"),
        "plugin-id.txt": (plugin_id + "\n").encode("ascii"),
        "lv2-uri.txt": (lv2_uri + "\n").encode("ascii"),
        "cell-project.json": (json.dumps({
            "schema": "cell.project.v1", "name": args.name, "plugin_id": plugin_id,
            "template": args.template or "effect", "formats": formats,
        }, indent=2) + "\n").encode("utf-8"),
    }
    # The SDK configures this Python file on installation. Keep the Turtle
    # placeholder intact until CMake configures the generated project.
    ttl_uri_placeholder = b"@" + b"project_lv2_uri" + b"@"
    files["plugin.lv2.ttl.in"] = ((gain / "gain.lv2.ttl").read_bytes()
        .replace(b"https://example.com/cell/gain", ttl_uri_placeholder)
        .replace(b"Cell Gain Example", args.name.encode("ascii")))
    if editor is not None:
        files.update(designer_files(editor))
    if banked:
        files["editor/layout.json"] = (editor / "layout.json").read_bytes()
        files["banked_check.cpp"] = (editor / "check.cpp").read_bytes()
        files["banked_clap_check.cpp"] = (editor / "clap_check.cpp").read_bytes()
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
        if instrument and name in ("processor.cpp", "main.cpp"):
            files[name] = ((instrument_source / name).read_bytes()
                .replace(b"com.example.cell.sine", plugin_id.encode("ascii"))
                .replace(b"Cell Sine", args.name.encode("ascii")))
            continue
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
    if instrument:
        files["parameter-ids.tsv"] = (instrument_source / "parameter-ids.tsv").read_bytes()
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
    if android_app or "clap" in formats:
        print(f'Build and open: cell run "{destination}"')


def run_stage(name, command, report_dir, timeout=None, env=None):
    log = report_dir / f"{name}.log"
    started = time.monotonic()
    error = None
    with log.open("wb") as output:
        try:
            exit_code = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                                       timeout=timeout, env=env).returncode
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
    try:
        artifact = artifact_path(build, config, format_name)
    except (OSError, ValueError) as error:
        log = report_dir / f"{name}.log"
        log.write_text(str(error) + "\n", encoding="utf-8")
        return {"name": name, "status": "failed", "exit_code": None,
                "reason": str(error), "log": log.name}

    command = [executable, "validate"] if format_name == "clap" else [executable]
    stage = run_stage(name, [*command, str(artifact)], report_dir, timeout=timeout)
    stage["artifact"] = str(artifact)
    return stage


def artifact_path(build, config, format_name):
    receipt = build / "cell-artifacts" / config / f"{format_name}.txt"
    artifact = Path(receipt.read_text(encoding="utf-8").strip())
    if not artifact.is_absolute() or artifact.suffix.lower() != f".{format_name}" or not artifact.exists():
        raise ValueError(f"No built {format_name.upper()} artifact in {receipt}")
    return artifact


def desktop_pipeline(args, project, build, junit, extra_configure=()):
    configure = ["cmake", "-S", str(project), "-B", str(build), f"-DCMAKE_BUILD_TYPE={args.config}"]
    sdk = args.sdk.resolve() if args.sdk else installed_sdk()
    if sdk:
        configure.append(f"-DCMAKE_PREFIX_PATH={sdk}")
    configure.extend(extra_configure)
    configure.extend(args.cmake_arg)
    return [
        ("configure", configure),
        ("build", ["cmake", "--build", str(build), "--config", args.config, "--parallel", str(args.jobs)]),
        ("test", ["ctest", "--test-dir", str(build), "-C", args.config,
                  "--output-on-failure", "--no-tests=error", "--output-junit", str(junit)]),
    ]


def run_pipeline(pipeline, report_dir):
    stages = []
    passed = True
    for name, command in pipeline:
        if passed:
            stage = run_stage(name, command, report_dir)
            if name == "test":
                test_summary(stage, report_dir / "tests.xml")
            passed = stage["status"] == "passed"
        else:
            stage = {"name": name, "status": "skipped", "reason": "Earlier stage failed"}
        stages.append(stage)
        print(f"{name}: {stage['status']}", flush=True)
    return stages, passed


def executable(value, name, defaults=()):
    candidate = shutil.which(os.fspath(value) if value else name)
    if not candidate and not value:
        candidate = next((str(path) for path in defaults if path.is_file()), None)
    if not candidate:
        raise ValueError(f"Could not find {value or name}. Supply --{'host' if name == 'reaper' else name}.")
    return Path(candidate).resolve()


def open_reaper(host, artifact, manifest, report_dir):
    stage = {"name": "open-reaper", "status": "failed", "log": "open-reaper.log"}
    started = time.monotonic()
    process = None
    copied = report_dir / "clap"
    log = report_dir / stage["log"]
    try:
        copied.mkdir()
        destination = copied / artifact.name
        if artifact.is_dir():
            shutil.copytree(artifact, destination, symlinks=True)
        else:
            shutil.copy2(artifact, destination)
        if any(char in str(copied) for char in ";\r\n"):
            raise ValueError("REAPER's CLAP path cannot contain a semicolon or newline")
        profile = report_dir / "reaper.ini"
        profile.write_text(f"[REAPER]\nlinux_audio_mode=2\nlinux_audio_srate=48000\nclap_path={copied}\n",
                           encoding="utf-8")
        lua = template_files()[0] / "open-in-reaper.lua"
        env = dict(os.environ, CLAP_PATH=str(copied), CELL_REFERENCE_RUN=str(report_dir),
                   CELL_REFERENCE_PLUGIN=manifest["name"],
                   CELL_REFERENCE_INSTRUMENT="1" if manifest["template"] == "instrument" else "0")
        with log.open("wb") as output:
            process = subprocess.Popen([host, "-newinst", "-nosplash", "-ignoreerrors", "-cfgfile", profile,
                                        "-new", lua], env=env, stdout=output, stderr=subprocess.STDOUT)
        result = report_dir / "host-result.txt"
        while time.monotonic() - started < 45:
            if result.is_file():
                message = result.read_text(encoding="utf-8").strip()
                if not message.startswith("PASS "):
                    raise ValueError(message)
                stage.update(status="passed", artifact=str(destination), host_pid=process.pid, result=message)
                print(message, flush=True)
                break
            if process.poll() is not None:
                raise ValueError(f"REAPER exited with code {process.returncode} before opening the editor")
            time.sleep(.1)
        else:
            raise ValueError("REAPER did not confirm an open editor within 45 seconds")
    except (OSError, ValueError) as error:
        stage["reason"] = str(error)
        with log.open("a", encoding="utf-8") as output:
            output.write(str(error) + "\n")
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    stage["elapsed_seconds"] = round(time.monotonic() - started, 3)
    return stage


def run_mobile(args, project, manifest, report_dir):
    android = project / "android"
    if not (android / "build.gradle").is_file():
        raise ValueError(f"No generated Android app in {project}")
    if args.android_cell_sdk is None:
        raise ValueError("Supply --android-cell-sdk with the installed Cell SDK per Android ABI")
    sdk = args.android_cell_sdk.resolve()
    for abi in ("arm64-v8a", "x86_64"):
        if not (sdk / abi / "lib/cmake/Cell/CellConfig.cmake").is_file():
            raise ValueError(f"Missing installed Android Cell SDK for {abi} below {sdk}")
    gradle = executable(args.gradle, "gradle")
    adb = []
    if not args.build_only:
        android_sdk = os.environ.get("ANDROID_HOME") or os.environ.get("ANDROID_SDK_ROOT")
        defaults = [Path(android_sdk) / "platform-tools" / ("adb.exe" if os.name == "nt" else "adb")] if android_sdk else []
        adb = [executable(args.adb, "adb", defaults)]
        if args.serial:
            adb.extend(["-s", args.serial])
    stages, passed = run_pipeline([
        ("build-apk", [gradle, "--project-dir", android, f"-PcellSdkRoot={sdk}", "assembleDebug"]),
    ], report_dir)
    if not passed:
        return stages, passed
    apk = android / "build/outputs/apk/debug/android-debug.apk"
    if not apk.is_file():
        candidates = list(apk.parent.glob("*.apk"))
        if len(candidates) != 1:
            raise ValueError(f"Expected a debug APK in {apk.parent}")
        apk = candidates[0]
    print(f"Built {apk}", flush=True)
    stages[0]["artifact"] = str(apk)
    if not args.build_only:
        component = manifest["plugin_id"] + "/org.cell.examples.designer.MainActivity"
        install = run_stage("install-apk", [*adb, "install", "-r", apk], report_dir, timeout=120)
        stages.append(install)
        print(f"install-apk: {install['status']}", flush=True)
        if install["status"] != "passed":
            return stages, False
        launch = run_stage("open-android", [*adb, "shell", "am", "start", "-W", "-n", component],
                           report_dir, timeout=60)
        if launch["status"] == "passed" and "Status: ok" not in (report_dir / launch["log"]).read_text(encoding="utf-8"):
            launch.update(status="failed", reason="Android did not confirm activity launch")
        stages.append(launch)
        passed = launch["status"] == "passed"
        print(f"open-android: {launch['status']}", flush=True)
    return stages, passed


def run_project(args):
    project = args.project.resolve()
    manifest = json.loads((project / "cell-project.json").read_text(encoding="utf-8"))
    if not isinstance(manifest, dict) or manifest.get("schema") != "cell.project.v1":
        raise ValueError("Expected a project generated by this SDK")
    if (not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]{0,63}", str(manifest.get("name", "")))
            or not isinstance(manifest.get("formats"), list)
            or manifest.get("plugin_id") != (project / "plugin-id.txt").read_text(encoding="ascii").strip()):
        raise ValueError("The generated project manifest and plug-in identity must agree")
    mobile = manifest.get("template") == "mobile" or ("android" in manifest["formats"] and "clap" not in manifest["formats"])
    if mobile and not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+", manifest["plugin_id"]):
        raise ValueError("Invalid Android app ID in cell-project.json")
    report_root = project / ".cell-run"
    report_root.mkdir(exist_ok=True)
    report_dir = Path(tempfile.mkdtemp(prefix="run-", dir=report_root))
    print(f"Quickstart logs: {report_dir}", flush=True)
    started = time.monotonic()
    if mobile:
        stages, passed = run_mobile(args, project, manifest, report_dir)
    else:
        if "clap" not in manifest["formats"]:
            raise ValueError("The reference host needs a CLAP project. Generate with --format clap.")
        fallback = (Path("/Applications/REAPER.app/Contents/MacOS/REAPER"),
                    Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "REAPER (x64)/reaper.exe")
        host = None if args.build_only else executable(args.host, "reaper", fallback)
        build = args.build_dir.resolve() if args.build_dir else report_dir / "build"
        if build == project:
            raise ValueError("Use an out-of-source build directory")
        stages, passed = run_pipeline(desktop_pipeline(args, project, build, report_dir / "tests.xml"), report_dir)
        if passed:
            artifact = artifact_path(build, args.config, "clap")
            stages[1]["artifact"] = str(artifact)
            print(f"Built {artifact}", flush=True)
            if host:
                stage = open_reaper(host, artifact, manifest, report_dir)
                stages.append(stage)
                passed = stage["status"] == "passed"
    report = {"schema": "cell.run.v1", "status": "passed" if passed else "failed", "project": str(project),
              "elapsed_seconds": round(time.monotonic() - started, 3), "stages": stages}
    receipt = report_dir / "report.json"
    receipt.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if not passed:
        stage = next(stage for stage in stages if stage["status"] == "failed")
        print(f"{stage['name']}: {stage.get('reason', 'see ' + stage['log'])}", file=sys.stderr)
    print(f"Report: {receipt}")
    return 0 if passed else 1


TRACE_ENV = ("CELL_DOCTOR_TRACE_DIR", "CELL_DOCTOR_PLUGIN_ID", "CELL_DOCTOR_TRACE_AUDIO")


def clap_module(bundle):
    if bundle.suffix.lower() != ".clap":
        raise ValueError(f"Expected a built .clap file or bundle: {bundle}")
    if bundle.is_file():
        return bundle
    contents = bundle / "Contents"
    with (contents / "Info.plist").open("rb") as stream:
        name = plistlib.load(stream).get("CFBundleExecutable", "")
    if not isinstance(name, str) or not name or Path(name).name != name or name in {".", ".."}:
        raise ValueError(f"Invalid CFBundleExecutable in {bundle}")
    module = contents / "MacOS" / name
    if not module.is_file():
        raise ValueError(f"Missing CLAP bundle executable: {module}")
    return module


def trace_environment():
    return {key: value for key, value in os.environ.items() if key not in TRACE_ENV}


def record_clap(args, bundle, project, report_dir):
    directory = args.record.resolve()
    if directory.exists() and (not directory.is_dir() or any(directory.iterdir())):
        raise ValueError("Use an empty recording directory")
    directory.mkdir(parents=True, exist_ok=True)
    settings = {"CELL_DOCTOR_TRACE_DIR": str(directory)}
    identity = project / "plugin-id.txt"
    if args.plugin_id:
        settings["CELL_DOCTOR_PLUGIN_ID"] = args.plugin_id
    elif not args.plugin and identity.is_file():
        settings["CELL_DOCTOR_PLUGIN_ID"] = identity.read_text(encoding="utf-8").strip()
    if args.record_audio:
        settings["CELL_DOCTOR_TRACE_AUDIO"] = "1"

    print(f"Record this CLAP build: {bundle}", flush=True)
    print(f"Close the plug-in or DAW to write each trace under {directory}", flush=True)
    log = report_dir / "record-clap.log"
    stage = {"name": "record-clap", "status": "passed", "log": log.name,
             "exit_code": None, "artifact": str(bundle), "environment": settings}
    if args.record_host:
        stage.update(run_stage("record-clap", [args.record_host, *args.host_arg], report_dir,
                               env={**trace_environment(), **settings}))
        paths = sorted(directory.glob("instance-*/host.celltrace"))
        stage["traces"] = [str(path) for path in paths]
        if stage["status"] == "passed" and not paths:
            stage.update(status="failed", reason="The DAW did not write a trace. Check the loaded plug-in build and launch a fresh DAW process.")
            with log.open("a", encoding="utf-8") as output:
                output.write(stage["reason"] + "\n")
    else:
        stage["status"] = "ready"
        log.write_text(json.dumps(settings, indent=2) + "\n", encoding="utf-8")
        print("Launch a fresh DAW process with the environment in record-clap.log.", flush=True)
    return stage


def replay_clap(args, bundle, module, report_dir):
    trace = args.replay.resolve()
    if not trace.is_file():
        raise ValueError(f"CLAP trace not found: {trace}")
    suffix = ".exe" if os.name == "nt" else ""
    defaults = [Path(__file__).resolve().with_name("cell-doctor-clap" + suffix)]
    if args.sdk:
        defaults.append(args.sdk.resolve() / "bin" / ("cell-doctor-clap" + suffix))
    helper = next((path for path in defaults if path.is_file()), None)
    if args.replay_host:
        helper = Path(args.replay_host).resolve()
    elif helper is None:
        helper = shutil.which("cell-doctor-clap")
    if not helper:
        raise ValueError("Install the desktop SDK or pass --replay-host /path/to/cell-doctor-clap")

    summary = report_dir / "clap-replay.json"
    command = [str(helper), str(module), str(bundle), str(trace), str(summary)]
    if args.realtime:
        command.append("--realtime")
    stage = run_stage("replay-clap", command, report_dir, timeout=args.replay_timeout, env=trace_environment())
    stage.update(artifact=str(bundle), trace=str(trace))
    if summary.is_file():
        stage["summary"] = summary.name
        stage["replay"] = json.loads(summary.read_text(encoding="utf-8"))
    return stage


def trace_stage(args, build, project, report_dir):
    name = "record-clap" if args.record else "replay-clap"
    try:
        bundle = args.plugin.resolve() if args.plugin else artifact_path(build, args.config, "clap")
        module = clap_module(bundle)
        if args.record:
            return record_clap(args, bundle, project, report_dir)
        return replay_clap(args, bundle, module, report_dir)
    except (OSError, ValueError, plistlib.InvalidFileException) as error:
        log = report_dir / f"{name}.log"
        log.write_text(str(error) + "\n", encoding="utf-8")
        return {"name": name, "status": "failed", "exit_code": None,
                "reason": str(error), "log": log.name}


def doctor(args):
    validators = dict(args.validator)
    if len(validators) != len(args.validator) or validators.keys() - {"clap", "vst3"}:
        raise ValueError("Select each validator format at most once (clap or vst3)")
    if validators and not re.fullmatch(r"[A-Za-z0-9_-]+", args.config):
        raise ValueError("Use a standard CMake configuration name for validator runs")
    if args.validator_timeout <= 0:
        raise ValueError("Validator timeout must be positive")
    if args.plugin and (not (args.record or args.replay) or validators):
        raise ValueError("--plugin selects a built CLAP artifact for recording or replay")
    if (args.record_audio or args.record_host or args.host_arg or args.plugin_id) and not args.record:
        raise ValueError("Recording options require --record")
    if args.host_arg and not args.record_host:
        raise ValueError("--host-arg requires --record-host")
    if (args.realtime or args.replay_host) and not args.replay:
        raise ValueError("Replay options require --replay")
    if args.replay_timeout < 1:
        raise ValueError("Replay timeout must be positive")
    project = args.project.resolve()
    if not args.plugin and not (project / "CMakeLists.txt").is_file():
        raise ValueError(f"No CMakeLists.txt in {project}")
    build = args.build_dir.resolve() if args.build_dir else None
    if build == project:
        raise ValueError("Use an out-of-source build directory")
    report_root = project / ".cell-doctor"
    report_root.mkdir(exist_ok=True)
    report_dir = Path(tempfile.mkdtemp(prefix="run-", dir=report_root))
    build = build or report_dir / "build"

    junit = report_dir / "tests.xml"
    print(f"Doctor report directory: {report_dir}", flush=True)
    stages, passed = [], True
    if not args.plugin:
        generated_probe = ("-DCELL_PROJECT_DOCTOR_ALLOCATIONS=ON",) if args.realtime and (
            project / "cell-project.json").is_file() else ()
        pipeline = desktop_pipeline(args, project, build, junit, generated_probe)
        stages, passed = run_pipeline(pipeline, report_dir)

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

    if args.record or args.replay:
        if tests_passed:
            stage = trace_stage(args, build, project, report_dir)
        else:
            stage = {"name": "record-clap" if args.record else "replay-clap",
                     "status": "skipped", "reason": "Earlier stage failed"}
        stages.append(stage)
        passed = passed and stage["status"] in {"passed", "ready"}
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


def build_options(parser):
    parser.add_argument("project", nargs="?", type=Path, default=Path("."))
    parser.add_argument("--sdk", type=Path, help="Installed desktop Cell SDK prefix")
    parser.add_argument("--build-dir", type=Path, help="Optional reusable desktop build directory")
    parser.add_argument("--config", default="Release", help="Desktop build configuration")
    parser.add_argument("--jobs", type=int, default=min(4, os.cpu_count() or 1), help="Parallel desktop build jobs")
    parser.add_argument("--cmake-arg", action="append", default=[],
                        help="Additional configure argument. Use --cmake-arg=-DNAME=VALUE.")


def main():
    parser = argparse.ArgumentParser(prog="cell", description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    new = commands.add_parser("new", help="Generate a Cell effect, instrument or mobile app")
    new.add_argument("name")
    new.add_argument("--template", choices=("effect", "banked", "instrument", "mobile"),
                     help="Select a CLAP effect, banked CLAP effect, polyphonic CLAP instrument or Android app")
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
    build_options(check)
    check.add_argument("--validator", nargs=2, action="append", default=[],
                       metavar=("FORMAT", "EXECUTABLE"),
                       help="Run clap-validator or Steinberg validator on a built bundle (repeatable)")
    check.add_argument("--validator-timeout", type=int, default=120, metavar="SECONDS")
    trace = check.add_mutually_exclusive_group()
    trace.add_argument("--record", type=Path, metavar="DIRECTORY", help="Prepare or launch a CLAP host recording")
    trace.add_argument("--replay", type=Path, metavar="TRACE", help="Replay a CLAP trace against the built plug-in")
    check.add_argument("--plugin", type=Path, metavar="CLAP", help="Use an existing CLAP artifact and skip the build")
    check.add_argument("--record-host", metavar="DAW", help="Launch a fresh DAW process with recording enabled")
    check.add_argument("--host-arg", action="append", default=[], help="DAW launch argument. Use --host-arg=VALUE.")
    check.add_argument("--plugin-id", help="Record only this CLAP ID (defaults to the generated project ID)")
    check.add_argument("--record-audio", action="store_true", help="Include float audio inputs in the trace")
    check.add_argument("--realtime", action="store_true", help="Pace CLAP replay and report real-time measurements")
    check.add_argument("--replay-host", help="Path to the Cell SDK's cell-doctor-clap executable")
    check.add_argument("--replay-timeout", type=int, default=120, metavar="SECONDS")
    launch = commands.add_parser("run", help="Build and open a generated project in REAPER or on Android")
    build_options(launch)
    launch.add_argument("--host", help="Path to the REAPER executable")
    launch.add_argument("--build-only", action="store_true", help="Build and check without launching")
    launch.add_argument("--android-cell-sdk", type=Path, help="Root containing the two installed Android Cell SDKs")
    launch.add_argument("--gradle", help="Path to the Gradle executable")
    launch.add_argument("--adb", help="Path to the Android Debug Bridge executable")
    launch.add_argument("--serial", help="Select an Android device or emulator")
    args = parser.parse_args()
    try:
        if args.command != "new":
            if not re.fullmatch(r"[A-Za-z0-9_-]+", args.config):
                raise ValueError("Use a standard CMake configuration name")
            if args.jobs < 1:
                raise ValueError("--jobs must be positive")
        if args.command == "doctor":
            return doctor(args)
        if args.command == "run":
            return run_project(args)
        new_project(args)
    except (OSError, ValueError) as error:
        print(f"cell: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
