# jti_nus — Fluent Bit INPUT plugin for Juniper JTI Native UDP Telemetry

```
 jti_nus stand for J uniper
                   T elemetry
                   I nterface
                   N ative
                   U DP
                   S ensors
```

Receives Juniper JTI (Junos Telemetry Interface) Native UDP Streaming packets
from **JunOS 22.1–25.4** and **Junos EVO 22.4–25.4** devices, decodes the
protobuf payload using generated **protobuf-c descriptors**, and feeds flat
key=value records into the Fluent Bit pipeline for routing to InfluxDB,
Elasticsearch, Splunk, flat files, or any other Fluent Bit output.

Both JunOS and Junos EVO devices can stream to the **same plugin instance on
the same port**.

---

## Repository layout

```
jti_nus_fluentbit/
├── cmd/plugin/
│   ├── jti_nus.c           ← UDP listener, queue, Fluent Bit callbacks,
│   │                          outer TelemetryStream hand-parser
│   ├── jti_walker.c        ← generic protobuf-c descriptor-driven sensor walker
│   ├── jti_walker.h        ← flat_record_t types and walker API
│   ├── jti_dispatch.c      ← auto-generated dispatch table (ext_num → descriptor)
│   └── jti_dispatch.h      ← dispatch table header
│
├── deployments/
│   ├── Dockerfile          ← two-stage build: compile plugin + minimal runtime image
│   ├── docker-compose.yml  ← compose file for container deployment
│   ├── fluent-bit.conf     ← example Fluent Bit configuration
│   └── plugins.conf        ← example tells Fluent Bit where to load the .so
│
├── scripts/
│   ├── gen_dispatch.py     ← regenerates jti_dispatch.c/.h from proto files
│   ├── list_needed_protos.py  ← finds transitive proto dependencies for linking
│   └── gen_lookup_update.py   ← compares proto trees across releases (reference)
│
├── docs/
│   ├── DOCKER.md               ← Docker deployment guide (topologies, Dockerfile, Compose)
│   └── SENSOR_COMPATIBILITY.md ← sensor_name differences between JTI and EVO
│
├── systemd/
│   └── fluent-bit.service  ← systemd service unit file
│
├── by_release/             ← per-release proto snapshots
│   ├── jti/22.1/ … 25.4/
│   └── evo/22.4/ … 25.4/
│
├── Makefile
└── README.md
```

---

## Architecture

```
Juniper device (JunOS or Junos EVO)
   │  JTI Native UDP Streaming
   │  [raw protobuf bytes, one TelemetryStream message per packet]
   ▼
┌─────────────────────────────────────────────────────────────────┐
│  jti_nus_fluentbit.so  (this plugin)                            │
│                                                                 │
│  FLBPluginInit                                                  │
│    ├── read config (Listen, Port, Buffer_Size, Debug)           │
│    ├── bind UDP socket on 0.0.0.0:4729                          │
│    └── spawn UDP listener thread                                │
│                                                                 │
│  UDP listener thread (runs continuously)                        │
│    ├── recvfrom() — one datagram per JTI packet                 │
│    ├── hand-parse outer TelemetryStream envelope                │
│    │     field 1   → system_id     → "device"                   │
│    │     field 6   → timestamp     → milliseconds               │
│    │     field 101 → EnterpriseSensors                          │
│    │       field 2636 → JuniperNetworksSensors                  │
│    │         field N  → raw sensor bytes                        │
│    ├── jti_dispatch_lookup(N) → sensor_name + pb descriptor     │
│    ├── protobuf_c_message_unpack(descriptor, sensor_bytes)      │
│    └── jti_walk_sensor() — descriptor-driven recursive walk     │
│          scalar fields → typed flat values (string/uint64/float)│
│          repeated sub-messages → one flat_record per element    │
│          (e.g. 200 interfaces → 200 independent records)        │
│                                                                 │
│  All sensor types (interface, firewall, BGP, SR-TE, chassis, …) │
│  flow through the SAME decode → flatten → queue path.           │
│  There is no internal type-based routing. Every record goes to  │
│  every configured [OUTPUT] that matches the [INPUT]'s Tag.      │
│                                                                 │
│  FLBPluginInputCallback (called by Fluent Bit on each tick)     │
│    └── drain queue → encode as MsgPack → hand to Fluent Bit     │
└───────────────────────┬─────────────────────────────────────────┘
                        │  Fluent Bit MsgPack records tagged jti.*
                        ▼
             Whatever [OUTPUT] plugin(s) you configure
          (InfluxDB, Elasticsearch, Splunk, file, stdout, …)
```

