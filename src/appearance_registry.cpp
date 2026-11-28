#include "appearance_registry.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace char_customizer {

namespace {

std::string trim_ws(std::string s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
    s.erase(s.begin());
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
    s.pop_back();
  return s;
}

uint32_t fnv1a_step(uint32_t h, const void *data, size_t len) {
  const auto *p = static_cast<const uint8_t *>(data);
  for (size_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

uint32_t compute_plan_hash(const LocalRenderPlan &plan) {
  uint32_t h = 2166136261u;
  static constexpr char kPlanVersion[] = "v6";
  h = fnv1a_step(h, kPlanVersion, sizeof(kPlanVersion) - 1u);
  const uint32_t body = static_cast<uint32_t>(plan.body_model);
  const uint8_t hide = plan.preserve_helmet_hide_hair ? 1u : 0u;
  h = fnv1a_step(h, &body, sizeof(body));
  h = fnv1a_step(h, &hide, sizeof(hide));
  h = fnv1a_step(h, plan.hairstyle.id.data(), plan.hairstyle.id.size());
  const uint8_t src = static_cast<uint8_t>(plan.hairstyle.source);
  const uint8_t sc = static_cast<uint8_t>(plan.hairstyle.sculpt);
  h = fnv1a_step(h, &src, sizeof(src));
  h = fnv1a_step(h, &sc, sizeof(sc));
  h = fnv1a_step(h, &plan.hairstyle.custom_transform,
                 sizeof(plan.hairstyle.custom_transform));
  h = fnv1a_step(h, plan.hairstyle.obj_path.data(),
                 plan.hairstyle.obj_path.size());
  h = fnv1a_step(h, plan.skin_tone.id.data(), plan.skin_tone.id.size());
  h = fnv1a_step(h, plan.eye_color.id.data(), plan.eye_color.id.size());
  return h ? h : 1u;
}

// Lightweight flat-JSON field readers for modular pack.json files (zero
// third-party dependency).
bool extract_json_string(const std::string &json, const std::string &key,
                         std::string &out) {
  const std::string pat = "\"" + key + "\"";
  size_t pos = json.find(pat);
  if (pos == std::string::npos)
    return false;
  pos = json.find(':', pos + pat.size());
  if (pos == std::string::npos)
    return false;
  pos = json.find('"', pos + 1);
  if (pos == std::string::npos)
    return false;
  size_t end = pos + 1;
  std::string val;
  while (end < json.size()) {
    const char ch = json[end++];
    if (ch == '\\' && end < json.size()) {
      const char esc = json[end++];
      if (esc == '"' || esc == '\\' || esc == '/')
        val.push_back(esc);
      else if (esc == 'n')
        val.push_back('\n');
      else if (esc == 't')
        val.push_back('\t');
      else
        val.push_back(esc);
      continue;
    }
    if (ch == '"') {
      out = val;
      return true;
    }
    val.push_back(ch);
  }
  return false;
}

bool extract_json_float(const std::string &json, const std::string &key,
                        float &out) {
  const std::string pat = "\"" + key + "\"";
  size_t pos = json.find(pat);
  if (pos == std::string::npos)
    return false;
  pos = json.find(':', pos + pat.size());
  if (pos == std::string::npos)
    return false;
  ++pos;
  while (pos < json.size() &&
         std::isspace(static_cast<unsigned char>(json[pos])))
    ++pos;
  char *endp = nullptr;
  const float v = std::strtof(json.c_str() + pos, &endp);
  if (endp == json.c_str() + pos || !std::isfinite(v))
    return false;
  out = v;
  return true;
}

bool extract_json_int(const std::string &json, const std::string &key,
                      int &out) {
  float f = 0.0f;
  if (!extract_json_float(json, key, f))
    return false;
  out = static_cast<int>(std::lround(f));
  return true;
}

bool valid_pack_id(const std::string &id) {
  if (id.empty() || id.size() > 64u)
    return false;
  for (unsigned char c : id) {
    if (!std::isalnum(c) && c != '_' && c != '-')
      return false;
  }
  return true;
}

std::string sanitize_ini_str(std::string s) {
  s.erase(
      std::remove_if(s.begin(), s.end(),
                     [](unsigned char c) { return c == '\r' || c == '\n'; }),
      s.end());
  return s;
}

} // namespace

NetworkSafeAppearance clamp_vanilla_wire(int32_t raw_model, int32_t raw_skin,
                                         int32_t raw_eye) {
  NetworkSafeAppearance wire{};
  wire.model_type = (raw_model == 1) ? 1u : 0u;
  wire.skin_color =
      (raw_skin >= 0 && raw_skin <= 6) ? static_cast<uint32_t>(raw_skin) : 2u;
  wire.eye_color =
      (raw_eye >= 0 && raw_eye <= 7) ? static_cast<uint32_t>(raw_eye) : 0u;
  return wire;
}

bool validate_external_hair_obj(const std::string &obj_path,
                                std::string *out_error) {
  std::ifstream in(obj_path);
  if (!in.is_open()) {
    if (out_error)
      *out_error = "Missing OBJ file";
    return false;
  }

  size_t v_count = 0;
  size_t f_count = 0;
  float min_x = 1e9f, max_x = -1e9f;
  float min_y = 1e9f, max_y = -1e9f;
  float min_z = 1e9f, max_z = -1e9f;

  std::string line;
  while (std::getline(in, line)) {
    const std::string t = trim_ws(line);
    if (t.empty() || t[0] == '#')
      continue;
    if (t.rfind("v ", 0) == 0) {
      std::istringstream iss(t.substr(2));
      float x = 0.0f, y = 0.0f, z = 0.0f;
      if (!(iss >> x >> y >> z) || !std::isfinite(x) || !std::isfinite(y) ||
          !std::isfinite(z)) {
        if (out_error)
          *out_error = "Non-finite vertex in OBJ";
        return false;
      }
      min_x = std::min(min_x, x);
      max_x = std::max(max_x, x);
      min_y = std::min(min_y, y);
      max_y = std::max(max_y, y);
      min_z = std::min(min_z, z);
      max_z = std::max(max_z, z);
      ++v_count;
      if (v_count > 65000u) {
        if (out_error)
          *out_error = "OBJ exceeds 65k vertex budget";
        return false;
      }
    } else if (t.rfind("vt ", 0) == 0) {
      std::istringstream iss(t.substr(3));
      float u = 0.0f, v = 0.0f;
      if (!(iss >> u >> v) || !std::isfinite(u) || !std::isfinite(v)) {
        if (out_error)
          *out_error = "Non-finite UV in OBJ";
        return false;
      }
    } else if (t.rfind("f ", 0) == 0) {
      std::istringstream iss(t.substr(2));
      std::string tok;
      size_t verts_in_face = 0;
      while (iss >> tok) {
        const size_t slash = tok.find('/');
        const std::string vstr =
            (slash == std::string::npos) ? tok : tok.substr(0, slash);
        if (vstr.empty()) {
          if (out_error)
            *out_error = "Empty face index in OBJ";
          return false;
        }
        char *endp = nullptr;
        const long idx = std::strtol(vstr.c_str(), &endp, 10);
        if (endp == vstr.c_str() || idx == 0) {
          if (out_error)
            *out_error = "Invalid face index in OBJ";
          return false;
        }
        const size_t abs_idx = static_cast<size_t>(
            idx > 0 ? idx : (static_cast<long>(v_count) + idx + 1));
        if (abs_idx == 0 || abs_idx > v_count) {
          if (out_error)
            *out_error = "Out-of-range face vertex index in OBJ";
          return false;
        }
        ++verts_in_face;
      }
      if (verts_in_face < 3u) {
        if (out_error)
          *out_error = "Degenerate face in OBJ";
        return false;
      }
      ++f_count;
    }
  }

  if (v_count < 3u || f_count < 1u) {
    if (out_error)
      *out_error = "OBJ has no geometry";
    return false;
  }

  const float extent = std::max({max_x - min_x, max_y - min_y, max_z - min_z});
  const float max_abs =
      std::max({std::fabs(min_x), std::fabs(max_x), std::fabs(min_y),
                std::fabs(max_y), std::fabs(min_z), std::fabs(max_z)});
  if (extent <= 0.01f || max_abs > 40.0f) {
    if (out_error)
      *out_error = "OBJ bounding box outside safe head bounds";
    return false;
  }
  return true;
}

AppearanceRegistry &AppearanceRegistry::instance() {
  static AppearanceRegistry inst;
  return inst;
}

void AppearanceRegistry::initialize(const std::string &mods_dir) {
  std::lock_guard<std::mutex> lock(mutex_);
  mods_dir_ = mods_dir;
  while (!mods_dir_.empty() &&
         (mods_dir_.back() == '/' || mods_dir_.back() == '\\')) {
    mods_dir_.pop_back();
  }
  populate_builtins_locked();
  scan_external_packs_locked();
}

void AppearanceRegistry::reload_packs() {
  std::lock_guard<std::mutex> lock(mutex_);
  populate_builtins_locked();
  scan_external_packs_locked();
}

void AppearanceRegistry::populate_builtins_locked() {
  hairstyles_.clear();
  skin_tones_.clear();
  eye_colors_.clear();

  // 12 built-in hairstyles covering Splatoon 1, Splatoon 2, and Splatoon 3
  // silhouettes, usable independently on ANY body model (Girl / Boy /
  // Octoling).
  hairstyles_ = {
      {"s1_girl_classic",
       "Classic Tentacles",
       "Splatoon 1",
       HairSourceKind::GirlNative,
       HairSculptPreset::None,
       {},
       0u,
       false,
       "",
       ""},
      {"s1_boy_topknot",
       "Classic Topknot",
       "Splatoon 1",
       HairSourceKind::BoyNative,
       HairSculptPreset::None,
       {},
       1u,
       false,
       "",
       ""},
      {"s1_octo_curls",
       "Octoling Curls",
       "Splatoon 1",
       HairSourceKind::OctoNative,
       HairSculptPreset::None,
       {},
       0u,
       false,
       "",
       ""},

      {"s2_short_bob",
       "Short Bob",
       "Splatoon 2",
       HairSourceKind::GirlNative,
       HairSculptPreset::S2ShortBob,
       {},
       0u,
       false,
       "",
       ""},
      {"s2_twin_tails",
       "Twin Tails",
       "Splatoon 2",
       HairSourceKind::GirlNative,
       HairSculptPreset::S2TwinTails,
       {},
       0u,
       false,
       "",
       ""},
      {"s2_spiky",
       "Spiky Crest",
       "Splatoon 2",
       HairSourceKind::BoyNative,
       HairSculptPreset::S2Spiky,
       {},
       1u,
       false,
       "",
       ""},
      {"s2_hipster",
       "Side Swoop",
       "Splatoon 2",
       HairSourceKind::GirlNative,
       HairSculptPreset::S2SideSwoop,
       {},
       1u,
       false,
       "",
       ""},
      {"s2_mush",
       "Mush Cut",
       "Splatoon 2",
       HairSourceKind::BoyNative,
       HairSculptPreset::S2MushCut,
       {},
       1u,
       false,
       "",
       ""},

      {"s3_long_wave",
       "Long Wave",
       "Splatoon 3",
       HairSourceKind::GirlNative,
       HairSculptPreset::S3LongWave,
       {},
       0u,
       false,
       "",
       ""},
      {"s3_pompadour",
       "Pompadour",
       "Splatoon 3",
       HairSourceKind::BoyNative,
       HairSculptPreset::S3Pompadour,
       {},
       1u,
       false,
       "",
       ""},
      {"s3_octo_curl",
       "Octo Curl",
       "Splatoon 3",
       HairSourceKind::OctoNative,
       HairSculptPreset::S3OctoCurl,
       {},
       0u,
       false,
       "",
       ""},
      {"s3_short_crop",
       "Short Crop",
       "Splatoon 3",
       HairSourceKind::BoyNative,
       HairSculptPreset::S3ShortCrop,
       {},
       1u,
       false,
       "",
       ""},
  };

  // 7 vanilla skin tones (0..6) + 4 extended shades with vanilla fallbacks
  skin_tones_ = {
      {"skin_vanilla_0", "Tone 1 (Fair)", 0u, false, 1.00f, 1.00f, 1.00f, 0.99f,
       0.89f, 0.82f},
      {"skin_vanilla_1", "Tone 2 (Light)", 1u, false, 1.00f, 1.00f, 1.00f,
       0.97f, 0.84f, 0.74f},
      {"skin_vanilla_2", "Tone 3 (Medium)", 2u, false, 1.00f, 1.00f, 1.00f,
       0.93f, 0.77f, 0.65f},
      {"skin_vanilla_3", "Tone 4 (Tan)", 3u, false, 1.00f, 1.00f, 1.00f, 0.86f,
       0.66f, 0.52f},
      {"skin_vanilla_4", "Tone 5 (Golden)", 4u, false, 1.00f, 1.00f, 1.00f,
       0.76f, 0.55f, 0.42f},
      {"skin_vanilla_5", "Tone 6 (Deep)", 5u, false, 1.00f, 1.00f, 1.00f, 0.62f,
       0.43f, 0.32f},
      {"skin_vanilla_6", "Tone 7 (Rich)", 6u, false, 1.00f, 1.00f, 1.00f, 0.47f,
       0.31f, 0.23f},

      {"skin_ext_porcelain", "Porcelain (Ext)", 0u, true, 1.05f, 1.03f, 1.05f,
       0.99f, 0.93f, 0.90f},
      {"skin_ext_warm_peach", "Warm Peach (Ext)", 2u, true, 1.06f, 0.95f, 0.88f,
       0.96f, 0.74f, 0.60f},
      {"skin_ext_olive", "Sunlit Olive (Ext)", 3u, true, 0.96f, 0.97f, 0.84f,
       0.82f, 0.67f, 0.49f},
      {"skin_ext_espresso", "Espresso (Ext)", 6u, true, 0.82f, 0.80f, 0.78f,
       0.38f, 0.25f, 0.19f},
  };

  // 8 vanilla eye colors (0..7) + 6 extended Splatoon 2/3 iris tints with
  // vanilla fallbacks
  eye_colors_ = {
      {"eye_vanilla_0", "Amber Brown", 0u, false, 1.00f, 1.00f, 1.00f, 0.48f,
       0.27f, 0.14f},
      {"eye_vanilla_1", "Vivid Pink", 1u, false, 1.00f, 1.00f, 1.00f, 0.94f,
       0.26f, 0.56f},
      {"eye_vanilla_2", "Cobalt Blue", 2u, false, 1.00f, 1.00f, 1.00f, 0.18f,
       0.48f, 0.95f},
      {"eye_vanilla_3", "Tangerine", 3u, false, 1.00f, 1.00f, 1.00f, 0.96f,
       0.48f, 0.12f},
      {"eye_vanilla_4", "Leaf Green", 4u, false, 1.00f, 1.00f, 1.00f, 0.24f,
       0.78f, 0.32f},
      {"eye_vanilla_5", "Royal Purple", 5u, false, 1.00f, 1.00f, 1.00f, 0.58f,
       0.28f, 0.88f},
      {"eye_vanilla_6", "Sun Yellow", 6u, false, 1.00f, 1.00f, 1.00f, 0.94f,
       0.82f, 0.16f},
      {"eye_vanilla_7", "Sky Cyan", 7u, false, 1.00f, 1.00f, 1.00f, 0.30f,
       0.82f, 0.94f},

      {"eye_ext_teal", "Deep Teal (Ext)", 2u, true, 0.35f, 0.95f, 0.90f, 0.14f,
       0.72f, 0.72f},
      {"eye_ext_crimson", "Ruby Crimson (Ext)", 1u, true, 1.20f, 0.30f, 0.35f,
       0.88f, 0.16f, 0.22f},
      {"eye_ext_gold", "Splat Gold (Ext)", 6u, true, 1.18f, 0.92f, 0.22f, 0.98f,
       0.76f, 0.14f},
      {"eye_ext_lavender", "Lavender (Ext)", 5u, true, 0.86f, 0.72f, 1.18f,
       0.74f, 0.62f, 0.96f},
      {"eye_ext_mint", "Neon Mint (Ext)", 4u, true, 0.48f, 1.15f, 0.85f, 0.36f,
       0.96f, 0.74f},
      {"eye_ext_silver", "Pearl Silver (Ext)", 7u, true, 0.92f, 0.95f, 1.02f,
       0.80f, 0.84f, 0.90f},
  };

  ink_colors_ = {
      {"ink_orange", "Orange", 0.812f, 0.345f, 0.110f},
      {"ink_blue", "Blue", 0.078f, 0.078f, 0.580f},
      {"ink_pink", "Neon Pink", 0.855f, 0.216f, 0.506f},
      {"ink_green", "Lime Green", 0.416f, 0.749f, 0.047f},
      {"ink_purple", "Purple", 0.431f, 0.027f, 0.541f},
      {"ink_turquoise", "Turquoise", 0.055f, 0.631f, 0.510f},
      {"ink_yellow", "Yellow", 0.988f, 0.843f, 0.020f},
      {"ink_sky_blue", "Sky Blue", 0.133f, 0.549f, 1.000f},
      {"ink_magenta", "Magenta", 0.647f, 0.169f, 0.522f},
      {"ink_mint", "Soda Mint", 0.557f, 0.859f, 0.722f},
      {"ink_lilac", "Lilac", 0.322f, 0.361f, 0.961f},
      {"ink_marigold", "Marigold", 0.933f, 0.667f, 0.024f},
      {"ink_coral", "Coral Red", 0.788f, 0.204f, 0.341f},
      {"ink_navy", "Deep Navy", 0.055f, 0.220f, 0.765f},
      {"ink_forest", "Forest Green", 0.176f, 0.576f, 0.078f},
      {"ink_chartreuse", "Chartreuse", 0.569f, 0.690f, 0.047f},
  };
}

void AppearanceRegistry::scan_external_packs_locked() {
  pack_warnings_.clear();
  external_pack_count_ = 0u;
  if (mods_dir_.empty())
    return;

  const std::filesystem::path packs_root =
      std::filesystem::path(mods_dir_) / "character_customizer_packs";
  std::error_code ec;
  if (!std::filesystem::exists(packs_root, ec) ||
      !std::filesystem::is_directory(packs_root, ec)) {
    return;
  }

  for (const auto &entry :
       std::filesystem::directory_iterator(packs_root, ec)) {
    if (!entry.is_directory(ec))
      continue;
    const std::filesystem::path manifest = entry.path() / "pack.json";
    if (!std::filesystem::is_regular_file(manifest, ec))
      continue;

    std::ifstream f(manifest);
    if (!f.is_open()) {
      pack_warnings_.push_back("Could not open " + manifest.string());
      continue;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string json = ss.str();

    HairstyleDef def{};
    if (!extract_json_string(json, "id", def.id) || !valid_pack_id(def.id)) {
      pack_warnings_.push_back("Invalid pack id in " + manifest.string());
      continue;
    }
    const bool dup =
        std::any_of(hairstyles_.begin(), hairstyles_.end(),
                    [&](const HairstyleDef &h) { return h.id == def.id; });
    if (dup) {
      pack_warnings_.push_back("Duplicate hairstyle id: " + def.id);
      continue;
    }
    if (!extract_json_string(json, "name", def.display_name) ||
        def.display_name.empty()) {
      def.display_name = def.id;
    }
    def.category = "Custom Pack";
    def.is_external = true;
    def.pack_dir = entry.path().string();

    int fallback_model = 0;
    if (extract_json_int(json, "vanilla_fallback_model", fallback_model)) {
      def.fallback_vanilla_model = (fallback_model == 1) ? 1u : 0u;
    }

    std::string obj_rel;
    if (extract_json_string(json, "obj", obj_rel) && !obj_rel.empty()) {
      const std::filesystem::path obj_full = entry.path() / obj_rel;
      std::string err;
      if (!validate_external_hair_obj(obj_full.string(), &err)) {
        pack_warnings_.push_back("Skipped pack " + def.id + ": " + err);
        continue;
      }
      def.source = HairSourceKind::ExternalObj;
      def.sculpt = HairSculptPreset::CustomParametric;
      def.obj_path = obj_full.string();
    } else {
      std::string base_src = "girl";
      extract_json_string(json, "base", base_src);
      if (base_src == "boy")
        def.source = HairSourceKind::BoyNative;
      else if (base_src == "octo")
        def.source = HairSourceKind::OctoNative;
      else
        def.source = HairSourceKind::GirlNative;
      def.sculpt = HairSculptPreset::CustomParametric;
    }

    extract_json_float(json, "scale_x", def.custom_transform.scale_x);
    extract_json_float(json, "scale_y", def.custom_transform.scale_y);
    extract_json_float(json, "scale_z", def.custom_transform.scale_z);
    extract_json_float(json, "offset_x", def.custom_transform.offset_x);
    extract_json_float(json, "offset_y", def.custom_transform.offset_y);
    extract_json_float(json, "offset_z", def.custom_transform.offset_z);
    extract_json_float(json, "tilt_deg", def.custom_transform.tilt_deg);

    auto clamp_f = [](float v, float lo, float hi, float defv) {
      return std::isfinite(v) ? std::clamp(v, lo, hi) : defv;
    };
    def.custom_transform.scale_x =
        clamp_f(def.custom_transform.scale_x, 0.3f, 2.5f, 1.0f);
    def.custom_transform.scale_y =
        clamp_f(def.custom_transform.scale_y, 0.3f, 2.5f, 1.0f);
    def.custom_transform.scale_z =
        clamp_f(def.custom_transform.scale_z, 0.3f, 2.5f, 1.0f);
    def.custom_transform.offset_x =
        clamp_f(def.custom_transform.offset_x, -5.0f, 5.0f, 0.0f);
    def.custom_transform.offset_y =
        clamp_f(def.custom_transform.offset_y, -5.0f, 5.0f, 0.0f);
    def.custom_transform.offset_z =
        clamp_f(def.custom_transform.offset_z, -5.0f, 5.0f, 0.0f);
    def.custom_transform.tilt_deg =
        clamp_f(def.custom_transform.tilt_deg, -45.0f, 45.0f, 0.0f);

    hairstyles_.push_back(std::move(def));
    ++external_pack_count_;
  }
}

std::string AppearanceRegistry::mods_dir() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mods_dir_;
}

std::string AppearanceRegistry::game_dir() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (mods_dir_.empty())
    return ".";
  return std::filesystem::path(mods_dir_).parent_path().string();
}

std::string AppearanceRegistry::packs_dir() const {
  return (std::filesystem::path(mods_dir()) / "character_customizer_packs")
      .string();
}

std::string AppearanceRegistry::ini_path() const {
  return (std::filesystem::path(mods_dir()) / "character_customizer.ini")
      .string();
}

std::string AppearanceRegistry::player_pack_path() const {
  return (std::filesystem::path(game_dir()) / "content" / "Pack" /
          "Player.pack")
      .string();
}

std::string AppearanceRegistry::overlay_model_dir() const {
  return (std::filesystem::path(mods_dir()) / "content" / "Model").string();
}

std::vector<HairstyleDef> AppearanceRegistry::hairstyles() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return hairstyles_;
}

