# ── Stage 1: extract Fluent Bit binary and config from official image ───────
# The official fluent/fluent-bit image is distroless — no shell, no package
# manager. It only contains /fluent-bit/bin/fluent-bit and /fluent-bit/etc/.
# We extract just those two paths; all runtime libraries come from debian below.
FROM fluent/fluent-bit:3.1 AS fluent-extract

# ── Stage 2: build jti_nus plugin ──────────────────────────────────────────
FROM ubuntu:22.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc make python3 \
    libmsgpack-dev \
    protobuf-compiler \
    protobuf-c-compiler libprotobuf-c-dev \
    libprotobuf-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .
RUN make

# ── Stage 3: runtime ───────────────────────────────────────────────────────
# debian:12-slim is ABI-compatible with the official Fluent Bit build and
# has apt-get so we can install all required runtime libraries.
FROM debian:12-slim

# Runtime libraries for Fluent Bit and the jti_nus plugin
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
# Note: the official image only has /fluent-bit/bin and /fluent-bit/etc —
# there is no /fluent-bit/lib in the distroless image.
COPY --from=fluent-extract /fluent-bit/bin/fluent-bit /fluent-bit/bin/fluent-bit
COPY --from=fluent-extract /fluent-bit/etc/           /fluent-bit/etc/

# Create plugin directory and install our plugin
RUN mkdir -p /fluent-bit/plugins
COPY --from=builder /build/jti_nus_fluentbit.so /fluent-bit/plugins/

# Install configuration
COPY deployments/plugins.conf    /fluent-bit/etc/plugins.conf
COPY deployments/fluent-bit.conf /fluent-bit/etc/fluent-bit.conf

# Create non-root user and log directory
RUN useradd -r -s /bin/false fluent-bit \
    && mkdir -p /var/log/jti_nus \
    && chown fluent-bit /var/log/jti_nus

USER fluent-bit

EXPOSE 4729/udp 2020/tcp

ENTRYPOINT ["/fluent-bit/bin/fluent-bit"]
CMD ["-c", "/fluent-bit/etc/fluent-bit.conf"]
