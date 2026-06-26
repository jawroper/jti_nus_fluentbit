/*
 * jti_nus.c — Fluent Bit C INPUT plugin for Juniper JTI Native UDP Telemetry
 *
 * Supports JunOS 22.1-25.4 and Junos EVO 22.4-25.4 native sensors.
 * Both JTI and EVO devices can stream to the same plugin instance with no
 * conflicts — there is no protobuf runtime, no global registry, no external runtime.
 *
 * Wire format decoded:
 *
 *   TelemetryStream {
 *     field  1 (string)  system_id   → "device" in output record
 *     field  4 (string)  sensor_name → sensor path from router (informational)
 *     field  6 (uint64)  timestamp   → milliseconds since epoch
 *     field 101 (message) enterprise {
 *       field 2636 (message) JuniperNetworksSensors {
 *         field N (message) <sensor>  → N looked up in sensor_map[]
 *       }
 *     }
 *   }
 *
 * Each field in the sensor message is recursively walked and emitted as a
 * flat key=value pair. Repeated message elements (e.g. per-interface stats)
 * each produce an independent Fluent Bit record.
 *
 * Build:
 *   gcc -shared -fPIC -O2 -o jti_nus_fluentbit.so jti_nus.c \
 *       -lmsgpack-c -lpthread
 *
 * fluent-bit.conf [INPUT] keys:
 *   Name          jti_nus
 *   Listen        0.0.0.0   (default)
 *   Port          4729      (default)
 *   Buffer_Size   65535     (default)
 *   Debug         Off       (On to enable packet-level logging)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <msgpack.h>
#include "jti_dispatch.h"
#include "jti_walker.h"

/* ── Fluent Bit external plugin API ──────────────────────────────────────── */
/*                                                                            */
/* External .so plugins communicate with Fluent Bit through a proxy interface */
/* defined by Fluent Bit's internal plugin_proxy mechanism. The structures    */
/* below match Fluent Bit's internal layout exactly — they must not be        */
/* changed without reference to Fluent Bit source: src/proxy/go/go.c and      */
/* include/fluent-bit/flb_plugin_proxy.h                                      */

#define FLB_ERROR   0
#define FLB_OK      1
#define FLB_RETRY   2

#define FLB_PROXY_INPUT_PLUGIN  1
#define FLB_PROXY_EXTERNAL      11   /* external .so plugin proxy type (Fluent Bit internal value) */

struct flb_plugin_proxy_def {
    int   type;
    int   proxy;
    int   flags;
    char *name;
    char *description;
    int   event_type;
};

struct flb_api {
    char *_;
    char *(*input_get_property)(char *, void *);
};

struct flb_plugin_proxy_context {
    void *remote_context;
    void *proxy;
};

/* Used by proxy_go_input_collect when calling FLBPluginInputCallbackCtx */
struct flb_input_proxy_context {
    int   coll_fd;          /* collector file descriptor — Fluent Bit internal */
    void *remote_context;   /* our jti_ctx_t* set during FLBPluginInit        */
    void *proxy;
};

struct flb_input_plugin_ctx {
    void                          *_;
    struct flb_api                *api;
    void                          *i_ins;
    struct flb_plugin_proxy_context *context;
};

static char *get_config_key(void *plugin, const char *key)
{
    struct flb_input_plugin_ctx *p = plugin;
    return p->api->input_get_property((char *)key, p->i_ins);
}

/* ── Protobuf wire-format constants ──────────────────────────────────────── */

#define PB_WIRE_VARINT   0
#define PB_WIRE_64BIT    1
#define PB_WIRE_LEN      2
#define PB_WIRE_32BIT    5

#define FIELD_SYSTEM_ID         1
#define FIELD_SENSOR_NAME_PATH  4
#define FIELD_TIMESTAMP         6
#define FIELD_ENTERPRISE        101
#define FIELD_JUNIPER_NETWORKS  2636
#define MAX_FIELD_DEPTH         16
#define RECORD_QUEUE_SIZE       8192
#define MAX_DEVICE_NAME         256
#define MAX_SENSOR_NAME         128

