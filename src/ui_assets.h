#pragma once

#include "splat_mod_api.h"

#include <cstdint>
#include <string>

namespace char_customizer {

class UiAssetsCache {
public:
  static UiAssetsCache &instance();

  void initialize(const std::string &game_dir);
  void ensure_uploaded(const SplatModUi *ui);
  void shutdown();

  uint64_t bg_wave_texture() const { return bg_wave_tex_; }
  uint64_t cursor_squid_texture() const { return squid_tex_; }

private:
  UiAssetsCache() = default;

  std::string layout_pack_path_;
  uint64_t last_epoch_ = 0u;
  uint64_t bg_wave_tex_ = 0u;
  uint64_t squid_tex_ = 0u;
  const SplatModUiEx *last_ex_ = nullptr;
  void *last_user_ = nullptr;
};

} // namespace char_customizer
