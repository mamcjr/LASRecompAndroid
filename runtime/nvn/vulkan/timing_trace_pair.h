// Diagnostic history only: it never supplies values to the game or to ADPF.
#pragma once
#include "timing_trace.h"

#ifdef __ANDROID__
#define VK_TIMING_HISTORY 128
typedef struct { VkTimingCounter entries[VK_TIMING_HISTORY]; unsigned next, count; } VkTimingHistory;
static inline bool vk_timing_pair(VkTimingHistory *h, const VkTimingCounter *s, VkTimingCounter *start) {
    bool found = false;
    // Most recent address match, including cross-frame pairs (reported separately, never silently discarded).
    for (unsigned n = 0; n < h->count; n++) {
        const VkTimingCounter *p = &h->entries[(h->next + VK_TIMING_HISTORY - 1 - n) % VK_TIMING_HISTORY];
        if (s->address >= 16 && p->address == s->address - 16) { *start = *p; found = true; break; }
    }
    h->entries[h->next] = *s;
    h->next = (h->next + 1) % VK_TIMING_HISTORY;
    if (h->count < VK_TIMING_HISTORY) h->count++;
    return found;
}
#endif
