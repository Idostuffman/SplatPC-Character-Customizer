#include "ui_assets.h"

#include "bfres_customizer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace char_customizer {

namespace {

uint16_t rd16(const uint8_t *p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8u) |
                               static_cast<uint16_t>(p[1]));
}

uint32_t rd32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24u) |
         (static_cast<uint32_t>(p[1]) << 16u) |
         (static_cast<uint32_t>(p[2]) << 8u) | static_cast<uint32_t>(p[3]);
}

void decode_bc4_block(const uint8_t *blk, uint8_t out[16]) {
  const uint8_t a0 = blk[0];
  const uint8_t a1 = blk[1];
  uint8_t pal[8]{};
  pal[0] = a0;
  pal[1] = a1;
  if (a0 > a1) {
    for (int i = 1; i <= 6; ++i) {
      pal[1 + i] = static_cast<uint8_t>(((7 - i) * a0 + i * a1) / 7);
    }
  } else {
    for (int i = 1; i <= 4; ++i) {
      pal[1 + i] = static_cast<uint8_t>(((5 - i) * a0 + i * a1) / 5);
    }
    pal[6] = 0u;
    pal[7] = 255u;
  }
  uint64_t bits = 0u;
  for (int i = 0; i < 6; ++i) {
    bits |= (static_cast<uint64_t>(blk[2 + i]) << (8 * i));
  }
  for (int i = 0; i < 16; ++i) {
    out[i] = pal[(bits >> (3 * i)) & 7u];
  }
}

uint32_t compute_pitch_blocks(uint32_t bw, uint32_t tile_mode) {
  const uint32_t min_p = (tile_mode <= 3u) ? 1u : 8u;
  return ((bw + min_p - 1u) / min_p) * min_p;
}

bool decode_bflim_mask(const uint8_t *data, size_t len, const SplatModUiEx *ex,
                       uint32_t &out_w, uint32_t &out_h,
                       std::vector<uint8_t> &out_rgba) {
  if (!data || len < 40u || !ex || !ex->detile_gx2_mip0)
    return false;
  const uint8_t *flim = data + len - 40u;
  if (std::memcmp(flim, "FLIM", 4) != 0)
    return false;
  const uint8_t *imag = flim + 16u;
  if (std::memcmp(imag, "imag", 4) != 0)
    return false;

  const uint16_t w = rd16(imag + 8u);
  const uint16_t h = rd16(imag + 10u);
  const uint8_t fmt = imag[14u];
  const uint8_t swiz = imag[15u];
  const uint32_t data_sz = rd32(imag + 16u);
  if (w == 0u || h == 0u || data_sz == 0u || data_sz > len - 40u)
    return false;

  const uint32_t tile_mode = swiz & 0x1Fu;
  const uint32_t gx2_swiz = static_cast<uint32_t>((swiz >> 5u) & 0x07u) << 8u;
  if (fmt != 15u && fmt != 16u && fmt != 17u)
    return false;

  const uint32_t bw = (w + 3u) / 4u;
  const uint32_t bh = (h + 3u) / 4u;
  const uint32_t pitch = compute_pitch_blocks(bw, tile_mode);
  const uint32_t bpe = (fmt == 17u) ? 128u : 64u;
  std::vector<uint8_t> linear(static_cast<size_t>(bw) * bh * (bpe / 8u), 0u);
  if (!ex->detile_gx2_mip0(nullptr, bw, bh, pitch, bpe, tile_mode, gx2_swiz,
                           data, data_sz, linear.data())) {
    return false;
  }

  out_w = w;
  out_h = h;
  out_rgba.assign(static_cast<size_t>(w) * h * 4u, 255u);
  for (uint32_t by = 0; by < bh; ++by) {
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const uint8_t *blk =
          linear.data() + (static_cast<size_t>(by) * bw + bx) * (bpe / 8u);
      uint8_t lum[16]{};
      uint8_t alp[16]{};
      decode_bc4_block(blk, lum);
      if (fmt == 17u) {
        decode_bc4_block(blk + 8u, alp);
      } else {
        std::memcpy(alp, lum, 16u);
      }
      for (uint32_t py = 0; py < 4u; ++py) {
        const uint32_t y = by * 4u + py;
        if (y >= h)
          continue;
        for (uint32_t px = 0; px < 4u; ++px) {
          const uint32_t x = bx * 4u + px;
          if (x >= w)
            continue;
          const size_t ti = py * 4u + px;
          uint8_t *dst =
              out_rgba.data() + (static_cast<size_t>(y) * w + x) * 4u;
          dst[0] = lum[ti];
          dst[1] = lum[ti];
          dst[2] = lum[ti];
          dst[3] = alp[ti];
        }
      }
    }
  }
  return true;
}

