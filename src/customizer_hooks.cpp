#include "customizer_hooks.h"

#include "appearance_registry.h"
#include "bfres_customizer.h"
#include "customizer_addrs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace char_customizer {

namespace {

const SplatModHost *g_host = nullptr;
thread_local int t_local_player_scope = 0;

std::atomic<uint32_t> s_hooks_installed{0u};
std::atomic<uint32_t> s_hooks_total{0u};
std::atomic<uint32_t> s_native_hooks_installed{0u};
std::atomic<uint32_t> s_native_hooks_total{9u};
std::atomic<uint32_t> s_hook_hits{0u};
std::atomic<uint32_t> s_runtime_fallbacks{0u};
std::atomic<bool> s_native_bootstrap_done{false};
std::atomic<bool> s_in_plaza{false};
std::atomic<bool> s_pending_plaza_reload{false};
std::mutex s_bootstrap_mutex;
std::mutex s_build_mutex;

struct PreloadedCustomHolder {
  uint32_t arc_mgr_ptr = 0u;
  std::string fmdl;
  std::string arc_name;
  uint32_t holder_ptr = 0u;
};

std::mutex s_preload_mutex;
PreloadedCustomHolder s_preloaded[2]{};

std::mutex s_ink_mutex;
bool s_has_active_ink = false;
InkColorDef s_active_ink{};
int64_t s_last_ink_roll_ms = 0;
std::atomic<bool> s_force_ink_reroll{false};
std::mt19937 s_ink_rng(
    static_cast<uint32_t>(std::random_device{}()) ^
    static_cast<uint32_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()));

int64_t steady_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool roll_or_get_active_ink_color(bool allow_reroll, InkColorDef &out) {
  auto &reg = AppearanceRegistry::instance();
  const CustomizerConfig cfg = reg.config();
  if (!cfg.custom_appearances_enabled || cfg.vanilla_appearance_mode) {
    std::lock_guard<std::mutex> lock(s_ink_mutex);
    s_has_active_ink = false;
    s_force_ink_reroll.store(false, std::memory_order_relaxed);
    return false;
  }
  const auto favs = reg.resolve_favorite_ink_colors(cfg);
  if (favs.empty()) {
    std::lock_guard<std::mutex> lock(s_ink_mutex);
    s_has_active_ink = false;
    s_force_ink_reroll.store(false, std::memory_order_relaxed);
    return false;
  }

  std::lock_guard<std::mutex> lock(s_ink_mutex);
  bool still_favorite = false;
  if (s_has_active_ink) {
    for (const auto &f : favs) {
      if (f.id == s_active_ink.id) {
        still_favorite = true;
        break;
      }
    }
  }
  const int64_t now_ms = steady_now_ms();
  const bool forced =
      s_force_ink_reroll.exchange(false, std::memory_order_acq_rel);
  const bool need_roll =
      forced || !s_has_active_ink || !still_favorite ||
      (allow_reroll && (now_ms - s_last_ink_roll_ms >= 1500));

  if (need_roll) {
    InkColorDef chosen{};
    if (!reg.pick_spawn_ink_color(cfg, s_active_ink.id, s_ink_rng(), chosen)) {
      s_has_active_ink = false;
      return false;
    }
    s_active_ink = chosen;
    s_has_active_ink = true;
    s_last_ink_roll_ms = now_ms;
  }
  out = s_active_ink;
  return true;
}

bool mem_ok(const uint8_t *mem, uint32_t addr, uint64_t size) {
  if (!mem || addr == 0u)
    return false;
  if (g_host && g_host->memory_accessible)
    return g_host->memory_accessible(mem, addr, size);
  return true;
}

uint32_t rd32(const uint8_t *mem, uint32_t addr, uint32_t def = 0u) {
  return mem_ok(mem, addr, 4u) ? splatmod_read32(mem, addr) : def;
}

uint8_t rd8(const uint8_t *mem, uint32_t addr, uint8_t def = 0u) {
  return mem_ok(mem, addr, 1u) ? splatmod_read8(mem, addr) : def;
}

void wr32(uint8_t *mem, uint32_t addr, uint32_t v) {
  if (mem_ok(mem, addr, 4u))
    splatmod_write32(mem, addr, v);
}

void wr8(uint8_t *mem, uint32_t addr, uint8_t v) {
  if (mem_ok(mem, addr, 1u))
    splatmod_write8(mem, addr, v);
}

uint32_t determine_local_team_slot(uint8_t *mem, int32_t gfx_mode) {
  if (gfx_mode == 2 || s_in_plaza.load(std::memory_order_relaxed))
    return 0u;
  if (g_host && g_host->active_scene_type && mem &&
      g_host->active_scene_type(mem, nullptr) == 3) {
    return 0u;
  }
  if (!mem)
    return 0u;
  const uint32_t game_info = rd32(mem, addr::kGameInfoPtr, 0u);
  if (game_info != 0u && mem_ok(mem, game_info + 0x14u, 4u)) {
    const uint32_t ary = rd32(mem, game_info + 0x10u, 0u);
    if (ary != 0u && mem_ok(mem, ary, 4u)) {
      const uint32_t p0 = rd32(mem, ary, 0u);
      if (p0 != 0u && mem_ok(mem, p0, addr::kPlayerInfoSize)) {
        const uint8_t valid = rd8(mem, p0 + addr::kPlayerInfoValid, 0u);
        const uint32_t team = rd32(mem, p0 + addr::kPlayerInfoTeam, 0u);
        if (valid != 0u && team == 1u)
          return 1u;
      }
    }
  }
  return 0u;
}

bool patch_team_color_buffer(uint8_t *mem, uint32_t colors_ptr,
                             uint32_t local_team, bool allow_reroll) {
  if (!mem || !mem_ok(mem, colors_ptr, 48u))
    return false;
  InkColorDef chosen{};
  if (!roll_or_get_active_ink_color(allow_reroll, chosen))
    return false;

  const uint32_t team = (local_team == 1u) ? 1u : 0u;
  const uint32_t ally_off = colors_ptr + team * 16u;
  splatmod_write_f32(mem, ally_off + 0u, chosen.r);
  splatmod_write_f32(mem, ally_off + 4u, chosen.g);
  splatmod_write_f32(mem, ally_off + 8u, chosen.b);
  splatmod_write_f32(mem, ally_off + 12u, 1.0f);

  const uint32_t opp_off = colors_ptr + (1u - team) * 16u;
  const float cur_r = splatmod_read_f32(mem, opp_off + 0u);
  const float cur_g = splatmod_read_f32(mem, opp_off + 4u);
  const float cur_b = splatmod_read_f32(mem, opp_off + 8u);
  const InkColorDef opp =
      AppearanceRegistry::instance().pick_contrasting_opponent_color(
          chosen, cur_r, cur_g, cur_b);
  if (!opp.id.empty()) {
    splatmod_write_f32(mem, opp_off + 0u, opp.r);
    splatmod_write_f32(mem, opp_off + 4u, opp.g);
    splatmod_write_f32(mem, opp_off + 8u, opp.b);
    splatmod_write_f32(mem, opp_off + 12u, 1.0f);
  }
  return true;
}

std::string read_guest_cstr(const uint8_t *mem, uint32_t ptr,
                            size_t max_len = 64u) {
  if (!mem_ok(mem, ptr, 1u))
    return {};
  std::string out;
  out.reserve(max_len);
  for (size_t i = 0; i < max_len; ++i) {
    if (!mem_ok(mem, ptr + static_cast<uint32_t>(i), 1u))
      break;
    const uint8_t c = splatmod_read8(mem, ptr + static_cast<uint32_t>(i));
    if (c == 0u)
      break;
    out.push_back(static_cast<char>(c));
  }
  return out;
}

std::string read_guest_safestring(const uint8_t *mem, uint32_t ss_ptr) {
  if (!mem_ok(mem, ss_ptr, 8u))
    return {};
  const uint32_t buf_ptr = rd32(mem, ss_ptr + 0u, 0u);
  return read_guest_cstr(mem, buf_ptr, 96u);
}

