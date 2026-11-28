#include "bfres_customizer.h"

#include "appearance_registry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace char_customizer {

namespace {

uint16_t rd_be16(const uint8_t *p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8u) |
                               static_cast<uint16_t>(p[1]));
}

uint32_t rd_be32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24u) |
         (static_cast<uint32_t>(p[1]) << 16u) |
         (static_cast<uint32_t>(p[2]) << 8u) | static_cast<uint32_t>(p[3]);
}

int32_t rd_bes32(const uint8_t *p) { return static_cast<int32_t>(rd_be32(p)); }

float rd_bef32(const uint8_t *p) {
  const uint32_t bits = rd_be32(p);
  float f = 0.0f;
  std::memcpy(&f, &bits, 4);
  return f;
}

void wr_be16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8u);
  p[1] = static_cast<uint8_t>(v & 0xFFu);
}

void wr_be32(uint8_t *p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v >> 24u);
  p[1] = static_cast<uint8_t>(v >> 16u);
  p[2] = static_cast<uint8_t>(v >> 8u);
  p[3] = static_cast<uint8_t>(v & 0xFFu);
}

void wr_bes32(uint8_t *p, int32_t v) { wr_be32(p, static_cast<uint32_t>(v)); }

void wr_bef32(uint8_t *p, float f) {
  uint32_t bits = 0;
  std::memcpy(&bits, &f, 4);
  wr_be32(p, bits);
}

size_t align_up(size_t v, size_t a) { return ((v + a - 1u) / a) * a; }

uint32_t sarc_name_hash(const std::string &s) {
  uint32_t h = 0;
  for (unsigned char c : s) {
    h = h * 0x00000065u + c;
  }
  return h;
}

std::string rd_bfres_str(const std::vector<uint8_t> &buf, size_t rel_pos) {
  if (rel_pos + 4u > buf.size())
    return {};
  const int32_t rel = rd_bes32(buf.data() + rel_pos);
  if (rel == 0)
    return {};
  const int64_t p = static_cast<int64_t>(rel_pos) + rel;
  if (p < 0 || static_cast<size_t>(p) >= buf.size())
    return {};
  size_t end = static_cast<size_t>(p);
  while (end < buf.size() && buf[end] != 0u)
    ++end;
  return std::string(reinterpret_cast<const char *>(buf.data() + p),
                     end - static_cast<size_t>(p));
}

struct BfresDictEntry {
  std::string name;
  size_t data_off = 0;
};

std::vector<BfresDictEntry> parse_bfres_dict(const std::vector<uint8_t> &buf,
                                             size_t grp_off) {
  std::vector<BfresDictEntry> out;
  if (grp_off == 0 || grp_off + 8u > buf.size())
    return out;
  const uint32_t count = rd_be32(buf.data() + grp_off + 4u);
  if (count > 4096u ||
      grp_off + 8u + static_cast<size_t>(count + 1u) * 16u > buf.size())
    return out;
  for (uint32_t i = 1u; i <= count; ++i) {
    const size_t eoff = grp_off + 8u + static_cast<size_t>(i) * 16u;
    const int32_t drel = rd_bes32(buf.data() + eoff + 12u);
    BfresDictEntry e{};
    e.name = rd_bfres_str(buf, eoff + 8u);
    if (drel != 0) {
      const int64_t abs = static_cast<int64_t>(eoff + 12u) + drel;
      if (abs > 0 && static_cast<size_t>(abs) < buf.size())
        e.data_off = static_cast<size_t>(abs);
    }
    out.push_back(std::move(e));
  }
  return out;
}

bool read_file_bytes(const std::string &path, std::vector<uint8_t> &out) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in.is_open())
    return false;
  const auto sz = in.tellg();
  if (sz <= 0 || sz > 256 * 1024 * 1024)
    return false;
  out.resize(static_cast<size_t>(sz));
  in.seekg(0, std::ios::beg);
  return static_cast<bool>(in.read(reinterpret_cast<char *>(out.data()), sz));
}

bool atomic_write_bytes(const std::string &path,
                        const std::vector<uint8_t> &bytes) {
  const std::filesystem::path p(path);
  const std::filesystem::path tmp(path + ".tmp");
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
      return false;
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.flush();
    if (!out.good())
      return false;
  }
  std::filesystem::rename(tmp, p, ec);
  if (ec) {
    std::filesystem::remove(p, ec);
    ec.clear();
    std::filesystem::rename(tmp, p, ec);
  }
  return !ec;
}

// Unified decoded vertex spanning all 8 Player BFRES streams (_p0, _n0, _t0,
// _w0, _i0, _u0, _u1, _u2).
struct TeamColorVertex {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  float nx = 0.0f, ny = 1.0f, nz = 0.0f;
  std::array<uint8_t, 4> t0{127, 0, 0, 127};
  std::array<uint8_t, 4> w0{255, 0, 0, 0};
  std::array<uint8_t, 4> i0{20, 20, 20, 0};
  std::array<uint8_t, 4> u0{};
  std::array<uint8_t, 4> u1{};
  std::array<uint8_t, 4> u2{};
  bool is_hair = false;
};

struct TeamColorShapeData {
  std::vector<TeamColorVertex> verts;
  std::array<std::vector<uint16_t>, 3> lod_indices{};
  uint32_t lod_count = 0u;
  size_t fvtx_off = 0u;
  size_t fshp_off = 0u;
  uint32_t buf_count = 0u;
  uint32_t vtx_skin_count = 0u;
  bool has_w0 = true;
  int32_t w0_buf_idx = 3;
  int32_t i0_buf_idx = 4;
  int32_t u0_buf_idx = 5;
  int32_t u1_buf_idx = 6;
  int32_t u2_buf_idx = 7;
};

float decode_snorm10(uint32_t bits10) {
  int32_t v = static_cast<int32_t>(bits10 & 0x3FFu);
  if (v & 0x200)
    v -= 0x400;
  return std::max(-1.0f, static_cast<float>(v) / 511.0f);
}

uint32_t encode_snorm10_2(float nx, float ny, float nz) {
  const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
  if (len > 1e-6f) {
    nx /= len;
    ny /= len;
    nz /= len;
  } else {
    nx = 0.0f;
    ny = 1.0f;
    nz = 0.0f;
  }
  auto q10 = [](float c) -> uint32_t {
    int32_t iv =
        static_cast<int32_t>(std::lround(std::clamp(c, -1.0f, 1.0f) * 511.0f));
    iv = std::clamp(iv, -511, 511);
    return static_cast<uint32_t>(iv) & 0x3FFu;
  };
  return q10(nx) | (q10(ny) << 10u) | (q10(nz) << 20u);
}

bool is_hair_bone_name(const std::string &b) {
  return b == "head" || b.find("Hair") != std::string::npos ||
         b.find("hair") != std::string::npos || b.rfind("nape", 0) == 0;
}

bool extract_teamcolor_shape(const std::vector<uint8_t> &bfres,
                             const std::string &shape_name, bool force_all_hair,
                             TeamColorShapeData &out) {
  out = TeamColorShapeData{};
  if (bfres.size() < 0x40u || std::memcmp(bfres.data(), "FRES", 4) != 0)
    return false;
  const int32_t fmdl_grp_rel = rd_bes32(bfres.data() + 0x20u);
  if (!fmdl_grp_rel)
    return false;
  const auto fmdls =
      parse_bfres_dict(bfres, 0x20u + static_cast<size_t>(fmdl_grp_rel));
  if (fmdls.empty())
    return false;
  const size_t moff = fmdls[0].data_off;

  const size_t skl =
      moff + 0x0Cu + static_cast<size_t>(rd_bes32(bfres.data() + moff + 0x0Cu));
  const size_t fvtx_arr =
      moff + 0x10u + static_cast<size_t>(rd_bes32(bfres.data() + moff + 0x10u));
  const size_t fshp_grp =
      moff + 0x14u + static_cast<size_t>(rd_bes32(bfres.data() + moff + 0x14u));

  const uint16_t bone_cnt = rd_be16(bfres.data() + skl + 0x08u);
  const uint16_t smooth_cnt = rd_be16(bfres.data() + skl + 0x0Au);
  const uint16_t rigid_cnt = rd_be16(bfres.data() + skl + 0x0Cu);
  const size_t barr =
      skl + 0x14u + static_cast<size_t>(rd_bes32(bfres.data() + skl + 0x14u));
  const size_t mtx_arr =
      skl + 0x18u + static_cast<size_t>(rd_bes32(bfres.data() + skl + 0x18u));

  std::vector<bool> bone_is_hair(bone_cnt, false);
  for (uint16_t bi = 0; bi < bone_cnt; ++bi) {
    bone_is_hair[bi] = is_hair_bone_name(
        rd_bfres_str(bfres, barr + static_cast<size_t>(bi) * 0x40u));
  }
  const uint32_t total_mtx =
      static_cast<uint32_t>(smooth_cnt) + static_cast<uint32_t>(rigid_cnt);
  std::vector<bool> mtx_is_hair(total_mtx, false);
  for (uint32_t mi = 0; mi < total_mtx; ++mi) {
    if (mtx_arr + static_cast<size_t>(mi) * 2u + 2u <= bfres.size()) {
      const uint16_t b =
          rd_be16(bfres.data() + mtx_arr + static_cast<size_t>(mi) * 2u);
      if (b < bone_cnt)
        mtx_is_hair[mi] = bone_is_hair[b];
    }
  }

  size_t fshp_off = 0;
  for (const auto &e : parse_bfres_dict(bfres, fshp_grp)) {
    if (e.name == shape_name) {
      fshp_off = e.data_off;
      break;
    }
  }
  if (fshp_off == 0)
    return false;

  const uint16_t vtx_idx = rd_be16(bfres.data() + fshp_off + 0x12u);
  const uint8_t vtx_skin_cnt = bfres[fshp_off + 0x16u];
  const uint8_t lod_cnt = bfres[fshp_off + 0x17u];
  const size_t voff = fvtx_arr + static_cast<size_t>(vtx_idx) * 0x20u;
  if (voff + 0x20u > bfres.size())
    return false;

  const uint8_t attr_cnt = bfres[voff + 0x04u];
  const uint8_t buf_cnt = bfres[voff + 0x05u];
  const uint32_t vtx_cnt = rd_be32(bfres.data() + voff + 0x08u);
  const size_t attr_arr =
      voff + 0x10u + static_cast<size_t>(rd_bes32(bfres.data() + voff + 0x10u));
  const size_t buf_arr =
      voff + 0x18u + static_cast<size_t>(rd_bes32(bfres.data() + voff + 0x18u));

  out.fvtx_off = voff;
  out.fshp_off = fshp_off;
  out.buf_count = buf_cnt;
  out.vtx_skin_count = vtx_skin_cnt;
  out.lod_count = std::min<uint32_t>(lod_cnt, 3u);

  std::unordered_map<std::string, int> attr_buf;
  for (uint8_t ai = 0; ai < attr_cnt; ++ai) {
    const size_t ao = attr_arr + static_cast<size_t>(ai) * 0x0Cu;
    attr_buf[rd_bfres_str(bfres, ao)] = bfres[ao + 4u];
  }

  auto read_buf_span = [&](int bi, const uint8_t *&ptr, uint16_t &stride,
                           uint32_t &sz) -> bool {
    if (bi < 0 || bi >= buf_cnt)
      return false;
    const size_t bo = buf_arr + static_cast<size_t>(bi) * 0x18u;
    sz = rd_be32(bfres.data() + bo + 4u);
    stride = rd_be16(bfres.data() + bo + 12u);
    const int32_t drel = rd_bes32(bfres.data() + bo + 20u);
    const size_t doff =
        static_cast<size_t>(static_cast<int64_t>(bo + 20u) + drel);
    if (doff + sz > bfres.size())
      return false;
    ptr = bfres.data() + doff;
    return true;
  };

  const uint8_t *p0 = nullptr;
  uint16_t p0_st = 0;
  uint32_t p0_sz = 0;
  const uint8_t *n0 = nullptr;
  uint16_t n0_st = 0;
  uint32_t n0_sz = 0;
  const uint8_t *t0 = nullptr;
  uint16_t t0_st = 0;
  uint32_t t0_sz = 0;
  const uint8_t *w0 = nullptr;
  uint16_t w0_st = 0;
  uint32_t w0_sz = 0;
  const uint8_t *i0 = nullptr;
  uint16_t i0_st = 0;
  uint32_t i0_sz = 0;
  const uint8_t *u0 = nullptr;
  uint16_t u0_st = 0;
  uint32_t u0_sz = 0;
  const uint8_t *u1 = nullptr;
  uint16_t u1_st = 0;
  uint32_t u1_sz = 0;
  const uint8_t *u2 = nullptr;
  uint16_t u2_st = 0;
  uint32_t u2_sz = 0;

  if (!read_buf_span(attr_buf.count("_p0") ? attr_buf["_p0"] : 0, p0, p0_st,
                     p0_sz))
    return false;
  read_buf_span(attr_buf.count("_n0") ? attr_buf["_n0"] : 1, n0, n0_st, n0_sz);
  read_buf_span(attr_buf.count("_t0") ? attr_buf["_t0"] : 2, t0, t0_st, t0_sz);
  out.w0_buf_idx = attr_buf.count("_w0") ? attr_buf["_w0"] : -1;
  out.has_w0 =
      (out.w0_buf_idx >= 0) && read_buf_span(out.w0_buf_idx, w0, w0_st, w0_sz);
  out.i0_buf_idx = attr_buf.count("_i0") ? attr_buf["_i0"] : -1;
  read_buf_span(out.i0_buf_idx, i0, i0_st, i0_sz);
  out.u0_buf_idx = attr_buf.count("_u0") ? attr_buf["_u0"] : -1;
  read_buf_span(out.u0_buf_idx, u0, u0_st, u0_sz);
  out.u1_buf_idx = attr_buf.count("_u1") ? attr_buf["_u1"] : -1;
  read_buf_span(out.u1_buf_idx, u1, u1_st, u1_sz);
  out.u2_buf_idx = attr_buf.count("_u2") ? attr_buf["_u2"] : -1;
  read_buf_span(out.u2_buf_idx, u2, u2_st, u2_sz);

  out.verts.resize(vtx_cnt);
  for (uint32_t vi = 0; vi < vtx_cnt; ++vi) {
    TeamColorVertex v{};
    if (vi * p0_st + 12u <= p0_sz) {
      v.x = rd_bef32(p0 + vi * p0_st + 0u);
      v.y = rd_bef32(p0 + vi * p0_st + 4u);
      v.z = rd_bef32(p0 + vi * p0_st + 8u);
    }
    if (n0 && vi * n0_st + 4u <= n0_sz) {
      const uint32_t np = rd_be32(n0 + vi * n0_st);
      v.nx = decode_snorm10(np);
      v.ny = decode_snorm10(np >> 10u);
      v.nz = decode_snorm10(np >> 20u);
    }
    if (t0 && vi * t0_st + 4u <= t0_sz)
      std::memcpy(v.t0.data(), t0 + vi * t0_st, 4u);
    if (w0 && vi * w0_st + 4u <= w0_sz)
      std::memcpy(v.w0.data(), w0 + vi * w0_st, 4u);
    if (i0 && vi * i0_st + 4u <= i0_sz) {
      std::memcpy(v.i0.data(), i0 + vi * i0_st, 4u);
    } else if (i0 && i0_st == 1u && vi < i0_sz) {
      v.i0 = {i0[vi], i0[vi], i0[vi], 0u};
      v.w0 = {255u, 0u, 0u, 0u};
    }
    if (u0 && vi * u0_st + 4u <= u0_sz)
      std::memcpy(v.u0.data(), u0 + vi * u0_st, 4u);
    if (u1 && vi * u1_st + 4u <= u1_sz)
      std::memcpy(v.u1.data(), u1 + vi * u1_st, 4u);
    if (u2 && vi * u2_st + 4u <= u2_sz)
      std::memcpy(v.u2.data(), u2 + vi * u2_st, 4u);

    if (force_all_hair) {
      v.is_hair = true;
    } else {
      const uint32_t chans =
          std::clamp<uint32_t>(vtx_skin_cnt ? vtx_skin_cnt : 1u, 1u, 4u);
      bool any_hair = false;
      for (uint32_t k = 0; k < chans; ++k) {
        if (!w0 || v.w0[k] > 0u) {
          const uint32_t mi = v.i0[k];
          if (mi < total_mtx && mtx_is_hair[mi])
            any_hair = true;
        }
      }
      v.is_hair = any_hair;
    }
    out.verts[vi] = v;
  }

  const size_t lod_arr =
      fshp_off + 0x24u +
      static_cast<size_t>(rd_bes32(bfres.data() + fshp_off + 0x24u));
  for (uint32_t li = 0; li < out.lod_count; ++li) {
    const size_t lo = lod_arr + static_cast<size_t>(li) * 0x1Cu;
    const uint32_t icnt = rd_be32(bfres.data() + lo + 8u);
    const int32_t ib_rel = rd_bes32(bfres.data() + lo + 20u);
    const size_t ibo =
        static_cast<size_t>(static_cast<int64_t>(lo + 20u) + ib_rel);
    const uint32_t isz = rd_be32(bfres.data() + ibo + 4u);
    const int32_t id_rel = rd_bes32(bfres.data() + ibo + 20u);
    const size_t id_off =
        static_cast<size_t>(static_cast<int64_t>(ibo + 20u) + id_rel);
    if (id_off + icnt * 2u > bfres.size() || icnt * 2u > isz)
      return false;
    out.lod_indices[li].resize(icnt);
    for (uint32_t k = 0; k < icnt; ++k) {
      out.lod_indices[li][k] =
          rd_be16(bfres.data() + id_off + static_cast<size_t>(k) * 2u);
    }
  }
  return true;
}

