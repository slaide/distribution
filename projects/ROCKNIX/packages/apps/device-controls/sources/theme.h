// SPDX-License-Identifier: GPL-2.0
// Shared visual theme for device-controls (SDL2 control panel + --sidebar
// overlay). Mirrors the konkr-launcher look so the two apps read as one family:
// slate darks with a single amber accent reserved for "where you are", real
// vector fonts at native pixel size (not the scaled bitmap default, the biggest
// "prototype look" offender), and rounded panels.
//
// Header-only: all definitions are inline, so both main.cpp and sidebar.cpp can
// include it without a separate translation unit. The shared per-process state
// (g_ui, g_font*) is declared extern here and defined once in main.cpp.
#pragma once

#include <cmath>
#include <unistd.h>
#include "imgui.h"

// ------------------------------------------------------------------ palette

static inline ImVec4 dc_rgb(unsigned hex, float a = 1.0f)
{
    return ImVec4(((hex >> 16) & 0xff) / 255.f, ((hex >> 8) & 0xff) / 255.f,
                  (hex & 0xff) / 255.f, a);
}

// Slate darks + one amber accent (identical to konkr-launcher). C_OK / C_INFO
// are the two extra data/state hues this utility needs: green = "on/active",
// cyan = "editable data" (the fan curve you are dragging). The accent stays
// reserved for selection and nav.
static const ImVec4 C_BG        = dc_rgb(0x0E1116);
static const ImVec4 C_PANEL     = dc_rgb(0x151A22);
static const ImVec4 C_TILE      = dc_rgb(0x1B212B);
static const ImVec4 C_TILE_HI   = dc_rgb(0x272F3C);
static const ImVec4 C_TEXT      = dc_rgb(0xE8EAED);
static const ImVec4 C_DIM       = dc_rgb(0x8A93A0);
static const ImVec4 C_ACCENT    = dc_rgb(0xF0A83C);
static const ImVec4 C_ACCENT_BG = dc_rgb(0xF0A83C, 0.16f);
static const ImVec4 C_WARN      = dc_rgb(0xE05A4E);
static const ImVec4 C_OK        = dc_rgb(0x5BC873);
static const ImVec4 C_INFO      = dc_rgb(0x5AB6E0);

// ----------------------------------------------- shared per-process UI state
// Defined in main.cpp; sidebar.cpp reads them. Only one entry point runs per
// process, so each loads its own fonts into its own ImGui atlas.
extern float   g_ui;          // display scale = window height / 720, clamped >= 1
extern ImFont *g_font;        // body (null => ImGui default bitmap fallback)
extern ImFont *g_font_bold;   // brand / titles
extern ImFont *g_font_small;  // hints / captions

// ------------------------------------------------------------------- fonts

// Load Liberation Sans at native pixel size scaled by `ui`; fall back to the
// built-in bitmap font (globally scaled) when the TTFs are missing. Must run
// before the backend builds its font atlas. Sets g_font / g_font_bold /
// g_font_small.
static inline void theme_load_fonts(ImGuiIO &io, float ui)
{
    const char *FONT      = "/usr/share/fonts/liberation/LiberationSans-Regular.ttf";
    const char *FONT_BOLD = "/usr/share/fonts/liberation/LiberationSans-Bold.ttf";
    g_font = g_font_bold = g_font_small = nullptr;
    if (access(FONT, F_OK) == 0) {
        g_font       = io.Fonts->AddFontFromFileTTF(FONT, roundf(20.0f * ui));
        g_font_small = io.Fonts->AddFontFromFileTTF(FONT, roundf(16.0f * ui));
    }
    if (access(FONT_BOLD, F_OK) == 0)
        g_font_bold = io.Fonts->AddFontFromFileTTF(FONT_BOLD, roundf(24.0f * ui));
    if (!g_font) {
        io.Fonts->AddFontDefault();
        io.FontGlobalScale = 1.6f * ui;
    }
}

// ------------------------------------------------------------------- style

