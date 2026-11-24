#!/usr/bin/env python3
"""Generate Unicode case data from Python's Unicode database, without runtime dependencies."""
from pathlib import Path
import unicodedata

output=Path(__file__).resolve().parents[1]/'src/unicode_case_data.hpp'
lines=['// Unicode case data; see licenses/UNICODE-LICENSE.txt.',
       '// Generated from Unicode '+unicodedata.unidata_version+' by tools/generate_unicode_case.py.',
       '#pragma once','namespace cell::unicode::data {',
       'struct Entry {char32_t source;char32_t target[3];unsigned char count;};']
for name,method in [('lower','lower'),('upper','upper'),('fold','casefold')]:
    lines.append('inline constexpr Entry '+name+'[] = {')
    for source in range(0x110000):
        text=chr(source);mapped=getattr(text,method)()
        if mapped==text:continue
        assert 1<=len(mapped)<=3
        targets=','.join(hex(ord(code)) for code in mapped)
        lines.append('{'+hex(source)+',{'+targets+'},'+str(len(mapped))+'},')
    lines.append('};')
lines+=['struct Range {char32_t first,last;};','inline constexpr Range word[] = {']
start=previous=None
for code in range(0x110000):
    included=chr(code)=='_' or unicodedata.category(chr(code))[0] in 'LMN'
    if included:
        if start is None:start=code
        previous=code
    elif start is not None:
        lines.append('{'+hex(start)+','+hex(previous)+'},');start=None
if start is not None:lines.append('{'+hex(start)+','+hex(previous)+'},')
lines+=['};','}']
output.write_text('\n'.join(lines)+'\n')
print(output.name,output.stat().st_size,'bytes; Unicode',unicodedata.unidata_version)
