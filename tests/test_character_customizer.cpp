#include "appearance_registry.h"
#include "bfres_customizer.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace char_customizer;

namespace {

int g_failures = 0;

#define EXPECT_TRUE(cond)                                                      \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "[FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond);   \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

#define EXPECT_EQ(a, b)                                                        \
  do {                                                                         \
    if ((a) != (b)) {                                                          \
      std::fprintf(stderr, "[FAIL] %s:%d: %s != %s\n", __FILE__, __LINE__, #a, \
                   #b);                                                        \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

void test_wire_protocol_and_scenarios() {
  const fs::path tmp_mods =
      fs::temp_directory_path() / "splatpc_cust_test_mods";
  std::error_code ec;
  fs::remove_all(tmp_mods, ec);
  fs::create_directories(tmp_mods, ec);

  auto &reg = AppearanceRegistry::instance();
  reg.initialize(tmp_mods.string());

  // Scenario A: Custom-mod client -> Vanilla Wii U / Cemu client wire safety
  // across ALL combos
  for (BodyModelId b :
       {BodyModelId::Girl, BodyModelId::Boy, BodyModelId::Octoling}) {
    for (const auto &h : reg.hairstyles()) {
      for (const auto &s : reg.skin_tones()) {
        for (const auto &e : reg.eye_colors()) {
          CustomizerConfig cfg{};
          cfg.body_model = b;
          cfg.hairstyle_id = h.id;
          cfg.skin_tone_id = s.id;
          cfg.eye_color_id = e.id;
          const ResolvedAppearance r = reg.resolve(cfg);
          // Strict vanilla protocol invariants: NEVER send Octoling(2),
          // out-of-range skin/eye, or custom hair
          EXPECT_TRUE(r.wire.model_type <= 1u);
          EXPECT_TRUE(r.wire.skin_color <= 6u);
          EXPECT_TRUE(r.wire.eye_color <= 7u);
        }
      }
    }
  }

  // Boy body + Girl hair is independent and activates custom synthesis!
  {
    CustomizerConfig cfg{};
    cfg.body_model = BodyModelId::Boy;
    cfg.hairstyle_id = "s1_girl_classic";
    const ResolvedAppearance r = reg.resolve(cfg);
    EXPECT_TRUE(r.local.use_custom_model);
    EXPECT_EQ(r.local.body_model, BodyModelId::Boy);
    EXPECT_EQ(r.local.hairstyle.id, std::string("s1_girl_classic"));
    EXPECT_EQ(r.wire.model_type, 1u);
  }

  // Girl body + Boy hair
  {
    CustomizerConfig cfg{};
    cfg.body_model = BodyModelId::Girl;
    cfg.hairstyle_id = "s1_boy_topknot";
    const ResolvedAppearance r = reg.resolve(cfg);
    EXPECT_TRUE(r.local.use_custom_model);
    EXPECT_EQ(r.local.body_model, BodyModelId::Girl);
    EXPECT_EQ(r.local.hairstyle.id, std::string("s1_boy_topknot"));
    EXPECT_EQ(r.wire.model_type, 0u);
  }

  // Opposite vanilla gender swap activates custom synthesis when live slot
  // differs! Matching live slot vanilla keeps use_custom_model == false.
  {
    CustomizerConfig boy_cfg{};
    boy_cfg.body_model = BodyModelId::Boy;
    boy_cfg.hairstyle_id = "s1_boy_topknot";
    boy_cfg.skin_tone_id = "skin_vanilla_2";
    boy_cfg.eye_color_id = "eye_vanilla_0";
    const ResolvedAppearance girl_save_to_boy = reg.resolve(boy_cfg, 0u, 2u, 0u);
    EXPECT_TRUE(girl_save_to_boy.local.use_custom_model);
    EXPECT_EQ(girl_save_to_boy.wire.model_type, 1u);

    const ResolvedAppearance boy_save_to_boy = reg.resolve(boy_cfg, 1u, 2u, 0u);
    EXPECT_TRUE(!boy_save_to_boy.local.use_custom_model);

    CustomizerConfig girl_cfg{};
    girl_cfg.body_model = BodyModelId::Girl;
    girl_cfg.hairstyle_id = "s1_girl_classic";
    girl_cfg.skin_tone_id = "skin_vanilla_2";
    girl_cfg.eye_color_id = "eye_vanilla_0";
    const ResolvedAppearance boy_save_to_girl =
        reg.resolve(girl_cfg, 1u, 2u, 0u);
    EXPECT_TRUE(boy_save_to_girl.local.use_custom_model);
    EXPECT_EQ(boy_save_to_girl.wire.model_type, 0u);

    const ResolvedAppearance girl_save_to_girl =
        reg.resolve(girl_cfg, 0u, 2u, 0u);
    EXPECT_TRUE(!girl_save_to_girl.local.use_custom_model);
  }

  // Scenario B: Untrusted/malformed inbound packets are safely clamped to
  // vanilla ranges
  {
    const auto s1 = clamp_vanilla_wire(2, 99, -5);
    EXPECT_EQ(s1.model_type, 0u);
    EXPECT_EQ(s1.skin_color, 2u);
    EXPECT_EQ(s1.eye_color, 0u);

    const auto s2 = clamp_vanilla_wire(1, 6, 7);
    EXPECT_EQ(s2.model_type, 1u);
    EXPECT_EQ(s2.skin_color, 6u);
    EXPECT_EQ(s2.eye_color, 7u);
  }

  // Vanilla Appearance Mode disables custom local rendering cleanly while
  // preserving saved choices
  {
    CustomizerConfig cfg{};
    cfg.body_model = BodyModelId::Octoling;
    cfg.hairstyle_id = "s3_pompadour";
    cfg.vanilla_appearance_mode = true;
    const ResolvedAppearance r = reg.resolve(cfg, 1u, 4u, 3u);
    EXPECT_TRUE(!r.local.use_custom_model);
    EXPECT_EQ(r.wire.model_type, 1u);
    EXPECT_EQ(r.wire.skin_color, 4u);
    EXPECT_EQ(r.wire.eye_color, 3u);
  }

  // Missing/unknown hairstyle, skin, eye IDs trigger automatic safe fallback
  // without crash
  {
    CustomizerConfig cfg{};
    cfg.hairstyle_id = "nonexistent_pack_hair_999";
    cfg.skin_tone_id = "bad_skin";
    cfg.eye_color_id = "bad_eye";
    const ResolvedAppearance r = reg.resolve(cfg);
    EXPECT_TRUE(r.used_fallback);
    EXPECT_TRUE(!r.fallback_reason.empty());
    EXPECT_TRUE(r.wire.model_type <= 1u);
    EXPECT_TRUE(r.wire.skin_color <= 6u);
    EXPECT_TRUE(r.wire.eye_color <= 7u);
  }

  // Scenario C: Modular external cosmetic pack + validation + round-trip INI
  // persistence
  const fs::path good_pack =
      tmp_mods / "character_customizer_packs" / "demo_spiky_obj";
  const fs::path bad_pack =
      tmp_mods / "character_customizer_packs" / "corrupt_obj";
  fs::create_directories(good_pack, ec);
  fs::create_directories(bad_pack, ec);

  {
    std::ofstream obj(good_pack / "hair.obj");
    obj << "v 0.0 11.5 0.0\n"
        << "v 1.5 9.8 0.5\n"
        << "v -1.5 9.8 0.5\n"
        << "vt 0.25 0.25\n"
        << "vt 0.75 0.25\n"
        << "vt 0.5 0.75\n"
        << "f 1/1 2/2 3/3\n";
    std::ofstream js(good_pack / "pack.json");
    js << "{\n"
       << "  \"id\": \"ext_demo_spike\",\n"
       << "  \"name\": \"Demo Spike Pack\",\n"
       << "  \"obj\": \"hair.obj\",\n"
       << "  \"scale_y\": 1.05\n"
       << "}\n";
  }
  {
    std::ofstream obj(bad_pack / "broken.obj");
    obj << "v NaN 999999.0 0.0\nf 1 2 99\n";
    std::ofstream js(bad_pack / "pack.json");
    js << "{ \"id\": \"bad_pack_skip\", \"name\": \"Corrupt\", \"obj\": "
          "\"broken.obj\" }\n";
  }

  reg.reload_packs();
  EXPECT_EQ(reg.external_pack_count(), size_t(1));
  EXPECT_TRUE(!reg.pack_warnings().empty());

  // Delete external OBJ at runtime -> resolve_current triggers automatic
  // fallback!
  fs::remove(good_pack / "hair.obj", ec);
  {
    CustomizerConfig cfg{};
    cfg.hairstyle_id = "ext_demo_spike";
    const ResolvedAppearance r = reg.resolve(cfg);
    EXPECT_TRUE(r.used_fallback);
    EXPECT_TRUE(r.local.hairstyle.id != "ext_demo_spike");
  }

  // INI config save & reload round-trip
  {
    CustomizerConfig cfg{};
    cfg.custom_appearances_enabled = true;
    cfg.multiplayer_compatibility = true;
    cfg.vanilla_appearance_mode = false;
    cfg.body_model = BodyModelId::Octoling;
    cfg.hairstyle_id = "s3_long_wave";
    cfg.skin_tone_id = "skin_ext_warm_peach";
    cfg.eye_color_id = "eye_ext_gold";
    reg.set_config(cfg);
    EXPECT_TRUE(reg.save_config());

    reg.set_config(CustomizerConfig{});
    EXPECT_TRUE(reg.load_config());
    const CustomizerConfig after = reg.config();
    EXPECT_EQ(after.body_model, BodyModelId::Octoling);
    EXPECT_EQ(after.hairstyle_id, std::string("s3_long_wave"));
    EXPECT_EQ(after.skin_tone_id, std::string("skin_ext_warm_peach"));
    EXPECT_EQ(after.eye_color_id, std::string("eye_ext_gold"));
  }

  fs::remove_all(tmp_mods, ec);
}

void test_yaz0_sarc_roundtrip() {
  std::vector<uint8_t> payload = {'F', 'R', 'E', 'S', 0, 0, 0, 0, 1,  2,  3, 4,
                                  0,   0,   0,   0,   0, 0, 0, 0, 99, 88, 77};
  const std::vector<SarcEntryOwned> in_entries = {
      {"Output.bfres", payload},
      {"Output/SealShadow.bsms", {0x42u, 0x53u, 0x4Du, 0x53u}},
  };
  const std::vector<uint8_t> sarc = build_sarc_be(in_entries);
  const std::vector<uint8_t> yaz0 =
      compress_yaz0_fast(sarc.data(), sarc.size());
  std::vector<uint8_t> decomp;
  EXPECT_TRUE(decompress_yaz0(yaz0.data(), yaz0.size(), decomp));
  EXPECT_EQ(decomp.size(), sarc.size());
  std::vector<SarcEntryOwned> parsed;
  EXPECT_TRUE(parse_sarc_owned(decomp.data(), decomp.size(), parsed));
  EXPECT_EQ(parsed.size(), size_t(2));
}

void test_bfres_integration_if_player_pack_present() {
  const std::vector<fs::path> candidates = {
      fs::path("c:/Users/sbenj/Desktop/Splatt/Game/content/Pack/Player.pack"),
      fs::path("../Game/content/Pack/Player.pack"),
      fs::path("Game/content/Pack/Player.pack"),
  };
  fs::path pack_path;
  std::error_code ec;
  for (const auto &c : candidates) {
    if (fs::exists(c, ec) && fs::is_regular_file(c, ec)) {
      pack_path = c;
      break;
    }
  }
  if (pack_path.empty()) {
    std::printf("[INFO] Player.pack not mounted here; skipping live BFRES "
                "synthesis check.\n");
    return;
  }

  const fs::path tmp_out = fs::temp_directory_path() / "splatpc_cust_bfres_out";
  fs::remove_all(tmp_out, ec);
  fs::create_directories(tmp_out, ec);

  auto &reg = AppearanceRegistry::instance();
  reg.initialize(tmp_out.string());

  struct Combo {
    BodyModelId body;
    const char *hair;
    const char *skin;
    const char *eye;
    const char *fmdl;
    bool expect_rebind;
  };
  const std::vector<Combo> combos = {
      {BodyModelId::Boy, "s1_girl_classic", "skin_vanilla_2", "eye_vanilla_0",
       "Player01", true},
      {BodyModelId::Boy, "s1_girl_classic", "skin_vanilla_2", "eye_vanilla_0",
       "Player00",
       true}, // Cross-gender SaveData FMDL match (Girl save -> Boy body)
      {BodyModelId::Girl, "s1_boy_topknot", "skin_vanilla_3", "eye_ext_teal",
       "Player00", true},
      {BodyModelId::Girl, "s1_boy_topknot", "skin_vanilla_3", "eye_ext_teal",
       "Player01",
       true}, // Cross-gender SaveData FMDL match (Boy save -> Girl body)
      {BodyModelId::Girl, "s2_short_bob", "skin_ext_warm_peach", "eye_ext_gold",
       "Player00", false},
      {BodyModelId::Boy, "s3_pompadour", "skin_vanilla_1", "eye_vanilla_5",
       "Player01", false},
      {BodyModelId::Girl, "s1_octo_curls", "skin_vanilla_0", "eye_ext_crimson",
       "Player00", true},
      {BodyModelId::Octoling, "s2_spiky", "skin_vanilla_4", "eye_vanilla_6",
       "Player00", true},
      {BodyModelId::Octoling, "s2_short_bob", "skin_vanilla_3", "eye_vanilla_4",
       "Player00", true},
      {BodyModelId::Octoling, "s1_boy_topknot", "skin_vanilla_3", "eye_vanilla_1",
       "Player00", true},
      {BodyModelId::Octoling, "s1_octo_curls", "skin_vanilla_2", "eye_vanilla_0",
       "Player00", true},
      {BodyModelId::Boy, "s1_boy_topknot", "skin_vanilla_2", "eye_vanilla_0",
       "Player00", false}, // Opposite vanilla gender: Girl save -> Boy body
      {BodyModelId::Girl, "s1_girl_classic", "skin_vanilla_2", "eye_vanilla_0",
       "Player01", false}, // Opposite vanilla gender: Boy save -> Girl body
  };

  auto rd_u16_be = [](const uint8_t *p) -> uint16_t {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8u) |
                                 static_cast<uint16_t>(p[1]));
  };
  auto rd_u32_be = [](const uint8_t *p) -> uint32_t {
    return (static_cast<uint32_t>(p[0]) << 24u) |
           (static_cast<uint32_t>(p[1]) << 16u) |
           (static_cast<uint32_t>(p[2]) << 8u) | static_cast<uint32_t>(p[3]);
  };
  auto rd_s32_be = [&](const uint8_t *p) -> int32_t {
    return static_cast<int32_t>(rd_u32_be(p));
  };
  auto rd_f32_be = [&](const uint8_t *p) -> float {
    const uint32_t bits = rd_u32_be(p);
    float val = 0.0f;
    std::memcpy(&val, &bits, sizeof(val));
    return val;
  };
  auto rd_bfres_str = [&](const std::vector<uint8_t> &buf,
                          size_t rel_pos) -> std::string {
    const int32_t rel = rd_s32_be(buf.data() + rel_pos);
    if (rel == 0)
      return {};
    const size_t p = static_cast<size_t>(static_cast<int64_t>(rel_pos) + rel);
    size_t end = p;
    while (end < buf.size() && buf[end] != 0u)
      ++end;
    return std::string(reinterpret_cast<const char *>(buf.data() + p), end - p);
  };
  auto parse_dict = [&](const std::vector<uint8_t> &buf, size_t grp_off) {
    struct Entry {
      std::string name;
      size_t data_off = 0;
    };
    std::vector<Entry> out;
    const uint32_t cnt = rd_u32_be(buf.data() + grp_off + 4u);
    for (uint32_t i = 1u; i <= cnt; ++i) {
      const size_t eoff = grp_off + 8u + static_cast<size_t>(i) * 16u;
      const int32_t drel = rd_s32_be(buf.data() + eoff + 12u);
      Entry e{};
      e.name = rd_bfres_str(buf, eoff + 8u);
      e.data_off =
          drel != 0 ? static_cast<size_t>(static_cast<int64_t>(eoff + 12u) + drel)
                    : 0u;
      out.push_back(std::move(e));
    }
    return out;
  };

  for (const auto &c : combos) {
    CustomizerConfig cfg{};
    cfg.body_model = c.body;
    cfg.hairstyle_id = c.hair;
    cfg.skin_tone_id = c.skin;
    cfg.eye_color_id = c.eye;
    const uint32_t req_model = (std::string(c.fmdl) == "Player01") ? 1u : 0u;
    ResolvedAppearance resolved = reg.resolve(cfg, req_model, 2u, 0u);
    EXPECT_TRUE(resolved.local.use_custom_model);
    resolved.local.custom_archive_name += std::string("_") + c.fmdl;

    const BfresBuildReport rep = build_custom_player_szs(
        pack_path.string(), tmp_out.string(), resolved.local, c.fmdl);
    EXPECT_TRUE(rep.ok);
    if (resolved.local.hairstyle.source == HairSourceKind::GirlNative ||
        resolved.local.hairstyle.source == HairSourceKind::BoyNative) {
      EXPECT_TRUE(rep.hair_vtx_count > 1000u);
    } else {
      EXPECT_TRUE(rep.hair_vtx_count > 100u);
    }
    EXPECT_TRUE(rep.lod0_index_count > 300u);
    EXPECT_EQ(rep.cross_body_head_rebind, c.expect_rebind);
    if (resolved.local.eye_color.is_extended)
      EXPECT_TRUE(rep.eye_tinted);
    if (resolved.local.skin_tone.is_extended)
      EXPECT_TRUE(rep.skin_tinted);

    const bool expect_cross_mat =
        (c.body == BodyModelId::Girl &&
         resolved.local.hairstyle.source != HairSourceKind::GirlNative) ||
        (c.body == BodyModelId::Boy &&
         resolved.local.hairstyle.source != HairSourceKind::BoyNative) ||
        (c.body == BodyModelId::Octoling &&
         resolved.local.hairstyle.source != HairSourceKind::OctoNative);

    // Inspect serialized BFRES internals directly: ResDic ref_bit == 61,
    // cross-body M_TeamColor_Hair / HairSrc_Alb / hair__M_TeamColor binding,
    // and Octoling rigid _i0 == 45.
    std::ifstream in(rep.output_szs_path, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(in.is_open());
    const auto sz = static_cast<size_t>(in.tellg());
    std::vector<uint8_t> szs_bytes(sz);
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char *>(szs_bytes.data()),
            static_cast<std::streamsize>(sz));
    std::vector<uint8_t> sarc;
    EXPECT_TRUE(decompress_yaz0(szs_bytes.data(), szs_bytes.size(), sarc));
    std::vector<SarcEntryOwned> entries;
    EXPECT_TRUE(parse_sarc_owned(sarc.data(), sarc.size(), entries));
    const std::vector<uint8_t> *bfres = nullptr;
    for (const auto &e : entries) {
      if (e.name == "Output.bfres") {
        bfres = &e.data;
        break;
      }
    }
    EXPECT_TRUE(bfres != nullptr);
    if (bfres) {
      const size_t grp =
          0x20u + static_cast<size_t>(rd_s32_be(bfres->data() + 0x20u));
      const uint32_t ref_bit = rd_u32_be(bfres->data() + grp + 8u + 16u + 0u);
      EXPECT_EQ(ref_bit,
                61u); // "Player00" and "Player01" both hash to radix ref_bit 61

      const int32_t fmdl_rel =
          rd_s32_be(bfres->data() + grp + 8u + 16u + 12u);
      const size_t moff =
          grp + 8u + 16u + 12u + static_cast<size_t>(fmdl_rel);
      const size_t fvtx_arr =
          moff + 0x10u +
          static_cast<size_t>(rd_s32_be(bfres->data() + moff + 0x10u));
      const size_t fshp_grp =
          moff + 0x14u +
          static_cast<size_t>(rd_s32_be(bfres->data() + moff + 0x14u));
      const size_t fmat_grp =
          moff + 0x18u +
          static_cast<size_t>(rd_s32_be(bfres->data() + moff + 0x18u));
      const size_t ftex_grp =
          0x24u + static_cast<size_t>(rd_s32_be(bfres->data() + 0x24u));

      const auto fmats = parse_dict(*bfres, fmat_grp);
      const auto ftexs = parse_dict(*bfres, ftex_grp);
      const auto fshps = parse_dict(*bfres, fshp_grp);

      int32_t hair_mat_idx = -1;
      for (size_t mi = 0; mi < fmats.size(); ++mi) {
        if (fmats[mi].name == "M_TeamColor_Hair") {
          hair_mat_idx = static_cast<int32_t>(mi);
          break;
        }
      }
      bool has_hair_src_alb = false;
      for (const auto &te : ftexs) {
        if (te.name == "HairSrc_Alb") {
          has_hair_src_alb = true;
          break;
        }
      }
      size_t hair_fshp_off = 0u;
      for (const auto &se : fshps) {
        if (se.name == "hair__M_TeamColor") {
          hair_fshp_off = se.data_off;
          break;
        }
      }

      if (expect_cross_mat) {
        EXPECT_TRUE(hair_mat_idx >= 0);
        EXPECT_TRUE(has_hair_src_alb);
        EXPECT_TRUE(hair_fshp_off != 0u);
        if (hair_fshp_off != 0u && hair_mat_idx >= 0) {
          EXPECT_EQ(static_cast<int32_t>(
                        rd_u16_be(bfres->data() + hair_fshp_off + 0x0Eu)),
                    hair_mat_idx);
        }
      } else {
        EXPECT_EQ(hair_mat_idx, -1);
        EXPECT_TRUE(!has_hair_src_alb);
        if (c.body != BodyModelId::Octoling) {
          EXPECT_EQ(hair_fshp_off, size_t(0));
        }
      }

      if (c.body == BodyModelId::Octoling) {
        for (const auto &me : fmats) {
          if (me.name == "M_Eye" || me.name == "M_Eyelid") {
            const size_t mo = me.data_off;
            const size_t rs = static_cast<size_t>(
                static_cast<int64_t>(mo + 0x20u) +
                rd_s32_be(bfres->data() + mo + 0x20u));
            const size_t sa = static_cast<size_t>(
                static_cast<int64_t>(mo + 0x24u) +
                rd_s32_be(bfres->data() + mo + 0x24u));
            EXPECT_EQ(rd_bfres_str(*bfres, sa + 4u),
                      std::string("gambit_uber"));
            EXPECT_EQ(rd_u16_be(bfres->data() + sa + 0x0Eu), uint16_t(140));
            const uint32_t rs_flags = rd_u32_be(bfres->data() + rs);
            EXPECT_EQ(rs_flags, me.name == "M_Eye" ? 0x1u : 0x2u);
          }
        }

        // hair__M_TeamColor uses vtx_idx 4 in Rival00.szs
        const size_t voff = fvtx_arr + 4u * 0x20u;
        const uint32_t vtx_cnt = rd_u32_be(bfres->data() + voff + 8u);
        const size_t barr =
            voff + 0x18u +
            static_cast<size_t>(rd_s32_be(bfres->data() + voff + 0x18u));
        const size_t bo0 = barr + 0u * 0x18u; // _p0 position buffer
        const uint16_t st0 = rd_u16_be(bfres->data() + bo0 + 12u);
        const size_t doff0 =
            static_cast<size_t>(static_cast<int64_t>(bo0 + 20u) +
                                rd_s32_be(bfres->data() + bo0 + 20u));
        float min_y = 1e9f, max_y = -1e9f, back_band_min_z = 1e9f;
        for (uint32_t vi = 0; vi < vtx_cnt; ++vi) {
          const float x = rd_f32_be(bfres->data() + doff0 + vi * st0 + 0u);
          const float y = rd_f32_be(bfres->data() + doff0 + vi * st0 + 4u);
          const float z = rd_f32_be(bfres->data() + doff0 + vi * st0 + 8u);
          min_y = std::min(min_y, y);
          max_y = std::max(max_y, y);
          if (y >= 0.8f && y <= 2.0f && std::fabs(x) <= 1.2f && z < 0.0f) {
            back_band_min_z = std::min(back_band_min_z, z);
          }
        }
        // Head-local coordinate space for rigid-skinned Rival00 hair
        EXPECT_TRUE(min_y > -6.8f && max_y < 9.5f);
        if (std::string(c.hair) == "s1_octo_curls") {
          EXPECT_TRUE(min_y > -2.5f && max_y < 6.0f);
        }
        if (std::string(c.hair) == "s1_boy_topknot") {
          EXPECT_TRUE(max_y < 6.0f);
          EXPECT_TRUE(back_band_min_z > -2.82f);
        }

        const size_t bo = barr + 3u * 0x18u; // rigid _i0 buffer
        const size_t doff =
            static_cast<size_t>(static_cast<int64_t>(bo + 20u) +
                                rd_s32_be(bfres->data() + bo + 20u));
        EXPECT_EQ((*bfres)[doff], uint8_t(45));
      } else if (std::string(c.hair) == "s1_octo_curls") {
        // Cross-body hair on Girl/Boy lives in cloned hair__M_TeamColor
        EXPECT_TRUE(hair_fshp_off != 0u);
        if (hair_fshp_off != 0u) {
          const uint16_t hvtx_idx =
              rd_u16_be(bfres->data() + hair_fshp_off + 0x12u);
          const size_t voff = fvtx_arr + static_cast<size_t>(hvtx_idx) * 0x20u;
          const uint32_t vtx_cnt = rd_u32_be(bfres->data() + voff + 8u);
          const size_t barr =
              voff + 0x18u +
              static_cast<size_t>(rd_s32_be(bfres->data() + voff + 0x18u));
          const size_t bo0 = barr + 0u * 0x18u;
          const uint16_t st0 = rd_u16_be(bfres->data() + bo0 + 12u);
          const size_t doff0 =
              static_cast<size_t>(static_cast<int64_t>(bo0 + 20u) +
                                  rd_s32_be(bfres->data() + bo0 + 20u));
          float max_y = -1e9f;
          for (uint32_t vi = 0; vi < vtx_cnt; ++vi) {
            const float y = rd_f32_be(bfres->data() + doff0 + vi * st0 + 4u);
            max_y = std::max(max_y, y);
          }
          // Smooth-skinned Inkling model-space head height (~9.72 + hair top)
          EXPECT_TRUE(max_y > 12.0f && max_y < 18.0f);
        }
      }
    }
  }

  fs::remove_all(tmp_out, ec);
}

void test_ink_color_favorites() {
  const fs::path tmp_mods =
      fs::temp_directory_path() / "splatpc_cust_ink_color_test";
  std::error_code ec;
  fs::remove_all(tmp_mods, ec);
  fs::create_directories(tmp_mods, ec);

  auto &reg = AppearanceRegistry::instance();
  reg.initialize(tmp_mods.string());

  const auto all_inks = reg.ink_colors();
  EXPECT_EQ(all_inks.size(), size_t(16));
  for (const auto &c : all_inks) {
    EXPECT_TRUE(!c.id.empty() && !c.display_name.empty());
    EXPECT_TRUE(c.r >= 0.0f && c.r <= 1.0f);
    EXPECT_TRUE(c.g >= 0.0f && c.g <= 1.0f);
    EXPECT_TRUE(c.b >= 0.0f && c.b <= 1.0f);
  }

  // Empty favorites -> returns false
  CustomizerConfig cfg = reg.config();
  cfg.favorite_ink_colors.clear();
  InkColorDef picked{};
  EXPECT_TRUE(!reg.pick_spawn_ink_color(cfg, "", 0u, picked));

  // Single favorite -> always picks that favorite
  cfg.favorite_ink_colors = {"ink_turquoise"};
  EXPECT_TRUE(reg.pick_spawn_ink_color(cfg, "ink_turquoise", 7u, picked));
  EXPECT_EQ(picked.id, std::string("ink_turquoise"));

  // Multi-favorite -> strictly from selected pool, hits all 3, never repeats last_id
  cfg.favorite_ink_colors = {"ink_pink", "ink_mint", "ink_navy"};
  int hit_pink = 0, hit_mint = 0, hit_navy = 0;
  std::string last_id;
  for (uint32_t step = 0u; step < 100u; ++step) {
    const uint32_t rng = step * 1103515245u + 12345u;
    EXPECT_TRUE(reg.pick_spawn_ink_color(cfg, last_id, rng, picked));
    EXPECT_TRUE(picked.id == "ink_pink" || picked.id == "ink_mint" ||
                picked.id == "ink_navy");
    if (!last_id.empty()) {
      EXPECT_TRUE(picked.id != last_id);
    }
    if (picked.id == "ink_pink")
      ++hit_pink;
    else if (picked.id == "ink_mint")
      ++hit_mint;
    else if (picked.id == "ink_navy")
      ++hit_navy;
    last_id = picked.id;
  }
  EXPECT_TRUE(hit_pink > 0 && hit_mint > 0 && hit_navy > 0);

  // INI round-trip & reset
  reg.set_config(cfg);
  EXPECT_TRUE(reg.save_config());
  cfg.favorite_ink_colors.clear();
  reg.set_config(cfg);
  EXPECT_TRUE(reg.load_config());
  const CustomizerConfig reloaded = reg.config();
  EXPECT_EQ(reloaded.favorite_ink_colors.size(), size_t(3));
  EXPECT_EQ(reloaded.favorite_ink_colors[0], std::string("ink_pink"));
  EXPECT_EQ(reloaded.favorite_ink_colors[1], std::string("ink_mint"));
  EXPECT_EQ(reloaded.favorite_ink_colors[2], std::string("ink_navy"));

  reg.reset_to_vanilla();
  EXPECT_TRUE(reg.config().favorite_ink_colors.empty());

  // Contrast check: keep contrasting opponent color, replace near-identical color
  InkColorDef pink{};
  EXPECT_TRUE(reg.find_ink_color("ink_pink", pink));
  InkColorDef keep =
      reg.pick_contrasting_opponent_color(pink, 0.055f, 0.631f, 0.510f);
  EXPECT_TRUE(keep.id.empty());
  EXPECT_TRUE(std::fabs(keep.r - 0.055f) < 1e-4f);

  InkColorDef rep_opp =
      reg.pick_contrasting_opponent_color(pink, pink.r, pink.g, pink.b);
  EXPECT_TRUE(!rep_opp.id.empty());
  const float dr = rep_opp.r - pink.r;
  const float dg = rep_opp.g - pink.g;
  const float db = rep_opp.b - pink.b;
  EXPECT_TRUE(std::sqrt(dr * dr + dg * dg + db * db) >= 0.45f);

  fs::remove_all(tmp_mods, ec);
}

} // namespace

int main() {
  test_wire_protocol_and_scenarios();
  test_yaz0_sarc_roundtrip();
  test_bfres_integration_if_player_pack_present();
  test_ink_color_favorites();
  if (g_failures > 0) {
    std::fprintf(stderr, "FAILED %d assertions\n", g_failures);
    return 1;
  }
  std::printf("ALL CHARACTER CUSTOMIZER TESTS PASSED\n");
  return 0;
}