/* ── Lookup table types ───────────────────────────────────────────────────── */

typedef struct {
    char   *data;       /* msgpack-encoded record (malloc'd) */
    size_t  size;
    uint32_t ts_sec;
} jti_record_t;

typedef struct {
    jti_record_t  records[RECORD_QUEUE_SIZE];
    int           head;
    int           tail;
    int           count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
} jti_queue_t;

static void queue_init(jti_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

static int queue_push(jti_queue_t *q, char *data, size_t size, uint32_t ts_sec)
{
    pthread_mutex_lock(&q->lock);
    if (q->count >= RECORD_QUEUE_SIZE) {
        pthread_mutex_unlock(&q->lock);
        return -1; /* full — drop */
    }
    q->records[q->tail].data   = data;
    q->records[q->tail].size   = size;
    q->records[q->tail].ts_sec = ts_sec;
    q->tail = (q->tail + 1) % RECORD_QUEUE_SIZE;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    return 0;
}

static int queue_pop(jti_queue_t *q, jti_record_t *out)
{
    pthread_mutex_lock(&q->lock);
    if (q->count == 0) {
        pthread_mutex_unlock(&q->lock);
        return -1;
    }
    *out = q->records[q->head];
    q->head = (q->head + 1) % RECORD_QUEUE_SIZE;
    q->count--;
    pthread_mutex_unlock(&q->lock);
    return 0;
}

/* ── Global context pointer (for FLBPluginInputCallback without ctx arg) ──── */
/* Fluent Bit v5 may call either FLBPluginInputCallback or                    */
/* FLBPluginInputCallbackCtx depending on build flags. We export both.        */
static void *g_ctx = NULL;

/* ── Plugin context ───────────────────────────────────────────────────────── */

typedef struct {
    char         listen[64];
    int          port;
    int          buf_size;
    int          debug;
    int          sock_fd;
    pthread_t    listener_thread;
    jti_queue_t  queue;
    /* stats */
    uint64_t     stat_packets;
    uint64_t     stat_records;
    uint64_t     stat_dropped;
    uint64_t     stat_errors;
    char         hostname[256];
} jti_ctx_t;

/* ── Debug logging ────────────────────────────────────────────────────────── */

static void jti_log(jti_ctx_t *ctx, const char *fmt, ...)
{
    if (!ctx || !ctx->debug) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[jti_nus] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static void jti_log_always(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[jti_nus] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ── Protobuf wire-format decoder ────────────────────────────────────────── */

/* Read a varint from buf[*pos], advancing *pos.
 * Returns 0 on success, -1 on overflow/truncation. */
static int pb_read_varint(const uint8_t *buf, size_t len,
                          size_t *pos, uint64_t *out)
{
    uint64_t val = 0;
    int shift = 0;
    while (*pos < len) {
        uint8_t b = buf[(*pos)++];
        val |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) { *out = val; return 0; }
        shift += 7;
        if (shift >= 64) return -1;
    }
    return -1;
}

/* Skip a field of given wire type. */
static int pb_skip(const uint8_t *buf, size_t len, size_t *pos, int wire)
{
    uint64_t v;
    switch (wire) {
    case PB_WIRE_VARINT:
        return pb_read_varint(buf, len, pos, &v);
    case PB_WIRE_64BIT:
        if (*pos + 8 > len) return -1;
        *pos += 8; return 0;
    case PB_WIRE_LEN:
        if (pb_read_varint(buf, len, pos, &v) < 0) return -1;
        if (*pos + v > len) return -1;
        *pos += (size_t)v; return 0;
    case PB_WIRE_32BIT:
        if (*pos + 4 > len) return -1;
        *pos += 4; return 0;
    default:
        return -1;
    }
}

/* ── Flat record ─────────────────────────────────────────────────────────── */


void flat_set(flat_record_t *r, const char *key, const char *val)
{
    if (r->count >= MAX_RECORD_FIELDS) return;
    snprintf(r->fields[r->count].key,  MAX_FIELD_NAME,  "%s", key);
    snprintf(r->fields[r->count].sval, MAX_FIELD_VALUE, "%s", val);
    r->fields[r->count].type = FT_STRING;
    r->count++;
}

void flat_set_uint(flat_record_t *r, const char *key, uint64_t val)
{
    if (r->count >= MAX_RECORD_FIELDS) return;
    snprintf(r->fields[r->count].key, MAX_FIELD_NAME, "%s", key);
    r->fields[r->count].uval = val;
    r->fields[r->count].type = FT_UINT64;
    r->count++;
}

void flat_set_double(flat_record_t *r, const char *key, double val)
{
    if (r->count >= MAX_RECORD_FIELDS) return;
    snprintf(r->fields[r->count].key, MAX_FIELD_NAME, "%s", key);
    r->fields[r->count].dval = val;
    r->fields[r->count].type = FT_DOUBLE;
    r->count++;
}

/*
 * walk_message — recursively walk a protobuf message, adding fields to `base`.
 * Repeated sub-messages each get their own flat_record (cloned from base).
 * Scalar fields are added to base directly.
 *
 * ext_num: the sensor extension number (for field name lookup)
 * depth:   recursion guard
 */
/* ── MsgPack encoder ─────────────────────────────────────────────────────── */

/*
 * encode_record — encode a flat_record_t as a Fluent Bit MsgPack record.
 *
 * Fluent Bit expects: [ timestamp_uint32, { key: value, ... } ]
 */
static char *encode_record(flat_record_t *r, uint32_t ts_sec, size_t *out_size)
{
    msgpack_sbuffer sbuf;
    msgpack_packer  pk;
    msgpack_sbuffer_init(&sbuf);
    msgpack_packer_init(&pk, &sbuf, msgpack_sbuffer_write);

    /* outer array: [ts, map] */
    msgpack_pack_array(&pk, 2);
    msgpack_pack_uint32(&pk, ts_sec);

    /* map: count = record fields */
    msgpack_pack_map(&pk, r->count);
    for (int i = 0; i < r->count; i++) {
        size_t klen = strlen(r->fields[i].key);
        msgpack_pack_str(&pk, klen);
        msgpack_pack_str_body(&pk, r->fields[i].key, klen);
        switch (r->fields[i].type) {
        case FT_UINT64:
            msgpack_pack_uint64(&pk, r->fields[i].uval);
            break;
        case FT_DOUBLE:
            msgpack_pack_double(&pk, r->fields[i].dval);
            break;
        case FT_STRING:
        default: {
            size_t vlen = strlen(r->fields[i].sval);
            msgpack_pack_str(&pk, vlen);
            msgpack_pack_str_body(&pk, r->fields[i].sval, vlen);
            break;
        }
        }
    }

    /* Transfer buffer ownership to caller */
    char *data = (char *)malloc(sbuf.size);
    if (!data) { msgpack_sbuffer_destroy(&sbuf); return NULL; }
    memcpy(data, sbuf.data, sbuf.size);
    *out_size = sbuf.size;
    msgpack_sbuffer_destroy(&sbuf);
    return data;
}

/* ── Packet parser ────────────────────────────────────────────────────────── */

#define MAX_EXTRAS_PER_PACKET 4096

static int parse_packet(jti_ctx_t *ctx,
                        const uint8_t *pkt, size_t pkt_len,
                        const char *src_addr)
{
    /* ── Step 1: parse TelemetryStream outer envelope ── */
    char     device[MAX_DEVICE_NAME] = "";
    uint64_t timestamp_ms = 0;

    /* Locate enterprise field (101) and JuniperNetworksSensors (2636) */
    const uint8_t *enterprise_buf = NULL;
    size_t         enterprise_len = 0;

    size_t pos = 0;
    while (pos < pkt_len) {
        uint64_t tag_val;
        if (pb_read_varint(pkt, pkt_len, &pos, &tag_val) < 0) break;
        uint32_t fnum  = (uint32_t)(tag_val >> 3);
        int      wtype = (int)(tag_val & 0x7);

        if (wtype == PB_WIRE_VARINT) {
            uint64_t v;
            if (pb_read_varint(pkt, pkt_len, &pos, &v) < 0) break;
            if (fnum == FIELD_TIMESTAMP)
                timestamp_ms = v;
        } else if (wtype == PB_WIRE_LEN) {
            uint64_t flen;
            if (pb_read_varint(pkt, pkt_len, &pos, &flen) < 0) break;
            if (pos + flen > pkt_len) break;
            if (fnum == FIELD_SYSTEM_ID) {
                size_t copy = flen < (MAX_DEVICE_NAME - 1) ? flen : MAX_DEVICE_NAME - 1;
                memcpy(device, pkt + pos, copy);
                device[copy] = '\0';
                /* Strip :port suffix */
                char *colon = strrchr(device, ':');
                if (colon) *colon = '\0';
            } else if (fnum == FIELD_ENTERPRISE) {
                enterprise_buf = pkt + pos;
                enterprise_len = (size_t)flen;
            }
            pos += (size_t)flen;
        } else {
            if (pb_skip(pkt, pkt_len, &pos, wtype) < 0) break;
        }
    }

    if (!enterprise_buf || enterprise_len == 0) {
        jti_log(ctx, "no enterprise field in packet from %s", src_addr);
        return -1;
    }

    /* ── Step 2: parse EnterpriseSensors → find field 2636 ── */
    const uint8_t *jnpr_buf = NULL;
    size_t         jnpr_len = 0;

    pos = 0;
    while (pos < enterprise_len) {
        uint64_t tag_val;
        if (pb_read_varint(enterprise_buf, enterprise_len, &pos, &tag_val) < 0) break;
        uint32_t fnum  = (uint32_t)(tag_val >> 3);
        int      wtype = (int)(tag_val & 0x7);
        if (wtype == PB_WIRE_LEN) {
            uint64_t flen;
            if (pb_read_varint(enterprise_buf, enterprise_len, &pos, &flen) < 0) break;
            if (pos + flen > enterprise_len) break;
            if (fnum == FIELD_JUNIPER_NETWORKS) {
                jnpr_buf = enterprise_buf + pos;
                jnpr_len = (size_t)flen;
            }
            pos += (size_t)flen;
        } else {
            if (pb_skip(enterprise_buf, enterprise_len, &pos, wtype) < 0) break;
        }
    }

    if (!jnpr_buf || jnpr_len == 0) {
        jti_log(ctx, "no JuniperNetworksSensors in packet from %s", src_addr);
        return -1;
    }

    /* ── Step 3: parse JuniperNetworksSensors → find sensor extension ── */
    uint32_t       ext_num     = 0;
    const uint8_t *sensor_buf  = NULL;
    size_t         sensor_len  = 0;

    pos = 0;
    while (pos < jnpr_len) {
        uint64_t tag_val;
        if (pb_read_varint(jnpr_buf, jnpr_len, &pos, &tag_val) < 0) break;
        uint32_t fnum  = (uint32_t)(tag_val >> 3);
        int      wtype = (int)(tag_val & 0x7);
        if (wtype == PB_WIRE_LEN) {
            uint64_t flen;
            if (pb_read_varint(jnpr_buf, jnpr_len, &pos, &flen) < 0) break;
            if (pos + flen > jnpr_len) break;
            /* Any length-delimited field here is the sensor message */
            ext_num    = fnum;
            sensor_buf = jnpr_buf + pos;
            sensor_len = (size_t)flen;
            pos += (size_t)flen;
            break; /* only one sensor per packet */
        } else {
            if (pb_skip(jnpr_buf, jnpr_len, &pos, wtype) < 0) break;
        }
    }

    if (!sensor_buf || sensor_len == 0 || ext_num == 0) {
        jti_log(ctx, "no sensor extension in packet from %s", src_addr);
        return -1;
    }

    /* ── Step 4: look up dispatch entry (sensor name + descriptor) ── */
    const jti_dispatch_entry_t *entry = jti_dispatch_lookup(ext_num);
    char sensor_name_buf[MAX_SENSOR_NAME];
    if (entry) {
        snprintf(sensor_name_buf, MAX_SENSOR_NAME, "%s", entry->sensor_name);
    } else {
        snprintf(sensor_name_buf, MAX_SENSOR_NAME, "sensor_%u", ext_num);
    }

    uint32_t ts_sec = (uint32_t)(timestamp_ms / 1000);

    jti_log(ctx, "packet from %s → device=%s sensor=%s ext=%u",
            src_addr, device, sensor_name_buf, ext_num);

    /* ── Step 5: unpack + walk sensor message via protobuf-c descriptor ── */
    flat_record_t  base;
    memset(&base, 0, sizeof(base));

    flat_record_t **extras = (flat_record_t **)malloc(
        MAX_EXTRAS_PER_PACKET * sizeof(flat_record_t *));
    int nextra = 0;

    if (!extras) return -1;

    if (entry) {
        if (jti_walk_sensor(sensor_buf, sensor_len, entry,
                            &base, extras, &nextra,
                            MAX_EXTRAS_PER_PACKET) < 0) {
            jti_log(ctx, "  WARNING: protobuf unpack failed sensor=%s ext=%u",
                    sensor_name_buf, ext_num);
        }
    } else {
        jti_log(ctx, "  unknown sensor ext=%u (no descriptor)", ext_num);
    }

    /* ── Step 6: stamp metadata on base and all extras, encode, queue ── */
    int records_emitted = 0;
    char ts_str[32], time_ms_str[32];
    snprintf(ts_str,    sizeof(ts_str),    "%u", ts_sec);
    snprintf(time_ms_str, sizeof(time_ms_str), "%llu",
             (unsigned long long)timestamp_ms);

    /* Stamp base */
    /* Only emit the base record if the top-level walk produced sensor fields.
     * When all data is in repeated sub-messages (e.g. interface_list),
     * the base record will be empty and should not be emitted as a stub. */
    int base_has_fields = (base.count > 0);

    flat_set(&base, "device",      device);
    flat_set(&base, "source",      src_addr);
    flat_set(&base, "sensor_name", sensor_name_buf);
    flat_set_uint(&base, "jti_timestamp", timestamp_ms);

    size_t sz;
    char *data = NULL;
    if (base_has_fields) {
        data = encode_record(&base, ts_sec, &sz);
        if (data) {
            if (queue_push(&ctx->queue, data, sz, ts_sec) < 0) {
                free(data);
                ctx->stat_dropped++;
                jti_log_always("WARNING: record queue full, dropping (device=%s sensor=%s)",
                               device, sensor_name_buf);
            } else {
                records_emitted++;
            }
        }
    }

    /* Stamp and queue extras */
    for (int i = 0; i < nextra; i++) {
        flat_set(extras[i], "device",      device);
        flat_set(extras[i], "source",      src_addr);
        flat_set(extras[i], "sensor_name", sensor_name_buf);
        flat_set_uint(extras[i], "jti_timestamp", timestamp_ms);

        data = encode_record(extras[i], ts_sec, &sz);
        if (data) {
            if (queue_push(&ctx->queue, data, sz, ts_sec) < 0) {
                free(data);
                ctx->stat_dropped++;
            } else {
                records_emitted++;
            }
        }
        free(extras[i]);
    }
    free(extras);

    jti_log(ctx, "  → %d records queued", records_emitted);
    ctx->stat_records += records_emitted;
    return records_emitted;
}

/* ── UDP listener thread ──────────────────────────────────────────────────── */

static void *udp_listener(void *arg)
{
    jti_ctx_t *ctx = (jti_ctx_t *)arg;
    uint8_t *buf   = (uint8_t *)malloc(ctx->buf_size);
    if (!buf) {
        jti_log_always("ERROR: failed to allocate receive buffer");
        return NULL;
    }

    struct sockaddr_storage src_addr;
    socklen_t src_len;
    char src_str[INET6_ADDRSTRLEN + 8];
    time_t last_stats = time(NULL);

    jti_log_always("listener thread started on UDP %s:%d buffer=%d bytes debug=%s",
                   ctx->listen, ctx->port, ctx->buf_size,
                   ctx->debug ? "on" : "off");

    while (1) {
        src_len = sizeof(src_addr);
        ssize_t n = recvfrom(ctx->sock_fd, buf, ctx->buf_size, 0,
                             (struct sockaddr *)&src_addr, &src_len);
        if (n < 0) {
            if (errno == EINTR) continue;
            jti_log_always("recvfrom error: %s", strerror(errno));
            break;
        }

        /* Format source address string */
        if (src_addr.ss_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)&src_addr;
            inet_ntop(AF_INET, &s->sin_addr, src_str, sizeof(src_str));
        } else {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)&src_addr;
            inet_ntop(AF_INET6, &s->sin6_addr, src_str, sizeof(src_str));
        }

        jti_log(ctx, "packet from %s size=%zd bytes", src_str, n);
        ctx->stat_packets++;

        if (parse_packet(ctx, buf, (size_t)n, src_str) < 0)
            ctx->stat_errors++;

        /* Periodic stats (debug only) */
        if (ctx->debug) {
            time_t now = time(NULL);
            if (now - last_stats >= 60) {
                jti_log_always("stats: packets=%llu records=%llu "
                               "dropped=%llu errors=%llu queue=%d",
                               (unsigned long long)ctx->stat_packets,
                               (unsigned long long)ctx->stat_records,
                               (unsigned long long)ctx->stat_dropped,
                               (unsigned long long)ctx->stat_errors,
                               ctx->queue.count);
                last_stats = now;
            }
        }
    }

    free(buf);
    return NULL;
}