// Verified Girl/Boy/Octo bind-pose head origin (0.000, 9.7208, 0.000) in
// Player00/Player01/Rival00 FSKL.
constexpr float kHeadBindY = 9.7208f;

void sculpt_hair_vertex(TeamColorVertex &v, HairSculptPreset preset,
                        const HairParamTransform &custom_tf) {
  float dx = v.x;
  float dy = v.y - kHeadBindY;
  float dz = v.z;

  switch (preset) {
  case HairSculptPreset::S2ShortBob:
    if (dy < 0.0f)
      dy *= 0.56f;
    dx *= 1.07f;
    dz = dz * 0.92f + 0.14f;
    break;
  case HairSculptPreset::S2TwinTails:
    if (dy < 0.2f && std::fabs(dx) > 0.7f) {
      dx += (dx > 0.0f ? 0.85f : -0.85f) * std::min(1.0f, (0.5f - dy) * 0.35f);
      dy += 0.55f * std::min(1.0f, std::fabs(dx) * 0.25f);
    }
    dz *= 0.96f;
    break;
  case HairSculptPreset::S2Spiky:
    if (dy > 0.4f) {
      dy *= 1.24f;
      dx *= 1.14f;
      dz *= 1.12f;
    }
    break;
  case HairSculptPreset::S2SideSwoop:
    if (dz > -0.2f || dx < 0.0f) {
      dx += 0.55f * std::max(0.0f, 1.0f - std::fabs(dy - 1.2f) * 0.25f);
    }
    if (dy < 0.0f)
      dy *= 0.65f;
    break;
  case HairSculptPreset::S2MushCut:
    if (dy < 0.0f)
      dy *= 0.48f;
    dx *= 1.14f;
    dz *= 1.10f;
    break;
  case HairSculptPreset::S3LongWave:
    if (dy < 0.3f) {
      dy *= 1.24f;
      dz -= 0.30f * std::sin(std::clamp(-dy * 0.45f, 0.0f, 3.14159f));
    }
    dx *= 1.05f;
    break;
  case HairSculptPreset::S3Pompadour:
    if (dz > -0.2f) {
      dy += 0.85f * std::clamp((dz + 0.2f) * 0.35f, 0.0f, 1.0f);
      dz += 0.55f;
    }
    dx *= 0.92f;
    break;
  case HairSculptPreset::S3OctoCurl:
    dx *= 0.92f;
    if (dy < 0.0f)
      dy *= 0.72f;
    else
      dy *= 1.06f;
    dz += 0.12f;
    break;
  case HairSculptPreset::S3ShortCrop:
    dx *= 0.94f;
    if (dy > 0.3f)
      dy *= 0.78f;
    if (dy < -0.2f)
      dy = -0.2f + (dy + 0.2f) * 0.45f;
    dz *= 0.92f;
    break;
  case HairSculptPreset::CustomParametric: {
    dx *= custom_tf.scale_x;
    dy *= custom_tf.scale_y;
    dz *= custom_tf.scale_z;
    const float rad = custom_tf.tilt_deg * 0.0174532925f;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    const float ny = dy * c - dz * s;
    const float nz = dy * s + dz * c;
    dy = ny + custom_tf.offset_y;
    dz = nz + custom_tf.offset_z;
    dx += custom_tf.offset_x;
    break;
  }
  case HairSculptPreset::None:
  default:
    break;
  }

  v.x = dx;
  v.y = kHeadBindY + dy;
  v.z = dz;
}

bool load_external_obj_hair(const std::string &obj_path,
                            const HairParamTransform &tf,
                            std::vector<TeamColorVertex> &out_verts,
                            std::vector<uint16_t> &out_indices) {
  std::ifstream in(obj_path);
  if (!in.is_open())
    return false;

  std::vector<std::array<float, 3>> pos;
  std::vector<std::array<float, 2>> uv;
  std::vector<std::array<float, 3>> nrm;
  out_verts.clear();
  out_indices.clear();

  auto to_u16 = [](float val) -> uint8_t {
    // Pack 0..1 float cleanly into snorm8/unorm8 UV channel (low byte
    // approximation keeps scale consistent)
    const int v =
        static_cast<int>(std::lround(std::clamp(val, -1.0f, 1.0f) * 127.0f));
    return static_cast<uint8_t>(static_cast<int8_t>(std::clamp(v, -127, 127)));
  };
  auto pack_uv_unorm16 = [&](float u, float v_coord,
                             std::array<uint8_t, 4> &dst) {
    // Player M_TeamColor UV streams use GX2 format 0x7 (UNORM_16_16 BE).
    const uint16_t qu = to_u16(u);
    const uint16_t qv = to_u16(v_coord);
    dst[0] = static_cast<uint8_t>(qu >> 8u);
    dst[1] = static_cast<uint8_t>(qu & 0xFFu);
    dst[2] = static_cast<uint8_t>(qv >> 8u);
    dst[3] = static_cast<uint8_t>(qv & 0xFFu);
  };

  std::string line;
  while (std::getline(in, line)) {
    std::istringstream iss(line);
    std::string tag;
    if (!(iss >> tag))
      continue;
    if (tag == "v") {
      float x = 0, y = 0, z = 0;
      if (iss >> x >> y >> z)
        pos.push_back({x, y, z});
    } else if (tag == "vt") {
      float u = 0, vc = 0;
      if (iss >> u >> vc)
        uv.push_back({u, vc});
    } else if (tag == "vn") {
      float nx = 0, ny = 1, nz = 0;
      if (iss >> nx >> ny >> nz)
        nrm.push_back({nx, ny, nz});
    } else if (tag == "f") {
      std::vector<uint16_t> face;
      std::string tok;
      while (iss >> tok) {
        int vi = 0, ti = 0, ni = 0;
        const size_t s1 = tok.find('/');
        vi = std::atoi(tok.substr(0, s1).c_str());
        if (s1 != std::string::npos) {
          const size_t s2 = tok.find('/', s1 + 1);
          if (s2 != s1 + 1)
            ti = std::atoi(tok.substr(s1 + 1, s2 - s1 - 1).c_str());
          if (s2 != std::string::npos)
            ni = std::atoi(tok.substr(s2 + 1).c_str());
        }
        const int p_idx =
            vi > 0 ? (vi - 1) : (static_cast<int>(pos.size()) + vi);
        if (p_idx < 0 || static_cast<size_t>(p_idx) >= pos.size())
          return false;

        TeamColorVertex tv{};
        tv.x = pos[p_idx][0];
        tv.y = pos[p_idx][1];
        tv.z = pos[p_idx][2];
        if (ni != 0) {
          const int n_idx =
              ni > 0 ? (ni - 1) : (static_cast<int>(nrm.size()) + ni);
          if (n_idx >= 0 && static_cast<size_t>(n_idx) < nrm.size()) {
            tv.nx = nrm[n_idx][0];
            tv.ny = nrm[n_idx][1];
            tv.nz = nrm[n_idx][2];
          }
        }
        float uu = 0.5f, vv = 0.5f;
        if (ti != 0) {
          const int t_idx =
              ti > 0 ? (ti - 1) : (static_cast<int>(uv.size()) + ti);
          if (t_idx >= 0 && static_cast<size_t>(t_idx) < uv.size()) {
            uu = uv[t_idx][0];
            vv = 1.0f - uv[t_idx][1];
          }
        }
        pack_uv_unorm16(uu, vv, tv.u0);
        tv.u1 = tv.u0;
        tv.u2 = tv.u0;
        tv.i0 = {20u, 20u, 20u, 0u};
        tv.w0 = {255u, 0u, 0u, 0u};
        tv.is_hair = true;
        sculpt_hair_vertex(tv, HairSculptPreset::CustomParametric, tf);

        if (out_verts.size() >= 60000u)
          return false;
        face.push_back(static_cast<uint16_t>(out_verts.size()));
        out_verts.push_back(tv);
      }
      for (size_t k = 1u; k + 1u < face.size(); ++k) {
        out_indices.push_back(face[0]);
        out_indices.push_back(face[k]);
        out_indices.push_back(face[k + 1u]);
      }
    }
  }
  return !out_verts.empty() && !out_indices.empty();
}

// Recompute smooth area-weighted vertex normals for sculpted hair vertices so
// shading stays clean.
void recompute_smooth_normals(std::vector<TeamColorVertex> &verts,
                              size_t start_v, size_t end_v,
                              const std::vector<uint16_t> &indices) {
  if (start_v >= end_v || end_v > verts.size())
    return;
  std::vector<float> acc_x(end_v - start_v, 0.0f);
  std::vector<float> acc_y(end_v - start_v, 0.0f);
  std::vector<float> acc_z(end_v - start_v, 0.0f);

  for (size_t k = 0; k + 2u < indices.size(); k += 3u) {
    const uint16_t i0 = indices[k];
    const uint16_t i1 = indices[k + 1u];
    const uint16_t i2 = indices[k + 2u];
    if (i0 < start_v || i0 >= end_v || i1 < start_v || i1 >= end_v ||
        i2 < start_v || i2 >= end_v)
      continue;
    const auto &v0 = verts[i0];
    const auto &v1 = verts[i1];
    const auto &v2 = verts[i2];
    const float ux = v1.x - v0.x, uy = v1.y - v0.y, uz = v1.z - v0.z;
    const float wx = v2.x - v0.x, wy = v2.y - v0.y, wz = v2.z - v0.z;
    const float fnx = uy * wz - uz * wy;
    const float fny = uz * wx - ux * wz;
    const float fnz = ux * wy - uy * wx;
    for (uint16_t vi : {i0, i1, i2}) {
      acc_x[vi - start_v] += fnx;
      acc_y[vi - start_v] += fny;
      acc_z[vi - start_v] += fnz;
    }
  }

  for (size_t vi = start_v; vi < end_v; ++vi) {
    const float sx = acc_x[vi - start_v];
    const float sy = acc_y[vi - start_v];
    const float sz = acc_z[vi - start_v];
    const float len = std::sqrt(sx * sx + sy * sy + sz * sz);
    if (len > 1e-6f) {
      verts[vi].nx = sx / len;
      verts[vi].ny = sy / len;
      verts[vi].nz = sz / len;
    }
  }
}

void filter_submesh(const std::vector<TeamColorVertex> &src_verts,
                    const std::vector<uint16_t> &src_indices, bool keep_hair,
                    uint16_t index_offset, std::vector<uint16_t> &out_indices) {
  out_indices.clear();
  out_indices.reserve(src_indices.size());
  for (size_t k = 0; k + 2u < src_indices.size(); k += 3u) {
    const uint16_t a = src_indices[k];
    const uint16_t b = src_indices[k + 1u];
    const uint16_t c = src_indices[k + 2u];
    if (a >= src_verts.size() || b >= src_verts.size() || c >= src_verts.size())
      continue;
    const bool tri_hair =
        src_verts[a].is_hair || src_verts[b].is_hair || src_verts[c].is_hair;
    if (tri_hair == keep_hair) {
      out_indices.push_back(static_cast<uint16_t>(a + index_offset));
      out_indices.push_back(static_cast<uint16_t>(b + index_offset));
      out_indices.push_back(static_cast<uint16_t>(c + index_offset));
    }
  }
}

uint16_t tint_rgb565(uint16_t c, float tr, float tg, float tb) {
  const float r = static_cast<float>((c >> 11u) & 31u) / 31.0f;
  const float g = static_cast<float>((c >> 5u) & 63u) / 63.0f;
  const float b = static_cast<float>(c & 31u) / 31.0f;
  const uint32_t nr = static_cast<uint32_t>(
      std::lround(std::clamp(r * tr, 0.0f, 1.0f) * 31.0f));
  const uint32_t ng = static_cast<uint32_t>(
      std::lround(std::clamp(g * tg, 0.0f, 1.0f) * 63.0f));
  const uint32_t nb = static_cast<uint32_t>(
      std::lround(std::clamp(b * tb, 0.0f, 1.0f) * 31.0f));
  return static_cast<uint16_t>((nr << 11u) | (ng << 5u) | nb);
}

// Tints BC1 (0x431, 8-byte blocks) or BC3 (0x433, 16-byte blocks with color
// endpoints at +8..+12) in-place in GX2 memory order, preserving 100% of GX2
// tiling, swizzling, and alpha channels!
void tint_gx2_dxt_blocks(uint8_t *data, uint32_t sz, uint32_t fmt, float tr,
                         float tg, float tb) {
  const bool is_bc1 = (fmt == 0x431u);
  const bool is_bc3 = (fmt == 0x433u);
  if (!is_bc1 && !is_bc3)
    return;
  const uint32_t blk_sz = is_bc1 ? 8u : 16u;
  const uint32_t col_off = is_bc1 ? 0u : 8u;
  for (uint32_t off = 0u; off + blk_sz <= sz; off += blk_sz) {
    uint8_t *cp = data + off + col_off;
    // Wii U BCn stores little-endian 16-bit RGB565 endpoints inside each GX2
    // block
    const uint16_t c0 =
        static_cast<uint16_t>(cp[0] | (static_cast<uint16_t>(cp[1]) << 8u));
    const uint16_t c1 =
        static_cast<uint16_t>(cp[2] | (static_cast<uint16_t>(cp[3]) << 8u));
    const uint16_t nc0 = tint_rgb565(c0, tr, tg, tb);
    const uint16_t nc1 = tint_rgb565(c1, tr, tg, tb);
    // Preserve c0 > c1 ordering flag for BC1 1-bit alpha semantics
    if (is_bc1 && (c0 <= c1) && (nc0 > nc1)) {
      cp[0] = static_cast<uint8_t>(nc1 & 0xFFu);
      cp[1] = static_cast<uint8_t>(nc1 >> 8u);
      cp[2] = static_cast<uint8_t>(nc0 & 0xFFu);
      cp[3] = static_cast<uint8_t>(nc0 >> 8u);
    } else {
      cp[0] = static_cast<uint8_t>(nc0 & 0xFFu);
      cp[1] = static_cast<uint8_t>(nc0 >> 8u);
      cp[2] = static_cast<uint8_t>(nc1 & 0xFFu);
      cp[3] = static_cast<uint8_t>(nc1 >> 8u);
    }
  }
}

