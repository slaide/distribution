// SPDX-License-Identifier: GPL-2.0
// SDL-side touch handling shared by device-controls (panel + the sidebar's
// gamescope backend) and konkr-launcher.
//
// Split from touch.h because that header is also used by the sidebar's Wayland
// backend, which is compiled ahead of any SDL include.
//
// ---------------------------------------------------------------------------
// Why a tap has to be held back a frame (the "tabs need two taps" bug).
//
// gamescope does not hand touch to its clients (its touch_click_mode defaults to
// Left), so a finger arrives as a cursor warp followed by BTN_LEFT in the same
// batch, indistinguishable from a mouse. ImGui applies both in one frame — so the
// press frame does carry the right MousePos, and the widget under the finger
// really is hit-tested. That part was never the problem.
//
// What loses the tap is ImGui's AllowOverlap gate, in ItemHoverable():
//
//     SetHoveredID(id);
//     if (item_flags & ImGuiItemFlags_AllowOverlap) {
//         g.HoveredIdAllowOverlap = true;
//         if (g.HoveredIdPreviousFrame != id)
//             return false;            // <- not hoverable, though HoveredId is set
//     }
//
// An item that opts into AllowOverlap only becomes hoverable once it was already
// the hovered id on the *previous* frame; that one-frame delay is what stops two
// overlapping items both reporting hovered on the frame the pointer arrives. A
// mouse never notices — it slides on, hovers a frame, clicks later. A finger
// teleports onto the widget and presses on that very frame, so ItemHoverable
// returns false, ButtonBehavior sees hovered == false, and nothing is pressed.
//
// TabItemEx passes ImGuiButtonFlags_AllowOverlap (for the close/scroll buttons
// that overlap tabs); plain Button() does not. That is the whole asymmetry: tabs
// ignored the first tap, buttons never did. The tap was not entirely wasted —
// SetHoveredID() runs before the bail, so on the next frame
// HoveredIdPreviousFrame does match and a second tap lands. Hence "two taps".
//
// So the fix is to give the widget that missing frame: a press arriving in the
// same batch as the move that brought the pointer onto it is held back until the
// next frame (dc_sdl_poll). This is what ImGui does for real touchscreens
// (#2702, "TouchScreen have no initial hover"), but done without relabelling the
// event as SDL_TOUCH_MOUSEID, so io.MouseSource stays Mouse and none of ImGui's
// other touch-mode behaviour (hover delays, nav ref pos, stationary thresholds)
// shifts along with it.
//
// KONKR_TOUCH_LOG=1 traces the whole path: every mouse/touch event, then per
// frame the settled MousePos / HoveredId / ActiveId / HoveredWindow, plus each
// submitted tab's rect with an inside= test against the tap point.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdio>
#include <cstdlib>
#include <SDL.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "touch.h"

// KONKR_TOUCH_LOG=1 traces the pointer/touch event stream and ImGui's response.
static inline bool dc_touchlog_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("KONKR_TOUCH_LOG");
        on = (e && e[0] == '1') ? 1 : 0;
    }
    return on == 1;
}

// Per-event-loop state for dc_sdl_poll / dc_sdl_touchify. dc_sdl_batch_begin
// clears the per-batch fields; `held` deliberately survives it, being the thing
// that carries a press across into the next frame.
struct DcTouchFix {
    bool      moved = false;     // a pointer move was seen earlier in this batch
    bool      cut = false;       // batch cut short to hold a press back
    bool      have_held = false; // `held` carries a press for the next frame
    SDL_Event held = {};
};

static inline void dc_sdl_batch_begin(DcTouchFix &st)
{
    st.moved = false;
    st.cut = false;
}

// Drop-in replacement for SDL_PollEvent, and the reason a first tap lands at all:
// a left press that turns up in the same batch as the move which brought the
// pointer onto the widget is held back and replayed at the top of the next frame,
// giving an AllowOverlap widget (i.e. a tab) the frame of hover it demands. See
// the note at the top of this file.
//
// Draining stops at that press so everything queued behind it — above all its own
// release — keeps its order. ImGui's event trickling then splits the replayed
// press from that release, so the press still gets a frame to itself.
//
// A press SDL already tags as touch is left alone: ImGui defers those itself
// (#2702), and holding it here too would cost it a second frame.
static inline int dc_sdl_poll(DcTouchFix &st, SDL_Event *ev)
{
    if (st.have_held) {          // replay first: it is older than anything new
        *ev = st.held;
        st.have_held = false;
        return 1;
    }
    if (st.cut || !SDL_PollEvent(ev))
        return 0;
    if (ev->type == SDL_MOUSEBUTTONDOWN && ev->button.button == SDL_BUTTON_LEFT &&
        ev->button.which != SDL_TOUCH_MOUSEID && st.moved) {
        st.held = *ev;
        st.have_held = true;
        st.cut = true;
        return 0;
    }
    return 1;
}

// Log one SDL pointer event as the app received it (before any rewrite).
static inline void dc_sdl_log_event(const SDL_Event *ev)
{
    if (!dc_touchlog_on())
        return;
    switch (ev->type) {
    case SDL_MOUSEMOTION:
        fprintf(stderr, "[touch] MOTION which=%u x=%d y=%d state=%u\n",
                ev->motion.which, ev->motion.x, ev->motion.y, ev->motion.state);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        fprintf(stderr, "[touch] BUTTON%s which=%u btn=%u x=%d y=%d clicks=%u\n",
                ev->type == SDL_MOUSEBUTTONDOWN ? "DOWN" : "UP  ",
                ev->button.which, ev->button.button,
                ev->button.x, ev->button.y, ev->button.clicks);
        break;
    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
    case SDL_FINGERMOTION:
        fprintf(stderr, "[touch] FINGER%s id=%lld x=%.3f y=%.3f\n",
                ev->type == SDL_FINGERDOWN ? "DOWN" :
                ev->type == SDL_FINGERUP ? "UP  " : "MOVE",
                (long long)ev->tfinger.fingerId, ev->tfinger.x, ev->tfinger.y);
        break;
    default:
        break;
    }
    fflush(stderr);
}

