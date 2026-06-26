#!/usr/bin/env python3
"""
gen_lookup_update.py — compare new Junos release proto files against the
current proto trees and output C lookup table lines for new sensors and fields.

Usage:
    python3 scripts/gen_lookup_update.py  \
        --new-jti  by_release/jti/26.1    \
        --new-evo  by_release/evo/26.1    \
        [--current-jti proto/jti]         \
        [--current-evo proto/evo]         \
        [--tree jti|evo|both]

Exit 0 = no changes needed. Exit 1 = changes found and printed.

Field map key encoding:
  Top-level fields:              field_num          (e.g. 51 -> "name")
  One level nested (parent P):   P * 10000 + N     (e.g. 151*10000+6 = 1510006)
  Two levels nested (P, Q):      (P*100+Q) * 10000 + N

This encoding is backward compatible: all existing flat entries (field_num < 10000)
are unchanged. Nested entries use keys >= 10000.
"""

import argparse
import os
import re
import sys
from collections import defaultdict

SKIP_KEYWORDS = [
    'aftman', 'am-', 'brcm-', '_render', 'picd_', 'hwdfpc', 'hwdre',
    'cmevod', 'jexpr', 'junos-pfe', 'platformd', 'resiliency',
    'gnmi', 'dialout', 'jnx_gnmi', 'bbe-statsd', 'nasd', 'ngapd',
]

def is_native(fname):
    return not any(k in fname for k in SKIP_KEYWORDS)


def extract_messages(content):
    """Return dict of message_name -> body_string using brace counting."""
    messages = {}
    i = 0
    while i < len(content):
        m = re.search(r'\bmessage\s+(\w+)\s*\{', content[i:])
        if not m:
            break
        msg_name = m.group(1)
        start = i + m.end() - 1
        depth, j = 1, start + 1
        while j < len(content) and depth > 0:
            if content[j] == '{':   depth += 1
            elif content[j] == '}': depth -= 1
            j += 1
        if msg_name not in messages:
            messages[msg_name] = content[start+1:j-1]
        i = i + m.start() + 1
    return messages


def strip_nested_messages(body):
    """Remove nested message definitions from a message body.
    Only direct field declarations remain, preventing inner message
    field numbers from being assigned to the wrong namespace.
    """
    result = []
    i = 0
    while i < len(body):
        m = re.search(r'\bmessage\s+\w+\s*\{', body[i:])
        if not m:
            result.append(body[i:])
            break
        result.append(body[i:i+m.start()])
        start = i + m.end() - 1
        depth, j = 1, start + 1
        while j < len(body) and depth > 0:
            if body[j] == '{':   depth += 1
            elif body[j] == '}': depth -= 1
            j += 1
        i = j
    return ''.join(result)


def get_extension(content):
    """Return (ext_num, sensor_name) or (None, None)."""
    m = re.search(
        r'extend\s+JuniperNetworksSensors\s*\{[^}]*'
        r'optional\s+\S+\s+(\S+)\s*=\s*(\d+)',
        content, re.DOTALL)
    if not m:
        return None, None
    return int(m.group(2)), m.group(1).replace('jnpr_', '').replace('_ext', '')


def encode_key(parent_chain, field_num):
    """
    Encode a nested field path as a single integer key.
    parent_chain: list of field numbers from root to direct parent.
    field_num: the field's own number.

    Top-level (no parents):    key = field_num
    One parent P:              key = P * 10000 + field_num
    Two parents P, Q:          key = (P * 100 + Q) * 10000 + field_num
    """
    if not parent_chain:
        return field_num
    if len(parent_chain) == 1:
        return parent_chain[0] * 10000 + field_num
    # Compress deeper chains: use last 2 parents
    p, q = parent_chain[-2], parent_chain[-1]
    return (p * 100 + q) * 10000 + field_num


MAX_WALK_DEPTH = 5  # Limit recursion depth to prevent hangs on deeply nested protos

def walk_fields(msg_name, messages, parent_chain=None, path_prefix='', visited=None, depth=0):
    """
    Recursively walk a message and yield (encoded_key, dotted_path, field_num).
    """
    if depth > MAX_WALK_DEPTH:
        return
    if visited is None:
        visited = set()
    if msg_name not in messages or msg_name in visited:
        return
    visited = visited | {msg_name}
    if parent_chain is None:
        parent_chain = []

    body = strip_nested_messages(messages[msg_name])
    for fm in re.finditer(
            r'(?:optional|required|repeated)\s+(\S+)\s+(\w+)\s*=\s*(\d+)',
            body):
        ftype, fname, fnum = fm.group(1), fm.group(2), int(fm.group(3))
        if ftype in ('telemetry_options', 'TelemetryFieldOptions'):
            continue
        if 'jnpr_' in fname and '_ext' in fname:
            continue

        full_path = f"{path_prefix}.{fname}" if path_prefix else fname
        key = encode_key(parent_chain, fnum)
        yield (key, full_path, fnum)

        # Recurse into known message types
        if ftype in messages and ftype not in visited:
            yield from walk_fields(
                ftype, messages,
                parent_chain=parent_chain + [fnum],
                path_prefix=full_path,
                visited=visited,
                depth=depth+1)


