#pragma once

#include "splat_mod_api.h"

#include <cstdint>
#include <string>

namespace char_customizer {

struct CustomizerRuntimeStats {
  uint32_t hooks_installed = 0;
  uint32_t hooks_total = 0;
  uint32_t native_hooks_installed = 0;
  uint32_t native_hooks_total = 0;
  uint32_t hook_hits = 0;
  uint32_t runtime_fallbacks = 0;
  bool in_plaza = false;
  bool reload_pending = false;
  bool has_active_ink_color = false;
  std::string active_ink_color_name;
  float active_ink_r = 0.0f;
  float active_ink_g = 0.0f;
  float active_ink_b = 0.0f;
};

bool install_hooks(const SplatModHost *host);
void uninstall_hooks(const SplatModHost *host);

// Builds/refreshes the active overlay .szs archive for the requested FMDL
// target ("Player00" or "Player01"). Safe to call from UI Apply, startup,
// or on-demand inside local-player load hooks.
std::string ensure_custom_archive_for_fmdl(const std::string &target_fmdl_name,
                                           std::string *out_status = nullptr);
bool rebuild_active_appearance(std::string *out_status = nullptr);

// Reads live vanilla SaveData fields from guest memory if available.
void query_live_vanilla_appearance(const uint8_t *mem, uint32_t *out_model,
                                   uint32_t *out_skin, uint32_t *out_eye);

CustomizerRuntimeStats get_runtime_stats();
bool request_plaza_reload();
void request_ink_color_reroll();

} // namespace char_customizer