// Builds temporary SafeString + string buffer directly on guest stack below SP.
uint32_t push_guest_safestring(CPUState *cpu, const std::string &text,
                               uint32_t &out_saved_sp) {
  out_saved_sp = cpu->r[1];
  const uint32_t scratch = (out_saved_sp - 0x80u) & ~0xFu;
  const uint32_t str_addr = scratch;
  const uint32_t ss_addr = scratch + 0x40u;
  const uint32_t new_sp = (scratch - 0x20u) & ~0xFu;
  if (!mem_ok(cpu->mem, new_sp, 0xA0u))
    return 0u;

  wr32(cpu->mem, new_sp, out_saved_sp);
  const size_t n = std::min<size_t>(text.size(), 60u);
  for (size_t i = 0; i < n; ++i) {
    wr8(cpu->mem, str_addr + static_cast<uint32_t>(i),
        static_cast<uint8_t>(text[i]));
  }
  wr8(cpu->mem, str_addr + static_cast<uint32_t>(n), 0u);

  wr32(cpu->mem, ss_addr + 0x00u, str_addr);
  wr32(cpu->mem, ss_addr + 0x04u, addr::kSafeStringVtable);
  cpu->r[1] = new_sp;
  return ss_addr;
}

bool call_guest_frame(CPUState *cpu, uint32_t fn_addr,
                      std::initializer_list<uint32_t> args,
                      uint32_t *out_r3 = nullptr) {
  if (!g_host || !g_host->call_guest || !cpu)
    return false;
  uint32_t argv[8]{};
  uint32_t n = 0u;
  for (uint32_t a : args) {
    if (n < 8u)
      argv[n++] = a;
  }
  uint32_t r3 = 0u;
  const bool ok = g_host->call_guest(cpu, fn_addr, argv, n, &r3);
  if (out_r3)
    *out_r3 = r3;
  return ok;
}

// Matches Gambit_fn_026BA198 (PlayerMgr::isLocalSlot) for primary local human
// player. During Game::Player::initSubObjs (0x02630E04), PlayerModel ctor and
// PlayerCustom setup run BEFORE Player::setCtrlType writes player + 0x58.
bool is_local_primary_player(const uint8_t *mem, uint32_t player) {
  if (!mem_ok(mem, player, 0xB0u))
    return false;
  const uint32_t main_mgr = rd32(mem, addr::kMainMgrPtr, 0u);
  if (!main_mgr ||
      !mem_ok(mem, main_mgr + addr::kMainMgrLocalPlayerNumOff, 4u))
    return false;
  const uint32_t local_num =
      rd32(mem, main_mgr + addr::kMainMgrLocalPlayerNumOff, 0u);
  if (local_num == 0u)
    return false;

  const uint32_t pmgr = rd32(mem, player + addr::kPlayerMgrBackPtrOff, 0u);
  const uint32_t slot_idx =
      rd32(mem, player + addr::kPlayerSlotIdxOff, 0xFFFFFFFFu);
  if (pmgr == 0u || slot_idx >= 8u ||
      !mem_ok(mem, pmgr + addr::kPlayerMgrLocalSlot0Off, 4u))
    return false;
  const uint32_t local_slot0 =
      rd32(mem, pmgr + addr::kPlayerMgrLocalSlot0Off, 0xFFFFFFFFu);
  return slot_idx == local_slot0;
}

uint32_t allocate_and_load_holder(CPUState *cpu, const std::string &arc_name,
                                  uint32_t heap_ptr) {
  if (!cpu || !cpu->mem || arc_name.empty())
    return 0u;
  uint32_t holder_ptr = 0u;
  if (!call_guest_frame(cpu, addr::kMemAlloc, {16u}, &holder_ptr) ||
      holder_ptr == 0u) {
    return 0u;
  }
  wr32(cpu->mem, holder_ptr + 0u, 0u);
  wr32(cpu->mem, holder_ptr + 4u, 0u);

  uint32_t saved_sp = 0u;
  const uint32_t ss_ptr = push_guest_safestring(cpu, arc_name, saved_sp);
  if (ss_ptr != 0u) {
    call_guest_frame(cpu, addr::kArchiveHolderLoad,
                     {holder_ptr, ss_ptr, 0u, heap_ptr});
    cpu->r[1] = saved_sp;
  }
  if (rd32(cpu->mem, holder_ptr + 0u, 0u) == 0u) {
    call_guest_frame(cpu, addr::kMemFree, {holder_ptr});
    return 0u;
  }
  return holder_ptr;
}

// Resolves the compiled Gambit_fn_XXXXXXXX body address from a RebrewU
// Gambit_fn_XXXXXXXX_entry stub stored in cpu->dispatch_table[guest_addr >> 2].
void *resolve_guest_body_from_dispatch(const CPUState *cpu,
                                       uint32_t guest_addr) {
  if (!cpu || !cpu->dispatch_table || (guest_addr & 3u) != 0u)
    return nullptr;
  const uint32_t idx = guest_addr >> 2u;
  if (idx >= cpu->dispatch_size || !cpu->dispatch_table[idx])
    return nullptr;

  const uint8_t *entry =
      reinterpret_cast<const uint8_t *>(cpu->dispatch_table[idx]);

  // Pattern A: 48 8B 91 <disp32>  E9 <rel32>  (mov rdx, [rcx+disp32]; jmp body)
  if (entry[0] == 0x48 && entry[1] == 0x8B && entry[2] == 0x91 &&
      entry[7] == 0xE9) {
    int32_t rel = 0;
    std::memcpy(&rel, entry + 8, sizeof(rel));
    return const_cast<uint8_t *>(entry + 12 + rel);
  }
  // Pattern B: 48 8B 51 <disp8>   E9 <rel32>  (mov rdx, [rcx+disp8]; jmp body)
  if (entry[0] == 0x48 && entry[1] == 0x8B && entry[2] == 0x51 &&
      entry[4] == 0xE9) {
    int32_t rel = 0;
    std::memcpy(&rel, entry + 5, sizeof(rel));
    return const_cast<uint8_t *>(entry + 9 + rel);
  }
  // Pattern C: E9 <rel32> (direct jmp body)
  if (entry[0] == 0xE9) {
    int32_t rel = 0;
    std::memcpy(&rel, entry + 1, sizeof(rel));
    return const_cast<uint8_t *>(entry + 5 + rel);
  }
  return nullptr;
}

// ---- Shared Detour Logic (reused by both Native body hooks & Guest fallbacks) ----

template <typename OrigFn>
void handle_archive_holder_load(CPUState *cpu, OrigFn call_orig) {
  if (t_local_player_scope <= 0 || !cpu || !cpu->mem) {
    call_orig();
    return;
  }

  auto &reg = AppearanceRegistry::instance();
  const CustomizerConfig cfg = reg.config();
  if (!cfg.custom_appearances_enabled || cfg.vanilla_appearance_mode) {
    call_orig();
    return;
  }

  const uint32_t dst_holder = cpu->r[3];
  const uint32_t ss_ptr = cpu->r[4];
  const uint32_t arg_flag = cpu->r[5];
  const uint32_t arg_heap = cpu->r[6];
  const std::string req_name = read_guest_safestring(cpu->mem, ss_ptr);

  std::string redirect;
  if (req_name == "Player00" || req_name == "Player01") {
    const uint32_t req_model = (req_name == "Player01") ? 1u : 0u;
    const ResolvedAppearance resolved = reg.resolve(cfg, req_model, 2u, 0u);
    if (resolved.local.use_custom_model) {
      redirect = ensure_custom_archive_for_fmdl(req_name, nullptr);
    }
  } else if (req_name == "Player00_anim" || req_name == "Player01_anim") {
    const uint32_t req_model = (req_name == "Player01_anim") ? 1u : 0u;
    const ResolvedAppearance resolved = reg.resolve(cfg, req_model, 2u, 0u);
    if (resolved.local.use_custom_model) {
      const std::string want_anim =
          body_anim_archive_name(resolved.local.body_model);
      if (want_anim != req_name) {
        redirect = want_anim;
      }
    }
  }

  if (redirect.empty()) {
    call_orig();
    return;
  }

  uint32_t saved_sp = 0u;
  const uint32_t tmp_ss = push_guest_safestring(cpu, redirect, saved_sp);
  if (tmp_ss == 0u) {
    call_orig();
    return;
  }

  cpu->r[3] = dst_holder;
  cpu->r[4] = tmp_ss;
  cpu->r[5] = arg_flag;
  cpu->r[6] = arg_heap;
  call_orig();
  cpu->r[1] = saved_sp;

  const uint32_t loaded_arc = rd32(cpu->mem, dst_holder + 0u, 0u);
  if (loaded_arc == 0u) {
    s_runtime_fallbacks.fetch_add(1u, std::memory_order_relaxed);
    if (g_host && g_host->log) {
      g_host->log(SPLATMOD_LOG_WARN, "char_customizer",
                  "ArchiveHolderLoad failed for %s; falling back to %s\n",
                  redirect.c_str(), req_name.c_str());
    }
    cpu->r[3] = dst_holder;
    cpu->r[4] = ss_ptr;
    cpu->r[5] = arg_flag;
    cpu->r[6] = arg_heap;
    call_orig();
  } else {
    s_hook_hits.fetch_add(1u, std::memory_order_relaxed);
  }
}

