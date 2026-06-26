/* jti_walker.h — public API for the protobuf-c sensor walker */
#ifndef JTI_WALKER_H
#define JTI_WALKER_H

#include <stdint.h>
#include "jti_dispatch.h"

/*
 * flat_record_t and flat_set* are defined in jti_nus.c.
 * Declare them here so jti_walker.c can use them.
 */
#define MAX_RECORD_FIELDS 512
#define MAX_FIELD_NAME    128
#define MAX_FIELD_VALUE   4096

typedef enum { FT_STRING = 0, FT_UINT64, FT_DOUBLE } flat_type_t;

typedef struct {
    char        key[128];
    flat_type_t type;
    union {
        char     sval[4096];
        uint64_t uval;
        double   dval;
    };
} flat_field_t;


typedef struct {
    flat_field_t fields[MAX_RECORD_FIELDS];
    int          count;
} flat_record_t;

/* Defined in jti_nus.c */
void flat_set(flat_record_t *r, const char *key, const char *val);
void flat_set_uint(flat_record_t *r, const char *key, uint64_t val);
void flat_set_double(flat_record_t *r, const char *key, double val);

/*
 * jti_walk_sensor — unpack and walk a sensor message.
 *
 * sensor_buf/sensor_len: raw protobuf bytes of the sensor sub-message
 * entry:    dispatch table entry (has descriptor + sensor_name)
 * base:     pre-allocated base record (caller fills metadata after)
 * extras:   array of flat_record_t* for repeated top-level elements
 * nextra:   pointer to count of extras filled
 * max_extra: maximum extras to fill
 *
 * Returns 0 on success, -1 if unpack fails (unknown sensor).
 */
int jti_walk_sensor(const uint8_t              *sensor_buf,
                    size_t                      sensor_len,
                    const jti_dispatch_entry_t *entry,
                    flat_record_t              *base,
                    flat_record_t             **extras,
                    int                        *nextra,
                    int                         max_extra);

#endif /* JTI_WALKER_H */