std::vector<SkinToneDef> AppearanceRegistry::skin_tones() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return skin_tones_;
}

std::vector<EyeColorDef> AppearanceRegistry::eye_colors() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return eye_colors_;
}

std::vector<InkColorDef> AppearanceRegistry::ink_colors() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ink_colors_;
}

bool AppearanceRegistry::find_ink_color(const std::string &id,
                                        InkColorDef &out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto &c : ink_colors_) {
    if (c.id == id) {
      out = c;
      return true;
    }
  }
  return false;
}

std::vector<InkColorDef> AppearanceRegistry::resolve_favorite_ink_colors(
    const CustomizerConfig &cfg) const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<InkColorDef> out;
  for (const auto &id : cfg.favorite_ink_colors) {
    bool dup = false;
    for (const auto &existing : out) {
      if (existing.id == id) {
        dup = true;
        break;
      }
    }
    if (dup)
      continue;
    for (const auto &c : ink_colors_) {
      if (c.id == id) {
        out.push_back(c);
        break;
      }
    }
  }
  return out;
}

bool AppearanceRegistry::pick_spawn_ink_color(const CustomizerConfig &cfg,
                                              const std::string &last_id,
                                              uint32_t rng_val,
                                              InkColorDef &out) const {
  if (!cfg.custom_appearances_enabled || cfg.vanilla_appearance_mode)
    return false;
  const std::vector<InkColorDef> favs = resolve_favorite_ink_colors(cfg);
  if (favs.empty())
    return false;
  if (favs.size() == 1u) {
    out = favs[0];
    return true;
  }
  std::vector<const InkColorDef *> pool;
  pool.reserve(favs.size());
  for (const auto &c : favs) {
    if (c.id != last_id)
      pool.push_back(&c);
  }
  if (pool.empty()) {
    for (const auto &c : favs)
      pool.push_back(&c);
  }
  out = *pool[static_cast<size_t>(rng_val) % pool.size()];
  return true;
}

