"""An SDK archive must be independent of the Sound Cell lab and its DSP deps."""
import hashlib
import json
from pathlib import Path
import sys
import shutil
import tempfile
import zipfile

sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from package_framework import package, source_files

root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as temporary:
    output=Path(temporary)/'framework.zip'
    manifest=package(root,output)
    with zipfile.ZipFile(output) as archive:
        names=archive.namelist()
        assert 'Cell-Framework/framework/CMakeLists.txt' in names
        for name in ('framework/README.md', 'framework/FEATURES.md',
                     'framework/IMPLEMENTATIONS.md', 'framework/FRAMEWORK-COMPARISON.md',
                     'framework/WEBVIEW.md', 'framework/cmake/CellWebView.cmake',
                     'src/webview/webview.cpp', 'src/webview/internal.hpp',
                     'src/webview/linux.cpp', 'src/webview/windows.cpp', 'src/webview/macos.mm',
                     'src/webview/compat/EventToken.h', 'tests/web_parameter_bridge_tests.cpp',
                     'tests/webview_contract_tests.cpp', 'examples/webview_editor/CMakeLists.txt',
                     'examples/webview_editor/bridge.cpp', 'examples/webview_editor/main.cpp',
                     'examples/webview_editor/page.hpp'):
            assert 'Cell-Framework/'+name in names, 'missing WebView or handoff documentation: '+name
        for name in ('include/cell/graphics_effects.hpp', 'include/cell/ui_effects.hpp',
                     'src/graphics_effects.cpp', 'src/ui/effects.cpp',
                     'framework/cmake/CellGraphics.cmake', 'framework/GRAPHICS-EFFECTS.md',
                     'tests/graphics_effects_tests.cpp', 'tests/ui_effects_tests.cpp',
                     'examples/effects_gallery/CMakeLists.txt', 'examples/effects_gallery/gallery.cpp'):
            assert 'Cell-Framework/'+name in names, 'missing graphics SDK service: '+name
        for name in ('include/cell/latest_prepared_exchange.hpp','tests/latest_prepared_exchange_tests.cpp','tests/windows_surface_presentation_tests.cpp','framework/PREPARED-STATE-AND-RASTER.md'):
            assert 'Cell-Framework/'+name in names
        for name in ('include/cell/reaper_project.hpp','include/cell/sample_clock.hpp','tests/sample_clock_tests.cpp','framework/HOST-PROJECTS.md'):
            assert 'Cell-Framework/'+name in names
        assert 'Cell-Framework/include/cell/hsv.hpp' not in names
        assert 'Cell-Framework/src/hsv.cpp' not in names
        assert 'Cell-Framework/framework/HSV-FORMAT.md' not in names
        for name in ('framework/TRANSPORT.md','tests/sdk_transport_consumer/CMakeLists.txt',
                     'tests/sdk_transport_consumer/processor.cpp','tests/sdk_transport_consumer/host.cpp'):
            assert 'Cell-Framework/'+name in names
        for name in ('examples/auv3_gain/midi_fixture.hpp', 'examples/auv3_gain/midi_processor.cpp',
                     'examples/auv3_gain/midi_check.mm', 'tests/sdk_transport_consumer/auv3_midi_fx_host.mm',
                     'examples/auv3_gain/instrument_fixture.hpp', 'examples/auv3_gain/instrument_processor.cpp',
                     'examples/auv3_gain/instrument_check.mm', 'tests/sdk_transport_consumer/auv3_instrument_host.mm'):
            assert 'Cell-Framework/'+name in names
        for name in ('examples/auv3_gain/Cell Gain Editor.html', 'src/formats/auv3_editor.mm',
                     'include/cell/detail/web_document.hpp'):
            assert 'Cell-Framework/'+name in names
        for name in ('examples/ui_designer/model.js', 'examples/ui_designer/runtime.js',
                     'include/cell/web_meter.hpp', 'tests/ui_designer_model_tests.cjs'):
            assert 'Cell-Framework/'+name in names
        assert 'Cell-Framework/framework/cmake/CellCompilerContract.cmake' in names
        assert 'Cell-Framework/tests/compiler_contract_tests.cpp' in names
        assert 'Cell-Framework/include/cell/midi.hpp' in names
        assert 'Cell-Framework/include/cell/midi_tools.hpp' in names
        assert 'Cell-Framework/src/midi_tools.cpp' in names
        assert 'Cell-Framework/src/ui/instrument_controller.cpp' in names
        assert 'Cell-Framework/include/cell/instrument_controller.hpp' in names
        assert 'Cell-Framework/include/cell/midi_control.hpp' in names
        assert 'Cell-Framework/include/cell/midi_note_state.hpp' in names
        assert 'Cell-Framework/tests/midi_note_state_tests.cpp' in names
        assert 'Cell-Framework/tests/midi_control_tests.cpp' in names
        assert 'Cell-Framework/tests/instrument_controller_tests.cpp' in names
        assert 'Cell-Framework/tests/ui_scaling_tests.cpp' in names
        assert 'Cell-Framework/tests/midi_tools_tests.cpp' in names
        assert 'Cell-Framework/include/cell/read_ahead.hpp' in names
        assert 'Cell-Framework/src/read_ahead.cpp' in names
        assert 'Cell-Framework/tests/read_ahead_tests.cpp' in names
        assert 'Cell-Framework/include/cell/latest_exchange.hpp' in names
        assert 'Cell-Framework/tests/latest_exchange_tests.cpp' in names
        assert 'Cell-Framework/tests/prepared_exchange_transition_tests.cpp' in names
        assert 'Cell-Framework/examples/sdk_gain/processor.cpp' in names
        assert 'Cell-Framework/include/cell/commands.hpp' in names
        for name in ('src/text.cpp', 'src/localization.cpp', 'src/file.cpp',
                     'include/cell/wave_export.hpp', 'tests/wave_export_tests.cpp',
                     'include/cell/trg.hpp', 'src/trg.cpp', 'tests/trg_tests.cpp',
                     'framework/TRG-FORMAT.md',
                     'include/cell/pooled_file.hpp', 'src/pooled_file.cpp',
                     'tests/pooled_file_tests.cpp', 'framework/POOLED-READS.md',
                     'tests/sdk_pooled_file_consumer/CMakeLists.txt',
                     'tests/sdk_trg_consumer/CMakeLists.txt',
                     'src/tci.cpp', 'include/cell/tci.hpp', 'framework/TCI-FORMAT.md',
                     'include/cell/audio_file.hpp', 'src/audio_file.cpp',
                     'src/wave_file.cpp', 'src/audio_file_codecs.cpp',
                     'src/audio_file_internal.hpp', 'third_party/sndfile/sndfile.h',
                     'third_party/sndfile/COPYING', 'third_party/sndfile/PROVENANCE.json',
                     'framework/AUDIO-FILES.md',
                     'framework/ARA-SOURCES.md',
                     'tests/sdk_ara_source_consumer/CMakeLists.txt',
                     'tests/sdk_audio_files_consumer/CMakeLists.txt',
                     'tests/sdk_audio_files_consumer/main.cpp',
                     'src/midi_message.cpp', 'src/smf.cpp', 'include/cell/smf.hpp',
                     'include/cell/memory_stream.hpp', 'framework/MIDI-FILES.md',
                     'src/file_streams.cpp', 'src/uuid.cpp', 'src/unicode_case_data.hpp',
                     'include/cell/ara/source_reader.hpp', 'include/cell/widgets/controls.hpp',
                     'src/ui/widget_controls.cpp', 'framework/FILES-AND-STREAMS.md',
                     'assets/CellBrandGrotesk-SemiBold.ttf', 'assets/SpaceGrotesk-OFL.txt',
                     'assets/Rajdhani-SemiBold.ttf', 'assets/Rajdhani-OFL.txt',
                     'licenses/UNICODE-LICENSE.txt'):
            assert 'Cell-Framework/'+name in names, 'missing shared SDK service: '+name
        for name in ('framework/NATIVE-HANDOFF.md', 'tests/run_owned_window_manager.py', 'tests/module_jobs_tests.cpp', 'tests/sdk_native_consumer/main.cpp', 'src/process.cpp', 'src/ui/platform_transfer.hpp', 'src/ui/platform_transfer_linux.cpp', 'src/ui/platform_transfer_windows.cpp', 'src/ui/x11_checked.hpp', 'tests/native_control_contract_tests.cpp', 'tests/native_windows_window_tests.cpp', 'tests/native_x11_window_tests.cpp', 'tests/native_device_tests.cpp', 'tests/run_owned_pulse_device.py', 'tests/sparse_fixture.hpp'):
            assert 'Cell-Framework/'+name in names, 'missing native runtime or contract: '+name
        assert not any('/products/' in name or '/dependencies/' in name or '/build/' in name or '/.git/' in name for name in names)
        assert not any('studio_dsp.hpp' in name or 'probe_editor' in name or 'sound-cell-bars' in name for name in names)
        stored=json.loads(archive.read('Cell-Framework/SOURCE-MANIFEST.json'))
        for record in stored['files']:
            assert hashlib.sha256(archive.read('Cell-Framework/'+record['path'])).hexdigest()==record['sha256']
        for name in ('LICENSE', 'NOTICE', 'framework/THIRD_PARTY_NOTICES.md'):
            assert archive.read('Cell-Framework/'+name)==(root/name).read_bytes()
    original=output.read_bytes()
    try:
        package(root,output)
    except ValueError:
        pass
    else:
        raise AssertionError('existing SDK archive was replaced')
    assert output.read_bytes()==original
    staged=Path(temporary)/'source';staged.mkdir()
    for name in source_files(root):
        destination=staged/name;destination.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(root/name,destination)
    for name in ['framework/private.env','framework/arbitrary-output/plugin.so','framework/arbitrary-output/CMakeCache.txt','examples/sdk_gain/private-token.txt']:
        path=staged/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text('synthetic secret or generated file')
    package(staged,Path(temporary)/'isolated.zip')
    with zipfile.ZipFile(Path(temporary)/'isolated.zip') as archive:
        assert not any('private.env' in name or 'arbitrary-output' in name or 'private-token' in name for name in archive.namelist())
    script=staged/'framework/CMakeLists.txt'
    original=script.read_text(encoding='utf-8')
    for variable, relative, expected in (
            ('CELL_FRAMEWORK_SOURCE_ROOT', 'framework/private.env', 'framework/private.env'),
            ('CMAKE_CURRENT_LIST_DIR', 'arbitrary-output/*.so', 'framework/arbitrary-output/*.so')):
        script.write_text(original+f'\ninclude("${{{variable}}}/{relative}")\n', encoding='utf-8')
        failed_archive=Path(temporary)/'unpackaged.zip'
        try:
            package(staged,failed_archive)
        except ValueError as error:
            assert f'references unpackaged CMake input: {expected}' in str(error)
        else:
            raise AssertionError('unpackaged CMake input was accepted: '+expected)
        assert not failed_archive.exists()
print(f'PASS: independent framework source package, {len(manifest["files"])} manifest hashes, unchanged license and no overwrite. No compilation performed.')
