# Docker Deployment Guide

## Deployment topologies

Before choosing a run option, identify which host receives the UDP packets
from your Junos devices:

**Topology A — container host receives the UDP packets:**
```
Juniper device
      │  UDP 4729
      ▼
 Container host
 ┌─────────────────────────────┐
 │  Docker container           │
 │  Fluent Bit + jti_nus.so   │──→ InfluxDB / Elasticsearch / Splunk
 └─────────────────────────────┘
```
Use `--network host` or `-p 4729:4729/udp` as shown below. The plugin
runs inside the container and receives packets directly.

**Topology B — a separate VM receives the UDP packets:**
```
Juniper device
      │  UDP 4729
      ▼
 Fluent Bit VM  (jti_nus plugin installed natively)
      │  outputs
      ▼
 Container host (InfluxDB / Grafana / dashboards)
```
The container host does **not** run the plugin. Fluent Bit and the `.so`
are installed natively on the Fluent Bit VM (see the Quick start section
of the README). The Docker setup below does not apply to this topology.

**Topology C — both in the same environment:**
```
Juniper devices
      │  UDP 4729
      ▼
 Fluent Bit VM  (jti_nus plugin, native install)
      │  outputs to
      ├──→ InfluxDB container
      ├──→ Grafana container
      └──→ Elasticsearch container
```
Run the plugin natively on the Fluent Bit VM. Run your storage and
visualisation stack in containers on a separate host. No plugin `.so`
is needed inside those containers.

The Dockerfile and Compose file below apply only to **Topology A** — where
you want Fluent Bit and the plugin running inside a container that directly
receives the UDP telemetry stream.

---

## Why three build stages

The official `fluent/fluent-bit` image is **distroless** — it contains only
the Fluent Bit binary and its direct runtime dependencies, with no shell, no
package manager, and no standard OS tools. This makes it secure and small but
means you cannot install additional packages into it directly.

The Dockerfile uses three stages to work around this:

| Stage | Base | Purpose |
|-------|------|---------|
| `fluent-extract` | `fluent/fluent-bit:3.1` | Extract the Fluent Bit binary and bundled libraries |
| `builder` | `ubuntu:22.04` | Compile the jti_nus plugin and all Juniper proto files |
| runtime | `debian:12-slim` | Install runtime libraries, combine Fluent Bit + plugin |

`debian:12-slim` is used as the runtime base because it has a real package
manager (`apt-get`) so we can install the libraries that both Fluent Bit and
the plugin need, and it is ABI-compatible with the official Fluent Bit build.

---

## Dockerfile

The Dockerfile is at `deployments/Dockerfile`:

```dockerfile
# ── Stage 1: extract Fluent Bit binary and config from official image ───────
# The official image is distroless — it only contains /fluent-bit/bin and
# /fluent-bit/etc. There is no /fluent-bit/lib in the distroless image.
FROM fluent/fluent-bit:3.1 AS fluent-extract

# ── Stage 2: build jti_nus plugin ──────────────────────────────────────────
FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc make python3 \
    libmsgpack-dev \
    protobuf-compiler \
    protobuf-c-compiler libprotobuf-c-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .
RUN make

# ── Stage 3: runtime ───────────────────────────────────────────────────────
FROM debian:12-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    libsasl2-2 \
    libpq5 \
    libsystemd0 \
    libyaml-0-2 \
    libcurl4 \
    libprotobuf-c1 \
    libmsgpackc2 \
    && rm -rf /var/lib/apt/lists/*

# Copy Fluent Bit binary and default config from official image.
# Only /fluent-bit/bin and /fluent-bit/etc exist in the distroless image.
COPY --from=fluent-extract /fluent-bit/bin/fluent-bit /fluent-bit/bin/fluent-bit
COPY --from=fluent-extract /fluent-bit/etc/           /fluent-bit/etc/

RUN mkdir -p /fluent-bit/plugins
COPY --from=builder /build/jti_nus_fluentbit.so /fluent-bit/plugins/

COPY deployments/plugins.conf    /fluent-bit/etc/plugins.conf
COPY deployments/fluent-bit.conf /fluent-bit/etc/fluent-bit.conf

RUN useradd -r -s /bin/false fluent-bit \
    && mkdir -p /var/log/jti_nus \
    && chown fluent-bit /var/log/jti_nus

USER fluent-bit

EXPOSE 4729/udp 2020/tcp

ENTRYPOINT ["/fluent-bit/bin/fluent-bit"]
CMD ["-c", "/fluent-bit/etc/fluent-bit.conf"]
```

> **Note:** If the build fails with missing shared libraries at runtime,
> run `ldd /fluent-bit/bin/fluent-bit` inside the container to see exactly
> which libraries are needed and add the missing packages to the `apt-get`
> install line.

---

## Build the image

Run from the repository root:

```bash
docker build -f deployments/Dockerfile -t jti_nus_fluentbit:latest .
```

