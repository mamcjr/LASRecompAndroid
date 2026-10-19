// Performance overlay on Android (the in-game menu's Show FPS, ui/menu.mm; on by default there), like the ones Android
// emulators show: frame rate and frame time with a graph, CPU clusters and this app's CPU share, the GPU and its driver
// and how much of the time the CPU waits on it (apps can't read the GPU's own load), memory, temperatures, battery
// power and Android's thermal state (runtime/platform/android.c plat_device_stats; the battery and thermal state come
// from the activity, LASRecompActivity.reportDeviceStats).
#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <stdio.h>

#include "imgui.h"

extern "C" {
#include "platform.h"
uint64_t vk_cum_wait_ns(void);  // vulkan/exec.c
const char *vk_gpu_desc(void);  // vulkan/device.c
}

#define FT_N 120  // frame times in the graph

static ImVec4 grade(float v, float good, float ok) {  // green / yellow / red
    return v >= good ? ImVec4(0.45f, 0.95f, 0.45f, 1) : v >= ok ? ImVec4(1.0f, 0.85f, 0.3f, 1) : ImVec4(1.0f, 0.4f, 0.35f, 1);
}

void perf_overlay_draw(ImVec2 display, float scale) {
    (void)display;
    // frame times: this runs once per presented frame
    static float ft[FT_N];
    static int nft, head;
    static uint64_t last_ns;
    uint64_t now = SDL_GetTicksNS();
    if (last_ns) {
        ft[head] = (float)((now - last_ns) / 1e6);
        head = (head + 1) % FT_N;
        if (nft < FT_N) nft++;
    }
    last_ns = now;

    // the rest twice a second
    static uint64_t next_ns, t0, wait0;
    static int frames;
    static float fps, avg_ms, max_ms, wait_pct;
    static PlatDeviceStats ds;
    frames++;
    if (now >= next_ns) {
        uint64_t w = vk_cum_wait_ns();
        if (t0) {
            double dt = (now - t0) / 1e9;
            fps = (float)(frames / dt);
            wait_pct = (float)((w - wait0) / 1e9 / dt * 100.0);
            if (wait_pct > 100) wait_pct = 100;
        }
        wait0 = w;
        frames = 0;
        t0 = now;
        next_ns = now + 500000000ull;
        float s = 0, mx = 0;
        for (int i = 0; i < nft; i++) { s += ft[i]; if (ft[i] > mx) mx = ft[i]; }
        avg_ms = nft ? s / nft : 0;
        max_ms = mx;
        plat_device_stats(&ds);
    }

    ImGui::SetNextWindowPos(ImVec2(16 * scale, 16 * scale));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##perf", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(0.62f);

    ImGui::TextColored(grade(fps, 55, 28), "%.0f FPS", fps);
    ImGui::SameLine();
    ImGui::TextDisabled("%.1f ms", fps > 0 ? 1000.0f / fps : 0.0f);
    float graph[FT_N];  // (oldest first)
    for (int i = 0; i < nft; i++) graph[i] = ft[(head - nft + i + FT_N) % FT_N];
    ImGui::PushStyleColor(ImGuiCol_PlotLines, grade(fps, 55, 28));
    ImGui::PlotLines("##ft", graph, nft, 0, NULL, 0.0f, 100.0f, ImVec2(260 * scale, 44 * scale));
    ImGui::PopStyleColor();
    ImGui::TextDisabled("frame avg %.1f ms  max %.0f ms", avg_ms, max_ms);

    char cpu[96] = "";
    for (int i = 0, n = 0; i < ds.ncluster && n < (int)sizeof cpu - 8; i++)
        n += snprintf(cpu + n, sizeof cpu - n, "%s%u", i ? " / " : "", ds.cluster_mhz[i]);
    ImGui::Text("CPU %s MHz", cpu);
    ImGui::SameLine();
    ImGui::TextDisabled("app %.0f%%", ds.cpu_pct);

    PlatAdpfStats adpf;
    plat_adpf_stats(&adpf);
    if (adpf.state > 0) {
        ImGui::TextColored(grade(adpf.target_ms - adpf.work_ms, 3.0f, 0.0f), "ADPF on (%d thr%s)",
                           adpf.threads, adpf.sessions > 1 ? ", split" : "");
        if (adpf.errors > 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.35f, 1), "err %d (%d)", adpf.errors, adpf.last_error);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("tgt %.1f ms", adpf.target_ms);
        if (adpf.render_ms > 0) {
            ImGui::TextDisabled("    game %.1f ms (cpu %d)  render %.1f ms (cpu %d)",
                                adpf.game_ms, adpf.game_cpu, adpf.render_ms, adpf.render_cpu);
        } else {
            ImGui::TextDisabled("    game %.1f ms (cpu %d)", adpf.game_ms, adpf.game_cpu);
        }
    } else if (adpf.state == 0) {
        ImGui::TextDisabled("ADPF: init...");
    } else {
        ImGui::TextDisabled("ADPF: %s", adpf.state == -2 ? "disabled" : "unavailable");
    }
    ImGui::Text("GPU %s", vk_gpu_desc());
    ImGui::TextColored(grade(100 - wait_pct, 50, 20), "    waiting on the GPU %.0f%%", wait_pct);
    ImGui::Text("RAM app %u MB", ds.rss_mb);
    ImGui::SameLine();
    ImGui::TextDisabled("free %u MB", ds.avail_mb);

    if (ds.soc_temp_c > 0) {
        ImGui::TextColored(grade(100 - ds.soc_temp_c, 35, 20), "SoC %.0f\xc2\xb0" "C", ds.soc_temp_c);
        ImGui::SameLine();
    }
    if (ds.batt_temp_c > 0) ImGui::TextColored(grade(100 - ds.batt_temp_c, 60, 55), "Battery %.0f\xc2\xb0" "C", ds.batt_temp_c);
    else ImGui::NewLine();
    if (ds.batt_pct >= 0) {
        if (ds.watts > 0) { ImGui::Text("Power %.1f W", ds.watts); ImGui::SameLine(); }
        ImGui::TextDisabled("battery %.0f%%", ds.batt_pct);
    }
    static const char *const thermal[] = {"normal", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
    if (ds.thermal >= 0 && ds.thermal <= 6) {
        ImGui::TextColored(grade((float)(6 - ds.thermal), 6, 4), "Thermal: %s", thermal[ds.thermal]);
        if (ds.headroom > 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("(headroom %.2f)", ds.headroom);
        }
    }
    ImGui::End();
}
#endif
