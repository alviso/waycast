#include "vmesh_wire.h"

#include <string.h>

/* explicit little-endian writes: identical bytes from the RISC-V P4,
 * a co-MCU, or the x86 simulator */
static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int vmesh_wire_encode(const vmesh_msg_t *m, uint8_t *buf, size_t cap)
{
    size_t note_len = strnlen(m->note, sizeof(m->note));
    if (note_len > VMESH_WIRE_NOTE_MAX) note_len = VMESH_WIRE_NOTE_MAX;
    size_t total = VMESH_WIRE_BASE + note_len;
    if (cap < total) return -1;

    buf[0] = m->version;
    buf[1] = m->msg_type;
    buf[2] = m->channel;
    buf[3] = m->hazard_type;
    buf[4] = m->severity;
    buf[5] = m->hops;
    wr32(buf + 6, m->origin_id);
    wr16(buf + 10, m->seq);
    if (VMESH_MT_HAS_REF(m->msg_type)) {
        /* no position of its own: slots carry the target reference */
        wr32(buf + 12, m->ref_origin);
        wr32(buf + 16, 0);
        wr32(buf + 20, m->created_s);
        wr16(buf + 24, m->ref_seq);
    } else {
        wr32(buf + 12, (uint32_t)m->lat_e7);
        wr32(buf + 16, (uint32_t)m->lon_e7);
        wr32(buf + 20, m->created_s);
        wr16(buf + 24, m->ttl_s);
    }
    wr16(buf + 26, m->radius_m_x10);
    buf[28] = (uint8_t)note_len;
    memcpy(buf + VMESH_WIRE_BASE, m->note, note_len);
    return (int)total;
}

int vmesh_wire_decode(const uint8_t *buf, size_t len, vmesh_msg_t *out)
{
    if (len < VMESH_WIRE_BASE) return -1;
    if (buf[0] != VMESH_PROTO_VERSION) return -2;

    uint8_t note_len = buf[28];
    if (note_len > VMESH_WIRE_NOTE_MAX ||
        len < (size_t)VMESH_WIRE_BASE + note_len)
        return -1;

    memset(out, 0, sizeof(*out));
    out->version      = buf[0];
    out->msg_type     = buf[1];
    out->channel      = buf[2];
    out->hazard_type  = buf[3];
    out->severity     = buf[4];
    out->hops         = buf[5];
    out->origin_id    = rd32(buf + 6);
    out->seq          = rd16(buf + 10);
    if (VMESH_MT_HAS_REF(out->msg_type)) {
        out->ref_origin = rd32(buf + 12);
        out->created_s  = rd32(buf + 20);
        out->ref_seq    = rd16(buf + 24);
    } else {
        out->lat_e7    = (int32_t)rd32(buf + 12);
        out->lon_e7    = (int32_t)rd32(buf + 16);
        out->created_s = rd32(buf + 20);
        out->ttl_s     = rd16(buf + 24);
    }
    out->radius_m_x10 = rd16(buf + 26);
    memcpy(out->note, buf + VMESH_WIRE_BASE, note_len);
    /* out->note is already NUL-terminated by the memset */
    return 0;
}
