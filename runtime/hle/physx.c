// A PhysX shape change without its simulation object (LASRecomp issue #314, ported from beta_5's hook): Hot Head
// (Turtle Rock) crashed reading 0x11 when the Magic Rod's fire hit him. RigidCore::onShapeChange (main+0x1408db0) and
// its sibling (main+0x1408d60) look the shape's simulation object up with main+0x14090a0 (RigidSim::getSimForShape),
// which hands back the address 1 when the simulation holds none - asserted only in PhysX's debug builds - and the
// update then read through it. tools/patches.py (SHAPE_SIM_SITES) skips the update there, to the functions' own
// return; this logs the first few, with the callers, so a report shows where it came from.
#include <stdint.h>
#include <stdio.h>

#include "../cpu.h"
#include "../rt.h"

void rt_physx_no_sim(uint64_t shape, uint32_t flags, uint64_t fp, uint64_t sp) {
    static int logged;
    if (logged++ >= 8) return;
    char where[160] = "";
    int n = 0;
    for (int i = 0; i < 6 && n < (int)sizeof where - 24 && fp >= sp && fp < sp + 0x100000 && !(fp & 7); i++) {
        n += snprintf(where + n, sizeof where - n, " %#llx", (unsigned long long)LD64(fp + 8));
        fp = LD64(fp);
    }
    LOG("[physx] shape %#llx changed (flags %#x) without a simulation object: update skipped (issue #314), from%s\n",
        (unsigned long long)shape, flags, where);
}
