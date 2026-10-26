// Vulkan device, window, swapchain and presentation (counterpart of the window/present half of metal.m).
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __ANDROID__
#include <android/native_window.h>
#include <dlfcn.h>
#endif

#include "../../paths.h"
#include "../../settings.h"
#include "../../ui/menu.h"
#include "vk.h"
#include "timing_trace.h"
#include "device_compat.h"
#include "../output_scale.h"
#include "../renderer.h"

VkG g_vk;
static SDL_Window *window;
static atomic_bool ui_ready;
static VkDebugUtilsMessengerEXT messenger;

// ================================================================== instance / device
static VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type, (void)user;
    static int n;
    if (sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT && n++ < 500) LOG("[vk] %s\n", d->pMessage);
    return VK_FALSE;
}

// LARECOMP_VK_VALIDATION=1: Khronos validation layer (MSYS2 package vulkan-validation-layers)
static void create_instance(void) {
#ifdef __ANDROID__
    vk_android_load();
#endif
    Uint32 n_sdl = 0;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_sdl);
    const char *ext[16];
    uint32_t n_ext = 0;
    for (Uint32 i = 0; i < n_sdl && n_ext < 14; i++) ext[n_ext++] = sdl_ext[i];
    bool validate = getenv("LARECOMP_VK_VALIDATION") != NULL;
    if (validate) ext[n_ext++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *layers[] = {"VK_LAYER_KHRONOS_validation"};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "LARecomp",
                             .pEngineName = "LARecomp", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = validate ? 1 : 0, .ppEnabledLayerNames = layers};
    VkResult r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    if (r != VK_SUCCESS && validate) {
        LOG("[vk] validation layer unavailable (%d); continuing without\n", r);
        ci.enabledLayerCount = 0;
        ci.enabledExtensionCount--;
        validate = false;
        r = vkCreateInstance(&ci, NULL, &g_vk.inst);
    }
    if (r != VK_SUCCESS) FATAL("vkCreateInstance: %d (is a Vulkan 1.3 driver installed?)\n", r);
#ifdef __ANDROID__
    volkLoadInstance(g_vk.inst);
#endif
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT f =
            (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(g_vk.inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = debug_cb};
        if (f) f(g_vk.inst, &mi, NULL, &messenger);
    }
}

static bool has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

// Discrete GPU first (LARECOMP_GPU=<index> picks one), needs Vulkan 1.3 and a queue that can present.
#ifdef __ANDROID__
// Drivers below Vulkan 1.3 (many phones' own, e.g. Mali up to Android 13-15): what of 1.2 / 1.3 the backend uses, as
// extensions. The backend then runs exactly its 1.3 code: the extensions' functions take the core names' place
// (compat_aliases, volk's pointers) and their structures are the same. `below`: the version from which it is core.
// After LASRecomp-Android-Plus by Nycolazs (MIT, runtime/nvn/vulkan/device.c COMPAT_EXT / compat_ok / compat_aliases).
// group: 0 required; else optional - all of a group or none of it - with a way without it (Mali's older drivers, issue #2):
//   CG_RP     dynamic rendering - classic render passes (rpcompat.c)
//   CG_SYNC2  synchronization2 - Vulkan 1.0's barriers and submissions (sync1_*, below)
//   CG_EDS    extended dynamic state - the states baked into each pipeline (exec.c PipeKey.fx)
//   CG_DEMOTE / CG_TERM - only SPIR-V 1.6 (Vulkan 1.3) uses them; older SPIR-V's discard is OpKill (shaders.c)
enum { CG_REQ, CG_RP, CG_SYNC2, CG_EDS, CG_DEMOTE, CG_TERM, CG_N };
static const char *const CG_WITHOUT[CG_N] = {"", "render passes", "Vulkan 1.0 barriers", "states baked into pipelines",
                                             "not needed below 1.3", "not needed below 1.3"};
static const struct { const char *name; uint32_t below; uint8_t group; } COMPAT_EXT[] = {
    {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_API_VERSION_1_3, CG_RP},
    {VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME, VK_API_VERSION_1_2, CG_RP},  // (dynamic rendering needs these two)
    {VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME, VK_API_VERSION_1_2, CG_RP},
    {VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME, VK_API_VERSION_1_3, CG_SYNC2},
    {VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, VK_API_VERSION_1_3, CG_EDS},
    {VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, VK_API_VERSION_1_3, CG_EDS},
    {VK_EXT_SHADER_DEMOTE_TO_HELPER_INVOCATION_EXTENSION_NAME, VK_API_VERSION_1_3, CG_DEMOTE},
    {VK_KHR_SHADER_TERMINATE_INVOCATION_EXTENSION_NAME, VK_API_VERSION_1_3, CG_TERM},
    {VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME, VK_API_VERSION_1_2},
    {VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME, VK_API_VERSION_1_2},
    {VK_KHR_8BIT_STORAGE_EXTENSION_NAME, VK_API_VERSION_1_2},
    {VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME, VK_API_VERSION_1_2},
    {VK_KHR_SAMPLER_MIRROR_CLAMP_TO_EDGE_EXTENSION_NAME, VK_API_VERSION_1_2},
};
#define N_COMPAT_EXT (sizeof COMPAT_EXT / sizeof *COMPAT_EXT)
static char g_compat_missing[512];  // (every one a rejected GPU lacked: the fatal message names them)

// Whether a device below 1.3 has every extension of an optional group
static bool compat_group(VkExtensionProperties *ep, uint32_t ne, uint32_t api, int group) {
    for (size_t i = 0; i < N_COMPAT_EXT; i++)
        if (COMPAT_EXT[i].group == group && api < COMPAT_EXT[i].below && !has_ext(ep, ne, COMPAT_EXT[i].name)) return false;
    return true;
}

// Whether a device below 1.3 has every extension it needs (missing ones logged)
static bool compat_ok(VkPhysicalDevice pd, uint32_t api) {
    if (api < VK_API_VERSION_1_1) {
        snprintf(g_compat_missing, sizeof g_compat_missing, "Vulkan 1.1");
        return false;
    }
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ep);
    bool ok = true;
    g_compat_missing[0] = 0;
    for (size_t i = 0; i < N_COMPAT_EXT; i++)
        if (api < COMPAT_EXT[i].below && !has_ext(ep, ne, COMPAT_EXT[i].name)) {
            if (COMPAT_EXT[i].group) {  // (optional: its group's other way)
                LOG("[vk-cap]   no %s: %s\n", COMPAT_EXT[i].name, CG_WITHOUT[COMPAT_EXT[i].group]);
                continue;
            }
            LOG("[vk-cap]   missing %s\n", COMPAT_EXT[i].name);
            size_t n = strlen(g_compat_missing);
            snprintf(g_compat_missing + n, sizeof g_compat_missing - n, "%s%s", n ? ", " : "", COMPAT_EXT[i].name);
            ok = false;
        }
    free(ep);
    return ok;
}

static bool env_on(const char *name) { const char *v = getenv(name); return v && atoi(v); }

// ---- synchronization2 without it (Mali's older drivers): Vulkan 1.0's barriers, submissions and timestamps. The
// stage / access bits below 32 are 1.0's own; the split-up ones above map to the 1.0 bit they came from.
static VkPipelineStageFlags sync1_stages(VkPipelineStageFlags2 s, bool src) {
    VkPipelineStageFlags r = (VkPipelineStageFlags)(s & 0xFFFFFFFFull);
    if (s & (VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT |
             VK_PIPELINE_STAGE_2_CLEAR_BIT)) r |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (s & (VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT)) r |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    if (s & VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT) r |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT;
    return r ? r : src ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;  // (NONE)
}
static VkAccessFlags sync1_access(VkAccessFlags2 a) {
    VkAccessFlags r = (VkAccessFlags)(a & 0xFFFFFFFFull);
    if (a & (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT)) r |= VK_ACCESS_SHADER_READ_BIT;
    if (a & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT) r |= VK_ACCESS_SHADER_WRITE_BIT;
    return r;
}
// (1.0 has one stage pair per call: one call per barrier)
static VKAPI_ATTR void VKAPI_CALL sync1_barrier(VkCommandBuffer cb, const VkDependencyInfo *d) {
    for (uint32_t i = 0; i < d->memoryBarrierCount; i++) {
        const VkMemoryBarrier2 *m = &d->pMemoryBarriers[i];
        VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = sync1_access(m->srcAccessMask),
                             .dstAccessMask = sync1_access(m->dstAccessMask)};
        vkCmdPipelineBarrier(cb, sync1_stages(m->srcStageMask, true), sync1_stages(m->dstStageMask, false), d->dependencyFlags,
                             1, &b, 0, NULL, 0, NULL);
    }
    for (uint32_t i = 0; i < d->bufferMemoryBarrierCount; i++) {
        const VkBufferMemoryBarrier2 *m = &d->pBufferMemoryBarriers[i];
        VkBufferMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, .srcAccessMask = sync1_access(m->srcAccessMask),
                                   .dstAccessMask = sync1_access(m->dstAccessMask), .srcQueueFamilyIndex = m->srcQueueFamilyIndex,
                                   .dstQueueFamilyIndex = m->dstQueueFamilyIndex, .buffer = m->buffer, .offset = m->offset,
                                   .size = m->size};
        vkCmdPipelineBarrier(cb, sync1_stages(m->srcStageMask, true), sync1_stages(m->dstStageMask, false), d->dependencyFlags,
                             0, NULL, 1, &b, 0, NULL);
    }
    for (uint32_t i = 0; i < d->imageMemoryBarrierCount; i++) {
        const VkImageMemoryBarrier2 *m = &d->pImageMemoryBarriers[i];
        VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = sync1_access(m->srcAccessMask),
                                  .dstAccessMask = sync1_access(m->dstAccessMask), .oldLayout = m->oldLayout,
                                  .newLayout = m->newLayout, .srcQueueFamilyIndex = m->srcQueueFamilyIndex,
                                  .dstQueueFamilyIndex = m->dstQueueFamilyIndex, .image = m->image,
                                  .subresourceRange = m->subresourceRange};
        vkCmdPipelineBarrier(cb, sync1_stages(m->srcStageMask, true), sync1_stages(m->dstStageMask, false), d->dependencyFlags,
                             0, NULL, 0, NULL, 1, &b);
    }
}
static VKAPI_ATTR void VKAPI_CALL sync1_timestamp(VkCommandBuffer cb, VkPipelineStageFlags2 s, VkQueryPool pool, uint32_t q) {
    VkPipelineStageFlags f = !s ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                           : (s & VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT) ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : sync1_stages(s, false);
    vkCmdWriteTimestamp(cb, (VkPipelineStageFlagBits)(f & -f), pool, q);  // (one stage: the earliest bit)
}
// (timeline values through VkTimelineSemaphoreSubmitInfo; a binary semaphore's value is ignored)
#define SYNC1_MAX 8
static VKAPI_ATTR VkResult VKAPI_CALL sync1_submit(VkQueue queue, uint32_t n, const VkSubmitInfo2 *si, VkFence fence) {
    if (n > SYNC1_MAX) n = SYNC1_MAX;  // (the backend submits one at a time)
    VkSubmitInfo s[SYNC1_MAX];
    VkTimelineSemaphoreSubmitInfo tl[SYNC1_MAX];
    VkSemaphore wsem[SYNC1_MAX][SYNC1_MAX], ssem[SYNC1_MAX][SYNC1_MAX];
    uint64_t wval[SYNC1_MAX][SYNC1_MAX], sval[SYNC1_MAX][SYNC1_MAX];
    VkPipelineStageFlags wst[SYNC1_MAX][SYNC1_MAX];
    VkCommandBuffer cbs[SYNC1_MAX][SYNC1_MAX];
    for (uint32_t i = 0; i < n; i++) {
        const VkSubmitInfo2 *a = &si[i];
        uint32_t nw = a->waitSemaphoreInfoCount < SYNC1_MAX ? a->waitSemaphoreInfoCount : SYNC1_MAX;
        uint32_t nc = a->commandBufferInfoCount < SYNC1_MAX ? a->commandBufferInfoCount : SYNC1_MAX;
        uint32_t ns = a->signalSemaphoreInfoCount < SYNC1_MAX ? a->signalSemaphoreInfoCount : SYNC1_MAX;
        for (uint32_t j = 0; j < nw; j++) {
            wsem[i][j] = a->pWaitSemaphoreInfos[j].semaphore;
            wval[i][j] = a->pWaitSemaphoreInfos[j].value;
            wst[i][j] = sync1_stages(a->pWaitSemaphoreInfos[j].stageMask, false);
        }
        for (uint32_t j = 0; j < nc; j++) cbs[i][j] = a->pCommandBufferInfos[j].commandBuffer;
        for (uint32_t j = 0; j < ns; j++) {
            ssem[i][j] = a->pSignalSemaphoreInfos[j].semaphore;
            sval[i][j] = a->pSignalSemaphoreInfos[j].value;
        }
        tl[i] = (VkTimelineSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
                                                .waitSemaphoreValueCount = nw, .pWaitSemaphoreValues = wval[i],
                                                .signalSemaphoreValueCount = ns, .pSignalSemaphoreValues = sval[i]};
        s[i] = (VkSubmitInfo){.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .pNext = &tl[i], .waitSemaphoreCount = nw,
                              .pWaitSemaphores = wsem[i], .pWaitDstStageMask = wst[i], .commandBufferCount = nc,
                              .pCommandBuffers = cbs[i], .signalSemaphoreCount = ns, .pSignalSemaphores = ssem[i]};
    }
    return vkQueueSubmit(queue, n, s, fence);
}

