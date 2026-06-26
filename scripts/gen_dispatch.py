#!/usr/bin/env python3
"""
gen_dispatch.py — generate jti_dispatch.c and jti_dispatch.h
from JTI and EVO proto files.

These files contain:
  - extern declarations for all sensor message descriptors
  - jti_sensor_dispatch[] table mapping ext_num -> descriptor + sensor_name
  - One entry per (tree, ext_num) combination

Usage:
  python3 scripts/gen_dispatch.py \
      --jti by_release/jti/25.4 \
      --evo by_release/evo/25.4 \
      --out-c cmd/plugin/jti_dispatch.c \
      --out-h cmd/plugin/jti_dispatch.h
"""

import argparse, os, re, sys

SKIP = ['aftman', 'am-', 'brcm-', '_render', 'picd_', 'hwdfpc', 'hwdre',
        'cmevod', 'jexpr', 'junos-pfe', 'platformd', 'resiliency',
        'gnmi', 'dialout', 'jnx_gnmi', 'bbe-statsd', 'nasd', 'ngapd']

def is_native(fname):
    return not any(k in fname for k in SKIP)

def get_sensor(content):
    m = re.search(
        r'extend\s+JuniperNetworksSensors\s*\{[^}]*'
        r'optional\s+(\S+)\s+(\S+)\s*=\s*(\d+)',
        content, re.DOTALL)
    if not m:
        return None, None, None
    msg_type  = m.group(1)
    field_name = m.group(2).replace('jnpr_','').replace('_ext','')
    ext_num   = int(m.group(3))
    return msg_type, field_name, ext_num

def msg_to_c_base(msg_name):
    """Convert a proto message name to protobuf-c C symbol base.

    protobuf-c naming rules:
    - Underscore before uppercase letter -> double underscore
        mpls_SrTe     -> mpls__sr_te
        network_instances_PIM -> network_instances__pim
    - Underscore before lowercase/digit stays single
        interfaces_mgmt -> interfaces_mgmt
    - Lowercase->uppercase CamelCase transition -> insert underscore
        LspStats -> lsp_stats, CpuMemory -> cpu_memory
    - Consecutive uppercase (acronyms) -> just lowercase, no splitting
        IGMP -> igmp, PIM -> pim, SRTE -> srte
    """
    result = []
    i = 0
    while i < len(msg_name):
        c = msg_name[i]
        if c == '_':
            # Double underscore if next char is uppercase
            if i + 1 < len(msg_name) and msg_name[i+1].isupper():
                result.append('__')
            else:
                result.append('_')
            i += 1
        elif c.isupper():
            # Insert underscore only on lowercase->uppercase transition
            prev_char = msg_name[i-1] if i > 0 else ''
            if prev_char.islower() or (prev_char.isdigit() and
                    i + 1 < len(msg_name) and msg_name[i+1].islower()):
                result.append('_')
            result.append(c.lower())
            i += 1
        else:
            result.append(c)
            i += 1
    return ''.join(result)

def msg_to_descriptor(msg_name):
    """CamelCase/snake_case message name → protobuf-c __descriptor symbol."""
    return msg_to_c_base(msg_name) + '__descriptor'

def msg_to_unpack(msg_name):
    """CamelCase/snake_case → protobuf-c __unpack symbol."""
    return msg_to_c_base(msg_name) + '__unpack'

def msg_to_free(msg_name):
    return msg_to_c_base(msg_name) + '__free_unpacked'