template <typename OrigFn>
void handle_player_mgr_load_archives(CPUState *cpu, OrigFn call_orig) {
  const uint32_t arc_mgr = cpu ? cpu->r[3] : 0u;
  const uint32_t arg_heap = cpu ? cpu->r[4] : 0u;
  call_orig();

  if (!cpu || !cpu->mem || arc_mgr == 0u)
    return;

  {
    std::lock_guard<std::mutex> lock(s_preload_mutex);
    s_preloaded[0] = PreloadedCustomHolder{};
    s_preloaded[1] = PreloadedCustomHolder{};
  }

  const auto &reg = AppearanceRegistry::instance();
  const CustomizerConfig cfg = reg.config();
  if (!cfg.custom_appearances_enabled || cfg.vanilla_appearance_mode)
    return;

  uint32_t live_m = 0u, live_s = 2u, live_e = 0u;
  query_live_vanilla_appearance(cpu->mem, &live_m, &live_s, &live_e);

  for (uint32_t slot = 0u; slot <= 1u; ++slot) {
    if (slot != live_m)
      continue;
    const std::string req_fmdl = (slot == 1u) ? "Player01" : "Player00";
    const ResolvedAppearance resolved = reg.resolve(cfg, slot, live_s, live_e);
    if (!resolved.local.use_custom_model)
      continue;

    const std::string arc_name =
        ensure_custom_archive_for_fmdl(req_fmdl, nullptr);
    if (arc_name.empty())
      continue;

    const uint32_t holder_ptr =
        allocate_and_load_holder(cpu, arc_name, arg_heap);
    if (holder_ptr != 0u) {
      std::lock_guard<std::mutex> lock(s_preload_mutex);
      s_preloaded[slot] = {arc_mgr, req_fmdl, arc_name, holder_ptr};
      if (g_host && g_host->log) {
        g_host->log(SPLATMOD_LOG_INFO, "char_customizer",
                    "Preloaded custom archive %s into PlayerMgr (arc_mgr=0x%08X)\n",
                    arc_name.c_str(), arc_mgr);
      }
    } else {
      s_runtime_fallbacks.fetch_add(1u, std::memory_order_relaxed);
      if (g_host && g_host->log) {
        g_host->log(SPLATMOD_LOG_WARN, "char_customizer",
                    "Failed preloading custom archive %s in PlayerMgr (heap=0x%08X)\n",
                    arc_name.c_str(), arg_heap);
      }
    }
  }
}

template <typename OrigFn>
void handle_player_model_ctor(CPUState *cpu, OrigFn call_orig) {
  if (!cpu || !cpu->mem) {
    call_orig();
    return;
  }

  const uint32_t this_pm = cpu->r[3];
  const uint32_t player = cpu->r[4];
  if (!is_local_primary_player(cpu->mem, player)) {
    call_orig();
    return;
  }

  auto &reg = AppearanceRegistry::instance();
  const CustomizerConfig cfg = reg.config();
  if (!cfg.custom_appearances_enabled || cfg.vanilla_appearance_mode) {
    call_orig();
    return;
  }

  const uint32_t pinfo = rd32(cpu->mem, player + addr::kPlayerInfoPtrOff, 0u);
  const uint32_t pmgr = rd32(cpu->mem, player + addr::kPlayerMgrBackPtrOff, 0u);
  if (!pinfo || !pmgr || !mem_ok(cpu->mem, pmgr, 0x2C0u)) {
    call_orig();
    return;
  }

  const uint32_t model_type =
      (rd32(cpu->mem, player + addr::kPlayerModelTypeOff, 0u) == 1u) ? 1u : 0u;
  const uint32_t skin_col =
      rd32(cpu->mem, pinfo + addr::kPlayerInfoSkinCol, 2u);
  const uint32_t eye_col = rd32(cpu->mem, pinfo + addr::kPlayerInfoEyeCol, 0u);
  const ResolvedAppearance resolved =
      reg.resolve(cfg, model_type, skin_col, eye_col);
  if (!resolved.local.use_custom_model) {
    call_orig();
    return;
  }

  const std::string req_fmdl = (model_type == 1u) ? "Player01" : "Player00";
  const std::string custom_arc =
      ensure_custom_archive_for_fmdl(req_fmdl, nullptr);
  if (custom_arc.empty()) {
    call_orig();
    return;
  }

  const uint32_t model_cnt =
      rd32(cpu->mem, pmgr + addr::kPlayerMgrModelArcAryOff - 8u, 0u);
  const uint32_t model_ary =
      rd32(cpu->mem, pmgr + addr::kPlayerMgrModelArcAryOff, 0u);
  if (model_type >= model_cnt ||
      !mem_ok(cpu->mem, model_ary + model_type * 4u, 4u)) {
    call_orig();
    return;
  }

  const uint32_t arc_mgr = pmgr + 0x238u;
  uint32_t holder_ptr = 0u;
  {
    std::lock_guard<std::mutex> lock(s_preload_mutex);
    const PreloadedCustomHolder &cached = s_preloaded[model_type];
    if (cached.arc_mgr_ptr == arc_mgr && cached.fmdl == req_fmdl &&
        cached.arc_name == custom_arc && cached.holder_ptr != 0u &&
        mem_ok(cpu->mem, cached.holder_ptr, 8u) &&
        rd32(cpu->mem, cached.holder_ptr + 0u, 0u) != 0u) {
      holder_ptr = cached.holder_ptr;
    }
  }

  if (holder_ptr == 0u) {
    holder_ptr = allocate_and_load_holder(cpu, custom_arc, 0u);
    if (holder_ptr != 0u) {
      std::lock_guard<std::mutex> lock(s_preload_mutex);
      s_preloaded[model_type] = {arc_mgr, req_fmdl, custom_arc, holder_ptr};
    }
  }

  if (holder_ptr == 0u) {
    s_runtime_fallbacks.fetch_add(1u, std::memory_order_relaxed);
    if (g_host && g_host->log) {
      g_host->log(SPLATMOD_LOG_WARN, "char_customizer",
                  "PlayerModelCtor failed loading %s; using vanilla\n",
                  custom_arc.c_str());
    }
    cpu->r[3] = this_pm;
    cpu->r[4] = player;
    call_orig();
    return;
  }

  auto try_swap_slot = [&](uint32_t ary_off, uint32_t want_idx,
                           uint32_t &out_orig) -> bool {
    if (want_idx == model_type)
      return false;
    const uint32_t cnt = rd32(cpu->mem, pmgr + ary_off - 8u, 0u);
    const uint32_t ary = rd32(cpu->mem, pmgr + ary_off, 0u);
    if (want_idx >= cnt || model_type >= cnt || ary == 0u ||
        !mem_ok(cpu->mem, ary + model_type * 4u, 4u) ||
        !mem_ok(cpu->mem, ary + want_idx * 4u, 4u)) {
      return false;
    }
    const uint32_t target = rd32(cpu->mem, ary + want_idx * 4u, 0u);
    if (target == 0u || rd32(cpu->mem, target + 0u, 0u) == 0u)
      return false;
    out_orig = rd32(cpu->mem, ary + model_type * 4u, 0u);
    wr32(cpu->mem, ary + model_type * 4u, target);
    return true;
  };

  auto restore_slot = [&](uint32_t ary_off, uint32_t orig) {
    const uint32_t ary = rd32(cpu->mem, pmgr + ary_off, 0u);
    if (ary != 0u && mem_ok(cpu->mem, ary + model_type * 4u, 4u)) {
      wr32(cpu->mem, ary + model_type * 4u, orig);
    }
  };

  const uint32_t want_anim_idx =
      (resolved.local.body_model == BodyModelId::Boy) ? 1u : 0u;
  const uint32_t want_form_idx =
      static_cast<uint32_t>(resolved.local.body_model);

  uint32_t orig_anim = 0u, orig_half = 0u, orig_squid = 0u;
  const bool swapped_anim =
      try_swap_slot(addr::kPlayerMgrAnimArcAryOff, want_anim_idx, orig_anim);
  const bool swapped_half =
      try_swap_slot(addr::kPlayerMgrHalfArcAryOff, want_form_idx, orig_half);
  const bool swapped_squid =
      try_swap_slot(addr::kPlayerMgrSquidArcAryOff, want_form_idx, orig_squid);

  const uint32_t orig_model_holder =
      rd32(cpu->mem, model_ary + model_type * 4u, 0u);
  wr32(cpu->mem, model_ary + model_type * 4u, holder_ptr);

  ++t_local_player_scope;
  cpu->r[3] = this_pm;
  cpu->r[4] = player;
  call_orig();
  --t_local_player_scope;

  // Immediately restore PlayerMgr tables so every other player & bot uses
  // unmodified vanilla assets.
  wr32(cpu->mem, model_ary + model_type * 4u, orig_model_holder);
  if (swapped_anim)
    restore_slot(addr::kPlayerMgrAnimArcAryOff, orig_anim);
  if (swapped_half)
    restore_slot(addr::kPlayerMgrHalfArcAryOff, orig_half);
  if (swapped_squid)
    restore_slot(addr::kPlayerMgrSquidArcAryOff, orig_squid);

  s_hook_hits.fetch_add(1u, std::memory_order_relaxed);
  if (g_host && g_host->log) {
    g_host->log(SPLATMOD_LOG_INFO, "char_customizer",
                "Applied %s to local player (slot=%u, body=%s, hair=%s)\n",
                custom_arc.c_str(),
                rd32(cpu->mem, player + addr::kPlayerSlotIdxOff, 0u),
                body_model_name(resolved.local.body_model),
                resolved.local.hairstyle.id.c_str());
  }
}

