#!/usr/bin/env python3
"""Embed a trusted binary resource without platform-specific linker tricks."""
import argparse
import json
import re
from pathlib import Path

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("input",type=Path)
parser.add_argument("output",type=Path)
parser.add_argument("--symbol",default="font")
parser.add_argument("--asset-dir", type=Path, help="Embed an offline editor's images and fonts")
args=parser.parse_args()
data=args.input.read_bytes()
assets = []
if args.asset_dir:
    allowed = {".png", ".gif", ".jpg", ".jpeg", ".webp", ".svg", ".woff", ".woff2", ".ttf", ".otf"}
    total = 0
    for path in sorted(args.asset_dir.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"Symbolic links are outside the editor asset bundle: {path}")
        if path.is_dir():
            continue
        name = path.relative_to(args.asset_dir).as_posix()
        if len(name) > 160 or ".." in name or not re.fullmatch(r"[A-Za-z0-9._/-]+", name) or path.suffix not in allowed:
            raise ValueError(f"Unsupported editor asset: {name}")
        size = path.stat().st_size
        total += size
        if not 0 < size <= 4 * 1024 * 1024 or total > 16 * 1024 * 1024 or len(assets) == 256:
            raise ValueError("Editor assets exceed their count or size limits")
        assets.append((name, path.read_bytes()))

def emit(out, symbol, content):
    out.write(f"alignas(16) inline constexpr unsigned char {symbol}[] = {{\n")
    for offset in range(0, len(content), 32):
        out.write(",".join(str(x) for x in content[offset:offset+32]) + ",\n")
    out.write("};\n")

args.output.parent.mkdir(parents=True,exist_ok=True)
with args.output.open("w") as out:
    if not args.symbol.isidentifier():raise ValueError("invalid resource symbol")
    out.write("#pragma once\n")
    if args.asset_dir:
        out.write("#include <cell/webview.hpp>\n#include <iterator>\n")
    out.write("namespace cell::assets {\n")
    emit(out, args.symbol, data)
    if args.asset_dir:
        for index, (_, content) in enumerate(assets):
            emit(out, f"{args.symbol}Asset{index}", content)
        out.write(f"inline std::vector<cell::web::Asset> {args.symbol}Assets() {{\nreturn {{\n")
        for index, (name, _) in enumerate(assets):
            symbol = f"{args.symbol}Asset{index}"
            out.write(f"    {{{json.dumps(name)}, {{std::begin({symbol}), std::end({symbol})}}}},\n")
        out.write("};\n}\n")
    out.write("}\n")