InkColorDef AppearanceRegistry::pick_contrasting_opponent_color(
    const InkColorDef &ally, float cur_r, float cur_g, float cur_b) const {
  const float dr = cur_r - ally.r;
  const float dg = cur_g - ally.g;
  const float db = cur_b - ally.b;
  if (dr * dr + dg * dg + db * db >= 0.45f * 0.45f) {
    return InkColorDef{"", "", cur_r, cur_g, cur_b};
  }
  std::lock_guard<std::mutex> lock(mutex_);
  InkColorDef best{"", "", cur_r, cur_g, cur_b};
  float best_dist_sq = -1.0f;
  for (const auto &c : ink_colors_) {
    const float cdr = c.r - ally.r;
    const float cdg = c.g - ally.g;
    const float cdb = c.b - ally.b;
    const float dist_sq = cdr * cdr + cdg * cdg + cdb * cdb;
    if (dist_sq > best_dist_sq) {
      best_dist_sq = dist_sq;
      best = c;
    }
  }
  return best;
}

size_t AppearanceRegistry::external_pack_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return external_pack_count_;
}

std::vector<std::string> AppearanceRegistry::pack_warnings() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return pack_warnings_;
}

CustomizerConfig AppearanceRegistry::config() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return cfg_;
}

void AppearanceRegistry::set_config(const CustomizerConfig &cfg) {
  std::lock_guard<std::mutex> lock(mutex_);
  cfg_ = cfg;
}