/* ── Fluent Bit plugin entrypoints ────────────────────────────────────────── */

int FLBPluginRegister(void *def)
{
    struct flb_plugin_proxy_def *p = (struct flb_plugin_proxy_def *)def;
    p->type        = FLB_PROXY_INPUT_PLUGIN;
    p->proxy       = FLB_PROXY_EXTERNAL; /* external .so plugin proxy type */
    p->flags       = 0;
    p->name        = strdup("jti_nus");
    p->description = strdup("Juniper JTI Native UDP Telemetry (C)");
    p->event_type  = 0;
    return FLB_OK;
}

int FLBPluginInit(void *plugin)
{
    jti_ctx_t *ctx = (jti_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) return FLB_ERROR;

    /* Read config */
    char *listen    = get_config_key(plugin, "Listen");
    char *port_s    = get_config_key(plugin, "Port");
    char *buf_s     = get_config_key(plugin, "Buffer_Size");
    char *debug_s   = get_config_key(plugin, "Debug");

    snprintf(ctx->listen, sizeof(ctx->listen), "%s",
             (listen && *listen) ? listen : "0.0.0.0");
    ctx->port     = (port_s  && *port_s)  ? atoi(port_s)  : 4729;
    ctx->buf_size = (buf_s   && *buf_s)   ? atoi(buf_s)   : 65535;
    ctx->debug    = (debug_s && *debug_s) &&
                    (strcasecmp(debug_s, "on")   == 0 ||
                     strcasecmp(debug_s, "true") == 0 ||
                     strcmp(debug_s, "1")        == 0);

    gethostname(ctx->hostname, sizeof(ctx->hostname) - 1);
    queue_init(&ctx->queue);

    /* Bind UDP socket */
    struct addrinfo hints = {0}, *res;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags    = AI_PASSIVE;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", ctx->port);

    if (getaddrinfo(ctx->listen, port_str, &hints, &res) != 0) {
        jti_log_always("ERROR: getaddrinfo failed for %s:%d", ctx->listen, ctx->port);
        free(ctx); return FLB_ERROR;
    }

    ctx->sock_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (ctx->sock_fd < 0) {
        jti_log_always("ERROR: socket() failed: %s", strerror(errno));
        freeaddrinfo(res); free(ctx); return FLB_ERROR;
    }

    int reuse = 1;
    setsockopt(ctx->sock_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    if (bind(ctx->sock_fd, res->ai_addr, res->ai_addrlen) < 0) {
        jti_log_always("ERROR: bind failed %s:%d: %s",
                       ctx->listen, ctx->port, strerror(errno));
        freeaddrinfo(res); close(ctx->sock_fd); free(ctx);
        return FLB_ERROR;
    }
    freeaddrinfo(res);

    /* Start listener thread */
    if (pthread_create(&ctx->listener_thread, NULL, udp_listener, ctx) != 0) {
        jti_log_always("ERROR: pthread_create failed: %s", strerror(errno));
        close(ctx->sock_fd); free(ctx); return FLB_ERROR;
    }
    pthread_detach(ctx->listener_thread);

    /* Store context — both in plugin proxy and in global for non-Ctx callback */
    struct flb_input_plugin_ctx *p = (struct flb_input_plugin_ctx *)plugin;
    p->context->remote_context = ctx;
    g_ctx = ctx;

    jti_log_always("listening on UDP %s:%d  buffer=%d  debug=%s",
                   ctx->listen, ctx->port, ctx->buf_size,
                   ctx->debug ? "on" : "off");
    if (ctx->debug)
        jti_log_always("debug mode enabled — packet-level logging active");

    return FLB_OK;
}

/* Forward declaration */
int FLBPluginInputCallbackCtx(void *remote_ctx, void **data, size_t *size);

/* FLBPluginInputCallback — called by Fluent Bit builds that don't pass context */
int FLBPluginInputCallback(void **data, size_t *size)
{
    return FLBPluginInputCallbackCtx(g_ctx, data, size);
}

/* FLBPluginInputCallbackCtx — called by Fluent Bit builds that pass context */
int FLBPluginInputCallbackCtx(void *remote_ctx, void **data, size_t *size)
{
    /* remote_ctx is flb_input_proxy_context* — extract our jti_ctx_t* from it */
    struct flb_input_proxy_context *proxy_ctx =
        (struct flb_input_proxy_context *)remote_ctx;
    jti_ctx_t *ctx = (proxy_ctx && proxy_ctx->remote_context)
                     ? (jti_ctx_t *)proxy_ctx->remote_context
                     : (jti_ctx_t *)g_ctx;
    if (!ctx) return FLB_OK;

    /* Drain up to 256 records per tick */
    const int MAX_PER_TICK = 256;
    char   *combined = NULL;
    size_t  total    = 0;
    int     count    = 0;

    jti_record_t rec;
    while (count < MAX_PER_TICK && queue_pop(&ctx->queue, &rec) == 0) {
        char *tmp = (char *)realloc(combined, total + rec.size);
        if (!tmp) { free(rec.data); break; }
        combined = tmp;
        memcpy(combined + total, rec.data, rec.size);
        total += rec.size;
        free(rec.data);
        count++;
    }

    if (!combined || total == 0) {
        free(combined);
        return FLB_OK;
    }

    jti_log(ctx, "flushing %d records to Fluent Bit pipeline (%zu bytes)",
            count, total);

    *data = combined;
    *size = total;
    return FLB_OK;
}

int FLBPluginInputCleanupCallback(void *data)
{
    free(data);
    return FLB_OK;
}

int FLBPluginInputCleanupCallbackCtx(void *remote_ctx, void *data)
{
    (void)remote_ctx;  /* remote_ctx is flb_input_proxy_context* — not needed here */
    free(data);
    return FLB_OK;
}

int FLBPluginExit(void)
{
    jti_log_always("shutting down");
    return FLB_OK;
}