// ---- extended dynamic state without it: these states are baked into each pipeline (exec.c PipeKey.fx), so the
// backend's calls setting them do nothing
static VKAPI_ATTR void VKAPI_CALL eds_cull(VkCommandBuffer cb, VkCullModeFlags v) { (void)cb, (void)v; }
static VKAPI_ATTR void VKAPI_CALL eds_front(VkCommandBuffer cb, VkFrontFace v) { (void)cb, (void)v; }
static VKAPI_ATTR void VKAPI_CALL eds_topology(VkCommandBuffer cb, VkPrimitiveTopology v) { (void)cb, (void)v; }
static VKAPI_ATTR void VKAPI_CALL eds_bool(VkCommandBuffer cb, VkBool32 v) { (void)cb, (void)v; }
static VKAPI_ATTR void VKAPI_CALL eds_compare(VkCommandBuffer cb, VkCompareOp v) { (void)cb, (void)v; }
static VKAPI_ATTR void VKAPI_CALL eds_stencil_op(VkCommandBuffer cb, VkStencilFaceFlags f, VkStencilOp a, VkStencilOp b,
                                                 VkStencilOp c, VkCompareOp d) {
    (void)cb, (void)f, (void)a, (void)b, (void)c, (void)d;
}

static void compat_aliases(void) {  // (after volkLoadDevice)
    if (g_vk.api < VK_API_VERSION_1_3) {
        vkCmdBeginRendering = vkCmdBeginRenderingKHR;
        vkCmdEndRendering = vkCmdEndRenderingKHR;
        vkCmdPipelineBarrier2 = vkCmdPipelineBarrier2KHR;
        vkQueueSubmit2 = vkQueueSubmit2KHR;
        vkCmdWriteTimestamp2 = vkCmdWriteTimestamp2KHR;
        vkCmdSetCullMode = vkCmdSetCullModeEXT;
        vkCmdSetFrontFace = vkCmdSetFrontFaceEXT;
        vkCmdSetPrimitiveTopology = vkCmdSetPrimitiveTopologyEXT;
        vkCmdSetDepthTestEnable = vkCmdSetDepthTestEnableEXT;
        vkCmdSetDepthWriteEnable = vkCmdSetDepthWriteEnableEXT;
        vkCmdSetDepthCompareOp = vkCmdSetDepthCompareOpEXT;
        vkCmdSetStencilTestEnable = vkCmdSetStencilTestEnableEXT;
        vkCmdSetStencilOp = vkCmdSetStencilOpEXT;
        vkCmdSetPrimitiveRestartEnable = vkCmdSetPrimitiveRestartEnableEXT;
        vkCmdSetDepthBiasEnable = vkCmdSetDepthBiasEnableEXT;
    }
    if (g_vk.api < VK_API_VERSION_1_2) {
        vkWaitSemaphores = vkWaitSemaphoresKHR;
        vkGetSemaphoreCounterValue = vkGetSemaphoreCounterValueKHR;
        vkSignalSemaphore = vkSignalSemaphoreKHR;
    }
    if (!g_vk.sync2) {  // (the backend's synchronization2 calls, translated: sync1_*)
        vkCmdPipelineBarrier2 = sync1_barrier;
        vkQueueSubmit2 = sync1_submit;
        vkCmdWriteTimestamp2 = sync1_timestamp;
    }
    if (!g_vk.eds) {  // (the states are in the pipelines: exec.c PipeKey.fx)
        vkCmdSetCullMode = eds_cull;
        vkCmdSetFrontFace = eds_front;
        vkCmdSetPrimitiveTopology = eds_topology;
        vkCmdSetDepthTestEnable = vkCmdSetDepthWriteEnable = vkCmdSetStencilTestEnable = eds_bool;
        vkCmdSetPrimitiveRestartEnable = vkCmdSetDepthBiasEnable = eds_bool;
        vkCmdSetDepthCompareOp = eds_compare;
        vkCmdSetStencilOp = eds_stencil_op;
    }
}

// (Android: a GPU whose queues can't present is skipped rather than fatal, and the fatal message says what to report;
// below 1.3, a GPU with the extensions above)
static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    int want = getenv("LARECOMP_GPU") ? atoi(getenv("LARECOMP_GPU")) : -1, best = -1, best_score = -1;
    uint32_t best_queue = UINT32_MAX;
    bool have_api = false;
    // LARECOMP_VK_API=1.1 / 1.2 (test): use a newer device as that version - the extensions' path on a phone at hand
    // (drivers keep listing the extensions their version promoted); 1.0: as a driver too old, for its error and the
    // driver dialog (platform/android.c driver_error_dialog)
    const char *force = getenv("LARECOMP_VK_API");
    uint32_t cap = force && !strcmp(force, "1.1") ? VK_API_VERSION_1_1 : force && !strcmp(force, "1.2") ? VK_API_VERSION_1_2
                 : force && !strcmp(force, "1.0") ? VK_API_VERSION_1_0 : 0;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        LOG("[vk] gpu %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (cap && p.apiVersion > cap) p.apiVersion = cap;
        if (p.apiVersion < VK_API_VERSION_1_3 && !compat_ok(pd[i], p.apiVersion)) {
            LOG("[vk-cap] skipping %s: Vulkan %u.%u without what the renderer needs of 1.3\n", p.deviceName,
                VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion));
            continue;
        }
        have_api = true;
        uint32_t nq = 0, present_queue = UINT32_MAX;
        vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &nq, NULL);
        VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
        if (nq && !qp) FATAL("could not allocate Vulkan queue information\n");
        vkGetPhysicalDeviceQueueFamilyProperties(pd[i], &nq, qp);
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 present = VK_FALSE;
            VkResult result = vkGetPhysicalDeviceSurfaceSupportKHR(pd[i], q, g_vk.surface, &present);
            if (result != VK_SUCCESS) LOG("[vk-cap] gpu %u queue %u surface query failed: %d\n", i, q, result);
            if (vk_backend_queue_supported(&qp[q], result, present)) { present_queue = q; break; }
        }
        free(qp);
        if (present_queue == UINT32_MAX) {
            LOG("[vk-cap] skipping %s: no graphics+compute queue can present to this surface\n", p.deviceName);
            continue;
        }
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if ((int)i == want) score = 100;
        if (score > best_score) { best_score = score; best = (int)i; best_queue = present_queue; }
    }
    if (best < 0 && have_api)
        FATAL("No Vulkan GPU has a graphics+compute queue that can present to the screen. Check the selected driver.\n");
    if (best < 0)
        FATAL("This phone's Vulkan driver is too old for the game: it needs Vulkan 1.3, or 1.1 / 1.2 with extensions "
              "(missing: %s). Check the selected driver and report your phone model and Android version.\n",
              g_compat_missing[0] ? g_compat_missing : "Vulkan 1.1");
    g_vk.phys = pd[best];
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    g_vk.qfam = best_queue;
    uint32_t api = cap && g_vk.props.apiVersion > cap ? cap : g_vk.props.apiVersion;
    g_vk.api = api < VK_API_VERSION_1_3 ? VK_MAKE_API_VERSION(0, 1, VK_API_VERSION_MINOR(api), 0) : VK_API_VERSION_1_3;
    LOG("[vk] using %s (Vulkan %u.%u%s)\n", g_vk.props.deviceName, VK_API_VERSION_MAJOR(g_vk.api),
        VK_API_VERSION_MINOR(g_vk.api), g_vk.api < VK_API_VERSION_1_3 ? " with extensions for 1.3" : "");
}
#else
static void pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_vk.inst, &n, NULL);
    if (!n) FATAL("no Vulkan device\n");
    VkPhysicalDevice *pd = calloc(n, sizeof *pd);
    vkEnumeratePhysicalDevices(g_vk.inst, &n, pd);
    int want = getenv("LARECOMP_GPU") ? atoi(getenv("LARECOMP_GPU")) : -1, best = -1, best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pd[i], &p);
        LOG("[vk] gpu %u: %s (api %u.%u.%u)\n", i, p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
            VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if ((int)i == want) score = 100;
        if (score > best_score) { best_score = score; best = (int)i; }
    }
    if (best < 0) FATAL("no Vulkan 1.3 device\n");
    g_vk.phys = pd[best];
    free(pd);
    vkGetPhysicalDeviceProperties(g_vk.phys, &g_vk.props);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, NULL);
    VkQueueFamilyProperties *qp = calloc(nq, sizeof *qp);
    vkGetPhysicalDeviceQueueFamilyProperties(g_vk.phys, &nq, qp);
    g_vk.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_vk.phys, i, g_vk.surface, &present);
        if ((qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present) { g_vk.qfam = i; break; }
    }
    free(qp);
    if (g_vk.qfam == UINT32_MAX) FATAL("no graphics+present queue\n");
    LOG("[vk] using %s\n", g_vk.props.deviceName);
}
#endif

static bool g_fault_ext;
// After VK_ERROR_DEVICE_LOST: what the driver says faulted (VK_EXT_device_fault), into the log.
void vk_device_fault_report(void) {
    static int once;
    if (once++ || !g_fault_ext) return;
    PFN_vkGetDeviceFaultInfoEXT get = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetDeviceFaultInfoEXT");
    if (!get) return;
    VkDeviceFaultCountsEXT c = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (get(g_vk.dev, &c, NULL) != VK_SUCCESS) { LOG("[vk] device fault: no info\n"); return; }
    VkDeviceFaultAddressInfoEXT addr[16];
    VkDeviceFaultVendorInfoEXT vend[16];
    if (c.addressInfoCount > 16) c.addressInfoCount = 16;
    if (c.vendorInfoCount > 16) c.vendorInfoCount = 16;
    c.vendorBinarySize = 0;
    VkDeviceFaultInfoEXT info = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = addr, .pVendorInfos = vend};
    if (get(g_vk.dev, &c, &info) != VK_SUCCESS && 0) return;
    LOG("[vk] device fault: %s\n", info.description);
    static const char *const kind[] = {"none", "read invalid", "write invalid", "execute invalid", "ip unknown", "ip invalid", "ip valid"};
    for (uint32_t i = 0; i < c.addressInfoCount; i++)
        LOG("[vk]   address %s %#llx (precision %#llx)\n", addr[i].addressType < 7 ? kind[addr[i].addressType] : "?",
            (unsigned long long)addr[i].reportedAddress, (unsigned long long)addr[i].addressPrecision);
    for (uint32_t i = 0; i < c.vendorInfoCount; i++)
        LOG("[vk]   vendor: %s (code %#llx data %#llx)\n", vend[i].description, (unsigned long long)vend[i].vendorFaultCode,
            (unsigned long long)vend[i].vendorFaultData);
}

static void create_device(void) {
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *ep = calloc(ne, sizeof *ep);
    vkEnumerateDeviceExtensionProperties(g_vk.phys, NULL, &ne, ep);
#ifdef __ANDROID__
    // An advertised extension does not imply every optional feature bit is supported.
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border_have = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT divisor_have = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    VkPhysicalDeviceFaultFeaturesEXT fault_have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executable_have = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
    VkPhysicalDeviceFeatures2 optional = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
#define QUERY_OPTIONAL(name, feature) do { if (has_ext(ep, ne, name)) { \
        feature.pNext = optional.pNext; optional.pNext = &feature; } } while (0)
    QUERY_OPTIONAL(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, border_have);
    QUERY_OPTIONAL(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME, divisor_have);
    QUERY_OPTIONAL(VK_EXT_DEVICE_FAULT_EXTENSION_NAME, fault_have);
    QUERY_OPTIONAL(VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME, executable_have);