bool apply_ftex_tint(std::vector<uint8_t> &bfres, const std::string &tex_name,
                     float tr, float tg, float tb) {
  const int32_t trel = rd_bes32(bfres.data() + 0x24u);
  if (!trel)
    return false;
  for (const auto &te :
       parse_bfres_dict(bfres, 0x24u + static_cast<size_t>(trel))) {
    if (te.name != tex_name)
      continue;
    const size_t to = te.data_off;
    if (to + 0xB8u > bfres.size())
      return false;
    const uint32_t fmt = rd_be32(bfres.data() + to + 24u);
    const uint32_t img_sz = rd_be32(bfres.data() + to + 36u);
    const uint32_t mip_sz = rd_be32(bfres.data() + to + 44u);
    const int32_t img_rel = rd_bes32(bfres.data() + to + 0xB0u);
    const int32_t mip_rel = rd_bes32(bfres.data() + to + 0xB4u);
    if (img_rel && img_sz) {
      const size_t ioff =
          static_cast<size_t>(static_cast<int64_t>(to + 0xB0u) + img_rel);
      if (ioff + img_sz <= bfres.size()) {
        tint_gx2_dxt_blocks(bfres.data() + ioff, img_sz, fmt, tr, tg, tb);
      }
    }
    if (mip_rel && mip_sz) {
      const size_t moff =
          static_cast<size_t>(static_cast<int64_t>(to + 0xB4u) + mip_rel);
      if (moff + mip_sz <= bfres.size()) {
        tint_gx2_dxt_blocks(bfres.data() + moff, mip_sz, fmt, tr, tg, tb);
      }
    }
    return true;
  }
  return false;
}

bool append_fmdl_teamcolor_buffers(
    std::vector<uint8_t> &bfres, const TeamColorShapeData &target_meta,
    const std::vector<TeamColorVertex> &verts,
    const std::array<std::vector<uint16_t>, 3> &lods, bool write_skin_weights) {
  if (verts.empty() || verts.size() > 65000u || lods[0].empty())
    return false;
  const uint32_t vtx_cnt = static_cast<uint32_t>(verts.size());
  const size_t voff = target_meta.fvtx_off;
  const size_t buf_arr =
      voff + 0x18u + static_cast<size_t>(rd_bes32(bfres.data() + voff + 0x18u));

  auto append_aligned = [&](const uint8_t *src, size_t len,
                            size_t align) -> size_t {
    const size_t off = align_up(bfres.size(), align);
    bfres.resize(off + len, 0u);
    std::memcpy(bfres.data() + off, src, len);
    return off;
  };

  // Pack all 8 buffers matching original strides
  std::vector<uint8_t> b_p0(vtx_cnt * 16u, 0u);
  std::vector<uint8_t> b_n0(vtx_cnt * 4u, 0u);
  std::vector<uint8_t> b_t0(vtx_cnt * 4u, 0u);
  std::vector<uint8_t> b_w0(vtx_cnt * 4u, 0u);
  std::vector<uint8_t> b_i0(vtx_cnt * 4u, 0u);
  std::vector<uint8_t> b_i0_1b(vtx_cnt, 20u);
  std::vector<uint8_t> b_u0(vtx_cnt * 4u, 0u);
  std::vector<uint8_t> b_u1(vtx_cnt * 4u, 0u);
  std::vector<uint8_t> b_u2(vtx_cnt * 4u, 0u);

  float min_x = 1e9f, max_x = -1e9f;
  float min_y = 1e9f, max_y = -1e9f;
  float min_z = 1e9f, max_z = -1e9f;

  for (uint32_t i = 0; i < vtx_cnt; ++i) {
    const auto &v = verts[i];
    wr_bef32(b_p0.data() + i * 16u + 0u, v.x);
    wr_bef32(b_p0.data() + i * 16u + 4u, v.y);
    wr_bef32(b_p0.data() + i * 16u + 8u, v.z);
    wr_be32(b_n0.data() + i * 4u, encode_snorm10_2(v.nx, v.ny, v.nz));
    std::memcpy(b_t0.data() + i * 4u, v.t0.data(), 4u);
    std::memcpy(b_w0.data() + i * 4u, v.w0.data(), 4u);
    std::memcpy(b_i0.data() + i * 4u, v.i0.data(), 4u);
    b_i0_1b[i] = v.i0[0];
    std::memcpy(b_u0.data() + i * 4u, v.u0.data(), 4u);
    std::memcpy(b_u1.data() + i * 4u, v.u1.data(), 4u);
    std::memcpy(b_u2.data() + i * 4u, v.u2.data(), 4u);
    min_x = std::min(min_x, v.x);
    max_x = std::max(max_x, v.x);
    min_y = std::min(min_y, v.y);
    max_y = std::max(max_y, v.y);
    min_z = std::min(min_z, v.z);
    max_z = std::max(max_z, v.z);
  }

  auto patch_vbuf = [&](int bi, const std::vector<uint8_t> &data) {
    if (bi < 0 || static_cast<uint32_t>(bi) >= target_meta.buf_count)
      return;
    const size_t bo = buf_arr + static_cast<size_t>(bi) * 0x18u;
    const size_t new_off = append_aligned(data.data(), data.size(), 64u);
    wr_be32(bfres.data() + bo + 4u, static_cast<uint32_t>(data.size()));
    wr_bes32(bfres.data() + bo + 20u,
             static_cast<int32_t>(static_cast<int64_t>(new_off) -
                                  static_cast<int64_t>(bo + 20u)));
  };

  wr_be32(bfres.data() + voff + 8u, vtx_cnt);
  patch_vbuf(0, b_p0);
  patch_vbuf(1, b_n0);
  patch_vbuf(2, b_t0);
  if (write_skin_weights && target_meta.w0_buf_idx >= 0)
    patch_vbuf(target_meta.w0_buf_idx, b_w0);
  if (target_meta.i0_buf_idx >= 0) {
    patch_vbuf(target_meta.i0_buf_idx,
               (target_meta.w0_buf_idx >= 0) ? b_i0 : b_i0_1b);
  }
  patch_vbuf(target_meta.u0_buf_idx, b_u0);
  patch_vbuf(target_meta.u1_buf_idx, b_u1);
  patch_vbuf(target_meta.u2_buf_idx, b_u2);

  // Patch all LOD index buffers & submesh counts on FSHP
  const size_t fshp_off = target_meta.fshp_off;
  const size_t lod_arr =
      fshp_off + 0x24u +
      static_cast<size_t>(rd_bes32(bfres.data() + fshp_off + 0x24u));
  for (uint32_t li = 0; li < target_meta.lod_count; ++li) {
    const auto &idx_vec = lods[li].empty() ? lods[0] : lods[li];
    const size_t lo = lod_arr + static_cast<size_t>(li) * 0x1Cu;
    const uint32_t new_icnt = static_cast<uint32_t>(idx_vec.size());
    std::vector<uint8_t> ib_bytes(idx_vec.size() * 2u, 0u);
    for (size_t k = 0; k < idx_vec.size(); ++k) {
      wr_be16(ib_bytes.data() + k * 2u, idx_vec[k]);
    }
    const size_t new_ioff =
        append_aligned(ib_bytes.data(), ib_bytes.size(), 64u);
    wr_be32(bfres.data() + lo + 8u, new_icnt);
    wr_be16(bfres.data() + lo + 12u, 1u);
    const int32_t sub_rel = rd_bes32(bfres.data() + lo + 16u);
    if (sub_rel != 0) {
      const size_t sub =
          static_cast<size_t>(static_cast<int64_t>(lo + 16u) + sub_rel);
      wr_be32(bfres.data() + sub + 0u, 0u);
      wr_be32(bfres.data() + sub + 4u, new_icnt);
    }
    const int32_t ib_rel = rd_bes32(bfres.data() + lo + 20u);
    const size_t ibo =
        static_cast<size_t>(static_cast<int64_t>(lo + 20u) + ib_rel);
    wr_be32(bfres.data() + ibo + 4u, static_cast<uint32_t>(ib_bytes.size()));
    wr_bes32(bfres.data() + ibo + 20u,
             static_cast<int32_t>(static_cast<int64_t>(new_ioff) -
                                  static_cast<int64_t>(ibo + 20u)));
  }

  // Update bounding radius generously so culling never clips stylized S2/S3
  // silhouettes
  const float cx = 0.5f * (min_x + max_x);
  const float cy = 0.5f * (min_y + max_y);
  const float cz = 0.5f * (min_z + max_z);
  const float rx = max_x - cx, ry = max_y - cy, rz = max_z - cz;
  const float rad =
      std::max(6.0f, std::sqrt(rx * rx + ry * ry + rz * rz) + 2.0f);
  wr_bef32(bfres.data() + fshp_off + 0x1Cu, rad);
  return true;
}

void patch_fmdl_name(std::vector<uint8_t> &bfres,
                     const std::string &target_name) {
  if (target_name.empty())
    return;
  const int32_t fmdl_grp_rel = rd_bes32(bfres.data() + 0x20u);
  if (!fmdl_grp_rel)
    return;
  const size_t grp = 0x20u + static_cast<size_t>(fmdl_grp_rel);
  const auto fmdls = parse_bfres_dict(bfres, grp);
  if (fmdls.empty())
    return;
  const size_t moff = fmdls[0].data_off;
  const std::string cur = rd_bfres_str(bfres, moff + 4u);
  if (cur == target_name)
    return;

  // Update FMDL ResDic radix-tree ref_bit on the 1-entry dictionary so name
  // lookups match.
  uint32_t msb = 0u;
  uint8_t last_ch = static_cast<uint8_t>(target_name.back());
  while (last_ch >>= 1u)
    ++msb;
  const uint32_t ref_bit =
      (static_cast<uint32_t>(target_name.size() - 1u) << 3u) | msb;
  if (grp + 8u + 16u + 4u <= bfres.size()) {
    wr_be32(bfres.data() + grp + 8u + 16u + 0u, ref_bit);
  }

  // Append BFRES string-table-style entry (+4 length prefix before
  // null-terminated UTF-8)
  const size_t str_rec = align_up(bfres.size(), 4u);
  bfres.resize(str_rec + 4u + target_name.size() + 1u, 0u);
  wr_be32(bfres.data() + str_rec, static_cast<uint32_t>(target_name.size()));
  std::memcpy(bfres.data() + str_rec + 4u, target_name.data(),
              target_name.size());
  const size_t str_off = str_rec + 4u;

  wr_bes32(bfres.data() + moff + 4u,
           static_cast<int32_t>(static_cast<int64_t>(str_off) -
                                static_cast<int64_t>(moff + 4u)));
  const size_t e_name_rel = grp + 8u + 16u + 8u;
  wr_bes32(bfres.data() + e_name_rel,
           static_cast<int32_t>(static_cast<int64_t>(str_off) -
                                static_cast<int64_t>(e_name_rel)));
}

size_t append_bfres_string(std::vector<uint8_t> &bfres, const std::string &s) {
  const size_t str_rec = align_up(bfres.size(), 4u);
  bfres.resize(str_rec + 4u + s.size() + 1u, 0u);
  wr_be32(bfres.data() + str_rec, static_cast<uint32_t>(s.size()));
  if (!s.empty()) {
    std::memcpy(bfres.data() + str_rec + 4u, s.data(), s.size());
  }
  return str_rec + 4u;
}

bool resdic_get_bit(const std::string &s, uint32_t ref_bit) {
  const size_t byte_idx = static_cast<size_t>(ref_bit >> 3u);
  if (byte_idx >= s.size())
    return false;
  return ((static_cast<uint8_t>(s[byte_idx]) >> (ref_bit & 7u)) & 1u) != 0u;
}

uint32_t resdic_highest_diff_bit(const std::string &a, const std::string &b) {
  const size_t max_bytes = std::max(a.size(), b.size());
  for (size_t i = max_bytes; i > 0u; --i) {
    const size_t idx = i - 1u;
    const uint8_t xa =
        (idx < a.size()) ? static_cast<uint8_t>(a[idx]) : static_cast<uint8_t>(0u);
    const uint8_t xb =
        (idx < b.size()) ? static_cast<uint8_t>(b[idx]) : static_cast<uint8_t>(0u);
    const uint8_t diff = static_cast<uint8_t>(xa ^ xb);
    if (diff != 0u) {
      for (int bit = 7; bit >= 0; --bit) {
        if ((diff & (1u << bit)) != 0u) {
          return (static_cast<uint32_t>(idx) << 3u) |
                 static_cast<uint32_t>(bit);
        }
      }
    }
  }
  return 0u;
}

bool lookup_bfres_resdic(const std::vector<uint8_t> &bfres, size_t grp_off,
                         const std::string &name,
                         size_t *out_data_off = nullptr) {
  if (grp_off == 0u || grp_off + 8u > bfres.size() || name.empty())
    return false;
  const uint32_t cnt = rd_be32(bfres.data() + grp_off + 4u);
  if (cnt == 0u || cnt > 4096u ||
      grp_off + 8u + static_cast<size_t>(cnt + 1u) * 16u > bfres.size())
    return false;

  uint32_t prev_ref = 0xFFFFFFFFu;
  uint16_t cur = rd_be16(bfres.data() + grp_off + 8u + 4u);
  for (uint32_t step = 0u; step <= cnt + 2u; ++step) {
    if (cur == 0u || cur > cnt)
      return false;
    const size_t eoff = grp_off + 8u + static_cast<size_t>(cur) * 16u;
    const uint32_t cur_ref = rd_be32(bfres.data() + eoff);
    if (cur_ref >= prev_ref)
      break;
    prev_ref = cur_ref;
    const uint16_t left = rd_be16(bfres.data() + eoff + 4u);
    const uint16_t right = rd_be16(bfres.data() + eoff + 6u);
    cur = resdic_get_bit(name, cur_ref) ? right : left;
  }
  if (cur == 0u || cur > cnt)
    return false;
  const size_t eoff = grp_off + 8u + static_cast<size_t>(cur) * 16u;
  if (rd_bfres_str(bfres, eoff + 8u) != name)
    return false;
  if (out_data_off) {
    const int32_t drel = rd_bes32(bfres.data() + eoff + 12u);
    if (drel == 0)
      return false;
    const int64_t abs = static_cast<int64_t>(eoff + 12u) + drel;
    if (abs <= 0 || static_cast<size_t>(abs) >= bfres.size())
      return false;
    *out_data_off = static_cast<size_t>(abs);
  }
  return true;
}

