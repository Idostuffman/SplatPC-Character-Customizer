#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace char_customizer {

enum class BodyModelId : uint32_t {
  Girl = 0u,
  Boy = 1u,
  Octoling = 2u,
};

enum class HairSourceKind : uint8_t {
  GirlNative = 0u,
  BoyNative = 1u,
  OctoNative = 2u,
  ExternalObj = 3u,
};

enum class HairSculptPreset : uint8_t {
  None = 0u,
  S2ShortBob = 1u,
  S2TwinTails = 2u,
  S2Spiky = 3u,
  S2SideSwoop = 4u,
  S2MushCut = 5u,
  S3LongWave = 6u,
  S3Pompadour = 7u,
  S3OctoCurl = 8u,
  S3ShortCrop = 9u,
  CustomParametric = 10u,
};

struct HairParamTransform {
  float scale_x = 1.0f;
  float scale_y = 1.0f;
  float scale_z = 1.0f;
  float offset_x = 0.0f;
  float offset_y = 0.0f;
  float offset_z = 0.0f;
  float tilt_deg = 0.0f;
};

struct HairstyleDef {
  std::string id;
  std::string display_name;
  std::string
      category; // "Splatoon 1", "Splatoon 2", "Splatoon 3", "Custom Pack"
  HairSourceKind source = HairSourceKind::GirlNative;
  HairSculptPreset sculpt = HairSculptPreset::None;
  HairParamTransform custom_transform{};
  uint32_t fallback_vanilla_model = 0u; // 0 = Girl, 1 = Boy
  bool is_external = false;
  std::string pack_dir;
  std::string obj_path;
};

struct SkinToneDef {
  std::string id;
  std::string display_name;
  uint32_t vanilla_id = 0u; // Strictly 0..6
  bool is_extended = false;
  float tint_r = 1.0f;
  float tint_g = 1.0f;
  float tint_b = 1.0f;
  float preview_r = 0.96f;
  float preview_g = 0.80f;
  float preview_b = 0.70f;
};

struct EyeColorDef {
  std::string id;
  std::string display_name;
  uint32_t vanilla_id = 0u; // Strictly 0..7
  bool is_extended = false;
  float tint_r = 1.0f;
  float tint_g = 1.0f;
  float tint_b = 1.0f;
  float preview_r = 0.45f;
  float preview_g = 0.25f;
  float preview_b = 0.14f;
};

struct InkColorDef {
  std::string id;
  std::string display_name;
  float r = 0.812f;
  float g = 0.345f;
  float b = 0.110f;
};

struct CustomizerConfig {
  bool custom_appearances_enabled = true;
  bool multiplayer_compatibility =
      true; // Strictly vanilla wire + safe fallback
  bool vanilla_appearance_mode =
      false; // Always render vanilla locally and online
  bool preserve_helmet_hide_hair = true;

  BodyModelId body_model = BodyModelId::Girl;
  std::string hairstyle_id = "s1_girl_classic";
  std::string skin_tone_id = "skin_vanilla_2";
  std::string eye_color_id = "eye_vanilla_0";
  std::vector<std::string> favorite_ink_colors;
};

// Strictly vanilla representation allowed on Cmn::PlayerInfo / ENL wire
// packets.
struct NetworkSafeAppearance {
  uint32_t model_type = 0u; // Strictly 0 or 1
  uint32_t skin_color = 0u; // Strictly 0..6
  uint32_t eye_color = 0u;  // Strictly 0..7
};

// Complete local rendering resolution produced from CustomizerConfig +
// installed packs.
struct LocalRenderPlan {
  bool use_custom_model = false;
  BodyModelId body_model = BodyModelId::Girl;
  HairstyleDef hairstyle{};
  SkinToneDef skin_tone{};
  EyeColorDef eye_color{};
  bool preserve_helmet_hide_hair = true;
  uint32_t content_hash = 0u;
  std::string custom_archive_name; // e.g., "CustPl_1a2b3c4d"
};

struct ResolvedAppearance {
  LocalRenderPlan local{};
  NetworkSafeAppearance wire{};
  bool used_fallback = false;
  std::string fallback_reason;
};

} // namespace char_customizer