void AppearanceRegistry::reset_to_vanilla(uint32_t vanilla_model,
                                          uint32_t vanilla_skin,
                                          uint32_t vanilla_eye) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto safe = clamp_vanilla_wire(static_cast<int32_t>(vanilla_model),
                                       static_cast<int32_t>(vanilla_skin),
                                       static_cast<int32_t>(vanilla_eye));
  cfg_.body_model =
      (safe.model_type == 1u) ? BodyModelId::Boy : BodyModelId::Girl;
  cfg_.hairstyle_id =
      (safe.model_type == 1u) ? "s1_boy_topknot" : "s1_girl_classic";
  cfg_.skin_tone_id = "skin_vanilla_" + std::to_string(safe.skin_color);
  cfg_.eye_color_id = "eye_vanilla_" + std::to_string(safe.eye_color);
  cfg_.favorite_ink_colors.clear();
}

bool AppearanceRegistry::save_config() const {
  const std::string path = ini_path();
  if (path.empty())
    return false;
  const CustomizerConfig cfg = config();

  const std::filesystem::path tmp = std::filesystem::path(path + ".tmp");
  std::error_code ec;
  std::filesystem::create_directories(tmp.parent_path(), ec);
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out.is_open())
      return false;
    out << "custom_appearances=" << (cfg.custom_appearances_enabled ? 1 : 0)
        << "\n";
    out << "multiplayer_compatibility="
        << (cfg.multiplayer_compatibility ? 1 : 0) << "\n";
    out << "vanilla_appearance_mode=" << (cfg.vanilla_appearance_mode ? 1 : 0)
        << "\n";
    out << "preserve_helmet_hide_hair="
        << (cfg.preserve_helmet_hide_hair ? 1 : 0) << "\n";
    out << "body_model=" << static_cast<uint32_t>(cfg.body_model) << "\n";
    out << "hairstyle_id=" << sanitize_ini_str(cfg.hairstyle_id) << "\n";
    out << "skin_tone_id=" << sanitize_ini_str(cfg.skin_tone_id) << "\n";
    out << "eye_color_id=" << sanitize_ini_str(cfg.eye_color_id) << "\n";
    out << "favorite_ink_colors=";
    for (size_t i = 0; i < cfg.favorite_ink_colors.size(); ++i) {
      if (i > 0u)
        out << ",";
      out << sanitize_ini_str(cfg.favorite_ink_colors[i]);
    }
    out << "\n";
    out.flush();
    if (!out.good())
      return false;
  }
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(tmp, path, ec);
  }
  return !ec;
}