template <typename OrigFn>
void handle_player_custom_setup(CPUState *cpu, OrigFn call_orig) {
  if (!cpu || !cpu->mem) {
    call_orig();
    return;
  }
  const uint32_t player = cpu->r[4];
  if (is_local_primary_player(cpu->mem, player)) {
    ++t_local_player_scope;
    call_orig();
    --t_local_player_scope;
  } else {
    call_orig();
  }
}

template <typename OrigFn>
void handle_set_skin_color(CPUState *cpu, OrigFn call_orig) {
  if (t_local_player_scope > 0 && cpu) {
    const auto &reg = AppearanceRegistry::instance();
    const CustomizerConfig cfg = reg.config();
    if (cfg.custom_appearances_enabled && !cfg.vanilla_appearance_mode) {
      const auto res = reg.resolve(cfg);
      cpu->r[5] = res.wire.skin_color;
    }
  }
  call_orig();
}

template <typename OrigFn>
void handle_set_eye_color(CPUState *cpu, OrigFn call_orig) {
  if (t_local_player_scope > 0 && cpu) {
    const auto &reg = AppearanceRegistry::instance();
    const CustomizerConfig cfg = reg.config();
    if (cfg.custom_appearances_enabled && !cfg.vanilla_appearance_mode) {
      const auto res = reg.resolve(cfg);
      cpu->r[5] = res.wire.eye_color;
    }
  }
  call_orig();
}

template <typename OrigFn>
void handle_plz_vs_door_calc(CPUState *cpu, OrigFn call_orig) {
  s_in_plaza.store(true, std::memory_order_relaxed);
  call_orig();

  if (!s_pending_plaza_reload.exchange(false, std::memory_order_acq_rel))
    return;
  if (!cpu || !cpu->mem)
    return;

  uint32_t ret = 0u;
  const uint32_t old_r1 = cpu->r[1];
  const uint32_t scratch = (old_r1 - 0x20u) & ~0xFu;
  const uint32_t frame_sp = (old_r1 - 0x40u) & ~0xFu;
  if (mem_ok(cpu->mem, frame_sp, 0x40u)) {
    wr32(cpu->mem, frame_sp, old_r1);
    wr32(cpu->mem, scratch + 0x00u, addr::kSceneStrPlaza);
    wr32(cpu->mem, scratch + 0x04u, addr::kSafeStringVtable);
    cpu->r[1] = frame_sp;
    call_guest_frame(cpu, addr::kSceneMgrChangeSceneStr, {scratch}, &ret);
    if (ret == 0u) {
      call_guest_frame(cpu, addr::kSceneMgrChangeSceneDirect, {scratch});
      ret = 2u;
    }
    cpu->r[1] = old_r1;
  }
  if (g_host && g_host->log) {
    g_host->log(SPLATMOD_LOG_INFO, "char_customizer",
                "Triggered Inkopolis Plaza reload for updated appearance (ret=%u)\n",
                ret);
  }
}

template <typename OrigFn>
void handle_player_info_pack_enl(CPUState *cpu, OrigFn call_orig) {
  const uint32_t wire_dst = cpu ? cpu->r[3] : 0u;
  call_orig();
  if (!cpu || !cpu->mem || !mem_ok(cpu->mem, wire_dst, 0x10u))
    return;

  const int32_t m =
      static_cast<int8_t>(rd8(cpu->mem, wire_dst + addr::kWireModelTypeS8, 0u));
  const int32_t s =
      static_cast<int8_t>(rd8(cpu->mem, wire_dst + addr::kWireSkinColS8, 0u));
  const int32_t e =
      static_cast<int8_t>(rd8(cpu->mem, wire_dst + addr::kWireEyeColS8, 0u));
  const auto safe = clamp_vanilla_wire(m, s, e);
  wr8(cpu->mem, wire_dst + addr::kWireModelTypeS8,
      static_cast<uint8_t>(safe.model_type));
  wr8(cpu->mem, wire_dst + addr::kWireSkinColS8,
      static_cast<uint8_t>(safe.skin_color));
  wr8(cpu->mem, wire_dst + addr::kWireEyeColS8,
      static_cast<uint8_t>(safe.eye_color));
}

template <typename OrigFn>
void handle_player_info_unpack_enl(CPUState *cpu, OrigFn call_orig) {
  const uint32_t dst_pinfo = cpu ? cpu->r[3] : 0u;
  call_orig();
  if (!cpu || !cpu->mem || !mem_ok(cpu->mem, dst_pinfo, addr::kPlayerInfoSize))
    return;

  const int32_t m = static_cast<int32_t>(
      rd32(cpu->mem, dst_pinfo + addr::kPlayerInfoModelType, 0u));
  const int32_t s = static_cast<int32_t>(
      rd32(cpu->mem, dst_pinfo + addr::kPlayerInfoSkinCol, 0u));
  const int32_t e = static_cast<int32_t>(
      rd32(cpu->mem, dst_pinfo + addr::kPlayerInfoEyeCol, 0u));
  const auto safe = clamp_vanilla_wire(m, s, e);
  if (static_cast<uint32_t>(m) != safe.model_type)
    wr32(cpu->mem, dst_pinfo + addr::kPlayerInfoModelType, safe.model_type);
  if (static_cast<uint32_t>(s) != safe.skin_color)
    wr32(cpu->mem, dst_pinfo + addr::kPlayerInfoSkinCol, safe.skin_color);
  if (static_cast<uint32_t>(e) != safe.eye_color)
    wr32(cpu->mem, dst_pinfo + addr::kPlayerInfoEyeCol, safe.eye_color);
}

