// Rendering without VK_KHR_dynamic_rendering (Vulkan 1.3's; older drivers, e.g. Mali's: issue #2): the renderer
// begins every pass with a VkRenderingInfo, and here, when the device has no dynamic rendering, that becomes a classic
// render pass - a VkRenderPass made from the attachments' formats, samples, load / store operations and layouts, and a
// VkFramebuffer from their views, both kept for reuse (what yuzu, Citra and Ryujinx do throughout). Pipelines are made
// against a render pass compatible with their attachments' formats (vk_rp_compat). Every render pass has the same
// self-dependency, so any two with the same formats and samples stay compatible.
// From BOTWRecompAndroid's (the same renderer). LARECOMP_NO_DYNAMIC_RENDERING=1 (device.c) takes this path on any
// driver. Android only: on the desktop, vk.h's inline wrappers call dynamic rendering as before.
#ifdef __ANDROID__
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"

#define RP_MAX 512
#define FB_MAX 8192            // (a power of two: open addressing)
#define FB_VIEWS 9             // 8 colors and a depth / stencil
#define VSET_MAX 8192          // (a power of two) views some framebuffer holds, and how many

typedef struct {
    uint32_t ncolor;
    VkFormat color[8], ds;
    VkSampleCountFlagBits samples;
    uint32_t load[FB_VIEWS], store[FB_VIEWS], sload, sstore;  // (index 8: depth; VkAttachmentLoadOp / StoreOp)
    VkImageLayout layout[FB_VIEWS];
    uint8_t depth, stencil;  // the depth / stencil attachment's aspects in use
} RpKey;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct { RpKey k; VkRenderPass rp; } rps[RP_MAX];
static int nrps;

typedef struct { VkRenderPass rp; VkImageView v[FB_VIEWS]; uint32_t w, h; VkFramebuffer fb; uint8_t state; } FbEnt;  // state 1 live, 2 gone
static FbEnt fbs[FB_MAX];
static int fb_live, fb_used;  // (used: live and gone - the probes' length)
static struct { VkImageView v; uint32_t n; } vset[VSET_MAX];
static struct { VkFramebuffer fb; uint64_t seq; } *late;  // framebuffers to destroy once the GPU is past them
static int nlate, cap_late;
static bool vset_full;  // (a view didn't fit: a gone view's framebuffers are looked for in the whole table)
static __thread bool begun;  // (this thread's vk_begin_rendering began a render pass)

static uint64_t mix(uint64_t h, uint64_t v) { return (h ^ v) * 0x100000001b3ull; }

static VkRenderPass make_rp(const RpKey *k) {
    VkAttachmentDescription a[FB_VIEWS];
    VkAttachmentReference cr[8], dr = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL};
    uint32_t n = 0;
    for (uint32_t i = 0; i < k->ncolor; i++) {
        cr[i] = (VkAttachmentReference){VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL};
        if (k->color[i] == VK_FORMAT_UNDEFINED) continue;
        a[n] = (VkAttachmentDescription){.format = k->color[i], .samples = k->samples, .loadOp = k->load[i],
                                         .storeOp = k->store[i], .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                         .initialLayout = k->layout[i], .finalLayout = k->layout[i]};
        cr[i] = (VkAttachmentReference){n++, k->layout[i]};
    }
    if (k->ds != VK_FORMAT_UNDEFINED) {
        a[n] = (VkAttachmentDescription){.format = k->ds, .samples = k->samples,
                                         .loadOp = k->depth ? k->load[8] : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                         .storeOp = k->depth ? k->store[8] : VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                         .stencilLoadOp = k->stencil ? k->sload : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                         .stencilStoreOp = k->stencil ? k->sstore : VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                         .initialLayout = k->layout[8], .finalLayout = k->layout[8]};
        dr = (VkAttachmentReference){n++, k->layout[8]};
    }
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = k->ncolor,
                               .pColorAttachments = cr, .pDepthStencilAttachment = &dr};
    // (a barrier inside the pass needs one: the attachments' writes to the fragment work after them, per region)
    const VkPipelineStageFlags fb_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    VkSubpassDependency dep = {.srcSubpass = 0, .dstSubpass = 0, .srcStageMask = fb_stages, .dstStageMask = fb_stages,
                               .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                               .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT};
    VkRenderPassCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = n, .pAttachments = a,
                                 .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 1, .pDependencies = &dep};
    VkRenderPass rp = VK_NULL_HANDLE;
    VkResult r = vkCreateRenderPass(g_vk.dev, &ci, NULL, &rp);
    if (r != VK_SUCCESS) LOG("[vk] render pass (%u colors, depth/stencil %d) failed: %d\n", k->ncolor, k->ds, r);
    return rp;
}

static VkRenderPass get_rp(const RpKey *k) {  // (lock held)
    for (int i = 0; i < nrps; i++)
        if (!memcmp(&rps[i].k, k, sizeof *k)) return rps[i].rp;
    VkRenderPass rp = make_rp(k);
    if (rp && nrps < RP_MAX) {
        rps[nrps].k = *k;
        rps[nrps++].rp = rp;
        static int logged;
        if (logged++ < 8 || nrps == RP_MAX) LOG("[vk] render passes (no dynamic rendering): %d\n", nrps);
    }
    return rp;
}