bool load_bflim_from_pack(const std::vector<SarcEntryOwned> &pack,
                          const char *szs_name, const char *bflim_sub,
                          const SplatModUiEx *ex, uint32_t &w, uint32_t &h,
                          std::vector<uint8_t> &rgba) {
  for (const auto &e : pack) {
    if (e.name != szs_name)
      continue;
    std::vector<uint8_t> outer;
    if (!decompress_yaz0(e.data.data(), e.data.size(), outer))
      return false;
    std::vector<SarcEntryOwned> inner;
    if (!parse_sarc_owned(outer.data(), outer.size(), inner))
      return false;
    for (const auto &oe : inner) {
      if (oe.name.size() >= 4u &&
          oe.name.compare(oe.name.size() - 4u, 4u, ".arc") == 0) {
        std::vector<SarcEntryOwned> arc_files;
        if (!parse_sarc_owned(oe.data.data(), oe.data.size(), arc_files))
          return false;
        for (const auto &af : arc_files) {
          if (af.name.find(bflim_sub) != std::string::npos) {
            return decode_bflim_mask(af.data.data(), af.data.size(), ex, w, h,
                                     rgba);
          }
        }
      } else if (oe.name.find(bflim_sub) != std::string::npos) {
        return decode_bflim_mask(oe.data.data(), oe.data.size(), ex, w, h,
                                 rgba);
      }
    }
  }
  return false;
}

} // namespace

UiAssetsCache &UiAssetsCache::instance() {
  static UiAssetsCache inst;
  return inst;
}

void UiAssetsCache::initialize(const std::string &game_dir) {
  layout_pack_path_ =
      (std::filesystem::path(game_dir) / "content" / "Pack" / "Layout.pack")
          .string();
}

void UiAssetsCache::ensure_uploaded(const SplatModUi *ui) {
  if (!ui || !ui->ex || ui->ex->struct_size < sizeof(SplatModUiEx))
    return;
  const SplatModUiEx *ex = ui->ex;
  if (!ex->texture_epoch || !ex->create_rgba_texture)
    return;

  const uint64_t epoch = ex->texture_epoch(ui->user);
  if (epoch != 0u && epoch == last_epoch_ &&
      (bg_wave_tex_ != 0u || squid_tex_ != 0u))
    return;

  last_ex_ = ex;
  last_user_ = ui->user;
  last_epoch_ = epoch;
  bg_wave_tex_ = 0u;
  squid_tex_ = 0u;

  std::ifstream in(layout_pack_path_, std::ios::binary | std::ios::ate);
  if (!in.is_open())
    return;
  const auto sz = in.tellg();
  if (sz <= 0 || sz > 256 * 1024 * 1024)
    return;
  std::vector<uint8_t> pack_bytes(static_cast<size_t>(sz));
  in.seekg(0, std::ios::beg);
  if (!in.read(reinterpret_cast<char *>(pack_bytes.data()), sz))
    return;

  std::vector<SarcEntryOwned> pack;
  if (!parse_sarc_owned(pack_bytes.data(), pack_bytes.size(), pack))
    return;

  uint32_t w = 0u, h = 0u;
  std::vector<uint8_t> raw;
  if (load_bflim_from_pack(pack, "Layout/Cmn_BG_00.szs",
                           "GambitWavePatternA^s.bflim", ex, w, h, raw)) {
    const uint32_t ow = w * 2u, oh = h * 2u;
    std::vector<uint8_t> tiled(static_cast<size_t>(ow) * oh * 4u, 255u);
    for (uint32_t y = 0; y < oh; ++y) {
      for (uint32_t x = 0; x < ow; ++x) {
        const uint8_t m =
            raw[(static_cast<size_t>(y % h) * w + (x % w)) * 4u + 3u];
        const float a = (static_cast<float>(m) / 255.0f) * 0.14f;
        uint8_t *dst = tiled.data() + (static_cast<size_t>(y) * ow + x) * 4u;
        dst[0] = static_cast<uint8_t>(18.0f * (1.0f - a) + 128.0f * a);
        dst[1] = static_cast<uint8_t>(18.0f * (1.0f - a) + 178.0f * a);
        dst[2] = static_cast<uint8_t>(22.0f * (1.0f - a) + 22.0f * a);
      }
    }
    bg_wave_tex_ = ex->create_rgba_texture(ui->user, tiled.data(), ow, oh);
  }

  if (load_bflim_from_pack(pack, "Layout/Cmn_Cursor_00.szs",
                           "IkaYajiAnime_00^s.bflim", ex, w, h, raw)) {
    for (size_t i = 0; i + 3u < raw.size(); i += 4u) {
      raw[i + 0u] = 255u;
      raw[i + 1u] = 106u;
      raw[i + 2u] = 0u;
    }
    squid_tex_ = ex->create_rgba_texture(ui->user, raw.data(), w, h);
  }
}

void UiAssetsCache::shutdown() {
  if (last_ex_ && last_ex_->destroy_texture) {
    if (bg_wave_tex_)
      last_ex_->destroy_texture(last_user_, bg_wave_tex_);
    if (squid_tex_)
      last_ex_->destroy_texture(last_user_, squid_tex_);
  }
  bg_wave_tex_ = 0u;
  squid_tex_ = 0u;
  last_epoch_ = 0u;
  last_ex_ = nullptr;
  last_user_ = nullptr;
}

} // namespace char_customizer
