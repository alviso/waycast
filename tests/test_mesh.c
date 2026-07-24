/* unit tests for the geo-ephemeral flooding core — `make test` */
#include "vmesh_mesh.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int tx_count;
static vmesh_msg_t last_tx;

static void tx_cb(const vmesh_msg_t *m, void *user)
{
    (void)user;
    tx_count++;
    last_tx = *m;
}

static vmesh_msg_t mk(uint32_t origin, uint16_t seq)
{
    vmesh_msg_t m = {0};
    m.version = VMESH_PROTO_VERSION;
    m.msg_type = VMESH_MT_HAZARD;
    m.hazard_type = VMESH_HZ_DEBRIS;
    m.severity = 2;
    m.origin_id = origin;
    m.seq = seq;
    m.lat_e7 = 373300000;      /* 37.33 */
    m.lon_e7 = -1220350000;    /* -122.035 */
    m.created_s = 1000;
    m.ttl_s = 600;
    m.radius_m_x10 = 300;      /* 3 km */
    m.hops = 1;
    return m;
}

static const mesh_config_t CFG = {
    .jitter_min_ms = 100, .jitter_max_ms = 400, .suppress_threshold = 2,
};

int main(void)
{
    vmesh_mesh_t mesh;
    /* we are ~1.1 km from the origin: inside the 3 km radius */
    double lat = 37.34, lon = -122.035;

    /* 1. fresh message: delivered, relayed exactly once after jitter */
    mesh_init(&mesh, &CFG, 7);
    tx_count = 0;
    vmesh_msg_t m = mk(0xAAAA0001, 1);
    assert(mesh_rx(&mesh, &m, 0.5f, lat, lon, 0, 1010) == true);
    mesh_tick(&mesh, 50, tx_cb, 0);
    assert(tx_count == 0); /* still inside jitter */
    mesh_tick(&mesh, 500, tx_cb, 0);
    assert(tx_count == 1);
    assert(last_tx.hops == 2); /* hop incremented on relay */
    mesh_tick(&mesh, 2000, tx_cb, 0);
    assert(tx_count == 1); /* ...and never again */

    /* 2. duplicate: dropped, not delivered */
    assert(mesh_rx(&mesh, &m, 0.5f, lat, lon, 600, 1010) == false);
    assert(mesh.stats.dup_dropped == 1);

    /* 3. expired on arrival: dropped */
    vmesh_msg_t old = mk(0xAAAA0002, 1);
    assert(mesh_rx(&mesh, &old, 0.5f, lat, lon, 700, 1000 + 601) == false);
    assert(mesh.stats.expired_drop == 1);

    /* 4. outside relevance radius: neither delivered nor relayed */
    vmesh_msg_t far = mk(0xAAAA0003, 1);
    assert(mesh_rx(&mesh, &far, 0.5f, 37.40, -122.035, 800, 1010) == false);
    assert(mesh.stats.radius_drop == 1); /* ~7.7 km > 3 km */

    /* 5. suppression: overhear 2 duplicates while pending -> cancel */
    mesh_init(&mesh, &CFG, 7);
    tx_count = 0;
    vmesh_msg_t s = mk(0xBBBB0001, 9);
    assert(mesh_rx(&mesh, &s, 0.5f, lat, lon, 0, 1010) == true);
    assert(mesh_rx(&mesh, &s, 0.5f, lat, lon, 20, 1010) == false); /* dup 1 */
    assert(mesh_rx(&mesh, &s, 0.5f, lat, lon, 40, 1010) == false); /* dup 2 */
    mesh_tick(&mesh, 1000, tx_cb, 0);
    assert(tx_count == 0);
    assert(mesh.stats.suppressed == 1);

    /* 6. beacons deliver but never relay */
    mesh_init(&mesh, &CFG, 7);
    tx_count = 0;
    vmesh_msg_t b = mk(0xCCCC0001, 3);
    b.msg_type = VMESH_MT_BEACON;
    assert(mesh_rx(&mesh, &b, 0.5f, lat, lon, 0, 1010) == true);
    mesh_tick(&mesh, 1000, tx_cb, 0);
    assert(tx_count == 0);

    /* 7. hop cap */
    vmesh_msg_t h = mk(0xDDDD0001, 4);
    h.hops = MESH_MAX_HOPS + 1;
    assert(mesh_rx(&mesh, &h, 0.5f, lat, lon, 0, 1010) == false);
    assert(mesh.stats.hop_drop == 1);

    /* 8. own message echoed back is not re-relayed */
    mesh_init(&mesh, &CFG, 7);
    tx_count = 0;
    vmesh_msg_t own = mk(0xEEEE0001, 5);
    mesh_note_own(&mesh, &own);
    assert(mesh_rx(&mesh, &own, 0.5f, lat, lon, 100, 1010) == false);
    mesh_tick(&mesh, 1000, tx_cb, 0);
    assert(tx_count == 0);

    /* 9. ATTEST (no position) relays on the hop cap alone */
    mesh_init(&mesh, &CFG, 7);
    tx_count = 0;
    vmesh_msg_t a = {0};
    a.msg_type = VMESH_MT_ATTEST;
    a.origin_id = 0xFFFF0001;
    a.seq = 6;
    a.hazard_type = VMESH_VERDICT_CONFIRM;
    a.ref_origin = 0xAAAA0001;
    a.ref_seq = 1;
    a.created_s = 1000;
    assert(mesh_rx(&mesh, &a, 0.5f, lat, lon, 0, 1010) == true);
    mesh_tick(&mesh, 1000, tx_cb, 0);
    assert(tx_count == 1);

    printf("test_mesh: all 9 cases passed\n");
    return 0;
}
