#pragma once

#include <cstdint>

namespace char_customizer::addr {

// ---- Global Singletons (big-endian pointer slots in .data) ----
constexpr uint32_t kGameInfoPtr = 0x101DD330u; // Cmn::GameInfo*
constexpr uint32_t kGfxSettingPtr = 0x101DE170u; // Cmn::GfxSetting*
constexpr uint32_t kMainMgrPtr = 0x101E2814u;  // Game::MainMgr*
constexpr uint32_t kSaveDataCmnPtr = 0x101E6750u;
constexpr uint32_t kEnlMgrPtr = 0x101E22ACu; // +0x44: 1=offline, 0=online

constexpr uint32_t kMainMgrLocalPlayerNumOff = 0x2C8u;
constexpr uint32_t kGameInfoTeamColorsOff =
    0x2E0u; // 3 x Color4f (TeamA, TeamB, Neutral)

// ---- Cmn::PlayerInfo / Wire Offsets ----
constexpr uint32_t kPlayerInfoValid = 0x04u; // u8
constexpr uint32_t kPlayerInfoTeam = 0x30u;  // u32 (0=Alpha, 1=Bravo)
constexpr uint32_t kPlayerInfoModelType =
    0x34u; // u32 (0=Girl, 1=Boy, 2=Octo local only)
constexpr uint32_t kPlayerInfoSkinCol = 0x38u; // u32 (0..6)
constexpr uint32_t kPlayerInfoEyeCol = 0x3Cu;  // u32 (0..7)
constexpr uint32_t kPlayerInfoSize = 0xECu;

// Packed ENL wire PlayerInfo offsets (Gambit_fn_021A8F30 / 021A92E8)
constexpr uint32_t kWireModelTypeS8 =
    0x08u; // s8 (>1 clamped to 0 by vanilla Wii U / Cemu)
constexpr uint32_t kWireSkinColS8 = 0x09u; // s8 (0..6)
constexpr uint32_t kWireEyeColS8 = 0x0Au;  // s8 (0..7)

// ---- Game::Player / PlayerMgr / PlayerModel ----
constexpr uint32_t kPlayerNetCtrlOff = 0x50u;  // u32 (0 = local simulation)
constexpr uint32_t kPlayerCtrlTypeOff = 0x58u; // u32 (0 = local human primary)
constexpr uint32_t kPlayerModelTypeOff =
    0x5Cu; // u32 (0 = Player00, 1 = Player01) read by PlayerModel ctor
constexpr uint32_t kPlayerSlotIdxOff = 0x9Cu;    // u32 (0..7) player slot index
constexpr uint32_t kPlayerInfoPtrOff = 0xA4u;    // Cmn::PlayerInfo*
constexpr uint32_t kPlayerMgrBackPtrOff = 0xA8u; // Game::PlayerMgr*

// PlayerMgr preloaded ArchiveHolder* arrays (8-byte ArchiveHolder[5] or ptr
// arrays) Verified in Gambit_fn_026BB2C0 (0x026BB344..0x026BB390): For index m
// in [0..count): holder_ptr = *(base + 0x08 + 4*m) -> ArchiveRes* at *(holder +
// 0)
constexpr uint32_t kPlayerMgrModelArcAryOff =
    0x240u; // ptr to ArchiveHolder*[count]
constexpr uint32_t kPlayerMgrHalfArcAryOff = 0x258u;
constexpr uint32_t kPlayerMgrSquidArcAryOff = 0x270u;
constexpr uint32_t kPlayerMgrAnimArcAryOff = 0x288u;
constexpr uint32_t kPlayerMgrLocalSlot0Off =
    0x2B0u; // primary local player's slot index

// ---- Guest Function Addresses (Splatoon v288) ----
constexpr uint32_t kArchiveHolderLoad =
    0x02891988u; // (ArchiveHolder* dst, SafeString* name, s32 flag, Heap* heap)
constexpr uint32_t kPlayerModelCtor =
    0x026BB2C0u; // (PlayerModel* this, Game::Player* player)
constexpr uint32_t kPlayerMgrLoadArchives =
    0x026BD334u; // (PlayerArcMgr* this, Heap* heap)
constexpr uint32_t kPlayerCustomSetup =
    0x026BD7BCu; // (PlayerCustom* this, Game::Player* player)
constexpr uint32_t kSetSkinColor =
    0x021A63A4u; // (PlayerCustom* obj, AnimHolder* anim, u32 skin_id)
constexpr uint32_t kSetEyeColor =
    0x021A63F0u; // (PlayerCustom* obj, AnimHolder* anim, u32 eye_id)
constexpr uint32_t kGfxGetTeamColor =
    0x02042094u; // (Color4f out[3], GfxSetting*, const s32* mode, ...)
constexpr uint32_t kGfxSetTeamColor =
    0x020436A0u; // (GfxSetting*, const Color4f in[3], s32)

// Local-player scene model setup functions (Plaza, Lobby, Equip, Shop,
// PlayerMake, Result, Duel)
constexpr uint32_t kPlazaPlayerSetup = 0x027C0FE8u;
constexpr uint32_t kLobbyPlayerSetup = 0x027F899Cu;
constexpr uint32_t kEquipPlayerSetup = 0x020389A4u;
constexpr uint32_t kShopPlayerSetup = 0x0283D108u;
constexpr uint32_t kMakePlayerSetup = 0x02824F5Cu;
constexpr uint32_t kResultPlayerSetup = 0x0280BC68u;
constexpr uint32_t kDuelPlayerSetup = 0x027D41D8u;

// Guest vtable-called bootstrap addresses
constexpr uint32_t kPlayerInitSubobjs =
    0x02630E04u; // (Game::Player* this)
constexpr uint32_t kPlayerCalc =
    0x02641C1Cu; // (Game::Player* this) per-frame Plaza & Battle
constexpr uint32_t kPlayerMgrLoad =
    0x026BA02Cu; // (Game::PlayerMgr* this, Heap* heap)

// Plaza tick & scene transition helpers
constexpr uint32_t kPlzVsDoorCalc = 0x027826A4u;
constexpr uint32_t kSceneMgrChangeSceneStr = 0x028C6570u;
constexpr uint32_t kSceneMgrChangeSceneDirect = 0x028C6548u;
constexpr uint32_t kSceneStrPlaza = 0x1011181Cu;

// ENL PlayerInfo network serialization & deserialization
constexpr uint32_t kPlayerInfoPackEnl = 0x021A92E8u; // (WirePacket* dst)
constexpr uint32_t kPlayerInfoUnpackEnl =
    0x021A8F30u; // (Cmn::PlayerInfo* dst, const WirePacket* src)

// SaveDataCmn getters
constexpr uint32_t kSaveDataGetModelType = 0x021AE238u; // (SaveDataCmn*) -> u32
constexpr uint32_t kSaveDataGetSkinColor = 0x021AE298u; // (SaveDataCmn*) -> u32
constexpr uint32_t kSaveDataGetEyeColor = 0x021AE2C8u;  // (SaveDataCmn*) -> u32

// Heap allocator
constexpr uint32_t kMemAlloc = 0x028CD9F0u; // (u32 size) -> guest ptr
constexpr uint32_t kMemFree = 0x028CDBE4u;

// sead::SafeString vtable in v288 .rodata
constexpr uint32_t kSafeStringVtable = 0x10111490u;

} // namespace char_customizer::addr
