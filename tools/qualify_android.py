#!/usr/bin/env python3
"""Run Cell's Android runtime checks on an adb-connected target."""
import argparse
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import wave


def android_elf_info(path):
    with path.open("rb") as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:6] != b"\x7fELF\x02\x01":
            raise ValueError(f"Expected a 64-bit little-endian ELF file: {path}")
        machine = struct.unpack_from("<H", header, 18)[0]
        abi = {62: "x86_64", 183: "arm64-v8a"}.get(machine)
        if not abi:
            raise ValueError(f"Unsupported ELF machine {machine}: {path}")

        # Read PT_NOTE rather than section names so stripped binaries work.
        table = struct.unpack_from("<Q", header, 32)[0]
        entry_size, count = struct.unpack_from("<HH", header, 54)
        length = path.stat().st_size
        if entry_size < 56 or table + entry_size * count > length:
            raise ValueError(f"Invalid ELF program header table: {path}")
        note_segments = []
        load_alignments = []
        for index in range(count):
            stream.seek(table + entry_size * index)
            kind, _, offset, address, _, size, _, alignment = struct.unpack("<IIQQQQQQ", stream.read(56))
            if kind == 1:  # PT_LOAD
                if alignment < 1 or alignment & (alignment - 1) or (address - offset) % alignment:
                    raise ValueError(f"Invalid ELF load segment alignment: {path}")
                load_alignments.append(alignment)
            elif kind == 4:  # PT_NOTE
                end = offset + size
                if end > length:
                    raise ValueError(f"Truncated ELF note segment: {path}")
                note_segments.append((offset, end))
        if not load_alignments:
            raise ValueError(f"Missing ELF load segments: {path}")

        for offset, end in note_segments:
            while offset + 12 <= end:
                stream.seek(offset)
                name_size, data_size, note_type = struct.unpack("<III", stream.read(12))
                data = offset + 12 + ((name_size + 3) & ~3)
                next_note = data + ((data_size + 3) & ~3)
                if next_note > end:
                    raise ValueError(f"Truncated ELF note: {path}")
                if name_size == 8 and note_type == 1 and data_size >= 4 and stream.read(8) == b"Android\0":
                    stream.seek(data)
                    api = struct.unpack("<I", stream.read(4))[0]
                    if api:
                        return abi, api, min(load_alignments)
                offset = next_note
    raise ValueError(f"Missing Android API level note: {path}")


def stage_render_fixture(run, remote):
    graph = {
        "schema": "cell.graph.v2", "sampleRate": 48000,
        "nodes": [{"id": "swap", "type": "channelMap", "sources": [1, 0]}],
        "connections": [{"from": "input", "to": "swap"}, {"from": "swap", "to": "output"}],
        "automation": [],
    }
    # Asymmetric samples span several processing blocks and an uneven final block.
    frames = [(n - 700, 3000 - n * 2) for n in range(1301)]
    with tempfile.TemporaryDirectory(prefix="cell-android-render-") as directory:
        fixture = Path(directory)
        (fixture / "graph.json").write_text(json.dumps(graph), encoding="utf-8")
        for name, samples in (("source.wav", frames),
                              ("expected.wav", [(right, left) for left, right in frames])):
            with wave.open(str(fixture / name), "wb") as audio:
                audio.setparams((2, 2, 48000, 0, "NONE", "not compressed"))
                audio.writeframes(b"".join(struct.pack("<hh", *frame) for frame in samples))
        for name in ("graph.json", "source.wav", "expected.wav"):
            run("push", str(fixture / name), f"{remote}/{name}")


