/**
 * names.h — origin→handle cache from HELLO frames (§7¾ pseudonyms).
 * RAM only, LRU, entries expire after 24 h: the mesh forgets names it
 * stops hearing, exactly like it forgets reports.
 */
#pragma once

#include <stdint.h>

#define NAMES_TTL_S (24u * 3600u)

void names_note(uint32_t origin, const char *handle, uint32_t now_s);

/* cached handle, or NULL if unknown/expired */
const char *names_get(uint32_t origin, uint32_t now_s);