template <typename OrigFn>
void handle_gfx_get_team_color(CPUState *cpu, OrigFn call_orig) {
  const uint32_t out_ptr = cpu ? cpu->r[3] : 0u;
  int32_t gfx_mode = -1;
  if (cpu && cpu->mem && mem_ok(cpu->mem, cpu->r[5], 4u)) {
    gfx_mode = static_cast<int32_t>(rd32(cpu->mem, cpu->r[5], 0u));
  }
  call_orig();
  if (!cpu || !cpu->mem || out_ptr == 0u)
    return;
  patch_team_color_buffer(cpu->mem, out_ptr,
                          determine_local_team_slot(cpu->mem, gfx_mode), true);
}

template <typename OrigFn>
void handle_gfx_set_team_color(CPUState *cpu, OrigFn call_orig) {
  const uint32_t in_ptr = cpu ? cpu->r[4] : 0u;
  if (cpu && cpu->mem && in_ptr != 0u) {
    if (patch_team_color_buffer(cpu->mem, in_ptr,
                                determine_local_team_slot(cpu->mem, -1),
                                true)) {
      const uint32_t game_info = rd32(cpu->mem, addr::kGameInfoPtr, 0u);
      const uint32_t gi_colors =
          game_info != 0u ? (game_info + addr::kGameInfoTeamColorsOff) : 0u;
      if (gi_colors != 0u && gi_colors != in_ptr &&
          mem_ok(cpu->mem, gi_colors, 48u)) {
        std::memcpy(cpu->mem + gi_colors, cpu->mem + in_ptr, 48u);
      }
    }
  }
  call_orig();
}

// ---- Vtable / Indirect Bootstrap & Scene Guest Hooks ----
SPLAT_DEFINE_GUEST_HOOK(HookBootstrapPlayerMgrLoad)
SPLAT_DEFINE_GUEST_HOOK(HookBootstrapPlayerInitSubobjs)
SPLAT_DEFINE_GUEST_HOOK(HookBootstrapPlayerCalc)
SPLAT_DEFINE_GUEST_HOOK(HookPlazaPlayerSetup)
SPLAT_DEFINE_GUEST_HOOK(HookLobbyPlayerSetup)
SPLAT_DEFINE_GUEST_HOOK(HookEquipPlayerSetup)
SPLAT_DEFINE_GUEST_HOOK(HookShopPlayerSetup)
SPLAT_DEFINE_GUEST_HOOK(HookMakePlayerSetup)
SPLAT_DEFINE_GUEST_HOOK(HookResultPlayerSetup)
SPLAT_DEFINE_GUEST_HOOK(HookDuelPlayerSetup)

// ---- Direct-Called Compiled Function Native Body Hooks ----
SPLAT_DEFINE_NATIVE_HOOK(NativeArchiveHolderLoad, void,
                         (CPUState * cpu, uint8_t *mem))
SPLAT_DEFINE_NATIVE_HOOK(NativePlayerMgrLoadArchives, void,
                         (CPUState * cpu, uint8_t *mem))
SPLAT_DEFINE_NATIVE_HOOK(NativePlayerModelCtor, void,
                         (CPUState * cpu, uint8_t *mem))
SPLAT_DEFINE_NATIVE_HOOK(NativePlayerCustomSetup, void,
                         (CPUState * cpu, uint8_t *mem))
SPLAT_DEFINE_NATIVE_HOOK(NativeSetSkinColor, void,
                         (CPUState * cpu, uint8_t *mem))
SPLAT_DEFINE_NATIVE_HOOK(NativeSetEyeColor, void,
                         (CPUState * cpu, uint8_t *mem))
SPLAT_DEFINE_NATIVE_HOOK(NativePlzVsDoorCalc, void,
                         (CPUState * cpu, uint8_t *mem))
SPLAT_DEFINE_NATIVE_HOOK(NativePlayerInfoPackEnl, void,
                         (CPUState * cpu, uint8_t *mem))
SPLAT_DEFINE_NATIVE_HOOK(NativePlayerInfoUnpackEnl, void,
                         (CPUState * cpu, uint8_t *mem))
SPLAT_DEFINE_NATIVE_HOOK(NativeGfxGetTeamColor, void,
                         (CPUState * cpu, uint8_t *mem))
SPLAT_DEFINE_NATIVE_HOOK(NativeGfxSetTeamColor, void,
                         (CPUState * cpu, uint8_t *mem))

// ---- Fallback Guest Hooks (only used if _entry body resolution fails) ----
SPLAT_DEFINE_GUEST_HOOK(HookArchiveHolderLoad)
SPLAT_DEFINE_GUEST_HOOK(HookPlayerMgrLoadArchives)
SPLAT_DEFINE_GUEST_HOOK(HookPlayerModelCtor)
SPLAT_DEFINE_GUEST_HOOK(HookPlayerCustomSetup)
SPLAT_DEFINE_GUEST_HOOK(HookSetSkinColor)
SPLAT_DEFINE_GUEST_HOOK(HookSetEyeColor)
SPLAT_DEFINE_GUEST_HOOK(HookPlzVsDoorCalc)
SPLAT_DEFINE_GUEST_HOOK(HookPlayerInfoPackEnl)
SPLAT_DEFINE_GUEST_HOOK(HookPlayerInfoUnpackEnl)
SPLAT_DEFINE_GUEST_HOOK(HookGfxGetTeamColor)
SPLAT_DEFINE_GUEST_HOOK(HookGfxSetTeamColor)

