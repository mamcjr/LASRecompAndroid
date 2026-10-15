// Issue #19: the two forwarders of a recycled action object (main+0x10cce50 = the vtable slot +0x28 of the class
// whose vtable is 0x7001c51358, main+0x10cce30 = its slot +0x30) do
//
//     x1 = this; x0 = [this+0x10]; pc = [[x0] + 0xa0]      (0xa8 in the +0x30 one)
//
// without checking [this+0x10]. The object is recycled by the task loop (main+0x13c21e0 dispatches the slot the
// task's state picks, and slot +0x00, main+0x10cce00, re-initialises the object: it writes the vtable and clears
// the delegate at +0x10). The class's own slot +0x38 (main+0x10cce80) checks *its* delegate (+0x18) with `cbz`
// before using it, so a missing delegate means "nothing to do". With the game's threads in the port's order the
// tick can dispatch +0x28 on an object whose delegate is still NULL, and the dereference crashed (signal 11, always
// at main+0x10cce50+0x8, always the same object). tools/patches.py calls this from both forwarders so they read a
// missing delegate the same way +0x38 does.
#include <stdint.h>

#include "../hle.h"

int rt_delegate_null(Cpu *c, int slot) {
    uint64_t obj = c->x[0];
    if (obj && *(uint64_t *)(uintptr_t)(obj + 0x10)) return 0;  // the usual case: forward like the original
    static int logged;
    if (logged++ < 8) LOG("[delegate] %#llx slot +0x%x: no delegate at [this+0x10], skipped (issue #19)\n", obj, slot);
    c->x[0] = 0;  // (returns 0, as slot +0x38 does when its delegate is missing: x0 is the delegate it loaded)
    return 1;
}
