#!/usr/bin/env python3
"""Manual SDK integration regression: independent installed VST3 consumer.
Usage: python test_vst3_external_consumer.py --framework ROOT --vst3-sdk ROOT
       --work WRITABLE_DIRECTORY --cmake /path/to/cmake
The official SDK must already be present. This test never fetches dependencies.
"""
import argparse,ctypes,pathlib,subprocess,sys
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--framework',type=pathlib.Path,required=True)
p.add_argument('--vst3-sdk',type=pathlib.Path,required=True)
p.add_argument('--work',type=pathlib.Path,required=True)
p.add_argument('--cmake',default='cmake');a=p.parse_args()
a.work.mkdir(parents=True,exist_ok=True)
install=(a.work/'installed').resolve()
def run(*args):subprocess.run([str(x) for x in args],check=True)
run(a.cmake,'-S',a.framework/'framework','-B',a.work/'sdk-build','-DCMAKE_BUILD_TYPE=Release',f'-DCMAKE_INSTALL_PREFIX={install}')
run(a.cmake,'--build',a.work/'sdk-build','-j','4')
run(a.cmake,'--install',a.work/'sdk-build')
run(a.cmake,'-S',a.framework/'examples/sdk_gain','-B',a.work/'consumer-build','-DCMAKE_BUILD_TYPE=Release',f'-DCMAKE_PREFIX_PATH={install}',f'-DCELL_GAIN_VST3_SDK={a.vst3_sdk.resolve()}')
run(a.cmake,'--build',a.work/'consumer-build','--target','CellGain_VST3','cell_sdk_gain_smoke','-j','4')
run(a.work/'consumer-build'/'cell_sdk_gain_smoke')
if sys.platform.startswith('linux'):
    plugins=list((a.work/'consumer-build').rglob('Cell Gain Example.so'))
    if len(plugins)!=1:raise RuntimeError('Expected exactly one independent gain plugin')
    lib=ctypes.CDLL(str(plugins[0].resolve()))
    for symbol in ['ModuleEntry','ModuleExit','GetPluginFactory']:getattr(lib,symbol)
    print('PASS: installed outside-subdirectory consumer builds and exports Linux module entry/exit/factory.')
else:print('PASS: native independent VST3 consumer and gain smoke build; inspect platform exports separately.')
