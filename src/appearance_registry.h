#pragma once

#include "appearance_types.h"

#include <mutex>
#include <string>
#include <vector>

namespace char_customizer {

inline const char *body_model_name(BodyModelId id) {
  switch (id) {
  case BodyModelId::Girl:
    return "Inkling Girl";
  case BodyModelId::Boy:
    return "Inkling Boy";
  case BodyModelId::Octoling:
    return "Octoling";
  }
  return "Inkling Girl";
}

inline const char *vanilla_model_archive(uint32_t vanilla_model_type) {
  return (vanilla_model_type == 1u) ? "Player01" : "Player00";
}

inline const char *body_archive_name(BodyModelId id) {
  switch (id) {
  case BodyModelId::Girl:
    return "Player00";
  case BodyModelId::Boy:
    return "Player01";
  case BodyModelId::Octoling:
    return "Rival00";
  }
  return "Player00";
}

inline const char *body_anim_archive_name(BodyModelId id) {
  // Octoling shares the Girl humanoid bone layout & animation set cleanly
  return (id == BodyModelId::Boy) ? "Player01_anim" : "Player00_anim";
}

// Clamps any untrusted PlayerInfo/wire appearance values strictly into Splatoon
// 1 vanilla bounds.
NetworkSafeAppearance clamp_vanilla_wire(int32_t raw_model, int32_t raw_skin,
                                         int32_t raw_eye);

// Validates an external Wavefront .obj hair mesh file (finite coords, valid
// triangles, head bounding region).
bool validate_external_hair_obj(const std::string &obj_path,
                                std::string *out_error = nullptr);

class AppearanceRegistry {
public:
  static AppearanceRegistry &instance();

  void initialize(const std::string &mods_dir);
  void reload_packs();

  std::string mods_dir() const;
  std::string game_dir() const;
  std::string packs_dir() const;
  std::string ini_path() const;
  std::string player_pack_path() const;
  std::string overlay_model_dir() const;

  std::vector<HairstyleDef> hairstyles() const;
  std::vector<SkinToneDef> skin_tones() const;
  std::vector<EyeColorDef> eye_colors() const;
  std::vector<InkColorDef> ink_colors() const;
  bool find_ink_color(const std::string &id, InkColorDef &out) const;
  std::vector<InkColorDef>
  resolve_favorite_ink_colors(const CustomizerConfig &cfg) const;
  bool pick_spawn_ink_color(const CustomizerConfig &cfg,
                            const std::string &last_id, uint32_t rng_val,
                            InkColorDef &out) const;
  InkColorDef pick_contrasting_opponent_color(const InkColorDef &ally,
                                              float cur_r, float cur_g,
                                              float cur_b) const;
  size_t external_pack_count() const;
  std::vector<std::string> pack_warnings() const;

  CustomizerConfig config() const;
  void set_config(const CustomizerConfig &cfg);
  bool save_config() const;
  bool load_config();
  void reset_to_vanilla(uint32_t vanilla_model = 0u, uint32_t vanilla_skin = 2u,
                        uint32_t vanilla_eye = 0u);

  // Pure resolution helper usable in tests without touching disk state
  ResolvedAppearance resolve(const CustomizerConfig &cfg,
                             uint32_t live_vanilla_model = 0u,
                             uint32_t live_vanilla_skin = 2u,
                             uint32_t live_vanilla_eye = 0u,
                             bool *external_asset_validator = nullptr) const;
  ResolvedAppearance resolve_current(uint32_t live_vanilla_model = 0u,
                                     uint32_t live_vanilla_skin = 2u,
                                     uint32_t live_vanilla_eye = 0u) const;

  // Active built .szs archive name ("CustPl_<hash>" or empty when vanilla
  // fallback applies)
  void set_active_custom_archive(const std::string &archive_name, uint32_t hash,
                                 const std::string &status_msg);
  std::string active_custom_archive() const;
  uint32_t active_custom_hash() const;
  std::string last_build_status() const;

private:
  AppearanceRegistry() = default;
  void populate_builtins_locked();
  void scan_external_packs_locked();

  mutable std::mutex mutex_;
  std::string mods_dir_;
  CustomizerConfig cfg_{};
  std::vector<HairstyleDef> hairstyles_;
  std::vector<SkinToneDef> skin_tones_;
  std::vector<EyeColorDef> eye_colors_;
  std::vector<InkColorDef> ink_colors_;
  std::vector<std::string> pack_warnings_;
  size_t external_pack_count_ = 0u;

  std::string active_custom_archive_;
  uint32_t active_custom_hash_ = 0u;
  std::string last_build_status_ = "Ready";
};

} // namespace char_customizer
