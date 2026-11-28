#include "customizer_ui.h"

#include "appearance_registry.h"
#include "customizer_hooks.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace char_customizer {

namespace {

std::string s_status_banner;
bool s_status_is_warning = false;

template <typename T, typename Pred>
size_t find_index_or_zero(const std::vector<T> &items, Pred pred) {
  for (size_t i = 0; i < items.size(); ++i) {
    if (pred(items[i]))
      return i;
  }
  return 0u;
}

bool choice_button(const SplatModUi *ui, const SplatModUiEx *ex,
                   const char *label, bool active) {
  if (!active) {
    return ui->button(ui->user, label, 0.0f);
  }
  if (ex) {
    ex->push_style_color(ui->user, SPLATMOD_COL_BUTTON, 0.26f, 0.59f, 0.98f,
                         0.85f);
    ex->push_style_color(ui->user, SPLATMOD_COL_BUTTON_HOVERED, 0.34f, 0.65f,
                         1.00f, 0.95f);
    ex->push_style_color(ui->user, SPLATMOD_COL_BUTTON_ACTIVE, 0.16f, 0.49f,
                         0.88f, 1.00f);
    const bool clicked = ui->button(ui->user, label, 0.0f);
    ex->pop_style_color(ui->user, 3);
    return clicked;
  }
  const std::string marked = std::string("[") + label + "]";
  return ui->button(ui->user, marked.c_str(), 0.0f);
}

void begin_row(const SplatModUi *ui, const SplatModUiEx *ex, const char *label,
               float col_x) {
  ui->text(ui->user, label);
  ui->same_line(ui->user);
  if (ex) {
    ex->set_cursor_x(ui->user, col_x);
  }
}

void draw_swatch(const SplatModUi *ui, const SplatModUiEx *ex, float r, float g,
                 float b) {
  if (!ex)
    return;
  ui->same_line(ui->user);
  const float sx = ex->get_cursor_x(ui->user);
  const float sy = ex->get_cursor_y(ui->user);
  ex->draw_rect_filled(ui->user, sx, sy + 2.0f, 18.0f, 18.0f, r, g, b, 1.0f,
                       2.0f);
  ex->dummy(ui->user, 18.0f, 18.0f);
}

} // namespace

const char *customizer_menu_status(void *) {
  static std::string s_line;
  const auto &reg = AppearanceRegistry::instance();
  const CustomizerConfig cfg = reg.config();
  if (!cfg.custom_appearances_enabled || cfg.vanilla_appearance_mode) {
    s_line = "Vanilla Mode";
  } else {
    const auto resolved = reg.resolve(cfg);
    s_line = std::string(body_model_name(resolved.local.body_model)) + " / " +
             resolved.local.hairstyle.display_name;
    const auto favs = reg.resolve_favorite_ink_colors(cfg);
    if (favs.size() == 1u) {
      s_line += " | " + favs[0].display_name;
    } else if (favs.size() > 1u) {
      s_line += " | " + std::to_string(favs.size()) + " Ink Colors";
    }
  }
  return s_line.c_str();
}

