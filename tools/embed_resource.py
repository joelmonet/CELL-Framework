#!/usr/bin/env python3
"""Embed a trusted binary resource without platform-specific linker tricks."""
import argparse
from pathlib import Path

parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument("input",type=Path)
parser.add_argument("output",type=Path)
parser.add_argument("--symbol",default="font")
args=parser.parse_args()
data=args.input.read_bytes()
args.output.parent.mkdir(parents=True,exist_ok=True)
with args.output.open("w") as out:
    if not args.symbol.isidentifier():raise ValueError("invalid resource symbol")
    out.write("#pragma once\nnamespace cell::assets {\nalignas(16) inline constexpr unsigned char "+args.symbol+"[] = {\n")
    for offset in range(0,len(data),32):out.write(",".join(str(x) for x in data[offset:offset+32])+",\n")
    out.write("};\n}\n")
