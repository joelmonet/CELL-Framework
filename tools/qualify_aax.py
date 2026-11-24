#!/usr/bin/env python3
"""Build and verify Cell's reference AAX adapter."""
import argparse
from pathlib import Path
import platform
import plistlib
import struct
import subprocess
import sys
from xml.parsers.expat import ExpatError


def verify_bundle(binary, system):
    if not binary.is_file() or binary.stat().st_size == 0:
        raise ValueError(f"Missing AAX executable: {binary}")
    contents = binary.parent.parent
    bundle = contents.parent
    if contents.name != "Contents" or bundle.suffix != ".aaxplugin":
        raise ValueError(f"Unexpected AAX bundle layout: {binary}")
    if system == "Darwin":
        if binary.parent.name != "MacOS":
            raise ValueError(f"Expected an AAX executable under Contents/MacOS: {binary}")
        with (contents / "Info.plist").open("rb") as stream:
            metadata = plistlib.load(stream)
        expected = {
            "CFBundleExecutable": binary.name,
            "CFBundleIdentifier": "com.example.cell.gain",
            "CFBundlePackageType": "TDMw",
            "CFBundleSignature": "PTul",
        }
        if not isinstance(metadata, dict):
            raise ValueError("Expected an AAX Info.plist dictionary")
        for key, value in expected.items():
            if metadata.get(key) != value:
                raise ValueError(f"Unexpected AAX Info.plist value for {key}: {metadata.get(key)!r}")
    else:
        if binary.parent.name != "x64" or binary.suffix != ".aaxplugin":
            raise ValueError(f"Expected a Windows AAX module under Contents/x64: {binary}")
        with binary.open("rb") as stream:
            header = stream.read(64)
            if len(header) != 64 or not header.startswith(b"MZ"):
                raise ValueError(f"Invalid Windows AAX PE header: {binary}")
            stream.seek(struct.unpack_from("<I", header, 60)[0])
            if stream.read(6) != b"PE\x00\x00\x64\x86":
                raise ValueError(f"Windows AAX executable is not x64: {binary}")
    return bundle


def qualify(args):
    system = platform.system()
    if args.link_probe:
        if system != "Linux":
            raise ValueError("--link-probe requires Linux")
        installed_sdk = args.link_probe.resolve()
        if not installed_sdk.is_dir():
            raise ValueError(f"Installed Cell SDK prefix does not exist: {installed_sdk}")
    elif system not in ("Windows", "Darwin"):
        raise ValueError("Native AAX qualification requires Windows or macOS. On Linux, use --link-probe with an installed Cell SDK.")

    aax_sdk = args.aax_sdk.resolve()
    if not (aax_sdk / "Interfaces" / "AAX_Exports.cpp").is_file():
        raise ValueError(f"Expected Interfaces/AAX_Exports.cpp under {aax_sdk}")

    root = Path(__file__).resolve().parents[1]
    build = str(args.build_dir.resolve())
    configure = ["cmake", "-B", build, "-DCMAKE_BUILD_TYPE=Release"]
    if args.link_probe:
        configure += ["-S", str(root / "tests" / "sdk_aax_link_consumer"),
                      f"-DCMAKE_PREFIX_PATH={installed_sdk}", f"-DCELL_TEST_AAX_SDK={aax_sdk}"]
        targets = ["cell_aax_test_module", "cell_aax_link_host"]
        tests = ("cell_aax_link",)
    else:
        configure += ["-S", str(root / "framework"), "-DCELL_SDK_EXAMPLES=ON",
                      "-DCELL_SDK_TESTS=ON", f"-DCELL_GAIN_AAX_SDK={aax_sdk}"]
        targets = ["CellGain_AAX", "cell_aax_link_host"]
        tests = ("cell_aax_exports", "cell_aax_runtime")

    subprocess.run(configure + args.cmake_arg, check=True)
    subprocess.run(["cmake", "--build", build, "--config", "Release", "--target", *targets], check=True)
    for test in tests:
        subprocess.run(["ctest", "--test-dir", build, "-C", "Release", "-R", f"^{test}$",
                        "--no-tests=error", "--output-on-failure"], check=True)
    if args.link_probe:
        print("Verified Linux AAX link, exports, and simulated registration.")
    else:
        module_file = Path(build) / "aax-module-Release.txt"
        binary = Path(module_file.read_text(encoding="utf-8").rstrip("\r\n"))
        bundle = verify_bundle(binary, system)
        print(f"Verified unsigned AAX bundle, exports, and simulated runtime: {bundle}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aax-sdk", required=True, type=Path, help="Official Avid AAX SDK root")
    parser.add_argument("--build-dir", required=True, type=Path, help="CMake build directory")
    parser.add_argument("--link-probe", type=Path, metavar="CELL_PREFIX",
                        help="Run the Linux link and registration probe with this installed Cell SDK")
    parser.add_argument("--cmake-arg", action="append", default=[],
                        help="Additional configure argument, for example --cmake-arg=-DCMAKE_OSX_ARCHITECTURES=arm64")
    try:
        qualify(parser.parse_args())
    except (ValueError, OSError, plistlib.InvalidFileException, ExpatError, subprocess.CalledProcessError) as error:
        print(f"AAX qualification failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