#undef QUERY_OPTIONAL
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &optional);
#endif
    const char *ext[32];
    uint32_t n_ext = 0;
    ext[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
#ifdef __ANDROID__
    // (below 1.3: pick_device checked the required ones are there; an optional group goes in only whole). Test
    // switches, each that group's other way on any driver (below 1.3 also as if its extensions were missing, as on
    // Mali's): LARECOMP_NO_DYNAMIC_RENDERING=1, LARECOMP_NO_SYNC2=1, LARECOMP_NO_EDS=1
    bool no_dr = env_on("LARECOMP_NO_DYNAMIC_RENDERING"), no_s2 = env_on("LARECOMP_NO_SYNC2"), no_eds = env_on("LARECOMP_NO_EDS");
    bool cg[CG_N];
    for (int g = 0; g < CG_N; g++) cg[g] = g_vk.api >= VK_API_VERSION_1_3 || compat_group(ep, ne, g_vk.api, g);
    if (g_vk.api < VK_API_VERSION_1_3) cg[CG_RP] &= !no_dr, cg[CG_SYNC2] &= !no_s2, cg[CG_EDS] &= !no_eds;
    for (size_t i = 0; i < N_COMPAT_EXT; i++)
        if (g_vk.api < COMPAT_EXT[i].below && cg[COMPAT_EXT[i].group]) ext[n_ext++] = COMPAT_EXT[i].name;
    // (VkPhysicalDeviceDriverProperties: core from 1.2, before that its own extension, only queried)
    bool driver_props = g_vk.api >= VK_API_VERSION_1_2 || has_ext(ep, ne, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
#endif
    g_vk.custom_border = has_ext(ep, ne, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
#ifdef __ANDROID__
    if (g_vk.custom_border && !vk_backend_border_supported(&border_have)) {
        LOG("[vk-cap] custom border disabled: renderer needs customBorderColors and customBorderColorWithoutFormat\n");
        g_vk.custom_border = false;
    }
#endif
    if (g_vk.custom_border) ext[n_ext++] = VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME;
    g_vk.divisor = has_ext(ep, ne, VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
#ifdef __ANDROID__
    if (g_vk.divisor && !divisor_have.vertexAttributeInstanceRateDivisor) {
        LOG("[vk-cap] instance divisor extension disabled: vertexAttributeInstanceRateDivisor unavailable\n");
        g_vk.divisor = false;
    }
#endif
    if (g_vk.divisor) ext[n_ext++] = VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME;
    g_vk.host_import = has_ext(ep, ne, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);  // guest heap as a GPU buffer (exec.c)
#ifdef __ANDROID__
    // Phones: the import pins all of the guest heap. Samsung's Xclipse driver took it and then failed the first
    // submission (VK_ERROR_UNKNOWN, Galaxy S26, in LASRecomp-Android-Plus), and Mali drivers offer the extension too;
    // Adreno's own driver doesn't. The CPU copies (exec.c gcopy) run everywhere. LARECOMP_VK_HOST_IMPORT=1: the import
    // where the driver offers it.
    if (g_vk.host_import && !env_on("LARECOMP_VK_HOST_IMPORT")) {
        LOG("[vk-cap] external memory host left out: the heap through CPU copies (LARECOMP_VK_HOST_IMPORT=1: import)\n");
        g_vk.host_import = false;
    }
#endif
    if (g_vk.host_import) ext[n_ext++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    bool fault = has_ext(ep, ne, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);  // device-lost diagnostics (vk_device_fault_report)
#ifdef __ANDROID__
    if (fault && !fault_have.deviceFault) {
        LOG("[vk-cap] device-fault diagnostics disabled: deviceFault unavailable\n");
        fault = false;
    }
#endif
    if (fault) ext[n_ext++] = VK_EXT_DEVICE_FAULT_EXTENSION_NAME;
    g_fault_ext = fault;
#ifdef __ANDROID__
    // LARECOMP_SHADER_STATS=1: each graphics pipeline's compiled shaders described in the log (registers, waves,
    // spills, instructions: exec.c build_pipeline), for finding what makes a translated shader slow on this GPU
    g_vk.exec_stats = getenv("LARECOMP_SHADER_STATS") &&
                      has_ext(ep, ne, VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME) && executable_have.pipelineExecutableInfo;
    if (g_vk.exec_stats) ext[n_ext++] = VK_KHR_PIPELINE_EXECUTABLE_PROPERTIES_EXTENSION_NAME;
    // the guest heap as GPU memory mapped at its own address (exec.c vk_heap_place; Turnip has it)
    g_vk.map_placed = has_ext(ep, ne, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME) && has_ext(ep, ne, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME);
    if (g_vk.map_placed) {
        ext[n_ext++] = VK_KHR_MAP_MEMORY_2_EXTENSION_NAME;
        ext[n_ext++] = VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME;
    }
#endif
    free(ep);

    // features the backend relies on (all present on current desktop drivers)
    VkPhysicalDeviceFeatures2 have = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
#ifdef __ANDROID__
    VkPhysicalDeviceVulkan13Features h13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features h12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &h13};
    VkPhysicalDeviceVulkan11Features h11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &h12};
    have.pNext = &h11;
    // Below 1.3 the Vulkan13 structure (below 1.2 the 11 / 12 ones too) isn't valid: the extensions' own structures are
    // asked instead and copied into h11 / h12 / h13, so what follows reads them as on 1.3
    VkPhysicalDeviceDynamicRenderingFeaturesKHR q_dr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    VkPhysicalDeviceSynchronization2FeaturesKHR q_s2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT q_eds = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT q_eds2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT q_dem = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT};
    VkPhysicalDeviceShaderTerminateInvocationFeaturesKHR q_term = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES_KHR};
    VkPhysicalDeviceTimelineSemaphoreFeatures q_tl = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
    VkPhysicalDeviceShaderFloat16Int8Features q_i8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,
                                                      .pNext = &q_tl};
    VkPhysicalDevice8BitStorageFeatures q_b8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, .pNext = &q_i8};
    VkPhysicalDeviceScalarBlockLayoutFeatures q_sbl = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,
                                                       .pNext = &q_b8};
    VkPhysicalDevice16BitStorageFeatures q_b16 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
                                                  .pNext = &q_sbl};
    if (g_vk.api < VK_API_VERSION_1_3) {  // (the 1.3 extensions' there, then 1.2's own 11 / 12 or 1.1's extensions')
        h12.pNext = NULL;
        void *q = g_vk.api >= VK_API_VERSION_1_2 ? (void *)&h11 : (void *)&q_b16;
#define QCHAIN(on, s) do { if (on) { (s).pNext = q; q = &(s); } } while (0)
        QCHAIN(cg[CG_RP], q_dr);
        QCHAIN(cg[CG_SYNC2], q_s2);
        QCHAIN(cg[CG_EDS], q_eds);
        QCHAIN(cg[CG_EDS], q_eds2);
        QCHAIN(cg[CG_DEMOTE], q_dem);
        QCHAIN(cg[CG_TERM], q_term);
#undef QCHAIN
        have.pNext = q;
    }
#endif
    vkGetPhysicalDeviceFeatures2(g_vk.phys, &have);
#ifdef __ANDROID__
    if (g_vk.api < VK_API_VERSION_1_3) {
        h13.dynamicRendering = cg[CG_RP] && q_dr.dynamicRendering;
        h13.synchronization2 = cg[CG_SYNC2] && q_s2.synchronization2;
        h13.shaderDemoteToHelperInvocation = cg[CG_DEMOTE] && q_dem.shaderDemoteToHelperInvocation;
        h13.shaderTerminateInvocation = cg[CG_TERM] && q_term.shaderTerminateInvocation;
        h13.maintenance4 = VK_TRUE;  // (only for 1.3's SPIR-V 1.6 LocalSizeId: older SPIR-V, shaders.c, doesn't use it)
        if (g_vk.api < VK_API_VERSION_1_2) {
            h11.storageBuffer16BitAccess = q_b16.storageBuffer16BitAccess;
            h12.timelineSemaphore = q_tl.timelineSemaphore;
            h12.shaderInt8 = q_i8.shaderInt8;
            h12.storageBuffer8BitAccess = q_b8.storageBuffer8BitAccess;
            h12.uniformAndStorageBuffer8BitAccess = q_b8.uniformAndStorageBuffer8BitAccess;
            h12.scalarBlockLayout = q_sbl.scalarBlockLayout;
            h12.samplerMirrorClampToEdge = VK_TRUE;  // (its extension has no feature bit: compat_ok found it)
        }
    }
    // Without synchronization2: Vulkan 1.0's barriers and submissions (sync1_*). Without extended dynamic state: the
    // states baked into each pipeline (exec.c). On 1.3 both are core: only the test switches leave them out.
    g_vk.sync2 = h13.synchronization2 && !no_s2;
    g_vk.eds = (g_vk.api >= VK_API_VERSION_1_3 || (cg[CG_EDS] && q_eds.extendedDynamicState && q_eds2.extendedDynamicState2)) && !no_eds;
    if (!g_vk.sync2) LOG("[vk] no synchronization2%s: Vulkan 1.0 barriers and submissions\n", no_s2 ? " (LARECOMP_NO_SYNC2)" : "");
    if (!g_vk.eds) LOG("[vk] no extended dynamic state%s: states baked into pipelines\n", no_eds ? " (LARECOMP_NO_EDS)" : "");
    // No dynamic rendering (Mali's older drivers: issue #2): classic render passes (rpcompat.c).
    // LARECOMP_NO_DYNAMIC_RENDERING=1 (test): that path on any driver.
    g_vk.dyn_render = h13.dynamicRendering && !no_dr;
    if (!g_vk.dyn_render) LOG("[vk] no dynamic rendering%s: render passes and framebuffers\n",
                              h13.dynamicRendering ? " (LARECOMP_NO_DYNAMIC_RENDERING)" : "");
    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                              .pNext = driver_props ? &driver : NULL};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &properties);
    if (!driver_props) snprintf(driver.driverName, sizeof driver.driverName, "unknown");
    LOG("[vk-cap] gpu %s vendor %#x device %#x api %u.%u.%u driver-id %u name %s info %s\n",
        g_vk.props.deviceName, g_vk.props.vendorID, g_vk.props.deviceID, VK_API_VERSION_MAJOR(g_vk.props.apiVersion),
        VK_API_VERSION_MINOR(g_vk.props.apiVersion), VK_API_VERSION_PATCH(g_vk.props.apiVersion),
        (unsigned)driver.driverID, driver.driverName, driver.driverInfo);
    LOG("[vk-cap] storage16 %u timeline %u mirror-clamp %u scalar %u dynamic-rendering %u sync2 %u demote %u terminate %u maintenance4 %u int64 %u int8 %u storage8 %u uniform-storage8 %u BC %u ASTC %u timestamps %u\n",
        h11.storageBuffer16BitAccess, h12.timelineSemaphore, h12.samplerMirrorClampToEdge, h12.scalarBlockLayout,
        h13.dynamicRendering, h13.synchronization2, h13.shaderDemoteToHelperInvocation, h13.shaderTerminateInvocation,
        h13.maintenance4, have.features.shaderInt64, h12.shaderInt8, h12.storageBuffer8BitAccess,
        h12.uniformAndStorageBuffer8BitAccess, have.features.textureCompressionBC, have.features.textureCompressionASTC_LDR,
        g_vk.props.limits.timestampComputeAndGraphics);
    {   // (for driver compatibility reports: how far a storage buffer reaches and the memory types, vulkan/exec.c)
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
        char types[256] = "";
        size_t n = 0;
        for (uint32_t i = 0; i < mp.memoryTypeCount && n < sizeof types - 16; i++)
            n += (size_t)snprintf(types + n, sizeof types - n, " %u:0x%x", i, (unsigned)mp.memoryTypes[i].propertyFlags);
        LOG("[vk-cap] storage range %llu MB, storage offset alignment %llu, memory types%s\n",
            (unsigned long long)(g_vk.props.limits.maxStorageBufferRange >> 20),
            (unsigned long long)g_vk.props.limits.minStorageBufferOffsetAlignment, types);
    }
    const char *missing = vk_missing_backend_feature(&h11, &h12, &h13);
    if (missing) FATAL("Vulkan driver on %s lacks a feature required by this renderer: %s.\n"
                       "Driver: %s. Include this message, phone model and app version in your bug report.\n",
                       g_vk.props.deviceName, missing, driver.driverName);
