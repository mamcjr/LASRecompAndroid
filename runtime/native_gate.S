// Gates between the game's own code and the runtime (Native game code, runtime/native.c). From BOTWRecompAndroid.
// Cpu (cpu.h): x[n] at 8n, sp 256, nzcv 272, tpidr 280, tpidrro 288; these use its padding: the host sp to run C on
// (336), the host's x18 (344); v[n] at 512 + 16n.
// While C runs, x19 holds the Cpu and x20 the way back into the stub (both callee-saved; the guest's are in the Cpu).
// A stub's frame on the guest stack is written again from the Cpu after C: the C side may move sp or change x30 (a
// fix that returns), and a call into the game from there (guest_call) runs below the guest's sp, over the old frame.
#if defined(__aarch64__) && defined(__ANDROID__)
#define C_SP 256
#define C_NZCV 272
#define C_HOST_SP 336
#define C_HOST_X18 344
#define C_V 512

    .text
    .p2align 4

// The calling thread's Cpu from TPIDR_EL0 (native.c g_native_tab, the same hash and probing): into \rd; \rt, \rk
// clobbered. No flags touched.
.macro CPU_LOOKUP rd, rt, rk
    mrs \rt, tpidr_el0
    eor \rd, \rt, \rt, lsr #12
    ubfx \rd, \rd, #12, #12
    adrp \rk, g_native_tab
    add \rk, \rk, :lo12:g_native_tab
    add \rd, \rk, \rd, lsl #4
1:  ldr \rk, [\rd]
    eor \rk, \rk, \rt
    cbz \rk, 2f
    add \rd, \rd, #16
    b 1b