size_t build_bfres_resdic(std::vector<uint8_t> &bfres,
                          const std::vector<BfresDictEntry> &entries) {
  struct TrieNode {
    uint32_t ref_bit = 0u;
    uint16_t left = 0u;
    uint16_t right = 0u;
    std::string name;
    size_t name_str_off = 0u;
    size_t data_off = 0u;
  };
  const uint32_t cnt = static_cast<uint32_t>(entries.size());
  std::vector<TrieNode> nodes(static_cast<size_t>(cnt) + 1u);
  nodes[0].ref_bit = 0xFFFFFFFFu;
  nodes[0].left = 0u;
  nodes[0].right = 0u;

  for (uint32_t i = 0u; i < cnt; ++i) {
    const uint16_t idx = static_cast<uint16_t>(i + 1u);
    const std::string &nm = entries[i].name;
    nodes[idx].name = nm;
    nodes[idx].name_str_off = append_bfres_string(bfres, nm);
    nodes[idx].data_off = entries[i].data_off;

    uint16_t prev = 0u;
    uint16_t cur = nodes[0].left;
    while (cur != 0u && nodes[cur].ref_bit < nodes[prev].ref_bit) {
      prev = cur;
      cur = resdic_get_bit(nm, nodes[cur].ref_bit) ? nodes[cur].right
                                                   : nodes[cur].left;
    }
    const uint32_t ref_bit = resdic_highest_diff_bit(nm, nodes[cur].name);
    nodes[idx].ref_bit = ref_bit;

    prev = 0u;
    cur = nodes[0].left;
    bool went_right = false;
    while (cur != 0u && nodes[cur].ref_bit < nodes[prev].ref_bit &&
           nodes[cur].ref_bit > ref_bit) {
      prev = cur;
      went_right = resdic_get_bit(nm, nodes[cur].ref_bit);
      cur = went_right ? nodes[cur].right : nodes[cur].left;
    }
    const bool new_bit = resdic_get_bit(nm, ref_bit);
    nodes[idx].left = new_bit ? cur : idx;
    nodes[idx].right = new_bit ? idx : cur;
    if (went_right)
      nodes[prev].right = idx;
    else
      nodes[prev].left = idx;
  }

  const size_t grp_off = align_up(bfres.size(), 4u);
  const uint32_t dict_size = 8u + (cnt + 1u) * 16u;
  bfres.resize(grp_off + dict_size, 0u);
  wr_be32(bfres.data() + grp_off + 0u, dict_size);
  wr_be32(bfres.data() + grp_off + 4u, cnt);
  for (uint32_t i = 0u; i <= cnt; ++i) {
    const size_t eoff = grp_off + 8u + static_cast<size_t>(i) * 16u;
    wr_be32(bfres.data() + eoff + 0u, nodes[i].ref_bit);
    wr_be16(bfres.data() + eoff + 4u, nodes[i].left);
    wr_be16(bfres.data() + eoff + 6u, nodes[i].right);
    if (i > 0u) {
      if (nodes[i].name_str_off != 0u) {
        wr_bes32(bfres.data() + eoff + 8u,
                 static_cast<int32_t>(static_cast<int64_t>(nodes[i].name_str_off) -
                                      static_cast<int64_t>(eoff + 8u)));
      }
      if (nodes[i].data_off != 0u) {
        wr_bes32(bfres.data() + eoff + 12u,
                 static_cast<int32_t>(static_cast<int64_t>(nodes[i].data_off) -
                                      static_cast<int64_t>(eoff + 12u)));
      }
    }
  }
  return grp_off;
}

bool clone_source_teamcolor_material(std::vector<uint8_t> &base_bfres,
                                     const std::vector<uint8_t> &src_bfres,
                                     uint16_t &out_hair_mat_idx) {
  if (src_bfres.size() < 0x68u || base_bfres.size() < 0x68u)
    return false;

  const int32_t src_ftex_rel = rd_bes32(src_bfres.data() + 0x24u);
  const int32_t base_ftex_rel = rd_bes32(base_bfres.data() + 0x24u);
  if (src_ftex_rel == 0 || base_ftex_rel == 0)
    return false;

  std::vector<BfresDictEntry> ftex_entries =
      parse_bfres_dict(base_bfres, 0x24u + static_cast<size_t>(base_ftex_rel));
  std::unordered_map<std::string, std::string> suf_to_cloned_tex;
  std::unordered_map<std::string, size_t> cloned_tex_str_off;

  const auto src_ftex_entries =
      parse_bfres_dict(src_bfres, 0x24u + static_cast<size_t>(src_ftex_rel));
  for (const auto &se : src_ftex_entries) {
    static constexpr const char kTeamPrefix[] = "M_TeamColor_";
    static constexpr size_t kPrefixLen = sizeof(kTeamPrefix) - 1u;
    if (se.name.rfind(kTeamPrefix, 0) != 0 || se.data_off == 0u ||
        se.data_off + 0xC0u > src_bfres.size())
      continue;

    const std::string suf = se.name.substr(kPrefixLen);
    const std::string new_tex_name = std::string("HairSrc_") + suf;

    const uint32_t img_sz = rd_be32(src_bfres.data() + se.data_off + 0x24u);
    const uint32_t mip_sz = rd_be32(src_bfres.data() + se.data_off + 0x2Cu);
    uint32_t raw_align = rd_be32(src_bfres.data() + se.data_off + 0x3Cu);
    if (raw_align < 0x1000u || raw_align > 0x10000u ||
        (raw_align & (raw_align - 1u)) != 0u) {
      raw_align = 0x1000u;
    }
    const int32_t src_img_rel =
        rd_bes32(src_bfres.data() + se.data_off + 0xB0u);
    const int64_t src_img_off =
        static_cast<int64_t>(se.data_off + 0xB0u) + src_img_rel;
    if (src_img_rel == 0 || src_img_off < 0 ||
        static_cast<size_t>(src_img_off) + img_sz > src_bfres.size())
      return false;

    size_t src_mip_off = 0u;
    if (mip_sz > 0u) {
      const int32_t src_mip_rel =
          rd_bes32(src_bfres.data() + se.data_off + 0xB4u);
      const int64_t abs_mip =
          static_cast<int64_t>(se.data_off + 0xB4u) + src_mip_rel;
      if (src_mip_rel == 0 || abs_mip < 0 ||
          static_cast<size_t>(abs_mip) + mip_sz > src_bfres.size())
        return false;
      src_mip_off = static_cast<size_t>(abs_mip);
    }

    const std::string path_str =
        rd_bfres_str(src_bfres, se.data_off + 0xACu);
    const size_t new_name_off = append_bfres_string(base_bfres, new_tex_name);
    const size_t new_path_off =
        path_str.empty() ? 0u : append_bfres_string(base_bfres, path_str);

    const size_t new_ftex_off = align_up(base_bfres.size(), 4u);
    base_bfres.resize(new_ftex_off + 0xC0u, 0u);
    std::memcpy(base_bfres.data() + new_ftex_off,
                src_bfres.data() + se.data_off, 0xC0u);

    const size_t new_img_off = align_up(base_bfres.size(), raw_align);
    base_bfres.resize(new_img_off + img_sz, 0u);
    std::memcpy(base_bfres.data() + new_img_off,
                src_bfres.data() + static_cast<size_t>(src_img_off), img_sz);

    size_t new_mip_off = 0u;
    if (mip_sz > 0u) {
      new_mip_off = align_up(base_bfres.size(), raw_align);
      base_bfres.resize(new_mip_off + mip_sz, 0u);
      std::memcpy(base_bfres.data() + new_mip_off,
                  src_bfres.data() + src_mip_off, mip_sz);
    }

    wr_bes32(base_bfres.data() + new_ftex_off + 0xA8u,
             static_cast<int32_t>(static_cast<int64_t>(new_name_off) -
                                  static_cast<int64_t>(new_ftex_off + 0xA8u)));
    wr_bes32(base_bfres.data() + new_ftex_off + 0xACu,
             new_path_off == 0u
                 ? 0
                 : static_cast<int32_t>(
                       static_cast<int64_t>(new_path_off) -
                       static_cast<int64_t>(new_ftex_off + 0xACu)));
    wr_bes32(base_bfres.data() + new_ftex_off + 0xB0u,
             static_cast<int32_t>(static_cast<int64_t>(new_img_off) -
                                  static_cast<int64_t>(new_ftex_off + 0xB0u)));
    wr_bes32(base_bfres.data() + new_ftex_off + 0xB4u,
             mip_sz == 0u
                 ? 0
                 : static_cast<int32_t>(
                       static_cast<int64_t>(new_mip_off) -
                       static_cast<int64_t>(new_ftex_off + 0xB4u)));
    wr_be32(base_bfres.data() + new_ftex_off + 0xB8u, 0u);
    wr_be32(base_bfres.data() + new_ftex_off + 0xBCu, 0u);

    ftex_entries.push_back({new_tex_name, new_ftex_off});
    suf_to_cloned_tex[suf] = new_tex_name;
    cloned_tex_str_off[new_tex_name] = new_name_off;
  }

  if (suf_to_cloned_tex.empty())
    return false;

  const size_t new_ftex_grp = build_bfres_resdic(base_bfres, ftex_entries);
  wr_bes32(base_bfres.data() + 0x24u,
           static_cast<int32_t>(static_cast<int64_t>(new_ftex_grp) - 0x24));
  wr_be16(base_bfres.data() + 0x52u, static_cast<uint16_t>(ftex_entries.size()));

  const int32_t fmdl_grp_rel = rd_bes32(base_bfres.data() + 0x20u);
  const auto fmdls =
      parse_bfres_dict(base_bfres, 0x20u + static_cast<size_t>(fmdl_grp_rel));
  if (fmdls.empty())
    return false;
  const size_t moff = fmdls[0].data_off;
  const size_t old_fmat_grp =
      moff + 0x18u + static_cast<size_t>(rd_bes32(base_bfres.data() + moff + 0x18u));
  std::vector<BfresDictEntry> fmat_entries =
      parse_bfres_dict(base_bfres, old_fmat_grp);
  size_t old_fmat = 0u;
  for (const auto &me : fmat_entries) {
    if (me.name == "M_TeamColor") {
      old_fmat = me.data_off;
      break;
    }
  }
  if (old_fmat == 0u || old_fmat + 0x4Cu > base_bfres.size())
    return false;

  const size_t hair_fmat_name_off =
      append_bfres_string(base_bfres, "M_TeamColor_Hair");

  const uint8_t tex_ref_cnt = base_bfres[old_fmat + 0x11u];
  const int32_t old_tarr_rel = rd_bes32(base_bfres.data() + old_fmat + 0x28u);
  size_t new_tarr = 0u;
  if (tex_ref_cnt > 0u && old_tarr_rel != 0) {
    const size_t old_tarr = static_cast<size_t>(
        static_cast<int64_t>(old_fmat + 0x28u) + old_tarr_rel);
    new_tarr = align_up(base_bfres.size(), 4u);
    base_bfres.resize(new_tarr + static_cast<size_t>(tex_ref_cnt) * 8u, 0u);
    for (uint8_t ti = 0u; ti < tex_ref_cnt; ++ti) {
      const size_t old_slot = old_tarr + static_cast<size_t>(ti) * 8u;
      const size_t new_slot = new_tarr + static_cast<size_t>(ti) * 8u;
      const int32_t old_srel = rd_bes32(base_bfres.data() + old_slot);
      size_t target_str_off = 0u;
      if (old_srel != 0) {
        const std::string orig_ref = rd_bfres_str(base_bfres, old_slot);
        target_str_off =
            static_cast<size_t>(static_cast<int64_t>(old_slot) + old_srel);
        static constexpr const char kTeamPrefix[] = "M_TeamColor_";
        if (orig_ref.rfind(kTeamPrefix, 0) == 0) {
          const auto it = suf_to_cloned_tex.find(orig_ref.substr(12));
          if (it != suf_to_cloned_tex.end()) {
            target_str_off = cloned_tex_str_off[it->second];
          }
        }
      }
      if (target_str_off != 0u) {
        wr_bes32(base_bfres.data() + new_slot,
                 static_cast<int32_t>(static_cast<int64_t>(target_str_off) -
                                      static_cast<int64_t>(new_slot)));
      }
      wr_be32(base_bfres.data() + new_slot + 4u, 0u);
    }
  }

  const size_t new_fmat = align_up(base_bfres.size(), 4u);
  base_bfres.resize(new_fmat + 0x4Cu, 0u);
  std::memcpy(base_bfres.data() + new_fmat, base_bfres.data() + old_fmat, 0x4Cu);
  wr_bes32(base_bfres.data() + new_fmat + 0x04u,
           static_cast<int32_t>(static_cast<int64_t>(hair_fmat_name_off) -
                                static_cast<int64_t>(new_fmat + 0x04u)));
  wr_be16(base_bfres.data() + new_fmat + 0x0Cu,
          static_cast<uint16_t>(fmat_entries.size()));
  wr_bes32(base_bfres.data() + new_fmat + 0x28u,
           new_tarr == 0u
               ? 0
               : static_cast<int32_t>(static_cast<int64_t>(new_tarr) -
                                      static_cast<int64_t>(new_fmat + 0x28u)));

  static constexpr size_t kFmatRelOffsets[] = {
      0x1Cu, 0x20u, 0x24u, 0x2Cu, 0x30u, 0x34u, 0x38u, 0x3Cu, 0x40u, 0x44u, 0x48u};
  const int64_t fmat_delta =
      static_cast<int64_t>(old_fmat) - static_cast<int64_t>(new_fmat);
  for (size_t off : kFmatRelOffsets) {
    const int32_t r = rd_bes32(base_bfres.data() + old_fmat + off);
    if (r != 0) {
      wr_bes32(base_bfres.data() + new_fmat + off,
               static_cast<int32_t>(static_cast<int64_t>(r) + fmat_delta));
    }
  }

  out_hair_mat_idx = static_cast<uint16_t>(fmat_entries.size());
  fmat_entries.push_back({"M_TeamColor_Hair", new_fmat});
  const size_t new_fmat_grp = build_bfres_resdic(base_bfres, fmat_entries);
  wr_bes32(base_bfres.data() + moff + 0x18u,
           static_cast<int32_t>(static_cast<int64_t>(new_fmat_grp) -
                                static_cast<int64_t>(moff + 0x18u)));
  wr_be16(base_bfres.data() + moff + 0x24u,
          static_cast<uint16_t>(fmat_entries.size()));
  return true;
}