#endif
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT div = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT,
        .vertexAttributeInstanceRateDivisor = VK_TRUE, .vertexAttributeInstanceRateZeroDivisor = VK_FALSE};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .customBorderColors = VK_TRUE,
        .customBorderColorWithoutFormat = VK_TRUE, .pNext = g_vk.divisor ? &div : NULL};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .dynamicRendering = VK_TRUE, .synchronization2 = VK_TRUE,
                                            .shaderDemoteToHelperInvocation = VK_TRUE, .shaderTerminateInvocation = VK_TRUE,
                                            .maintenance4 = VK_TRUE,
                                            .pNext = g_vk.custom_border ? (void *)&cbc : g_vk.divisor ? (void *)&div : NULL};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .timelineSemaphore = VK_TRUE, .shaderInt8 = VK_TRUE, .samplerMirrorClampToEdge = VK_TRUE,
                                            .uniformAndStorageBuffer8BitAccess = VK_TRUE, .storageBuffer8BitAccess = VK_TRUE,
                                            .scalarBlockLayout = VK_TRUE, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                            .storageBuffer16BitAccess = VK_TRUE, .pNext = &f12};
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .deviceFault = VK_TRUE,
                                           .pNext = &f11};
    VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = fault ? (void *)&ff : (void *)&f11};
    VkPhysicalDeviceFeatures *b = &f.features, *h = &have.features;
#ifdef __ANDROID__
    {   // Adreno has no uniformAndStorageBuffer8BitAccess, and no shader uses 8-bit uniform buffers
        // (docs/ANDROID-PORT.md, Fase 0): the 8-bit features only when the driver has them
        f12.shaderInt8 = h12.shaderInt8;
        f12.uniformAndStorageBuffer8BitAccess = h12.uniformAndStorageBuffer8BitAccess;
        // nor 16-bit storage: no translated or built-in shader declares it (none of 3592 cached on the Adreno 830),
        // and the Adreno 610 under Turnip lacks it (issue #4). LARECOMP_VK_NO_STORAGE16=1 (test): as if it lacked it.
        if (getenv("LARECOMP_VK_NO_STORAGE16") && atoi(getenv("LARECOMP_VK_NO_STORAGE16")))
            h11.storageBuffer16BitAccess = VK_FALSE, LOG("[vk] 16-bit storage left off (LARECOMP_VK_NO_STORAGE16)\n");
        f11.storageBuffer16BitAccess = h11.storageBuffer16BitAccess;
        f13.dynamicRendering = g_vk.dyn_render;
        f13.synchronization2 = g_vk.sync2;
        f12.storageBuffer8BitAccess = h12.storageBuffer8BitAccess;
        // device addresses: shaders reach the heap placed in GPU memory through a pointer where a storage buffer's
        // range can't cover it (exec.c vk_heap_place; Turnip on Adreno 7xx: 128 MB). Required from Vulkan 1.3 on.
        g_vk.bda = g_vk.api >= VK_API_VERSION_1_2 && h12.bufferDeviceAddress && g_vk.map_placed;
        f12.bufferDeviceAddress = g_vk.bda;
    }
    // Below 1.3: the same features through the extensions' structures (1.2: its own 11 / 12 ones and the 1.3
    // extensions'; 1.1: the extensions' for both), ahead of the optional ones f13 led to
    VkPhysicalDeviceDynamicRenderingFeaturesKHR e_dr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR,
                                                        .dynamicRendering = VK_TRUE, .pNext = f13.pNext};
    VkPhysicalDeviceSynchronization2FeaturesKHR e_s2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES_KHR,
                                                        .synchronization2 = VK_TRUE, .pNext = &e_dr};
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT e_eds = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT, .extendedDynamicState = VK_TRUE,
        .pNext = &e_s2};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT e_eds2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, .extendedDynamicState2 = VK_TRUE,
        .pNext = &e_eds};
    VkPhysicalDeviceShaderDemoteToHelperInvocationFeaturesEXT e_dem = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DEMOTE_TO_HELPER_INVOCATION_FEATURES_EXT,
        .shaderDemoteToHelperInvocation = VK_TRUE, .pNext = &e_eds2};
    VkPhysicalDeviceShaderTerminateInvocationFeaturesKHR e_term = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_TERMINATE_INVOCATION_FEATURES_KHR,
        .shaderTerminateInvocation = VK_TRUE, .pNext = &e_dem};
    VkPhysicalDeviceTimelineSemaphoreFeatures e_tl = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES,
                                                      .timelineSemaphore = VK_TRUE};
    VkPhysicalDeviceShaderFloat16Int8Features e_i8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES,
                                                      .shaderInt8 = h12.shaderInt8, .pNext = &e_tl};
    VkPhysicalDevice8BitStorageFeatures e_b8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES,
                                                .storageBuffer8BitAccess = h12.storageBuffer8BitAccess,
                                                .uniformAndStorageBuffer8BitAccess = h12.uniformAndStorageBuffer8BitAccess,
                                                .pNext = &e_i8};
    VkPhysicalDeviceScalarBlockLayoutFeatures e_sbl = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES,
                                                       .scalarBlockLayout = VK_TRUE, .pNext = &e_b8};
    VkPhysicalDevice16BitStorageFeatures e_b16 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES,
                                                  .storageBuffer16BitAccess = h11.storageBuffer16BitAccess, .pNext = &e_sbl};
    if (g_vk.api < VK_API_VERSION_1_3) {
        void *x = f13.pNext;  // (the 1.3 extensions' in use, ahead of the optional ones f13 led to)
#define ECHAIN(on, s) do { if (on) { (s).pNext = x; x = &(s); } } while (0)
        ECHAIN(g_vk.dyn_render, e_dr);
        ECHAIN(g_vk.sync2, e_s2);
        ECHAIN(g_vk.eds, e_eds);
        ECHAIN(g_vk.eds, e_eds2);
        ECHAIN(h13.shaderDemoteToHelperInvocation, e_dem);
        ECHAIN(h13.shaderTerminateInvocation, e_term);
#undef ECHAIN
        e_tl.pNext = x;
        void *head = &e_b16;
        if (g_vk.api >= VK_API_VERSION_1_2) {
            f12.pNext = x;  // (f11 -> f12 -> the 1.3 extensions'; f13 left out)
            head = &f11;
        }
        ff.pNext = head;
        f.pNext = fault ? (void *)&ff : head;
    }
#endif
    b->independentBlend = h->independentBlend;
    b->dualSrcBlend = h->dualSrcBlend;
    b->fillModeNonSolid = h->fillModeNonSolid;
    b->samplerAnisotropy = h->samplerAnisotropy;
    b->textureCompressionBC = h->textureCompressionBC;
#ifdef __ANDROID__
    b->textureCompressionASTC_LDR = h->textureCompressionASTC_LDR;  // (Adreno: the game's ASTC textures as they are)
#endif
    b->shaderInt64 = h->shaderInt64;
    g_vk.shader_int64 = h->shaderInt64;
    b->depthBiasClamp = h->depthBiasClamp;
    b->depthClamp = h->depthClamp;
    b->largePoints = h->largePoints;
    b->wideLines = h->wideLines;
    b->imageCubeArray = h->imageCubeArray;
    b->sampleRateShading = h->sampleRateShading;
    b->fragmentStoresAndAtomics = h->fragmentStoresAndAtomics;
    b->vertexPipelineStoresAndAtomics = h->vertexPipelineStoresAndAtomics;
    b->robustBufferAccess = h->robustBufferAccess;  // guest shaders index cbufs freely: keep OOB reads harmless
    b->shaderClipDistance = h->shaderClipDistance;
#ifdef __ANDROID__
    if (!h->shaderInt64) LOG("[vk] no shaderInt64: translating shader address/carry math to 32-bit words\n");
#else
    if (!h->shaderInt64) LOG("[vk] warning: no shaderInt64 (64-bit address math in shaders)\n");
#endif
#ifdef __ANDROID__
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
    if (g_vk.map_placed) {
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT pf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 q = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &pf};
        vkGetPhysicalDeviceFeatures2(g_vk.phys, &q);
        VkPhysicalDeviceMapMemoryPlacedPropertiesEXT pp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 qp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pp};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &qp);
        g_vk.placed_align = pp.minPlacedMemoryMapAlignment;
        if (pf.memoryMapPlaced) {
            placed.memoryMapPlaced = VK_TRUE;
            placed.pNext = f.pNext;
            f.pNext = &placed;
        } else {
            g_vk.map_placed = false;
            n_ext -= 2;  // (the two map extensions were the last added)
        }
    }
    VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pex = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, .pipelineExecutableInfo = VK_TRUE};
    if (g_vk.exec_stats) { pex.pNext = f.pNext; f.pNext = &pex; }
#endif
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qfam,
                                  .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo di = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f, .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qi, .enabledExtensionCount = n_ext, .ppEnabledExtensionNames = ext};
    VK_CHECK(vkCreateDevice(g_vk.phys, &di, NULL, &g_vk.dev));
#ifdef __ANDROID__
    volkLoadDevice(g_vk.dev);
    compat_aliases();
    LOG("[vk] Vulkan %u.%u device, %u extensions\n", VK_API_VERSION_MAJOR(g_vk.api), VK_API_VERSION_MINOR(g_vk.api), n_ext);
#endif
    vkGetDeviceQueue(g_vk.dev, g_vk.qfam, 0, &g_vk.queue);

    VmaAllocatorCreateInfo ai = {.physicalDevice = g_vk.phys, .device = g_vk.dev, .instance = g_vk.inst,
                                 .vulkanApiVersion = VK_API_VERSION_1_3};
#ifdef __ANDROID__
    ai.vulkanApiVersion = g_vk.api;
    VmaVulkanFunctions vf = {.vkGetInstanceProcAddr = vkGetInstanceProcAddr, .vkGetDeviceProcAddr = vkGetDeviceProcAddr};
    ai.pVulkanFunctions = &vf;  // (VMA_DYNAMIC_VULKAN_FUNCTIONS: from the driver volk loaded)
#endif
    VK_CHECK(vmaCreateAllocator(&ai, &g_vk.vma));

    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE, .initialValue = 0};
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &g_vk.timeline));
    VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_vk.qfam};
    VK_CHECK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_vk.pool));

    // pipeline cache: cache/vk_pipelines.bin (written by vk_pipeline_cache_save)
    size_t sz = 0;
    void *data = NULL;
    FILE *fp = fopen("cache/vk_pipelines.bin", "rb");
    if (fp) {
#ifdef __ANDROID__  // (a size cap and the public header checked: a file from another driver or a cut-off write)
        long length = -1;
        if (!fseek(fp, 0, SEEK_END)) length = ftell(fp);
        if (length >= 32 && (unsigned long)length <= VK_BACKEND_PIPELINE_CACHE_MAX && !fseek(fp, 0, SEEK_SET)) {
            data = malloc((size_t)length);
            if (data && fread(data, 1, (size_t)length, fp) == (size_t)length &&
                vk_backend_cache_compatible(data, (size_t)length, &g_vk.props)) sz = (size_t)length;
        }
        if (!sz) { LOG("[vk-cache] ignoring unreadable, malformed or incompatible pipeline cache; starting empty\n");
            free(data); data = NULL; }
#else
        fseek(fp, 0, SEEK_END);
        sz = (size_t)ftell(fp);
        fseek(fp, 0, SEEK_SET);
        data = malloc(sz);
        if (fread(data, 1, sz, fp) != sz) sz = 0;
#endif
        fclose(fp);
    }
    VkPipelineCacheCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = sz,
                                    .pInitialData = data};
    if (vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache) != VK_SUCCESS) {
#ifdef __ANDROID__
        LOG("[vk-cache] driver rejected pipeline cache; retrying empty\n");
        pc.pInitialData = NULL;
#endif
        pc.initialDataSize = 0;
        VK_CHECK(vkCreatePipelineCache(g_vk.dev, &pc, NULL, &g_vk.pcache));
    }
    free(data);
}

void vk_pipeline_cache_save(void) {
    size_t sz = 0;
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) != VK_SUCCESS || !sz) return;
    void *data = malloc(sz);
    if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, data) == VK_SUCCESS) {
        mkdir("cache", 0755);
        FILE *f = fopen("cache/vk_pipelines.bin.tmp", "wb");
        if (f) {
            bool ok = fwrite(data, 1, sz, f) == sz;
            fclose(f);
            if (ok) { remove("cache/vk_pipelines.bin"); rename("cache/vk_pipelines.bin.tmp", "cache/vk_pipelines.bin"); }
        }
    }
    free(data);
}

