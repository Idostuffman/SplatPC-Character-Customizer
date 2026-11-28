#include "appearance_registry.h"
#include "bfres_customizer.h"
#include "customizer_hooks.h"
#include "customizer_ui.h"
#include "splat_mod_api.h"
#include "ui_assets.h"

#include <cstdio>
#include <string>

namespace {

const SplatModHost *g_host = nullptr;
SplatModHookHandle g_menu_handle = 0;

constexpr SplatModInfo kModInfo = {
    SPLATMOD_ABI_VERSION,   "character_customizer",
    "Character Customizer", "1.0.0",
    "SplatPC Community",
};

} // namespace

SPLATMOD_EXPORT const SplatModInfo *splatmod_info(void) { return &kModInfo; }

SPLATMOD_EXPORT bool splatmod_init(const SplatModHost *host) {
  if (!host || host->abi_version != SPLATMOD_ABI_VERSION) {
    return false;
  }
  g_host = host;

  const std::string mods_dir =
      (host->mods_dir && host->mods_dir()) ? host->mods_dir() : "./mods";
  auto &reg = char_customizer::AppearanceRegistry::instance();
  reg.initialize(mods_dir);
  reg.load_config();
  char_customizer::UiAssetsCache::instance().initialize(reg.game_dir());
  const bool hooks_ok = char_customizer::install_hooks(host);
  const auto stats = char_customizer::get_runtime_stats();

  if (host->add_menu_page) {
    g_menu_handle = host->add_menu_page(
        "Character Customizer", &char_customizer::draw_customizer_menu_page,
        &char_customizer::customizer_menu_status, nullptr);
  }

  if (host->log) {
    host->log(SPLATMOD_LOG_INFO, "char_customizer",
              "Initialized Character Customizer v1.0.0 (rev=%u, "
              "hairstyles=%zu, packs=%zu, hooks_ok=%s, installed=%u/%u)\n",
              host->game_revision, reg.hairstyles().size(),
              reg.external_pack_count(), hooks_ok ? "yes" : "no",
              stats.hooks_installed, stats.hooks_total);
  }
  std::fprintf(stderr,
               "[char_customizer] Initialized Character Customizer v1.0.0 "
               "(hairstyles=%zu, packs=%zu, hooks=%u/%u)\n",
               reg.hairstyles().size(), reg.external_pack_count(),
               stats.hooks_installed, stats.hooks_total);
  std::fflush(stderr);
  return true;
}

SPLATMOD_EXPORT void splatmod_shutdown(void) {
  if (g_host && g_menu_handle && g_host->unhook_extension) {
    g_host->unhook_extension(g_menu_handle);
    g_menu_handle = 0;
  }
  char_customizer::UiAssetsCache::instance().shutdown();
  char_customizer::uninstall_hooks(g_host);
  g_host = nullptr;
}