The first build takes ~2–3 minutes — proto compilation (~30s) dominates.
The build context (all source files including `by_release/` proto snapshots)
is ~3.5 MB — this is normal and expected.
Subsequent builds are faster because Docker caches the proto compilation
layer unless the proto files change.

---

## Run the container

The container must receive UDP on the JTI port. Use `--network host`
(strongly recommended) or publish the UDP port explicitly:

```bash
# Option A — host networking (recommended, Linux only)
sudo docker run -d \
    --name jti_nus \
    --network host \
    --restart unless-stopped \
    jti_nus_fluentbit:latest

# Option B — bridge networking with port mapping
sudo docker run -d \
    --name jti_nus \
    -p 4729:4729/udp \
    --restart unless-stopped \
    jti_nus_fluentbit:latest
```

> **Why host networking?** With bridge networking and port mapping, high-rate
> UDP packet bursts can cause kernel buffer drops before Docker forwards them
> to the container. Host networking avoids this entirely by giving the
> container direct access to the host network stack.

---

## Log directory and volume mounts

The Dockerfile creates `/var/log/jti_nus` inside the container and assigns
it to the `fluent-bit` user, so the file output plugin can write to it
without permission errors.

How you mount it determines whether you need to do anything on the host VM:

**Named volume (recommended — Compose default):**
```yaml
volumes:
  - jti_logs:/var/log/jti_nus
```
Docker manages the storage. The directory does not need to exist on the host
VM. Logs persist across container restarts and are accessible on the host
under `/var/lib/docker/volumes/jti_logs/_data/`.

**Bind mount (specific host path):**
```bash
-v /var/log/jti_nus:/var/log/jti_nus
```
The directory **must exist on the host VM before starting the container.**
If it does not exist, Docker creates it owned by `root` and the `fluent-bit`
user inside the container will not be able to write to it. Create it first:

```bash
sudo mkdir -p /var/log/jti_nus
sudo chown 999:999 /var/log/jti_nus   # 999 is the fluent-bit UID inside the container
```

Use the named volume approach unless you specifically need the logs at a
known path on the host VM.

---

## Configuring fluent-bit.conf for the container

The `deployments/fluent-bit.conf` file is written for a native install where
Fluent Bit's config files live under `/etc/fluent-bit/`. Inside the container
they live under `/fluent-bit/etc/`. **Before running the container you must
update the `[SERVICE]` section** of your `fluent-bit.conf`:

```ini
[SERVICE]
    flush           1
    daemon          Off
    log_level       info
    parsers_file    /fluent-bit/etc/parsers.conf   # ← changed from /etc/fluent-bit/
    plugins_file    /fluent-bit/etc/plugins.conf   # ← changed from /etc/fluent-bit/
    http_server     On
    http_listen     0.0.0.0
    http_port       2020
    storage.metrics on
```

If you leave the native paths (`/etc/fluent-bit/...`) in place the container
will fail to start with:
```
[error] could not open parser configuration file, aborting.
[error] plugins_file not found, aborting.
```

---

## Override configuration at runtime

Mount your own `fluent-bit.conf` to customise outputs without rebuilding
the image:

```bash
sudo docker run -d \
    --name jti_nus \
    --network host \
    --restart unless-stopped \
    -v /etc/jti_nus/fluent-bit.conf:/fluent-bit/etc/fluent-bit.conf:ro \
    -v /var/log/jti_nus:/var/log/jti_nus \
    jti_nus_fluentbit:latest
```

---

## Docker Compose

`deployments/docker-compose.yml`:

```yaml
services:
  jti_nus:
    build:
      context: ..
      dockerfile: deployments/Dockerfile
    image: jti_nus_fluentbit:latest
    container_name: jti_nus
    network_mode: host
    restart: unless-stopped
    volumes:
      - ./fluent-bit.conf:/fluent-bit/etc/fluent-bit.conf:ro
      - jti_logs:/var/log/jti_nus

volumes:
  jti_logs:
```

Run from the `deployments/` directory:

```bash
docker compose -f deployments/docker-compose.yml up -d
docker compose -f deployments/docker-compose.yml logs -f
```

---

## Viewing logs from the container

```bash
# Fluent Bit output (startup, errors, debug messages)
sudo docker logs -f jti_nus

# Decoded records (if file output is configured)
sudo docker exec jti_nus tail -f /var/log/jti_nus/jti.log
```

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `error opening plugin: undefined symbol` | Missing runtime library | Add missing package to `apt-get` in Stage 3 |
| `cannot open shared object file` | Library not in container | Run `docker exec jti_nus ldd /fluent-bit/plugins/jti_nus_fluentbit.so` to identify missing libs |
| No UDP packets received | Bridge networking buffer drops | Switch to `--network host` |
| `permission denied` connecting to Docker | User not in docker group | Use `sudo` or `sudo usermod -aG docker $USER` then re-login |
| Config changes not taking effect | Old image cached | `docker stop jti_nus && docker rm jti_nus` then rerun |