bool fix_rival_eye_and_eyelid_materials(std::vector<uint8_t> &bfres) {
  if (bfres.size() < 0x50u || std::memcmp(bfres.data(), "FRES", 4) != 0)
    return false;
  const int32_t fmdl_grp_rel = rd_bes32(bfres.data() + 0x20u);
  if (fmdl_grp_rel == 0)
    return false;
  const auto fmdls =
      parse_bfres_dict(bfres, 0x20u + static_cast<size_t>(fmdl_grp_rel));
  if (fmdls.empty())
    return false;
  const size_t moff = fmdls[0].data_off;
  const int32_t fmat_grp_rel = rd_bes32(bfres.data() + moff + 0x18u);
  if (fmat_grp_rel == 0)
    return false;
  const size_t fmat_grp = moff + 0x18u + static_cast<size_t>(fmat_grp_rel);
  const auto fmat_entries = parse_bfres_dict(bfres, fmat_grp);

  static constexpr uint8_t kEyeOpaRenderState[0x2Cu] = {
      0x00, 0x00, 0x00, 0x01, 0x00, 0x28, 0x02, 0x42, 0x49, 0x74, 0x97,
      0x36, 0x00, 0x00, 0x00, 0x06, 0x3f, 0x00, 0x00, 0x00, 0x00, 0xcc,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x01, 0x05, 0x04, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  static constexpr uint8_t kEyelidMaskRenderState[0x2Cu] = {
      0x00, 0x00, 0x00, 0x02, 0x00, 0x28, 0x02, 0x42, 0x49, 0x74, 0x97,
      0x36, 0x00, 0x00, 0x00, 0x0e, 0x3f, 0x00, 0x00, 0x00, 0x00, 0xcc,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x01, 0x05, 0x04, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

  bool patched_eye = false;
  bool patched_eyelid = false;
  for (const auto &me : fmat_entries) {
    if (me.name != "M_Eye" && me.name != "M_Eyelid")
      continue;
    const bool is_eye = (me.name == "M_Eye");
    const size_t mo = me.data_off;
    if (mo == 0u || mo + 0x4Cu > bfres.size())
      return false;

    const int32_t sa_rel = rd_bes32(bfres.data() + mo + 0x24u);
    if (sa_rel == 0)
      return false;
    const size_t sa =
        static_cast<size_t>(static_cast<int64_t>(mo + 0x24u) + sa_rel);
    if (sa + 0x1Cu > bfres.size())
      return false;

    if (rd_bfres_str(bfres, sa + 0x04u) == "gambit_uber_xlu") {
      const size_t uber_off = append_bfres_string(bfres, "gambit_uber");
      wr_bes32(bfres.data() + sa + 0x04u,
               static_cast<int32_t>(static_cast<int64_t>(uber_off) -
                                    static_cast<int64_t>(sa + 0x04u)));

      const int32_t opt_rel = rd_bes32(bfres.data() + sa + 0x18u);
      if (opt_rel == 0)
        return false;
      const size_t old_opt_dict =
          static_cast<size_t>(static_cast<int64_t>(sa + 0x18u) + opt_rel);

      const size_t val_1 = append_bfres_string(bfres, "1");
      const size_t val_0 = append_bfres_string(bfres, "0");
      const size_t val_neg1 = append_bfres_string(bfres, "-1");

      std::vector<BfresDictEntry> new_opts;
      for (const auto &oe : parse_bfres_dict(bfres, old_opt_dict)) {
        if (oe.name == "enable_xlu_alpha_out" ||
            oe.name == "enable_xlu_spec" ||
            oe.name == "enable_color_buffer_blend") {
          continue;
        }
        BfresDictEntry kept = oe;
        if (oe.name == "enable_shadow_pre_pass") {
          kept.data_off = val_1;
        } else if (is_eye) {
          if (oe.name == "gsys_enable_area_env")
            kept.data_off = val_0;
        } else {
          if (oe.name == "bake_shadow_type")
            kept.data_off = val_neg1;
          else if (oe.name == "gambit_paint_indirect_illumination" ||
                   oe.name == "multi_tex_calc_type_color")
            kept.data_off = val_0;
        }
        new_opts.push_back(std::move(kept));
      }
      const size_t new_opt_dict = build_bfres_resdic(bfres, new_opts);
      wr_be16(bfres.data() + sa + 0x0Eu,
              static_cast<uint16_t>(new_opts.size()));
      wr_bes32(bfres.data() + sa + 0x18u,
               static_cast<int32_t>(static_cast<int64_t>(new_opt_dict) -
                                    static_cast<int64_t>(sa + 0x18u)));
    }

    const int32_t rs_rel = rd_bes32(bfres.data() + mo + 0x20u);
    if (rs_rel == 0)
      return false;
    const size_t rs =
        static_cast<size_t>(static_cast<int64_t>(mo + 0x20u) + rs_rel);
    if (rs + 0x2Cu > bfres.size())
      return false;
    std::memcpy(bfres.data() + rs,
                is_eye ? kEyeOpaRenderState : kEyelidMaskRenderState, 0x2Cu);

    const int32_t ri_rel = rd_bes32(bfres.data() + mo + 0x1Cu);
    if (ri_rel != 0) {
      const size_t ri_dict =
          static_cast<size_t>(static_cast<int64_t>(mo + 0x1Cu) + ri_rel);
      const size_t none_off = is_eye ? append_bfres_string(bfres, "none") : 0u;
      for (const auto &re : parse_bfres_dict(bfres, ri_dict)) {
        if (re.data_off == 0u || re.data_off + 12u > bfres.size())
          continue;
        if (is_eye && re.name == "gsys_priority_hint") {
          wr_bes32(bfres.data() + re.data_off + 8u,
                   static_cast<int32_t>(static_cast<int64_t>(none_off) -
                                        static_cast<int64_t>(re.data_off + 8u)));
        } else if (!is_eye && re.name == "gsys_priority") {
          wr_be32(bfres.data() + re.data_off + 8u, 1u);
        }
      }
    }

    const uint16_t pcnt = rd_be16(bfres.data() + mo + 0x12u);
    const int32_t parr_rel = rd_bes32(bfres.data() + mo + 0x34u);
    const int32_t pdata_rel = rd_bes32(bfres.data() + mo + 0x3Cu);
    if (parr_rel != 0 && pdata_rel != 0) {
      const size_t parr =
          static_cast<size_t>(static_cast<int64_t>(mo + 0x34u) + parr_rel);
      const size_t pdata =
          static_cast<size_t>(static_cast<int64_t>(mo + 0x3Cu) + pdata_rel);
      for (uint16_t pi = 0u; pi < pcnt; ++pi) {
        const size_t po = parr + static_cast<size_t>(pi) * 0x14u;
        if (po + 0x14u > bfres.size())
          break;
        const uint8_t psz = bfres[po + 1u];
        const uint16_t poff = rd_be16(bfres.data() + po + 2u);
        const std::string pnm = rd_bfres_str(bfres, po + 16u);
        if (pdata + poff + psz > bfres.size())
          continue;
        if (is_eye) {
          if (pnm == "specular_color" && psz >= 12u) {
            wr_bef32(bfres.data() + pdata + poff + 0u, 1.0f);
            wr_bef32(bfres.data() + pdata + poff + 4u, 1.0f);
            wr_bef32(bfres.data() + pdata + poff + 8u, 1.0f);
          } else if (pnm == "specular_intensity" && psz >= 4u) {
            wr_bef32(bfres.data() + pdata + poff, 0.015f);
          } else if (pnm == "specular_roughness" && psz >= 4u) {
            wr_bef32(bfres.data() + pdata + poff, 0.2f);
          } else if (pnm == "light_fresnel_power" && psz >= 4u) {
            wr_bef32(bfres.data() + pdata + poff, 0.5f);
          }
        } else {
          if (pnm == "specular_color" && psz >= 12u) {
            wr_bef32(bfres.data() + pdata + poff + 0u, 1.0f);
            wr_bef32(bfres.data() + pdata + poff + 4u, 1.0f);
            wr_bef32(bfres.data() + pdata + poff + 8u, 1.0f);
          }
        }
      }
    }

    if (is_eye)
      patched_eye = true;
    else
      patched_eyelid = true;
  }
  return patched_eye && patched_eyelid;
}

bool add_cloned_hair_fshp_and_fvtx(std::vector<uint8_t> &bfres,
                                   TeamColorShapeData &base_shape,
                                   uint16_t hair_mat_idx,
                                   TeamColorShapeData &out_hair_meta) {
  const int32_t fmdl_grp_rel = rd_bes32(bfres.data() + 0x20u);
  const auto fmdls =
      parse_bfres_dict(bfres, 0x20u + static_cast<size_t>(fmdl_grp_rel));
  if (fmdls.empty())
    return false;
  const size_t moff = fmdls[0].data_off;
  const uint16_t old_fvtx_cnt = rd_be16(bfres.data() + moff + 0x20u);
  const uint16_t old_fshp_cnt = rd_be16(bfres.data() + moff + 0x22u);
  const size_t old_fvtx_arr =
      moff + 0x10u + static_cast<size_t>(rd_bes32(bfres.data() + moff + 0x10u));
  const size_t old_fshp_grp =
      moff + 0x14u + static_cast<size_t>(rd_bes32(bfres.data() + moff + 0x14u));

  const size_t new_fvtx_arr = align_up(bfres.size(), 4u);
  bfres.resize(new_fvtx_arr + static_cast<size_t>(old_fvtx_cnt + 1u) * 0x20u,
               0u);
  std::memcpy(bfres.data() + new_fvtx_arr, bfres.data() + old_fvtx_arr,
              static_cast<size_t>(old_fvtx_cnt) * 0x20u);

  const int64_t fvtx_arr_delta =
      static_cast<int64_t>(old_fvtx_arr) - static_cast<int64_t>(new_fvtx_arr);
  for (uint16_t vi = 0u; vi < old_fvtx_cnt; ++vi) {
    const size_t vo = new_fvtx_arr + static_cast<size_t>(vi) * 0x20u;
    for (size_t off : {size_t{0x10u}, size_t{0x14u}, size_t{0x18u}}) {
      const int32_t r = rd_bes32(bfres.data() + vo + off);
      if (r != 0) {
        wr_bes32(bfres.data() + vo + off,
                 static_cast<int32_t>(static_cast<int64_t>(r) + fvtx_arr_delta));
      }
    }
  }

  // Clone base_shape FVTX metadata into new slot old_fvtx_cnt with its own buf_arr
  const size_t src_voff = base_shape.fvtx_off;
  const size_t new_voff =
      new_fvtx_arr + static_cast<size_t>(old_fvtx_cnt) * 0x20u;
  std::memcpy(bfres.data() + new_voff, bfres.data() + src_voff, 0x20u);
  wr_be16(bfres.data() + new_voff + 0x06u, old_fvtx_cnt);

  const int64_t src_to_new_vtx_delta =
      static_cast<int64_t>(src_voff) - static_cast<int64_t>(new_voff);
  for (size_t off : {size_t{0x10u}, size_t{0x14u}}) {
    const int32_t r = rd_bes32(bfres.data() + src_voff + off);
    if (r != 0) {
      wr_bes32(bfres.data() + new_voff + off,
               static_cast<int32_t>(static_cast<int64_t>(r) +
                                    src_to_new_vtx_delta));
    }
  }

  const size_t old_buf_arr = static_cast<size_t>(
      static_cast<int64_t>(src_voff + 0x18u) +
      rd_bes32(bfres.data() + src_voff + 0x18u));
  const size_t new_buf_arr = align_up(bfres.size(), 4u);
  bfres.resize(new_buf_arr +
                   static_cast<size_t>(base_shape.buf_count) * 0x18u,
               0u);
  std::memcpy(bfres.data() + new_buf_arr, bfres.data() + old_buf_arr,
              static_cast<size_t>(base_shape.buf_count) * 0x18u);
  const int64_t buf_arr_delta =
      static_cast<int64_t>(old_buf_arr) - static_cast<int64_t>(new_buf_arr);
  for (uint32_t bi = 0u; bi < base_shape.buf_count; ++bi) {
    const size_t bo = new_buf_arr + static_cast<size_t>(bi) * 0x18u;
    const int32_t r = rd_bes32(bfres.data() + bo + 20u);
    if (r != 0) {
      wr_bes32(bfres.data() + bo + 20u,
               static_cast<int32_t>(static_cast<int64_t>(r) + buf_arr_delta));
    }
  }
  wr_bes32(bfres.data() + new_voff + 0x18u,
           static_cast<int32_t>(static_cast<int64_t>(new_buf_arr) -
                                static_cast<int64_t>(new_voff + 0x18u)));

  // Update FMDL FVTX array pointer & count, plus base_shape.fvtx_off & existing FSHP links
  wr_bes32(bfres.data() + moff + 0x10u,
           static_cast<int32_t>(static_cast<int64_t>(new_fvtx_arr) -
                                static_cast<int64_t>(moff + 0x10u)));
  wr_be16(bfres.data() + moff + 0x20u, static_cast<uint16_t>(old_fvtx_cnt + 1u));
  const uint16_t base_vtx_idx = rd_be16(bfres.data() + base_shape.fshp_off + 0x12u);
  base_shape.fvtx_off =
      new_fvtx_arr + static_cast<size_t>(base_vtx_idx) * 0x20u;

  std::vector<BfresDictEntry> fshp_entries =
      parse_bfres_dict(bfres, old_fshp_grp);
  for (const auto &se : fshp_entries) {
    const uint16_t vidx = rd_be16(bfres.data() + se.data_off + 0x12u);
    const size_t target_vo = new_fvtx_arr + static_cast<size_t>(vidx) * 0x20u;
    wr_bes32(bfres.data() + se.data_off + 0x20u,
             static_cast<int32_t>(static_cast<int64_t>(target_vo) -
                                  static_cast<int64_t>(se.data_off + 0x20u)));
  }

  // Clone base_shape FSHP into hair__M_TeamColor with its own LOD / submesh / index buffers
  const size_t hair_fshp_name_off =
      append_bfres_string(bfres, "hair__M_TeamColor");
  const size_t src_so = base_shape.fshp_off;
  const uint8_t lod_cnt = bfres[src_so + 0x17u];
  const size_t old_lod_arr = static_cast<size_t>(
      static_cast<int64_t>(src_so + 0x24u) +
      rd_bes32(bfres.data() + src_so + 0x24u));
  const size_t new_lod_arr = align_up(bfres.size(), 4u);
  bfres.resize(new_lod_arr + static_cast<size_t>(lod_cnt) * 0x1Cu, 0u);
  std::memcpy(bfres.data() + new_lod_arr, bfres.data() + old_lod_arr,
              static_cast<size_t>(lod_cnt) * 0x1Cu);

  for (uint8_t li = 0u; li < lod_cnt; ++li) {
    const size_t old_lo = old_lod_arr + static_cast<size_t>(li) * 0x1Cu;
    const size_t new_lo = new_lod_arr + static_cast<size_t>(li) * 0x1Cu;

    const int32_t sub_rel = rd_bes32(bfres.data() + old_lo + 16u);
    if (sub_rel != 0) {
      const uint16_t sub_cnt = std::max<uint16_t>(1u, rd_be16(bfres.data() + old_lo + 12u));
      const size_t old_sub =
          static_cast<size_t>(static_cast<int64_t>(old_lo + 16u) + sub_rel);
      const size_t new_sub = align_up(bfres.size(), 4u);
      bfres.resize(new_sub + static_cast<size_t>(sub_cnt) * 8u, 0u);
      std::memcpy(bfres.data() + new_sub, bfres.data() + old_sub,
                  static_cast<size_t>(sub_cnt) * 8u);
      wr_bes32(bfres.data() + new_lo + 16u,
               static_cast<int32_t>(static_cast<int64_t>(new_sub) -
                                    static_cast<int64_t>(new_lo + 16u)));
    }

    const int32_t ib_rel = rd_bes32(bfres.data() + old_lo + 20u);
    if (ib_rel != 0) {
      const size_t old_ibo =
          static_cast<size_t>(static_cast<int64_t>(old_lo + 20u) + ib_rel);
      const size_t new_ibo = align_up(bfres.size(), 4u);
      bfres.resize(new_ibo + 0x18u, 0u);
      std::memcpy(bfres.data() + new_ibo, bfres.data() + old_ibo, 0x18u);
      const int32_t id_rel = rd_bes32(bfres.data() + old_ibo + 20u);
      if (id_rel != 0) {
        wr_bes32(bfres.data() + new_ibo + 20u,
                 static_cast<int32_t>(static_cast<int64_t>(id_rel) +
                                      (static_cast<int64_t>(old_ibo) -
                                       static_cast<int64_t>(new_ibo))));
      }
      wr_bes32(bfres.data() + new_lo + 20u,
               static_cast<int32_t>(static_cast<int64_t>(new_ibo) -
                                    static_cast<int64_t>(new_lo + 20u)));
    }
  }

  const size_t new_so = align_up(bfres.size(), 4u);
  bfres.resize(new_so + 0x38u, 0u);
  std::memcpy(bfres.data() + new_so, bfres.data() + src_so, 0x38u);
  wr_bes32(bfres.data() + new_so + 0x04u,
           static_cast<int32_t>(static_cast<int64_t>(hair_fshp_name_off) -
                                static_cast<int64_t>(new_so + 0x04u)));
  wr_be16(bfres.data() + new_so + 0x0Cu, old_fshp_cnt);
  wr_be16(bfres.data() + new_so + 0x0Eu, hair_mat_idx);
  wr_be16(bfres.data() + new_so + 0x12u, old_fvtx_cnt);
  wr_bes32(bfres.data() + new_so + 0x20u,
           static_cast<int32_t>(static_cast<int64_t>(new_voff) -
                                static_cast<int64_t>(new_so + 0x20u)));
  wr_bes32(bfres.data() + new_so + 0x24u,
           static_cast<int32_t>(static_cast<int64_t>(new_lod_arr) -
                                static_cast<int64_t>(new_so + 0x24u)));

  const int64_t fshp_delta =
      static_cast<int64_t>(src_so) - static_cast<int64_t>(new_so);
  for (size_t off : {size_t{0x28u}, size_t{0x2Cu}, size_t{0x30u}}) {
    const int32_t r = rd_bes32(bfres.data() + src_so + off);
    if (r != 0) {
      wr_bes32(bfres.data() + new_so + off,
               static_cast<int32_t>(static_cast<int64_t>(r) + fshp_delta));
    }
  }

  fshp_entries.push_back({"hair__M_TeamColor", new_so});
  const size_t new_fshp_grp = build_bfres_resdic(bfres, fshp_entries);
  wr_bes32(bfres.data() + moff + 0x14u,
           static_cast<int32_t>(static_cast<int64_t>(new_fshp_grp) -
                                static_cast<int64_t>(moff + 0x14u)));
  wr_be16(bfres.data() + moff + 0x22u,
          static_cast<uint16_t>(fshp_entries.size()));

  out_hair_meta = base_shape;
  out_hair_meta.fvtx_off = new_voff;
  out_hair_meta.fshp_off = new_so;
  return true;
}

void bake_hair_wait_pose(const std::vector<uint8_t> &src_bfres,
                         HairSourceKind src_kind, TeamColorShapeData &shape) {
  if (src_kind != HairSourceKind::BoyNative &&
      src_kind != HairSourceKind::GirlNative) {
    return;
  }
  if (src_bfres.size() < 0x40u || std::memcmp(src_bfres.data(), "FRES", 4) != 0)
    return;
  const int32_t fmdl_grp_rel = rd_bes32(src_bfres.data() + 0x20u);
  if (fmdl_grp_rel == 0)
    return;
  const auto fmdls =
      parse_bfres_dict(src_bfres, 0x20u + static_cast<size_t>(fmdl_grp_rel));
  if (fmdls.empty())
    return;

  const size_t moff = fmdls[0].data_off;
  const int32_t skl_rel = rd_bes32(src_bfres.data() + moff + 0x0Cu);
  if (skl_rel == 0)
    return;
  const size_t skl = moff + 0x0Cu + static_cast<size_t>(skl_rel);
  if (skl + 0x20u > src_bfres.size())
    return;

  const uint16_t bone_cnt = rd_be16(src_bfres.data() + skl + 0x08u);
  const uint16_t smooth_cnt = rd_be16(src_bfres.data() + skl + 0x0Au);
  const size_t barr =
      skl + 0x14u + static_cast<size_t>(rd_bes32(src_bfres.data() + skl + 0x14u));
  const size_t mtx_arr =
      skl + 0x18u + static_cast<size_t>(rd_bes32(src_bfres.data() + skl + 0x18u));
  const size_t inv_arr =
      skl + 0x1Cu + static_cast<size_t>(rd_bes32(src_bfres.data() + skl + 0x1Cu));
  if (barr + static_cast<size_t>(bone_cnt) * 0x40u > src_bfres.size() ||
      mtx_arr + static_cast<size_t>(smooth_cnt) * 2u > src_bfres.size() ||
      inv_arr + static_cast<size_t>(smooth_cnt) * 48u > src_bfres.size()) {
    return;
  }

  struct Mat4 {
    float m[4][4]{};
  };
  auto mul_mat4 = [](const Mat4 &a, const Mat4 &b) -> Mat4 {
    Mat4 out{};
    for (int i = 0; i < 4; ++i) {
      for (int k = 0; k < 4; ++k) {
        const float aik = a.m[i][k];
        for (int j = 0; j < 4; ++j)
          out.m[i][j] += aik * b.m[k][j];
      }
    }
    return out;
  };
  auto override_wait_rot = [&](const std::string &bn, float &rx, float &ry,
                               float &rz) {
    struct RotEntry {
      const char *name;
      float rx, ry, rz;
    };
    static constexpr RotEntry kBoyWait[] = {
        {"Hair2_L", -1.7105f, 0.0971f, 0.0f},
        {"Hair3_L", -0.0281f, 0.0f, 0.2092f},
        {"Hair4_L", -0.0281f, 0.0f, 0.1017f},
        {"Hair2_R", -1.0984f, -0.1070f, 0.0f},
        {"Hair3_R", -0.0912f, 0.0f, 0.2088f},
        {"Hair4_R", -0.0912f, 0.0f, 0.1218f},
    };
    static constexpr RotEntry kGirlWait[] = {
        {"Hair1_L", -0.1075f, 0.0f, 0.0338f},
        {"Hair2_L", 0.0496f, 0.0f, 0.0387f},
        {"Hair3_L", -0.0479f, -0.1114f, -0.0213f},
        {"Hair4_L", -0.0271f, -0.1700f, 0.2133f},
        {"Hair1_R", -3.2680f, 0.0f, -0.1185f},
        {"Hair2_R", -0.0563f, 0.0f, 0.2563f},
        {"Hair3_R", -0.0361f, 0.1558f, 0.2453f},
        {"Hair4_R", -0.0525f, 0.0561f, -0.0377f},
        {"nape1", 0.4113f, 0.0f, -0.0220f},
        {"nape2", 0.0630f, 0.0f, -0.0477f},
    };
    const RotEntry *tbl =
        (src_kind == HairSourceKind::BoyNative) ? kBoyWait : kGirlWait;
    const size_t cnt = (src_kind == HairSourceKind::BoyNative)
                           ? (sizeof(kBoyWait) / sizeof(kBoyWait[0]))
                           : (sizeof(kGirlWait) / sizeof(kGirlWait[0]));
    for (size_t i = 0; i < cnt; ++i) {
      if (bn == tbl[i].name) {
        rx = tbl[i].rx;
        ry = tbl[i].ry;
        rz = tbl[i].rz;
        return;
      }
    }
  };

  std::vector<Mat4> worlds(bone_cnt);
  for (uint16_t bi = 0; bi < bone_cnt; ++bi) {
    const size_t bo = barr + static_cast<size_t>(bi) * 0x40u;
    const std::string bname = rd_bfres_str(src_bfres, bo);
    const int16_t parent = static_cast<int16_t>(rd_be16(src_bfres.data() + bo + 6u));
    const float sx = rd_bef32(src_bfres.data() + bo + 20u);
    const float sy = rd_bef32(src_bfres.data() + bo + 24u);
    const float sz = rd_bef32(src_bfres.data() + bo + 28u);
    float rx = rd_bef32(src_bfres.data() + bo + 32u);
    float ry = rd_bef32(src_bfres.data() + bo + 36u);
    float rz = rd_bef32(src_bfres.data() + bo + 40u);
    const float tx = rd_bef32(src_bfres.data() + bo + 48u);
    const float ty = rd_bef32(src_bfres.data() + bo + 52u);
    const float tz = rd_bef32(src_bfres.data() + bo + 56u);
    override_wait_rot(bname, rx, ry, rz);

    const float cx = std::cos(rx), sx_ = std::sin(rx);
    const float cy = std::cos(ry), sy_ = std::sin(ry);
    const float cz = std::cos(rz), sz_ = std::sin(rz);
    Mat4 local{};
    local.m[0][0] = (cz * cy) * sx;
    local.m[0][1] = (cz * sy_ * sx_ - sz_ * cx) * sy;
    local.m[0][2] = (cz * sy_ * cx + sz_ * sx_) * sz;
    local.m[0][3] = tx;
    local.m[1][0] = (sz_ * cy) * sx;
    local.m[1][1] = (sz_ * sy_ * sx_ + cz * cx) * sy;
    local.m[1][2] = (sz_ * sy_ * cx - cz * sx_) * sz;
    local.m[1][3] = ty;
    local.m[2][0] = (-sy_) * sx;
    local.m[2][1] = (cy * sx_) * sy;
    local.m[2][2] = (cy * cx) * sz;
    local.m[2][3] = tz;
    local.m[3][3] = 1.0f;

    worlds[bi] = (parent >= 0 && parent < static_cast<int16_t>(bi))
                     ? mul_mat4(worlds[static_cast<size_t>(parent)], local)
                     : local;
  }

  std::vector<Mat4> skin_mats(smooth_cnt);
  for (uint16_t mi = 0; mi < smooth_cnt; ++mi) {
    const uint16_t bidx =
        rd_be16(src_bfres.data() + mtx_arr + static_cast<size_t>(mi) * 2u);
    if (bidx >= bone_cnt)
      return;
    const size_t ioff = inv_arr + static_cast<size_t>(mi) * 48u;
    Mat4 inv{};
    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 4; ++col) {
        inv.m[row][col] = rd_bef32(
            src_bfres.data() + ioff + static_cast<size_t>(row * 4 + col) * 4u);
      }
    }
    inv.m[3][3] = 1.0f;
    skin_mats[mi] = mul_mat4(worlds[bidx], inv);
  }

  const uint32_t chans =
      std::clamp<uint32_t>(shape.vtx_skin_count ? shape.vtx_skin_count : 1u,
                           1u, 4u);
  for (auto &v : shape.verts) {
    if (!v.is_hair)
      continue;
    Mat4 blend{};
    if (shape.has_w0) {
      uint32_t wsum = 0u;
      for (uint32_t k = 0; k < chans; ++k) {
        if (v.w0[k] > 0u && v.i0[k] < smooth_cnt)
          wsum += v.w0[k];
      }
      if (wsum == 0u)
        continue;
      const float inv_w = 1.0f / static_cast<float>(wsum);
      for (uint32_t k = 0; k < chans; ++k) {
        if (v.w0[k] == 0u || v.i0[k] >= smooth_cnt)
          continue;
        const float w = static_cast<float>(v.w0[k]) * inv_w;
        const Mat4 &sm = skin_mats[v.i0[k]];
        for (int r = 0; r < 4; ++r) {
          for (int c = 0; c < 4; ++c)
            blend.m[r][c] += w * sm.m[r][c];
        }
      }
    } else if (v.i0[0] < smooth_cnt) {
      blend = skin_mats[v.i0[0]];
    } else {
      continue;
    }

    const float px = blend.m[0][0] * v.x + blend.m[0][1] * v.y +
                     blend.m[0][2] * v.z + blend.m[0][3];
    const float py = blend.m[1][0] * v.x + blend.m[1][1] * v.y +
                     blend.m[1][2] * v.z + blend.m[1][3];
    const float pz = blend.m[2][0] * v.x + blend.m[2][1] * v.y +
                     blend.m[2][2] * v.z + blend.m[2][3];
    float nrx =
        blend.m[0][0] * v.nx + blend.m[0][1] * v.ny + blend.m[0][2] * v.nz;
    float nry =
        blend.m[1][0] * v.nx + blend.m[1][1] * v.ny + blend.m[1][2] * v.nz;
    float nrz =
        blend.m[2][0] * v.nx + blend.m[2][1] * v.ny + blend.m[2][2] * v.nz;
    const float nlen = std::sqrt(nrx * nrx + nry * nry + nrz * nrz);
    if (nlen > 1e-6f) {
      nrx /= nlen;
      nry /= nlen;
      nrz /= nlen;
    }
    if (std::isfinite(px) && std::isfinite(py) && std::isfinite(pz)) {
      v.x = px;
      v.y = py;
      v.z = pz;
    }
    if (std::isfinite(nrx) && std::isfinite(nry) && std::isfinite(nrz) &&
        nlen > 1e-6f) {
      v.nx = nrx;
      v.ny = nry;
      v.nz = nrz;
    }
  }
}

