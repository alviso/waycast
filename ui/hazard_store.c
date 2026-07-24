#include "hazard_store.h"

#include <string.h>

static hazard_t store[HAZARD_MAX];

/* an ATTEST isn't stored — it updates its target's tallies (§7¾):
 * confirmations extend life (capped 2x base), denials shorten it */
static void apply_attest(const vmesh_msg_t *m)
{
    for (int i = 0; i < HAZARD_MAX; i++) {
        hazard_t *h = &store[i];
        if (!h->active || h->msg.origin_id != m->ref_origin ||
            h->msg.seq != m->ref_seq)
            continue;
        if (m->hazard_type == VMESH_VERDICT_CONFIRM) {
            if (h->confirms < 255) h->confirms++;
            uint32_t bump = h->msg.ttl_s / 4u;
            uint32_t cap = 65535u;
            h->msg.ttl_s = (uint16_t)((h->msg.ttl_s + bump > cap)
                                          ? cap
                                          : h->msg.ttl_s + bump);
        } else {
            if (h->denies < 255) h->denies++;
            uint16_t cut = (uint16_t)(h->msg.ttl_s / 4u);
            h->msg.ttl_s = (uint16_t)(h->msg.ttl_s > cut + 60
                                          ? h->msg.ttl_s - cut
                                          : 60);
        }
        return;
    }
}

hazard_t *hazard_store_ingest(const vmesh_msg_t *m)
{
    if (m->msg_type == VMESH_MT_ATTEST) {
        apply_attest(m);
        return NULL;
    }
    /* Q&A traffic is transient conversation, not corkboard content */
    if (m->msg_type == VMESH_MT_QUERY || m->msg_type == VMESH_MT_REPLY)
        return NULL;

    /* the store holds anything geo-ephemeral the UI shows: safety
     * hazards and LOCAL-channel notices alike (same dedup + expiry) */
    if (m->msg_type != VMESH_MT_HAZARD && m->channel != VMESH_CH_LOCAL)
        return NULL;

    int free_slot = -1;
    for (int i = 0; i < HAZARD_MAX; i++) {
        if (!store[i].active) {
            if (free_slot < 0) free_slot = i;
            continue;
        }
        /* dedup id = origin + seq (handoff-spec §2) */
        if (store[i].msg.origin_id == m->origin_id &&
            store[i].msg.seq == m->seq)
            return NULL;
    }
    if (free_slot < 0) return NULL;

    store[free_slot].active = true;
    store[free_slot].msg = *m;
    store[free_slot].chip = NULL;
    return &store[free_slot];
}

void hazard_store_prune(uint32_t now_s, void (*on_expire)(hazard_t *))
{
    for (int i = 0; i < HAZARD_MAX; i++) {
        hazard_t *h = &store[i];
        if (!h->active) continue;
        if (now_s >= h->msg.created_s + h->msg.ttl_s) {
            if (on_expire) on_expire(h);
            memset(h, 0, sizeof(*h));
        }
    }
}

void hazard_store_clear(void (*on_drop)(hazard_t *))
{
    for (int i = 0; i < HAZARD_MAX; i++) {
        hazard_t *h = &store[i];
        if (!h->active) continue;
        if (on_drop) on_drop(h);
        memset(h, 0, sizeof(*h));
    }
}

hazard_t *hazard_store_slot(int i)
{
    return (i >= 0 && i < HAZARD_MAX) ? &store[i] : NULL;
}

float hazard_age_frac(const hazard_t *h, uint32_t now_s)
{
    if (h->msg.ttl_s == 0) return 1.f;
    float f = (float)(now_s - h->msg.created_s) / (float)h->msg.ttl_s;
    return f < 0.f ? 0.f : (f > 1.f ? 1.f : f);
}