VkRenderPass vk_rp_compat(uint32_t ncolor, const VkFormat *colors, VkFormat depth, VkFormat stencil, VkSampleCountFlagBits samples) {
    if (g_vk.dyn_render) return VK_NULL_HANDLE;
    RpKey k;
    memset(&k, 0, sizeof k);  // (padding too: keys compare as bytes)
    k.ncolor = ncolor > 8 ? 8 : ncolor;
    for (uint32_t i = 0; i < k.ncolor; i++) k.color[i] = colors ? colors[i] : VK_FORMAT_UNDEFINED;
    k.ds = depth != VK_FORMAT_UNDEFINED ? depth : stencil;
    k.depth = depth != VK_FORMAT_UNDEFINED, k.stencil = stencil != VK_FORMAT_UNDEFINED;
    k.samples = samples ? samples : VK_SAMPLE_COUNT_1_BIT;
    for (int i = 0; i < FB_VIEWS; i++) k.load[i] = VK_ATTACHMENT_LOAD_OP_LOAD, k.store[i] = VK_ATTACHMENT_STORE_OP_STORE, k.layout[i] = VK_IMAGE_LAYOUT_GENERAL;
    k.sload = VK_ATTACHMENT_LOAD_OP_LOAD, k.sstore = VK_ATTACHMENT_STORE_OP_STORE;
    pthread_mutex_lock(&lock);
    VkRenderPass rp = get_rp(&k);
    pthread_mutex_unlock(&lock);
    return rp;
}

// ---- framebuffers
static uint32_t vset_slot(VkImageView v) {
    uint32_t h = (uint32_t)(mix(0xcbf29ce484222325ull, (uint64_t)(uintptr_t)v) >> 17);
    for (uint32_t i = 0; i < VSET_MAX; i++) {
        uint32_t s = (h + i) & (VSET_MAX - 1);
        if (vset[s].v == v || !vset[s].v) return s;
    }
    return ~0u;
}

static void destroy_late(VkFramebuffer fb) {  // (lock held) once the work submitted so far is done
    if (nlate == cap_late) {
        cap_late = cap_late ? cap_late * 2 : 256;
        late = realloc(late, (size_t)cap_late * sizeof *late);
    }
    late[nlate].fb = fb;
    late[nlate++].seq = vk_pending_seq();
}

static void drop_all(void) {  // (lock held) the table full: every framebuffer goes, once the GPU is past it
    for (int i = 0; i < FB_MAX; i++)
        if (fbs[i].state == 1) destroy_late(fbs[i].fb);
    memset(fbs, 0, sizeof fbs);
    memset(vset, 0, sizeof vset);
    vset_full = false;
    fb_live = fb_used = 0;
    LOG("[vk] framebuffers (no dynamic rendering): table full, made again\n");
}

static VkFramebuffer get_fb(VkRenderPass rp, const VkImageView *v, uint32_t nv, uint32_t w, uint32_t h) {  // (lock held)
    FbEnt want;
    memset(&want, 0, sizeof want);
    want.rp = rp, want.w = w, want.h = h;
    memcpy(want.v, v, nv * sizeof *v);
    uint64_t hh = mix(mix(0xcbf29ce484222325ull, (uint64_t)(uintptr_t)rp), ((uint64_t)w << 32) | h);
    for (uint32_t i = 0; i < nv; i++) hh = mix(hh, (uint64_t)(uintptr_t)v[i]);
    uint32_t s0 = (uint32_t)(hh >> 20), free_slot = ~0u;
    for (uint32_t i = 0; i < FB_MAX; i++) {
        FbEnt *e = &fbs[(s0 + i) & (FB_MAX - 1)];
        if (!e->state) { if (free_slot == ~0u) free_slot = (s0 + i) & (FB_MAX - 1); break; }
        if (e->state == 2) { if (free_slot == ~0u) free_slot = (s0 + i) & (FB_MAX - 1); continue; }
        if (e->rp == rp && e->w == w && e->h == h && !memcmp(e->v, want.v, sizeof want.v)) return e->fb;
    }
    if (fb_used >= FB_MAX * 3 / 4) { drop_all(); free_slot = s0 & (FB_MAX - 1); }
    VkFramebufferCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = nv,
                                  .pAttachments = v, .width = w, .height = h, .layers = 1};
    VkFramebuffer fb = VK_NULL_HANDLE;
    VkResult r = vkCreateFramebuffer(g_vk.dev, &ci, NULL, &fb);
    if (r != VK_SUCCESS) {
        LOG("[vk] framebuffer %ux%u (%u views) failed: %d\n", w, h, nv, r);
        return VK_NULL_HANDLE;
    }
    FbEnt *e = &fbs[free_slot];
    if (!e->state) fb_used++;
    *e = want;
    e->fb = fb, e->state = 1;
    fb_live++;
    for (uint32_t i = 0; i < nv; i++) {
        uint32_t s = vset_slot(v[i]);
        if (s == ~0u) { vset_full = true; continue; }
        vset[s].v = v[i];
        vset[s].n++;
    }
    return fb;
}

