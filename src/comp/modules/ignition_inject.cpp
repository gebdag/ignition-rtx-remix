#include "std_include.hpp"

#include "ignition_inject.hpp"
#include "shared/common/config.hpp"

#include <filesystem>
#include <fstream>

namespace comp
{
	namespace
	{
		using namespace game;

		game::RenderScene_t o_render_scene = nullptr;
		game::UploadTexture_t o_upload_texture = nullptr;

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

		void __cdecl hk_render_scene()
		{
			const auto self = ignition_inject::get();

			if (self) {
				self->capture_scene();
			}

			o_render_scene();

			if (self) {
				self->finish_scene();
			}
		}

		constexpr D3DVERTEXELEMENT9 INJECT_DECL[] = {
			{ 0,  0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
			{ 0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL,   0 },
			{ 0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
			D3DDECL_END()
		};

		// The rotation Ignition builds in TransformAllObjects, without the focal/depth scales
		// it folds into rows 0/1/2 (reproduced from 0x0044D640).
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
		void euler_to_rotation(const double pitch, const double yaw, const double roll,
		                       const double angle_to_rad_neg, float r[3][3])
		{
			const double sp = sin(pitch * angle_to_rad_neg);
			const double cp = cos(pitch * angle_to_rad_neg);
			const double sy = sin(yaw * angle_to_rad_neg);
			const double cy = cos(yaw * angle_to_rad_neg);
			const double sr = sin(roll * angle_to_rad_neg);
			const double cr = cos(roll * angle_to_rad_neg);

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
	}

	bool ignition_inject::suppress_game_raster()
	{
		static const bool on =
			shared::common::config::get().get_bool("Ignition", "SuppressGameRaster", false);
		return on;
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

		if (shared::utils::hook::detour(game::rebase(game::ADDR_RenderScene), hk_render_scene,
			reinterpret_cast<void**>(&o_render_scene)))
		{
			shared::common::log("Ignition", "Hooked RenderScene - object-space injection armed.",
				shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
		}
		else
		{
			shared::common::log("Ignition", std::format("failed to hook RenderScene @ 0x{:08X}",
				game::ADDR_RenderScene), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}

		if (!shared::utils::hook::detour(game::rebase(game::ADDR_UploadTexture), hk_upload_texture,
			reinterpret_cast<void**>(&o_upload_texture)))
		{
			shared::common::log("Ignition", std::format("failed to hook UploadTexture @ 0x{:08X}",
				game::ADDR_UploadTexture), shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
		}
	}

	// Keeps the raw page. A device may not exist yet -- textures are uploaded during level load,
	// well before the first frame -- so conversion is deferred to first use.
	void ignition_inject::on_texture_uploaded(const int32_t tex_id, const uint8_t* src)
	{
		auto& page = m_texture_pages[tex_id];
		page.assign(src, src + game::TEXTURE_PIXELS);

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

	// The uploaded pages are GR_TEXFMT_P_8 palette indices -- confirmed at runtime, the format
	// flag at 0x00621E60 reads 1, so the remap-to-RGB332 branch is never taken and the LUT at
	// 0x00621360 is not involved. The palette itself is SYS.COL in the game directory: an
	// 8 byte header followed by 256 RGB triples. Decoding the dumped pages with it produces
	// coherent wood, dirt, grass and stone at full 24 bit depth.
	void ignition_inject::ensure_palette()
	{
		if (m_palette_loaded) {
			return;
		}
		m_palette_loaded = true;

		// Index 0 is the game's transparent colour: it drives the Glide chroma key
		// (grChromakeyMode / grChromakeyValue) and shows up as large flat regions in cut-out
		// pages such as fences and foliage.
		for (int i = 0; i < 256; ++i) {
			m_palette[i] = 0xFFFF00FFu;   // magenta, so an unloaded palette is unmistakable
		}

		std::ifstream file("SYS.COL", std::ios::binary);
		if (!file) {
			shared::common::log("IgnTex", "SYS.COL not found - textures will render magenta",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}

		uint8_t header[8]{};
		uint8_t rgb[256 * 3]{};
		file.read(reinterpret_cast<char*>(header), sizeof(header));
		file.read(reinterpret_cast<char*>(rgb), sizeof(rgb));

		if (!file) {
			shared::common::log("IgnTex", "SYS.COL too short to hold a 256 entry palette",
				shared::common::LOG_TYPE::LOG_TYPE_ERROR, true);
			return;
		}

		for (int i = 0; i < 256; ++i)
		{
			const uint32_t r = rgb[i * 3 + 0];
			const uint32_t g = rgb[i * 3 + 1];
			const uint32_t b = rgb[i * 3 + 2];
			const uint32_t a = (i == 0) ? 0x00000000u : 0xFF000000u;
			m_palette[i] = a | (r << 16) | (g << 8) | b;
		}

		shared::common::log("IgnTex", "Loaded SYS.COL palette (index 0 transparent)",
			shared::common::LOG_TYPE::LOG_TYPE_GREEN, true);
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

		ensure_palette();

		D3DLOCKED_RECT rect{};
		if (SUCCEEDED(tex->LockRect(0, &rect, nullptr, 0)))
		{
			const auto& bytes = page->second;
			for (int y = 0; y < game::TEXTURE_SIZE; ++y)
			{
				auto* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(rect.pBits) + y * rect.Pitch);
				for (int x = 0; x < game::TEXTURE_SIZE; ++x) {
					dst[x] = m_palette[bytes[y * game::TEXTURE_SIZE + x]];
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
		struct pending_tri { int32_t tex_sel; bool chroma_keyed; ffp_vertex v[3]; };
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

			if (game::face_is_textured_tri(op))
			{
				const auto f = reinterpret_cast<const game::ign_face_tri*>(cur);
				const int32_t idx[3] = { f->i0, f->i1, f->i2 };

				bool ok = true;
				for (const int32_t v : idx) {
					if (v < 0 || v >= n_verts) { ok = false; break; }
				}

				if (ok)
				{
					const int32_t uv[3][2] = { { f->u0, f->v0 }, { f->u1, f->v1 }, { f->u2, f->v2 } };

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

					// Flat normal. Splitting per face is forced by the per-face UVs anyway,
					// so this costs nothing extra.
					const float ax = tri[1].x - tri[0].x, ay = tri[1].y - tri[0].y, az = tri[1].z - tri[0].z;
					const float bx = tri[2].x - tri[0].x, by = tri[2].y - tri[0].y, bz = tri[2].z - tri[0].z;
					float nx = ay * bz - az * by;
					float ny = az * bx - ax * bz;
					float nz = ax * by - ay * bx;
					const float len = sqrtf(nx * nx + ny * ny + nz * nz);
					if (len > 1e-6f) { nx /= len; ny /= len; nz /= len; }
					else { nx = 0.0f; ny = 1.0f; nz = 0.0f; }

					for (auto& t : tri) { t.nx = nx; t.ny = ny; t.nz = nz; }

					pending_tri pt{};
					pt.tex_sel = f->tex_sel;
					pt.chroma_keyed = game::face_is_chroma_keyed(op);
					pt.v[0] = tri[0]; pt.v[1] = tri[1]; pt.v[2] = tri[2];
					tris.push_back(pt);
				}
			}

			cur += stride;
		}

		if (tris.empty()) {
			return false;
		}

		std::stable_sort(tris.begin(), tris.end(), [](const pending_tri& a, const pending_tri& b) {
			if (a.chroma_keyed != b.chroma_keyed) return a.chroma_keyed < b.chroma_keyed;
			return a.tex_sel < b.tex_sel;
		});

		out.reserve(tris.size() * 3);
		for (size_t i = 0; i < tris.size(); ++i)
		{
			if (parts.empty() || parts.back().tex_sel != tris[i].tex_sel
				|| parts.back().chroma_keyed != tris[i].chroma_keyed) {
				parts.push_back({ tris[i].tex_sel, tris[i].chroma_keyed, static_cast<uint32_t>(i), 0 });
			}
			++parts.back().triangle_count;
			out.insert(out.end(), tris[i].v, tris[i].v + 3);
		}

		return true;
	}

	const ignition_inject::mesh_geometry* ignition_inject::geometry_for(IDirect3DDevice9* dev,
	                                                                    game::ign_mesh* mesh)
	{
		if (const auto it = m_geometry.find(mesh); it != m_geometry.end()) {
			it->second.last_used_scene = m_scenes_submitted;
			return &it->second;
		}

		std::vector<ffp_vertex> vertices;
		std::vector<mesh_part> parts;
		if (!extract_geometry(mesh, vertices, parts)) {
			return nullptr;
		}

		const auto bytes = static_cast<UINT>(vertices.size() * sizeof(ffp_vertex));

		IDirect3DVertexBuffer9* vb = nullptr;
		if (FAILED(dev->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &vb, nullptr))) {
			++m_vb_create_failed;
			return nullptr;
		}

		void* dst = nullptr;
		if (FAILED(vb->Lock(0, bytes, &dst, 0))) {
			vb->Release();
			return nullptr;
		}
		memcpy(dst, vertices.data(), bytes);
		vb->Unlock();

		mesh_geometry geo{};
		geo.vertex_buffer = vb;
		geo.vertex_count = static_cast<uint32_t>(vertices.size());
		geo.triangle_count = geo.vertex_count / 3;
		geo.last_used_scene = m_scenes_submitted;
		geo.parts = std::move(parts);

		return &(m_geometry[mesh] = geo);
	}

	D3DMATRIX ignition_inject::build_world(const game::ign_object* obj)
	{
		D3DMATRIX m{};

		float r[3][3];
		euler_to_rotation(obj->rot_x, obj->rot_y, obj->rot_z, game::OBJ_ANGLE_TO_RAD_NEG, r);

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
		euler_to_rotation(m_scene.pitch_deg, m_scene.yaw_deg, m_scene.roll_deg,
			game::DEG_TO_RAD_NEG, r);

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

			const auto geo = geometry_for(dev, mesh);
			if (!geo || !geo->triangle_count) {
				++m_obj_extract_failed;
			}

			if (geo && geo->triangle_count) {
				m_queue.push_back({ geo, build_world(obj), obj->tex_page });
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
	// The game's scene walk is finished but the device is not inside a BeginScene/EndScene pair
	// here, so the queue is held until the proxy's BeginScene, which is.
	void ignition_inject::finish_scene()
	{
	}

	void ignition_inject::on_present()
	{
		++m_presents;

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
			shared::common::log("Ignition", std::format(
				"captures={} (noScene={} noDevice={} skippedViewport={}) endScenes={} submits={} "
				"lastDraws={} lastVerts={} meshes={} tex(built={} pages={} miss={} oob={} unset={} missIds={}) fail(vb={} tex={} noMesh={} insane={} extract={}) lastDrawErr=0x{:08X}",
				m_captures, m_captures_no_scene, m_captures_no_device, m_captures_skipped_viewport,
				m_end_scenes, m_submits, m_last_draws, m_last_vertices, m_geometry.size(),
				m_textures_built, m_texture_pages.size(), m_texture_misses,
				m_tex_index_oob, m_tex_entry_unset, m_missing_ids.size(),
				m_vb_create_failed, m_tex_create_failed, m_obj_no_mesh, m_obj_insane_counts,
				m_obj_extract_failed, m_captures_merged_pass,
				static_cast<uint32_t>(m_last_draw_error)),
				shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, true);
		}

		if (m_scene_valid && !m_queue.empty())
		{
			if (const auto dev = shared::globals::d3d_device; dev) {
				submit(dev);
			}
		}

		m_scene_valid = false;
		m_queue.clear();

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

	void ignition_inject::submit(IDirect3DDevice9* dev)
	{
		D3DMATRIX view{}, projection{};
		if (!build_view(view) || !build_projection(projection)) {
			return;
		}

		// nGlide owns the device for the rest of the frame, so every state this replay
		// touches is captured and put back afterwards.
		IDirect3DStateBlock9* saved = nullptr;
		if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved))) {
			return;
		}

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
		dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
		dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
		dev->SetRenderState(D3DRS_ALPHAREF, 0);
		dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
		// Ignition picks its winding at runtime from the sign of scene->zoom, so neither
		// winding can be assumed here.
		dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

		ensure_white_texture(dev);
		dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
		dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
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
				game::tex_resolve why{};
				const int32_t tex_id = game::resolve_texture_id(part.tex_sel, inst.tex_page, why);
				if (why == game::tex_resolve::index_out_of_range) ++m_tex_index_oob;
				else if (why == game::tex_resolve::table_entry_unset) ++m_tex_entry_unset;

				dev->SetTexture(0, texture_for(dev, tex_id));

				// Palette index 0 is written with alpha 0. Chroma-keyed faces discard it;
				// opaque faces keep drawing it black, exactly as the game does.
				dev->SetRenderState(D3DRS_ALPHATESTENABLE, part.chroma_keyed ? TRUE : FALSE);

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
		s_injecting = false;

		saved->Apply();
		saved->Release();

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