// ================================================================== swapchain
static bool swapchain_readback = true;  // (Android: false where the surface can't be a transfer source)
static VkPresentModeKHR present_mode(void) {
    // MAILBOX by default: the game paces itself (vi.c's emulated 60 Hz vsync), and with FIFO every submission
    // queued behind the one waiting for a window image, so the game's fence waits followed the presentation engine:
    // a throttled / captured / busy compositor slowed the whole game (4-21 fps with the GPU mostly idle; mailbox:
    // 60). No tearing. LARECOMP_VK_PRESENT=fifo|immediate|mailbox to choose.
    const char *e = getenv("LARECOMP_VK_PRESENT");
#ifdef __ANDROID__
    // Frame generation (framegen.c): FIFO, which shows the in-between frame, presented right after the real one, a
    // refresh later (mailbox would replace it before it is ever shown)
    extern bool fg_enabled(void);
    if (!e && fg_enabled()) e = "fifo";
#endif
    VkPresentModeKHR want = !e || !strcmp(e, "mailbox") ? VK_PRESENT_MODE_MAILBOX_KHR
                          : !strcmp(e, "immediate") ? VK_PRESENT_MODE_IMMEDIATE_KHR : VK_PRESENT_MODE_FIFO_KHR;
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16) n = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_vk.phys, g_vk.surface, &n, m);
    for (uint32_t i = 0; i < n; i++) if (m[i] == want) return want;
    return VK_PRESENT_MODE_FIFO_KHR;
}

static void create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_vk.phys, g_vk.surface, &caps));
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
#ifdef __ANDROID__
    // The surface reports the panel's own (portrait) orientation; the window is landscape: images of the window's
    // size, turned by the compositor (pre-rotation in the app would spare it that: docs/ANDROID-PORT.md, Fase 3)
    VkExtent2D ext = {(uint32_t)w, (uint32_t)h};
    VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    LOG("[vk] swapchain: surface %ux%u transform %#x, window %dx%d, present mode %d, images %u-%u\n", caps.currentExtent.width,
        caps.currentExtent.height, caps.currentTransform, w, h, present_mode(), caps.minImageCount, caps.maxImageCount);
#else
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : (VkExtent2D){(uint32_t)w, (uint32_t)h};
    VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
#endif
    if (!ext.width || !ext.height) return;  // minimised
    uint32_t nf = 0;
#ifdef __ANDROID__
    VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL));
    if (!nf) FATAL("Vulkan driver reports no display surface formats. Check the selected driver.\n");
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    VkResult format_result = vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
    if ((format_result != VK_SUCCESS && format_result != VK_INCOMPLETE) || !nf)
        FATAL("Vulkan surface format query failed: %d.\n", format_result);
#else
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_vk.phys, g_vk.surface, &nf, fmts);
#endif
    // scanout shows the bytes the game wrote (see the raw views in the present pass): a UNORM swapchain like Metal's
    VkSurfaceFormatKHR sf = fmts[0];
#ifdef __ANDROID__
    if (nf == 1 && sf.format == VK_FORMAT_UNDEFINED) sf.format = VK_FORMAT_B8G8R8A8_UNORM;
#endif
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { sf = fmts[i]; break; }
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > 8) count = 8;
#ifdef __ANDROID__  // (only the usages and composition the surface supports: drivers differ)
    if (count < caps.minImageCount) FATAL("Surface needs %u swapchain images; renderer supports at most 8.\n", caps.minImageCount);
    VkImageUsageFlags usage = vk_backend_swapchain_usage(caps.supportedUsageFlags);
    VkCompositeAlphaFlagBitsKHR alpha = vk_backend_composite_alpha(caps.supportedCompositeAlpha);
    if (!usage || !alpha) FATAL("Vulkan surface lacks supported color rendering or composition (usage %#x alpha %#x).\n",
                               caps.supportedUsageFlags, caps.supportedCompositeAlpha);
    swapchain_readback = (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (!swapchain_readback) LOG("[vk-cap] window screenshots unavailable: surface has no transfer-source usage\n");
#else
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
#endif
    VkSwapchainKHR old = g_vk.swapchain;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_vk.surface, .minImageCount = count,
        .imageFormat = sf.format, .imageColorSpace = sf.colorSpace, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = usage,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = transform,
        .compositeAlpha = alpha, .presentMode = present_mode(), .clipped = VK_TRUE,
        .oldSwapchain = old};
    VK_CHECK(vkCreateSwapchainKHR(g_vk.dev, &ci, NULL, &g_vk.swapchain));
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vk_rp_forget_view(g_vk.sc_views[i]), vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    if (old) vkDestroySwapchainKHR(g_vk.dev, old, NULL);
    g_vk.sc_format = sf.format;
    g_vk.sc_extent = ext;
#ifdef __ANDROID__
    VK_CHECK(vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, NULL));
    if (g_vk.sc_count > 8) FATAL("Driver created %u swapchain images; renderer supports at most 8.\n", g_vk.sc_count);
    VK_CHECK(vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images));
#else
    g_vk.sc_count = 8;
    vkGetSwapchainImagesKHR(g_vk.dev, g_vk.swapchain, &g_vk.sc_count, g_vk.sc_images);
#endif
    for (uint32_t i = 0; i < g_vk.sc_count; i++) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = g_vk.sc_images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sf.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_vk.sc_views[i]));
    }
}

static void recreate_swapchain(void) {
    vkDeviceWaitIdle(g_vk.dev);
    create_swapchain();
}

#ifdef __ANDROID__
// The app in the background loses its window surface (SDL clears the window's ANativeWindow, and gives the new one
// when it comes back): the Vulkan surface and swapchain are made again for the new window. Frames in between are
// skipped. Before this, everything after a trip to the background stayed black (every acquire: SURFACE_LOST).
static struct ANativeWindow *surface_aw;  // the window g_vk.surface was made for
static bool surface_lost;

static struct ANativeWindow *android_window(void) {
    return SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
}

// The display rate frame generation wants (framegen.c: 60, or 120 with its 120 Hz option), on the game's own surface -
// the games' way to ask (ANativeWindow_setFrameRate, a fixed source: the display picks a rate it divides evenly); the
// activity's preferred display mode alone (LASRecompActivity.preferRefreshRate) was overruled to 120 on the POCO F7
// Ultra. 0: none asked. Asked again for a new surface (back from the background).
static float surface_hz;
static void surface_rate_apply(struct ANativeWindow *aw) {
    if (!aw || surface_hz <= 0) return;
    // (libnativewindow's: API 30 / 31 - looked up, the build links only libandroid)
    typedef int32_t (*SetRate)(struct ANativeWindow *, float, int8_t);
    typedef int32_t (*SetRate2)(struct ANativeWindow *, float, int8_t, int8_t);  // (API 31: always, not only seamless)
    static SetRate set1;
    static SetRate2 set2;
    static bool looked;
    if (!looked) {
        looked = true;
        void *lib = dlopen("libnativewindow.so", RTLD_NOW);
        if (lib) set1 = (SetRate)dlsym(lib, "ANativeWindow_setFrameRate"),
                 set2 = (SetRate2)dlsym(lib, "ANativeWindow_setFrameRateWithChangeStrategy");
    }
    int32_t r = set2   ? set2(aw, surface_hz, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE, 1 /* ALWAYS */)
                : set1 ? set1(aw, surface_hz, ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE)
                       : -1;
    LOG("[vk] surface frame rate %.0f Hz asked (%d)\n", surface_hz, r);
}
void vk_surface_frame_rate(float hz) {
    surface_hz = hz;
    surface_rate_apply(surface_aw);
}

static void android_create_surface(struct ANativeWindow *aw) {
    VkAndroidSurfaceCreateInfoKHR si = {.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR, .window = aw};
    VK_CHECK(vkCreateAndroidSurfaceKHR(g_vk.inst, &si, NULL, &g_vk.surface));
    surface_aw = aw;
    surface_rate_apply(aw);
}

static bool android_surface_ready(void) {  // false: no window to draw into now
    struct ANativeWindow *aw = android_window();
    if (!aw) { surface_lost = true; return false; }
    if (aw == surface_aw && !surface_lost) return true;
    vkDeviceWaitIdle(g_vk.dev);
    for (uint32_t i = 0; i < g_vk.sc_count; i++) vk_rp_forget_view(g_vk.sc_views[i]), vkDestroyImageView(g_vk.dev, g_vk.sc_views[i], NULL);
    g_vk.sc_count = 0;
    if (g_vk.swapchain) vkDestroySwapchainKHR(g_vk.dev, g_vk.swapchain, NULL);
    g_vk.swapchain = VK_NULL_HANDLE;
    vkDestroySurfaceKHR(g_vk.inst, g_vk.surface, NULL);
    android_create_surface(aw);
    surface_lost = false;
    LOG("[vk] window surface made again (the app came back from the background)\n");
    create_swapchain();
    return g_vk.swapchain != VK_NULL_HANDLE;
}
#endif

// ================================================================== frames (start screen and game)
// Acquire semaphores rotate per frame in flight; a slot is reused once the submission that waited on it is done.
#define FRAMES 3
static VkSemaphore acquire_sem[FRAMES], done_sem[8];
static uint64_t slot_seq[FRAMES];
static int slot;

static VkSemaphore binary_semaphore(void) {
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore s;
    VK_CHECK(vkCreateSemaphore(g_vk.dev, &si, NULL, &s));
    return s;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = from, .newLayout = to, .image = img,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}

typedef struct { uint32_t image; VkSemaphore acquired; bool ok; } Frame;
static Frame cur_frame;

// Acquire the next window image and begin rendering into it (cleared to `clear`). Returns false when there is
// nothing to draw into (minimised window).
bool vk_frame_begin(const float clear[4]) {
    cur_frame.ok = false;
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    if (!w || !h) return false;  // minimised
#ifdef __ANDROID__
    if (!android_surface_ready()) return false;
#endif
    if (!g_vk.swapchain || (uint32_t)w != g_vk.sc_extent.width || (uint32_t)h != g_vk.sc_extent.height) recreate_swapchain();
    if (!g_vk.swapchain) return false;
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] frame begin %dx%d (swapchain %ux%u)\n", w, h, g_vk.sc_extent.width, g_vk.sc_extent.height);
    slot = (slot + 1) % FRAMES;
    if (!acquire_sem[slot]) acquire_sem[slot] = binary_semaphore();
#ifdef __ANDROID__
    bool timing = vk_timing_trace_on();
    uint64_t trace_start = timing ? SDL_GetTicksNS() : 0;
#endif
    vk_wait_seq(slot_seq[slot]);
#ifdef __ANDROID__
    if (timing) vk_timing_trace_wait(VK_TIMING_SLOT, SDL_GetTicksNS() - trace_start);
    trace_start = timing ? SDL_GetTicksNS() : 0;
#endif
    VkResult r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        recreate_swapchain();
        r = vkAcquireNextImageKHR(g_vk.dev, g_vk.swapchain, UINT64_MAX, acquire_sem[slot], VK_NULL_HANDLE, &cur_frame.image);
    }
#ifdef __ANDROID__
    if (timing) vk_timing_trace_wait(VK_TIMING_ACQUIRE, SDL_GetTicksNS() - trace_start);
    if (r == VK_ERROR_SURFACE_LOST_KHR) { surface_lost = true; return false; }  // (made again at the next frame)
#endif
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { LOG("[vk] acquire failed: %d\n", r); return false; }
    cur_frame.acquired = acquire_sem[slot];
    slot_seq[slot] = vk_pending_seq();
    VkCommandBuffer cb = vk_cmd();
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue.color.float32 = {clear[0], clear[1], clear[2], clear[3]}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vk_begin_rendering(cb, &ri, &g_vk.sc_format, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    cur_frame.ok = true;
    return true;
}

// Passes of their own between vk_frame_begin and vk_frame_end (exec.c post passes): end the window image's
// rendering, and begin it again keeping what was drawn.
void vk_frame_suspend(void) {
    if (cur_frame.ok) vk_end_rendering(vk_cmd());
}
void vk_frame_resume(void) {
    if (!cur_frame.ok) return;
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo ca = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = g_vk.sc_views[cur_frame.image],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_vk.sc_extent}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vk_begin_rendering(cb, &ri, &g_vk.sc_format, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT);
    VkViewport vp = {0, 0, (float)g_vk.sc_extent.width, (float)g_vk.sc_extent.height, 0, 1};
    VkRect2D sc = {{0, 0}, g_vk.sc_extent};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
}

VkExtent2D vk_frame_extent(void) { return g_vk.sc_extent; }
void *vk_frame_cmd(void) { return (void *)vk_cmd(); }

static void save_window_image(VkCommandBuffer cb, VkSpan *out) {
    uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
    *out = vk_readback((uint64_t)w * h * 4);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy rg = {.bufferOffset = out->off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out->buf, 1, &rg);
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}