int draw_customizer_menu_page(const SplatModUi *ui, void *) {
  if (!ui)
    return SPLATMOD_MENU_STAY;

  auto &reg = AppearanceRegistry::instance();
  CustomizerConfig cfg = reg.config();
  const auto hairs = reg.hairstyles();
  const auto skins = reg.skin_tones();
  const auto eyes = reg.eye_colors();

  const SplatModUiEx *ex =
      (ui->ex && ui->ex->struct_size >= sizeof(SplatModUiEx)) ? ui->ex
                                                              : nullptr;
  const float col_x = ex ? (ex->get_cursor_x(ui->user) + 108.0f) : 0.0f;

  bool changed = false;

  // ---- Appearance ----
  ui->separator_text(ui->user, "Appearance");

  begin_row(ui, ex, "Body Model", col_x);
  if (choice_button(ui, ex, "Inkling Girl##body_0",
                    cfg.body_model == BodyModelId::Girl)) {
    cfg.body_model = BodyModelId::Girl;
    changed = true;
  }
  ui->same_line(ui->user);
  if (choice_button(ui, ex, "Inkling Boy##body_1",
                    cfg.body_model == BodyModelId::Boy)) {
    cfg.body_model = BodyModelId::Boy;
    changed = true;
  }
  ui->same_line(ui->user);
  if (choice_button(ui, ex, "Octoling##body_2",
                    cfg.body_model == BodyModelId::Octoling)) {
    cfg.body_model = BodyModelId::Octoling;
    changed = true;
  }

  // Hairstyle
  const size_t h_idx = find_index_or_zero(
      hairs, [&](const HairstyleDef &h) { return h.id == cfg.hairstyle_id; });
  begin_row(ui, ex, "Hairstyle", col_x);
  if (ui->button(ui->user, " < ##hair_prev", 0.0f) && !hairs.empty()) {
    cfg.hairstyle_id =
        hairs[(h_idx + hairs.size() - 1u) % hairs.size()].id;
    changed = true;
  }
  ui->same_line(ui->user);
  if (ui->button(ui->user, " > ##hair_next", 0.0f) && !hairs.empty()) {
    cfg.hairstyle_id = hairs[(h_idx + 1u) % hairs.size()].id;
    changed = true;
  }
  ui->same_line(ui->user);
  if (hairs.empty()) {
    ui->text(ui->user, "Default");
  } else {
    char h_buf[160];
    std::snprintf(h_buf, sizeof(h_buf), "%s (%s)  [%zu/%zu]",
                  hairs[h_idx].display_name.c_str(),
                  hairs[h_idx].category.c_str(), h_idx + 1u, hairs.size());
    ui->text(ui->user, h_buf);
  }

  // Eye Color
  const size_t e_idx = find_index_or_zero(
      eyes, [&](const EyeColorDef &e) { return e.id == cfg.eye_color_id; });
  begin_row(ui, ex, "Eye Color", col_x);
  if (ui->button(ui->user, " < ##eye_prev", 0.0f) && !eyes.empty()) {
    cfg.eye_color_id = eyes[(e_idx + eyes.size() - 1u) % eyes.size()].id;
    changed = true;
  }
  ui->same_line(ui->user);
  if (ui->button(ui->user, " > ##eye_next", 0.0f) && !eyes.empty()) {
    cfg.eye_color_id = eyes[(e_idx + 1u) % eyes.size()].id;
    changed = true;
  }
  if (!eyes.empty()) {
    draw_swatch(ui, ex, eyes[e_idx].preview_r, eyes[e_idx].preview_g,
                eyes[e_idx].preview_b);
    ui->same_line(ui->user);
    char e_buf[128];
    std::snprintf(e_buf, sizeof(e_buf), "%s  [%zu/%zu]",
                  eyes[e_idx].display_name.c_str(), e_idx + 1u, eyes.size());
    ui->text(ui->user, e_buf);
  } else {
    ui->same_line(ui->user);
    ui->text(ui->user, "Default");
  }

  // Skin Tone
  const size_t s_idx = find_index_or_zero(
      skins, [&](const SkinToneDef &s) { return s.id == cfg.skin_tone_id; });
  begin_row(ui, ex, "Skin Tone", col_x);
  if (ui->button(ui->user, " < ##skin_prev", 0.0f) && !skins.empty()) {
    cfg.skin_tone_id = skins[(s_idx + skins.size() - 1u) % skins.size()].id;
    changed = true;
  }
  ui->same_line(ui->user);
  if (ui->button(ui->user, " > ##skin_next", 0.0f) && !skins.empty()) {
    cfg.skin_tone_id = skins[(s_idx + 1u) % skins.size()].id;
    changed = true;
  }
  if (!skins.empty()) {
    draw_swatch(ui, ex, skins[s_idx].preview_r, skins[s_idx].preview_g,
                skins[s_idx].preview_b);
    ui->same_line(ui->user);
    char s_buf[128];
    std::snprintf(s_buf, sizeof(s_buf), "%s  [%zu/%zu]",
                  skins[s_idx].display_name.c_str(), s_idx + 1u, skins.size());
    ui->text(ui->user, s_buf);
  } else {
    ui->same_line(ui->user);
    ui->text(ui->user, "Default");
  }

  ui->spacing(ui->user);

  const CustomizerRuntimeStats stats = get_runtime_stats();

  // ---- Ink Color Favorites ----
  ui->separator_text(ui->user, "Ink Color Favorites");
  const auto all_inks = reg.ink_colors();
  const auto active_favs = reg.resolve_favorite_ink_colors(cfg);
  if (active_favs.empty()) {
    ui->text_colored(ui->user, 0.72f, 0.72f, 0.76f, 1.0f,
                     "Mode: Game Default (click colors below to select favorites)");
  } else if (active_favs.size() == 1u) {
    char mode_buf[128];
    std::snprintf(mode_buf, sizeof(mode_buf), "Mode: Fixed (%s)",
                  active_favs[0].display_name.c_str());
    ui->text_colored(ui->user, 0.45f, 0.90f, 1.0f, 1.0f, mode_buf);
    ui->same_line(ui->user);
    if (ui->button(ui->user, "Clear##ink_clear", 0.0f)) {
      cfg.favorite_ink_colors.clear();
      changed = true;
    }
  } else {
    char mode_buf[128];
    std::snprintf(mode_buf, sizeof(mode_buf), "Mode: Random from %zu favorites",
                  active_favs.size());
    ui->text_colored(ui->user, 0.45f, 0.90f, 1.0f, 1.0f, mode_buf);
    if (stats.has_active_ink_color) {
      draw_swatch(ui, ex, stats.active_ink_r, stats.active_ink_g,
                  stats.active_ink_b);
      ui->same_line(ui->user);
      char active_buf[96];
      std::snprintf(active_buf, sizeof(active_buf), "Current: %s",
                    stats.active_ink_color_name.c_str());
      ui->text(ui->user, active_buf);
    }
    ui->same_line(ui->user);
    if (ui->button(ui->user, "Clear##ink_clear", 0.0f)) {
      cfg.favorite_ink_colors.clear();
      changed = true;
    }
  }

  float avail_w = 0.0f, avail_h = 0.0f;
  if (ex)
    ex->get_avail_size(ui->user, &avail_w, &avail_h);
  const float btn_w =
      (ex && avail_w > 240.0f)
          ? std::clamp((avail_w - 24.0f) / 4.0f, 100.0f, 134.0f)
          : 122.0f;

  for (size_t i = 0; i < all_inks.size(); ++i) {
    const InkColorDef &c = all_inks[i];
    if ((i % 4u) != 0u)
      ui->same_line(ui->user);

    const bool is_selected =
        std::find(cfg.favorite_ink_colors.begin(), cfg.favorite_ink_colors.end(),
                  c.id) != cfg.favorite_ink_colors.end();
    char lbl[96];
    std::snprintf(lbl, sizeof(lbl), "%s %s##ink_%s",
                  is_selected ? "*" : (ex ? " " : "[ ]"),
                  c.display_name.c_str(), c.id.c_str());

    bool toggled = false;
    if (ex) {
      const float bx = ex->get_cursor_x(ui->user);
      const float by = ex->get_cursor_y(ui->user);
      if (is_selected) {
        const float luma = 0.299f * c.r + 0.587f * c.g + 0.114f * c.b;
        const float tr = (luma > 0.50f) ? 0.05f : 0.98f;
        const float tg = (luma > 0.50f) ? 0.06f : 0.98f;
        const float tb = (luma > 0.50f) ? 0.08f : 0.99f;
        ex->push_style_color(ui->user, SPLATMOD_COL_BUTTON, c.r, c.g, c.b,
                             0.95f);
        ex->push_style_color(ui->user, SPLATMOD_COL_BUTTON_HOVERED,
                             std::min(1.0f, c.r + 0.12f),
                             std::min(1.0f, c.g + 0.12f),
                             std::min(1.0f, c.b + 0.12f), 1.0f);
        ex->push_style_color(ui->user, SPLATMOD_COL_BUTTON_ACTIVE,
                             c.r * 0.80f, c.g * 0.80f, c.b * 0.80f, 1.0f);
        ex->push_style_color(ui->user, SPLATMOD_COL_TEXT, tr, tg, tb, 1.0f);
        toggled = ex->button_ex(ui->user, lbl, btn_w, 26.0f);
        ex->pop_style_color(ui->user, 4);
      } else {
        ex->push_style_color(ui->user, SPLATMOD_COL_BUTTON, 0.14f, 0.15f,
                             0.19f, 0.90f);
        ex->push_style_color(ui->user, SPLATMOD_COL_BUTTON_HOVERED, 0.22f,
                             0.24f, 0.30f, 0.95f);
        ex->push_style_color(ui->user, SPLATMOD_COL_BUTTON_ACTIVE, 0.18f, 0.20f,
                             0.25f, 1.0f);
        toggled = ex->button_ex(ui->user, lbl, btn_w, 26.0f);
        ex->pop_style_color(ui->user, 3);
        ex->draw_rect_filled(ui->user, bx + 2.0f, by + 3.0f, 5.0f, 20.0f, c.r,
                             c.g, c.b, 1.0f, 2.0f);
      }
    } else {
      toggled = ui->button(ui->user, lbl, btn_w);
    }

    if (toggled) {
      auto it = std::find(cfg.favorite_ink_colors.begin(),
                          cfg.favorite_ink_colors.end(), c.id);
      if (it != cfg.favorite_ink_colors.end()) {
        cfg.favorite_ink_colors.erase(it);
      } else {
        cfg.favorite_ink_colors.push_back(c.id);
      }
      changed = true;
    }
  }

  ui->spacing(ui->user);

  const ResolvedAppearance resolved = reg.resolve(cfg);
  char wire_line[160];
  std::snprintf(
      wire_line, sizeof(wire_line),
      "Online fallback (Wii U / Cemu): %s, Skin %u, Eye %u",
      (resolved.wire.model_type == 1u) ? "Inkling Boy" : "Inkling Girl",
      resolved.wire.skin_color + 1u, resolved.wire.eye_color + 1u);
  ui->text_colored(ui->user, 0.65f, 0.65f, 0.68f, 1.0f, wire_line);

  if (resolved.used_fallback && !resolved.fallback_reason.empty()) {
    ui->text_colored(ui->user, 0.95f, 0.72f, 0.25f, 1.0f,
                     resolved.fallback_reason.c_str());
  }

  ui->spacing(ui->user);
  const char *apply_label =
      stats.in_plaza ? "Apply & Reload Plaza" : "Apply";
  if (ui->button(ui->user, apply_label, 0.0f)) {
    reg.set_config(cfg);
    reg.save_config();
    request_ink_color_reroll();
    std::string status;
    const bool built_ok = rebuild_active_appearance(&status);
    s_status_is_warning = !built_ok;
    if (request_plaza_reload()) {
      s_status_banner = status + " - Reloading Plaza";
      return SPLATMOD_MENU_CLOSE;
    }
    s_status_banner = status + " (applies on next Plaza/Lobby/Battle spawn)";
  }
  ui->same_line(ui->user);
  if (ui->button(ui->user, "Reset to Vanilla", 0.0f)) {
    reg.reset_to_vanilla();
    reg.save_config();
    cfg = reg.config();
    request_ink_color_reroll();
    std::string status;
    const bool built_ok = rebuild_active_appearance(&status);
    s_status_is_warning = !built_ok;
    if (request_plaza_reload()) {
      s_status_banner = status + " - Reloading Plaza";
      return SPLATMOD_MENU_CLOSE;
    }
    s_status_banner = status;
  }

  ui->spacing(ui->user);

  // ---- Options ----
  ui->separator_text(ui->user, "Options");
  if (ui->checkbox(ui->user, "Enable custom appearances",
                   &cfg.custom_appearances_enabled))
    changed = true;
  if (ui->checkbox(ui->user, "Vanilla appearance mode",
                   &cfg.vanilla_appearance_mode))
    changed = true;
  if (ui->checkbox(ui->user, "Multiplayer compatibility (vanilla wire safety)",
                   &cfg.multiplayer_compatibility))
    changed = true;
  if (ui->checkbox(ui->user, "Hide hair under helmets",
                   &cfg.preserve_helmet_hide_hair))
    changed = true;

  ui->spacing(ui->user);

  // ---- Cosmetic Packs & Status ----
  ui->separator_text(ui->user, "Cosmetic Packs & Status");
  char pack_info[128];
  std::snprintf(pack_info, sizeof(pack_info),
                "External cosmetic packs loaded: %zu",
                reg.external_pack_count());
  ui->text(ui->user, pack_info);
  ui->same_line(ui->user);
  if (ui->button(ui->user, "Reload Packs", 0.0f)) {
    reg.reload_packs();
    std::string status;
    const bool built_ok = rebuild_active_appearance(&status);
    s_status_is_warning = !built_ok;
    s_status_banner = status;
  }

  char diag_info[192];
  std::snprintf(
      diag_info, sizeof(diag_info),
      "Hooks: %u/%u (native: %u/%u) | Applied: %u | Fallbacks: %u | Plaza: %s",
      stats.hooks_installed, stats.hooks_total, stats.native_hooks_installed,
      stats.native_hooks_total, stats.hook_hits, stats.runtime_fallbacks,
      stats.in_plaza ? "Yes" : "No");
  ui->text_colored(ui->user, 0.65f, 0.65f, 0.68f, 1.0f, diag_info);

  if (changed) {
    reg.set_config(cfg);
    reg.save_config();
    s_status_is_warning = false;
    s_status_banner = stats.in_plaza
                          ? "Selection saved - click Apply & Reload Plaza."
                          : "Selection saved - click Apply.";
  }

  if (!s_status_banner.empty()) {
    if (s_status_is_warning) {
      ui->text_colored(ui->user, 0.95f, 0.45f, 0.45f, 1.0f,
                       s_status_banner.c_str());
    } else {
      ui->text_colored(ui->user, 0.35f, 0.95f, 0.45f, 1.0f,
                       s_status_banner.c_str());
    }
  }

  return SPLATMOD_MENU_STAY;
}

} // namespace char_customizer