// Log ImGui's per-frame view of the pointer. Call right after ImGui::NewFrame():
// MousePos is post-trickling, ActiveId is last frame's (kept alive by
// UpdateMouseMovingWindowNewFrame) and HoveredWindow is already this frame's,
// because NewFrame() ends with UpdateHoveredWindowAndCaptureFlags().
//
// HoveredId is deliberately NOT logged here: NewFrame() zeroes it before any
// widget is submitted, so it reads 0 on every frame regardless of what is under
// the finger. HoveredIdPreviousFrame is the meaningful one at this point; the
// live value is logged by dc_imgui_log_endframe() below.
static inline void dc_imgui_log_frame(void)
{
    if (!dc_touchlog_on())
        return;
    ImGuiContext &g = *ImGui::GetCurrentContext();
    ImGuiIO &io = g.IO;
    // Quiet frames (nothing pressed, nothing hovered, no click) are skipped so a
    // trace stays readable while the app idles at 60fps.
    if (!io.MouseDown[0] && !io.MouseClicked[0] && !io.MouseReleased[0] &&
        g.HoveredIdPreviousFrame == 0 && g.ActiveId == 0)
        return;
    fprintf(stderr, "[touch]  frame pos=(%.0f,%.0f) src=%d down=%d clicked=%d "
                    "released=%d hovprev=0x%08X active=0x%08X dragmax=%.1f "
                    "hwnd=%s owned=%d popups=%d\n",
            io.MousePos.x, io.MousePos.y, (int)io.MouseSource,
            (int)io.MouseDown[0], (int)io.MouseClicked[0], (int)io.MouseReleased[0],
            g.HoveredIdPreviousFrame, g.ActiveId, io.MouseDragMaxDistanceSqr[0],
            g.HoveredWindow ? g.HoveredWindow->Name : "(null)",
            (int)io.MouseDownOwned[0], g.OpenPopupStack.Size);
    fflush(stderr);
}

// Log ImGui's settled view of the frame. Call after the last widget has been
// submitted (right before or after ImGui::Render()), where HoveredId is this
// frame's real answer to "what is under the finger".
//
// HoveredIdIsDisabled is the tell for "an item passed the rect test but was
// discarded" (disabled item, or hovering inhibited by a popup), which is
// otherwise indistinguishable from "the finger hit nothing".
static inline void dc_imgui_log_endframe(void)
{
    if (!dc_touchlog_on())
        return;
    ImGuiContext &g = *ImGui::GetCurrentContext();
    ImGuiIO &io = g.IO;
    if (!io.MouseDown[0] && !io.MouseClicked[0] && !io.MouseReleased[0] &&
        g.HoveredId == 0 && g.ActiveId == 0)
        return;
    fprintf(stderr, "[touch]  end   hovered=0x%08X hovdisabled=%d active=0x%08X "
                    "hwnd=%s nav=%s moving=%s\n",
            g.HoveredId, (int)g.HoveredIdIsDisabled, g.ActiveId,
            g.HoveredWindow ? g.HoveredWindow->Name : "(null)",
            g.NavWindow ? g.NavWindow->Name : "(null)",
            g.MovingWindow ? g.MovingWindow->Name : "(null)");
    fflush(stderr);
}

// Log the just-submitted item's id, rect and whether the pointer is inside it.
// Call immediately after submitting a widget; only frames where a button changed
// state print, so this can sit in a per-frame loop.
//
// This is what tells a "the widget refused the click" bug apart from a "the
// finger never landed on the widget" one: the rect is the widget's real
// on-screen box this frame, in the same coordinate space as pos= above.
static inline void dc_touchlog_item(const char *label)
{
    if (!dc_touchlog_on())
        return;
    ImGuiContext &g = *ImGui::GetCurrentContext();
    ImGuiIO &io = g.IO;
    if (!io.MouseClicked[0] && !io.MouseReleased[0])
        return;
    const ImRect &r = g.LastItemData.Rect;
    fprintf(stderr, "[touch]   item '%s' id=0x%08X rect=(%.0f,%.0f)-(%.0f,%.0f) "
                    "inside=%d status=0x%X\n",
            label, g.LastItemData.ID, r.Min.x, r.Min.y, r.Max.x, r.Max.y,
            (int)r.Contains(io.MousePos), (unsigned)g.LastItemData.StatusFlags);
    fflush(stderr);
}

// Tracks the batch state dc_sdl_poll needs, and returns true when this event is a
// finger lift — i.e. the caller should dc_touch_park() after handing it to ImGui.
//
// Only a genuine touch event (the sway / wl_touch path, which SDL tags
// SDL_TOUCH_MOUSEID) can be recognised as a lift. Under gamescope a tap is
// indistinguishable from a mouse click, so nothing is parked there and the
// pointer stays where the finger left it.
//
// Call for every event BEFORE ImGui_ImplSDL2_ProcessEvent.
static inline bool dc_sdl_touchify(DcTouchFix &st, SDL_Event *ev)
{
    dc_sdl_log_event(ev);

    switch (ev->type) {
    case SDL_MOUSEMOTION:
        st.moved = true;
        return false;
    case SDL_MOUSEBUTTONUP:
        return ev->button.which == SDL_TOUCH_MOUSEID;
    default:
        return false;
    }
}