#ifdef __ANDROID__
static bool g_fg_copy;  // vk_frame_end: this real frame gets an in-between one (framegen.c: its HUD mask)
#endif
// End rendering into the window image and present it. shot_path: also save the window image as PNG.
void vk_frame_end(const char *shot_path) {
    if (!cur_frame.ok) return;
    if (shot_path && !swapchain_readback) {
        LOG("[vk] window screenshot skipped: unsupported surface readback\n");
        shot_path = NULL;
    }
    VkCommandBuffer cb = vk_cmd();
    vk_end_rendering(cb);
    VkSpan shot = {0};
    if (shot_path) save_window_image(cb, &shot);
#ifdef __ANDROID__
    if (g_fg_copy) {  // (frame generation: outside rendering, before the real picture goes to the presentation engine)
        extern void fg_copy_window(VkCommandBuffer cb, VkImage sc, VkFormat fmt, uint32_t w, uint32_t h);
        fg_copy_window(cb, g_vk.sc_images[cur_frame.image], g_vk.sc_format, g_vk.sc_extent.width, g_vk.sc_extent.height);
        g_fg_copy = false;
    }
#endif
    image_barrier(cb, g_vk.sc_images[cur_frame.image], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    uint32_t img = cur_frame.image;
    if (!done_sem[img]) done_sem[img] = binary_semaphore();
    uint64_t seq = vk_pending_seq();
    vk_submit_extra(cur_frame.acquired, done_sem[img], VK_NULL_HANDLE);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &done_sem[img],
                           .swapchainCount = 1, .pSwapchains = &g_vk.swapchain, .pImageIndices = &img};
#ifdef __ANDROID__
    uint64_t trace_start = vk_timing_trace_on() ? SDL_GetTicksNS() : 0;
#endif
    VkResult r = vkQueuePresentKHR(g_vk.queue, &pi);
#ifdef __ANDROID__
    if (trace_start) vk_timing_trace_wait(VK_TIMING_PRESENT, SDL_GetTicksNS() - trace_start);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) recreate_swapchain();  // (SUBOPTIMAL: every frame of an unrotated swapchain)
    else if (r == VK_ERROR_SURFACE_LOST_KHR) surface_lost = true;
#else
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) recreate_swapchain();
#endif
    cur_frame.ok = false;
    if (shot_path) {
        vk_wait_seq(seq);
        uint32_t w = g_vk.sc_extent.width, h = g_vk.sc_extent.height;
        uint8_t *px = shot.ptr;
        bool bgr = g_vk.sc_format == VK_FORMAT_B8G8R8A8_UNORM || g_vk.sc_format == VK_FORMAT_B8G8R8A8_SRGB;
        for (uint64_t i = 0; bgr && i < (uint64_t)w * h; i++) { uint8_t t = px[i * 4]; px[i * 4] = px[i * 4 + 2]; px[i * 4 + 2] = t; }
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) px[i * 4 + 3] = 255;
        vk_save_png(shot_path, px, w, h);
    }
}

void vk_save_png(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
#ifdef __ANDROID__
    // Resolve debug captures against the writable data root instead of Android SDL's relative-path/asset lookup.
    char absolute[1400];
    if (path[0] != '/') {
        int n = snprintf(absolute, sizeof absolute, "%s/%s", rt_data_root(), path);
        if (n < 0 || (size_t)n >= sizeof absolute) { LOG("[vk] screenshot path too long\n"); return; }
        path = absolute;
    }
#endif
    SDL_Surface *s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888, (void *)rgba, (int)w * 4);
    if (!s || !SDL_SavePNG(s, path)) LOG("[vk] saving %s failed: %s\n", path, SDL_GetError());
    else LOG("[vk] saved %s (%ux%u)\n", path, w, h);
    SDL_DestroySurface(s);
}

// ================================================================== game presentation (metal.m backend_present)
// Draws the game's image into the window with the widescreen crop, then the overlay menu.
extern void vk_present_draw(VkCommandBuffer cb, TexBackend *b, float vx, float vy, float vw, float vh, const float pv[8]);
#ifdef __ANDROID__
// The window around the picture (a 16:9 picture - movies, menus, the file select, Authentic 16:9 - in a wider or
// taller window) filled with the picture's own edges, blurred and dimmed, instead of black (Video > Side bars); and a
// frame of only the game's 16:9 UI box (exec.c g_box_drawn) gets the box's edges over the rest of its target, which
// held another frame's picture (the bars flickered). The picture was drawn into (vx, vy, vw, vh) from the uv crop of
// b; again: the same frame drawn a second time (an in-between one). From LASRecomp-Android-Plus (MIT).
static void bars_backdrop(VkCommandBuffer cb, TexBackend *b, double vx, double vy, double vw, double vh, const float crop[4],
                          bool flip, bool again) {
    extern bool g_box_drawn, g_full_drawn;
    extern void *g_box_rt;
    extern float g_box_frac[4];
    extern void vk_present_backdrop(VkCommandBuffer cb, float x, float y, float w, float h, const float uvr[4], bool flip,
                                    float light);
    static int on = -1;  // LARECOMP_BARS=0: the bars as the target has them
    if (on < 0) on = !getenv("LARECOMP_BARS") || atoi(getenv("LARECOMP_BARS"));
    static bool need;  // (this present's real frame: kept for its in-between frames)
    if (!again) need = g_box_drawn && !g_full_drawn && g_box_rt == (void *)b, g_box_drawn = g_full_drawn = false;
    if (!on || g_settings.black_bars || crop[2] <= 0 || crop[3] <= 0) return;
    {
        double W = g_vk.sc_extent.width, H = g_vk.sc_extent.height, cw = crop[2] * 0.3, ch = crop[3] * 0.3;
        if (vx >= 1) {
            float uv[4] = {crop[0], crop[1], (float)cw, crop[3]};
            vk_present_backdrop(cb, 0, (float)vy, (float)vx, (float)vh, uv, flip, 0.42f);
        }
        if (W - vx - vw >= 1) {
            float uv[4] = {(float)(crop[0] + crop[2] - cw), crop[1], (float)cw, crop[3]};
            vk_present_backdrop(cb, (float)(vx + vw), (float)vy, (float)(W - vx - vw), (float)vh, uv, flip, 0.42f);
        }
        if (vy >= 1) {
            float uv[4] = {crop[0], flip ? (float)(crop[1] + crop[3] - ch) : crop[1], crop[2], (float)ch};
            vk_present_backdrop(cb, 0, 0, (float)W, (float)vy, uv, flip, 0.42f);
        }
        if (H - vy - vh >= 1) {
            float uv[4] = {crop[0], flip ? crop[1] : (float)(crop[1] + crop[3] - ch), crop[2], (float)ch};
            vk_present_backdrop(cb, 0, (float)(vy + vh), (float)W, (float)(H - vy - vh), uv, flip, 0.42f);
        }
    }
    if (!need) return;
    const float *f = g_box_frac;
    double x0 = vx + (f[0] - crop[0]) / crop[2] * vw, x1 = vx + (f[1] - crop[0]) / crop[2] * vw;
    double y0 = vy + (f[2] - crop[1]) / crop[3] * vh, y1 = vy + (f[3] - crop[1]) / crop[3] * vh;
    float bw = f[1] - f[0], bh = f[3] - f[2], light = 0.42f;
    if (x0 - vx >= 1) {  // left, from the box's left third; right, from its right third
        float uv[4] = {f[0], f[2], bw * 0.3f, bh};
        vk_present_backdrop(cb, (float)vx, (float)vy, (float)(x0 - vx), (float)vh, uv, flip, light);
    }
    if (vx + vw - x1 >= 1) {
        float uv[4] = {f[1] - bw * 0.3f, f[2], bw * 0.3f, bh};
        vk_present_backdrop(cb, (float)x1, (float)vy, (float)(vx + vw - x1), (float)vh, uv, flip, light);
    }
    if (y0 - vy >= 1) {  // (a taller window: above and below)
        float uv[4] = {f[0], flip ? f[3] - bh * 0.3f : f[2], bw, bh * 0.3f};
        vk_present_backdrop(cb, (float)x0, (float)vy, (float)(x1 - x0), (float)(y0 - vy), uv, flip, light);
    }
    if (vy + vh - y1 >= 1) {
        float uv[4] = {f[0], flip ? f[2] : f[3] - bh * 0.3f, bw, bh * 0.3f};
        vk_present_backdrop(cb, (float)x0, (float)y1, (float)(x1 - x0), (float)(vy + vh - y1), uv, flip, light);
    }
}
#endif
void renderer_submit(void);
void renderer_set_scale(float sx, float sy);
uint64_t renderer_present_count(void);
void renderer_save_texture_tex(TexBackend *b, const char *path);

static uint64_t frames, total_frames, g_frame_counter;
#ifdef __ANDROID__
// LARECOMP_VK_ASYNC (vulkan/async.c): the game's threads count the presents they queued, the render thread the ones
// it ran - each side's "this frame" stays its own
static uint64_t g_game_frames;
bool vk_async_on(void);
bool vk_async_is_render_thread(void);
void vk_async_present(NvnTexture *t);
void vk_async_drain(void);
uint64_t renderer_present_count(void) { return vk_async_on() && !vk_async_is_render_thread() ? g_game_frames : total_frames; }
#else
uint64_t renderer_present_count(void) { return total_frames; }
#endif
uint64_t vk_frame_counter(void) { return g_frame_counter; }
static float g_shot_crop[4] = {0, 0, 1, 1};
static double last_fps_time;
static NvnTexture *g_last_presented;

const float *vk_shot_crop(void) { return g_shot_crop; }

void backend_present(NvnTexture *t);
#ifdef __ANDROID__
// The game's half of a present: pacing, input, tests - on the game's thread (its frame: renderer_present_count)
static void present_game_half(void) {
    extern void vk_pace_frame(void);
    vk_pace_frame();  // (exec.c: the game's 30 fps mode when frames don't fit 60)
    extern void input_frame(uint64_t frame);
    input_frame(renderer_present_count());
    static long slow_ms = -1;
    if (slow_ms < 0) slow_ms = getenv("LARECOMP_SLOW_FRAME_MS") ? atol(getenv("LARECOMP_SLOW_FRAME_MS")) : 0;
    static long slow_until = -1;  // LARECOMP_SLOW_FRAME_UNTIL=<frame>: the stall ends there (frame pacing recovery tests)
    if (slow_until < 0) slow_until = getenv("LARECOMP_SLOW_FRAME_UNTIL") ? atol(getenv("LARECOMP_SLOW_FRAME_UNTIL")) : 0;
    if (slow_ms > 0 && renderer_present_count() > 2600 && (!slow_until || (long)renderer_present_count() < slow_until)) SDL_Delay((Uint32)slow_ms);
    extern void fnprof_frame(uint64_t frame);
    fnprof_frame(renderer_present_count());
    extern void warp_frame(uint64_t frame);
    warp_frame(renderer_present_count());
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (LARECOMP_REBOOT_AT)
    reboot_test_frame(renderer_present_count());
    extern void save_editor_frame(uint64_t frame);
    save_editor_frame(renderer_present_count());
}

void vk_present_now(NvnTexture *t);
void backend_present(NvnTexture *t) {
    if (!vk_async_on()) {
        vk_present_now(t);
        return;
    }
    g_game_frames++;
    vk_async_present(t);  // (the window's half, on the render thread: vk_present_now)
    present_game_half();
}
#endif
void backend_represent(void) {
    if (g_last_presented) backend_present(g_last_presented);
    else SDL_Delay(16);
}