def qualify(args):
    build = args.build_dir.resolve()
    artifacts = (
        build / "cell_portable_graph_tests",
        build / "gain-example" / "cell_sdk_gain_smoke",
        build / "android-audio-example" / "cell_android_audio_probe",
        build / "cell_graph_demo",
        args.libcxx.resolve(),
    )
    if bool(args.jni_library) != bool(args.jni_dex):
        raise ValueError("--jni-library and --jni-dex must be supplied together")
    binary_artifacts = (*artifacts, args.jni_library.resolve()) if args.jni_library else artifacts
    if args.jni_dex and not args.jni_dex.is_file():
        raise ValueError(f"JNI smoke test DEX archive is unavailable: {args.jni_dex}")
    binary_info = {artifact: android_elf_info(artifact) for artifact in binary_artifacts}
    abi, minimum_api, _ = binary_info[artifacts[0]]
    for artifact, (artifact_abi, api, _) in binary_info.items():
        if artifact_abi != abi:
            raise ValueError(f"{artifact} does not match the {abi} build")
        minimum_api = max(minimum_api, api)

    adb = [args.adb]
    if args.serial:
        adb += ["-s", args.serial]

    def run(*command, check=True, capture=False):
        return subprocess.run(adb + list(command), check=check, timeout=args.timeout,
                              capture_output=capture, text=True)

    supported = run("shell", "getprop", "ro.product.cpu.abilist", capture=True).stdout.strip().split(",")
    if abi not in supported:
        raise ValueError(f"Target supports {', '.join(supported)}, but the build is {abi}")
    device_api = run("shell", "getprop", "ro.build.version.sdk", capture=True).stdout.strip()
    if not re.fullmatch(r"[0-9]+", device_api):
        raise ValueError(f"Could not determine the target Android API level: {device_api!r}")
    if int(device_api) < minimum_api:
        raise ValueError(f"Target Android API {device_api} is older than the build minimum API {minimum_api}")
    page_size = run("shell", "getconf", "PAGE_SIZE", capture=True).stdout.strip()
    if not re.fullmatch(r"[0-9]{1,9}", page_size):
        raise ValueError(f"Could not determine the target page size: {page_size!r}")
    page_bytes = int(page_size)
    if page_bytes < 4096 or page_bytes & (page_bytes - 1):
        raise ValueError(f"Unexpected target page size: {page_bytes}")
    for artifact, (_, _, alignment) in binary_info.items():
        if alignment < page_bytes:
            raise ValueError(
                f"{artifact.name} has {alignment}-byte ELF load alignment. "
                f"The target uses {page_bytes}-byte pages."
            )
    remote = run("shell", "mktemp", "-d", "/data/local/tmp/cell-qualify.XXXXXXXX", capture=True).stdout.strip()
    if not re.fullmatch(r"/data/local/tmp/cell-qualify\.[A-Za-z0-9]{8}", remote):
        raise ValueError(f"Unexpected adb temporary directory: {remote!r}")

    try:
        remote_names = [file.name for file in artifacts[:-1]] + ["libc++_shared.so"]
        for artifact, name in zip(artifacts, remote_names):
            run("push", str(artifact), f"{remote}/{name}")
        if args.jni_library:
            run("push", str(args.jni_library.resolve()), f"{remote}/libcell_android_graph.so")
            run("push", str(args.jni_dex.resolve()), f"{remote}/jni-smoke.zip")
            run("shell", "chmod", "400", f"{remote}/jni-smoke.zip")
        run("shell", "chmod", "700", *(f"{remote}/{name}" for name in remote_names[:-1]))
        failures = []
        for command in (remote_names[0], remote_names[1],
                        f"{remote_names[2]} aaudio", f"{remote_names[2]} aaudio-low-latency",
                        f"{remote_names[2]} opensl"):
            print(f"[{abi}] {command}", flush=True)
            result = run("shell", f"cd {remote} && LD_LIBRARY_PATH=. ./{command}", check=False)
            if result.returncode:
                failures.append(command)
        print(f"[{abi}] pack, render, and compare a portable graph project", flush=True)
        stage_render_fixture(run, remote)
        for command in (
            "pack graph.json source.wav smoke.cellgraph",
            "render-project smoke.cellgraph rendered.wav",
            "check-render smoke.cellgraph expected.wav",
        ):
            result = run("shell", f"cd {remote} && LD_LIBRARY_PATH=. ./cell_graph_demo {command}", check=False)
            if result.returncode:
                failures.append(command)
                break
        else:
            if args.jni_library:
                print(f"[{abi}] render and compare through the Java JNI bridge", flush=True)
                launch = (f"cd {remote} && CLASSPATH={remote}/jni-smoke.zip "
                          f"LD_LIBRARY_PATH={remote} /system/bin/app_process64 "
                          f"-Djava.library.path={remote} {remote} org.cell.examples.GraphRendererSmoke "
                          f"{remote}/smoke.cellgraph {remote}/jni.wav")
                result = run("shell", launch, check=False)
                if result.returncode:
                    failures.append("JNI graph renderer")
                else:
                    result = run("shell", f"cd {remote} && LD_LIBRARY_PATH=. "
                                 "./cell_graph_demo check-render smoke.cellgraph jni.wav", check=False)
                    if result.returncode:
                        failures.append("JNI graph render comparison")
        if failures:
            raise ValueError(f"Android probes failed: {', '.join(failures)}")
    finally:
        # Remove only the private directory created by this invocation.
        try:
            run("shell", "rm", "-rf", remote)
        except (OSError, subprocess.SubprocessError) as error:
            print(f"Could not clean Android test directory {remote}: {error}", file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--libcxx", required=True, type=Path, help="Matching NDK libc++_shared.so")
    parser.add_argument("--jni-library", type=Path, help="Matching Android libcell_android_graph.so")
    parser.add_argument("--jni-dex", type=Path, help="DEX ZIP containing GraphRenderer and GraphRendererSmoke")
    parser.add_argument("--adb", default="adb", help="Path to adb")
    parser.add_argument("--serial", help="adb device serial if multiple targets are connected")
    parser.add_argument("--timeout", type=int, default=60, help="Timeout in seconds for each adb command")
    args = parser.parse_args()
    if args.timeout < 1:
        parser.error("--timeout must be positive")
    try:
        qualify(args)
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Android qualification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