def get_fields(content):
    """
    Parse all fields from a proto file, returning dict of encoded_key -> path.
    Handles nested messages correctly.
    """
    messages = extract_messages(content)

    # Find top-level sensor message
    ext_m = re.search(
        r'extend\s+JuniperNetworksSensors\s*\{[^}]*'
        r'optional\s+(\S+)\s+\S+\s*=\s*\d+',
        content, re.DOTALL)
    if not ext_m:
        return {}

    top_msg = ext_m.group(1)
    fields = {}
    seen_keys = set()
    for key, path, fnum in walk_fields(top_msg, messages):
        if key not in seen_keys:
            seen_keys.add(key)
            fields[key] = path
    return fields


def load_tree(proto_dir):
    """Load all native sensor files. Returns dict of ext_num -> sensor info."""
    sensors = {}
    if not os.path.isdir(proto_dir):
        return sensors
    for fname in sorted(os.listdir(proto_dir)):
        if not fname.endswith('.proto') or not is_native(fname):
            continue
        content = open(os.path.join(proto_dir, fname)).read()
        ext, sname = get_extension(content)
        if ext and ext not in sensors:
            sensors[ext] = {
                'sensor_name': sname,
                'filename': fname,
                'fields': get_fields(content),
            }
    return sensors


def c_sensor_line(ext, name, fname):
    return f'    {{ {ext:5d}, "{name}" }},  /* {fname} */'

def c_field_line(ext, key, path):
    return f'    {{ {ext:5d}, {key:10d}, "{path}" }},'


def main():
    ap = argparse.ArgumentParser(description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--new-jti', required=True)
    ap.add_argument('--new-evo', default=None)
    ap.add_argument('--current-jti', default='proto/jti')
    ap.add_argument('--current-evo', default='proto/evo')
    ap.add_argument('--tree', choices=['jti','evo','both'], default='both')
    args = ap.parse_args()

    any_output = False
    trees = []
    if args.tree in ('jti', 'both'):
        trees.append(('JTI', args.current_jti, args.new_jti))
    if args.tree in ('evo', 'both') and args.new_evo:
        trees.append(('EVO', args.current_evo, args.new_evo))

    for tree_name, current_dir, new_dir in trees:
        print(f"\n{'='*70}")
        print(f"# {tree_name}: comparing {current_dir} → {new_dir}")
        print(f"{'='*70}\n")

        current = load_tree(current_dir)
        new     = load_tree(new_dir)
        if not new:
            print(f"# WARNING: no native sensor files in {new_dir}")
            continue

        # New sensors
        new_sensors = {k: v for k, v in new.items() if k not in current}
        if new_sensors:
            any_output = True
            print(f"# ── NEW SENSORS ({len(new_sensors)}) ────────────────────────────────────────")
            print("# Add to sensor_map[]:\n")
            for ext in sorted(new_sensors):
                s = new_sensors[ext]
                print(c_sensor_line(ext, s['sensor_name'], s['filename']))
            print()
            print("# Add to field_map[]:\n")
            for ext in sorted(new_sensors):
                s = new_sensors[ext]
                print(f"    /* {s['sensor_name']} (ext={ext}) */")
                for key in sorted(s['fields']):
                    print(c_field_line(ext, key, s['fields'][key]))
                print()
        else:
            print("# No new sensors.\n")

        # Changed sensors (new fields in existing sensors)
        changed = []
        for ext, new_s in new.items():
            if ext not in current:
                continue
            cur_fields = current[ext]['fields']
            new_fields_added   = {k: v for k, v in new_s['fields'].items()
                                  if k not in cur_fields}
            fields_removed = {k: v for k, v in cur_fields.items()
                              if k not in new_s['fields']}
            if new_fields_added or fields_removed:
                changed.append((ext, new_s, new_fields_added, fields_removed))

        if changed:
            any_output = True
            print(f"# ── CHANGED SENSORS ({len(changed)}) ──────────────────────────────────────")
            for ext, new_s, added, removed in changed:
                print(f"\n    /* {new_s['sensor_name']} (ext={ext}) */")
                if added:
                    print(f"    /* ADD: */")
                    for key in sorted(added):
                        print(c_field_line(ext, key, added[key]))
                if removed:
                    print(f"    /* REVIEW — no longer in proto (key, path): */")
                    for key, path in sorted(removed.items()):
                        print(f"    /* REMOVED: ext={ext} key={key} \"{path}\" */")
            print()
        else:
            print("# No field changes in existing sensors.\n")

        # Summary
        removed_sensors = {k: v for k, v in current.items() if k not in new}
        print(f"# ── SUMMARY ─────────────────────────────────────────────────────────")
        print(f"#   Current: {len(current)} sensors  →  New: {len(new)} sensors")
        print(f"#   Added: {len(new_sensors)}  Removed: {len(removed_sensors)}  Changed: {len(changed)}")
        print()

    if not any_output:
        print("\n# No changes — lookup tables are up to date.")
        sys.exit(0)
    sys.exit(1)


if __name__ == '__main__':
    main()