#ifdef __ANDROID__
void vk_present_now(NvnTexture *t) {  // (the window's half of backend_present)
#else
void backend_present(NvnTexture *t) {
#endif
    g_last_presented = t;
    TexBackend *b = vk_tex(t, true);
    b->presented = true;
    vk_end_pass();
    static const float black[4] = {0, 0, 0, 1};
    uint64_t t0 = SDL_GetTicksNS();
    bool have = vk_frame_begin(black);
    uint64_t nd = SDL_GetTicksNS() - t0;
    if (nd > 30000000) {
        static int warned;
        if (warned++ < 20) LOG("[vk] acquire blocked %llums\n", (unsigned long long)(nd / 1000000));
    }
    if (have) {
        double dw = g_vk.sc_extent.width, dh = g_vk.sc_extent.height;
        extern void widescreen_frame_end(uint64_t frame);
        widescreen_frame_end(total_frames);
        extern void widescreen_set_factor(float f);
        extern bool widescreen_allowed(void);
        // render scale + placement in the window: runtime/nvn/output_scale.c (Video tab "Internal resolution");
        // no GPU timer here (-1): dynamic resolution goes by the frame time
        double gpu_ms = -1;
#ifdef __ANDROID__
        {   // (Android: the GPU's busy time since the last frame, from the submissions' timestamps - exec.c
            // gpu_busy_total, resolved a frame or two late, so per frame it jitters and dynamic resolution averages it;
            // none until the first one resolves, or ever without timestamps). By frame time alone it took a new
            // area's shader compiles - long frames, the GPU idle - for a GPU at its limit (issue #3: down to 0.6)
            extern uint64_t vk_gpu_busy_ns(void);
            static uint64_t last_busy;
            uint64_t busy = vk_gpu_busy_ns();
            if (busy) gpu_ms = last_busy ? (busy - last_busy) / 1e6 : -1;
            last_busy = busy;
        }
#endif
        OutScale os;
        output_scale(dw, dh, widescreen_allowed(), SDL_GetTicksNS() / 1e9, gpu_ms, &os);
#ifdef __ANDROID__
        static int matrix_trace = -1;
        static uint64_t matrix_last;
        if (matrix_trace < 0) matrix_trace = getenv("LARECOMP_BENCHMARK_CONFIG") != NULL;
        if (matrix_trace) {
            uint64_t now = plat_now_ns();
            if (now - matrix_last >= 2000000000ull) {
                matrix_last = now;
                LOG("[matrix-scale] monotonic-ns %llu frame %llu res %d upscale %d aa %d aspect %d render %.0fx%.0f dynamic %.2f\n",
                    (unsigned long long)now, (unsigned long long)total_frames, g_settings.res_mode,
                    g_settings.upscaler, g_settings.antialias, g_settings.aspect,
                    os.sx * 1920.0, os.sy * 1080.0, output_scale_dynamic_factor());
            }
        }
#endif
        double sx = os.sx, sy = os.sy, vx = os.vx, vy = os.vy, vw = os.vw, vh = os.vh, shape = os.shape;
        // interiors / dungeons: their 16:9 centre, or the part inside a room wider than one screen
        extern void widescreen_present_crop(uint64_t, double, float *, double *, double *, double *, double *);
        float crop[4];  // uv origin and size
        widescreen_present_crop(total_frames, shape, crop, &vx, &vy, &vw, &vh);
        widescreen_set_factor((float)shape);
        renderer_set_scale((float)sx, (float)sy);
        VkCommandBuffer cb = vk_cmd();
        float pv[8] = {renderer_origin_lower_left() ? 1.0f : 0.0f, 0, 0, 0, crop[0], crop[1], crop[2], crop[3]};
        memcpy(g_shot_crop, crop, sizeof crop);
        vk_present_draw(cb, b, (float)vx, (float)vy, (float)vw, (float)vh, pv);
#ifdef __ANDROID__
        bars_backdrop(cb, b, vx, vy, vw, vh, crop, pv[0] > 0.5f, false);
#endif
        static long menu_at = -1;
        if (menu_at < 0) menu_at = getenv("LARECOMP_MENU_AT") ? atol(getenv("LARECOMP_MENU_AT")) : 0;
        if (menu_at && (long)total_frames + 1 == menu_at) { extern void menu_test_open(void); menu_test_open(); }
        menu_render((void *)cb, g_vk.sc_extent.width, g_vk.sc_extent.height);
        static long win_shot = -1;
        if (win_shot < 0) win_shot = getenv("LARECOMP_WINDOW_SHOT") ? atol(getenv("LARECOMP_WINDOW_SHOT")) : 0;
        bool shot = win_shot && (long)total_frames + 1 == win_shot;
#ifdef __ANDROID__
        extern bool fg_frame_wanted(uint64_t frame, TexBackend *fin);
        extern bool fg_draw(VkCommandBuffer cb, VkFormat fmt, uint32_t ww, uint32_t wh, TexBackend *fin, const float rect[4],
                            const float crop[4], bool flip_present, float step);
        extern int fg_frames(void);
        extern void fg_frame_done(void);
        bool gen = fg_frame_wanted(total_frames, b);
        g_fg_copy = gen;
        // LARECOMP_FG_SHOT=<frame>: that frame's window picture, its in-between one and the next real one (build/fg_*.png)
        static long fg_shot = -1;
        if (fg_shot < 0) fg_shot = getenv("LARECOMP_FG_SHOT") ? atol(getenv("LARECOMP_FG_SHOT")) : 0;
        bool fs0 = fg_shot && (long)total_frames == fg_shot, fs1 = fg_shot && (long)total_frames == fg_shot + 1;
        vk_frame_end(shot ? "build/window.png" : fs0 ? "build/fg_real0.png" : fs1 ? "build/fg_real1.png" : NULL);
        // the in-between frames: the next swapchain images, each presented a refresh after the one before (FIFO) -
        // one for the 30 fps mode at 60 Hz or 60 fps at 120 Hz, three for 30 fps at 120 Hz (framegen.c fg_frames)
        for (int k = 1, n = gen ? fg_frames() : 0; k <= n && vk_frame_begin(black); k++) {
            float rect[4] = {(float)vx, (float)vy, (float)vw, (float)vh};
            fg_draw(vk_cmd(), g_vk.sc_format, g_vk.sc_extent.width, g_vk.sc_extent.height, b, rect, crop, pv[0] > 0.5f,
                    (float)k / (n + 1));
            bars_backdrop(vk_cmd(), b, vx, vy, vw, vh, crop, pv[0] > 0.5f, true);
            extern bool fg_draws_overlays(void);  // (drawn from the presented picture: the window's overlays on top again)
            extern void menu_redraw(void *cmd);
            if (fg_draws_overlays()) menu_redraw((void *)vk_cmd());
            vk_frame_end(fs0 && k == 1 ? "build/fg_gen0.png" : NULL);
        }
        if (gen) fg_frame_done();
#else
        vk_frame_end(shot ? "build/window.png" : NULL);
#endif
        if (shot && getenv("LARECOMP_EXIT_AFTER_SHOT")) { fflush(stderr); _exit(0); }
    } else {
        renderer_submit();
    }
    extern void vk_prof_frame(uint64_t frame, uint64_t present_ns);
    vk_prof_frame(total_frames, SDL_GetTicksNS() - t0);
    frames++;
    total_frames++;
    g_frame_counter++;
#ifdef __ANDROID__
    vk_timing_trace_frame(total_frames);
    if (!vk_async_on()) present_game_half();  // (same place as before; LARECOMP_VK_ASYNC: on the game's thread)
#else
    extern void vk_pace_frame(void);
    vk_pace_frame();  // (exec.c: the game's 30 fps mode when frames don't fit 60)
    extern void input_frame(uint64_t frame);
    input_frame(total_frames);
    static long slow_ms = -1;
    if (slow_ms < 0) slow_ms = getenv("LARECOMP_SLOW_FRAME_MS") ? atol(getenv("LARECOMP_SLOW_FRAME_MS")) : 0;
    static long slow_until = -1;  // LARECOMP_SLOW_FRAME_UNTIL=<frame>: the stall ends there (frame pacing recovery tests)
    if (slow_until < 0) slow_until = getenv("LARECOMP_SLOW_FRAME_UNTIL") ? atol(getenv("LARECOMP_SLOW_FRAME_UNTIL")) : 0;
    if (slow_ms > 0 && total_frames > 2600 && (!slow_until || (long)total_frames < slow_until)) SDL_Delay((Uint32)slow_ms);
    extern void fnprof_frame(uint64_t frame);
    fnprof_frame(total_frames);
    extern void warp_frame(uint64_t frame);
    warp_frame(total_frames);
    extern void reboot_test_frame(uint64_t frame);  // runtime/main.c (LARECOMP_REBOOT_AT)
    reboot_test_frame(total_frames);
    extern void save_editor_frame(uint64_t frame);
    save_editor_frame(total_frames);
#endif
    extern void vk_level_change_check(void);
    vk_level_change_check();
    static long shot_every = -1;
    if (shot_every < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_EVERY"); shot_every = e ? atol(e) : 0; }
    extern bool g_debug_frame;
    static long debug_at = -1;
    if (debug_at < 0) { const char *e = getenv("LARECOMP_DEBUG_FRAME"); debug_at = e ? atol(e) : 0; }
    extern volatile int g_debug_frame_request;
    if (g_debug_frame_request) { g_debug_frame_request = 0; debug_at = (long)total_frames + 1; }
    // LARECOMP_DEBUG_FRAMES=<n> (with LARECOMP_DEBUG_FRAME / F9): dump n consecutive frames (build/pass_<frame>_NNN.png)
    static long debug_n = -1;
    if (debug_n < 0) { const char *e = getenv("LARECOMP_DEBUG_FRAMES"); debug_n = e && atol(e) > 1 ? atol(e) : 1; }
    g_debug_frame = debug_at && total_frames >= (uint64_t)debug_at && total_frames < (uint64_t)debug_at + (uint64_t)debug_n;
    {   // LARECOMP_GPUPROF_AT=<frame>[,<frame>...]: GPU time per pass of those frames ("[gpuprof]" in the log)
        extern bool g_gpuprof_frame;
        extern void vk_gpuprof_report(void);
        if (g_gpuprof_frame) { g_gpuprof_frame = false; vk_gpuprof_report(); }
        const char *e = getenv("LARECOMP_GPUPROF_AT");
        for (const char *p = e; p && *p;) {
            long at = strtol(p, (char **)&p, 10);
            if (at == (long)total_frames) { g_gpuprof_frame = true; break; }
            if (*p == ',') p++; else break;
        }
    }
    if (g_debug_frame) LOG("===== debug frame %llu =====\n", (unsigned long long)total_frames);
    {   // LARECOMP_SHOT_AT=<frame>,<frame>,... (ascending): the game image at those frames -> build/frames/frame_N.png
        static const char *next;
        static int init;
        if (!init) { init = 1; next = getenv("LARECOMP_SHOT_AT"); if (next) mkdir("build/frames", 0755); }
        while (next && *next) {
            long at = strtol(next, (char **)&next, 10);
            if ((long)total_frames < at) { while (next > getenv("LARECOMP_SHOT_AT") && next[-1] != ',') next--; break; }
            if ((long)total_frames == at) {
                char path[256];
                snprintf(path, sizeof path, "build/frames/frame_%05llu.png", (unsigned long long)total_frames);
                renderer_save_texture_tex(b, path);
            }
            if (*next == ',') next++;
        }
    }
    static long shot_from = -1;
    if (shot_from < 0) { const char *e = getenv("LARECOMP_SCREENSHOT_FROM"); shot_from = e ? atol(e) : 0; }
    if (shot_every && total_frames % shot_every == 0 && (long)total_frames >= shot_from) {
        char path[256];
        if (shot_from) mkdir("build/frames", 0755);
        snprintf(path, sizeof path, shot_from ? "build/frames/frame_%05llu.png" : "build/frame_%05llu.png",
                 (unsigned long long)total_frames);
        renderer_save_texture_tex(b, path);
        // a series (SCREENSHOT_FROM) runs on until LARECOMP_WINDOW_SHOT exits
        if (getenv("LARECOMP_EXIT_AFTER_SHOT") && !shot_from) { fflush(stderr); _exit(0); }
    }
    double now = SDL_GetTicks() / 1000.0;
    static double fps_iv = -1, prev_present, max_gap;
    if (fps_iv < 0) fps_iv = getenv("LARECOMP_FPS_INTERVAL") ? atof(getenv("LARECOMP_FPS_INTERVAL")) : 2.0;
    if (prev_present > 0 && now - prev_present > max_gap) max_gap = now - prev_present;
    prev_present = now;
    if (now - last_fps_time >= fps_iv) {
        extern const char *game_level_name(void);
#ifdef __ANDROID__
        // (Android) and the GPU's busy time per frame over the interval (exec.c gpu_busy_total, from the submissions'
        // timestamps; "-" without them): whether the GPU or the CPU holds the frame rate (issue #2: a Mali at 15-19 fps)
        extern uint64_t vk_gpu_busy_ns(void);
        static uint64_t busy_last;
        uint64_t busy = vk_gpu_busy_ns();
        char gpu[24] = "-";
        if (busy && busy_last && frames) snprintf(gpu, sizeof gpu, "%.1fms", (busy - busy_last) / 1e6 / frames);
        busy_last = busy;
        if (last_fps_time > 0)
            LOG("[vk] t=%.1f frame %llu %.1f fps max %.0fms gpu %s level %s (%s)\n", now, (unsigned long long)total_frames,
                frames / (now - last_fps_time), max_gap * 1000.0, gpu, game_level_name(), renderer_stats());
#else
        if (last_fps_time > 0)
            LOG("[vk] t=%.1f frame %llu %.1f fps max %.0fms level %s (%s)\n", now, (unsigned long long)total_frames,
                frames / (now - last_fps_time), max_gap * 1000.0, game_level_name(), renderer_stats());
#endif
        max_gap = 0;
        frames = 0;
        last_fps_time = now;
    }
}

// ================================================================== UI thread
void vk_exec_init(void);
void vk_shaders_init(void);

// exit() from any thread: let the GPU finish, keep the pipeline cache, then end the process without the DLL
// teardown (driver / validation-layer threads can stall it for minutes while the game's threads still run).
static void quick_exit_handler(void) {
    static int trace = -1;
    if (trace < 0) trace = getenv("LARECOMP_VK_TRACE") != NULL;
    if (trace) LOG("[vk] exit: waiting for the GPU\n");
    if (g_vk.dev) {
        vkDeviceWaitIdle(g_vk.dev);
        if (trace) LOG("[vk] exit: saving the pipeline cache\n");
        vk_pipeline_cache_save();
    }
    if (trace) LOG("[vk] exit: terminating\n");
    fflush(stdout);
    fflush(stderr);
#if defined(_WIN32) || defined(__linux__)
    plat_terminate();
#endif
}

#ifdef __ANDROID__
// Driver compatibility's Automatic (settings.h gpu_heap / turnip_lrz): the fastest paths on the Adreno 8xx GPUs they
// were made on, the compatible ones elsewhere (an Adreno 750 drew text and colors wrong with them). name: Vulkan's
// "Adreno (TM) 830v1" or the kernel's "Adreno830v2".
static int adreno_number(const char *name) {
    const char *a = name ? strstr(name, "Adreno") : NULL;
    if (!a) return 0;
    while (*a && (*a < '0' || *a > '9')) a++;
    return atoi(a);
}
bool vk_adreno_8xx(const char *name) {
    int n = adreno_number(name);
    return n >= 800 && n < 900;
}
// LRZ's Automatic: on for the Adreno 7xx too - that 750's text and colors were the 128 MB storage range (fixed in
// 0.5.0), and with LRZ on it draws right at 55-60 fps (issue #3)
bool vk_lrz_auto(const char *name) {
    int n = adreno_number(name);
    return n >= 700 && n < 900;
}
static bool g_lrz_at_start = true;
bool vk_lrz_at_start(void) { return g_lrz_at_start; }  // (the start screen: a change needs a restart)
const char *vk_gpu_name(void) { return g_vk.props.deviceName; }
#endif

void ui_window_init(void) {
    if (window) return;
    atexit(quick_exit_handler);
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");  // (the back button opens the menu: ui/touch_controls.mm)
#endif
    settings_load();
#ifdef __ANDROID__
    {   // Driver compatibility (the start screen's Saves & files tab): applied before the driver loads
        extern void android_log_file(bool on);
        android_log_file(g_settings.log_file);
        // Turnip reads TU_DEBUG once, when it starts: low-resolution Z off (it can drop draws on some Adreno GPUs).
        // Automatic needs the GPU before Vulkan names it: the kernel's (the GPU driver reads it from the app too), else
        // the last start's; unknown: on.
        int lrz = g_settings.turnip_lrz;
        char model[64] = "";
        if (lrz < 0) {
            FILE *f = fopen("/sys/class/kgsl/kgsl-3d0/gpu_model", "r");
            if (f) {
                if (!fgets(model, sizeof model, f)) model[0] = 0;
                fclose(f);
                model[strcspn(model, "\r\n")] = 0;
            }
            if (!model[0]) snprintf(model, sizeof model, "%s", g_settings.gpu_model);
            lrz = !model[0] || vk_lrz_auto(model);
        }
        g_lrz_at_start = lrz != 0;
        if (!lrz && !getenv("TU_DEBUG")) setenv("TU_DEBUG", "nolrz", 1);
        LOG("[vk] Turnip LRZ %s (%s%s)%s%s\n", lrz ? "on" : "off", g_settings.turnip_lrz < 0 ? "automatic: " : "settings",
            g_settings.turnip_lrz < 0 ? (model[0] ? model : "GPU unknown") : "", getenv("TU_DEBUG") ? ", TU_DEBUG=" : "",
            getenv("TU_DEBUG") ? getenv("TU_DEBUG") : "");
    }
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) FATAL("SDL_Init: %s\n", SDL_GetError());
    int win_w = 1280, win_h = 720;
    if (getenv("LARECOMP_WINDOW")) sscanf(getenv("LARECOMP_WINDOW"), "%dx%d", &win_w, &win_h);
    SDL_WindowFlags wflags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#ifdef __ANDROID__
    wflags |= SDL_WINDOW_FULLSCREEN;  // (immersive: no system bars)
#endif
    if (getenv("LARECOMP_HIDDEN")) wflags |= SDL_WINDOW_HIDDEN;
    {   // scripted test runs (user input ignored: runtime/hle/input.c input_user_blocked) open without taking the
        // keyboard focus from whatever the player is doing
        const char *v = getenv("LARECOMP_NO_USER_INPUT");
        bool scripted = v ? atoi(v) != 0 : (getenv("LARECOMP_INPUT") || getenv("LARECOMP_DOORJUMP_TEST") || getenv("LARECOMP_PLACE") ||
                                            getenv("LARECOMP_HOST_INPUT") || getenv("LARECOMP_BIND_TEST"));
        if (scripted) SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    }
    window = SDL_CreateWindow("LAS Recomp", win_w, win_h, wflags);
#ifdef __ANDROID__
    // opened with the screen locked / off: no surface to make the window on yet - it comes once the activity shows.
    // SDL_PumpEvents sleeps while the app is paused, so the 5 s count only time in the foreground: a window that fails
    // for another reason shows the error below instead of a black screen (0.5.0-0.5.3 waited 10 minutes for any)
    for (int tries = 0; !window && tries < 20 * 5; tries++) {
        if (tries == 0) LOG("[vk] waiting for the Android window (%s)\n", SDL_GetError());
        SDL_PumpEvents();
        SDL_Delay(50);
        window = SDL_CreateWindow("LAS Recomp", win_w, win_h, wflags);
    }
#endif
    if (!window) FATAL("SDL_CreateWindow: %s\n", SDL_GetError());
    rt_window_icon(window);
    // the Fullscreen setting applies from the start screen on (not only once the game runs); test windows stay windows
#ifdef __ANDROID__
    SDL_SetWindowFullscreen(window, true);  // (always: the whole screen, system bars hidden)
#else
    if (g_settings.fullscreen && !getenv("LARECOMP_WINDOW") && !getenv("LARECOMP_HIDDEN")) SDL_SetWindowFullscreen(window, true);
#endif
    create_instance();
#ifdef __ANDROID__
    {   // (through the driver volk loaded: SDL would use the system loader's)
        struct ANativeWindow *aw = android_window();
        // (opened with the screen locked: the surface went away after the window was made - it comes back once the
        // activity shows)
        for (int tries = 0; !aw && tries < 20 * 5; tries++) {  // (5 s in the foreground, as above)
            if (tries == 0) LOG("[vk] waiting for the Android window (the screen is locked or off)\n");
            SDL_PumpEvents();
            SDL_Delay(50);
            aw = android_window();
        }
        if (!aw) FATAL("no Android window\n");
        android_create_surface(aw);
    }
#else
    if (!SDL_Vulkan_CreateSurface(window, g_vk.inst, NULL, &g_vk.surface)) FATAL("SDL_Vulkan_CreateSurface: %s\n", SDL_GetError());
#endif
    pick_device();
#ifdef __ANDROID__
    if (getenv("LARECOMP_VK_STORAGE_RANGE_MB")) {  // (a test: a storage buffer reaching less, as Turnip's 128 MB on Adreno 7xx)
        uint64_t mb = strtoull(getenv("LARECOMP_VK_STORAGE_RANGE_MB"), NULL, 0);
        if (mb && (mb << 20) < g_vk.props.limits.maxStorageBufferRange) g_vk.props.limits.maxStorageBufferRange = (uint32_t)(mb << 20);
    }
#endif
    create_device();
#ifdef __ANDROID__
    if (strcmp(g_settings.gpu_model, g_vk.props.deviceName)) {  // (for turnip_lrz's Automatic at the next start)
        snprintf(g_settings.gpu_model, sizeof g_settings.gpu_model, "%s", g_vk.props.deviceName);
        settings_save();
    }
#endif
    create_swapchain();
    vk_exec_init();
    vk_shaders_init();
}

SDL_Window *ui_window(void) { return window; }

#ifdef __ANDROID__
// For the performance overlay (ui/perf_overlay.mm): the GPU and its driver, short ("Adreno (TM) 830 - Turnip").
const char *vk_gpu_desc(void) {
    static char s[160];
    if (s[0] || !g_vk.phys) return s;
    VkPhysicalDeviceDriverProperties d = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &d};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p);
    const char *drv = strstr(d.driverName, "turnip") ? "Turnip" : strstr(d.driverName, "Qualcomm") ? "Qualcomm" : d.driverName;
    snprintf(s, sizeof s, "%s - %s", p.properties.deviceName, drv);
    return s;
}

