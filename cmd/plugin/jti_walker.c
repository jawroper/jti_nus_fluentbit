/*
 * jti_walker.c — protobuf-c descriptor-driven sensor message walker
 *
 * Replaces the hand-written walk_message() / field_map lookup approach
 * with proper schema-aware decoding using generated protobuf-c descriptors.
 *
 * For each sensor packet:
 *   1. unpack the raw bytes using the top-level message descriptor
 *   2. walk the decoded message tree using field descriptors
 *   3. for repeated sub-message fields: emit one flat_record per element
 *   4. for scalar fields: emit typed values (string / uint64 / double)
 *
 * The flat_record_t interface is unchanged so encode_record() and
 * queue_push() in jti_nus.c need no modifications.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <protobuf-c/protobuf-c.h>

#include "jti_dispatch.h"
#include "jti_walker.h"

/* ── forward declaration ──────────────────────────────────────────────────── */
static void walk_pb(const ProtobufCMessage *msg,
                    flat_record_t          *rec,
                    flat_record_t         **extras,
                    int                   *nextra,
                    int                    max_extra,
                    int                    depth);

/* ── helpers ──────────────────────────────────────────────────────────────── */

#define MAX_WALK_DEPTH 8

/* Emit a scalar field value into rec */
static void emit_scalar(flat_record_t               *rec,
                        const ProtobufCFieldDescriptor *fd,
                        const void                  *elem)
{
    switch (fd->type) {
    case PROTOBUF_C_TYPE_STRING: {
        const char *s = *(const char **)elem;
        flat_set(rec, fd->name, s ? s : "");
        break;
    }
    case PROTOBUF_C_TYPE_BYTES: {
        /* Emit byte fields as hex string */
        const ProtobufCBinaryData *bd = (const ProtobufCBinaryData *)elem;
        char hex[256] = "";
        size_t i, n = bd->len < 64 ? bd->len : 64;
        for (i = 0; i < n; i++)
            snprintf(hex + i*2, sizeof(hex) - i*2, "%02x", bd->data[i]);
        flat_set(rec, fd->name, hex);
        break;
    }
    case PROTOBUF_C_TYPE_BOOL:
        flat_set_uint(rec, fd->name,
                      (uint64_t)*(const protobuf_c_boolean *)elem);
        break;
    case PROTOBUF_C_TYPE_UINT64:
    case PROTOBUF_C_TYPE_FIXED64:
        flat_set_uint(rec, fd->name, *(const uint64_t *)elem);
        break;
    case PROTOBUF_C_TYPE_INT64:
    case PROTOBUF_C_TYPE_SINT64:
    case PROTOBUF_C_TYPE_SFIXED64:
        flat_set_uint(rec, fd->name, (uint64_t)*(const int64_t *)elem);
        break;
    case PROTOBUF_C_TYPE_UINT32:
    case PROTOBUF_C_TYPE_FIXED32:
        flat_set_uint(rec, fd->name, (uint64_t)*(const uint32_t *)elem);
        break;
    case PROTOBUF_C_TYPE_INT32:
    case PROTOBUF_C_TYPE_SINT32:
    case PROTOBUF_C_TYPE_SFIXED32:
        flat_set_uint(rec, fd->name, (uint64_t)(uint32_t)*(const int32_t *)elem);
        break;
    case PROTOBUF_C_TYPE_FLOAT:
        flat_set_double(rec, fd->name, (double)*(const float *)elem);
        break;
    case PROTOBUF_C_TYPE_DOUBLE:
        flat_set_double(rec, fd->name, *(const double *)elem);
        break;
    case PROTOBUF_C_TYPE_ENUM:
        flat_set_uint(rec, fd->name, (uint64_t)*(const int32_t *)elem);
        break;
    default:
        break;
    }
}

/* Check if an optional scalar field is set */
static int scalar_is_set(const ProtobufCMessage          *msg,
                          const ProtobufCFieldDescriptor  *fd)
{
    if (fd->label != PROTOBUF_C_LABEL_OPTIONAL)
        return 1;  /* required fields are always present */
    /* Optional scalars have a has_<field> boolean at quantifier_offset */
    const char *base = (const char *)msg;
    return *(const protobuf_c_boolean *)(base + fd->quantifier_offset);
}

/*
 * walk_pb_field — process one field of a message.
 *
 * For scalar fields: emit directly into rec.
 * For message fields:
 *   - If depth == 0 (top-level repeated messages): each element becomes
 *     its own record (clone of rec added to extras).
 *   - Otherwise: recurse, flattening fields into the current rec.
 */
