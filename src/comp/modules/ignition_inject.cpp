#include "std_include.hpp"

#include "ignition_inject.hpp"
#include "shared/common/config.hpp"

#include <filesystem>
#include <fstream>
#include <tuple>

namespace comp
{
	namespace
	{
		using namespace game;

		game::TransformAllObjects_t o_transform_all_objects = nullptr;
		game::UploadTexture_t o_upload_texture = nullptr;
		game::RunDisplayList_t o_run_display_list = nullptr;

		// Drops the world display list before it becomes Glide calls, leaving 2D overlays
		// untouched. Identified by return address because one function serves every list.
		void __cdecl hk_run_display_list(void* ctx)
		{
			const auto ret = reinterpret_cast<uint32_t>(_ReturnAddress());

			const bool is_world = (ret == game::rebase(game::RET_WorldDisplayList_A)
				|| ret == game::rebase(game::RET_WorldDisplayList_B)
				|| ret == game::rebase(game::RET_WorldDisplayList_C));

			if (is_world) {
				++ignition_inject::s_world_lists_seen;
			}
			else {
				++ignition_inject::s_other_lists_seen;
				// Which site draws the menu is the open question -- the menu region was never
				// analysed, so the call inventory cannot be trusted. Record the callers.
				++ignition_inject::s_list_callers[ret - shared::globals::exe_module_addr
					+ game::PREFERRED_BASE];
			}

			if (is_world && ignition_inject::suppress_world_raster()) {
				++ignition_inject::s_world_lists_dropped;
				return;
			}

			o_run_display_list(ctx);
		}

		// The game hands Glide a mipmap id; the same id is what g_texTable stores and what our
		// per-face lookup resolves to, so capturing the pair here is all the correlation needed.
		int __cdecl hk_upload_texture(void* src)
		{
			const int tex_id = o_upload_texture(src);

			if (const auto self = ignition_inject::get(); self && src && tex_id != game::TEX_ID_INVALID) {
				self->on_texture_uploaded(tex_id, static_cast<const uint8_t*>(src));
			}

			return tex_id;
		}

		// Capture at TransformAllObjects rather than at RenderScene, because RenderScene builds
		// the object list *inside* itself:
		//
		//     0x0044A969  call FUN_0044BE30          <- streams the object list around the camera
		//     0x0044A97A  call TransformAllObjects   <- we hook here
		//     0x0044A98C  call EmitAllFaces
		//     0x0044A99E  call FlushDepthBuckets
		//
		// Capturing at RenderScene entry read the list left over from the *previous* pass. The
		// game runs several passes per frame, each with a different visibility filter in
		// DAT_00622E84 (set at 0x00436FFC and 0x00437288 immediately before each call), so the
		// stale list belonged to some other car's streamed neighbourhood -- the world appeared
		// to follow the AI cars instead of the player.
		void __cdecl hk_transform_all_objects()
		{
			if (const auto self = ignition_inject::get()) {
				self->capture_scene();
			}

			o_transform_all_objects();
		}

		// FNV-1a over the mesh data extract_geometry reads: the vertex array, and the records of
		// the face opcodes we turn into triangles. Everything Ignition stores is 32 bit, so the
		// hash consumes words rather than bytes.
		uint64_t mesh_signature(const game::ign_mesh* mesh)
		{
			uint64_t h = 1469598103934665603ull;

			const auto mix = [&h](const int32_t* words, const size_t count) {
				for (size_t i = 0; i < count; ++i) {
					h = (h ^ static_cast<uint32_t>(words[i])) * 1099511628211ull;
				}
			};

			mix(reinterpret_cast<const int32_t*>(game::mesh_vertices(mesh)),
				static_cast<size_t>(mesh->vertex_count) * 3);

			const auto* cur = static_cast<const uint8_t*>(game::mesh_faces(mesh));
			for (int32_t i = 0; i < mesh->face_count; ++i)
			{
				const uint32_t op = static_cast<uint32_t>(*reinterpret_cast<const int32_t*>(cur)) & 0xFFu;
				if (op >= std::size(game::FACE_STRIDE)) {
					break;
				}

				const uint8_t stride = game::FACE_STRIDE[op];
				if (stride == 0) {
					break;
				}

				if (game::face_is_textured_tri(op)) {
					mix(reinterpret_cast<const int32_t*>(cur), stride / sizeof(int32_t));
				}

				cur += stride;
			}

			return h;
		}

