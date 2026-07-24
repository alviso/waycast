/**
 * hazard_store.h — active-hazard state: dedup by (origin, seq), aging,
 * expiry pruning. Ephemerality lives here: expired means gone, no archive.
 */
#pragma once

#include "vmesh_msg.h"

#define HAZARD_MAX 64

typedef struct {
    bool        active;
    vmesh_msg_t msg;
    void       *chip;     /* lv_obj_t* owned by map_view */
    /* corroboration tallies (plan §7¾) — local, from heard ATTESTs */
    uint8_t     confirms;
    uint8_t     denies;
    int8_t      my_vote;  /* 0 none, +1 confirmed, -1 denied */
    bool        echoed;   /* own report: heard rebroadcast by a relay
                             ("✓ heard by town") — delivery receipt
                             without any ack protocol */
} hazard_t;

/* Returns the slot if ingested (new), NULL if duplicate/full/not a hazard. */
hazard_t *hazard_store_ingest(const vmesh_msg_t *m);

/* Remove expired hazards. map_view is told to drop chips via callback. */
void hazard_store_prune(uint32_t now_s, void (*on_expire)(hazard_t *));

/* Wipe every active hazard now (chips dropped via on_drop). Used when
 * demo mode toggles so scripted events don't linger on the real map. */
void hazard_store_clear(void (*on_drop)(hazard_t *));

hazard_t *hazard_store_slot(int i); /* i in [0, HAZARD_MAX); may be inactive */

/* 0.0 = just created, 1.0 = at expiry */
float hazard_age_frac(const hazard_t *h, uint32_t now_s);
