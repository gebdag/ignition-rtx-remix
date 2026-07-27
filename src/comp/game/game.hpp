#pragma once
#include "structs.hpp"

namespace comp::game
{
	// Ign_3dfx.exe has no .reloc section and loads at its preferred base; this was confirmed
	// live by disassembling 0x0044D020 in the running process and matching it byte-for-byte
	// against the static listing. The rebase is kept so addresses stay meaningful regardless.
	constexpr uint32_t PREFERRED_BASE = 0x00400000u;

	inline uint32_t rebase(const uint32_t static_addr) {
		return shared::globals::exe_module_addr + (static_addr - PREFERRED_BASE);
	}

	// --------------
	// game functions

	// RenderScene -- one full pass of the 3D world:
	//     0x0044A969  call 0x0044BE30  BuildObjectList  (streams objects around the camera)
	//     0x0044A97A  call 0x0044D020  TransformAllObjects
	//     0x0044A98C  call 0x0044E8D0  EmitAllFaces
	//     0x0044A99E  call 0x0044B770  FlushDepthBuckets
	//
	// The game runs this several times per frame, setting the visibility filter DAT_00622E84
	// immediately before each call (0x00436FFC, 0x00437288). Every pass draws into the same
	// viewport, so together they form one image.
	constexpr uint32_t ADDR_RenderScene = 0x0044A910u;

	// BuildObjectList -- clears scene->object_count and refills g_objectList from a grid
	// neighbourhood around scene->cam/yaw, filtered by DAT_00622E84 (0 = everything, otherwise
	// only objects whose tag at +0x24 is 0 or matches).
	constexpr uint32_t ADDR_BuildObjectList = 0x0044BE30u;

	// TransformAllObjects -- the capture point. By here BuildObjectList has run for THIS pass,
	// so the object list is current, and nothing has been projected yet. Capturing any earlier
	// (at RenderScene entry) reads the previous pass's list.
	//
	// 0x44D020 tests focal_x <= 0xFA and otherwise tail-calls 0x44D640, so hooking it covers
	// both transform paths. Live focal_x is 253 (menu) / 425 (race).
	constexpr uint32_t ADDR_TransformAllObjects = 0x0044D020u;
	typedef void(__cdecl* TransformAllObjects_t)();

	// Render pacing. The game refuses to draw unless a full 36 Hz tick has elapsed:
	//
	//   FUN_0041D450  local = now/27.7778 - last;  if (1.0 <= local) { last = now/27.7778; ... }
	//                 returns local, so < 1.0 while inside a tick
	//   FUN_004116A0  dVar1 = FUN_0041D450(); if (dVar1 < 1.0) { idle } else { update + render }
	//
	// Both compare against the *shared* 1.0 at 0x0046D458, which 21 other instructions also
	// use -- so the value cannot be changed. Instead these two instructions
	// (fcomp qword ptr [0x46D458], encoded DC 1D + disp32) get their operand redirected to a
	// private constant.
	//
	// Lowering the gate is safe for game speed because FUN_0041D450 stores the true current
	// time and returns the real elapsed fraction: crossing at 0.5 yields dVar1 = 0.5, so half
	// a tick of movement for half a tick of real time. Changing the 27.7778 period instead
	// would double dVar1 for the same elapsed time and run the game fast.
	constexpr uint32_t ADDR_RenderGateCmp_Delta = 0x0041D47Du;
	constexpr uint32_t ADDR_RenderGateCmp_Tick = 0x00411820u;
	constexpr uint8_t FCOMP_M64_OPCODE[2] = { 0xDC, 0x1D };

	// RunDisplayList(ctx) -- walks a display list and turns it into Glide calls. Called for the
	// world right after each RenderScene, and separately for HUD and menu overlays:
	//
	//   world : 0x00437014, 0x00437298   (return addresses 0x00437019, 0x0043729D)
	//   other : 0x00409D1E, 0x00409DF7, 0x0043D957, 0x0043EA1E, 0x0043FE6E
	//
	// Skipping the world calls stops those triangles being emitted at all, which is the only
	// reliable place to separate world from HUD: nGlide buffers Glide calls and flushes them at
	// swap, so nothing flagged during the game's own frame survives to the D3D draws. Filtering
	// on depth state does not work either -- the game depth-sorts into buckets and never uses a
	// depth buffer, so ZENABLE is off for the world as well as the HUD.
	constexpr uint32_t ADDR_RunDisplayList = 0x00450EC0u;
	typedef void(__cdecl* RunDisplayList_t)(void* ctx);

	constexpr uint32_t RET_WorldDisplayList_A = 0x00437019u;
	constexpr uint32_t RET_WorldDisplayList_B = 0x0043729Du;

