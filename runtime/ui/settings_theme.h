#pragma once
#include "imgui.h"

// Android settings: warm inventory paper, carved wood and island greens. Scoped so
// the launcher, touch controls and performance overlay keep their own contrast.
inline void settings_theme_apply() {
#ifdef __ANDROID__
    ImVec4 *c = ImGui::GetStyle().Colors;
    const ImVec4 ink(0.20f, 0.25f, 0.19f, 1), paper(0.96f, 0.92f, 0.81f, 1);
    const ImVec4 leaf(0.69f, 0.79f, 0.52f, 1), selected(0.57f, 0.71f, 0.40f, 1);
    const ImVec4 wood(0.49f, 0.37f, 0.22f, 1), green(0.22f, 0.38f, 0.24f, 1);
    c[ImGuiCol_Text] = ink;
    c[ImGuiCol_TextDisabled] = ImVec4(0.39f, 0.40f, 0.31f, 1);
    c[ImGuiCol_WindowBg] = c[ImGuiCol_ChildBg] = paper;
    c[ImGuiCol_PopupBg] = ImVec4(0.99f, 0.96f, 0.87f, 1);
    c[ImGuiCol_Border] = ImVec4(0.60f, 0.50f, 0.33f, 0.65f);
    c[ImGuiCol_FrameBg] = ImVec4(0.88f, 0.84f, 0.70f, 1);
    c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_ButtonHovered] = c[ImGuiCol_HeaderHovered] = leaf;
    c[ImGuiCol_FrameBgActive] = c[ImGuiCol_ButtonActive] = c[ImGuiCol_HeaderActive] = selected;
    c[ImGuiCol_Button] = ImVec4(0.84f, 0.85f, 0.64f, 1);
    c[ImGuiCol_Header] = selected;
    c[ImGuiCol_CheckMark] = green;
    c[ImGuiCol_SliderGrab] = green;
    c[ImGuiCol_SliderGrabActive] = ink;
    c[ImGuiCol_Separator] = ImVec4(0.56f, 0.47f, 0.30f, 0.40f);
    c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = wood;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.85f, 0.81f, 0.67f, 0.55f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.56f, 0.57f, 0.39f, 1);
    c[ImGuiCol_ScrollbarGrabHovered] = c[ImGuiCol_ScrollbarGrabActive] = green;
    c[ImGuiCol_NavCursor] = green;
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.40f, 0.60f, 0.32f, 0.35f);
    c[ImGuiCol_TextLink] = green;
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.84f, 0.82f, 0.66f, 1);
    c[ImGuiCol_TableBorderStrong] = c[ImGuiCol_Border];
    c[ImGuiCol_TableBorderLight] = c[ImGuiCol_Separator];
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(0.35f, 0.39f, 0.23f, 0.06f);
#endif
}

struct SettingsThemeScope {
    ImGuiStyle saved;
    SettingsThemeScope() : saved(ImGui::GetStyle()) { settings_theme_apply(); }
    ~SettingsThemeScope() { ImGui::GetStyle() = saved; }
};

inline ImVec4 settings_title_color() {
#ifdef __ANDROID__
    return ImVec4(0.22f, 0.38f, 0.24f, 1);
#else
    return ImVec4(0.93f, 0.76f, 0.33f, 1);
#endif
}

inline void settings_theme_frame(float scale) {
#ifdef __ANDROID__
    ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p, ImVec2(p.x + sz.x, p.y + sz.y), true);
    const ImU32 wood = IM_COL32(126, 96, 57, 255), trim = IM_COL32(190, 162, 107, 255);
    float inset = 3 * scale;
    dl->AddRect(ImVec2(p.x + inset, p.y + inset), ImVec2(p.x + sz.x - inset, p.y + sz.y - inset),
                wood, 14 * scale, 0, 3 * scale);
    inset = 9 * scale;
    dl->AddRect(ImVec2(p.x + inset, p.y + inset), ImVec2(p.x + sz.x - inset, p.y + sz.y - inset),
                trim, 9 * scale, 0, scale);
    dl->PopClipRect();
#endif
}

inline ImVec4 settings_warning_color() {
#ifdef __ANDROID__
    return ImVec4(0.61f, 0.24f, 0.17f, 1);
#else
    return ImVec4(1, 0.5f, 0.4f, 1);
#endif
}