void ensure_native_body_hooks_installed(CPUState *cpu) {
  if (s_native_bootstrap_done.load(std::memory_order_acquire))
    return;
  if (!cpu || !cpu->dispatch_table || !g_host)
    return;

  std::lock_guard<std::mutex> lock(s_bootstrap_mutex);
  if (s_native_bootstrap_done.load(std::memory_order_relaxed))
    return;

  uint32_t native_ok = 0u;
  constexpr uint32_t native_total = 11u;

  auto install_body_or_fallback = [&](const char *name, uint32_t guest_addr,
                                      HookPriority prio, auto install_native,
                                      auto install_guest) {
    void *body = resolve_guest_body_from_dispatch(cpu, guest_addr);
    if (body && install_native(body, prio)) {
      ++native_ok;
      return;
    }
    const bool g_ok = install_guest(prio);
    if (g_host->log) {
      g_host->log(SPLATMOD_LOG_WARN, "char_customizer",
                  "Native hook fallback for %s (0x%08X, body=%p): guest=%s\n",
                  name, guest_addr, body, g_ok ? "ok" : "fail");
    }
  };

  install_body_or_fallback(
      "ArchiveHolderLoad", addr::kArchiveHolderLoad, HookPriority::HIGH,
      [&](void *b, HookPriority p) {
        return NativeArchiveHolderLoad::InstallAt(g_host, b, p);
      },
      [&](HookPriority p) {
        return HookArchiveHolderLoad::InstallAt(g_host,
                                                addr::kArchiveHolderLoad, p);
      });
  install_body_or_fallback(
      "PlayerMgrLoadArchives", addr::kPlayerMgrLoadArchives, HookPriority::HIGH,
      [&](void *b, HookPriority p) {
        return NativePlayerMgrLoadArchives::InstallAt(g_host, b, p);
      },
      [&](HookPriority p) {
        return HookPlayerMgrLoadArchives::InstallAt(
            g_host, addr::kPlayerMgrLoadArchives, p);
      });
  install_body_or_fallback(
      "PlayerModelCtor", addr::kPlayerModelCtor, HookPriority::HIGH,
      [&](void *b, HookPriority p) {
        return NativePlayerModelCtor::InstallAt(g_host, b, p);
      },
      [&](HookPriority p) {
        return HookPlayerModelCtor::InstallAt(g_host, addr::kPlayerModelCtor, p);
      });
  install_body_or_fallback(
      "PlayerCustomSetup", addr::kPlayerCustomSetup, HookPriority::HIGH,
      [&](void *b, HookPriority p) {
        return NativePlayerCustomSetup::InstallAt(g_host, b, p);
      },
      [&](HookPriority p) {
        return HookPlayerCustomSetup::InstallAt(g_host,
                                                addr::kPlayerCustomSetup, p);
      });
  install_body_or_fallback(
      "SetSkinColor", addr::kSetSkinColor, HookPriority::HIGH,
      [&](void *b, HookPriority p) {
        return NativeSetSkinColor::InstallAt(g_host, b, p);
      },
      [&](HookPriority p) {
        return HookSetSkinColor::InstallAt(g_host, addr::kSetSkinColor, p);
      });
  install_body_or_fallback(
      "SetEyeColor", addr::kSetEyeColor, HookPriority::HIGH,
      [&](void *b, HookPriority p) {
        return NativeSetEyeColor::InstallAt(g_host, b, p);
      },
      [&](HookPriority p) {
        return HookSetEyeColor::InstallAt(g_host, addr::kSetEyeColor, p);
      });
  install_body_or_fallback(
      "PlzVsDoorCalc", addr::kPlzVsDoorCalc, HookPriority::MEDIUM,
      [&](void *b, HookPriority p) {
        return NativePlzVsDoorCalc::InstallAt(g_host, b, p);
      },
      [&](HookPriority p) {
        return HookPlzVsDoorCalc::InstallAt(g_host, addr::kPlzVsDoorCalc, p);
      });
  install_body_or_fallback(
      "PlayerInfoPackEnl", addr::kPlayerInfoPackEnl, HookPriority::HIGH,
      [&](void *b, HookPriority p) {
        return NativePlayerInfoPackEnl::InstallAt(g_host, b, p);
      },
      [&](HookPriority p) {
        return HookPlayerInfoPackEnl::InstallAt(g_host,
                                                addr::kPlayerInfoPackEnl, p);
      });
  install_body_or_fallback(
      "PlayerInfoUnpackEnl", addr::kPlayerInfoUnpackEnl, HookPriority::HIGH,
      [&](void *b, HookPriority p) {
        return NativePlayerInfoUnpackEnl::InstallAt(g_host, b, p);
      },
      [&](HookPriority p) {
        return HookPlayerInfoUnpackEnl::InstallAt(
            g_host, addr::kPlayerInfoUnpackEnl, p);
      });
  install_body_or_fallback(
      "GfxGetTeamColor", addr::kGfxGetTeamColor, HookPriority::HIGH,
      [&](void *b, HookPriority p) {
        return NativeGfxGetTeamColor::InstallAt(g_host, b, p);
      },
      [&](HookPriority p) {
        return HookGfxGetTeamColor::InstallAt(g_host, addr::kGfxGetTeamColor,
                                              p);
      });
  install_body_or_fallback(
      "GfxSetTeamColor", addr::kGfxSetTeamColor, HookPriority::HIGH,
      [&](void *b, HookPriority p) {
        return NativeGfxSetTeamColor::InstallAt(g_host, b, p);
      },
      [&](HookPriority p) {
        return HookGfxSetTeamColor::InstallAt(g_host, addr::kGfxSetTeamColor,
                                              p);
      });

  s_native_hooks_installed.store(native_ok, std::memory_order_relaxed);
  s_native_hooks_total.store(native_total, std::memory_order_relaxed);
  s_native_bootstrap_done.store(true, std::memory_order_release);

  if (g_host->log) {
    g_host->log(SPLATMOD_LOG_INFO, "char_customizer",
                "Installed native body hooks: %u/%u\n", native_ok,
                native_total);
  }
  std::fprintf(stderr, "[char_customizer] Installed native body hooks: %u/%u\n",
               native_ok, native_total);
  std::fflush(stderr);
}

template <typename HookT>
void run_in_local_scope(CPUState *cpu, bool clear_plaza) {
  ensure_native_body_hooks_installed(cpu);
  if (clear_plaza)
    s_in_plaza.store(false, std::memory_order_relaxed);
  ++t_local_player_scope;
  HookT::Orig(cpu);
  --t_local_player_scope;
}

void HookBootstrapPlayerMgrLoad::Callback(CPUState *cpu, uint32_t) {
  ensure_native_body_hooks_installed(cpu);
  if (cpu && cpu->mem) {
    bool already_has = false;
    {
      std::lock_guard<std::mutex> lock(s_ink_mutex);
      already_has = s_has_active_ink;
    }
    if (!already_has) {
      const uint32_t gfx = rd32(cpu->mem, addr::kGfxSettingPtr, 0u);
      const uint32_t game_info = rd32(cpu->mem, addr::kGameInfoPtr, 0u);
      if (gfx != 0u && game_info != 0u &&
          patch_team_color_buffer(
              cpu->mem, game_info + addr::kGameInfoTeamColorsOff,
              determine_local_team_slot(cpu->mem, -1), true)) {
        call_guest_frame(
            cpu, addr::kGfxSetTeamColor,
            {gfx, game_info + addr::kGameInfoTeamColorsOff, 0u});
      }
    }
  }
  s_in_plaza.store(false, std::memory_order_relaxed);
  Orig(cpu);
}

void HookBootstrapPlayerInitSubobjs::Callback(CPUState *cpu, uint32_t) {
  ensure_native_body_hooks_installed(cpu);
  Orig(cpu);
}

void HookBootstrapPlayerCalc::Callback(CPUState *cpu, uint32_t) {
  if (!s_native_bootstrap_done.load(std::memory_order_acquire))
    ensure_native_body_hooks_installed(cpu);
  Orig(cpu);
}

void HookPlazaPlayerSetup::Callback(CPUState *cpu, uint32_t) {
  run_in_local_scope<HookPlazaPlayerSetup>(cpu, false);
}
void HookLobbyPlayerSetup::Callback(CPUState *cpu, uint32_t) {
  run_in_local_scope<HookLobbyPlayerSetup>(cpu, true);
}
void HookEquipPlayerSetup::Callback(CPUState *cpu, uint32_t) {
  run_in_local_scope<HookEquipPlayerSetup>(cpu, true);
}
void HookShopPlayerSetup::Callback(CPUState *cpu, uint32_t) {
  run_in_local_scope<HookShopPlayerSetup>(cpu, true);
}
void HookMakePlayerSetup::Callback(CPUState *cpu, uint32_t) {
  run_in_local_scope<HookMakePlayerSetup>(cpu, true);
}
void HookResultPlayerSetup::Callback(CPUState *cpu, uint32_t) {
  run_in_local_scope<HookResultPlayerSetup>(cpu, true);
}
void HookDuelPlayerSetup::Callback(CPUState *cpu, uint32_t) {
  run_in_local_scope<HookDuelPlayerSetup>(cpu, true);
}

void NativeArchiveHolderLoad::Callback(CPUState *cpu, uint8_t *mem) {
  handle_archive_holder_load(cpu, [&]() { Orig(cpu, mem); });
}
void NativePlayerMgrLoadArchives::Callback(CPUState *cpu, uint8_t *mem) {
  handle_player_mgr_load_archives(cpu, [&]() { Orig(cpu, mem); });
}
void NativePlayerModelCtor::Callback(CPUState *cpu, uint8_t *mem) {
  handle_player_model_ctor(cpu, [&]() { Orig(cpu, mem); });
}
void NativePlayerCustomSetup::Callback(CPUState *cpu, uint8_t *mem) {
  handle_player_custom_setup(cpu, [&]() { Orig(cpu, mem); });
}
void NativeSetSkinColor::Callback(CPUState *cpu, uint8_t *mem) {
  handle_set_skin_color(cpu, [&]() { Orig(cpu, mem); });
}
void NativeSetEyeColor::Callback(CPUState *cpu, uint8_t *mem) {
  handle_set_eye_color(cpu, [&]() { Orig(cpu, mem); });
}
void NativePlzVsDoorCalc::Callback(CPUState *cpu, uint8_t *mem) {
  handle_plz_vs_door_calc(cpu, [&]() { Orig(cpu, mem); });
}
void NativePlayerInfoPackEnl::Callback(CPUState *cpu, uint8_t *mem) {
  handle_player_info_pack_enl(cpu, [&]() { Orig(cpu, mem); });
}
void NativePlayerInfoUnpackEnl::Callback(CPUState *cpu, uint8_t *mem) {
  handle_player_info_unpack_enl(cpu, [&]() { Orig(cpu, mem); });
}
void NativeGfxGetTeamColor::Callback(CPUState *cpu, uint8_t *mem) {
  handle_gfx_get_team_color(cpu, [&]() { Orig(cpu, mem); });
}
void NativeGfxSetTeamColor::Callback(CPUState *cpu, uint8_t *mem) {
  handle_gfx_set_team_color(cpu, [&]() { Orig(cpu, mem); });
}