// Qualcomm's own driver (not Turnip): its shader compiler crashed on the structured control flow (vulkan/exec.c)
bool vk_driver_is_qualcomm(void) {
    static int q = -1;
    if (q < 0 && g_vk.phys) {
        VkPhysicalDeviceDriverProperties d = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &d};
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p);
        q = d.driverID == VK_DRIVER_ID_QUALCOMM_PROPRIETARY;
    }
    return q > 0;
}
#endif

void backend_init(void) {
    while (!atomic_load(&ui_ready)) SDL_Delay(1);
    LOG("[vk] %s, swapchain %ux%u\n", g_vk.props.deviceName, g_vk.sc_extent.width, g_vk.sc_extent.height);
}

// ---- in-place reboot (runtime/main.c teardown; main thread, guest threads stopped)
void backend_reset(void) {
    if (!g_vk.dev) return;
#ifdef __ANDROID__
    vk_async_drain();
#endif
    vkDeviceWaitIdle(g_vk.dev);
    extern void vk_textures_reset(void);
    extern void vk_exec_reset(void);
    vk_textures_reset();
    vk_exec_reset();
    g_last_presented = NULL;
}
void backend_guest_memory_reset(void) {}  // (exec.c re-imports the heap on first use)
void backend_clear_window(void) {
    static const float black[4] = {0, 0, 0, 1};
#ifdef __ANDROID__
    vk_async_drain();
#endif
    if (g_vk.dev && vk_frame_begin(black)) vk_frame_end(NULL);
}

#ifdef __ANDROID__
void vk_wait_ready(void) {
    while (!atomic_load(&ui_ready)) SDL_Delay(1);
}
#endif

int ui_run(void) {
    ui_window_init();
    atomic_store(&ui_ready, true);
    for (;;) {
        if (rt_reboot_requested()) return rt_reboot_requested();  // (runtime/main.c tears the game down)
        SDL_Event e;
#ifdef __ANDROID__
        // (SDL's wait doesn't sleep on Android: it kept a core busy updating joysticks and reading the clock - 40% of
        // the app's CPU in a profile. Poll what's there, then sleep: input at most 4 ms later.)
        static bool polled;
        if (polled) SDL_Delay(4);
        polled = true;
        while (SDL_PollEvent(&e)) {
#else
        while (SDL_WaitEventTimeout(&e, 16)) {
#endif
#ifdef __ANDROID__
            if (e.type == SDL_EVENT_WILL_ENTER_BACKGROUND && g_vk.dev) {  // (may not come back)
                vk_pipeline_cache_save();
                settings_save();  // (options changed with the menu still open were lost: from LASRecomp-Android-Plus)
            }
#endif
            if (e.type == SDL_EVENT_QUIT) {
                LOG("window closed\n");
                exit(0);
            }
            extern bool input_user_blocked(const SDL_Event *e);
            if (input_user_blocked(&e)) continue;  // scripted test runs: the real keyboard / mouse / pads do nothing (as metal.m)
#ifdef __ANDROID__
            extern bool touch_controls_event(const SDL_Event *e);  // (ui/touch_controls.mm: fingers, the back button)
            if (touch_controls_event(&e)) continue;
#endif
            if (menu_handle_event(&e)) continue;
            input_handle_event(&e);
        }
        menu_apply_window_requests(window);
#ifdef __ANDROID__
        {   // the pipeline cache to disk now and then while it grows: an app is killed rather than exited, so
            // quick_exit_handler never saves it (every start compiled every shader again: missing objects at first)
            static uint64_t last;
            static size_t saved;
            uint64_t now = SDL_GetTicks();
            if (g_vk.dev && now - last > 30000) {
                last = now;
                size_t sz = 0;
                if (vkGetPipelineCacheData(g_vk.dev, g_vk.pcache, &sz, NULL) == VK_SUCCESS && sz > saved + 65536) {
                    vk_pipeline_cache_save();
                    saved = sz;
                    LOG("[vk] pipeline cache saved (%zu KB)\n", sz >> 10);
                }
            }
        }
#endif
    }
}