// A view is gone (textures.c vk_collect_dead: the GPU is past its last use, and so past every framebuffer holding it)
void vk_rp_forget_view(VkImageView v) {
    if (g_vk.dyn_render || !v) return;
    pthread_mutex_lock(&lock);
    uint32_t s = vset_slot(v);
    if (vset_full || (s != ~0u && vset[s].v == v && vset[s].n)) {
        for (int i = 0; i < FB_MAX; i++) {
            FbEnt *e = &fbs[i];
            if (e->state != 1) continue;
            bool has = false;
            for (int j = 0; j < FB_VIEWS && !has; j++) has = e->v[j] == v;
            if (!has) continue;
            vkDestroyFramebuffer(g_vk.dev, e->fb, NULL);
            for (int j = 0; j < FB_VIEWS; j++) {  // (the other views' counts)
                if (!e->v[j] || e->v[j] == v) continue;
                uint32_t o = vset_slot(e->v[j]);
                if (o != ~0u && vset[o].v == e->v[j] && vset[o].n) vset[o].n--;
            }
            e->state = 2;
            fb_live--;
        }
        if (s != ~0u && vset[s].v == v) vset[s].n = 0;  // (the slot stays taken: other views' probes run through it)
    }
    pthread_mutex_unlock(&lock);
}

// The framebuffers dropped with a full table, destroyed once the GPU is past them (textures.c vk_collect_dead)
void vk_rp_collect(uint64_t done) {
    if (g_vk.dyn_render || !nlate) return;
    pthread_mutex_lock(&lock);
    int k = 0;
    for (int i = 0; i < nlate; i++) {
        if (late[i].seq > done) { late[k++] = late[i]; continue; }
        vkDestroyFramebuffer(g_vk.dev, late[i].fb, NULL);
    }
    nlate = k;
    pthread_mutex_unlock(&lock);
}

// ---- begin / end
void vk_begin_rendering(VkCommandBuffer cb, const VkRenderingInfo *ri, const VkFormat *colors, VkFormat ds_format,
                        VkSampleCountFlagBits samples) {
    if (g_vk.dyn_render) { vkCmdBeginRendering(cb, ri); return; }
    RpKey k;
    memset(&k, 0, sizeof k);
    k.ncolor = ri->colorAttachmentCount > 8 ? 8 : ri->colorAttachmentCount;
    k.samples = samples ? samples : VK_SAMPLE_COUNT_1_BIT;
    VkImageView views[FB_VIEWS];
    VkClearValue clears[FB_VIEWS];
    uint32_t nv = 0;
    for (uint32_t i = 0; i < k.ncolor; i++) {
        const VkRenderingAttachmentInfo *a = &ri->pColorAttachments[i];
        if (!a->imageView || !colors || colors[i] == VK_FORMAT_UNDEFINED) continue;  // (unused slot)
        k.color[i] = colors[i];
        k.load[i] = a->loadOp, k.store[i] = a->storeOp, k.layout[i] = a->imageLayout;
        clears[nv] = a->clearValue;
        views[nv++] = a->imageView;
    }
    const VkRenderingAttachmentInfo *d = ri->pDepthAttachment && ri->pDepthAttachment->imageView ? ri->pDepthAttachment : NULL;
    const VkRenderingAttachmentInfo *s = ri->pStencilAttachment && ri->pStencilAttachment->imageView ? ri->pStencilAttachment : NULL;
    if ((d || s) && ds_format != VK_FORMAT_UNDEFINED) {
        const VkRenderingAttachmentInfo *a = d ? d : s;
        k.ds = ds_format, k.depth = d != NULL, k.stencil = s != NULL;
        if (d) k.load[8] = d->loadOp, k.store[8] = d->storeOp;
        if (s) k.sload = s->loadOp, k.sstore = s->storeOp;
        k.layout[8] = a->imageLayout;
        clears[nv] = a->clearValue;
        if (s) clears[nv].depthStencil.stencil = s->clearValue.depthStencil.stencil;
        views[nv++] = a->imageView;
    }
    uint32_t w = ri->renderArea.offset.x + ri->renderArea.extent.width, h = ri->renderArea.offset.y + ri->renderArea.extent.height;
    pthread_mutex_lock(&lock);
    VkRenderPass rp = get_rp(&k);
    VkFramebuffer fb = rp ? get_fb(rp, views, nv, w, h) : VK_NULL_HANDLE;
    pthread_mutex_unlock(&lock);
    begun = fb != VK_NULL_HANDLE;
    if (!fb) return;  // (logged: the draws until vk_end_rendering are recorded outside a pass, and fail)
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp, .framebuffer = fb,
                                .renderArea = ri->renderArea, .clearValueCount = nv, .pClearValues = clears};
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
}

void vk_end_rendering(VkCommandBuffer cb) {
    if (g_vk.dyn_render) vkCmdEndRendering(cb);
    else if (begun) vkCmdEndRenderPass(cb);
    begun = false;
}
#endif
