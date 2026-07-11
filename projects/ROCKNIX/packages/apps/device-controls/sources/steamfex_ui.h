// SPDX-License-Identifier: GPL-2.0
// steamfex_ui.h — shared per-game FEX-tuning + save-recovery editor.
//
// The ONE implementation of the per-game Steam editor, included by both
// device-controls and konkr-launcher. Each app keeps its own game-selection
// chrome (device-controls: a combo; konkr-launcher: a two-pane list) and, for
// the selected game, calls steamfex::draw_game_editor(game). All the FEX
// profile logic + stranded-save recovery lives in steamfex.h.

#pragma once

#include "imgui.h"
#include "steamfex.h"

namespace steamfex {

// Amber accent, matching both apps' palette (namespaced to avoid clashing with
// each app's own C_ACCENT).
static const ImVec4 ui_accent(0xF0 / 255.f, 0xA8 / 255.f, 0x3C / 255.f, 1.0f);

// Render the editor for one selected game. Loads/saves the game's FEX profile
// internally (keyed on appid) and handles the "* BACKUP" save recovery.
static void draw_game_editor(const Game &g)
{
    const bool  is_default = (g.appid == DEFAULT_APPID);
    const float fs = ImGui::GetFontSize();

    static int         loaded_for = -1;
    static Tune        tune, default_tune;
    static bool        has_override = false;
    static char        extra_buf[4096] = { 0 };
    static std::string status;

    auto reload = [&]() {
        load_profile(DEFAULT_APPID, default_tune);
        load_profile(g.appid, tune);
        has_override = profile_exists(g.appid);
        snprintf(extra_buf, sizeof extra_buf, "%s", tune.extra.c_str());
        loaded_for = g.appid;
    };
    if (loaded_for != g.appid) {
        status.clear();
        reload();
    }

    // ---- mapping / default header ----
    if (is_default) {
        ImGui::TextUnformatted("Default profile");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Baseline every tuned game inherits, unless it sets "
                            "its own override.");
        ImGui::PopTextWrapPos();
    } else {
        bool en = tune.mapping;
        if (ImGui::Checkbox("Use FEX tuning for this game", &en)) {
            if (set_mapping(g.appid, en)) {
                tune.mapping = en;
                status = en ? "Enabled (config.vdf updated; may need a Steam "
                              "restart to register)."
                            : "Disabled (stock Proton).";
            } else {
                status = "config.vdf edit failed - set 'KONKR FEX-Tuned' in "
                         "Steam > Properties > Compatibility.";
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled(has_override ? "[override]" : "[inherits Default]");
    }
    ImGui::Separator();

    // ---- stranded saves recovery (Proton "* BACKUP") ----
    if (!is_default) {
        static std::vector<SaveBackup> backups;
        static int backups_for = -999;
        static int restored_for = -1;
        if (backups_for != g.appid) {
            backups = scan_backups(g.appid);
            backups_for = g.appid;
            restored_for = -1;
        }
        if (!backups.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ui_accent);
            ImGui::TextUnformatted("Stranded saves found");
            ImGui::PopStyleColor();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextDisabled("Proton moved this game's save folder aside on a "
                                "version change, so the game and Steam Cloud can't "
                                "see the saves. Restore copies them back without "
                                "overwriting anything newer.");
            ImGui::PopTextWrapPos();
            for (auto &b : backups)
                ImGui::BulletText("%s  -  %d file%s", b.label.c_str(), b.files,
                                  b.files == 1 ? "" : "s");
            if (ImGui::Button("Restore saves")) {
                for (auto &b : backups)
                    restore_backup(b);
                restored_for = g.appid;
                status = "Saves restored into the game folder.";
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete backup")) {
                for (auto &b : backups)
                    remove_backup(b);
                status = "Backup folder(s) removed.";
                backups_for = -999;   // rescan -> block disappears
                restored_for = -1;
            }
            if (restored_for == g.appid) {
                ImGui::PushStyleColor(ImGuiCol_Text, ui_accent);
                ImGui::TextWrapped("Restored - launch the game to check your saves "
                                   "loaded, then Delete backup to clean up.");
                ImGui::PopStyleColor();
            }
            ImGui::Separator();
        }
    }

    // ---- preset ----
    std::string presets;
    for (int i = 0; i < N_PRESETS; i++) {
        presets += PRESET_NAMES[i];
        presets.push_back('\0');
    }
    ImGui::SetNextItemWidth(fs * 12.0f);
    if (ImGui::Combo("Preset", &tune.preset, presets.c_str()) && tune.preset != 0)
        apply_preset(tune, tune.preset);

    // ---- FEX flags ----
    ImGui::Separator();
    ImGui::TextDisabled(is_default ? "FEX flags  ('FEX default' = global config)"
                                   : "FEX flags  ('Inherit' = take Default's value)");
    const char *tri_items = is_default ? "FEX default\0On\0Off\0" : "Inherit\0On\0Off\0";
    for (int i = 0; i < N_FEX_BOOLS; i++) {
        int idx = (tune.tri[i] == -1) ? 0 : (tune.tri[i] == 1 ? 1 : 2);
        ImGui::PushID(i);
        ImGui::SetNextItemWidth(fs * 8.0f);
        if (ImGui::Combo(FEX_BOOLS[i].label, &idx, tri_items)) {
            tune.tri[i] = (idx == 0) ? -1 : (idx == 1 ? 1 : 0);
            tune.preset = 0;   // manual edit => Custom
        }
        if (!is_default && tune.tri[i] == -1) {
            int dv = default_tune.tri[i];
            ImGui::SameLine();
            ImGui::TextDisabled("(default: %s)",
                                dv == 1 ? "On" : dv == 0 ? "Off" : "FEX default");
        }
        ImGui::PopID();
    }

    const char *smc_items = is_default ? "FEX default\0none\0mtrack\0full\0"
                                       : "Inherit\0none\0mtrack\0full\0";
    ImGui::SetNextItemWidth(fs * 8.0f);
    if (ImGui::Combo("SMC checks", &tune.smc, smc_items))
        tune.preset = 0;
    if (!is_default && tune.smc == 0) {
        int ds = default_tune.smc;
        ImGui::SameLine();
        ImGui::TextDisabled("(default: %s)", ds == 0 ? "FEX default" : SMC_VALUES[ds]);
    }

    // ---- environment variables ----
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Environment variables (one KEY=VALUE per line). The lever "
                        "for render-bound games (DXVK / Mesa / Turnip / wine).");
    ImGui::PopTextWrapPos();
    if (!is_default && !default_tune.extra.empty() &&
        ImGui::TreeNode("Inherited from Default (read-only)")) {
        ImGui::TextDisabled("%s", default_tune.extra.c_str());
        ImGui::TreePop();
    }
    ImGui::InputTextMultiline("##extra", extra_buf, sizeof extra_buf,
                              ImVec2(-1, fs * 6.0f));
    if (ImGui::Button("Insert recommended")) {
        size_t len = strlen(extra_buf);
        if (len && extra_buf[len - 1] != '\n' && len + 1 < sizeof extra_buf)
            extra_buf[len++] = '\n', extra_buf[len] = '\0';
        strncat(extra_buf, RECOMMENDED_ENV, sizeof extra_buf - strlen(extra_buf) - 1);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Turnip/Mesa/DXVK perf baseline");

    // ---- save / revert ----
    ImGui::Separator();
    const char *save_label = is_default ? "Save Default" : "Save override";
    if (ImGui::Button(save_label)) {
        tune.extra = extra_buf;
        bool ok = save_profile(g.appid, tune);
        if (ok)
            reload();   // refresh override/default state
        status = ok ? (is_default ? "Default saved." : "Override saved.")
                    : "Failed to write profile.";
    }
    if (!is_default && has_override) {
        ImGui::SameLine();
        if (ImGui::Button("Delete override")) {
            delete_profile(g.appid);
            reload();   // flags revert to Inherit
            status = "Override deleted; game now inherits Default.";
        }
    }
    if (!status.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::PopTextWrapPos();
    }
}

} // namespace steamfex
