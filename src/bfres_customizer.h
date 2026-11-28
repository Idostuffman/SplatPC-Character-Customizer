#pragma once

#include "appearance_types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace char_customizer {

struct BfresBuildReport {
  bool ok = false;
  std::string archive_name;
  std::string output_szs_path;
  uint32_t combined_vtx_count = 0u;
  uint32_t body_vtx_count = 0u;
  uint32_t hair_vtx_count = 0u;
  uint32_t lod0_index_count = 0u;
  bool cross_body_head_rebind = false;
  bool eye_tinted = false;
  bool skin_tinted = false;
  std::string message;
};

// SARC / Yaz0 helpers usable by both the BFRES compositor and tests.
bool decompress_yaz0(const uint8_t *src, size_t len, std::vector<uint8_t> &out);
std::vector<uint8_t> compress_yaz0_fast(const uint8_t *src, size_t len);

struct SarcEntryOwned {
  std::string name;
  std::vector<uint8_t> data;
};

bool parse_sarc_owned(const uint8_t *buf, size_t len,
                      std::vector<SarcEntryOwned> &out);
std::vector<uint8_t> build_sarc_be(const std::vector<SarcEntryOwned> &entries);

// Builds a validated custom Player .szs archive inside overlay_model_dir from
// read-only Player.pack. target_fmdl_name should match the vanilla SaveData
// model ("Player00" or "Player01") so native loaders bind FMDL 0 cleanly
// without changing any original game files.
BfresBuildReport build_custom_player_szs(const std::string &player_pack_path,
                                         const std::string &overlay_model_dir,
                                         const LocalRenderPlan &plan,
                                         const std::string &target_fmdl_name);

// Deletes temporary/stale CustPl_*.szs files inside overlay_model_dir (keeps up
// to max_keep newest files).
void prune_custom_player_archives(const std::string &overlay_model_dir,
                                  size_t max_keep = 2u);

// Validates a compiled .szs (Yaz0 -> SARC -> Output.bfres integrity, vertex
// counts, indices, alignment).
bool validate_custom_player_szs_bytes(const std::vector<uint8_t> &szs_bytes,
                                      const std::string &expected_fmdl_name,
                                      uint32_t *out_vtx_cnt = nullptr,
                                      uint32_t *out_lod0_icnt = nullptr,
                                      std::string *out_err = nullptr);

} // namespace char_customizer