		constexpr D3DVERTEXELEMENT9 INJECT_DECL[] = {
			{ 0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
			{ 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0 },
			{ 0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
			D3DDECL_END()
		};

		// The camera rotation Ignition builds in TransformAllObjects, without the focal/depth
		// scales it folds into rows 0/1/2 (reproduced from 0x0044D640).
		//
		// 0x0045FAB4 is COS and 0x0045FA04 is SIN, not the other way round -- both are thunks
		// through 0x004624A5 so the disassembly does not say which, but the live matrix does.
		// Measured against the game's own matrix at pitch/yaw = 71.59/146.89 degrees:
		//     row2 = (-cp*sy, sp, cp*cy) = ( 0.1727, -0.9487, -0.2648)
		//     game                         = ( 0.1725, -0.9488, -0.2645)
		// The giveaway is row1.y: with the opposite assignment it evaluates to sp*sr, which is
		// identically zero whenever roll is zero, yet the game reports -0.3158.
		//
		// Angles arrive in 1/10 degree units -- the game's constant at 0x0046DC50 is pi/1800,
		// not pi/180.
		void camera_rotation(const double pitch, const double yaw, const double roll, float r[3][3])
		{
			const double n = game::CAM_ANGLE_TO_RAD_NEG;
			const double sp = sin(pitch * n);
			const double cp = cos(pitch * n);
			const double sy = sin(yaw * n);
			const double cy = cos(yaw * n);
			const double sr = sin(roll * n);
			const double cr = cos(roll * n);

			r[0][0] = static_cast<float>(cy * cr - sr * sp * sy);
			r[0][1] = static_cast<float>(-(cp * sr));
			r[0][2] = static_cast<float>(sy * cr + sr * sp * cy);

			r[1][0] = static_cast<float>(cy * sr + cr * sp * sy);
			r[1][1] = static_cast<float>(cp * cr);
			r[1][2] = static_cast<float>(sy * sr - cr * sp * cy);

			r[2][0] = static_cast<float>(-(cp * sy));
			r[2][1] = static_cast<float>(sp);
			r[2][2] = static_cast<float>(cp * cy);
		}

		// The per-object rotation, which does NOT follow the camera's convention above.
		//
		// TransformObjectRotated (0x0044E160) never calls sin or cos: it reads the shared trig
		// table allocated at 0x0044AB0E and indexes it at 5400 - rot_x, 5400 - rot_y but
		// 1800 + rot_z. That table is filled from a counter starting at -1800 (0x0044AE11) with
		// angle = counter/10 degrees, so entry i holds sin((i - 1800)/10 deg); the cosine table
		// at 0x00621EA8 is the same array offset by 900 entries, i.e. by +90 degrees. Reading it
		// at 5400 - rot is therefore an evaluation at MINUS the stored angle, and only Z keeps
		// the angle's sign -- which is the whole difference from the camera, where all three are
		// negated alike.
		//
		// The nine products the function then forms (0x0044E2B2 - 0x0044E4FE) compose to
		//     Ry(-rot_y) * Rx(+rot_x) * Rz(-rot_z) * diag(1, -1, 1)
		// -- an improper matrix, because the rotated path multiplies the raw vertex Y while the
		// unrotated fast path subtracts it (0x0044DAF4, `sub esi, eax`). Our buffers carry that
		// negation already (extract_geometry), so the trailing flip cancels and what an object
		// needs is the proper rotation alone.
		//
		// Getting this wrong is invisible until an object pitches: the composition agrees with
		// the camera's for yaw alone and for roll alone, which covers every piece of track
		// scenery and any car on the flat. A car on a slope is the first thing that disagrees.
		void object_rotation(const int16_t rot_x, const int16_t rot_y, const int16_t rot_z,
		                     float r[3][3])
		{
			const double k = game::OBJ_ANGLE_TO_RAD;
			const double sx = sin(rot_x * k), cx = cos(rot_x * k);
			const double sy = sin(rot_y * k), cy = cos(rot_y * k);
			const double sz = sin(rot_z * k), cz = cos(rot_z * k);

			r[0][0] = static_cast<float>(sx * sy * sz + cy * cz);
			r[0][1] = static_cast<float>(cy * sz - sx * sy * cz);
			r[0][2] = static_cast<float>(-(cx * sy));

			r[1][0] = static_cast<float>(-(cx * sz));
			r[1][1] = static_cast<float>(cx * cz);
			r[1][2] = static_cast<float>(-sx);

			r[2][0] = static_cast<float>(sy * cz - sx * cy * sz);
			r[2][1] = static_cast<float>(sx * cy * cz + sy * sz);
			r[2][2] = static_cast<float>(cx * cy);
		}
	}

	namespace
	{
		// Must outlive the patched instructions; they reference it directly.
		double s_render_gate = 1.0;

		bool s_smooth_normals = true;
		float s_smooth_cos_threshold = 0.5f;   // 60 degrees

		void load_smoothing_settings()
		{
			auto& cfg = shared::common::config::get();
			s_smooth_normals = cfg.get_bool("Ignition", "SmoothNormals", true);
			const float deg = cfg.get_float("Ignition", "SmoothAngleDegrees", 60.0f);
			s_smooth_cos_threshold = cosf(deg * 3.14159265f / 180.0f);
		}
	}

	// Redirects the two render-gate comparisons at their instruction operands, so the game
	// draws more than once per 36 Hz logic tick. See ADDR_RenderGateCmp_* for why this is the
	// safe lever and the tick period is not.
	void ignition_inject::patch_render_rate()
	{
		const float mult = shared::common::config::get().get_float("Ignition", "RenderRateMultiplier", 1.0f);
		if (mult <= 1.0f) {
			return;
		}

		s_render_gate = 1.0 / static_cast<double>(mult);

		for (const uint32_t site : { game::ADDR_RenderGateCmp_Delta, game::ADDR_RenderGateCmp_Tick })
		{
			auto* code = reinterpret_cast<uint8_t*>(game::rebase(site));
			if (code[0] != game::FCOMP_M64_OPCODE[0] || code[1] != game::FCOMP_M64_OPCODE[1]) {
				shared::common::log("IgnRate", std::format(
					"render gate at 0x{:08X} is not the expected fcomp ({:02X} {:02X}) - not patching",
					site, code[0], code[1]), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
				return;
			}
		}

		for (const uint32_t site : { game::ADDR_RenderGateCmp_Delta, game::ADDR_RenderGateCmp_Tick })
		{
			auto* operand = reinterpret_cast<uint32_t*>(game::rebase(site) + 2);
			DWORD prot = 0;
			if (VirtualProtect(operand, sizeof(uint32_t), PAGE_EXECUTE_READWRITE, &prot)) {
				*operand = reinterpret_cast<uint32_t>(&s_render_gate);
				VirtualProtect(operand, sizeof(uint32_t), prot, &prot);
			}
		}

		shared::common::log("IgnRate", std::format(
			"render gate lowered to {:.3f} ticks -- up to {:.0f} fps, logic still 36 Hz",
			s_render_gate, 36.0 * mult), shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
	}

	bool ignition_inject::suppress_game_raster()
	{
		static const bool on =
			shared::common::config::get().get_bool("Ignition", "SuppressGameRaster", false);
		return on;
	}

	bool ignition_inject::suppress_world_raster()
	{
		static const bool on =
			shared::common::config::get().get_bool("Ignition", "SuppressWorldRaster", true);
		return on;
	}

	bool ignition_inject::should_drop_game_draw()
	{
		if (!suppress_game_raster()) {
			return false;
		}

		static const bool keep_ui =
			shared::common::config::get().get_bool("Ignition", "KeepGameUI", true);

		// The world is depth tested; Ignition's 2D display-list output is not. That split is
		// carried by the render state, which survives nGlide's buffering, unlike anything we
		// could flag during the game's own display-list walk.
		return keep_ui ? s_game_z_enabled : true;
	}

	bool ignition_inject::suppress_game_blit()
	{
		static const bool on =
			shared::common::config::get().get_bool("Ignition", "SuppressGameBlit", false);
		return on;
	}

	ignition_inject::ignition_inject()
	{
		p_this = this;
		m_queue.reserve(256);
		load_smoothing_settings();
		patch_render_rate();

		// 0x0044D020 tail-calls 0x0044D640 for the focal range the game actually uses, so
		// hooking the former covers both transform paths.
		if (shared::utils::hook::detour(game::rebase(game::ADDR_TransformAllObjects),
			hk_transform_all_objects, reinterpret_cast<void**>(&o_transform_all_objects)))
		{
			shared::common::log("Ignition", "Hooked TransformAllObjects - object-space injection armed.",
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}
		else
		{
			shared::common::log("Ignition", std::format("failed to hook TransformAllObjects @ 0x{:08X}",
				game::ADDR_TransformAllObjects), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}

		if (!shared::utils::hook::detour(game::rebase(game::ADDR_UploadTexture), hk_upload_texture,
			reinterpret_cast<void**>(&o_upload_texture)))
		{
			shared::common::log("Ignition", std::format("failed to hook UploadTexture @ 0x{:08X}",
				game::ADDR_UploadTexture), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}

		if (!shared::utils::hook::detour(game::rebase(game::ADDR_RunDisplayList), hk_run_display_list,
			reinterpret_cast<void**>(&o_run_display_list)))
		{
			shared::common::log("Ignition", std::format("failed to hook RunDisplayList @ 0x{:08X}",
				game::ADDR_RunDisplayList), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}
	}

	// Keeps the raw page. A device may not exist yet -- textures are uploaded during level load,
	// well before the first frame -- so conversion is deferred to first use.
	void ignition_inject::on_texture_uploaded(const int32_t tex_id, const uint8_t* src)
	{
		auto& page = m_texture_pages[tex_id];
		page.pixels.assign(src, src + game::TEXTURE_PIXELS);
		snapshot_palette(page.palette);

		dump_texture_debug(tex_id, src);

		// A re-upload to the same id means new content; drop the stale conversion.
		if (const auto it = m_textures.find(tex_id); it != m_textures.end()) {
			if (it->second) it->second->Release();
			m_textures.erase(it);
		}
	}

	// One-shot dump of the raw inputs to the texture conversion, so the encoding can be
	// determined from real data instead of inferred from the disassembly. Writes the source
	// page exactly as the game passed it, plus the remap table and the format flag that picks
	// between the RGB332 and P_8 branches.
	void ignition_inject::dump_texture_debug(const int32_t tex_id, const uint8_t* src)
	{
		if (m_debug_dumps >= 4 || !shared::common::config::get().get_bool("Ignition", "DumpTextures", false)) {
			return;
		}

		const auto dir = std::filesystem::path("rtx_comp") / "texdump";
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);

		if (m_debug_dumps == 0)
		{
			const auto lut = reinterpret_cast<const uint8_t*>(game::rebase(game::ADDR_g_texRemapLut));
			std::ofstream(dir / "lut.bin", std::ios::binary).write(
				reinterpret_cast<const char*>(lut), 256);

			const int32_t flag = *reinterpret_cast<const int32_t*>(game::rebase(game::ADDR_g_texFormatFlag));
			uint32_t distinct = 0;
			bool seen[256]{};
			for (int i = 0; i < 256; ++i) { if (!seen[lut[i]]) { seen[lut[i]] = true; ++distinct; } }

			shared::common::log("IgnTex", std::format(
				"formatFlag={} -> {} | LUT distinct={} identity={}",
				flag, flag == 0 ? "remap to RGB332 (fmt 0)" : "raw P_8 (fmt 5)",
				distinct, [&] { for (int i = 0; i < 256; ++i) if (lut[i] != i) return "no"; return "yes"; }()),
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}

		std::ofstream(dir / std::format("page_{:04d}.bin", tex_id), std::ios::binary).write(
			reinterpret_cast<const char*>(src), game::TEXTURE_PIXELS);

		++m_debug_dumps;
	}

	// The uploaded pages are GR_TEXFMT_P_8 palette indices -- the format flag at 0x00621E60
	// reads 1 at runtime, so the remap-to-RGB332 branch never runs.
	//
	// The palette is the game's live colour table, snapshotted per upload because it is per
	// level: a single global palette matches the first track and miscolours every later one.
	void ignition_inject::snapshot_palette(uint32_t (&out)[256])
	{
		const uint32_t* table = game::get_color_table();

		bool any = false;
		for (uint32_t i = 0; i < game::COLOR_TABLE_ENTRIES; ++i) {
			if (table[i] != 0) { any = true; break; }
		}

		if (!any) {
			ensure_fallback_palette();
			memcpy(out, m_fallback_palette, sizeof(out));
			return;
		}

		for (uint32_t i = 0; i < game::COLOR_TABLE_ENTRIES; ++i)
		{
			// GrColor_t is ARGB; index 0 is the chroma key and becomes fully transparent.
			const uint32_t c = table[i];
			out[i] = (i == 0) ? (c & 0x00FFFFFFu) : (0xFF000000u | (c & 0x00FFFFFFu));
		}
	}

	// Only reached if the live table has not been populated yet.
	void ignition_inject::ensure_fallback_palette()
	{
		if (m_palette_loaded) {
			return;
		}
		m_palette_loaded = true;

		for (int i = 0; i < 256; ++i) {
			m_fallback_palette[i] = 0xFFFF00FFu;   // magenta, so a missing palette is obvious
		}

		std::ifstream file("SYS.COL", std::ios::binary);
		if (!file) {
			shared::common::log("IgnTex", "no live colour table and SYS.COL missing",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}

		uint8_t header[8]{};
		uint8_t rgb[256 * 3]{};
		file.read(reinterpret_cast<char*>(header), sizeof(header));
		file.read(reinterpret_cast<char*>(rgb), sizeof(rgb));
		if (!file) {
			return;
		}

		for (int i = 0; i < 256; ++i) {
			const uint32_t c = (rgb[i * 3] << 16) | (rgb[i * 3 + 1] << 8) | rgb[i * 3 + 2];
			m_fallback_palette[i] = (i == 0) ? c : (0xFF000000u | c);
		}
	}

	IDirect3DTexture9* ignition_inject::texture_for(IDirect3DDevice9* dev, const int32_t tex_id)
	{
		if (tex_id == game::TEX_ID_INVALID) {
			return m_white_texture;
		}

		if (const auto it = m_textures.find(tex_id); it != m_textures.end()) {
			return it->second ? it->second : m_white_texture;
		}

		const auto page = m_texture_pages.find(tex_id);
		if (page == m_texture_pages.end()) {
			++m_texture_misses;
			if (m_missing_ids.size() < 32) m_missing_ids.insert(tex_id);
			return m_white_texture;
		}

		IDirect3DTexture9* tex = nullptr;
		if (FAILED(dev->CreateTexture(game::TEXTURE_SIZE, game::TEXTURE_SIZE, 1, 0,
			D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) || !tex)
		{
			++m_tex_create_failed;
			m_textures[tex_id] = nullptr;
			return m_white_texture;
		}

		D3DLOCKED_RECT rect{};
		if (SUCCEEDED(tex->LockRect(0, &rect, nullptr, 0)))
		{
			const auto& bytes = page->second.pixels;
			const auto& pal = page->second.palette;
			for (int y = 0; y < game::TEXTURE_SIZE; ++y)
			{
				auto* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(rect.pBits) + y * rect.Pitch);
				for (int x = 0; x < game::TEXTURE_SIZE; ++x) {
					dst[x] = pal[bytes[y * game::TEXTURE_SIZE + x]];
				}
			}
			tex->UnlockRect(0);
		}

		++m_textures_built;
		m_textures[tex_id] = tex;
		return tex;
	}

	ignition_inject::~ignition_inject()
	{
		release_all();
		p_this = nullptr;
	}

	void ignition_inject::release_all()
	{
		for (auto& [mesh, geo] : m_geometry) {
			if (geo.vertex_buffer) geo.vertex_buffer->Release();
		}
		m_geometry.clear();

		for (auto& [id, tex] : m_textures) {
			if (tex) tex->Release();
		}
		m_textures.clear();
		m_texture_pages.clear();

		if (m_sprite_buffer) { m_sprite_buffer->Release(); m_sprite_buffer = nullptr; }
		m_sprite_buffer_verts = 0;

		if (m_white_texture) { m_white_texture->Release(); m_white_texture = nullptr; }
		if (m_vertex_decl) { m_vertex_decl->Release(); m_vertex_decl = nullptr; }
	}

	// Expands one mesh into flat-shaded, per-face-split triangles.
	//
	// The face stream is variable stride and can end early on a terminator, so it is walked
	// sequentially and never indexed. Object-space Y is negated to match the transform, which
	// computes world Y as (object.y - vertex.y).
	bool ignition_inject::extract_geometry(const game::ign_mesh* mesh, std::vector<ffp_vertex>& out,
	                                       std::vector<mesh_part>& parts)
	{
		const auto verts = game::mesh_vertices(mesh);
		const auto face_base = static_cast<const uint8_t*>(game::mesh_faces(mesh));
		const int32_t n_verts = mesh->vertex_count;

		out.clear();
		parts.clear();

		// Gathered first, then sorted by selector so that each texture becomes one draw.
		struct pending_tri
		{
			int32_t tex_sel;
			uint32_t colour;
			bool textured;
			game::face_material material;
			int32_t src_index[3];             // mesh vertex indices, for sharing normals
			float area_nx, area_ny, area_nz;  // un-normalised face normal (length == 2*area)
			ffp_vertex v[3];
		};
		std::vector<pending_tri> tris;

		const uint8_t* cur = face_base;
		for (int32_t i = 0; i < mesh->face_count; ++i)
		{
			const int32_t raw = *reinterpret_cast<const int32_t*>(cur);
			const uint32_t op = static_cast<uint32_t>(raw) & 0xFFu;

			if (op >= std::size(game::FACE_STRIDE)) {
				break;
			}

			const uint8_t stride = game::FACE_STRIDE[op];
			if (stride == 0) {
				break;    // terminator -- this model's face stream ends here
			}

			// Shared by both triangle kinds: flat faces are the textured ones with the UVs
			// collapsed onto the white texture and a colour carried alongside instead.
			const auto add_triangle = [&](const int32_t (&idx)[3], const int32_t (&uv)[3][2],
			                              const int32_t tex_sel, const uint32_t colour,
			                              const bool textured)
			{
				for (const int32_t v : idx) {
					if (v < 0 || v >= n_verts) return;
				}

				ffp_vertex tri[3]{};
				for (int k = 0; k < 3; ++k)
				{
					const auto& s = verts[idx[k]];
					tri[k].x = static_cast<float>(s.x);
					tri[k].y = static_cast<float>(-s.y);
					tri[k].z = static_cast<float>(s.z);
					// Observed U/V span 491..65024, so 1/65536 lands them in 0..1.
					tri[k].u = static_cast<float>(uv[k][0]) / 65536.0f;
					tri[k].v = static_cast<float>(uv[k][1]) / 65536.0f;
				}

				// Face normal, weighted by triangle area via the un-normalised cross
				// product so that large faces dominate the smoothed result.
				const float ax = tri[1].x - tri[0].x, ay = tri[1].y - tri[0].y, az = tri[1].z - tri[0].z;
				const float bx = tri[2].x - tri[0].x, by = tri[2].y - tri[0].y, bz = tri[2].z - tri[0].z;
				float nx = ay * bz - az * by;
				float ny = az * bx - ax * bz;
				float nz = ax * by - ay * bx;
				const float len = sqrtf(nx * nx + ny * ny + nz * nz);

				pending_tri pt{};
				pt.area_nx = nx; pt.area_ny = ny; pt.area_nz = nz;

				if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }
				else { nx = 0.0f; ny = 1.0f; nz = 0.0f; }

				for (auto& t : tri) { t.nx = nx; t.ny = ny; t.nz = nz; }

				pt.tex_sel = tex_sel;
				pt.colour = colour;
				pt.textured = textured;
				pt.material = game::face_material_for(op);
				pt.src_index[0] = idx[0]; pt.src_index[1] = idx[1]; pt.src_index[2] = idx[2];
				pt.v[0] = tri[0]; pt.v[1] = tri[1]; pt.v[2] = tri[2];
				tris.push_back(pt);
			};

			if (game::face_is_textured_tri(op))
			{
				const auto f = reinterpret_cast<const game::ign_face_tri*>(cur);
				const int32_t idx[3] = { f->i0, f->i1, f->i2 };
				const int32_t uv[3][2] = { { f->u0, f->v0 }, { f->u1, f->v1 }, { f->u2, f->v2 } };
				add_triangle(idx, uv, f->tex_sel, 0x00FFFFFFu, true);
			}
			else if (game::face_is_coloured_tri(op))
			{
				const auto f = reinterpret_cast<const game::ign_face_flat*>(cur);
				const uint32_t entry = static_cast<uint32_t>(f->colour) & 0xFFu;

				// The display-list case drops index 0 outright (0x0045105A: test cl,cl / je).
				if (entry != 0)
				{
					const int32_t idx[3] = { f->i0, f->i1, f->i2 };
					const int32_t uv[3][2]{};
					add_triangle(idx, uv, 0, game::get_color_table()[entry] & 0x00FFFFFFu, false);
				}
			}

			cur += stride;
		}

		if (tris.empty()) {
			return false;
		}

		// Smooth normals across faces that share a mesh vertex.
		//
		// Ignition stores no normals, and the per-face UVs force a vertex split per triangle,
		// so flat shading is what falls out naturally -- and it makes the terrain read as
		// faceted. Accumulating the area-weighted face normals per *source* vertex index
		// recovers the shared normal the geometry implies.
		//
		// A crease threshold keeps genuine hard edges hard: if a face disagrees with the
		// accumulated normal by more than the configured angle it keeps its flat normal, so
		// box-like scenery does not turn into a blob.
		if (s_smooth_normals)
		{
			std::vector<float> acc(static_cast<size_t>(n_verts) * 3, 0.0f);
			for (const auto& t : tris) {
				for (const int32_t vi : t.src_index) {
					acc[vi * 3 + 0] += t.area_nx;
					acc[vi * 3 + 1] += t.area_ny;
					acc[vi * 3 + 2] += t.area_nz;
				}
			}

			for (int32_t v = 0; v < n_verts; ++v) {
				float* n = &acc[v * 3];
				const float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				if (l > 1e-6f) { n[0] /= l; n[1] /= l; n[2] /= l; }
			}

			for (auto& t : tris) {
				for (int k = 0; k < 3; ++k) {
					const float* n = &acc[t.src_index[k] * 3];
					const float d = t.v[k].nx * n[0] + t.v[k].ny * n[1] + t.v[k].nz * n[2];
					if (d >= s_smooth_cos_threshold) {
						t.v[k].nx = n[0]; t.v[k].ny = n[1]; t.v[k].nz = n[2];
					}
				}
			}
		}

		// Opaque runs first, so a mesh that mixes solid and translucent faces still composites in
		// the right order when the geometry is rasterized rather than path traced.
		const auto part_key = [](const pending_tri& t) {
			return std::tuple{ t.material.blend, t.material.chroma_keyed, t.material.opacity,
			                   t.textured, t.tex_sel, t.colour };
		};

		std::stable_sort(tris.begin(), tris.end(), [&](const pending_tri& a, const pending_tri& b) {
			return part_key(a) < part_key(b);
		});

		out.reserve(tris.size() * 3);
		for (size_t i = 0; i < tris.size(); ++i)
		{
			if (parts.empty() || part_key(tris[i - 1]) != part_key(tris[i])) {
				parts.push_back({ tris[i].tex_sel, tris[i].colour, tris[i].textured,
				                  tris[i].material, static_cast<uint32_t>(i), 0 });
			}
			++parts.back().triangle_count;
			out.insert(out.end(), tris[i].v, tris[i].v + 3);
		}

		return true;
	}

	// Rebuilds one cache entry from the mesh as it stands right now, reusing the vertex buffer
	// whenever it is still the right size.
	bool ignition_inject::fill_geometry(IDirect3DDevice9* dev, const game::ign_mesh* mesh,
	                                    mesh_geometry& geo)
	{
		std::vector<ffp_vertex> vertices;
		std::vector<mesh_part> parts;
		if (!extract_geometry(mesh, vertices, parts)) {
			return false;
		}

		const auto bytes = static_cast<UINT>(vertices.size() * sizeof(ffp_vertex));

		if (geo.vertex_buffer && geo.vertex_count != vertices.size()) {
			geo.vertex_buffer->Release();
			geo.vertex_buffer = nullptr;
		}

		if (!geo.vertex_buffer
			&& FAILED(dev->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED,
				&geo.vertex_buffer, nullptr)))
		{
			++m_vb_create_failed;
			geo.vertex_buffer = nullptr;
			return false;
		}

		void* dst = nullptr;
		if (FAILED(geo.vertex_buffer->Lock(0, bytes, &dst, 0))) {
			return false;
		}
		memcpy(dst, vertices.data(), bytes);
		geo.vertex_buffer->Unlock();

		geo.vertex_count = static_cast<uint32_t>(vertices.size());
		geo.triangle_count = geo.vertex_count / 3;
		geo.parts = std::move(parts);
		return true;
	}

	// Ignition animates by editing meshes in place -- measured live in a Canada race, 57 of the
	// 4619 textured faces on screen move their UVs in whole 64x64 tile steps (0x4000) and flip
	// texSel between texture pages, while g_texTable and the colour table stay byte-identical.
	// Car meshes deform the same way through their vertex array. So a cache keyed only on the
	// mesh pointer freezes every animated surface on whatever frame it was first seen.
	//
	// The signature covers exactly what extract_geometry consumes, and is taken once per mesh per
	// scene: walking the streams is a few hundred kilobytes of reads, far cheaper than re-running
	// extraction, so only the handful of meshes that actually changed pay for a rebuild.
	const ignition_inject::mesh_geometry* ignition_inject::geometry_for(IDirect3DDevice9* dev,
	                                                                    game::ign_mesh* mesh)
	{
		if (const auto it = m_geometry.find(mesh); it != m_geometry.end())
		{
			auto& geo = it->second;
			geo.last_used_scene = m_scenes_submitted;

			if (geo.last_checked_scene != m_scenes_submitted)
			{
				geo.last_checked_scene = m_scenes_submitted;
				if (const uint64_t sig = mesh_signature(mesh); sig != geo.signature)
				{
					++m_geometry_rebuilds;
					if (!fill_geometry(dev, mesh, geo)) {
						return nullptr;
					}
					// Only once the rebuild succeeded, so a failed one is retried rather than
					// leaving the entry marked current with stale contents.
					geo.signature = sig;
				}
			}

			return &geo;
		}

		mesh_geometry geo{};
		geo.signature = mesh_signature(mesh);
		geo.last_used_scene = m_scenes_submitted;
		geo.last_checked_scene = m_scenes_submitted;
		if (!fill_geometry(dev, mesh, geo)) {
			if (geo.vertex_buffer) geo.vertex_buffer->Release();
			return nullptr;
		}

		return &(m_geometry[mesh] = std::move(geo));
	}

	// Sprites are gathered per object per frame rather than cached with the mesh: their records
	// animate, and the quad they become is camera-facing, so nothing about them survives a frame.
	void ignition_inject::collect_sprites(const game::ign_object* obj, const D3DMATRIX& world)
	{
		const auto mesh = obj->mesh;
		const auto verts = game::mesh_vertices(mesh);
		const int32_t n_verts = mesh->vertex_count;

		// The emitter takes the texture from the first page of the object's group and never from
		// a per-face selector (0x004509AA), which a zero selector reproduces exactly.
		game::tex_resolve why{};
		const int32_t tex_id = game::resolve_texture_id(0, obj->tex_page, why);

		const auto* cur = static_cast<const uint8_t*>(game::mesh_faces(mesh));
		for (int32_t i = 0; i < mesh->face_count; ++i)
		{
			const uint32_t op = static_cast<uint32_t>(*reinterpret_cast<const int32_t*>(cur)) & 0xFFu;
			if (op >= std::size(game::FACE_STRIDE)) {
				break;
			}

			const uint8_t stride = game::FACE_STRIDE[op];
			if (stride == 0) {
				break;
			}

			if (game::face_is_sprite(op))
			{
				const auto f = reinterpret_cast<const game::ign_face_sprite*>(cur);
				if (f->vertex >= 0 && f->vertex < n_verts
					&& (f->scale_x > 0 || f->scale_y > 0))
				{
					const auto& s = verts[f->vertex];
					const float ox = static_cast<float>(s.x);
					const float oy = static_cast<float>(-s.y);
					const float oz = static_cast<float>(s.z);

					sprite_instance sp{};
					sp.x = ox * world._11 + oy * world._21 + oz * world._31 + world._41;
					sp.y = ox * world._12 + oy * world._22 + oz * world._32 + world._42;
					sp.z = ox * world._13 + oy * world._23 + oz * world._33 + world._43;
					sp.scale_x = f->scale_x;
					sp.scale_y = f->scale_y;
					sp.u0 = static_cast<float>(f->u0) / 65536.0f;
					sp.v0 = static_cast<float>(f->v0) / 65536.0f;
					sp.u1 = static_cast<float>(f->u1) / 65536.0f;
					sp.v1 = static_cast<float>(f->v1) / 65536.0f;
					sp.tex_id = tex_id;
					sp.material = game::sprite_material_for(op,
						static_cast<uint8_t>(f->opacity & 0xFF));
					m_sprites.push_back(sp);
				}
			}

			cur += stride;
		}
	}

	D3DMATRIX ignition_inject::build_world(const game::ign_object* obj)
	{
		D3DMATRIX m{};

		float r[3][3];
		object_rotation(obj->rot_x, obj->rot_y, obj->rot_z, r);

		// Row-vector convention: world = vertex * M.
		m._11 = r[0][0]; m._12 = r[1][0]; m._13 = r[2][0]; m._14 = 0.0f;
		m._21 = r[0][1]; m._22 = r[1][1]; m._23 = r[2][1]; m._24 = 0.0f;
		m._31 = r[0][2]; m._32 = r[1][2]; m._33 = r[2][2]; m._34 = 0.0f;
		m._41 = static_cast<float>(obj->pos_x);
		m._42 = static_cast<float>(obj->pos_y);
		m._43 = static_cast<float>(obj->pos_z);
		m._44 = 1.0f;

		return m;
	}

	bool ignition_inject::build_view(D3DMATRIX& out) const
	{
		float r[3][3];
		camera_rotation(m_scene.pitch_deg, m_scene.yaw_deg, m_scene.roll_deg, r);

		// The game scales rows 0 and 1 by NEGATIVE focal lengths (0x0044D640: `(float)-focalX`
		// and `(float)-focalY`), and its screen Y grows downward. Folding both facts in:
		//     sx_px = center_x - focal_x*zoom * (v.R0)/(v.R2)   -> R0 points LEFT
		//     sy_px = center_y - focal_y      * (v.R1)/(v.R2)   -> R1 points UP
		// so the game's basis is (left, up, forward) while D3D view space is
		// (right, up, forward). Negating row 0 converts between them; without it the world is
		// mirrored, which is also why Remix decomposed the camera as left-handed.
		r[0][0] = -r[0][0];
		r[0][1] = -r[0][1];
		r[0][2] = -r[0][2];

		const float cam[3] = {
			static_cast<float>(m_scene.cam_x),
			static_cast<float>(m_scene.cam_y),
			static_cast<float>(m_scene.cam_z),
		};

		// viewPos = R * (worldPos - cam); in row-vector form that is the transpose of R.
		out._11 = r[0][0]; out._12 = r[1][0]; out._13 = r[2][0]; out._14 = 0.0f;
		out._21 = r[0][1]; out._22 = r[1][1]; out._23 = r[2][1]; out._24 = 0.0f;
		out._31 = r[0][2]; out._32 = r[1][2]; out._33 = r[2][2]; out._34 = 0.0f;
		out._41 = -(cam[0] * r[0][0] + cam[1] * r[0][1] + cam[2] * r[0][2]);
		out._42 = -(cam[0] * r[1][0] + cam[1] * r[1][1] + cam[2] * r[1][2]);
		out._43 = -(cam[0] * r[2][0] + cam[1] * r[2][1] + cam[2] * r[2][2]);
		out._44 = 1.0f;

		return true;
	}

	bool ignition_inject::build_projection(D3DMATRIX& out) const
	{
		if (m_scene.focal_x <= 0 || m_scene.focal_y <= 0 || m_scene.center_x <= 0) {
			return false;
		}

		// Ignition's screen mapping, worked through from 0x0044D640:
		//   row0/row1 are scaled by focal * 16384/64 == focal * 256, then row0 by zoom
		//   row2 is scaled by 262144, and w = (v.row2 >> 16) + near, so w ~= 4 * view_z
		//   sx_fixed = center_x*256 + ((v.row0 - ofsX) / w) * 4
		// The 4 in the output cancels the 4 in w, and the 256 cancels the 24.8 shift:
		//   sx_px = center_x + (view_x / view_z) * focal_x * zoom
		// so the pixel focal length is focal_x * zoom, with no factor of 4.
		//
		// Note 0x0044D020 folds that 4 into rows 0/1 instead and divides without it, so both
		// paths agree; taking the scale from the wrong one makes the frustum 4x too narrow.
		static const float scale =
			shared::common::config::get().get_float("Ignition", "FovScale", 1.0f);
		const double zoom = (m_scene.zoom != 0.0) ? fabs(m_scene.zoom) : 1.0;
		const float fx = static_cast<float>(m_scene.focal_x * zoom) * scale;
		const float fy = static_cast<float>(m_scene.focal_y) * scale;

		if (fx <= 0.0f || fy <= 0.0f) {
			return false;
		}

		const float half_w = static_cast<float>(m_scene.center_x);
		const float half_h = static_cast<float>(m_scene.center_y > 0 ? m_scene.center_y : 200);

		// Left-handed perspective straight from the focal lengths, so a non-square viewport
		// (the race view is a letterboxed band) keeps its correct aspect.
		// near_clamp is the game's own near limit in view-z units; using it instead of 1.0
		// keeps the depth range tight enough to be useful.
		const float near_z = static_cast<float>(m_scene.near_clamp > 0 ? m_scene.near_clamp : 1);
		const float far_z = 200000.0f;

		memset(&out, 0, sizeof(out));
		out._11 = fx / half_w;
		out._22 = fy / half_h;
		out._33 = far_z / (far_z - near_z);
		out._34 = 1.0f;
		out._43 = -near_z * far_z / (far_z - near_z);

		return true;
	}

	void ignition_inject::capture_scene()
	{
		++m_captures;

		// Only the first viewport of a frame is forwarded: split-screen and the mirror-style
		// sub-views would otherwise hand Remix a second camera for the same frame.
		//
		// This must return before touching the queue. Clearing first would let a later
		// viewport in the same frame discard the geometry the first one captured, and since
		// the queue is not refilled until the next frame that frame submits nothing at all --
		// which showed up as scenery blinking out whenever a second view was on screen.
		const auto scene = game::get_scene();
		const auto objects = game::get_object_list();
		if (!scene || !objects || scene->object_count <= 0) {
			++m_captures_no_scene;
			return;
		}

		const auto dev = shared::globals::d3d_device;
		if (!dev) {
			++m_captures_no_device;
			return;
		}

		const bool first_of_frame = (m_viewport_index++ == 0);

		if (first_of_frame)
		{
			m_scene = *scene;
			m_scene_valid = true;
			m_queue.clear();
			m_sprites.clear();
		}
		else
		{
			// Ignition draws the world in more than one pass per frame, each carrying a
			// different slice of the object list -- counts swing between roughly 130 and 350
			// while the camera barely moves. Dropping the later passes made near and far
			// scenery alternate on and off, which is why the single-pass pause menu always
			// looked complete. Passes that share the camera are merged instead.
			//
			// A genuinely different camera means a separate view (split screen, mirrors), and
			// those must still be dropped: Remix takes one camera per frame.
			const double dx = scene->cam_x - m_scene.cam_x;
			const double dy = scene->cam_y - m_scene.cam_y;
			const double dz = scene->cam_z - m_scene.cam_z;

			const bool same_camera = !m_scene_valid ? false :
				(dx * dx + dy * dy + dz * dz) < 1.0
				&& scene->pitch_deg == m_scene.pitch_deg
				&& scene->yaw_deg == m_scene.yaw_deg
				&& scene->roll_deg == m_scene.roll_deg;

			if (!same_camera) {
				++m_captures_skipped_viewport;
				return;
			}

			++m_captures_merged_pass;
		}

		for (int32_t i = 0; i < scene->object_count; ++i)
		{
			const auto obj = objects[i];
			if (!obj || !obj->mesh) {
				++m_obj_no_mesh;
				continue;
			}

			const auto mesh = obj->mesh;
			if (mesh->vertex_count <= 0 || mesh->vertex_count > MAX_SANE_VERTICES
				|| mesh->face_count <= 0 || mesh->face_count > MAX_SANE_FACES) {
				++m_obj_insane_counts;
				continue;
			}

			const D3DMATRIX world = build_world(obj);
			collect_sprites(obj, world);

			const auto geo = geometry_for(dev, mesh);
			if (!geo || !geo->triangle_count) {
				++m_obj_extract_failed;
			}

			if (geo && geo->triangle_count) {
				m_queue.push_back({ geo, world, obj->tex_page });
			}
		}
	}

	// Submit where Carmageddon 2 does: at the end of the game's own scene walk.
	//
	// This is the point before nGlide has begun its frame, so the back buffer is still the
	// bound render target and no offscreen surface is in the way. nGlide only opens its single
	// BeginScene/EndScene pair later, when it flushes buffered Glide triangles -- which is why
	// submitting inside that window meant drawing into its private surface, where Remix never
	// looks. The one thing missing here is an open scene, so we open our own.
	void ignition_inject::on_present()
	{
		++m_presents;

		// The game's logic runs at a fixed 36 Hz (0x004116A0 divides elapsed milliseconds by
		// 27.7778), but rendering is called once per main-loop iteration and nothing gates it.
		// Measuring presents against captures tells us whether the observed 36 fps is the game
		// or something outside it.
		{
			const auto now = GetTickCount64();
			if (m_rate_window_start == 0) {
				m_rate_window_start = now;
				m_rate_presents = m_presents;
				m_rate_captures = m_captures;
			}
			else if (now - m_rate_window_start >= 2000)
			{
				const double secs = static_cast<double>(now - m_rate_window_start) / 1000.0;
				shared::common::log("IgnRate", std::format(
					"presents/s={:.1f} avgSubmit={:.2f}ms (logic is fixed 36 Hz; "
					"submit is our own cost, the rest is game + nGlide + Remix)",
					(m_presents - m_rate_presents) / secs,
					m_submit_ms_total / (m_presents - m_rate_presents ? m_presents - m_rate_presents : 1)),
					shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, true);
				m_rate_window_start = now;
				m_rate_presents = m_presents;
				m_rate_captures = m_captures;
				m_submit_ms_total = 0.0;
			}
		}

		// Present is the real frame boundary, so viewport counting restarts here.
		m_viewport_index = 0;
		s_game_draws_this_frame = 0;

		if ((m_presents % 300) == 0)
		{
			const auto& c = s_census;
			shared::common::log("IgnAPI", std::format(
				"per-{}-frames: DrawPrim={} DrawIdx={} DrawPrimUP={} DrawIdxUP={} | "
				"StretchRect={} UpdateSurface={} ColorFill={} SetRT={} (nonzeroIdx={}) | "
				"BeginScene={} submits={} queued={}",
				300, c.draw_prim, c.draw_indexed, c.draw_prim_up, c.draw_indexed_up,
				c.stretch_rect, c.update_surface, c.color_fill, c.set_render_target,
				c.set_rt_nonzero, c.begin_scene, m_submits, m_queue.size()),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, true);

			s_census = api_census{};
		}
	}

	void ignition_inject::on_begin_scene_submit()
	{
		++m_end_scenes;
		++s_census.begin_scene;

		if ((m_end_scenes % 300) == 0)
		{
			if (!s_list_callers.empty())
			{
				std::string callers;
				for (const auto& [ret, count] : s_list_callers) {
					callers += std::format("{}0x{:08X}x{}", callers.empty() ? "" : " ", ret, count);
				}
				shared::common::log("IgnLists", std::format("non-world RunDisplayList callers: {}", callers),
					shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, true);
			}

			shared::common::log("Ignition", std::format(
				"captures={} (noScene={} noDevice={} skippedViewport={}) endScenes={} submits={} "
				"lastDraws={} lastVerts={} lastSprites={} meshes={} tex(built={} pages={} miss={} oob={} unset={} missIds={}) "
				"rebuilds={} fail(vb={} tex={} noMesh={} insane={} extract={}) merged={} "
				"lists(world={} dropped={} other={}) lastDrawErr=0x{:08X}",
				m_captures, m_captures_no_scene, m_captures_no_device, m_captures_skipped_viewport,
				m_end_scenes, m_submits, m_last_draws, m_last_vertices, m_last_sprites, m_geometry.size(),
				m_textures_built, m_texture_pages.size(), m_texture_misses,
				m_tex_index_oob, m_tex_entry_unset, m_missing_ids.size(), m_geometry_rebuilds,
				m_vb_create_failed, m_tex_create_failed, m_obj_no_mesh, m_obj_insane_counts,
				m_obj_extract_failed, m_captures_merged_pass,
				s_world_lists_seen, s_world_lists_dropped, s_other_lists_seen,
				static_cast<uint32_t>(m_last_draw_error)),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, true);
		}

		if (m_scene_valid && (!m_queue.empty() || !m_sprites.empty()))
		{
			if (const auto dev = shared::globals::d3d_device; dev) {
				submit(dev);
			}
		}

		m_scene_valid = false;
		m_queue.clear();
		m_sprites.clear();

		// Viewport counting is NOT reset here. Present is the frame boundary; resetting
		// mid-frame lets a second RenderScene in the same frame pass as viewport 0 and replace
		// the captured scene with a different camera, so the submitted view alternates between
		// frames and motion vectors never settle.
	}

	void ignition_inject::ensure_white_texture(IDirect3DDevice9* dev)
	{
		if (m_white_texture) {
			return;
		}

		if (FAILED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
			&m_white_texture, nullptr))) {
			return;
		}

		D3DLOCKED_RECT rect{};
		if (SUCCEEDED(m_white_texture->LockRect(0, &rect, nullptr, 0))) {
			*static_cast<uint32_t*>(rect.pBits) = 0xFFFFFFFFu;
			m_white_texture->UnlockRect(0);
		}
	}

	void ignition_inject::evict_stale_geometry()
	{
		for (auto it = m_geometry.begin(); it != m_geometry.end(); )
		{
			if (m_scenes_submitted - it->second.last_used_scene > GEOMETRY_EVICT_AFTER_SCENES)
			{
				if (it->second.vertex_buffer) it->second.vertex_buffer->Release();
				it = m_geometry.erase(it);
			}
			else {
				++it;
			}
		}
	}

	// Reproduces the Glide state the face's rasterizer would have set.
	//
	// Remix derives its own translucency from exactly these render states
	// (rtx_instance_manager.cpp: SRC_ALPHA/ONE_MINUS_SRC_ALPHA becomes BlendType::kAlpha,
	// SRC_ALPHA/ONE becomes kAlphaEmissive), and replays the texture-stage alpha ops and
	// D3DRS_TEXTUREFACTOR in its own shader, so the opacity reaches the path tracer intact.
	void ignition_inject::apply_material(IDirect3DDevice9* dev, const game::face_material& mat,
	                                     const uint32_t colour)
	{
		// Palette index 0 is written with alpha 0. Chroma-keyed faces discard it; the rest keep
		// drawing it black, exactly as the game does.
		dev->SetRenderState(D3DRS_ALPHATESTENABLE, mat.chroma_keyed ? TRUE : FALSE);
		dev->SetRenderState(D3DRS_TEXTUREFACTOR,
			(static_cast<DWORD>(mat.opacity) << 24) | (colour & 0x00FFFFFFu));

		const bool blended = mat.blend != game::face_blend::opaque;
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, blended ? TRUE : FALSE);
		dev->SetRenderState(D3DRS_SRCBLEND, blended ? D3DBLEND_SRCALPHA : D3DBLEND_ONE);
		dev->SetRenderState(D3DRS_DESTBLEND,
			mat.blend == game::face_blend::additive ? D3DBLEND_ONE :
			mat.blend == game::face_blend::alpha    ? D3DBLEND_INVSRCALPHA : D3DBLEND_ZERO);

		// Ignition sorts back-to-front and owns no depth buffer, so translucent faces never wrote
		// depth. Keeping that is what stops a cloud from occluding what is behind it.
		dev->SetRenderState(D3DRS_ZWRITEENABLE, blended ? FALSE : TRUE);
	}

	// Expands the gathered sprites into camera-facing quads.
	//
	// The game sizes a sprite in screen space, across two steps. Emitter 0x00450860 scales the
	// record's value by the depth (`scale * 4.0 / w`), and rasterizer 0x00452DF0 then multiplies
	// that by half the UV span, shifting both down by 8 first:
	//
	//     half_fixed = (halfUV >> 8) * ((scale * 4 / w) >> 8)
	//
	// Undoing it against the projection we hand Remix -- half_px = W * focal / view_z, screen
	// units being 24.8 fixed, and w = 4 * view_z -- leaves
	//
	//     W = halfUV_fraction * scale / (256 * focal)
	//
	// with the depth term cancelling as it must for a world-space size. Dropping the UV term is
	// what made the first version eight times too large for a 64x64 frame on a 256x256 page.
	void ignition_inject::submit_sprites(IDirect3DDevice9* dev, const D3DMATRIX& view)
	{
		const double zoom = (m_scene.zoom != 0.0) ? fabs(m_scene.zoom) : 1.0;
		const float focal_x = static_cast<float>(m_scene.focal_x * zoom);
		const float focal_y = static_cast<float>(m_scene.focal_y);
		if (focal_x <= 0.0f || focal_y <= 0.0f) {
			return;
		}

		// Columns of the view rotation are the world-space axes of the camera basis.
		const float right[3] = { view._11, view._21, view._31 };
		const float up[3]    = { view._12, view._22, view._32 };
		const float fwd[3]   = { view._13, view._23, view._33 };

		std::stable_sort(m_sprites.begin(), m_sprites.end(),
			[](const sprite_instance& a, const sprite_instance& b) {
				return std::tuple{ a.material.blend, a.material.opacity, a.tex_id }
				     < std::tuple{ b.material.blend, b.material.opacity, b.tex_id };
			});

		m_sprite_vertices.clear();
		m_sprite_vertices.reserve(m_sprites.size() * 6);

		struct sprite_run { uint32_t first_vertex, vertex_count; int32_t tex_id; game::face_material material; };
		std::vector<sprite_run> runs;

		for (const auto& sp : m_sprites)
		{
			// The emitter drops the sprite when the anchor's w is at or below 200, and w is four
			// times the view depth.
			const float view_z = sp.x * fwd[0] + sp.y * fwd[1] + sp.z * fwd[2] + view._43;
			if (view_z <= 50.0f) {
				continue;
			}

			const float half_u = fabsf(sp.u1 - sp.u0) * 0.5f;
			const float half_v = fabsf(sp.v1 - sp.v0) * 0.5f;
			const float hw = half_u * static_cast<float>(sp.scale_x) / (256.0f * focal_x);
			const float hh = half_v * static_cast<float>(sp.scale_y) / (256.0f * focal_y);

			ffp_vertex corner[4]{};
			const float sx[4] = { -hw,  hw,  hw, -hw };
			const float sy[4] = {  hh,  hh, -hh, -hh };
			const float cu[4] = { sp.u0, sp.u1, sp.u1, sp.u0 };
			const float cv[4] = { sp.v0, sp.v0, sp.v1, sp.v1 };

			for (int k = 0; k < 4; ++k)
			{
				corner[k].x = sp.x + right[0] * sx[k] + up[0] * sy[k];
				corner[k].y = sp.y + right[1] * sx[k] + up[1] * sy[k];
				corner[k].z = sp.z + right[2] * sx[k] + up[2] * sy[k];
				corner[k].nx = -fwd[0];
				corner[k].ny = -fwd[1];
				corner[k].nz = -fwd[2];
				corner[k].u = cu[k];
				corner[k].v = cv[k];
			}

			if (runs.empty() || runs.back().tex_id != sp.tex_id
				|| runs.back().material.blend != sp.material.blend
				|| runs.back().material.opacity != sp.material.opacity)
			{
				runs.push_back({ static_cast<uint32_t>(m_sprite_vertices.size()), 0,
				                 sp.tex_id, sp.material });
			}

			for (const int k : { 0, 1, 2, 0, 2, 3 }) {
				m_sprite_vertices.push_back(corner[k]);
			}
			runs.back().vertex_count += 6;
		}

		if (m_sprite_vertices.empty()) {
			return;
		}

		const auto bytes = static_cast<UINT>(m_sprite_vertices.size() * sizeof(ffp_vertex));

		if (m_sprite_buffer && m_sprite_buffer_verts < m_sprite_vertices.size()) {
			m_sprite_buffer->Release();
			m_sprite_buffer = nullptr;
		}

		if (!m_sprite_buffer)
		{
			m_sprite_buffer_verts = static_cast<uint32_t>(m_sprite_vertices.size()) * 2;
			if (FAILED(dev->CreateVertexBuffer(m_sprite_buffer_verts * sizeof(ffp_vertex),
				D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &m_sprite_buffer, nullptr)))
			{
				++m_vb_create_failed;
				m_sprite_buffer = nullptr;
				return;
			}
		}

		void* dst = nullptr;
		if (FAILED(m_sprite_buffer->Lock(0, bytes, &dst, 0))) {
			return;
		}
		memcpy(dst, m_sprite_vertices.data(), bytes);
		m_sprite_buffer->Unlock();

		D3DMATRIX identity{};
		identity._11 = identity._22 = identity._33 = identity._44 = 1.0f;
		dev->SetTransform(D3DTS_WORLD, &identity);
		dev->SetStreamSource(0, m_sprite_buffer, 0, sizeof(ffp_vertex));

		for (const auto& run : runs)
		{
			dev->SetTexture(0, texture_for(dev, run.tex_id));
			apply_material(dev, run.material, 0x00FFFFFFu);
			dev->DrawPrimitive(D3DPT_TRIANGLELIST, run.first_vertex, run.vertex_count / 3);
		}

		m_last_sprites = static_cast<uint32_t>(m_sprite_vertices.size() / 6);

		// The size derivation is the one part of the sprite path that cannot be checked against
		// the disassembly alone, so report it once with the numbers that went into it.
		if (!m_logged_first_sprites)
		{
			m_logged_first_sprites = true;
			const auto& sp = m_sprites.front();
			shared::common::log("IgnSprite", std::format(
				"first sprite submit: quads={} runs={} focal=({:.0f},{:.0f}) "
				"scale=({},{}) halfUV=({:.4f},{:.4f}) -> world half=({:.2f},{:.2f}) "
				"uv=({:.3f},{:.3f})-({:.3f},{:.3f}) tex={} opacity={} blend={}",
				m_last_sprites, runs.size(), focal_x, focal_y,
				sp.scale_x, sp.scale_y,
				fabsf(sp.u1 - sp.u0) * 0.5f, fabsf(sp.v1 - sp.v0) * 0.5f,
				fabsf(sp.u1 - sp.u0) * 0.5f * static_cast<float>(sp.scale_x) / (256.0f * focal_x),
				fabsf(sp.v1 - sp.v0) * 0.5f * static_cast<float>(sp.scale_y) / (256.0f * focal_y),
				sp.u0, sp.v0, sp.u1, sp.v1, sp.tex_id, sp.material.opacity,
				static_cast<int>(sp.material.blend)),
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}
	}

	void ignition_inject::submit(IDirect3DDevice9* dev)
	{
		D3DMATRIX view{}, projection{};
		if (!build_view(view) || !build_projection(projection)) {
			return;
		}

		// nGlide owns the device for the rest of the frame, so every state this replay
		// touches is captured and put back afterwards.
		// Create the state block once and re-record it each frame. CreateStateBlock(D3DSBT_ALL)
		// allocates and snapshots the entire device state; doing that per frame is expensive
		// enough to show up in frame time on its own.
		if (!m_state_block) {
			if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &m_state_block)) || !m_state_block) {
				return;
			}
		}
		else if (FAILED(m_state_block->Capture())) {
			return;
		}

		LARGE_INTEGER submit_begin{};
		QueryPerformanceCounter(&submit_begin);

		// nGlide renders into an offscreen target and blits it to the back buffer once per
		// frame (the census shows exactly one StretchRect and three SetRenderTarget calls per
		// frame, and zero non-UP draws). Remix only builds its scene from draws to the PRIMARY
		// render target, so geometry submitted while nGlide's offscreen target is bound is
		// merely rasterized into that surface and blitted -- never raytraced.
		dev->SetVertexShader(nullptr);
		dev->SetPixelShader(nullptr);

		if (!m_vertex_decl) {
			dev->CreateVertexDeclaration(INJECT_DECL, &m_vertex_decl);
		}
		dev->SetVertexDeclaration(m_vertex_decl);

		dev->SetTransform(D3DTS_VIEW, &view);
		dev->SetTransform(D3DTS_PROJECTION, &projection);

		dev->SetRenderState(D3DRS_LIGHTING, FALSE);
		dev->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
		dev->SetRenderState(D3DRS_ALPHAREF, 0);
		dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
		dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
		// Ignition picks its winding at runtime from the sign of scene->zoom, so neither
		// winding can be assumed here.
		dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

		ensure_white_texture(dev);
		// Modulating by the texture factor rather than selecting the texture lets flat-shaded
		// faces share this setup: they bind the white texture and put their palette colour in the
		// factor, while textured faces leave the factor's RGB at white and are unaffected.
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);

		// Glide takes the blend alpha purely from the constant colour
		// (guAlphaSource(GR_ALPHASOURCE_CC_ALPHA) in every translucent rasterizer), and cuts the
		// chroma key with a separate test. Modulating texture alpha by the texture factor
		// expresses both at once: the factor carries the opacity, while the zero alpha we give
		// palette index 0 survives for the alpha test to discard.
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
		dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

		// Remix matches stage 0's TEXCOORDINDEX against the declaration's UsageIndex; leaving
		// whatever nGlide last set makes it reject TEXCOORD0 and report the mesh as having no
		// UVs. A stale texture transform would corrupt the coordinates just as badly.
		dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
		dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);

		dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
		dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

		// Without these we inherit whatever nGlide last bound, which is point sampling -- the
		// software-renderer look. These are 256x256 pages stretched over large track polygons,
		// so the filter choice is very visible.
		dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);

		// Stale bindings from the Glide path would otherwise be captured as this geometry's
		// material by Remix.
		for (DWORD stage = 1; stage < 8; ++stage) {
			dev->SetTexture(stage, nullptr);
		}

		uint32_t draws = 0, vertices = 0;
		const uint32_t game_draws_before_us = s_game_draws_this_frame;

		s_injecting = true;
		for (const auto& inst : m_queue)
		{
			dev->SetTransform(D3DTS_WORLD, &inst.world);
			dev->SetStreamSource(0, inst.geometry->vertex_buffer, 0, sizeof(ffp_vertex));

			for (const auto& part : inst.geometry->parts)
			{
				if (part.textured)
				{
					game::tex_resolve why{};
					const int32_t tex_id = game::resolve_texture_id(part.tex_sel, inst.tex_page, why);
					if (why == game::tex_resolve::index_out_of_range) ++m_tex_index_oob;
					else if (why == game::tex_resolve::table_entry_unset) ++m_tex_entry_unset;

					dev->SetTexture(0, texture_for(dev, tex_id));
				}
				else {
					dev->SetTexture(0, m_white_texture);
				}

				apply_material(dev, part.material, part.colour);

				const HRESULT hr = dev->DrawPrimitive(D3DPT_TRIANGLELIST,
					part.first_triangle * 3, part.triangle_count);
				if (SUCCEEDED(hr))
				{
					++draws;
					vertices += part.triangle_count * 3;
				}
				else {
					m_last_draw_error = hr;
				}
			}
		}
		// After the world, so translucent particles composite over it.
		m_last_sprites = 0;
		if (!m_sprites.empty()) {
			submit_sprites(dev, view);
		}
		s_injecting = false;