// Apply the launcher's rounded slate style at scale `ui` (metrics are absolute
// pixels, so this replaces ScaleAllSizes + FontGlobalScale entirely).
static inline void theme_apply_style(float ui)
{
    ImGuiStyle &st = ImGui::GetStyle();
    st = ImGuiStyle();

    st.WindowPadding     = ImVec2(18 * ui, 16 * ui);
    st.FramePadding      = ImVec2(12 * ui, 7 * ui);
    st.CellPadding       = ImVec2(8 * ui, 5 * ui);
    st.ItemSpacing       = ImVec2(11 * ui, 9 * ui);
    st.ItemInnerSpacing  = ImVec2(8 * ui, 6 * ui);
    st.ScrollbarSize     = 12 * ui;
    st.GrabMinSize       = 14 * ui;
    st.IndentSpacing     = 22 * ui;
    st.WindowRounding    = 0;
    st.ChildRounding     = 10 * ui;
    st.FrameRounding     = 7 * ui;
    st.PopupRounding     = 10 * ui;
    st.GrabRounding      = 6 * ui;
    st.TabRounding       = 8 * ui;
    st.ScrollbarRounding = 8 * ui;
    st.WindowBorderSize  = 0;
    st.ChildBorderSize   = 1;
    st.PopupBorderSize   = 1;
    st.FrameBorderSize   = 0;
    st.TabBarBorderSize  = 2 * ui;
    st.SeparatorTextBorderSize = 2 * ui;

    ImVec4 *c = st.Colors;
    c[ImGuiCol_WindowBg]              = C_BG;
    c[ImGuiCol_ChildBg]               = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]               = C_PANEL;
    c[ImGuiCol_Border]                = dc_rgb(0x2A3340);
    c[ImGuiCol_BorderShadow]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Text]                  = C_TEXT;
    c[ImGuiCol_TextDisabled]          = C_DIM;
    c[ImGuiCol_Button]                = C_TILE;
    c[ImGuiCol_ButtonHovered]         = C_TILE_HI;
    c[ImGuiCol_ButtonActive]          = dc_rgb(0xF0A83C, 0.35f);
    c[ImGuiCol_FrameBg]               = C_TILE;
    c[ImGuiCol_FrameBgHovered]        = C_TILE_HI;
    c[ImGuiCol_FrameBgActive]         = C_TILE_HI;
    c[ImGuiCol_Header]                = C_ACCENT_BG;
    c[ImGuiCol_HeaderHovered]         = dc_rgb(0xF0A83C, 0.26f);
    c[ImGuiCol_HeaderActive]          = dc_rgb(0xF0A83C, 0.38f);
    c[ImGuiCol_Tab]                   = C_TILE;
    c[ImGuiCol_TabHovered]            = C_TILE_HI;
    c[ImGuiCol_TabSelected]           = C_ACCENT_BG;
    c[ImGuiCol_TabSelectedOverline]   = C_ACCENT;
    c[ImGuiCol_TabDimmed]             = C_TILE;
    c[ImGuiCol_TabDimmedSelected]     = C_ACCENT_BG;
    c[ImGuiCol_NavCursor]             = C_ACCENT;
    c[ImGuiCol_CheckMark]             = C_ACCENT;
    c[ImGuiCol_SliderGrab]            = C_ACCENT;
    c[ImGuiCol_SliderGrabActive]      = dc_rgb(0xFFC062);
    c[ImGuiCol_Separator]             = dc_rgb(0x232A35);
    c[ImGuiCol_SeparatorHovered]      = dc_rgb(0x39445A);
    c[ImGuiCol_SeparatorActive]       = C_ACCENT;
    c[ImGuiCol_PlotLines]             = C_INFO;
    c[ImGuiCol_PlotLinesHovered]      = C_ACCENT;
    c[ImGuiCol_PlotHistogram]         = C_ACCENT;
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]         = dc_rgb(0x2A3340);
    c[ImGuiCol_ScrollbarGrabHovered]  = dc_rgb(0x39445A);
    c[ImGuiCol_ScrollbarGrabActive]   = dc_rgb(0x39445A);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, 0.65f);
}

// ------------------------------------------------------------- shared widgets

// battery glyph: rounded body + terminal nub + proportional fill (+ bolt when
// charging), drawn at `p`, `h` pixels tall. Returns the drawn width. `cap` is
// 0-100 (<0 = unknown); mirrors konkr-launcher's icon.
static inline float dc_battery_icon(ImDrawList *dl, ImVec2 p, float h,
                                    int cap, bool charging, float ui)
{
    const float w = h * 1.85f;
    const float r = 2.5f * ui;
    ImU32 frame = ImGui::GetColorU32(C_DIM);
    ImVec4 fillc = cap <= 15 ? C_WARN : cap <= 30 ? C_ACCENT : dc_rgb(0xB9C2CE);
    if (charging)
        fillc = C_ACCENT;

    dl->AddRect(p, ImVec2(p.x + w, p.y + h), frame, r, 0, 1.5f * ui);
    dl->AddRectFilled(ImVec2(p.x + w + 1.5f * ui, p.y + h * 0.30f),
                      ImVec2(p.x + w + 3.5f * ui, p.y + h * 0.70f), frame, 1.0f * ui);
    float inset = 3.0f * ui;
    float fw = (w - 2 * inset) * (cap < 0 ? 0 : cap) / 100.0f;
    if (fw > 0)
        dl->AddRectFilled(ImVec2(p.x + inset, p.y + inset),
                          ImVec2(p.x + inset + fw, p.y + h - inset),
                          ImGui::GetColorU32(fillc), 1.5f * ui);
    if (charging) {
        float cx = p.x + w * 0.5f, cy = p.y + h * 0.5f, s = h * 0.34f;
        ImU32 bolt = ImGui::GetColorU32(C_BG);
        dl->AddTriangleFilled(ImVec2(cx + s * 0.35f, cy - s),
                              ImVec2(cx - s * 0.55f, cy + s * 0.18f),
                              ImVec2(cx + s * 0.10f, cy + s * 0.18f), bolt);
        dl->AddTriangleFilled(ImVec2(cx - s * 0.35f, cy + s),
                              ImVec2(cx + s * 0.55f, cy - s * 0.18f),
                              ImVec2(cx - s * 0.10f, cy - s * 0.18f), bolt);
    }
    return w + 3.5f * ui;
}

// one "chip + label" controller hint at the current cursor; advances the cursor
// and calls SameLine (draw inside a NoNav bar). Mirrors konkr-launcher's footer.
static inline void dc_hint(const char *btn, const char *label, float ui)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 ts = ImGui::CalcTextSize(btn);
    ImVec2 ls = ImGui::CalcTextSize(label);
    float h = ts.y + 5 * ui;
    float w = std::max(h, ts.x + 12 * ui);
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
                      ImGui::GetColorU32(C_TILE_HI), h / 2);
    dl->AddText(ImVec2(p.x + (w - ts.x) / 2, p.y + 2.5f * ui),
                ImGui::GetColorU32(C_TEXT), btn);
    dl->AddText(ImVec2(p.x + w + 7 * ui, p.y + (h - ls.y) / 2),
                ImGui::GetColorU32(C_DIM), label);
    ImGui::Dummy(ImVec2(w + 7 * ui + ls.x, h));
    ImGui::SameLine(0, 20 * ui);
}
