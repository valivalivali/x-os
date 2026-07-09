#!/usr/bin/env python3
"""Convert an SVG file to a C header with the SVG data as a const char array."""
import sys

if len(sys.argv) != 3:
    print("Usage: svg_to_header.py <input.svg> <output.h>", file=sys.stderr)
    sys.exit(1)

inp, outp = sys.argv[1], sys.argv[2]

with open(inp, 'r') as f:
    data = f.read()

lines = ['#ifndef SVG_DATA_H', '#define SVG_DATA_H', '', 'static const char svg_data[] =']

# Split into chunks and escape each one
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
