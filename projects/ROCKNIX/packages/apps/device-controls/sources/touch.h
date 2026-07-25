// SPDX-License-Identifier: GPL-2.0
// Touchscreen helpers shared by device-controls (panel + --sidebar overlay) and
// konkr-launcher.
//
// These apps are driven by a finger as often as by the d-pad, and ImGui out of
// the box assumes a mouse: a scrollable region only scrolls from the wheel or by
// grabbing its (deliberately thin) scrollbar, and the pointer keeps hovering
// whatever it was last left on, so the last-tapped widget stays lit. Both are
// fixed here rather than per-app so the panel, the overlay and the launcher
// behave identically under a finger.
//
// Header-only: everything is inline, so any translation unit can include it.
#pragma once

#include <cfloat>
#include <cmath>
#include "imgui.h"
// For ActiveId / MoveId: telling "the finger is dragging empty space" apart from
// "the finger is working a widget" is not expressible through the public API —
// IsAnyItemActive() is true for both. See dc_touch_scroll.
#include "imgui_internal.h"

// Drag/flick-to-scroll for the current window. Call once, immediately after the
// Begin()/BeginChild() of a scrollable region and before submitting any widget.
//
// Only a drag that starts on empty space scrolls; a press that lands on a slider,
// button or combo still belongs to that widget. Lifting mid-drag keeps scrolling
// with a decaying velocity — the flick is what makes a long list read as a touch
// list instead of a scrollbar.
//
// One drag is tracked at a time, keyed on the window that started it, so nested
// or sibling scroll regions can never fight over the same finger.
static inline void dc_touch_scroll(void)
{
    ImGuiContext &g = *ImGui::GetCurrentContext();
    ImGuiWindow *win = g.CurrentWindow;
    ImGuiIO &io = g.IO;

    static ImGuiID owner = 0;   // window whose drag / flick is live
    static bool  dragging = false;
    static float vel = 0.0f;    // px per frame, sign follows MouseDelta.y

    if (!win || win->ScrollMax.y <= 0.0f) {   // nothing to scroll here
        if (win && owner == win->ID) {
            dragging = false;
            vel = 0.0f;
        }
        return;
    }

    // A press on empty space leaves ActiveId on the window's MoveId — ImGui sets
    // that even for a NoMove window, purely to stop other widgets hovering — while
    // a press on a widget leaves ActiveId on the widget. So this is exactly the
    // test "the finger is not busy with a widget", which IsAnyItemActive() cannot
    // express (it is true in both cases).
    const bool finger_free = (g.ActiveId == 0 || g.ActiveId == win->MoveId);

    // Velocity is smoothed rather than sampled: a finger does not move every
    // single frame, and taking the last frame's raw delta would let one still
    // frame right before the lift cancel the flick entirely.
    const float SMOOTH = 0.4f;

    if (dragging && owner == win->ID) {
        if (io.MouseDown[0]) {
            ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
            vel += (io.MouseDelta.y - vel) * SMOOTH;
            return;
        }
        dragging = false;                     // lifted: hand over to the flick
    } else if (!dragging && finger_free && io.MouseDown[0] &&
               ImGui::IsMouseDragging(0, io.MouseDragThreshold) &&
               ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows |
                                      ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        // Waiting for the drag threshold is what keeps a tap (and a tap's jitter)
        // from scrolling at all.
        dragging = true;
        owner = win->ID;
        ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
        vel = io.MouseDelta.y;
        return;
    }

    if (!dragging && owner == win->ID && fabsf(vel) > 0.6f) {
        ImGui::SetScrollY(ImGui::GetScrollY() - vel);
        vel *= 0.90f;
    }
}

// Park ImGui's pointer offscreen. Call when a finger is lifted: touch delivers a
// position but never a "moved away", so without this the widget under the last
// tap stays hovered (highlighted) indefinitely, which reads as a stuck
// selection. Queued after the button-release event, so ImGui still completes the
// click at the real position first (its event trickling defers a position change
// that follows a button change to a later frame).
static inline void dc_touch_park(void)
{
    ImGui::GetIO().AddMousePosEvent(-FLT_MAX, -FLT_MAX);
}
