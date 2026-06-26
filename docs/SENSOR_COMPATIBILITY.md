# Sensor Compatibility Notes

This document describes known differences in `sensor_name` values between
JunOS JTI and Junos EVO devices, and explains how the dispatch table handles
conflicting extension numbers.

## Background

The plugin's dispatch table (`cmd/plugin/jti_dispatch.c`) is generated from
two proto trees:

- `by_release/jti/` — JunOS 22.1–25.4 native sensor definitions
- `by_release/evo/` — Junos EVO 22.4–25.4 sensor definitions

For extension numbers claimed by **both** trees with the **same message type**,
a single dispatch entry covers both JTI and EVO devices (tagged `JTI_TREE_BOTH`).

For the 3 extension numbers where JTI and EVO define **different message types**,
the dispatch table has **two separate entries** — one for each tree — both with
the same `ext_num`. At decode time, the first entry in the table is tried; if
`protobuf_c_message_unpack()` fails, the sensor bytes are skipped. In practice,
the message bytes from a JTI device will only decode cleanly against the JTI
descriptor, and EVO bytes against the EVO descriptor.

The `sensor_name` value emitted in records comes from the extension field name
in the proto file: `jnpr_<sensor>_ext` → strip prefix/suffix → `sensor_name`.

---

## Conflicting extension numbers (JTI vs EVO)

The following 3 extension numbers have different message types in JTI and EVO.
Both entries exist in the dispatch table; the correct one is selected
automatically at decode time based on which unpack succeeds.

| Ext | JTI `sensor_name`          | EVO `sensor_name`               | Notes |
|-----|----------------------------|---------------------------------|-------|
| 40  | `components`               | `af_fab_statistics`             | JTI=chassis components; EVO=AF fabric stats |
| 53  | `arp_information_mib_arp`  | `ldp_p2mp_lsp_branch_stats`     | Completely different sensors on the same slot |
| 227 | `qos_pfe_egress_qstats_227`| `qos_pfe_egress_qstats`         | Same sensor, slightly different message definition |

If you need a consistent `sensor_name` regardless of device type, add a Lua
filter to `fluent-bit.conf`:

```ini
[FILTER]
    Name    lua
    Match   jti.*
    script  /etc/fluent-bit/jti_sensor_rename.lua
    call    rename_sensors
```

Create `/etc/fluent-bit/jti_sensor_rename.lua`:

```lua
-- Remap sensor_name values for conflicting extension slots.
-- Adjust to match your environment.
local RENAMES = {
    -- ext=227: JTI emits a _227 suffix; normalise to match EVO name
    qos_pfe_egress_qstats_227 = "qos_pfe_egress_qstats",
}

function rename_sensors(tag, timestamp, record)
    local name = record["sensor_name"]
    if name and RENAMES[name] then
        record["sensor_name"] = RENAMES[name]
        return 1, timestamp, record
    end
    return 0, timestamp, record
end
```

---

## EVO-only sensors

129 EVO proto files have no JTI equivalent. These decode with their correct
EVO `sensor_name` values. They include EVO platform-specific sensors such as
`ehmd_comp_oc` (`components_telemetry`), `mgmt-ethd_oc_evo_intf_stats`
(`interfaces_mgmt`), `mib2d_oc_evo_intf_state` (`interfaces_mib_evolved`),
and many others.

---

## JTI-only sensors

Some JTI sensors have no EVO equivalent and will never produce records from
an EVO device:

- `bbe-smgd-*` — BNG subscriber management (JunOS only)
- `spu_*` — Security Processing Unit stats (SRX/JunOS only)
- `jdhcpd_*`, `jl2tpd_*`, `jpppd_*` — subscriber services (JunOS only)
- `saegw-upad_*` — mobile gateway (JunOS only)
- `packet_capture` — JunOS only

These cause no errors — dispatch entries exist but the router simply never
sends packets with those extension numbers.

---

## Verifying sensor names from your router

Enable `Debug On` in the `[INPUT]` block and watch the journal:

```bash
sudo journalctl -u fluent-bit.service -f | grep "sensor="
```

Each decoded packet logs:
```
packet from 192.168.1.1 → device=re0 sensor=interfaces_mgmt ext=99
```

Cross-reference the `ext=N` value against the dispatch table in
`cmd/plugin/jti_dispatch.c` to confirm which message type is being used.