	// The per-pass visibility filter. Objects carry a tag at +0x24; a pass includes an object
	// when the tag is 0 or equals this value.
	constexpr uint32_t ADDR_g_visibilityFilter = 0x00622E84u;

	// Walks each object's variable-stride face stream, dispatching through g_faceDispatch.
	// Only needed if we ever want the game's own per-face material resolution.
	constexpr uint32_t ADDR_EmitAllFaces = 0x0044E8D0u;

	// UploadTexture(src) -- the game's only texture upload path, called from 0x00409E10 and
	// 0x00415E50. Always 256x256 at 8 bits per pixel. It remaps the source bytes through
	// g_texRemapLut into GR_TEXFMT_RGB_332 and returns the GrMipMapId_t that g_texTable stores,
	// which is what per-face texSel ultimately resolves to.
	constexpr uint32_t ADDR_UploadTexture = 0x00450E10u;
	typedef int(__cdecl* UploadTexture_t)(void* src);

	constexpr int TEXTURE_SIZE = 256;
	constexpr int TEXTURE_PIXELS = TEXTURE_SIZE * TEXTURE_SIZE;

	// Selects the upload format: 0 takes the remap-to-RGB332 path, anything else uploads raw
	// GR_TEXFMT_P_8 indices. grTexDownloadTable is never called anywhere in the executable, so
	// no Glide palette is ever supplied and the P_8 path cannot be the one in use.
	constexpr uint32_t ADDR_g_texFormatFlag = 0x00621E60u;

	// 256 bytes: source palette index -> RGB332.
	constexpr uint32_t ADDR_g_texRemapLut = 0x00621360u;

	// --------------
	// game variables

	// ign_scene** -- swapped between the menu view and each race viewport.
	constexpr uint32_t ADDR_g_scene = 0x006236A8u;

	// ign_object*** -- array of object pointers, scene->object_count entries.
	constexpr uint32_t ADDR_g_objectList = 0x00622E78u;

	// texSel >> 16, plus object->tex_page * 0x20, indexes this table. 512 entries, reset to
	// 0xFFFFFFFF (invalid mipmap id) by the renderer init at 0x0044A9E0.
	constexpr uint32_t ADDR_g_texTable = 0x00622EA0u;
	constexpr uint32_t TEX_TABLE_ENTRIES = 512u;
	constexpr int32_t TEX_ID_INVALID = -1;

	// Why a face failed to resolve to a texture, so "it went white" can be diagnosed instead
	// of guessed at.
	enum class tex_resolve : uint8_t { ok, index_out_of_range, table_entry_unset };

	inline int32_t resolve_texture_id(const int32_t tex_sel, const int32_t tex_page,
	                                  tex_resolve& why) {
		const uint32_t index = static_cast<uint32_t>(tex_sel >> 16) + tex_page * 0x20u;
		if (index >= TEX_TABLE_ENTRIES) {
			why = tex_resolve::index_out_of_range;
			return TEX_ID_INVALID;
		}

		const int32_t id = reinterpret_cast<const int32_t*>(rebase(ADDR_g_texTable))[index];
		why = (id == TEX_ID_INVALID) ? tex_resolve::table_entry_unset : tex_resolve::ok;
		return id;
	}

	// The fused projection*view 3x3 the game itself uses, fixed point 16.16. We do NOT feed
	// this to Remix -- rows 0/1 have the focal lengths baked in and row 2 is scaled by 2^18.
	// It is read only for diagnostics, to check our reconstruction against the game's own.
	constexpr uint32_t ADDR_g_viewMatrix = 0x004A55F0u;

	inline ign_scene* get_scene() {
		return *reinterpret_cast<ign_scene**>(rebase(ADDR_g_scene));
	}

	inline ign_object** get_object_list() {
		return *reinterpret_cast<ign_object***>(rebase(ADDR_g_objectList));
	}

	// Angle -> radians with the game's own sign convention. The constant at 0x0046DC50 is
	// -pi/1800, NOT -pi/180: angles are in 1/10 degree units, so a full turn is 3600. The
	// negation is folded in here rather than applied ad hoc at each call site.
	constexpr double ANGLE_TO_RAD_NEG = -0.0017453292519943333;

	// Object Euler angles use the same 1/10 degree units -- 0x0044DC20 normalises them
	// against 0xE10 (3600), which is a full turn. They are NOT a different scale from the
	// camera's; treating them as such makes every object rotate a tenth as far as it should.
	constexpr double DEG_TO_RAD_NEG = ANGLE_TO_RAD_NEG;
	constexpr double OBJ_ANGLE_TO_RAD_NEG = ANGLE_TO_RAD_NEG;

	// Face UVs are 24.8 fixed point in texels: the rasterizer computes (u >> 4) * 0.0625,
	// which is u / 256.
	constexpr float UV_FIXED_SCALE = 1.0f / 256.0f;

	// ---

	extern void init_game_addresses();
}