**Standard fields emitted on every record:**

| Field           | Example                 | Source                                        |
|-----------------|-------------------------|-----------------------------------------------|
| `device`        | `SV4E`                  | `system_id` field from TelemetryStream        |
| `source`        | `100.123.152.101`       | Source IP address of the UDP packet           |
| `sensor_name`   | `interfaces_mgmt`       | Looked up from `jti_dispatch[]` by ext_num    |
| `jti_timestamp` | `1750812345000`         | Timestamp in milliseconds from TelemetryStream|
| *(sensor fields)* | `in_octets=987654`   | Decoded via protobuf-c generated descriptor   |

Field names and types come directly from the generated protobuf-c descriptors —
no lookup tables, no guessing, no `field_N` fallbacks for known sensors.
Unknown extension numbers (not in the dispatch table) emit `sensor_name = "sensor_N"`
and the raw bytes are skipped.

---

## Quick start

### Prerequisites

**1. Fluent Bit v2.0 or later**

```bash
# Debian / Ubuntu — official Fluent Bit apt repository
curl https://raw.githubusercontent.com/fluent/fluent-bit/master/install.sh | sh
sudo apt-get install fluent-bit

# Or follow the official guide for your distro:
# https://docs.fluentbit.io/manual/installation/linux
```

Verify:
```bash
fluent-bit --version
```

**2. GCC, libmsgpack-c, and protobuf-c**

```bash
# Debian / Ubuntu
sudo apt-get install gcc libmsgpack-dev protobuf-compiler protobuf-c-compiler libprotobuf-c-dev

# RHEL / CentOS / Rocky
sudo yum install gcc msgpack-devel protobuf-c-devel protobuf-c-compiler
```

`protoc-c` compiles the Juniper `.proto` files to C at **build time** on your
build machine. It does not need to be installed on the Fluent Bit server.
No Go toolchain, no protoc for proto3, no generated `.pb.go` files.

### Build and install

The first `make` compiles all Juniper proto files (~30 seconds) then links
the plugin:

```bash
make
sudo make install
```

Subsequent builds after code-only changes to `cmd/plugin/` skip proto
recompilation automatically (make uses a sentinel file):

```bash
# If only jti_nus.c / jti_walker.c / jti_dispatch.c changed:
make
sudo make install
```

Force a full rebuild including proto compilation:
```bash
make clean && make
sudo make install
```

Installs `jti_nus_fluentbit.so` to `/usr/local/lib/fluent-bit/`.

To install to a different location:
```bash
sudo make install INSTALL_DIR=/path/to/your/plugins
```

### Configure Fluent Bit

Copy the example configs and edit for your environment:

```bash
sudo cp deployments/plugins.conf    /etc/fluent-bit/plugins.conf
sudo cp deployments/fluent-bit.conf /etc/fluent-bit/fluent-bit.conf
# Edit fluent-bit.conf — set your InfluxDB host, org, bucket, token
```

**`[INPUT]` configuration keys:**

| Key           | Default   | Description                              |
|---------------|-----------|------------------------------------------|
| `Listen`      | `0.0.0.0` | UDP listen address                       |
| `Port`        | `4729`    | UDP listen port                          |
| `Buffer_Size` | `65535`   | UDP receive buffer in bytes              |
| `Debug`       | `Off`     | `On` to enable packet-level debug logging|

### Start with systemd