void HookArchiveHolderLoad::Callback(CPUState *cpu, uint32_t) {
  handle_archive_holder_load(cpu, [&]() { Orig(cpu); });
}
void HookPlayerMgrLoadArchives::Callback(CPUState *cpu, uint32_t) {
  handle_player_mgr_load_archives(cpu, [&]() { Orig(cpu); });
}
void HookPlayerModelCtor::Callback(CPUState *cpu, uint32_t) {
  handle_player_model_ctor(cpu, [&]() { Orig(cpu); });
}
void HookPlayerCustomSetup::Callback(CPUState *cpu, uint32_t) {
  handle_player_custom_setup(cpu, [&]() { Orig(cpu); });
}
void HookSetSkinColor::Callback(CPUState *cpu, uint32_t) {
  handle_set_skin_color(cpu, [&]() { Orig(cpu); });
}
void HookSetEyeColor::Callback(CPUState *cpu, uint32_t) {
  handle_set_eye_color(cpu, [&]() { Orig(cpu); });
}
void HookPlzVsDoorCalc::Callback(CPUState *cpu, uint32_t) {
  handle_plz_vs_door_calc(cpu, [&]() { Orig(cpu); });
}
void HookPlayerInfoPackEnl::Callback(CPUState *cpu, uint32_t) {
  handle_player_info_pack_enl(cpu, [&]() { Orig(cpu); });
}
void HookPlayerInfoUnpackEnl::Callback(CPUState *cpu, uint32_t) {
  handle_player_info_unpack_enl(cpu, [&]() { Orig(cpu); });
}
void HookGfxGetTeamColor::Callback(CPUState *cpu, uint32_t) {
  handle_gfx_get_team_color(cpu, [&]() { Orig(cpu); });
}
void HookGfxSetTeamColor::Callback(CPUState *cpu, uint32_t) {
  handle_gfx_set_team_color(cpu, [&]() { Orig(cpu); });
}

} // namespace

void query_live_vanilla_appearance(const uint8_t *mem, uint32_t *out_model,
                                   uint32_t *out_skin, uint32_t *out_eye) {
  uint32_t m = 0u, s = 2u, e = 0u;
  if (mem) {
    const uint32_t game_info = rd32(mem, addr::kGameInfoPtr, 0u);
    if (game_info && mem_ok(mem, game_info + 0x14u, 4u)) {
      const uint32_t ary = rd32(mem, game_info + 0x10u, 0u);
      if (ary && mem_ok(mem, ary, 4u)) {
        const uint32_t p0 = rd32(mem, ary, 0u);
        if (p0 && mem_ok(mem, p0, addr::kPlayerInfoSize)) {
          const auto safe = clamp_vanilla_wire(
              static_cast<int32_t>(
                  rd32(mem, p0 + addr::kPlayerInfoModelType, 0u)),
              static_cast<int32_t>(
                  rd32(mem, p0 + addr::kPlayerInfoSkinCol, 2u)),
              static_cast<int32_t>(
                  rd32(mem, p0 + addr::kPlayerInfoEyeCol, 0u)));
          m = safe.model_type;
          s = safe.skin_color;
          e = safe.eye_color;
        }
      }
    }
  }
  if (out_model)
    *out_model = m;
  if (out_skin)
    *out_skin = s;
  if (out_eye)
    *out_eye = e;
}

std::string ensure_custom_archive_for_fmdl(const std::string &target_fmdl_name,
                                           std::string *out_status) {
  std::lock_guard<std::mutex> lock(s_build_mutex);
  auto &reg = AppearanceRegistry::instance();
  const CustomizerConfig cfg = reg.config();
  const std::string fmdl =
      target_fmdl_name.empty()
          ? ((cfg.body_model == BodyModelId::Boy) ? "Player01" : "Player00")
          : target_fmdl_name;
  const uint32_t req_model = (fmdl == "Player01") ? 1u : 0u;
  const ResolvedAppearance resolved = reg.resolve(cfg, req_model, 2u, 0u);

  if (!resolved.local.use_custom_model) {
    reg.set_active_custom_archive("", 0u, "Vanilla appearance active");
    if (out_status)
      *out_status = "Vanilla appearance active";
    return {};
  }

  const char fmdl_tag = (fmdl == "Player01") ? '1' : '0';

  LocalRenderPlan tagged_plan = resolved.local;
  tagged_plan.custom_archive_name =
      resolved.local.custom_archive_name + "_" + fmdl_tag;

  const std::filesystem::path expected_path =
      std::filesystem::path(reg.overlay_model_dir()) /
      (tagged_plan.custom_archive_name + ".szs");
  std::error_code ec;
  if (std::filesystem::is_regular_file(expected_path, ec)) {
    const std::string ok_msg =
        "Cached " + tagged_plan.custom_archive_name + ".szs";
    reg.set_active_custom_archive(tagged_plan.custom_archive_name,
                                  resolved.local.content_hash, ok_msg);
    if (out_status)
      *out_status = ok_msg;
    return tagged_plan.custom_archive_name;
  }

  const BfresBuildReport rep = build_custom_player_szs(
      reg.player_pack_path(), reg.overlay_model_dir(), tagged_plan, fmdl);

  if (!rep.ok) {
    reg.set_active_custom_archive("", 0u,
                                  "Fallback to vanilla: " + rep.message);
    if (out_status)
      *out_status = rep.message;
    if (g_host && g_host->log) {
      g_host->log(SPLATMOD_LOG_WARN, "char_customizer",
                  "Build failed (using vanilla fallback): %s\n",
                  rep.message.c_str());
    }
    return {};
  }

  reg.set_active_custom_archive(rep.archive_name, resolved.local.content_hash,
                                rep.message);
  if (out_status)
    *out_status = rep.message;
  if (g_host && g_host->log) {
    g_host->log(SPLATMOD_LOG_INFO, "char_customizer",
                "%s (fmdl=%s, vtx=%u, hair=%u, head_rebind=%s)\n",
                rep.message.c_str(), fmdl.c_str(), rep.combined_vtx_count,
                rep.hair_vtx_count, rep.cross_body_head_rebind ? "yes" : "no");
  }
  return rep.archive_name;
}

bool rebuild_active_appearance(std::string *out_status) {
  auto &reg = AppearanceRegistry::instance();
  const CustomizerConfig cfg = reg.config();
  const ResolvedAppearance res0 = reg.resolve(cfg, 0u, 2u, 0u);
  const ResolvedAppearance res1 = reg.resolve(cfg, 1u, 2u, 0u);
  if (!res0.local.use_custom_model && !res1.local.use_custom_model) {
    reg.set_active_custom_archive("", 0u, "Vanilla appearance active");
    if (out_status)
      *out_status = "Vanilla appearance active";
    return true;
  }

  bool any_built = false;
  std::string summary;
  if (res0.local.use_custom_model) {
    std::string s0;
    if (!ensure_custom_archive_for_fmdl("Player00", &s0).empty())
      any_built = true;
    summary = s0;
  }
  if (res1.local.use_custom_model) {
    std::string s1;
    if (!ensure_custom_archive_for_fmdl("Player01", &s1).empty())
      any_built = true;
    if (summary.empty())
      summary = s1;
  }
  if (out_status)
    *out_status = summary;
  return any_built;
}

