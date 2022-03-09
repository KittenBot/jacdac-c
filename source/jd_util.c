// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "jd_protocol.h"

#define ALIGN(x) (((x) + 3) & ~3)

static uint32_t seed;

// https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function
uint32_t jd_hash_fnv1a(const void *data, unsigned len) {
    const uint8_t *d = (const uint8_t *)data;
    uint32_t h = 0x811c9dc5;
    while (len--)
        h = (h ^ *d++) * 0x1000193;
    return h;
}

__attribute__((weak)) void jd_panic(void) {
    hw_panic();
}

__attribute__((weak)) uint64_t jd_device_id(void) {
    return hw_device_id();
}

void jd_seed_random(uint32_t s) {
    seed = (seed * 0x1000193) ^ s;
}

uint32_t jd_random() {
    if (seed == 0)
        jd_seed_random(13);

    // xorshift algorithm
    uint32_t x = seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    seed = x;
    return x;
}

// return v +/- 25% or so
uint32_t jd_random_around(uint32_t v) {
    uint32_t mask = 0xfffffff;
    while (mask > v)
        mask >>= 1;
    return (v - (mask >> 1)) + (jd_random() & mask);
}

// https://wiki.nicksoft.info/mcu:pic16:crc-16:home
uint16_t jd_crc16(const void *data, uint32_t size) {
    const uint8_t *ptr = (const uint8_t *)data;
    uint16_t crc = 0xffff;
    while (size--) {
        uint8_t data = *ptr++;
        uint8_t x = (crc >> 8) ^ data;
        x ^= x >> 4;
        crc = (crc << 8) ^ (x << 12) ^ (x << 5) ^ x;
    }
    return crc;
}

void jd_compute_crc(jd_frame_t *frame) {
    frame->crc = jd_crc16((uint8_t *)frame + 2, JD_FRAME_SIZE(frame) - 2);
}

int jd_shift_frame(jd_frame_t *frame) {
    int psize = frame->size;
    jd_packet_t *pkt = (jd_packet_t *)frame;
    int oldsz = pkt->service_size + 4;
    if (ALIGN(oldsz) >= psize)
        return 0; // nothing to shift

    int ptr;
    if (frame->data[oldsz] == 0xff) {
        ptr = frame->data[oldsz + 1];
        if (ptr >= psize)
            return 0; // End-of-frame
        if (ptr <= oldsz) {
            return 0; // don't let it go back, must be some corruption
        }
    } else {
        ptr = ALIGN(oldsz);
    }

    // assume the first one got the ACK sorted
    frame->flags &= ~JD_FRAME_FLAG_ACK_REQUESTED;

    uint8_t *src = &frame->data[ptr];
    int newsz = *src + 4;
    if (ptr + newsz > psize) {
        return 0;
    }
    uint32_t *dst = (uint32_t *)frame->data;
    uint32_t *srcw = (uint32_t *)src;
    // don't trust memmove()
    for (int i = 0; i < newsz; i += 4)
        *dst++ = *srcw++;
    // store ptr
    ptr += ALIGN(newsz);
    frame->data[newsz] = 0xff;
    frame->data[newsz + 1] = ptr;

    return 1;
}

void jd_reset_frame(jd_frame_t *frame) {
    frame->size = 0x00;
}

void *jd_push_in_frame(jd_frame_t *frame, unsigned service_num, unsigned service_cmd,
                       unsigned service_size) {
    if (service_num >> 8)
        jd_panic();
    if (service_cmd >> 16)
        jd_panic();
    uint8_t *dst = frame->data + frame->size;
    unsigned szLeft = frame->data + JD_SERIAL_PAYLOAD_SIZE - dst;
    if (service_size + 4 > szLeft)
        return NULL;
    *dst++ = service_size;
    *dst++ = service_num;
    *dst++ = service_cmd & 0xff;
    *dst++ = service_cmd >> 8;
    frame->size += ALIGN(service_size + 4);
    return dst;
}

bool jd_should_sample(uint32_t *sample, uint32_t period) {
    if (in_future(*sample))
        return false;

    *sample += period;

    if (!in_future(*sample))
        // we lost some samples
        *sample = now + period;

    return true;
}

bool jd_should_sample_delay(uint32_t *sample, uint32_t period) {
    if (in_future(*sample))
        return false;

    *sample = now + period;

    return true;
}

void jd_to_hex(char *dst, const void *src, size_t len) {
    const char *hex = "0123456789abcdef";
    const uint8_t *p = src;
    for (size_t i = 0; i < len; ++i) {
        dst[i * 2] = hex[p[i] >> 4];
        dst[i * 2 + 1] = hex[p[i] & 0xf];
    }
    dst[len * 2 + 1] = 0;
}
