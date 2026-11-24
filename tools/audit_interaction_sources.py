#!/usr/bin/env python3
"""Inspect interaction source boundaries without configuring or compiling C++."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def audit(root,grammar=False):
    root=Path(root).resolve(strict=True)
    names=['include/cell/plugin.hpp','include/cell/ui.hpp','include/cell/ui_interaction.hpp',
        'src/plugin.cpp','src/ui/window_linux.cpp','src/ui/window_windows.cpp','src/ui/window_macos.mm',
        'src/formats/vst3.cpp','src/formats/clap.cpp','src/formats/aax.cpp',
        'tests/interaction_tests.cpp','framework/INTERACTIONS.md','framework/CMakeLists.txt',
        'framework/cmake/CellConfig.cmake.in']
    studio=(root/'products/studio/interaction_panels.inc').is_file()
    if studio:names+=['products/studio/workbench_editor.cpp','products/studio/interaction_panels.inc','tests/studio_panel_ui_tests.cpp']
    records=[]
    for name in names:
        path=root/name
        if not path.is_file() or path.is_symlink():raise ValueError('missing or linked source: '+name)
        data=path.read_bytes()
        if path.suffix in {'.cpp','.hpp','.mm','.inc'}:
            if re.search(rb'^\s*#\s*include\s*[<"][^>"\n]*juce',data,re.I|re.M):raise ValueError('JUCE include: '+name)
            if name.startswith(('include/','src/')) and re.search(rb'^\s*#\s*include\s*[<"][^>"\n]*products/',data,re.M):raise ValueError('product dependency in framework: '+name)
        records.append(dict(path=name,sha256=hashlib.sha256(data).hexdigest()))
    result=dict(schema='cell.interaction-source-audit.v1',compilation_performed=False,cmake_configured=False,
        cpp_tests_run=False,native_tests_run=False,type_checking=False,abi_checked=False,release_qualified=False,
        studio_integration_included=studio,native_accessibility_providers=False,source_hashes=records)
    if grammar:
        from tree_sitter import Language,Parser
        import tree_sitter_cpp
        from cmakelang import lex,parse
        parser=Parser(Language(tree_sitter_cpp.language()));checked=[]
        for name in names:
            if Path(name).suffix not in {'.cpp','.hpp','.inc'}:continue
            text=(root/name).read_text()
            text=re.sub(r'=\s*\{\s*\}(?=\s*[,\)])','= cellSourceAuditDefault',text)
            text='\n'.join(line if line.lstrip().startswith('#') else re.sub(r'\b(?:PLUGIN_API|AAX_CALLBACK|CLAP_ABI|CLAP_EXPORT|CALLBACK)\b','',line) for line in text.splitlines())
            if name.endswith('.inc'):text='class HardwareEditor {\n'+text+'\n};'
            if name.endswith('/vst3.cpp'):
                # Opaque pre-existing SDK class/factory macros and conditional
                # inheritance are not C++ grammar. This is not preprocessing.
                text=text.split('BEGIN_FACTORY_DEF',1)[0]
                text=re.sub(r'\b(?:OBJ_METHODS|DEF_INTERFACE|END_DEFINE_INTERFACES|REFCOUNT_METHODS)\([^)]*\)','',text).replace('DEFINE_INTERFACES','')
                text=re.sub(r'#if SMTG_OS_LINUX\s*,public Linux::ITimerHandler\s*#endif','',text)
                text=text.replace('unsigned(channel)','static_cast<unsigned>(channel)')
            if parser.parse(text.encode()).root_node.has_error:raise ValueError('C++ grammar issue: '+name)
            checked.append(name)
        cmake=['framework/CMakeLists.txt','framework/cmake/CellConfig.cmake.in']
        if (root/'CMakeLists.txt').is_file():cmake.append('CMakeLists.txt')
        for name in cmake:parse.parse(lex.tokenize((root/name).read_text()))
        result['grammar']=dict(cpp_files=checked,cmake_files=cmake,objective_cpp_parsed=False,
            normalizations=['Empty default arguments','Opaque SDK calling conventions','VST3 SDK class macros, conditional inheritance and factory macro region','Pre-existing VST3 functional-cast ambiguity'])
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1])
    parser.add_argument('--grammar',action='store_true')
    parser.add_argument('--output',type=Path,help='Create a new evidence file; never overwrite a checkpoint')
    args=parser.parse_args();result=audit(args.root,args.grammar)
    if args.output:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        with args.output.open('x') as stream:json.dump(result,stream,indent=2);stream.write('\n')
    print('PASS: interaction sources and framework dependency boundary'+('; normalized C++/CMake grammar' if args.grammar else '')+'. No configure, compilation, C++ tests or host run.')


if __name__=='__main__':main()
