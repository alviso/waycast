#include "names.h"

#include <stdio.h>
#include <string.h>

#define N 32

static struct {
    uint32_t origin;
    uint32_t heard_s;
    char     handle[16];
} tab[N];

void names_note(uint32_t origin, const char *handle, uint32_t now_s)
{
    if (!origin || !handle || !handle[0]) return;
    int free_i = -1, oldest = 0;
    for (int i = 0; i < N; i++) {
        if (tab[i].origin == origin) { free_i = i; break; }
        if (!tab[i].origin && free_i < 0) free_i = i;
        if (tab[i].heard_s < tab[oldest].heard_s) oldest = i;
    }
    int k = free_i >= 0 ? free_i : oldest; /* LRU eviction */
    tab[k].origin = origin;
    tab[k].heard_s = now_s;
    snprintf(tab[k].handle, sizeof(tab[k].handle), "%s", handle);
}

const char *names_get(uint32_t origin, uint32_t now_s)
{
    for (int i = 0; i < N; i++)
        if (tab[i].origin == origin) {
            if (now_s - tab[i].heard_s > NAMES_TTL_S) return 0;
            return tab[i].handle;
        }
    return 0;
}
