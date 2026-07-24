/* round-trip tests for the OTA wire encoding — `make test` */
#include "vmesh_wire.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static vmesh_msg_t sample_hazard(void)
{
    vmesh_msg_t m = {0};
    m.version      = VMESH_PROTO_VERSION;
    m.msg_type     = VMESH_MT_HAZARD;
    m.channel      = VMESH_CH_SAFETY;
    m.hazard_type  = VMESH_HZ_CRASH;
    m.severity     = 3;
    m.hops         = 2;
    m.origin_id    = 0xDEADBEEF;
    m.seq          = 4242;
    m.lat_e7       = 373302000;   /* 37.3302 */
    m.lon_e7       = -1220352000; /* -122.0352 (negative survives) */
    m.created_s    = 1750000123;
    m.ttl_s        = 1800;
    m.radius_m_x10 = 500;
    snprintf(m.note, sizeof(m.note), "multi-vehicle, right lane");
    return m;
}

static void expect_roundtrip(const vmesh_msg_t *in)
{
    uint8_t buf[VMESH_WIRE_MAX];
    int n = vmesh_wire_encode(in, buf, sizeof(buf));
    assert(n >= VMESH_WIRE_BASE);

    vmesh_msg_t out;
    assert(vmesh_wire_decode(buf, (size_t)n, &out) == 0);

    assert(out.version == in->version);
    assert(out.msg_type == in->msg_type);
    assert(out.channel == in->channel);
    assert(out.hazard_type == in->hazard_type);
    assert(out.severity == in->severity);
    assert(out.hops == in->hops);
    assert(out.origin_id == in->origin_id);
    assert(out.seq == in->seq);
    assert(out.lat_e7 == in->lat_e7);
    assert(out.lon_e7 == in->lon_e7);
    assert(out.created_s == in->created_s);
    assert(out.ttl_s == in->ttl_s);
    assert(out.radius_m_x10 == in->radius_m_x10);
    assert(strcmp(out.note, in->note) == 0);
}

static void test_query_reply_roundtrip(void)
{
    vmesh_msg_t q = {0}, out;
    q.version = VMESH_PROTO_VERSION;
    q.msg_type = VMESH_MT_QUERY;
    q.channel = VMESH_CH_LOCAL;
    q.origin_id = 0xAABBCCDD;
    q.seq = 77;
    q.ref_origin = 0x11223344; /* the place asked */
    q.created_s = 1234567;
    snprintf(q.note, sizeof(q.note), "hours?");
    uint8_t buf[VMESH_WIRE_MAX];
    int n = vmesh_wire_encode(&q, buf, sizeof(buf));
    assert(n > 0);
    assert(vmesh_wire_decode(buf, (size_t)n, &out) == 0);
    assert(out.msg_type == VMESH_MT_QUERY);
    assert(out.ref_origin == 0x11223344);
    assert(strcmp(out.note, "hours?") == 0);

    vmesh_msg_t r = {0};
    r.version = VMESH_PROTO_VERSION;
    r.msg_type = VMESH_MT_REPLY;
    r.origin_id = 0x11223344;
    r.seq = 1001;
    r.ref_origin = q.origin_id;
    r.ref_seq = q.seq;
    snprintf(r.note, sizeof(r.note), "open till 18:00 today");
    n = vmesh_wire_encode(&r, buf, sizeof(buf));
    assert(n > 0);
    assert(vmesh_wire_decode(buf, (size_t)n, &out) == 0);
    assert(out.msg_type == VMESH_MT_REPLY);
    assert(out.ref_origin == q.origin_id && out.ref_seq == q.seq);
    assert(strcmp(out.note, "open till 18:00 today") == 0);
    printf("  query/reply roundtrip ok\n");
}

int main(void)
{
    /* 1. hazard with note round-trips */
    vmesh_msg_t m = sample_hazard();
    expect_roundtrip(&m);

    /* 2. LOCAL notice round-trips */
    m.channel = VMESH_CH_LOCAL;
    m.msg_type = VMESH_MT_TEXT;
    m.hazard_type = VMESH_LC_LODGING;
    m.ttl_s = 14400;
    snprintf(m.note, sizeof(m.note), "2 rooms - Cupertino Inn");
    expect_roundtrip(&m);

    /* 3. empty note -> exactly the base size */
    m.note[0] = 0;
    uint8_t buf[VMESH_WIRE_MAX];
    assert(vmesh_wire_encode(&m, buf, sizeof(buf)) == VMESH_WIRE_BASE);
    expect_roundtrip(&m);

    /* 4. encode refuses a too-small buffer */
    assert(vmesh_wire_encode(&m, buf, VMESH_WIRE_BASE - 1) == -1);

    /* 5. decode refuses a truncated frame */
    vmesh_msg_t out;
    assert(vmesh_wire_decode(buf, VMESH_WIRE_BASE - 1, &out) == -1);

    /* 6. decode refuses a lying note_len */
    m = sample_hazard();
    int n = vmesh_wire_encode(&m, buf, sizeof(buf));
    buf[28] = (uint8_t)(n - VMESH_WIRE_BASE + 1); /* claims 1 extra byte */
    assert(vmesh_wire_decode(buf, (size_t)n, &out) == -1);

    /* 7. decode rejects an unknown protocol version */
    n = vmesh_wire_encode(&m, buf, sizeof(buf));
    buf[0] = 0x7F;
    assert(vmesh_wire_decode(buf, (size_t)n, &out) == -2);

    /* 8. max-length note survives */
    memset(m.note, 'x', sizeof(m.note) - 1);
    m.note[sizeof(m.note) - 1] = 0;
    expect_roundtrip(&m);

    /* 9. ATTEST frames carry the target reference through the
     * repurposed position slots */
    memset(&m, 0, sizeof(m));
    m.version     = VMESH_PROTO_VERSION;
    m.msg_type    = VMESH_MT_ATTEST;
    m.hazard_type = VMESH_VERDICT_CONFIRM;
    m.origin_id   = 0x11223344;
    m.seq         = 7;
    m.ref_origin  = 0xCAFEBABE;
    m.ref_seq     = 4242;
    m.created_s   = 1750000456;
    n = vmesh_wire_encode(&m, buf, sizeof(buf));
    assert(n == VMESH_WIRE_BASE);
    assert(vmesh_wire_decode(buf, (size_t)n, &out) == 0);
    assert(out.msg_type == VMESH_MT_ATTEST);
    assert(out.ref_origin == 0xCAFEBABE);
    assert(out.ref_seq == 4242);
    assert(out.hazard_type == VMESH_VERDICT_CONFIRM);
    assert(out.origin_id == 0x11223344);
    test_query_reply_roundtrip();

    printf("test_wire: all 9 cases passed\n");
    return 0;
}
