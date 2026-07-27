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

	// RenderScene -- the entire 3D frame for one viewport: transform, emit, flush.
	// Called from 0x00409BC0 (menu, which is genuine 3D) and twice from 0x00435E80
	// (race, once per split-screen viewport). This is the capture point: on entry the
	// scene and object list are fully populated and nothing has been projected yet.
	constexpr uint32_t ADDR_RenderScene = 0x0044A910u;
	typedef void(__cdecl* RenderScene_t)();

	// TransformAllObjects. 0x44D020 tests focal_x <= 0xFA and otherwise tail-calls 0x44D640,
	// so hooking 0x44D020 alone covers both paths. Live focal_x is 253 (menu) / 425 (race),
	// meaning 0x44D640 is what actually executes -- but we never need to hook it directly.
	constexpr uint32_t ADDR_TransformAllObjects = 0x0044D020u;

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

	inline int32_t resolve_texture_id(const int32_t tex_sel, const int32_t tex_page) {
		const uint32_t index = static_cast<uint32_t>(tex_sel >> 16) + tex_page * 0x20u;
		if (index >= TEX_TABLE_ENTRIES) {
			return TEX_ID_INVALID;
		}
		return reinterpret_cast<const int32_t*>(rebase(ADDR_g_texTable))[index];
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
