// SPDX-License-Identifier: GPL-2.0
// Copyright (C) 2026-present ROCKNIX (https://github.com/ROCKNIX)
//
// Shared Wi-Fi panel for device-controls: used by the control-panel "Wi-Fi" tab
// (main.cpp) and reused verbatim in the --sidebar overlay (sidebar.cpp), so the
// network can be changed on-device without going through Steam or ES.
//
// Backend is the ROCKNIX `wifictl` helper (NetworkManager/iwd). Scan (~15 s) and
// connect (~90 s) run on a detached worker thread and publish through atomics;
// the panel only ever reads state per frame, so the UI never blocks. Networks
// NetworkManager already has a saved profile for connect on one press (no
// password prompt); new networks bring up an on-screen keyboard (a grid of
// ImGui buttons) so entry is fully gamepad-navigable -- the sidebar has no
// physical keyboard and its nav arrives as synthetic gamepad events.
#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "imgui.h"
#include "theme.h"

namespace wifiui {

// ------------------------------------------------------------------ backend

inline std::string run(const char *cmd)
{
    std::string out;
    FILE *p = popen(cmd, "r");
    if (!p)
        return out;
    char b[512];
    while (fgets(b, sizeof b, p))
        out += b;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

// single-quote for safe interpolation into a /bin/sh command line
inline std::string shq(const std::string &s)
{
    std::string r = "'";
    for (char c : s) {
        if (c == '\'') r += "'\\''";
        else           r.push_back(c);
    }
    r.push_back('\'');
    return r;
}

inline std::vector<std::string> split_lines(const std::string &out)
{
    std::vector<std::string> v;
    size_t i = 0;
    while (i <= out.size()) {
        size_t e = out.find('\n', i);
        std::string line = out.substr(i, e == std::string::npos ? out.size() - i : e - i);
        if (!line.empty())
            v.push_back(line);
        if (e == std::string::npos)
            break;
        i = e + 1;
    }
    return v;
}

// SSID of the currently active wifi connection ("" if none)
inline std::string active_ssid()
{
    return run("nmcli -t -f active,ssid dev wifi 2>/dev/null | "
               "awk -F: '$1==\"yes\"{print $2; exit}'");
}

struct State {
    std::mutex               mtx;         // guards nets / saved
    std::vector<std::string> nets;        // last scan result
    std::vector<std::string> saved;       // NM saved-profile names (usually SSIDs)
    std::atomic<bool>        scanning{false};
    std::atomic<bool>        connecting{false};
    std::atomic<int>         result{0};   // set by connect worker: 1 ok, -1 fail
    std::string              cur_ssid;    // active SSID (polled, main thread)
    double                   next_poll = 0;
    bool                     scanned_once = false;

    enum Mode { LIST, PASSWORD };
    int         mode = LIST;
    std::string sel_ssid;                 // network being joined
    std::string pw;                       // typed passphrase
    bool        show_pw = false;
    bool        shift = false;            // OSK uppercase
    bool        symbols = false;          // OSK symbol set
    bool        focus_keyboard = false;   // grab nav focus entering PASSWORD
    bool        tried_saved = false;      // in-flight connect used a saved profile
    bool        cancel_connect = false;   // user bailed out of a connect attempt
    std::string msg;                      // transient result / error line
};

inline State &S()
{
    static State s;
    return s;
}

inline bool is_saved(const std::string &ssid)
{
    std::lock_guard<std::mutex> lk(S().mtx);
    return std::find(S().saved.begin(), S().saved.end(), ssid) != S().saved.end();
}

inline void start_scan()
{
    State &s = S();
    if (s.scanning.exchange(true))
        return;
    s.scanned_once = true;
    std::thread([] {
        // Rescan, then list SIGNAL:SSID, sort by signal (desc), strip the numeric
        // "signal:" prefix (SSIDs may themselves contain ':'), and dedupe -- so
        // the strongest access points come first.
        std::vector<std::string> nets = split_lines(run(
            "nmcli -w 15 device wifi rescan 2>/dev/null; "
            "nmcli -t -f SIGNAL,SSID device wifi list 2>/dev/null | "
            "sort -t: -k1,1 -rn | sed 's/^[0-9]*://' | awk 'NF && !seen[$0]++'"));
        std::vector<std::string> saved = split_lines(
            run("nmcli -t -f NAME connection show 2>/dev/null"));
        {
            std::lock_guard<std::mutex> lk(S().mtx);
            S().nets.swap(nets);
            S().saved.swap(saved);
        }
        S().scanning = false;
    }).detach();
}

// psk empty + saved profile -> reactivate it (keeps stored credentials);
// otherwise (re)join with the given passphrase via wifictl.
inline void start_connect(const std::string &ssid, const std::string &psk, bool use_saved)
{
    State &s = S();
    if (s.connecting.exchange(true))
        return;
    s.result = 0;
    s.sel_ssid = ssid;
    s.tried_saved = use_saved;
    s.cancel_connect = false;
    std::thread([ssid, psk, use_saved] {
        // `timeout` bounds the wait so a bad password / unreachable AP can't pin
        // the "Connecting…" state for the full nmcli -w window.
        if (use_saved) {
            run(("timeout 35 nmcli -w 30 connection up id " + shq(ssid) +
                 " >/dev/null 2>&1").c_str());
        } else {
            // Persist the choice so NM remembers it and reconnects on next boot.
            std::string cmd =
                ". /etc/profile 2>/dev/null; "
                "set_setting wifi.enabled 1 >/dev/null 2>&1; "
                "set_setting wifi.ssid " + shq(ssid) + " >/dev/null 2>&1; "
                "set_setting wifi.key "  + shq(psk)  + " >/dev/null 2>&1; "
                "timeout 40 wifictl connect " + shq(ssid) + " " + shq(psk) + " >/dev/null 2>&1";
            run(cmd.c_str());
        }
        // Pin NM autoconnect to this network so resume/boot rejoin IT, not
        // another saved AP (mirrors the konkr-wifi-resume suspend hook). Demote
        // every other saved Wi-Fi profile.
        run(("nmcli -t -f NAME,TYPE connection show 2>/dev/null | "
             "awk -F: '$2 ~ /wireless/{print $1}' | while IFS= read -r n; do "
             "if [ \"$n\" = " + shq(ssid) + " ]; then "
             "nmcli connection modify \"$n\" connection.autoconnect yes "
             "connection.autoconnect-priority 100 2>/dev/null; "
             "else nmcli connection modify \"$n\" connection.autoconnect no 2>/dev/null; fi; "
             "done").c_str());
        std::string cur = active_ssid();
        {
            std::lock_guard<std::mutex> lk(S().mtx);
            S().cur_ssid = cur;
        }
        S().result = (cur == ssid) ? 1 : -1;
        S().connecting = false;
    }).detach();
}

inline void set_enabled(bool on)
{
    std::string cmd = ". /etc/profile 2>/dev/null; set_setting wifi.enabled ";
    cmd += on ? "1 >/dev/null 2>&1; wifictl enable >/dev/null 2>&1 &"
              : "0 >/dev/null 2>&1; wifictl disable >/dev/null 2>&1 &";
    run(cmd.c_str());
}

inline void open_keyboard(const std::string &ssid)
{
    State &s = S();
    s.sel_ssid = ssid;
    s.pw.clear();
    s.msg.clear();
    s.show_pw = s.shift = s.symbols = false;
    s.focus_keyboard = true;
    s.mode = State::PASSWORD;
}

// --------------------------------------------------------------- on-screen kb

// One OSK key. Returns true when pressed. `first` grabs nav focus on entry.
inline bool osk_key(const std::string &label, float w, float h, bool first)
{
    if (first && S().focus_keyboard) {
        ImGui::SetKeyboardFocusHere();
        S().focus_keyboard = false;
    }
    return ImGui::Button(label.c_str(), ImVec2(w, h));
}

inline void draw_password()
{
    State &s = S();

    if (g_font_bold) ImGui::PushFont(g_font_bold);
    ImGui::TextColored(C_ACCENT, "Connect to %s", s.sel_ssid.c_str());
    if (g_font_bold) ImGui::PopFont();
    ImGui::Spacing();

    if (s.connecting) {
        ImGui::TextColored(C_INFO, "Connecting...");
        ImGui::TextColored(C_DIM, "This can take a moment.");
        ImGui::Spacing();
        if (ImGui::Button("Cancel")) {
            s.cancel_connect = true;
            s.mode = State::LIST;
            s.msg.clear();
        }
        return;   // hide the keyboard while the worker runs
    }
    if (!s.msg.empty())
        ImGui::TextColored(C_WARN, "%s", s.msg.c_str());

    // passphrase field: a real read-only InputText frame (correctly sized and
    // vertically centred, unlike hand-drawn text), masked unless "show".
    char buf[128];
    snprintf(buf, sizeof buf, "%s", s.pw.c_str());
    ImGuiInputTextFlags fl = ImGuiInputTextFlags_ReadOnly;
    if (!s.show_pw) fl |= ImGuiInputTextFlags_Password;
    ImGui::TextColored(C_DIM, "Password");
    ImGui::SameLine();
    if (ImGui::SmallButton(s.show_pw ? "hide" : "show"))
        s.show_pw = !s.show_pw;
    ImGui::SetNextItemWidth(-1);
    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);   // OSK drives it; skip in nav
    ImGui::InputText("##pw", buf, sizeof buf, fl);
    ImGui::PopItemFlag();
    ImGui::Spacing();

    // key grid: sized so a 10-wide row fills the available width in either host
    const float sp    = ImGui::GetStyle().ItemSpacing.x;
    const float avail = ImGui::GetContentRegionAvail().x;
    const float kw    = (avail - sp * 9) / 10.0f;
    const float kh    = ImGui::GetFontSize() * 1.7f;

    static const char *ROWS_LOWER[4] = { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" };
    static const char *ROWS_SYM[4]   = { "1234567890", "@#$%&*-_+=", "!?()[]{}/\\", ".,:;'\"|<>~" };
    const char *const *rows = s.symbols ? ROWS_SYM : ROWS_LOWER;

    bool first = true;
    for (int r = 0; r < 4; r++) {
        for (const char *c = rows[r]; *c; c++) {
            char ch = *c;
            if (!s.symbols && s.shift && ch >= 'a' && ch <= 'z')
                ch = (char)std::toupper((unsigned char)ch);
            ImGui::PushID(r * 100 + (int)(c - rows[r]));
            if (osk_key(std::string(1, ch), kw, kh, first))
                s.pw.push_back(ch);
            ImGui::PopID();
            first = false;
            if (*(c + 1))
                ImGui::SameLine();
        }
    }

    // modifier / edit row
    float mw = (avail - sp * 3) / 4.0f;
    if (ImGui::Button(s.shift ? "Shift*" : "Shift", ImVec2(mw, kh)))
        s.shift = !s.shift;
    ImGui::SameLine();
    if (ImGui::Button(s.symbols ? "abc" : "?123", ImVec2(mw, kh)))
        s.symbols = !s.symbols;
    ImGui::SameLine();
    if (ImGui::Button("Space", ImVec2(mw, kh)))
        s.pw.push_back(' ');
    ImGui::SameLine();
    if (ImGui::Button("Del", ImVec2(mw, kh)) && !s.pw.empty())
        s.pw.pop_back();

    ImGui::Spacing();
    float aw = (avail - sp) / 2.0f;
    if (ImGui::Button("Cancel", ImVec2(aw, kh * 1.1f))) {
        s.mode = State::LIST;
        s.pw.clear();
        s.msg.clear();
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, C_ACCENT_BG);
    if (ImGui::Button("Connect", ImVec2(aw, kh * 1.1f))) {
        s.msg.clear();
        start_connect(s.sel_ssid, s.pw, false);
    }
    ImGui::PopStyleColor();
}

inline void draw_list()
{
    State &s = S();
    const float u = g_ui;

    if (g_font_bold) ImGui::PushFont(g_font_bold);
    ImGui::TextColored(C_ACCENT, "Wi-Fi");
    if (g_font_bold) ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (!s.cur_ssid.empty())
        ImGui::TextColored(C_OK, "  - %s", s.cur_ssid.c_str());
    else
        ImGui::TextColored(C_DIM, "  - not connected");

    if (s.connecting && !s.cancel_connect) {
        ImGui::Spacing();
        ImGui::TextColored(C_INFO, "Connecting to %s...", s.sel_ssid.c_str());
        ImGui::Spacing();
        if (ImGui::Button("Cancel"))
            s.cancel_connect = true;
        return;   // keep the list steady while a saved-profile join runs
    }
    if (s.connecting && s.cancel_connect)
        ImGui::TextColored(C_DIM, "Finishing previous attempt...");
    if (!s.msg.empty())
        ImGui::TextColored(s.cur_ssid.empty() ? C_WARN : C_OK, "%s", s.msg.c_str());
    ImGui::Spacing();

    if (s.scanning) {
        ImGui::BeginDisabled();
        ImGui::Button("Scanning...");
        ImGui::EndDisabled();
    } else if (ImGui::Button("Rescan")) {
        start_scan();
    }
    ImGui::SameLine();
    if (ImGui::Button("Wi-Fi Off"))
        set_enabled(false);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    std::lock_guard<std::mutex> lk(s.mtx);
    if (s.nets.empty()) {
        ImGui::TextColored(C_DIM, s.scanning ? "Looking for networks..."
                                             : "No networks found. Try Rescan.");
        return;
    }
    // No wrapper child: render the rows directly into the host's scroll region
    // (the tab's ##content / the sidebar's content child). A nested child here
    // added a second scrollbar that truncated the list and a nav boundary that
    // D-pad-down would not cross into.
    for (size_t i = 0; i < s.nets.size(); i++) {
        const std::string &ssid = s.nets[i];
        bool active = (ssid == s.cur_ssid);
        bool saved  = std::find(s.saved.begin(), s.saved.end(), ssid) != s.saved.end();
        ImGui::PushID((int)i);
        if (active) ImGui::PushStyleColor(ImGuiCol_Text, C_OK);
        std::string label = (active ? "* " : "  ") + ssid;
        if (saved && !active) label += "   (saved)";
        if (ImGui::Selectable(label.c_str(), false, 0,
                              ImVec2(0, ImGui::GetFontSize() + 12 * u))) {
            s.msg.clear();
            if (active) {
                // already on it: nothing to do
            } else if (saved) {
                start_connect(ssid, "", true);      // one-press: use stored creds
            } else {
                open_keyboard(ssid);                // new network: prompt for the key
            }
        }
        if (active) ImGui::PopStyleColor();
        ImGui::PopID();
    }
}

// The whole panel. Call once per frame from the tab / sidebar.
inline void draw()
{
    State &s = S();

    // throttled active-SSID poll (~2 s); never while a connect worker is running
    double t = ImGui::GetTime();
    if (!s.connecting && t >= s.next_poll) {
        s.cur_ssid = active_ssid();
        s.next_poll = t + 2.0;
    }

    // consume a finished connect
    if (s.result.load() != 0 && !s.connecting) {
        int r = s.result.exchange(0);
        if (s.cancel_connect) {
            s.cancel_connect = false;   // user bailed: drop the result silently
        } else if (r == 1) {
            s.msg = "Connected to " + s.sel_ssid;
            s.mode = State::LIST;
            s.pw.clear();
        } else {
            // failed: a saved-profile attempt falls through to the keyboard so the
            // password can be re-entered (e.g. the stored one is stale).
            s.msg = s.tried_saved ? "Saved password failed - enter it again."
                                  : "Could not connect - check the password.";
            s.mode = State::PASSWORD;
        }
    }

    // auto-scan the first time the panel is shown
    if (!s.scanned_once && !s.scanning)
        start_scan();

    if (s.mode == State::PASSWORD)
        draw_password();
    else
        draw_list();
}

} // namespace wifiui
