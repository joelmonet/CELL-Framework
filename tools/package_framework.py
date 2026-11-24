#!/usr/bin/env python3
"""Create an independent framework source package without product DSP or builds."""
import argparse
import fnmatch
import hashlib
import json
import os
import re
from pathlib import Path
import tempfile
import zipfile

FILES = {
    "README.md",
    ".github/workflows/apple-sdk.yml",
    ".github/workflows/windows-sdk.yml",
    ".github/workflows/raspberry-pi-sdk.yml",
    "examples/portable_graph/tempo-modulation.json",
    "examples/ui_designer/index.html", "examples/ui_designer/designer.js",
    "examples/ui_designer/model.js", "examples/ui_designer/runtime.js",
    "examples/ui_designer/fonts.js", "tools/build_designer_fonts.py",
    "tests/ui_designer_model_tests.cjs", "tests/ui_designer_browser_tests.cjs",
    "examples/graph_editor/index.html", "examples/graph_editor/editor.js", "examples/graph_editor/serve.py",
    "examples/portable_graph/modulation.json", "examples/portable_graph/azimuth-modulation.json",
    "examples/sdk_gain/standalone.cpp", "framework/RASPBERRY-PI.md",
    "tests/convolution_tests.cpp",
    "framework/ANALYSIS-LEASE.md", "tests/analysis_lease_tests.cpp", "framework/RESONANCE-MASK.md", "framework/SYNTHESIS.md", "tests/resonance_mask_tests.cpp", "tests/physical_percussion_tests.cpp", "tests/wavetable_synthesis_tests.cpp",
    "src/graph_project.cpp", "tests/graph_project_tests.cpp",
    "examples/portable_graph/parallel.json", "examples/portable_graph/compensated.json", "examples/portable_graph/intent.json", "examples/portable_graph/saturation.json", "examples/portable_graph/surround.json", "examples/portable_graph/position.json",
    "examples/portable_graph/surround-position.json", "examples/portable_graph/tremolo.json",
    "tests/sdk_aax_link_consumer/CMakeLists.txt", "tests/sdk_aax_link_consumer/host.cpp",
    "tests/sdk_aax_link_consumer/descriptor.cpp",
    "tests/test_cell_doctor.py",
    "tests/test_project_generator.py",
    "tools/cell.py", "tools/cell.cmd", "tools/project/CMakeLists.txt.in",
    "tools/qualify_reaper.py", "tools/qualify_reaper.lua",
    "tools/qualify_ardour_lv2.py", "tools/qualify_ardour_lv2.lua",
    "tools/qualify_ardour_lv2_stimulus.lua",
    "tools/project/parameter_identity.cpp", "tools/project/parameter-ids.tsv",
    "tools/project/check_au_bundle.py",
    "tools/project/github-ci.yml", "tools/project/render.cpp", "tools/project/README.md", "framework/PROJECT-GENERATOR.md",
    "examples/android_graph/CMakeLists.txt", "examples/android_graph/bridge.cpp",
    "examples/android_graph/java/org/cell/examples/GraphRenderer.java",
    "tests/android_graph/GraphRendererSmoke.java",
    "examples/android_designer/CMakeLists.txt", "src/formats/android_designer.cpp",
    "examples/android_designer/build.gradle", "examples/android_designer/settings.gradle",
    "examples/android_designer/cell-app.properties",
    "examples/android_designer/src/main/AndroidManifest.xml",
    "examples/android_designer/src/main/java/org/cell/examples/designer/MainActivity.java",
    "examples/android_audio/main.cpp", "examples/android_audio/CMakeLists.txt", "framework/ANDROID.md", "tools/qualify_android.py", "framework/IOS.md",
    "tests/network_audio_tests.cpp", "framework/NETWORK-AUDIO.md", "examples/network_audio/loopback.cpp",
    "examples/network_audio/udp_socket.hpp", "examples/network_audio/wave_stream.cpp",
    "examples/network_audio/latency_measure.hpp", "tests/test_network_audio_wave.py",
    "tests/test_network_audio_devices.py",
    "framework/LEGACY-EFFECTS.md",
    "tests/legacy_effect_host.cpp",
    "tests/ara_archive_tests.cpp",
    "src/formats/audio_effect.hpp", "src/formats/ladspa.cpp",
    "src/formats/lv2.cpp", "src/formats/lv2_ui.cpp", "src/formats/lv2_programs.hpp", "examples/sdk_gain/gain.lv2.ttl", "examples/sdk_gain/Cell Gain.jsfx",
    "framework/AAX.md", "tools/qualify_aax.py",
    "framework/AU.md", "framework/cmake/CellAUInfo.plist.in", "src/formats/au.cpp", "tests/au_gain_host.cpp",
    "src/formats/au_editor.hpp", "src/formats/au_editor.mm", "tests/au_cocoa_editor.mm",
    "framework/cmake/CellAUv3Info.plist.in", "framework/cmake/CellAUv3MacSandbox.entitlements",
    "src/formats/auv3.mm", "src/formats/auv3_editor.mm", "src/formats/auv3_automation.hpp", "src/formats/auv3_layouts.hpp",
    "examples/auv3_gain/CMakeLists.txt", "examples/auv3_gain/main.mm",
    "examples/auv3_gain/Cell Gain Editor.html", "examples/auv3_gain/cell-ui-bindings.hpp",
    "examples/auv3_gain/Info.plist.in", "examples/auv3_gain/midi_fixture.hpp",
    "examples/auv3_gain/midi_processor.cpp", "examples/auv3_gain/midi_check.mm",
    "examples/auv3_gain/instrument_fixture.hpp", "examples/auv3_gain/instrument_processor.cpp",
    "examples/auv3_gain/instrument_check.mm",
    "src/portable_graph.cpp", "examples/portable_graph/processor.hpp",
    "examples/portable_graph/main.cpp", "examples/portable_graph/example.json", "examples/portable_graph/reordered.json",
    "tests/portable_graph_tests.cpp", "framework/PORTABLE-GRAPH.md",
    "framework/VALIDATION.md", "framework/FEATURES.md", "framework/IMPLEMENTATIONS.md", "framework/FRAMEWORK-COMPARISON.md", "framework/WEBVIEW.md",
    "framework/cmake/CellWebView.cmake", "src/webview/internal.hpp", "src/webview/webview.cpp", "src/webview/linux.cpp", "src/webview/windows.cpp", "src/webview/macos.mm", "src/webview/compat/EventToken.h",
    "framework/cmake/CellPluginBundleInfo.plist.in",
    "tests/vst3_cocoa_editor.mm",
    "tests/test_web_gesture_replay.py", "tests/web_parameter_bridge_tests.cpp", "tests/webview_contract_tests.cpp", "tests/clap_cocoa_editor.mm", "examples/webview_editor/CMakeLists.txt", "examples/webview_editor/bridge.cpp", "examples/webview_editor/main.cpp", "examples/webview_editor/page.hpp",
    "src/graphics_effects.cpp", "src/ui/effects.cpp",
    "framework/cmake/CellGraphics.cmake", "framework/GRAPHICS-EFFECTS.md",
    "tests/graphics_effects_tests.cpp", "tests/ui_effects_tests.cpp",
    "examples/effects_gallery/CMakeLists.txt", "examples/effects_gallery/gallery.cpp", "examples/effects_gallery/benchmark.cpp",
    "framework/CPP-STANDARDS.md",
    "tests/scroll_viewport_tests.cpp", "tests/filter_slopes_tests.cpp", "tests/popup_menu_layout_tests.cpp", "framework/FILTER-SLOPES.md", "framework/TILT-DYNAMICS.md", "tests/tilt_dynamics_tests.cpp",
    'src/pitch_detection.cpp', 'tests/pitch_detection_tests.cpp', 'framework/PITCH-TRACKING.md',
    'framework/MIDI-OUTPUT.md', 'src/memory.cpp', 'tests/midi_delivery_tests.cpp',
    'framework/LIBRARY-BROWSING.md',
    'framework/AUTOMATION.md', 'tests/parameter_hooks_tests.cpp',
    'framework/cmake/CellWindowsVersion.cmake', 'framework/cmake/CellWindowsVersion.rc.in',
    'tests/file_dialog_tests.cpp', 'src/ui/file_dialog.cpp', 'src/ui/file_dialog_macos.mm',
    'tests/fxp_tests.cpp', 'tests/latest_prepared_exchange_tests.cpp', 'tests/windows_surface_presentation_tests.cpp',
    'framework/PREPARED-STATE-AND-RASTER.md',
    'src/detail/integer_decimal.hpp', 'tests/integer_decimal_tests.cpp',
    'framework/TRANSPORT.md', 'framework/HOST-PROJECTS.md', 'tests/sdk_transport_consumer/CMakeLists.txt',
    'tests/sdk_transport_consumer/processor.cpp', 'tests/sdk_transport_consumer/host.cpp',
    'tests/sdk_transport_consumer/auv3_host.mm', 'tests/sdk_transport_consumer/auv3_midi_fx_host.mm',
    'tests/sdk_transport_consumer/auv3_instrument_host.mm',
    'tests/auv3_automation_tests.cpp',
    'src/pooled_file.cpp', 'tests/pooled_file_tests.cpp', 'framework/POOLED-READS.md',
    'tests/sdk_pooled_file_consumer/CMakeLists.txt',
    'src/trg.cpp', 'tests/trg_tests.cpp', 'framework/TRG-FORMAT.md',
    'tests/sdk_trg_consumer/CMakeLists.txt',
    'tests/wave_export_tests.cpp',
    'src/tci.cpp', 'tests/tci_format_tests.cpp', 'tests/fixtures/tci_fixture.hpp',
    'framework/TCI-FORMAT.md',
    'src/midi_message.cpp', 'src/smf.cpp', 'tests/midi_message_tests.cpp',
    'tests/smf_tests.cpp', 'framework/MIDI-FILES.md',
    'framework/GOG-FORMAT.md',
    'src/gog.cpp', 'tests/gog_format_tests.cpp', 'tests/fixtures/gog_fixture.hpp',
    'framework/cmake/CellNativeServices.cmake',
    'src/midi_devices.cpp', 'tests/midi_devices_tests.cpp',
    'tests/midi_note_state_tests.cpp',
    'src/ksp.cpp', 'tests/ksp_tests.cpp',
    'framework/NATIVE-HANDOFF.md',
    'tests/run_owned_window_manager.py',
    'tests/module_jobs_tests.cpp',
    'tests/sdk_native_consumer/CMakeLists.txt', 'tests/sdk_native_consumer/main.cpp',
    'src/process.cpp',
    'src/ui/platform_transfer.hpp',
    'src/ui/platform_transfer_linux.cpp',
    'src/ui/platform_transfer_windows.cpp',
    'src/ui/x11_checked.hpp',
    'tests/native_control_contract_tests.cpp',
    'tests/native_windows_window_tests.cpp',
    'tests/native_x11_window_tests.cpp',
    'tests/native_device_tests.cpp',
    'tests/run_owned_pulse_device.py',
    'tests/sparse_fixture.hpp',
    'src/audio_file.cpp', 'src/wave_file.cpp', 'src/audio_file_codecs.cpp', 'src/audio_file_internal.hpp',
    'tests/wave_file_tests.cpp', 'tests/audio_codec_tests.cpp',
    'tests/sdk_audio_files_consumer/CMakeLists.txt', 'tests/sdk_audio_files_consumer/main.cpp',
    'tests/sdk_ara_source_consumer/CMakeLists.txt',
    'third_party/sndfile/sndfile.h', 'third_party/sndfile/COPYING', 'third_party/sndfile/PROVENANCE.json',
    'framework/AUDIO-FILES.md',
    'framework/ARA-SOURCES.md',
    'src/midi_message.cpp', 'src/smf.cpp', 'tests/midi_message_tests.cpp', 'tests/smf_tests.cpp', 'framework/MIDI-FILES.md',
    'src/text.cpp', 'src/unicode_case_data.hpp', 'src/localization.cpp',
    'src/uuid.cpp', 'src/file.cpp', 'src/file_streams.cpp',
    'src/action_history.cpp', 'src/dispatch.cpp', 'src/xml.cpp', 'src/state_tree.cpp',
    'src/ara/source_reader.cpp', 'src/ui/timeline.cpp',
    'src/ui/widget_component.cpp', 'src/ui/widget_controls.cpp',
    'src/ui/widget_drawing.cpp', 'src/ui/widget_input.cpp',
    'src/ui/widget_navigation.cpp', 'src/ui/widget_services.cpp',
    'tests/text_tests.cpp', 'tests/localization_tests.cpp', 'tests/uuid_tests.cpp',
    'tests/native_file_tests.cpp', 'tests/action_history_tests.cpp', 'tests/dispatch_tests.cpp',
    'tests/xml_tests.cpp', 'tests/state_tree_tests.cpp', 'tests/ara_source_reader_tests.cpp',
    'tests/owned_audio_tests.cpp', 'tests/real_fft_tests.cpp', 'tests/utility_tests.cpp',
    'tests/font_roles_tests.cpp', 'tests/timeline_tests.cpp',
    'tests/widget_controls_tests.cpp', 'tests/widget_drawing_tests.cpp',
    'tests/widget_navigation_tests.cpp', 'tests/widget_window_tests.cpp',
    'tests/x11_visual_tests.cpp', 'tools/generate_unicode_case.py',
    'framework/FONTS-AND-LOCALIZATION.md', 'framework/FILES-AND-STREAMS.md',
    'framework/PROJECT-STATE.md', 'licenses/UNICODE-LICENSE.txt',
    'assets/CellBrandGrotesk-SemiBold.ttf', 'assets/Rajdhani-SemiBold.ttf',
    'assets/SpaceGrotesk-OFL.txt', 'assets/Rajdhani-OFL.txt', 'assets/BRAND-FONTS.json',
    'src/midi_tools.cpp', 'tests/midi_tools_tests.cpp', 'tests/midi_control_tests.cpp',
    'tests/latest_exchange_tests.cpp',
    'tests/callback_gate_tests.cpp',
    'src/read_ahead.cpp', 'tests/read_ahead_tests.cpp',
    'framework/cmake/CellCompilerContract.cmake', 'tests/compiler_contract_tests.cpp',
    'src/audio_device.cpp', 'cmake/CellAudioDevice.cmake', 'licenses/miniaudio.txt',
    'framework/CMakeLists.txt', 'framework/README.md', 'framework/THIRD_PARTY_NOTICES.md', 'framework/wrappers.json', 'framework/INTERACTIONS.md',
    'framework/cmake/CellAddPlugin.cmake', 'framework/cmake/CellConfig.cmake.in', 'framework/cmake/CellAAXInfo.plist.in',
    'examples/control_gallery/CMakeLists.txt', 'examples/control_gallery/gallery.cpp', 'examples/sdk_gain/CMakeLists.txt', 'examples/sdk_gain/gain.hpp',
    'examples/sdk_gain/processor.cpp', 'examples/sdk_gain/main.cpp', 'examples/sdk_gain/editor.cpp',
    'framework/UI-STANDARDS.md', 'framework/SERVICES.md',
    'LICENSE', 'NOTICE', 'src/wavetable.cpp', 'src/plugin.cpp', 'src/files.cpp', 'src/json.cpp', 'src/jobs.cpp', 'src/history.cpp', 'src/state_transfer.cpp', 'src/assets.cpp', 'src/thumbnails.cpp', 'src/legacy_state.cpp', 'src/state_migration.cpp',
    'src/ui/canvas.cpp', 'src/ui/instrument_controller.cpp', 'src/ui/window_linux.cpp', 'src/ui/window_windows.cpp', 'src/ui/window_macos.mm',
    'tests/instrument_controller_tests.cpp', 'tests/ui_scaling_tests.cpp',
    'tests/test_vst3_external_consumer.py',
    'src/formats/clap.cpp', 'src/formats/vst3.cpp', 'src/formats/webview2_plugin.hpp', 'src/formats/cocoa_web_editor.hpp', 'src/formats/reaper_project.hpp', 'src/formats/vst2.cpp', 'src/formats/aax.cpp', 'src/wrappers/runtime.cpp',
    'tools/embed_resource.py', 'tools/package_framework.py', 'tools/audit_wrapper_sources.py', 'tools/audit_interaction_sources.py',
    'tests/service_tests.cpp', 'tests/json_tests.cpp', 'tests/ui_components_tests.cpp', 'tests/product_query_tests.cpp', 'tests/wrapper_runtime_tests.cpp', 'tests/parameter_timeline_tests.cpp',
    'tests/allocation_watch.hpp', 'tests/vst2_host.cpp', 'tests/test_framework_package.py', 'tests/interaction_tests.cpp',
    'tests/prepared_exchange_transition_tests.cpp', 'tests/sample_clock_tests.cpp',
    'tests/sdk_audio_consumer/CMakeLists.txt', 'tests/sdk_audio_consumer/main.cpp',
    'assets/Inter.ttf', 'assets/Inter-Bold.ttf', 'assets/DejaVuSansMono.ttf',
    'assets/OFL.txt', 'assets/DejaVu-LICENSE.txt', 'licenses/Blend2D.txt', 'licenses/CLAP.txt', 'licenses/VST3.txt',
}