void extract_hair_vertices_only(
    const TeamColorShapeData &src, bool rebind_to_head, HairSculptPreset sculpt,
    const HairParamTransform &tf, std::vector<TeamColorVertex> &out_hair_verts,
    std::array<std::vector<uint16_t>, 3> &out_hair_lods) {
  out_hair_verts.clear();
  for (auto &l : out_hair_lods)
    l.clear();

  std::vector<int32_t> remap(src.verts.size(), -1);
  for (size_t i = 0; i < src.verts.size(); ++i) {
    if (!src.verts[i].is_hair)
      continue;
    remap[i] = static_cast<int32_t>(out_hair_verts.size());
    TeamColorVertex tv = src.verts[i];
    sculpt_hair_vertex(tv, sculpt, tf);
    if (rebind_to_head) {
      // Smooth matrix index 20 == bone 21 ('head') across Player00, Player01,
      // and Rival00!
      tv.i0 = {20u, 20u, 20u, 0u};
      tv.w0 = {255u, 0u, 0u, 0u};
    }
    out_hair_verts.push_back(tv);
  }

  for (uint32_t li = 0; li < 3u; ++li) {
    const auto &src_l = (li < src.lod_count && !src.lod_indices[li].empty())
                            ? src.lod_indices[li]
                            : src.lod_indices[0];
    for (size_t k = 0; k + 2u < src_l.size(); k += 3u) {
      const uint16_t a = src_l[k], b = src_l[k + 1u], c = src_l[k + 2u];
      if (a >= remap.size() || b >= remap.size() || c >= remap.size())
        continue;
      if (remap[a] >= 0 && remap[b] >= 0 && remap[c] >= 0) {
        out_hair_lods[li].push_back(static_cast<uint16_t>(remap[a]));
        out_hair_lods[li].push_back(static_cast<uint16_t>(remap[b]));
        out_hair_lods[li].push_back(static_cast<uint16_t>(remap[c]));
      }
    }
  }

  if (sculpt != HairSculptPreset::None && !out_hair_lods[0].empty()) {
    recompute_smooth_normals(out_hair_verts, 0u, out_hair_verts.size(),
                             out_hair_lods[0]);
  }
}

} // namespace

bool decompress_yaz0(const uint8_t *src, size_t len,
                     std::vector<uint8_t> &out) {
  if (!src || len < 16u || std::memcmp(src, "Yaz0", 4) != 0)
    return false;
  const uint32_t uncomp_size = rd_be32(src + 4u);
  if (uncomp_size == 0u || uncomp_size > 128u * 1024u * 1024u)
    return false;

  out.clear();
  out.reserve(uncomp_size);
  size_t pos = 16u;
  while (out.size() < uncomp_size && pos < len) {
    const uint8_t flags = src[pos++];
    for (int bit = 7; bit >= 0 && out.size() < uncomp_size && pos < len;
         --bit) {
      if ((flags >> bit) & 1u) {
        out.push_back(src[pos++]);
      } else {
        if (pos + 2u > len)
          return false;
        const uint8_t b1 = src[pos++];
        const uint8_t b2 = src[pos++];
        const uint32_t dist = (static_cast<uint32_t>(b1 & 0x0Fu) << 8u) | b2;
        if (dist + 1u > out.size())
          return false;
        size_t copy_pos = out.size() - (dist + 1u);
        uint32_t count = (static_cast<uint32_t>(b1) >> 4u) + 2u;
        if ((b1 >> 4u) == 0u) {
          if (pos >= len)
            return false;
          count = static_cast<uint32_t>(src[pos++]) + 0x12u;
        }
        for (uint32_t c = 0; c < count && out.size() < uncomp_size; ++c) {
          out.push_back(out[copy_pos++]);
        }
      }
    }
  }
  return out.size() == uncomp_size;
}

