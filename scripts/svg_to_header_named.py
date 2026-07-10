#!/usr/bin/env python3
"""Convert an SVG file to a C header with the SVG data as a const char array.
Usage: svg_to_header_named.py <input.svg> <output.h> <var_name> <guard_name>
"""
import sys

if len(sys.argv) != 5:
    print("Usage: svg_to_header_named.py <input.svg> <output.h> <var_name> <guard_name>", file=sys.stderr)
    sys.exit(1)

inp, outp, var_name, guard_name = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]

with open(inp, 'r') as f:
    data = f.read()

lines = [f'#ifndef {guard_name}', f'#define {guard_name}', '', f'static const char {var_name}[] =']

chunk_size = 200
i = 0
while i < len(data):
    chunk = data[i:i+chunk_size]
    escaped = chunk.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')
    lines.append('  "' + escaped + '"')
    i += chunk_size

lines.append('  ;')
lines.append('')
lines.append('#endif')

with open(outp, 'w') as f:
    f.write('\n'.join(lines) + '\n')