bool AppearanceRegistry::load_config() {
  const std::string path = ini_path();
  std::ifstream in(path);
  if (!in.is_open())
    return false;

  CustomizerConfig loaded = config();
  std::string line;
  while (std::getline(in, line)) {
    line = trim_ws(line);
    if (line.empty() || line[0] == '#' || line[0] == ';')
      continue;
    const size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string k = trim_ws(line.substr(0, eq));
    const std::string v = trim_ws(line.substr(eq + 1));
    if (k == "custom_appearances")
      loaded.custom_appearances_enabled = (v == "1" || v == "true");
    else if (k == "multiplayer_compatibility")
      loaded.multiplayer_compatibility = (v == "1" || v == "true");
    else if (k == "vanilla_appearance_mode")
      loaded.vanilla_appearance_mode = (v == "1" || v == "true");
    else if (k == "preserve_helmet_hide_hair")
      loaded.preserve_helmet_hide_hair = (v == "1" || v == "true");
    else if (k == "body_model") {
      const int b = std::atoi(v.c_str());
      if (b == 1)
        loaded.body_model = BodyModelId::Boy;
      else if (b == 2)
        loaded.body_model = BodyModelId::Octoling;
      else
        loaded.body_model = BodyModelId::Girl;
    } else if (k == "hairstyle_id" && !v.empty())
      loaded.hairstyle_id = v;
    else if (k == "skin_tone_id" && !v.empty())
      loaded.skin_tone_id = v;
    else if (k == "eye_color_id" && !v.empty())
      loaded.eye_color_id = v;
    else if (k == "favorite_ink_colors") {
      loaded.favorite_ink_colors.clear();
      std::istringstream iss(v);
      std::string tok;
      while (std::getline(iss, tok, ',')) {
        tok = trim_ws(tok);
        if (!tok.empty())
          loaded.favorite_ink_colors.push_back(tok);
      }
    }
  }

  // Validate loaded IDs cleanly through fallback resolver? Wait: preserve
  // unknown IDs only if multiplayer_compatibility is enabled (it will cleanly
  // fall back until pack loads), or sanitize? Keep ID in config so a pack added
  // later still activates, while resolve() safely falls back!
  set_config(loaded);
  return true;
}