std::vector<uint8_t> compress_yaz0_fast(const uint8_t *src, size_t len) {
  std::vector<uint8_t> out;
  out.resize(16u, 0u);
  std::memcpy(out.data(), "Yaz0", 4);
  wr_be32(out.data() + 4u, static_cast<uint32_t>(len));

  // Fast literal+RLE/short-window Yaz0 encoder: keeps build under 40ms with
  // lossless round-trip!
  out.reserve(16u + len + (len / 8u) + 16u);
  size_t pos = 0;
  while (pos < len) {
    const size_t flag_off = out.size();
    out.push_back(0u);
    uint8_t flags = 0u;
    for (int bit = 7; bit >= 0 && pos < len; --bit) {
      // Check small lookback match on 4-byte aligned runs (great compression on
      // repeated zero/padding bytes)
      size_t best_len = 0u;
      size_t best_dist = 0u;
      if (pos >= 1u && len - pos >= 3u) {
        const size_t max_d = std::min<size_t>(pos, 256u);
        const size_t max_l = std::min<size_t>(len - pos, 273u);
        // Fast check dist=1 (RLE zeros) and dist=4/8/16
        for (size_t d : {size_t(1), size_t(4), size_t(8), size_t(16)}) {
          if (d > max_d)
            continue;
          size_t l = 0u;
          while (l < max_l && src[pos + l] == src[pos - d + l])
            ++l;
          if (l > best_len) {
            best_len = l;
            best_dist = d;
          }
        }
      }
      if (best_len >= 3u) {
        const uint32_t dist_m1 = static_cast<uint32_t>(best_dist - 1u);
        if (best_len <= 17u) {
          const uint8_t b1 = static_cast<uint8_t>(((best_len - 2u) << 4u) |
                                                  ((dist_m1 >> 8u) & 0x0Fu));
          const uint8_t b2 = static_cast<uint8_t>(dist_m1 & 0xFFu);
          out.push_back(b1);
          out.push_back(b2);
        } else {
          const uint8_t b1 = static_cast<uint8_t>((dist_m1 >> 8u) & 0x0Fu);
          const uint8_t b2 = static_cast<uint8_t>(dist_m1 & 0xFFu);
          const uint8_t b3 = static_cast<uint8_t>(best_len - 0x12u);
          out.push_back(b1);
          out.push_back(b2);
          out.push_back(b3);
        }
        pos += best_len;
      } else {
        flags |= static_cast<uint8_t>(1u << bit);
        out.push_back(src[pos++]);
      }
    }
    out[flag_off] = flags;
  }
  return out;
}

bool parse_sarc_owned(const uint8_t *buf, size_t len,
                      std::vector<SarcEntryOwned> &out) {
  out.clear();
  if (!buf || len < 0x20u || std::memcmp(buf, "SARC", 4) != 0)
    return false;
  if (rd_be16(buf + 0x06u) != 0xFEFFu)
    return false;
  const uint32_t data_off = rd_be32(buf + 0x0Cu);
  if (data_off > len || std::memcmp(buf + 0x14u, "SFAT", 4) != 0)
    return false;
  const uint16_t node_count = rd_be16(buf + 0x1Au);
  const size_t sfnt_off = 0x20u + static_cast<size_t>(node_count) * 16u;
  if (sfnt_off + 8u > len || std::memcmp(buf + sfnt_off, "SFNT", 4) != 0)
    return false;
  const size_t str_base = sfnt_off + 8u;

  out.reserve(node_count);
  for (uint16_t i = 0; i < node_count; ++i) {
    const uint8_t *e = buf + 0x20u + static_cast<size_t>(i) * 16u;
    const uint32_t attrs = rd_be32(e + 4u);
    const uint32_t sstart = rd_be32(e + 8u);
    const uint32_t send = rd_be32(e + 12u);
    if (send < sstart || static_cast<size_t>(data_off) + send > len)
      continue;
    const size_t noff =
        str_base + static_cast<size_t>(attrs & 0x00FFFFFFu) * 4u;
    if (noff >= len)
      continue;
    size_t nend = noff;
    while (nend < len && buf[nend] != 0u)
      ++nend;
    SarcEntryOwned item{};
    item.name.assign(reinterpret_cast<const char *>(buf + noff), nend - noff);
    item.data.assign(buf + data_off + sstart, buf + data_off + send);
    out.push_back(std::move(item));
  }
  return !out.empty();
}

std::vector<uint8_t> build_sarc_be(const std::vector<SarcEntryOwned> &entries) {
  std::vector<SarcEntryOwned> sorted = entries;
  std::sort(sorted.begin(), sorted.end(),
            [](const SarcEntryOwned &a, const SarcEntryOwned &b) {
              return sarc_name_hash(a.name) < sarc_name_hash(b.name);
            });

  const uint16_t n = static_cast<uint16_t>(sorted.size());
  const size_t sfat_off = 0x14u;
  const size_t sfat_nodes_off = sfat_off + 0x0Cu;
  const size_t sfnt_off = sfat_nodes_off + static_cast<size_t>(n) * 16u;
  const size_t sfnt_str_off = sfnt_off + 8u;

  std::vector<uint8_t> str_table;
  std::vector<uint32_t> str_word_offsets(n, 0u);
  for (uint16_t i = 0; i < n; ++i) {
    const size_t off = align_up(str_table.size(), 4u);
    str_word_offsets[i] = static_cast<uint32_t>(off / 4u);
    str_table.resize(off + sorted[i].name.size() + 1u, 0u);
    std::memcpy(str_table.data() + off, sorted[i].name.data(),
                sorted[i].name.size());
  }
  str_table.resize(align_up(str_table.size(), 4u), 0u);

  // Wii U BFRES inside SARC prefers 0x2000 alignment so GX2 vertex/texture
  // buffers match
  const size_t data_start = align_up(sfnt_str_off + str_table.size(), 0x2000u);
  std::vector<uint8_t> out(data_start, 0u);

  std::vector<uint32_t> d_start(n, 0u);
  std::vector<uint32_t> d_end(n, 0u);
  for (uint16_t i = 0; i < n; ++i) {
    const size_t rel_off = align_up(out.size() - data_start, 0x2000u);
    out.resize(data_start + rel_off + sorted[i].data.size(), 0u);
    d_start[i] = static_cast<uint32_t>(rel_off);
    d_end[i] = static_cast<uint32_t>(rel_off + sorted[i].data.size());
    std::memcpy(out.data() + data_start + rel_off, sorted[i].data.data(),
                sorted[i].data.size());
  }

  // SARC header (20 bytes)
  std::memcpy(out.data(), "SARC", 4);
  wr_be16(out.data() + 0x04u, 0x14u);
  wr_be16(out.data() + 0x06u, 0xFEFFu);
  wr_be32(out.data() + 0x08u, static_cast<uint32_t>(out.size()));
  wr_be32(out.data() + 0x0Cu, static_cast<uint32_t>(data_start));
  wr_be16(out.data() + 0x10u, 0x0100u);

  // SFAT header + nodes
  std::memcpy(out.data() + sfat_off, "SFAT", 4);
  wr_be16(out.data() + sfat_off + 4u, 0x0Cu);
  wr_be16(out.data() + sfat_off + 6u, n);
  wr_be32(out.data() + sfat_off + 8u, 0x00000065u);
  for (uint16_t i = 0; i < n; ++i) {
    uint8_t *ne = out.data() + sfat_nodes_off + static_cast<size_t>(i) * 16u;
    wr_be32(ne + 0u, sarc_name_hash(sorted[i].name));
    wr_be32(ne + 4u, 0x01000000u | (str_word_offsets[i] & 0x00FFFFFFu));
    wr_be32(ne + 8u, d_start[i]);
    wr_be32(ne + 12u, d_end[i]);
  }

  // SFNT header + names
  std::memcpy(out.data() + sfnt_off, "SFNT", 4);
  wr_be16(out.data() + sfnt_off + 4u, 0x08u);
  if (!str_table.empty())
    std::memcpy(out.data() + sfnt_str_off, str_table.data(), str_table.size());

  return out;
}

bool validate_custom_player_szs_bytes(const std::vector<uint8_t> &szs_bytes,
                                      const std::string &expected_fmdl_name,
                                      uint32_t *out_vtx_cnt,
                                      uint32_t *out_lod0_icnt,
                                      std::string *out_err) {
  std::vector<uint8_t> sarc_bytes;
  if (!decompress_yaz0(szs_bytes.data(), szs_bytes.size(), sarc_bytes)) {
    if (out_err)
      *out_err = "Invalid Yaz0 stream";
    return false;
  }
  std::vector<SarcEntryOwned> entries;
  if (!parse_sarc_owned(sarc_bytes.data(), sarc_bytes.size(), entries)) {
    if (out_err)
      *out_err = "Invalid SARC archive";
    return false;
  }
  const std::vector<uint8_t> *bfres_ptr = nullptr;
  for (const auto &e : entries) {
    if (e.name == "Output.bfres") {
      bfres_ptr = &e.data;
      break;
    }
  }
  if (!bfres_ptr || bfres_ptr->size() < 0x50u ||
      std::memcmp(bfres_ptr->data(), "FRES", 4) != 0) {
    if (out_err)
      *out_err = "Missing Output.bfres";
    return false;
  }
  const auto &bfres = *bfres_ptr;
  const uint32_t declared_sz = rd_be32(bfres.data() + 0x10u);
  if (declared_sz != bfres.size()) {
    if (out_err)
      *out_err = "FRES size mismatch";
    return false;
  }
  const int32_t fmdl_rel = rd_bes32(bfres.data() + 0x20u);
  const auto fmdls =
      parse_bfres_dict(bfres, 0x20u + static_cast<size_t>(fmdl_rel));
  if (fmdls.empty()) {
    if (out_err)
      *out_err = "No FMDL in Output.bfres";
    return false;
  }
  if (!expected_fmdl_name.empty() && fmdls[0].name != expected_fmdl_name) {
    if (out_err)
      *out_err = "Unexpected FMDL name: " + fmdls[0].name;
    return false;
  }

  const size_t moff = fmdls[0].data_off;
  const uint16_t fvtx_cnt = rd_be16(bfres.data() + moff + 0x20u);
  const uint16_t fmat_cnt = rd_be16(bfres.data() + moff + 0x24u);
  const size_t fvtx_arr =
      moff + 0x10u + static_cast<size_t>(rd_bes32(bfres.data() + moff + 0x10u));
  const size_t fshp_grp =
      moff + 0x14u + static_cast<size_t>(rd_bes32(bfres.data() + moff + 0x14u));
  const size_t fmat_grp =
      moff + 0x18u + static_cast<size_t>(rd_bes32(bfres.data() + moff + 0x18u));
  const int32_t ftex_rel = rd_bes32(bfres.data() + 0x24u);
  const size_t ftex_grp =
      ftex_rel != 0 ? (0x24u + static_cast<size_t>(ftex_rel)) : 0u;

  if (ftex_grp != 0u) {
    for (const auto &te : parse_bfres_dict(bfres, ftex_grp)) {
      if (!lookup_bfres_resdic(bfres, ftex_grp, te.name)) {
        if (out_err)
          *out_err = "FTEX ResDic lookup failed for " + te.name;
        return false;
      }
    }
  }
  for (const auto &me : parse_bfres_dict(bfres, fmat_grp)) {
    if (!lookup_bfres_resdic(bfres, fmat_grp, me.name)) {
      if (out_err)
        *out_err = "FMAT ResDic lookup failed for " + me.name;
      return false;
    }
    if ((me.name == "M_Eye" || me.name == "M_Eyelid") &&
        me.data_off + 0x4Cu <= bfres.size()) {
      const int32_t sa_rel = rd_bes32(bfres.data() + me.data_off + 0x24u);
      const size_t sa = static_cast<size_t>(
          static_cast<int64_t>(me.data_off + 0x24u) + sa_rel);
      const int32_t opt_rel =
          (sa_rel != 0 && sa + 0x1Cu <= bfres.size())
              ? rd_bes32(bfres.data() + sa + 0x18u)
              : 0;
      const size_t opt_dict = static_cast<size_t>(
          static_cast<int64_t>(sa + 0x18u) + opt_rel);
      if (sa_rel == 0 || opt_rel == 0 ||
          !lookup_bfres_resdic(bfres, opt_dict, "enable_shadow_pre_pass")) {
        if (out_err)
          *out_err = "FMAT opt_dict lookup failed for " + me.name;
        return false;
      }
    }
  }

  uint32_t total_vtx = 0u;
  uint32_t total_lod0 = 0u;
  for (const auto &se : parse_bfres_dict(bfres, fshp_grp)) {
    if (!lookup_bfres_resdic(bfres, fshp_grp, se.name)) {
      if (out_err)
        *out_err = "FSHP ResDic lookup failed for " + se.name;
      return false;
    }
    const size_t so = se.data_off;
    const uint16_t mat_idx = rd_be16(bfres.data() + so + 0x0Eu);
    const uint16_t vtx_idx = rd_be16(bfres.data() + so + 0x12u);
    if (mat_idx >= fmat_cnt || vtx_idx >= fvtx_cnt) {
      if (out_err)
        *out_err = "FSHP index out of range in " + se.name;
      return false;
    }
    if (se.name.find("TeamColor") == std::string::npos)
      continue;
    if (se.name.rfind("mouth", 0) == 0 || se.name.rfind("eyebrow", 0) == 0)
      continue;
    const uint8_t lod_cnt = bfres[so + 0x17u];
    const size_t voff = fvtx_arr + static_cast<size_t>(vtx_idx) * 0x20u;
    const uint8_t buf_cnt = bfres[voff + 5u];
    const uint32_t vtx_cnt = rd_be32(bfres.data() + voff + 8u);
    const size_t buf_arr =
        voff + 0x18u +
        static_cast<size_t>(rd_bes32(bfres.data() + voff + 0x18u));

    for (uint8_t bi = 0; bi < buf_cnt; ++bi) {
      const size_t bo = buf_arr + static_cast<size_t>(bi) * 0x18u;
      const uint32_t bsz = rd_be32(bfres.data() + bo + 4u);
      const int32_t brel = rd_bes32(bfres.data() + bo + 20u);
      const int64_t boff = static_cast<int64_t>(bo + 20u) + brel;
      if (boff < 0 || static_cast<size_t>(boff) + bsz > bfres.size()) {
        if (out_err)
          *out_err = "Vertex buffer out of bounds";
        return false;
      }
    }
    const size_t lod_arr =
        so + 0x24u + static_cast<size_t>(rd_bes32(bfres.data() + so + 0x24u));
    for (uint8_t li = 0; li < lod_cnt; ++li) {
      const size_t lo = lod_arr + static_cast<size_t>(li) * 0x1Cu;
      const uint32_t icnt = rd_be32(bfres.data() + lo + 8u);
      const int32_t ib_rel = rd_bes32(bfres.data() + lo + 20u);
      const size_t ibo =
          static_cast<size_t>(static_cast<int64_t>(lo + 20u) + ib_rel);
      const uint32_t isz = rd_be32(bfres.data() + ibo + 4u);
      const int32_t id_rel = rd_bes32(bfres.data() + ibo + 20u);
      const int64_t id_off = static_cast<int64_t>(ibo + 20u) + id_rel;
      if (id_off < 0 ||
          static_cast<size_t>(id_off) + icnt * 2u > bfres.size() ||
          icnt * 2u > isz) {
        if (out_err)
          *out_err = "Index buffer out of bounds";
        return false;
      }
      for (uint32_t k = 0; k < icnt; ++k) {
        if (rd_be16(bfres.data() + static_cast<size_t>(id_off) +
                    static_cast<size_t>(k) * 2u) >= vtx_cnt) {
          if (out_err)
            *out_err = "Vertex index out of bounds";
          return false;
        }
      }
      if (li == 0u && (se.name.find("hair") != std::string::npos ||
                       se.name.find("Player") != std::string::npos ||
                       se.name.find("body") != std::string::npos)) {
        total_lod0 += icnt;
      }
    }
    if (se.name.find("hair") != std::string::npos ||
        se.name.find("Player") != std::string::npos ||
        se.name.find("body") != std::string::npos) {
      total_vtx += vtx_cnt;
    }
  }

  if (out_vtx_cnt)
    *out_vtx_cnt = total_vtx;
  if (out_lod0_icnt)
    *out_lod0_icnt = total_lod0;
  return true;
}