```bash
sudo cp systemd/fluent-bit.service /etc/systemd/system/fluent-bit.service
sudo systemctl daemon-reload
sudo systemctl enable fluent-bit.service
sudo systemctl start  fluent-bit.service
sudo systemctl status fluent-bit.service
```

### Configure your Junos devices

```
# Streaming server — Fluent Bit host
set services analytics streaming-server fluent-bit remote-address <FluentBit-IP>
set services analytics streaming-server fluent-bit remote-port 4729

# Export profile — UDP, optional management interface routing
set services analytics export-profile jti-udp transport udp
set services analytics export-profile jti-udp reporting-rate 30
set services analytics export-profile jti-udp local-address <mgmt-interface-IP>
set services analytics export-profile jti-udp routing-instance mgmt_junos

# One sensor block per resource path
set services analytics sensor interfaces server-name fluent-bit
set services analytics sensor interfaces export-name jti-udp
set services analytics sensor interfaces resource /junos/system/linecard/interface/

set services analytics sensor chassis server-name fluent-bit
set services analytics sensor chassis export-name jti-udp
set services analytics sensor chassis resource /junos/chassis/
```

> **Note:** Junos devices can only stream Native UDP via a revenue interface.
>           Junos EVO devices can use either a revenue interface or the
>           management interface as long as the interface is in the default
>           routing instance.


Verify on the device:
```
show agent sensors
show services analytics
```

---

## How decoding works

The plugin uses a **hybrid approach**:

**Outer envelope — hand-parsed:** The `TelemetryStream` wrapper fields
(device name, timestamp, enterprise extension) use a fixed, stable wire
format that never changes between Junos releases. These 5 fields are
decoded by a small hand-written varint parser in `jti_nus.c`.

**Inner sensor messages — protobuf-c generated:** Once the raw sensor bytes
are extracted and the extension number is identified, the plugin calls
`protobuf_c_message_unpack()` using the descriptor from the dispatch table.
The `jti_walker.c` generic walker then traverses the decoded message tree
using the descriptor's field metadata:

- Scalar fields (`string`, `uint64`, `bool`, `float`, etc.) → emitted with
  correct names and types directly from the descriptor
- Repeated sub-messages (e.g. one sub-message per interface) → one
  `flat_record_t` per element, all stamped with the same metadata
- Nested sub-messages → flattened into the current record, field names
  from the descriptor at each nesting level

This eliminates all field name/type guessing. Field names come from
the generated C structs — the same names used in the `.proto` files.

**Dispatch table** (`cmd/plugin/jti_dispatch.c`) maps extension number →
`(sensor_name, ProtobufCMessageDescriptor*)`. It is auto-generated by
`scripts/gen_dispatch.py` from the proto files in `by_release/`.

---

## Adding a new Junos release (e.g. 26.1)

### Step 1 — Add the proto files

```bash
cp -r /path/to/26.1-jti-protos  by_release/jti/26.1
cp -r /path/to/26.1-evo-protos  by_release/evo/26.1
```

### Step 2 — Regenerate the dispatch table

```bash
make dispatch
# or directly:
python3 scripts/gen_dispatch.py \
    --jti by_release/jti/26.1 \
    --evo by_release/evo/26.1 \
    --out-c cmd/plugin/jti_dispatch.c \
    --out-h cmd/plugin/jti_dispatch.h
```

This rewrites `cmd/plugin/jti_dispatch.c` and `cmd/plugin/jti_dispatch.h`
with all sensors from **both** JTI and EVO trees, using the latest release.

### Step 3 — Rebuild

```bash
make clean && make
sudo make install
sudo systemctl restart fluent-bit.service
```

`make clean` is needed because the dispatch table changed, which means the
proto compilation step also needs to pick up any new `.proto` files.

---

## Running in Docker

The plugin supports several deployment topologies depending on whether the
container host receives the UDP telemetry stream directly or whether Fluent Bit
runs on a separate VM.

See **[docs/DOCKER.md](docs/DOCKER.md)** for the full guide covering:

- Deployment topology selection (container receives UDP vs separate Fluent Bit VM)
- Dockerfile (three-stage build — the official Fluent Bit image is distroless
  so the runtime is based on `debian:12-slim` instead)
- `docker run` options — host networking vs port mapping
- Runtime config override via volume mounts
- Docker Compose
- Viewing container logs
- Troubleshooting missing libraries

---

## Debugging

Enable packet-level logging by setting `Debug On` in the `[INPUT]` block:

```ini
[INPUT]
    Name          jti_nus
    Port          4729
    Buffer_Size   65535
    Debug         On
```

With debug enabled you will see:
```
[jti_nus] packet from 100.123.152.101 size=312 bytes
[jti_nus] packet from 100.123.152.101 → device=SV4E sensor=interfaces_mgmt ext=99
[jti_nus]   → 1 records queued
[jti_nus] flushing 12 records to Fluent Bit pipeline
[jti_nus] stats: packets=142  records=3408  dropped=0  errors=0  queue=0
```

To watch decoded records in real time:
```bash
tail -f /var/log/jti_nus/jti.log | jq .
tail -f /var/log/jti_nus/jti.log | jq 'select(.sensor_name == "interfaces_mgmt")'
tail -f /var/log/jti_nus/jti.log | jq 'select(.device == "SV4E")'
```

Verify UDP packets are arriving before the plugin decodes them:
```bash
sudo tcpdump -i any -n udp port 4729 -c 5
```

---

## Output plugin examples

**InfluxDB v2:**
```ini
[OUTPUT]
    Name          influxdb
    Match         jti.*
    Host          influxdb-host
    Port          8086
    Bucket        jti_native
    Org           your-org
    Sequence_Tag  _seq
    Tag_Keys      sensor_name device
    HTTP_Token    your-token
```

**Elasticsearch:**
```ini
[OUTPUT]
    Name          es
    Match         jti.*
    Host          elasticsearch-host
    Port          9200
    Index         jti-telemetry
```

**Splunk HEC:**
```ini
[OUTPUT]
    Name          splunk
    Match         jti.*
    Host          splunk-host
    Port          8088
    Splunk_Token  your-hec-token
```

**File (plain JSON — pipe to `jq` for readability):**
```ini
[OUTPUT]
    Name          file
    Match         jti.*
    Path          /var/log/jti_nus
    File          jti.log
    Format        plain
```

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `error opening plugin: undefined symbol` | Missing transitive proto dependency | Run `make clean && make` |
| `error loading proxy plugin` | Wrong .so path in plugins.conf | Check `Path` in `/etc/fluent-bit/plugins.conf` |
| `no sensor extension in packet` | Keepalive/control packet (normal) | Ignore — not an error; disable `Debug` to suppress |
| `sensor_name = "sensor_N"` in records | Extension number not in dispatch table | Run `make dispatch` with new proto files and rebuild |
| Records not appearing in InfluxDB | Field type conflict from old schema | Drop and recreate the InfluxDB bucket for a clean schema |
| InfluxDB `422` errors | Stale field type schema | Drop bucket, recreate, restart Fluent Bit |
| No records at all | Packets not arriving | `tcpdump -i any udp port 4729` — check device config |
| `cannot find -lmsgpackc` | Wrong library name for distro | `find /usr -name "libmsgpack*"` and adjust `LIBS` in Makefile |
| `cannot find -lprotobuf-c` | protobuf-c not installed | `apt-get install libprotobuf-c-dev` |

---

## Sensor coverage

**275 native sensors** across JTI and EVO trees (JunOS 22.1–25.4, Junos EVO 22.4–25.4).
Render variants (`*-render.proto`, `aftman-*`, `brcm-*`) are excluded — they
duplicate native sensors. The dispatch table has **186 entries** covering both
JTI-only, EVO-only, and shared sensors, with separate entries for the 3
extension numbers where JTI and EVO define different message types.

See [docs/SENSOR_COMPATIBILITY.md](docs/SENSOR_COMPATIBILITY.md) for
sensor_name differences between JTI and EVO devices.