ResolvedAppearance AppearanceRegistry::resolve(
    const CustomizerConfig &cfg, uint32_t live_vanilla_model,
    uint32_t live_vanilla_skin, uint32_t live_vanilla_eye,
    bool *external_asset_validator) const {
  std::lock_guard<std::mutex> lock(mutex_);
  ResolvedAppearance res{};
  const auto safe_live =
      clamp_vanilla_wire(static_cast<int32_t>(live_vanilla_model),
                         static_cast<int32_t>(live_vanilla_skin),
                         static_cast<int32_t>(live_vanilla_eye));

  // Resolve Skin & Eye definitions with fallback
  const SkinToneDef *skin = nullptr;
  for (const auto &s : skin_tones_) {
    if (s.id == cfg.skin_tone_id) {
      skin = &s;
      break;
    }
  }
  if (!skin && !skin_tones_.empty()) {
    skin = &skin_tones_[std::min<size_t>(safe_live.skin_color,
                                         skin_tones_.size() - 1u)];
    res.used_fallback = true;
    res.fallback_reason =
        "Unknown skin tone ID; fell back to vanilla skin tone.";
  }

  const EyeColorDef *eye = nullptr;
  for (const auto &e : eye_colors_) {
    if (e.id == cfg.eye_color_id) {
      eye = &e;
      break;
    }
  }
  if (!eye && !eye_colors_.empty()) {
    eye = &eye_colors_[std::min<size_t>(safe_live.eye_color,
                                        eye_colors_.size() - 1u)];
    res.used_fallback = true;
    res.fallback_reason =
        "Unknown eye color ID; fell back to vanilla eye color.";
  }

  // Wire appearance is ALWAYS strictly vanilla {model in 0..1, skin in 0..6,
  // eye in 0..7}.
  if (cfg.vanilla_appearance_mode || !cfg.custom_appearances_enabled) {
    res.wire = safe_live;
    res.local.use_custom_model = false;
    res.local.body_model =
        (safe_live.model_type == 1u) ? BodyModelId::Boy : BodyModelId::Girl;
    if (skin)
      res.local.skin_tone = *skin;
    if (eye)
      res.local.eye_color = *eye;
    return res;
  }

  // Determine network-safe fallback model/skin/eye for remote players
  const uint32_t body_u32 = static_cast<uint32_t>(cfg.body_model);
  const uint32_t wire_model =
      (body_u32 <= 1u) ? body_u32
                       : 0u; // Octoling maps safely to Girl (0) on Wii U / Cemu
  const uint32_t wire_skin =
      skin ? (skin->vanilla_id <= 6u ? skin->vanilla_id : 2u)
           : safe_live.skin_color;
  const uint32_t wire_eye = eye ? (eye->vanilla_id <= 7u ? eye->vanilla_id : 0u)
                                : safe_live.eye_color;
  res.wire = clamp_vanilla_wire(static_cast<int32_t>(wire_model),
                                static_cast<int32_t>(wire_skin),
                                static_cast<int32_t>(wire_eye));

  // Resolve Hairstyle with fallback
  const HairstyleDef *hair = nullptr;
  for (const auto &h : hairstyles_) {
    if (h.id == cfg.hairstyle_id) {
      hair = &h;
      break;
    }
  }
  if (!hair && !hairstyles_.empty()) {
    hair = &hairstyles_[res.wire.model_type == 1u ? 1u : 0u];
    res.used_fallback = true;
    res.fallback_reason =
        "Missing hairstyle '" + cfg.hairstyle_id + "'; fell back to vanilla.";
  }

  // Validate external pack mesh if applicable
  if (hair && hair->source == HairSourceKind::ExternalObj) {
    bool ok = true;
    std::string err;
    if (external_asset_validator) {
      ok = *external_asset_validator;
    } else {
      ok = validate_external_hair_obj(hair->obj_path, &err);
    }
    if (!ok && !hairstyles_.empty()) {
      hair = &hairstyles_[res.wire.model_type == 1u ? 1u : 0u];
      res.used_fallback = true;
      res.fallback_reason = "External pack asset invalid" +
                            (err.empty() ? "" : (": " + err)) + "; fell back.";
    }
  }

  res.local.body_model = (body_u32 <= 2u) ? cfg.body_model : BodyModelId::Girl;
  if (hair)
    res.local.hairstyle = *hair;
  if (skin)
    res.local.skin_tone = *skin;
  if (eye)
    res.local.eye_color = *eye;
  res.local.preserve_helmet_hide_hair = cfg.preserve_helmet_hide_hair;

  // Exact vanilla choice matching the requested/live slot model with no
  // extended colors and no Octoling needs zero custom BFRES synthesis.
  const bool is_exact_live_vanilla_girl =
      (safe_live.model_type == 0u && res.local.body_model == BodyModelId::Girl &&
       res.local.hairstyle.id == "s1_girl_classic");
  const bool is_exact_live_vanilla_boy =
      (safe_live.model_type == 1u && res.local.body_model == BodyModelId::Boy &&
       res.local.hairstyle.id == "s1_boy_topknot");
  const bool has_extended_color =
      (res.local.skin_tone.is_extended || res.local.eye_color.is_extended);
  res.local.use_custom_model =
      !(is_exact_live_vanilla_girl || is_exact_live_vanilla_boy) ||
      has_extended_color;

  if (res.local.use_custom_model) {
    res.local.content_hash = compute_plan_hash(res.local);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "CustPl_%08X", res.local.content_hash);
    res.local.custom_archive_name = buf;
  }

  return res;
}

ResolvedAppearance
AppearanceRegistry::resolve_current(uint32_t live_vanilla_model,
                                    uint32_t live_vanilla_skin,
                                    uint32_t live_vanilla_eye) const {
  return resolve(config(), live_vanilla_model, live_vanilla_skin,
                 live_vanilla_eye, nullptr);
}

void AppearanceRegistry::set_active_custom_archive(
    const std::string &archive_name, uint32_t hash,
    const std::string &status_msg) {
  std::lock_guard<std::mutex> lock(mutex_);
  active_custom_archive_ = archive_name;
  active_custom_hash_ = hash;
  last_build_status_ = status_msg;
}

std::string AppearanceRegistry::active_custom_archive() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_custom_archive_;
}

uint32_t AppearanceRegistry::active_custom_hash() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_custom_hash_;
}

std::string AppearanceRegistry::last_build_status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_build_status_;
}

} // namespace char_customizer