static void walk_pb_field(const ProtobufCMessage          *msg,
                           const ProtobufCFieldDescriptor  *fd,
                           flat_record_t                   *rec,
                           flat_record_t                  **extras,
                           int                            *nextra,
                           int                             max_extra,
                           int                             depth)
{
    const char *base = (const char *)msg;

    if (fd->label == PROTOBUF_C_LABEL_REPEATED) {
        size_t count = *(const size_t *)(base + fd->quantifier_offset);
        if (count == 0) return;

        if (fd->type == PROTOBUF_C_TYPE_MESSAGE) {
            ProtobufCMessage **arr =
                *(ProtobufCMessage ***)(base + fd->offset);

            for (size_t j = 0; j < count; j++) {
                if (!arr[j]) continue;

                if (depth == 0 && extras && nextra) {
                    /*
                     * Top-level repeated message → separate record.
                     * Clone rec (which has device/sensor metadata already
                     * added by the caller) and walk into the clone.
                     */
                    if (*nextra >= max_extra) break;
                    flat_record_t *clone = calloc(1, sizeof(flat_record_t));
                    if (!clone) break;
                    *clone = *rec;  /* copy metadata fields */
                    walk_pb(arr[j], clone, NULL, NULL, 0, depth + 1);
                    extras[(*nextra)++] = clone;
                } else {
                    /* Nested repeated → flatten into current record */
                    walk_pb(arr[j], rec, extras, nextra, max_extra,
                            depth + 1);
                }
            }
        } else {
            /* Repeated scalar — emit each element with [index] suffix */
            size_t elem_size;
            switch (fd->type) {
            case PROTOBUF_C_TYPE_UINT64:
            case PROTOBUF_C_TYPE_INT64:
            case PROTOBUF_C_TYPE_FIXED64:
            case PROTOBUF_C_TYPE_SFIXED64:
            case PROTOBUF_C_TYPE_SINT64:
            case PROTOBUF_C_TYPE_DOUBLE:
                elem_size = 8; break;
            case PROTOBUF_C_TYPE_STRING:
                elem_size = sizeof(char *); break;
            default:
                elem_size = 4; break;
            }
            void *arr = *(void **)(base + fd->offset);
            for (size_t j = 0; j < count && j < 16; j++) {
                char key[128];
                snprintf(key, sizeof(key), "%s_%zu", fd->name, j);
                /* Temporarily override name for emit */
                ProtobufCFieldDescriptor tmp = *fd;
                tmp.name = key;
                emit_scalar(rec, &tmp, (const char *)arr + j * elem_size);
            }
        }
    } else {
        /* Optional or required field */
        if (fd->type == PROTOBUF_C_TYPE_MESSAGE) {
            const ProtobufCMessage *sub =
                *(const ProtobufCMessage **)(base + fd->offset);
            if (sub) {
                walk_pb(sub, rec, extras, nextra, max_extra, depth + 1);
            }
        } else {
            /* Scalar */
            if (!scalar_is_set(msg, fd)) return;
            emit_scalar(rec, fd, base + fd->offset);
        }
    }
}

/*
 * walk_pb — recursively walk a decoded protobuf message,
 * emitting all fields into rec.
 */
static void walk_pb(const ProtobufCMessage *msg,
                    flat_record_t          *rec,
                    flat_record_t         **extras,
                    int                   *nextra,
                    int                    max_extra,
                    int                    depth)
{
    if (!msg || depth > MAX_WALK_DEPTH) return;
    const ProtobufCMessageDescriptor *desc = msg->descriptor;
    if (!desc) return;

    for (unsigned i = 0; i < desc->n_fields; i++) {
        walk_pb_field(msg, &desc->fields[i],
                      rec, extras, nextra, max_extra, depth);
    }
}

/* ── public API ───────────────────────────────────────────────────────────── */

int jti_walk_sensor(const uint8_t                *sensor_buf,
                    size_t                        sensor_len,
                    const jti_dispatch_entry_t   *entry,
                    flat_record_t                *base,
                    flat_record_t               **extras,
                    int                          *nextra,
                    int                           max_extra)
{
    if (!entry || !entry->descriptor) return -1;

    /* Use protobuf-c generated unpack */
    ProtobufCMessage *msg = (ProtobufCMessage *)
        protobuf_c_message_unpack(entry->descriptor, NULL,
                                  sensor_len, sensor_buf);
    if (!msg) return -1;

    /* Walk the decoded message */
    walk_pb(msg, base, extras, nextra, max_extra, 0);

    protobuf_c_message_free_unpacked(msg, NULL);
    return 0;
}
