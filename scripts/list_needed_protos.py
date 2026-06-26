#!/usr/bin/env python3
"""
Print the list of .pb-c.c files needed to link jti_dispatch.c,
including all transitive dependencies (headers that include other headers).
"""
import sys, os, re

dispatch_c = sys.argv[1]
merged_dir = sys.argv[2]

def get_proto_includes(h_path):
    """Get .pb-c.h files included by a header (excluding google/ ones)."""
    if not os.path.exists(h_path):
        return set()
    with open(h_path) as f:
        content = f.read()
    return {m.group(1) for m in re.finditer(r'#include "([^"]+\.pb-c\.h)"', content)
            if not m.group(1).startswith('google/')}

with open(dispatch_c) as f:
    dispatch = f.read()

# Start with directly needed headers
needed = set(re.findall(r'#include "([^"]+\.pb-c\.h)"', dispatch))
needed.add('telemetry_top.pb-c.h')

# Expand transitively
frontier = set(needed)
for _ in range(8):  # max 8 levels deep
    new = set()
    for h in frontier:
        for dep in get_proto_includes(os.path.join(merged_dir, h)):
            if dep not in needed:
                needed.add(dep)
                new.add(dep)
    if not new:
        break
    frontier = new

# Map headers to .c files and print
for h in sorted(needed):
    c = h.replace('.pb-c.h', '.pb-c.c')
    path = os.path.join(merged_dir, c)
    if os.path.exists(path):
        print(path)

# Always include google protos
for gp in ['google/protobuf/descriptor.pb-c.c', 'google/protobuf/any.pb-c.c']:
    path = os.path.join(merged_dir, gp)
    if os.path.exists(path):
        print(path)