		m_state_block->Apply();

		LARGE_INTEGER submit_end{}, freq{};
		QueryPerformanceCounter(&submit_end);
		QueryPerformanceFrequency(&freq);
		m_submit_ms_total += static_cast<double>(submit_end.QuadPart - submit_begin.QuadPart)
			* 1000.0 / static_cast<double>(freq.QuadPart);

		if (!m_logged_first_submit)
		{
			m_logged_first_submit = true;
			shared::common::log("Ignition", std::format(
				"first submit: gameDrawsBeforeUs={} decl={} queued={} draws={} verts={} cam=({:.1f},{:.1f},{:.1f}) "
				"euler=({:.1f},{:.1f},{:.1f}) focal=({},{}) center=({},{}) drawErr=0x{:08X}",
				game_draws_before_us, m_vertex_decl ? "ok" : "NULL(!!)",
				m_queue.size(), draws, vertices,
				m_scene.cam_x, m_scene.cam_y, m_scene.cam_z,
				m_scene.pitch_deg, m_scene.yaw_deg, m_scene.roll_deg,
				m_scene.focal_x, m_scene.focal_y, m_scene.center_x, m_scene.center_y,
				static_cast<uint32_t>(m_last_draw_error)),
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}

		// Per-frame camera telemetry. "forward" is view row 2, which is what the orientation
		// should be judged on; logging it every few frames makes an oscillation obvious in the
		// log instead of only on screen.
		if ((m_submits % 30) == 0)
		{
			shared::common::log("IgnCam", std::format(
				"gameDrawsBeforeUs={} objs={} draws={} pos=({:.0f},{:.0f},{:.0f}) euler=({:.2f},{:.2f},{:.2f}) "
				"fwd=({:.3f},{:.3f},{:.3f}) up=({:.3f},{:.3f},{:.3f}) zoom={:.3f}",
				game_draws_before_us, m_scene.object_count, draws,
				m_scene.cam_x, m_scene.cam_y, m_scene.cam_z,
				m_scene.pitch_deg, m_scene.yaw_deg, m_scene.roll_deg,
				view._13, view._23, view._33,
				view._12, view._22, view._32,
				m_scene.zoom),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, true);
		}

		m_last_draws = draws;
		m_last_vertices = vertices;
		++m_submits;
		++m_scenes_submitted;

		if ((m_scenes_submitted % 600) == 0) {
			evict_stale_geometry();
		}
	}
}