def check_cmake_inputs(root, names):
    """Require literal SDK-root and list-directory inputs in the source archive."""
    for script in sorted(names):
        if not script.startswith('framework/') or not script.endswith(('CMakeLists.txt', '.cmake')):
            continue
        directory = Path(script).parent
        contents = (root / script).read_text(encoding='utf-8')
        for prefix, relative in re.findall(
                r'\$\{(CELL_FRAMEWORK_SOURCE_ROOT|CMAKE_CURRENT_LIST_DIR)\}/([^";\s>)]+)',
                contents):
            if '$' in relative:
                continue
            base = directory if prefix == 'CMAKE_CURRENT_LIST_DIR' else Path('.')
            path = Path(os.path.normpath(base / relative)).as_posix()
            if any(mark in path for mark in '*?['):
                included = any(fnmatch.fnmatchcase(name, path) for name in names)
            else:
                included = path == "." or path in names or any(name.startswith(path + '/') for name in names)
            if not included:
                raise ValueError(f'{script} references unpackaged CMake input: {path}')


def source_files(root):
    root = Path(root).resolve(strict=True)
    names = set(FILES)
    names.update(path.relative_to(root).as_posix() for path in (root/'include/cell').rglob('*.hpp') if path.name != 'studio_dsp.hpp')
    for name in names:
        path = root/name
        if not path.is_file() or path.is_symlink() or any(parent.is_symlink() for parent in path.parents if parent != root.parent):
            raise ValueError('missing or linked framework source: '+name)
    if not any(name.startswith('framework/cmake/') for name in names):
        raise ValueError('framework package metadata is missing')
    check_cmake_inputs(root, names)
    return sorted(names)

