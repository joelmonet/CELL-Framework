# Original Cell Framework implementation; see LICENSE.
"""Check the generated Audio Unit bundle without registering it in Core Audio."""
import argparse
from hashlib import sha256
from pathlib import Path
import plistlib
import sys
from xml.parsers.expat import ExpatError


def read_bundle(contents, executables, bundle_id, package_type, version):
    with (contents / "Info.plist").open("rb") as stream:
        plist = plistlib.load(stream)
    if not isinstance(plist, dict):
        raise ValueError(f"{contents}/Info.plist must contain a dictionary")
    for key, expected in {"CFBundleIdentifier": bundle_id, "CFBundlePackageType": package_type,
                          "CFBundleShortVersionString": version, "CFBundleVersion": version}.items():
        if plist.get(key) != expected:
            raise ValueError(f"{key} differs from the generated project: expected {expected!r}")
    executable = plist.get("CFBundleExecutable")
    if not isinstance(executable, str) or not executable or Path(executable).name != executable or executable in (".", ".."):
        raise ValueError("Missing or invalid bundle executable name")
    with (executables / executable).open("rb") as stream:
        if stream.read(4) not in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
                                   b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca"):
            raise ValueError("The bundle executable is not a Mach-O binary")
    return plist


def check_component(components, identity, version_code, **properties):
    component_type, subtype, manufacturer = identity.read_text(encoding="ascii").strip().split("\t")
    if not isinstance(components, list) or len(components) != 1 or not isinstance(components[0], dict):
        raise ValueError("Expected one Audio Unit component in the generated bundle")
    expected_values = {"type": component_type, "subtype": subtype, "manufacturer": manufacturer,
                       "version": version_code, **properties}
    for key, expected in expected_values.items():
        value = components[0].get(key)
        if type(value) is not type(expected) or value != expected:
            raise ValueError(f"Audio Unit {key} differs from the generated project: expected {expected!r}")


def check_auv3(app, extension_name, identity, bundle_id, version, version_code, designer_html):
    macos = (app / "Contents").is_dir()
    contents = app / "Contents" if macos else app
    read_bundle(contents, contents / "MacOS" if macos else contents, bundle_id, "APPL", version)
    extension_id = bundle_id + ".auv3"
    extension = contents / "PlugIns" / extension_name
    contents = extension / "Contents" if macos else extension
    plist = read_bundle(contents, contents / "MacOS" if macos else contents, extension_id, "XPC!", version)
    registration = plist.get("NSExtension")
    if not isinstance(registration, dict) or not isinstance(registration.get("NSExtensionAttributes"), dict):
        raise ValueError("Missing AUv3 extension attributes")
    point = "com.apple.AudioUnit-UI" if designer_html else "com.apple.AudioUnit"
    class_prefix = "CellAUv3Editor_" if designer_html else "CellAUv3Factory_"
    principal_class = class_prefix + sha256(extension_id.encode("ascii")).hexdigest()[:16]
    if registration.get("NSExtensionPointIdentifier") != point or registration.get("NSExtensionPrincipalClass") != principal_class:
        raise ValueError("Unexpected AUv3 extension point or principal class")
    check_component(registration["NSExtensionAttributes"].get("AudioComponents"), identity, version_code, sandboxSafe=True)
    resource = (contents / "Resources" if macos else contents) / "CellEditor.html"
    if designer_html and resource.read_bytes() != designer_html.read_bytes():
        raise ValueError("Embedded AUv3 editor does not match the Designer export")
    if not designer_html and resource.exists():
        raise ValueError("An AUv3 build without the Designer editor contains an old editor resource")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bundle", type=Path)
    parser.add_argument("identity", type=Path)
    parser.add_argument("bundle_id")
    parser.add_argument("version")
    parser.add_argument("version_code", type=int)
    parser.add_argument("--extension", help="Name of the AUv3 extension embedded inside the app")
    parser.add_argument("--designer-html", type=Path)
    args = parser.parse_args()
    if args.designer_html and not args.extension:
        parser.error("--designer-html requires --extension")
    try:
        if args.extension:
            check_auv3(args.bundle, args.extension, args.identity, args.bundle_id, args.version, args.version_code, args.designer_html)
        else:
            contents = args.bundle / "Contents"
            plist = read_bundle(contents, contents / "MacOS", args.bundle_id, "BNDL", args.version)
            check_component(plist.get("AudioComponents"), args.identity, args.version_code, factoryFunction="CellAudioUnitFactory")
    except (OSError, ValueError, plistlib.InvalidFileException, ExpatError) as error:
        sys.exit(f"Audio Unit bundle check failed: {error}")
    print("Audio Unit bundle identity, version, and executable passed.")
