#!/usr/bin/env python3
"""Audit wrapper source wiring without invoking CMake, a compiler or a host."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def audit(root, grammar=False):
    root=Path(root).resolve(strict=True)
    contract=json.loads((root/'framework/wrappers.json').read_text())
    if contract.get('schema')!='cell.wrapper-source-contract.v1' or not isinstance(contract.get('new_source_compiled'),bool):
        raise ValueError('wrapper source contract must state its compilation scope')
    for name,item in contract['formats'].items():
        if item.get('qualified_platforms') and ('unbuilt' in item['status'] or not contract['new_source_compiled']):
            raise ValueError('unbuilt format cannot claim platform qualification: '+name)
        if not set(item.get('qualified_platforms',[])).issubset(set(item['platforms'])):
            raise ValueError('unknown qualified platform: '+name)
    if set(contract['formats'])!={'clap','vst3','vst2','aax'}:
        raise ValueError('all four requested format sources must be accounted for')
    helper=(root/'framework/cmake/CellAddPlugin.cmake').read_text()
    sdk=(root/'framework/CMakeLists.txt').read_text()
    config=(root/'framework/cmake/CellConfig.cmake.in').read_text()
    if 'Cell::Wrappers' not in sdk or 'set(Cell_Wrappers_FOUND TRUE)' not in config:
        raise ValueError('independent SDK wrapper component is not exported')
    names=set(contract['runtime_sources']+contract['unrun_cpp_tests'])
    names.update(['framework/wrappers.json','framework/CMakeLists.txt','framework/cmake/CellAddPlugin.cmake',
                  'framework/cmake/CellConfig.cmake.in','framework/cmake/CellAAXInfo.plist.in',
                  'include/cell/plugin.hpp','include/cell/parameters.hpp',
                  'include/cell/callback_gate.hpp','src/formats/reaper_project.hpp'])
    for item in contract['formats'].values():
        names.add(item['source'])
        if not re.search(r'function\('+re.escape(item['helper'])+r'\s',helper):
            raise ValueError('format helper missing: '+item['helper'])
        if Path(item['source']).name not in sdk:
            raise ValueError('installed adapter source missing: '+item['source'])
    records=[]
    for name in sorted(names):
        path=root/name
        if not path.is_file() or path.is_symlink():raise ValueError('missing or linked wrapper source: '+name)
        data=path.read_bytes()
        if path.suffix in {'.cpp','.hpp'}:
            text=data.decode('utf-8')
            if re.search(r'^\s*#\s*include\s*[<"][^>"\n]*juce',text,re.I|re.M):raise ValueError('JUCE include in wrapper source: '+name)
            if 'products/' in text and name.startswith(('include/','src/')):raise ValueError('product dependency in reusable wrapper: '+name)
        records.append(dict(path=name,sha256=hashlib.sha256(data).hexdigest()))
    result=dict(schema='cell.wrapper-source-audit.v1',source_contract=contract['schema'],
        compilation_performed=False,cmake_configured=False,cpp_tests_run=False,host_tests_run=False,
        release_qualified=False,format_sources=4,source_hashes=records)
    if grammar:
        from tree_sitter import Language,Parser
        import tree_sitter_cpp
        from cmakelang import lex,parse
        parser=Parser(Language(tree_sitter_cpp.language()))
        # This is a grammar pass, not preprocessing, C++ type checking or ABI
        # validation. Opaque SDK calling-convention tokens are removed; the
        # parser's ambiguity on empty default arguments is normalized.
        paths=contract['runtime_sources']+contract['unrun_cpp_tests']+[
            'include/cell/plugin.hpp','include/cell/parameters.hpp','src/formats/vst2.cpp','src/formats/aax.cpp']
        checked=[]
        for name in paths:
            text=(root/name).read_text()
            text='\n'.join(line if line.lstrip().startswith('#') else re.sub(r'\b(?:AAX_CALLBACK|VSTCALLBACK|CELL_VST2_EXPORT)\b','',line) for line in text.splitlines())
            text=re.sub(r'=\s*\{\s*\}(?=\s*[,\)])','= cellSourceAuditDefault',text)
            if parser.parse(text.encode()).root_node.has_error:raise ValueError('C++ grammar issue: '+name)
            checked.append(name)
        cmake=['framework/CMakeLists.txt','framework/cmake/CellAddPlugin.cmake','examples/sdk_gain/CMakeLists.txt']
        if (root/'CMakeLists.txt').is_file():cmake.append('CMakeLists.txt')
        for name in cmake:parse.parse(lex.tokenize((root/name).read_text()))
        result['grammar_scan']=dict(cpp_files=checked,cmake_files=cmake,type_checking=False,abi_checked=False)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1])
    parser.add_argument('--grammar',action='store_true',help='Use installed tree-sitter/tree-sitter-cpp/cmakelang grammar parsers only')
    parser.add_argument('--output',type=Path,help='Optional new evidence file; never replaces an existing checkpoint')
    args=parser.parse_args()
    result=audit(args.root,args.grammar)
    if args.output:
        args.output.parent.mkdir(parents=True,exist_ok=True)
        with args.output.open('x') as stream:json.dump(result,stream,indent=2);stream.write('\n')
    print('PASS: four wrapper sources, SDK exports/helpers, dependency boundary and source hashes'+('; grammar parsed' if args.grammar else '')+'. No CMake configure, compilation, C++ tests or host launch performed.')


if __name__=='__main__':main()