2:  ldr \rd, [\rd, #8]
.endm

.macro SAVE_X0_15 c
    stp x0, x1, [\c, #0]
    stp x2, x3, [\c, #16]
    stp x4, x5, [\c, #32]
    stp x6, x7, [\c, #48]
    stp x8, x9, [\c, #64]
    stp x10, x11, [\c, #80]
    stp x12, x13, [\c, #96]
    stp x14, x15, [\c, #112]
.endm
.macro SAVE_X18_29 c
    stp x18, x19, [\c, #144]
    stp x20, x21, [\c, #160]
    stp x22, x23, [\c, #176]
    stp x24, x25, [\c, #192]
    stp x26, x27, [\c, #208]
    stp x28, x29, [\c, #224]
.endm
// x16 = the Cpu; x19 last (it may be the Cpu's own register), x0-x1 after it
.macro LOAD_GUEST_X c
    ldr x18, [\c, #144]
    ldp x20, x21, [\c, #160]
    ldp x22, x23, [\c, #176]
    ldp x24, x25, [\c, #192]
    ldp x26, x27, [\c, #208]
    ldp x28, x29, [\c, #224]
    ldp x2, x3, [\c, #16]
    ldp x4, x5, [\c, #32]
    ldp x6, x7, [\c, #48]
    ldp x8, x9, [\c, #64]
    ldp x10, x11, [\c, #80]
    ldp x12, x13, [\c, #96]
    ldp x14, x15, [\c, #112]
    ldr x19, [\c, #152]
    ldp x0, x1, [\c, #0]
.endm
.macro SAVE_Q n, c, t
    add \t, \c, #C_V
    stp q0, q1, [\t, #0]
    stp q2, q3, [\t, #32]
    stp q4, q5, [\t, #64]
    stp q6, q7, [\t, #96]
.if \n > 8
    stp q8, q9, [\t, #128]
    stp q10, q11, [\t, #160]
    stp q12, q13, [\t, #192]
    stp q14, q15, [\t, #224]
    stp q16, q17, [\t, #256]
    stp q18, q19, [\t, #288]
    stp q20, q21, [\t, #320]
    stp q22, q23, [\t, #352]
    stp q24, q25, [\t, #384]
    stp q26, q27, [\t, #416]
    stp q28, q29, [\t, #448]
    stp q30, q31, [\t, #480]
.endif
.endm
.macro LOAD_Q n, c, t
    add \t, \c, #C_V
    ldp q0, q1, [\t, #0]
    ldp q2, q3, [\t, #32]
    ldp q4, q5, [\t, #64]
    ldp q6, q7, [\t, #96]
.if \n > 8
    ldp q8, q9, [\t, #128]
    ldp q10, q11, [\t, #160]
    ldp q12, q13, [\t, #192]
    ldp q14, q15, [\t, #224]
    ldp q16, q17, [\t, #256]
    ldp q18, q19, [\t, #288]
    ldp q20, q21, [\t, #320]
    ldp q22, q23, [\t, #352]
    ldp q24, q25, [\t, #384]
    ldp q26, q27, [\t, #416]
    ldp q28, q29, [\t, #448]
    ldp q30, q31, [\t, #480]
.endif
.endm

// A stub's 48-byte frame ([sp] x16, x17, [sp + 16] x30, [sp + 24] the number, [sp + 32] free): every guest register
// into the Cpu (x16, x17, x30 from the frame, sp as it was before the stub), then the host's sp and x18, x0 = the Cpu,
// w1 = the number, x19 = the Cpu, x20 = the way back into the stub.
.macro GATE_IN
    str x17, [sp, #24]
    str x15, [sp, #32]
    CPU_LOOKUP x16, x17, x15
    ldr x15, [sp, #32]
    SAVE_X0_15 x16
    ldp x0, x1, [sp]
    stp x0, x1, [x16, #128]
    SAVE_X18_29 x16
    ldr x0, [sp, #16]
    str x0, [x16, #240]
    add x0, sp, #48
    str x0, [x16, #C_SP]
    mrs x0, nzcv
    str x0, [x16, #C_NZCV]
    SAVE_Q 32, x16, x0
    mov x19, x16
    mov x20, x30
    ldr w1, [sp, #24]
    ldr x18, [x19, #C_HOST_X18]
    ldr x0, [x19, #C_HOST_SP]
    mov sp, x0
    mov x0, x19
.endm
// Back from C (x0 = its answer, kept at [sp + 24] for the stub): the frame written again below the Cpu's sp, every
// register from the Cpu
.macro GATE_OUT
    mov x16, x19
    ldr x1, [x16, #C_SP]
    sub sp, x1, #48
    str x0, [sp, #24]
    ldp x0, x1, [x16, #128]
    stp x0, x1, [sp]
    ldr x0, [x16, #240]
    str x0, [sp, #16]
    ldr x0, [x16, #C_NZCV]
    msr nzcv, x0
    LOAD_Q 32, x16, x0
    mov x30, x20
    LOAD_GUEST_X x16
    ret
.endm

// void native_enter(Cpu *c, uint64_t addr): call the guest function at addr with c's registers, as guest_call does.
// The guest gets x19 = c, which it gives back when it returns (callee-saved): native_exit finds the Cpu there. c's own
// x19 is kept aside, and x20-x29 are written back on return: the gates of svcs and runtime calls inside this call left
// that code's values in c (BOTWRecompAndroid: nn::init::Start's x20 came back as an init function's).
    .globl native_enter
    .hidden native_enter
    .type native_enter, %function
native_enter:
    stp x29, x30, [sp, #-192]!
    mov x29, sp
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    ldr x2, [x0, #C_HOST_SP]        // (an outer native_enter's, restored on the way out: nesting)
    ldr x3, [x0, #C_HOST_X18]
    stp x2, x3, [sp, #160]
    ldr x2, [x0, #152]              // (c's x19: the guest gets the Cpu there, native_exit puts it back)
    str x2, [sp, #176]
    str x18, [x0, #C_HOST_X18]
    mov x2, sp
    str x2, [x0, #C_HOST_SP]
    mov x16, x1
    mov x17, x0
    ldr x2, [x17, #C_SP]
    mov sp, x2
    ldr x2, [x17, #C_NZCV]
    msr nzcv, x2
    LOAD_Q 32, x17, x2
    adr x30, native_exit
    ldr x18, [x17, #144]
    ldp x20, x21, [x17, #160]
    ldp x22, x23, [x17, #176]
    ldp x24, x25, [x17, #192]
    ldp x26, x27, [x17, #208]
    ldp x28, x29, [x17, #224]
    ldp x2, x3, [x17, #16]
    ldp x4, x5, [x17, #32]
    ldp x6, x7, [x17, #48]
    ldp x8, x9, [x17, #64]
    ldp x10, x11, [x17, #80]
    ldp x12, x13, [x17, #96]
    ldp x14, x15, [x17, #112]
    mov x19, x17
    ldp x0, x1, [x17, #0]
    br x16

native_exit:                        // the guest function returned: x19 = its Cpu
    SAVE_X0_15 x19
    stp x16, x17, [x19, #128]
    str x18, [x19, #144]
    // x20-x29 are the entry's again (callee-saved): back into c, over what gates inside the call left there
    stp x20, x21, [x19, #160]
    stp x22, x23, [x19, #176]
    stp x24, x25, [x19, #192]
    stp x26, x27, [x19, #208]
    stp x28, x29, [x19, #224]
    mov x0, sp
    str x0, [x19, #C_SP]
    mrs x0, nzcv
    str x0, [x19, #C_NZCV]
    SAVE_Q 8, x19, x0
    ldr x18, [x19, #C_HOST_X18]
    ldr x0, [x19, #C_HOST_SP]
    mov sp, x0
    ldp x2, x3, [sp, #160]
    str x2, [x19, #C_HOST_SP]
    str x3, [x19, #C_HOST_X18]
    ldr x2, [sp, #176]
    str x2, [x19, #152]
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp x29, x30, [sp], #192
    ret
    .size native_enter, .-native_enter

// From a rewritten svc's stub (native.c stub_svc): x17 = the svc number, x30 = back into the stub. An svc preserves
// every register but its results: rt_svc runs on the host stack (native_svc_c) and they all come back from the Cpu.
    .globl native_svc_gate
    .hidden native_svc_gate
    .type native_svc_gate, %function
native_svc_gate:
    GATE_IN
    bl native_svc_c
    GATE_OUT
    .size native_svc_gate, .-native_svc_gate

// From a fix's stub (native.c stub_fix): x17 = the fix's number; the handler's answer (how to go on) goes back to the
// stub at [sp + 24].
    .globl native_patch_gate
    .hidden native_patch_gate
    .type native_patch_gate, %function
native_patch_gate:
    GATE_IN
    bl native_patch_c
    GATE_OUT
    .size native_patch_gate, .-native_patch_gate

// From an HLE slot (native.c prepare_hle): a call into the runtime. x17 = the slot, x30 = the guest's return, x16 free
// (as at any call). The arguments go into the Cpu, the function runs on the host stack (native_hle_c), and the
// registers come back from the Cpu - its results, and whatever the function set there.
    .globl native_hle_gate
    .hidden native_hle_gate
    .type native_hle_gate, %function
native_hle_gate:
    sub sp, sp, #32
    str x17, [sp]
    str x15, [sp, #8]
    CPU_LOOKUP x16, x17, x15
    ldr x15, [sp, #8]
    SAVE_X0_15 x16
    SAVE_X18_29 x16
    str x30, [x16, #240]
    add x0, sp, #32
    str x0, [x16, #C_SP]
    mrs x0, nzcv
    str x0, [x16, #C_NZCV]
    SAVE_Q 8, x16, x0
    mov x19, x16
    ldr w1, [sp]
    ldr x18, [x19, #C_HOST_X18]
    ldr x0, [x19, #C_HOST_SP]
    mov sp, x0
    mov x0, x19
    bl native_hle_c
    mov x16, x19
    ldr x0, [x16, #C_SP]
    mov sp, x0
    ldr x0, [x16, #C_NZCV]
    msr nzcv, x0
    LOAD_Q 8, x16, x0
    ldr x30, [x16, #240]
    LOAD_GUEST_X x16
    ret
    .size native_hle_gate, .-native_hle_gate
#endif
