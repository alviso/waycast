#include "vmesh_mesh.h"

#include <math.h>
#include <string.h>

/* xorshift32 — deterministic, no libc rand */
static uint32_t rng_next(vmesh_mesh_t *m)
{
    uint32_t x = m->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return m->rng = x;
}

static uint64_t msg_id(const vmesh_msg_t *msg)
{
    return ((uint64_t)msg->origin_id << 16) | msg->seq;
}

static bool seen_check_add(vmesh_mesh_t *m, const vmesh_msg_t *msg)
{
    uint64_t id = msg_id(msg);
    for (int i = 0; i < MESH_SEEN_SIZE; i++)
        if (m->seen_id[i] == id) return true;
    m->seen_id[m->seen_next] = id;
    m->seen_next = (m->seen_next + 1) % MESH_SEEN_SIZE;
    return false;
}

static double dist_m(double lat1, double lon1, double lat2, double lon2)
{
    double dy = (lat2 - lat1) * 110540.0;
    double dx = (lon2 - lon1) * 111320.0 * cos(lat1 * M_PI / 180.0);
    return sqrt(dx * dx + dy * dy);
}

void mesh_init(vmesh_mesh_t *m, const mesh_config_t *cfg, uint32_t seed)
{
    memset(m, 0, sizeof(*m));
    m->cfg = *cfg;
    if (m->cfg.jitter_max_ms <= m->cfg.jitter_min_ms)
        m->cfg.jitter_max_ms = m->cfg.jitter_min_ms + 1;
    m->rng = seed ? seed : 0xC0FFEE42;
}

void mesh_note_own(vmesh_mesh_t *m, const vmesh_msg_t *msg)
{
    (void)seen_check_add(m, msg);
}

void vmesh_clock_normalize(vmesh_msg_t *m, uint32_t now_s)
{
    if (m->created_s > now_s + VMESH_CLOCK_SKEW_S ||
        m->created_s + VMESH_CLOCK_SKEW_S < now_s)
        m->created_s = now_s;
}

bool mesh_rx(vmesh_mesh_t *m, const vmesh_msg_t *msg, float proximity,
             double my_lat, double my_lon, uint32_t now_ms,
             uint32_t now_unix_s)
{
    if (proximity < 0.f) proximity = 0.f;
    if (proximity > 1.f) proximity = 1.f;
    m->stats.rx_total++;

    /* 1. dedup — also bump suppression counters on pending relays */
    uint64_t id = msg_id(msg);
    bool dup = false;
    for (int i = 0; i < MESH_SEEN_SIZE; i++)
        if (m->seen_id[i] == id) { dup = true; break; }
    if (dup) {
        for (int i = 0; i < MESH_PENDING_SIZE; i++) {
            mesh_pending_t *p = &m->pending[i];
            if (p->used && msg_id(&p->msg) == id && p->overheard < 255)
                p->overheard++;
        }
        m->stats.dup_dropped++;
        return false;
    }

    /* 2. freshness */
    if (msg->ttl_s > 0 &&
        now_unix_s >= msg->created_s + msg->ttl_s) {
        m->stats.expired_drop++;
        return false;
    }

    /* hop safety valve (spec §2's TTL-limit alternative, kept as a
     * backstop even with the radius rule) */
    if (msg->hops > MESH_MAX_HOPS) {
        m->stats.hop_drop++;
        return false;
    }

    /* 3. relevance radius — position-carrying messages only.
     * ATTEST frames have no position (they reference one); they ride
     * on the hop cap alone. Beacons are single-hop by design. */
    bool has_position = !VMESH_MT_HAS_REF(msg->msg_type);
    if (has_position && msg->radius_m_x10 > 0) {
        double d = dist_m(my_lat, my_lon,
                          msg->lat_e7 / 1e7, msg->lon_e7 / 1e7);
        if (d > (double)msg->radius_m_x10 * 10.0) {
            m->stats.radius_drop++;
            return false; /* not for here: neither shown nor relayed.
                           * NOT cached as seen — driving into its radius
                           * later must still deliver it. */
        }
    }

    /* commit to it: cache now (after the drop gates) so out-of-radius
     * and stale frames don't poison dedup for a later, relevant pass */
    (void)seen_check_add(m, msg);
    m->stats.delivered++;

    /* 4. schedule the single rebroadcast (beacons never relay) */
    if (msg->msg_type == VMESH_MT_BEACON) return true;

    for (int i = 0; i < MESH_PENDING_SIZE; i++) {
        mesh_pending_t *p = &m->pending[i];
        if (p->used) continue;
        p->used = true;
        p->overheard = 0;
        p->msg = *msg;
        p->msg.hops++;
        /* contention-based forwarding: the base delay grows with
         * sender proximity (far receivers go first — max progress),
         * plus a small random spread to break ties among equals */
        uint32_t span = m->cfg.jitter_max_ms - m->cfg.jitter_min_ms;
        uint32_t base = (uint32_t)(proximity * span * 0.75f);
        uint32_t spread = span / 4 > 0 ? span / 4 : 1;
        p->due_ms = now_ms + m->cfg.jitter_min_ms + base +
                    rng_next(m) % spread;
        break;
        /* pending full: silently skip the relay — the message was
         * still delivered locally, and in a crowd someone else will
         * carry it (that's the §8 point) */
    }
    return true;
}

void mesh_tick(vmesh_mesh_t *m, uint32_t now_ms,
               mesh_tx_cb_t tx, void *user)
{
    for (int i = 0; i < MESH_PENDING_SIZE; i++) {
        mesh_pending_t *p = &m->pending[i];
        if (!p->used) continue;

        if (m->cfg.suppress_threshold &&
            p->overheard >= m->cfg.suppress_threshold) {
            p->used = false; /* enough neighbors said it already */
            m->stats.suppressed++;
            continue;
        }
        if ((int32_t)(now_ms - p->due_ms) >= 0) {
            p->used = false;
            m->stats.relayed++;
            tx(&p->msg, user);
        }
    }
}
