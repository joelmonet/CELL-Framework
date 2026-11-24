"""Exercise the source or installed generator in an isolated directory."""
from pathlib import Path
import subprocess
import sys
import tempfile


def run(script, root, *arguments):
    return subprocess.run([sys.executable, str(script), "new", *arguments],
                          cwd=root, capture_output=True, text=True, encoding="utf-8")


def test(script):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        custom = "net.celltest.custom"
        output = root / "Folder With Spaces"
        created = run(script, root, "Custom", "--output", str(output),
                      "--id", custom, "--format", "clap", "--format", "vst3", "--ui", "--github-ci")
        assert created.returncode == 0, created.stderr
        config = (output / "CMakeLists.txt").read_text(encoding="utf-8")
        processor = (output / "processor.cpp").read_text(encoding="utf-8")
        assert config.startswith("cmake_minimum_required")
        assert "{{" not in config and "project(Custom VERSION" in config
        assert config.count(f'set(project_plugin_id "{custom}")') == 1 and custom in processor
        assert config.count('ID "${project_plugin_id}"') == 3
        assert (output / "plugin-id.txt").read_text(encoding="ascii") == custom + "\n"
        for option in ("CLAP", "VST3", "LV2", "AU", "AUV3", "UI"):
            assert f"option(CELL_PROJECT_{option} " in config
        assert config.count('" ON)') == 3
        workflow = (output / ".github" / "workflows" / "cell.yml").read_text(encoding="utf-8")
        assert all(runner in workflow for runner in ("ubuntu-latest", "macos-latest", "windows-latest"))
        assert "vendor/Cell-Framework/framework" in workflow
        assert "-DCELL_PROJECT_AUV3=OFF" in workflow
        assert (output / "parameter-ids.tsv").read_text(encoding="ascii").startswith("gain\t")
        assert (output / "parameter_identity.cpp").is_file()
        assert (output / "lv2-uri.txt").read_text(encoding="ascii") == f"https://example.com/lv2/{custom}\n"
        assert (output / "legacy_effect_host.cpp").is_file()
        assert (output / "LICENSE").is_file()
        assert (output / "allocation_watch.hpp").is_file()
        assert run(script, root, "Custom", "--output", str(output)).returncode != 0
        assert (output / "CMakeLists.txt").read_text(encoding="utf-8") == config

        ids = []
        for name in ("First", "Second"):
            generated = run(script, root, name)
            assert generated.returncode == 0, generated.stderr
            assert not (root / name / ".github").exists()
            ids.append(generated.stdout.split("(")[-1].split(")")[0])
        assert ids[0] != ids[1] and all(item.startswith("org.cell.generated.") for item in ids)
        assert run(script, root, "NUL").returncode != 0
        assert run(script, root, "Invalid", "--id", "bad").returncode != 0
        assert not (root / "Invalid").exists()

        exports = root / "Designer Exports"
        exports.mkdir()
        html = exports / "cell-plugin-editor.html"
        bindings = exports / "cell-ui-bindings.hpp"
        html.write_bytes("<!doctype html>\r\n<title>Éditeur</title>\r\n".encode("utf-8"))
        options = ("--designer-dir", str(exports), "--format", "clap", "--format", "vst3")
        failed = run(script, root, "MissingBindings", *options)
        assert failed.returncode != 0 and bindings.name in failed.stderr
        assert not (root / "MissingBindings").exists()

        bindings.write_bytes(b"\xff")
        failed = run(script, root, "InvalidBindings", *options)
        assert failed.returncode != 0 and "UTF-8" in failed.stderr
        assert not (root / "InvalidBindings").exists()

        bindings.write_bytes(b"// Designer header\r\n")
        conflict = run(script, root, "ConflictingEditors", *options, "--ui")
        assert conflict.returncode != 0 and "either --ui or --designer-dir" in conflict.stderr
        assert not (root / "ConflictingEditors").exists()

        generated = run(script, root, "Designed", *options)
        assert generated.returncode == 0, generated.stderr
        designer_project = root / "Designed"
        for asset in (html, bindings):
            assert (designer_project / "editor" / asset.name).read_bytes() == asset.read_bytes()
        config = (designer_project / "CMakeLists.txt").read_text(encoding="utf-8")
        assert 'option(CELL_PROJECT_DESIGNER "Build the Cell UI Designer plug-in editor" ON)' in config
        assert "CLAP_EDITOR_BINDINGS" in config and "VST3_EDITOR_BINDINGS" in config

        lv2_uri = "https://plugins.example.org/audio/pi-gain"
        lv2 = run(script, root, "PiGain", "--format", "lv2", "--lv2-uri", lv2_uri,
                  "--designer-dir", str(exports))
        assert lv2.returncode == 0, lv2.stderr
        project = root / "PiGain"
        config = (project / "CMakeLists.txt").read_text(encoding="utf-8")
        assert 'option(CELL_PROJECT_LV2 "Build the LV2 plug-in" ON)' in config
        assert f'set(project_lv2_uri "{lv2_uri}")' in config
        assert (project / "lv2-uri.txt").read_text(encoding="ascii") == lv2_uri + "\n"
        description = (project / "plugin.lv2.ttl.in").read_text(encoding="utf-8")
        assert "<@project_lv2_uri@>" in description and 'doap:name "PiGain"' in description
        invalid = run(script, root, "InvalidLv2Uri", "--format", "lv2", "--lv2-uri", "https:///without-a-host")
        assert invalid.returncode != 0 and "LV2 URI" in invalid.stderr
        assert not (root / "InvalidLv2Uri").exists()

        au_codes = ("--au-subtype", "StGn", "--au-manufacturer", "Acme")
        au = run(script, root, "StudioAU", "--format", "au", *au_codes, "--designer-dir", str(exports))
        assert au.returncode == 0, au.stderr
        project = root / "StudioAU"
        config = (project / "CMakeLists.txt").read_text(encoding="utf-8")
        assert 'option(CELL_PROJECT_AU "Build the macOS AUv2 plug-in" ON)' in config
        assert 'set(project_au_subtype "StGn")' in config and 'set(project_au_manufacturer "Acme")' in config
        assert (project / "au-identity.txt").read_text(encoding="ascii") == "aufx\tStGn\tAcme\n"
        for name in ("check_au_bundle.py", "au_gain_host.cpp", "au_cocoa_editor.mm"):
            assert (project / name).is_file()
            assert not (output / name).exists()
        failed = run(script, root, "MissingAuCodes", "--format", "au", "--au-subtype", "StGn")
        assert failed.returncode != 0 and "--au-manufacturer" in failed.stderr
        assert not (root / "MissingAuCodes").exists()
        failed = run(script, root, "InvalidAuCode", "--format", "au", "--au-subtype", "StG!", "--au-manufacturer", "Acme")
        assert failed.returncode != 0 and "four ASCII" in failed.stderr
        assert not (root / "InvalidAuCode").exists()
        failed = run(script, root, "InvalidAuBundleId", "--id", "org.example.studio_au", "--format", "au", *au_codes)
        assert failed.returncode != 0 and "bundle ID" in failed.stderr
        assert not (root / "InvalidAuBundleId").exists()

        auv3 = run(script, root, "MobileGain", "--format", "auv3", *au_codes, "--designer-dir", str(exports))
        assert auv3.returncode == 0, auv3.stderr
        project = root / "MobileGain"
        config = (project / "CMakeLists.txt").read_text(encoding="utf-8")
        assert 'option(CELL_PROJECT_AUV3 "Build the AUv3 app extension and containing app" ON)' in config
        assert 'option(CELL_PROJECT_AU "Build the macOS AUv2 plug-in" OFF)' in config
        assert 'ID "${project_plugin_id}.auv3"' in config
        assert (project / "au-identity.txt").read_text(encoding="ascii") == "aufx\tStGn\tAcme\n"
        for name in ("auv3_app.mm", "auv3-app.plist.in", "auv3-app.entitlements", "check_au_bundle.py"):
            assert (project / name).is_file()
            assert not (output / name).exists()
        failed = run(script, root, "MissingAuv3Codes", "--format", "auv3")
        assert failed.returncode != 0 and "--au-manufacturer" in failed.stderr
        assert not (root / "MissingAuv3Codes").exists()

        android_id = "com.example.mobile_gain"
        android = run(script, root, "PocketGain", "--format", "android", "--format", "lv2",
                      "--id", android_id, "--designer-dir", str(exports))
        assert android.returncode == 0, android.stderr
        project = root / "PocketGain"
        config = (project / "CMakeLists.txt").read_text(encoding="utf-8")
        assert "if(ANDROID AND ON)" in config
        assert 'option(CELL_PROJECT_LV2 "Build the LV2 plug-in" ON)' in config
        assert (project / "plugin-id.txt").read_text(encoding="ascii").strip() == android_id
        app = project / "android"
        app_config = (app / "cell-app.properties").read_text(encoding="ascii")
        assert "identityFile=../plugin-id.txt\n" in app_config
        assert "nativeProject=../CMakeLists.txt\n" in app_config
        assert "name=PocketGain\n" in app_config
        assert 'rootProject.name = "PocketGain"' in (app / "settings.gradle").read_text(encoding="utf-8")
        assert (app / "src/main/java/org/cell/examples/designer/MainActivity.java").is_file()
        assert (app / "src/main/AndroidManifest.xml").is_file()
        for asset in (html, bindings):
            assert (project / "editor" / asset.name).read_bytes() == asset.read_bytes()
        default = run(script, root, "DefaultAndroid", "--format", "android")
        assert default.returncode == 0, default.stderr
        for asset in (html, bindings):
            assert (root / "DefaultAndroid" / "editor" / asset.name).stat().st_size > 0
        failed = run(script, root, "InvalidAndroidId", "--format", "android", "--id", "com.example.1plug-in")
        assert failed.returncode != 0 and "Android app ID" in failed.stderr
        assert not (root / "InvalidAndroidId").exists() and not (output / "android").exists()


if __name__ == "__main__":
    script = (Path(sys.argv[1]) if len(sys.argv) > 1 else
              Path(__file__).resolve().parents[1] / "tools" / "cell.py")
    test(script.resolve())
