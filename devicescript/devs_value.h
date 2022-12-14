#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef union {
    double _f;
    uint64_t u64;
    struct {
        uint32_t mantisa32;
        uint32_t mantisa20 : 20;
        uint32_t exponent : 11;
        uint32_t sign : 1;
    };
    struct {
        int32_t val_int32;
        uint32_t exp_sign;
    };
} value_t;

#define DEVS_INT_TAG (0U - 1U)
#define DEVS_NAN_TAG 0x7ff80000
#define DEVS_HANDLE_TAG 0x7ff00000

static inline bool devs_is_tagged_int(value_t t) {
    return (t.exp_sign + 1) == 0;
}

static inline bool devs_is_nan(value_t t) {
    return t.exp_sign == DEVS_NAN_TAG;
}

static inline bool devs_is_handle(value_t t) {
    return (t.exp_sign >> (20 - 1)) == (0x7ff << 1);
}

// handle type is 20 bit
static inline int devs_handle_type(value_t t) {
    return devs_is_handle(t) ? (t.exp_sign << 12) >> 12 : 0;
}

static inline uint32_t devs_handle_value(value_t t) {
    return t.mantisa32;
}

#if JD_64
void *devs_handle_ptr_value(devs_ctx_t *ctx, value_t t);
#else
static inline void *devs_handle_ptr_value(devs_ctx_t *ctx, value_t t) {
    return (void *)t.mantisa32;
}
#endif

static inline value_t devs_value_from_handle(int type, uint32_t value) {
    value_t r;
    r.exp_sign = DEVS_HANDLE_TAG + type;
    r.mantisa32 = value;
    return r;
}

#define DEVS_HANDLE_GC_MASK 0x80
#define DEVS_HANDLE_IMG_MASK 0x40 // TODO remove this?

#define DEVS_HANDLE_TYPE_FLOAT64_OR_NULL 0x00
#define DEVS_HANDLE_TYPE_SPECIAL 0x01
#define DEVS_HANDLE_TYPE_FIBER 0x02
#define DEVS_HANDLE_TYPE_GC_OBJECT (DEVS_HANDLE_GC_MASK | 0x03)
#define DEVS_HANDLE_TYPE_IMG_BUFFERISH 0x04
#define DEVS_HANDLE_TYPE_ROLE 0x05
#define DEVS_HANDLE_TYPE_FUNCTION 0x06

#define DEVS_SPECIAL_F64_NULL 0 // has to be zero! NULL is represented as all zero
#define DEVS_SPECIAL_FALSE 1
#define DEVS_SPECIAL_TRUE 0x40
#define DEVS_SPECIAL_PKT_BUFFER 0x41

static inline bool devs_is_null(value_t t) {
    return t.u64 == 0;
}

static inline bool devs_bufferish_is_buffer(value_t v) {
    return (devs_handle_value(v) >> DEVS_STRIDX__SHIFT) == DEVS_STRIDX_BUFFER;
}

static inline bool devs_is_special(value_t t) {
    return devs_is_null(t) || (t.exp_sign == DEVS_HANDLE_TAG + DEVS_HANDLE_TYPE_SPECIAL);
}

bool devs_is_nullish(value_t t);
bool devs_is_number(value_t t);


value_t devs_value_from_double(double v);
value_t devs_value_from_int(int v);
value_t devs_value_from_bool(int v);
value_t devs_value_from_pointer(devs_ctx_t *ctx, int gc_tag, void *ptr);
static inline value_t devs_value_from_gc_obj(devs_ctx_t *ctx, void *ptr) {
    return devs_value_from_pointer(ctx, DEVS_HANDLE_TYPE_GC_OBJECT, ptr);
}

int32_t devs_value_to_int(value_t v);
double devs_value_to_double(value_t v);
bool devs_value_to_bool(value_t v);

bool devs_value_ieee_eq(devs_ctx_t *ctx, value_t a, value_t b);
bool devs_value_eq(devs_ctx_t *ctx, value_t a, value_t b);

// returns one of DEVS_OBJECT_TYPE_*
unsigned devs_value_typeof(devs_ctx_t *ctx, value_t v);

extern const value_t devs_zero;
extern const value_t devs_one;
extern const value_t devs_nan;
extern const value_t devs_int_min;
extern const value_t devs_max_int_1;
extern const value_t devs_null;
extern const value_t devs_true;
extern const value_t devs_false;
extern const value_t devs_pkt_buffer;

#define devs_void devs_null
#define devs_undefined devs_null

bool devs_is_buffer(devs_ctx_t *ctx, value_t v);
bool devs_buffer_is_writable(devs_ctx_t *ctx, value_t v);
void *devs_buffer_data(devs_ctx_t *ctx, value_t v, unsigned *sz);

// this returns NULL if v is neither buffer not string
const void *devs_bufferish_data(devs_ctx_t *ctx, value_t v, unsigned *sz);

bool devs_is_array(devs_ctx_t *ctx, value_t v);

bool devs_is_string(devs_ctx_t *ctx, value_t v);
value_t devs_string_concat(devs_ctx_t *ctx, value_t a, value_t b);
const char *devs_string_get_utf8(devs_ctx_t *ctx, value_t s, unsigned *size);
value_t devs_value_to_string(devs_ctx_t *ctx, value_t v);
value_t devs_string_vsprintf(devs_ctx_t *ctx, const char *format, va_list ap);
__attribute__((format(printf, 2, 3))) value_t devs_string_sprintf(devs_ctx_t *ctx,
                                                                  const char *format, ...);
value_t devs_string_from_utf8(devs_ctx_t *ctx, const uint8_t *utf8, unsigned len);

void devs_value_pin(devs_ctx_t *ctx, value_t v);
void devs_value_unpin(devs_ctx_t *ctx, value_t v);