void prune_custom_player_archives(const std::string &overlay_model_dir,
                                  size_t max_keep) {
  std::error_code ec;
  if (!std::filesystem::exists(overlay_model_dir, ec))
    return;
  struct FileEntry {
    std::filesystem::path path;
    std::filesystem::file_time_type mtime;
  };
  std::vector<FileEntry> files;
  for (const auto &entry :
       std::filesystem::directory_iterator(overlay_model_dir, ec)) {
    if (!entry.is_regular_file(ec))
      continue;
    const std::string fn = entry.path().filename().string();
    if (fn.rfind("CustPl_", 0) == 0) {
      files.push_back({entry.path(), entry.last_write_time(ec)});
    }
  }
  if (files.size() <= max_keep)
    return;
  std::sort(
      files.begin(), files.end(),
      [](const FileEntry &a, const FileEntry &b) { return a.mtime > b.mtime; });
  for (size_t i = max_keep; i < files.size(); ++i) {
    std::filesystem::remove(files[i].path, ec);
  }
}

BfresBuildReport build_custom_player_szs(const std::string &player_pack_path,
                                         const std::string &overlay_model_dir,
                                         const LocalRenderPlan &plan,
                                         const std::string &target_fmdl_name) {
  BfresBuildReport rep{};
  rep.archive_name = plan.custom_archive_name;

  std::vector<uint8_t> pack_bytes;
  if (!read_file_bytes(player_pack_path, pack_bytes)) {
    rep.message = "Missing Player.pack at " + player_pack_path;
    return rep;
  }
  std::vector<SarcEntryOwned> pack_entries;
  if (!parse_sarc_owned(pack_bytes.data(), pack_bytes.size(), pack_entries)) {
    rep.message = "Failed to parse Player.pack SARC";
    return rep;
  }

  auto load_archive_bfres = [&](const std::string &pack_key,
                                std::vector<SarcEntryOwned> &out_entries,
                                std::vector<uint8_t> &out_bfres) -> bool {
    for (const auto &e : pack_entries) {
      if (e.name != pack_key)
        continue;
      std::vector<uint8_t> sarc;
      if (!decompress_yaz0(e.data.data(), e.data.size(), sarc))
        return false;
      if (!parse_sarc_owned(sarc.data(), sarc.size(), out_entries))
        return false;
      for (const auto &se : out_entries) {
        if (se.name == "Output.bfres") {
          out_bfres = se.data;
          return true;
        }
      }
    }
    return false;
  };

  const std::string base_key =
      std::string("Model/") + body_archive_name(plan.body_model) + ".szs";
  std::vector<SarcEntryOwned> base_sarc;
  std::vector<uint8_t> base_bfres;
  if (!load_archive_bfres(base_key, base_sarc, base_bfres)) {
    rep.message = "Missing base archive " + base_key;
    return rep;
  }

  // Determine source hair shape + rebinding requirements
  std::vector<TeamColorVertex> hair_verts;
  std::array<std::vector<uint16_t>, 3> hair_lods{};
  std::vector<uint8_t> src_bfres;
  bool rebind_head = true;

  if (plan.hairstyle.source == HairSourceKind::ExternalObj) {
    if (!load_external_obj_hair(plan.hairstyle.obj_path,
                                plan.hairstyle.custom_transform, hair_verts,
                                hair_lods[0])) {
      rep.message =
          "Failed to load external hair OBJ: " + plan.hairstyle.obj_path;
      return rep;
    }
    hair_lods[1] = hair_lods[0];
    hair_lods[2] = hair_lods[0];
    recompute_smooth_normals(hair_verts, 0u, hair_verts.size(), hair_lods[0]);
    rebind_head = true;
  } else {
    std::string src_key = "Model/Player00.szs";
    std::string src_shape = "Player00__M_TeamColor";
    bool src_all_hair = false;
    if (plan.hairstyle.source == HairSourceKind::BoyNative) {
      src_key = "Model/Player01.szs";
      src_shape = "Player01__M_TeamColor";
      rebind_head = (plan.body_model != BodyModelId::Boy);
    } else if (plan.hairstyle.source == HairSourceKind::OctoNative) {
      src_key = "Model/Rival00.szs";
      src_shape = "hair__M_TeamColor";
      src_all_hair = true;
      rebind_head = true;
    } else {
      rebind_head = (plan.body_model != BodyModelId::Girl);
    }

    TeamColorShapeData src_hair_shape{};
    if (src_key == base_key) {
      if (!extract_teamcolor_shape(base_bfres, src_shape, src_all_hair,
                                   src_hair_shape)) {
        rep.message = "Failed to extract hair shape " + src_shape;
        return rep;
      }
    } else {
      std::vector<SarcEntryOwned> tmp_sarc;
      if (!load_archive_bfres(src_key, tmp_sarc, src_bfres) ||
          !extract_teamcolor_shape(src_bfres, src_shape, src_all_hair,
                                   src_hair_shape)) {
        rep.message = "Failed to extract hair source from " + src_key;
        return rep;
      }
    }
    if (rebind_head &&
        (plan.hairstyle.source == HairSourceKind::BoyNative ||
         plan.hairstyle.source == HairSourceKind::GirlNative)) {
      // Bake Boy/Girl Hair*/nape* bones into their Wait rest pose before
      // freezing them onto a single rigid/smooth head bone.
      bake_hair_wait_pose(src_bfres.empty() ? base_bfres : src_bfres,
                          plan.hairstyle.source, src_hair_shape);
    }
    if (plan.hairstyle.source == HairSourceKind::OctoNative) {
      // Rival00 hair__M_TeamColor is stored in head-local rigid space; lift
      // into canonical model space before sculpting or Inkling synthesis.
      for (auto &sv : src_hair_shape.verts)
        sv.y += kHeadBindY;
    }
    extract_hair_vertices_only(
        src_hair_shape, rebind_head, plan.hairstyle.sculpt,
        plan.hairstyle.custom_transform, hair_verts, hair_lods);
  }

  if (hair_verts.empty() || hair_lods[0].empty()) {
    rep.message = "Extracted empty hair geometry";
    return rep;
  }

  rep.cross_body_head_rebind = rebind_head;
  rep.hair_vtx_count = static_cast<uint32_t>(hair_verts.size());

  uint16_t hair_mat_idx = 0u;
  const bool has_cross_body_material = !src_bfres.empty();
  if (has_cross_body_material) {
    if (!clone_source_teamcolor_material(base_bfres, src_bfres, hair_mat_idx)) {
      rep.message = "Failed cloning cross-body hair material & FTEX textures";
      return rep;
    }
  }

  if (plan.body_model == BodyModelId::Octoling) {
    // Rival00 already has separate body__M_TeamColor & hair__M_TeamColor —
    // replace hair__M_TeamColor cleanly!
    TeamColorShapeData rival_hair_meta{};
    if (!extract_teamcolor_shape(base_bfres, "hair__M_TeamColor", true,
                                 rival_hair_meta)) {
      rep.message = "Missing Rival00 hair__M_TeamColor";
      return rep;
    }
    // Rival00 hair uses 1-byte rigid skinning at buffer 3 (no _w0) in
    // head-local space; subtract kHeadBindY and preserve Rival00's head rigid
    // index (45).
    const uint8_t head_rigid_idx =
        rival_hair_meta.verts.empty() ? 45u : rival_hair_meta.verts[0].i0[0];
    for (auto &hv : hair_verts) {
      hv.y -= kHeadBindY;
      hv.i0[0] = head_rigid_idx;
    }
    if (has_cross_body_material) {
      wr_be16(base_bfres.data() + rival_hair_meta.fshp_off + 0x0Eu,
              hair_mat_idx);
    }
    if (!append_fmdl_teamcolor_buffers(base_bfres, rival_hair_meta, hair_verts,
                                       hair_lods, false)) {
      rep.message = "Failed writing Rival00 custom hair buffers";
      return rep;
    }
    if (!fix_rival_eye_and_eyelid_materials(base_bfres)) {
      rep.message = "Failed patching Rival00 M_Eye/M_Eyelid materials";
      return rep;
    }
    rep.body_vtx_count = 1379u;
    rep.combined_vtx_count = rep.hair_vtx_count;
    rep.lod0_index_count = static_cast<uint32_t>(hair_lods[0].size());
  } else {
    const std::string base_shape_name = (plan.body_model == BodyModelId::Boy)
                                            ? "Player01__M_TeamColor"
                                            : "Player00__M_TeamColor";
    TeamColorShapeData base_shape{};
    if (!extract_teamcolor_shape(base_bfres, base_shape_name, false,
                                 base_shape)) {
      rep.message = "Missing base shape " + base_shape_name;
      return rep;
    }

    // Extract body-only vertices and remap LOD0/1/2 body triangles cleanly
    std::vector<TeamColorVertex> body_verts;
    std::vector<int32_t> body_remap(base_shape.verts.size(), -1);
    for (size_t i = 0; i < base_shape.verts.size(); ++i) {
      if (base_shape.verts[i].is_hair)
        continue;
      body_remap[i] = static_cast<int32_t>(body_verts.size());
      body_verts.push_back(base_shape.verts[i]);
    }
    rep.body_vtx_count = static_cast<uint32_t>(body_verts.size());

    std::array<std::vector<uint16_t>, 3> body_lods{};
    for (uint32_t li = 0; li < 3u; ++li) {
      const auto &src_l = base_shape.lod_indices[li];
      for (size_t k = 0; k + 2u < src_l.size(); k += 3u) {
        const uint16_t a = src_l[k], b = src_l[k + 1u], c = src_l[k + 2u];
        if (a < body_remap.size() && b < body_remap.size() &&
            c < body_remap.size() && body_remap[a] >= 0 && body_remap[b] >= 0 &&
            body_remap[c] >= 0) {
          body_lods[li].push_back(static_cast<uint16_t>(body_remap[a]));
          body_lods[li].push_back(static_cast<uint16_t>(body_remap[b]));
          body_lods[li].push_back(static_cast<uint16_t>(body_remap[c]));
        }
      }
    }

    if (has_cross_body_material) {
      TeamColorShapeData hair_meta{};
      if (!add_cloned_hair_fshp_and_fvtx(base_bfres, base_shape, hair_mat_idx,
                                         hair_meta)) {
        rep.message = "Failed creating separate hair__M_TeamColor FSHP/FVTX";
        return rep;
      }
      if (!append_fmdl_teamcolor_buffers(base_bfres, base_shape, body_verts,
                                         body_lods, true)) {
        rep.message = "Failed writing Inkling body-only M_TeamColor buffers";
        return rep;
      }
      if (!append_fmdl_teamcolor_buffers(base_bfres, hair_meta, hair_verts,
                                         hair_lods, true)) {
        rep.message = "Failed writing cross-body hair__M_TeamColor buffers";
        return rep;
      }
      rep.combined_vtx_count =
          static_cast<uint32_t>(body_verts.size() + hair_verts.size());
      rep.lod0_index_count =
          static_cast<uint32_t>(body_lods[0].size() + hair_lods[0].size());
    } else {
      const uint16_t hair_base_idx = static_cast<uint16_t>(body_verts.size());
      body_verts.insert(body_verts.end(), hair_verts.begin(), hair_verts.end());
      for (uint32_t li = 0; li < 3u; ++li) {
        for (uint16_t hidx : hair_lods[li]) {
          body_lods[li].push_back(
              static_cast<uint16_t>(hair_base_idx + hidx));
        }
      }

      if (!append_fmdl_teamcolor_buffers(base_bfres, base_shape, body_verts,
                                         body_lods, true)) {
        rep.message = "Failed writing combined Player M_TeamColor buffers";
        return rep;
      }
      rep.combined_vtx_count = static_cast<uint32_t>(body_verts.size());
      rep.lod0_index_count = static_cast<uint32_t>(body_lods[0].size());
    }
  }

  // Apply optional extended Eye / Skin texture color endpoint tinting in-place
  // (preserving GX2 tiling!)
  if (plan.eye_color.is_extended) {
    rep.eye_tinted =
        apply_ftex_tint(base_bfres, "M_Eye_Alb", plan.eye_color.tint_r,
                        plan.eye_color.tint_g, plan.eye_color.tint_b);
  }
  if (plan.skin_tone.is_extended) {
    rep.skin_tinted =
        apply_ftex_tint(base_bfres, "M_Body_Alb", plan.skin_tone.tint_r,
                        plan.skin_tone.tint_g, plan.skin_tone.tint_b);
  }

  // Patch FMDL name so engine FMDL lookup matches the slot's model name
  if (!target_fmdl_name.empty()) {
    patch_fmdl_name(base_bfres, target_fmdl_name);
  }

  // Update FRES file size field at +0x10
  wr_be32(base_bfres.data() + 0x10u, static_cast<uint32_t>(base_bfres.size()));

  // Replace Output.bfres in base SARC, re-pack, Yaz0-compress, and validate
  // round-trip before publishing!
  for (auto &e : base_sarc) {
    if (e.name == "Output.bfres") {
      e.data = std::move(base_bfres);
      break;
    }
  }
  const std::vector<uint8_t> packed_sarc = build_sarc_be(base_sarc);
  const std::vector<uint8_t> szs_bytes =
      compress_yaz0_fast(packed_sarc.data(), packed_sarc.size());

  std::string val_err;
  if (!validate_custom_player_szs_bytes(szs_bytes, target_fmdl_name, nullptr,
                                        nullptr, &val_err)) {
    rep.message = "Post-build validation failed: " + val_err;
    return rep;
  }

  const std::filesystem::path out_path =
      std::filesystem::path(overlay_model_dir) /
      (plan.custom_archive_name + ".szs");
  if (!atomic_write_bytes(out_path.string(), szs_bytes)) {
    rep.message = "Failed writing overlay .szs to " + out_path.string();
    return rep;
  }

  prune_custom_player_archives(overlay_model_dir, 4u);
  rep.ok = true;
  rep.output_szs_path = out_path.string();
  rep.message = "Built " + plan.custom_archive_name + ".szs (" +
                std::to_string(szs_bytes.size() / 1024u) + " KB)";
  return rep;
}

} // namespace char_customizer