CustomizerRuntimeStats get_runtime_stats() {
  CustomizerRuntimeStats st{};
  st.hooks_installed = s_hooks_installed.load(std::memory_order_relaxed);
  st.hooks_total = s_hooks_total.load(std::memory_order_relaxed);
  st.native_hooks_installed =
      s_native_hooks_installed.load(std::memory_order_relaxed);
  st.native_hooks_total = s_native_hooks_total.load(std::memory_order_relaxed);
  st.hook_hits = s_hook_hits.load(std::memory_order_relaxed);
  st.runtime_fallbacks = s_runtime_fallbacks.load(std::memory_order_relaxed);
  st.in_plaza = s_in_plaza.load(std::memory_order_relaxed);
  st.reload_pending = s_pending_plaza_reload.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(s_ink_mutex);
    st.has_active_ink_color = s_has_active_ink;
    if (s_has_active_ink) {
      st.active_ink_color_name = s_active_ink.display_name;
      st.active_ink_r = s_active_ink.r;
      st.active_ink_g = s_active_ink.g;
      st.active_ink_b = s_active_ink.b;
    }
  }
  return st;
}

bool request_plaza_reload() {
  if (!s_in_plaza.load(std::memory_order_relaxed))
    return false;
  s_pending_plaza_reload.store(true, std::memory_order_release);
  return true;
}

void request_ink_color_reroll() {
  s_force_ink_reroll.store(true, std::memory_order_release);
}

bool install_hooks(const SplatModHost *host) {
  g_host = host;
  if (!host)
    return false;

  s_native_bootstrap_done.store(false, std::memory_order_relaxed);
  s_native_hooks_installed.store(0u, std::memory_order_relaxed);
  s_native_hooks_total.store(11u, std::memory_order_relaxed);
  s_in_plaza.store(false, std::memory_order_relaxed);
  s_pending_plaza_reload.store(false, std::memory_order_relaxed);

  uint32_t installed = 0u;
  uint32_t total = 0u;
  auto try_install = [&](const char *name, uint32_t fn_addr, bool ok_call) {
    ++total;
    if (ok_call) {
      ++installed;
    } else if (host->log) {
      host->log(SPLATMOD_LOG_WARN, "char_customizer",
                "Failed to install guest hook %s at 0x%08X\n", name, fn_addr);
    }
  };

  // ONLY install vtable/indirect hooks via hook_guest! Direct-called functions
  // are hooked via host->hook_native at runtime by reading the original
  // _entry stubs from cpu->dispatch_table on first bootstrap call.
  try_install("BootstrapPlayerMgrLoad", addr::kPlayerMgrLoad,
              HookBootstrapPlayerMgrLoad::InstallAt(
                  host, addr::kPlayerMgrLoad, HookPriority::ULTRA_HIGH));
  try_install("BootstrapPlayerInitSubobjs", addr::kPlayerInitSubobjs,
              HookBootstrapPlayerInitSubobjs::InstallAt(
                  host, addr::kPlayerInitSubobjs, HookPriority::ULTRA_HIGH));
  try_install("BootstrapPlayerCalc", addr::kPlayerCalc,
              HookBootstrapPlayerCalc::InstallAt(host, addr::kPlayerCalc,
                                                 HookPriority::LOW));

  try_install("PlazaPlayerSetup", addr::kPlazaPlayerSetup,
              HookPlazaPlayerSetup::InstallAt(host, addr::kPlazaPlayerSetup,
                                              HookPriority::HIGH));
  try_install("LobbyPlayerSetup", addr::kLobbyPlayerSetup,
              HookLobbyPlayerSetup::InstallAt(host, addr::kLobbyPlayerSetup,
                                              HookPriority::HIGH));
  try_install("EquipPlayerSetup", addr::kEquipPlayerSetup,
              HookEquipPlayerSetup::InstallAt(host, addr::kEquipPlayerSetup,
                                              HookPriority::HIGH));
  try_install("ShopPlayerSetup", addr::kShopPlayerSetup,
              HookShopPlayerSetup::InstallAt(host, addr::kShopPlayerSetup,
                                             HookPriority::HIGH));
  try_install("MakePlayerSetup", addr::kMakePlayerSetup,
              HookMakePlayerSetup::InstallAt(host, addr::kMakePlayerSetup,
                                             HookPriority::HIGH));
  try_install("ResultPlayerSetup", addr::kResultPlayerSetup,
              HookResultPlayerSetup::InstallAt(host, addr::kResultPlayerSetup,
                                               HookPriority::HIGH));
  try_install("DuelPlayerSetup", addr::kDuelPlayerSetup,
              HookDuelPlayerSetup::InstallAt(host, addr::kDuelPlayerSetup,
                                             HookPriority::HIGH));

  s_hooks_installed.store(installed, std::memory_order_relaxed);
  s_hooks_total.store(total, std::memory_order_relaxed);

  rebuild_active_appearance(nullptr);
  return installed == total;
}

void uninstall_hooks(const SplatModHost *host) {
  if (host) {
    HookBootstrapPlayerMgrLoad::Uninstall(host);
    HookBootstrapPlayerInitSubobjs::Uninstall(host);
    HookBootstrapPlayerCalc::Uninstall(host);
    HookPlazaPlayerSetup::Uninstall(host);
    HookLobbyPlayerSetup::Uninstall(host);
    HookEquipPlayerSetup::Uninstall(host);
    HookShopPlayerSetup::Uninstall(host);
    HookMakePlayerSetup::Uninstall(host);
    HookResultPlayerSetup::Uninstall(host);
    HookDuelPlayerSetup::Uninstall(host);

    NativeArchiveHolderLoad::Uninstall(host);
    NativePlayerMgrLoadArchives::Uninstall(host);
    NativePlayerModelCtor::Uninstall(host);
    NativePlayerCustomSetup::Uninstall(host);
    NativeSetSkinColor::Uninstall(host);
    NativeSetEyeColor::Uninstall(host);
    NativePlzVsDoorCalc::Uninstall(host);
    NativePlayerInfoPackEnl::Uninstall(host);
    NativePlayerInfoUnpackEnl::Uninstall(host);
    NativeGfxGetTeamColor::Uninstall(host);
    NativeGfxSetTeamColor::Uninstall(host);

    HookArchiveHolderLoad::Uninstall(host);
    HookPlayerMgrLoadArchives::Uninstall(host);
    HookPlayerModelCtor::Uninstall(host);
    HookPlayerCustomSetup::Uninstall(host);
    HookSetSkinColor::Uninstall(host);
    HookSetEyeColor::Uninstall(host);
    HookPlzVsDoorCalc::Uninstall(host);
    HookPlayerInfoPackEnl::Uninstall(host);
    HookPlayerInfoUnpackEnl::Uninstall(host);
    HookGfxGetTeamColor::Uninstall(host);
    HookGfxSetTeamColor::Uninstall(host);
  }
  {
    std::lock_guard<std::mutex> lock(s_preload_mutex);
    s_preloaded[0] = PreloadedCustomHolder{};
    s_preloaded[1] = PreloadedCustomHolder{};
  }
  {
    std::lock_guard<std::mutex> lock(s_ink_mutex);
    s_has_active_ink = false;
    s_active_ink = InkColorDef{};
    s_last_ink_roll_ms = 0;
  }
  s_force_ink_reroll.store(false, std::memory_order_relaxed);
  s_native_bootstrap_done.store(false, std::memory_order_relaxed);
  s_hooks_installed.store(0u, std::memory_order_relaxed);
  s_hooks_total.store(0u, std::memory_order_relaxed);
  s_native_hooks_installed.store(0u, std::memory_order_relaxed);
  s_in_plaza.store(false, std::memory_order_relaxed);
  s_pending_plaza_reload.store(false, std::memory_order_relaxed);
  g_host = nullptr;
}

} // namespace char_customizer