def load_sensors(proto_dir, tree_name):
    sensors = {}
    if not os.path.isdir(proto_dir):
        return sensors
    for fname in sorted(os.listdir(proto_dir)):
        if not fname.endswith('.proto') or not is_native(fname):
            continue
        with open(os.path.join(proto_dir, fname)) as f:
            content = f.read()
        msg_type, sensor_name, ext_num = get_sensor(content)
        if ext_num is None or ext_num in sensors:
            continue
        # Derive the generated header filename
        header = fname.replace('.proto', '.pb-c.h')
        sensors[ext_num] = {
            'msg_type':    msg_type,
            'sensor_name': sensor_name,
            'descriptor':  msg_to_descriptor(msg_type),
            'unpack':      msg_to_unpack(msg_type),
            'free':        msg_to_free(msg_type),
            'header':      header,
            'proto':       fname,
            'tree':        tree_name,
        }
    return sensors

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--jti', required=True, help='JTI proto directory')
    ap.add_argument('--evo', required=True, help='EVO proto directory')
    ap.add_argument('--out-c', default='cmd/plugin/jti_dispatch.c')
    ap.add_argument('--out-h', default='cmd/plugin/jti_dispatch.h')
    args = ap.parse_args()

    jti = load_sensors(args.jti, 'jti')
    evo = load_sensors(args.evo, 'evo')

    # Merge: EVO overrides JTI for same ext_num
    # (EVO devices send EVO protos; JTI devices send JTI protos;
    #  for conflicts we keep both and try JTI first at decode time)
    conflicts = {k for k in jti if k in evo and jti[k]['msg_type'] != evo[k]['msg_type']}
    print(f"JTI sensors: {len(jti)}  EVO sensors: {len(evo)}  "
          f"conflicts: {len(conflicts)}", file=sys.stderr)
    if conflicts:
        print(f"  Conflicting ext_nums (will have dual entries): "
              f"{sorted(conflicts)}", file=sys.stderr)

    # Build list of dispatch entries (one per ext_num per tree for conflicts)
    entries = []
    all_exts = sorted(set(list(jti.keys()) + list(evo.keys())))
    for ext in all_exts:
        if ext in conflicts:
            # Both trees — separate entries flagged by tree
            entries.append((ext, jti[ext], 'JTI'))
            entries.append((ext, evo[ext], 'EVO'))
        elif ext in jti:
            entries.append((ext, jti[ext], 'BOTH'))
        else:
            entries.append((ext, evo[ext], 'EVO'))

    # Collect unique headers needed
    headers = sorted(set(s['header'] for _, s, _ in entries))

    # --- Write .h ---
    with open(args.out_h, 'w') as fh:
        fh.write("""\
/* jti_dispatch.h — auto-generated by scripts/gen_dispatch.py  DO NOT EDIT */
#ifndef JTI_DISPATCH_H
#define JTI_DISPATCH_H

#include <stdint.h>
#include <protobuf-c/protobuf-c.h>

typedef enum {
    JTI_TREE_JTI  = 0,
    JTI_TREE_EVO  = 1,
    JTI_TREE_BOTH = 2,
} jti_tree_t;

typedef struct {
    uint32_t                         ext_num;
    jti_tree_t                       tree;
    const char                      *sensor_name;
    const ProtobufCMessageDescriptor *descriptor;
} jti_dispatch_entry_t;

extern const jti_dispatch_entry_t jti_dispatch[];
extern const int                  jti_dispatch_size;

/* Look up the dispatch entry for an extension number.
 * Returns NULL if not found. For conflicts, prefers JTI. */
const jti_dispatch_entry_t *jti_dispatch_lookup(uint32_t ext_num);

#endif /* JTI_DISPATCH_H */
""")

    # --- Write .c ---
    with open(args.out_c, 'w') as fc:
        fc.write("/* jti_dispatch.c — auto-generated by scripts/gen_dispatch.py"
                 "  DO NOT EDIT */\n")
        fc.write('#include "jti_dispatch.h"\n\n')

        # Include all sensor headers
        for hdr in headers:
            fc.write(f'#include "{hdr}"\n')
        fc.write('\n')

        # extern declarations for descriptors
        all_descriptors = sorted(set(s['descriptor'] for _, s, _ in entries))
        for desc in all_descriptors:
            fc.write(f'extern const ProtobufCMessageDescriptor {desc};\n')
        fc.write('\n')

        # Dispatch table
        fc.write('const jti_dispatch_entry_t jti_dispatch[] = {\n')
        for ext, s, tree in entries:
            tree_enum = f'JTI_TREE_{tree}'
            fc.write(
                f'    {{ {ext:5d}, {tree_enum}, '
                f'"{s["sensor_name"]}", '
                f'&{s["descriptor"]} }},\n'
            )
        fc.write('};\n\n')
        fc.write(f'const int jti_dispatch_size = '
                 f'{len(entries)};\n\n')

        # Lookup function
        fc.write("""\
const jti_dispatch_entry_t *jti_dispatch_lookup(uint32_t ext_num)
{
    /* Linear scan — table is small enough; could binary-search if needed */
    for (int i = 0; i < jti_dispatch_size; i++) {
        if (jti_dispatch[i].ext_num == ext_num)
            return &jti_dispatch[i];
    }
    return NULL;
}
""")

    print(f"Written {args.out_c} and {args.out_h} "
          f"({len(entries)} dispatch entries)", file=sys.stderr)

if __name__ == '__main__':
    main()