def package(source, output):
    root, output = Path(source).resolve(strict=True), Path(output).absolute()
    if output.suffix.lower() != '.zip' or output.resolve().is_relative_to(root):
        raise ValueError('choose a ZIP destination outside the source tree')
    if output.exists():
        raise ValueError('output exists; choose a new archive name')
    names = source_files(root)
    manifest = dict(schema='cell.framework-source.v1',compiled=False,
        license='Apache-2.0 (original framework); optional third-party notices retained',
        files=[dict(path=name,bytes=(root/name).stat().st_size,sha256=hashlib.sha256((root/name).read_bytes()).hexdigest()) for name in names])
    output.parent.mkdir(parents=True,exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix='.cell-framework-',suffix='.zip',dir=output.parent)
    os.close(fd)
    try:
        with zipfile.ZipFile(temporary,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
            for name in names:
                archive.write(root/name,'Cell-Framework/'+name)
            archive.writestr('Cell-Framework/SOURCE-MANIFEST.json',json.dumps(manifest,indent=2)+'\n')
        with zipfile.ZipFile(temporary) as archive:
            if archive.testzip() is not None:
                raise ValueError('framework archive CRC verification failed')
            for record in manifest['files']:
                if hashlib.sha256(archive.read('Cell-Framework/'+record['path'])).hexdigest() != record['sha256']:
                    raise ValueError('framework archive content changed')
        os.link(temporary,output)
    finally:
        Path(temporary).unlink(missing_ok=True)
    return manifest

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source',type=Path)
    parser.add_argument('output',type=Path)
    args=parser.parse_args()
    result=package(args.source,args.output)
    print(f'Framework source archive verified: {len(result["files"])} files; no compilation performed.')
