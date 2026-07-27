#pragma once

namespace comp
{
	/*
	 * Forwards Ignition's geometry to RTX Remix in object space.
	 *
	 * Ignition transforms and projects on the CPU in fixed-point integers, so everything that
	 * reaches nGlide is already screen-space -- Remix rejects those draws and never finds a
	 * camera. This module taps RenderScene (0x0044A910), where the scene and object list are
	 * fully populated but nothing has been projected yet, and replays the geometry onto the
	 * D3D9 device nGlide created, with WORLD / VIEW / PROJECTION supplied via SetTransform.
	 *
	 * Unlike Carmageddon 2 there is no HUD to filter out: Ignition's 2D and text go through a
	 * separate display-list path (0x0043E050 / 0x0043F9B0 / 0x00437EC0) and never reach
	 * RenderScene. Both of its call sites -- the menu and the race -- are genuine 3D, so both
	 * are forwarded.
	 */
	class ignition_inject final : public shared::common::loader::component_module
	{
	public:
		ignition_inject();
		~ignition_inject();

		static inline ignition_inject* p_this = nullptr;
		static ignition_inject* get() { return p_this; }

		// True only while submit() is replaying our own geometry, so the proxy can tell our
		// draws apart from nGlide's when SuppressGameRaster is on.
		static inline bool s_injecting = false;

		// Diagnostic: drop nGlide's own draws so only injected geometry reaches the screen.
		// A black screen then means our geometry is not being traced; visible white geometry
		// means it was being traced all along and merely hidden behind the game's blit.
		static bool suppress_game_raster();

		// Depth state as last set by the game, shadowed from SetRenderState.
		static inline bool s_game_z_enabled = true;

		// Whether an nGlide draw should be dropped. With KeepGameUI on, only depth-tested
		// draws are dropped -- those are the 3D world, which we replace. Depth-less draws are
		// the HUD and are let through so Remix sees them as ordinary pre-transformed geometry
		// it can categorise and tag.
		static bool should_drop_game_draw();

		// Drops the world display list at the source, before nGlide ever sees it, so the HUD
		// and menu lists still render normally and reach Remix as taggable UI draws.
		static bool suppress_world_raster();
		static inline uint32_t s_world_lists_dropped = 0;

		// world lists seen vs captures answers a specific question: a world display list can
		// only exist if RenderScene ran, and RenderScene always calls TransformAllObjects. So
		// world > 0 with captures == 0 means our transform hook is not firing, whereas both
		// being 0 just means no 3D scene was on screen.
		static inline uint32_t s_world_lists_seen = 0;
		static inline uint32_t s_other_lists_seen = 0;

		// nGlide composites its whole frame offscreen and blits it to the back buffer with a
		// single StretchRect. That blit lands on top of whatever Remix produced, so while it
		// runs the path traced image can never be seen. Suppressing just the blit is far more
		// surgical than dropping every draw: nGlide still renders normally into its own
		// surface, which keeps its state machine happy.
		static bool suppress_game_blit();

		// Per-frame census of what nGlide actually calls. Suppressing draws reveals the path
		// traced image, yet hiding textures in Remix's picker changes nothing -- so how the
		// final image reaches the back buffer is the open question, and guessing at it has
		// already cost several rounds. These counters answer it directly.
		struct api_census
		{
			uint32_t draw_prim, draw_indexed, draw_prim_up, draw_indexed_up;
			uint32_t stretch_rect, update_surface, color_fill, set_render_target;
			uint32_t set_rt_nonzero;   // render target set to something other than index 0
			uint32_t begin_scene;
		};
		static inline api_census s_census{};

		// nGlide draws seen since the last Present. Remix demotes every draw that follows its
		// UI-triggered RTX injection to plain rasterization (d3d9_rtx.cpp:573), and nGlide's
		// pre-transformed orthographic blit is exactly such a trigger. If this is non-zero when
		// we submit, we are already too late and nothing we send can be raytraced.
		static inline uint32_t s_game_draws_this_frame = 0;

		// Called from the UploadTexture detour once the game has a mipmap id for the page.
		void on_texture_uploaded(int32_t tex_id, const uint8_t* src);

		// Called from the proxy's Present; logs and resets the census.
		void on_present();

		// Called from the TransformAllObjects detour, once the object list has been streamed
		// for this pass and before anything is projected.
		void capture_scene();

		// Called from the proxy's BeginScene, immediately after the real BeginScene succeeds.
		//
		// Two constraints pin this down. The game's RenderScene runs during its own update,
		// outside any BeginScene/EndScene pair, where DrawPrimitive fails with
		// D3DERR_INVALIDCALL -- so the geometry cannot be submitted where it is captured.
		// And nGlide's fullscreen blit is a pre-transformed quad that Remix classifies as UI,
		// which calls triggerInjectRTX() and raytraces the frame on the spot
		// (d3d9_rtx.cpp:590). Anything submitted after that is too late to be traced or to
		// supply a camera. Submitting here puts our geometry in before nGlide draws.
		void on_begin_scene_submit();


	private:
		// Position, flat face normal, UV. Ignition stores no normals and its UVs are per-face,
		// so vertices are split per triangle either way.
		struct ffp_vertex
		{
			float x, y, z;
			float nx, ny, nz;
			float u, v;
		};

		// A run of triangles inside one mesh that share a texture selector. Faces are sorted by
		// selector when the buffer is built so each distinct texture costs exactly one draw.
		//
		// The selector is stored rather than a resolved texture because resolution needs the
		// object's texture page too, and one mesh is instanced by objects on different pages.
		struct mesh_part
		{
			int32_t tex_sel;
			bool chroma_keyed;      // palette index 0 is cut out rather than drawn black
			uint32_t first_triangle;
			uint32_t triangle_count;
		};

		// A mesh uploaded once and reused. Static contents are what let Remix keep the
		// acceleration structure it builds instead of rebuilding it every frame.
		struct mesh_geometry
		{
			IDirect3DVertexBuffer9* vertex_buffer;
			uint32_t vertex_count;
			uint32_t triangle_count;
			uint32_t last_used_scene;
			std::vector<mesh_part> parts;
		};

		struct queued_instance
		{
			const mesh_geometry* geometry;
			D3DMATRIX world;
			int32_t tex_page;
		};

		void submit(IDirect3DDevice9* dev);
		bool build_view(D3DMATRIX& out) const;
		bool build_projection(D3DMATRIX& out) const;
		static D3DMATRIX build_world(const game::ign_object* obj);

		const mesh_geometry* geometry_for(IDirect3DDevice9* dev, game::ign_mesh* mesh);
		static bool extract_geometry(const game::ign_mesh* mesh, std::vector<ffp_vertex>& out,
		                            std::vector<mesh_part>& parts);

		// Textures arrive before a device exists, so the raw 8bpp page is kept and converted on
		// first use. Keyed by GrMipMapId_t, which is what g_texTable stores and what the game's
		// own guTexSource call would have received.
		IDirect3DTexture9* texture_for(IDirect3DDevice9* dev, int32_t tex_id);

		// Debug aid gated by [Ignition] DumpTextures: writes the raw source page, the remap
		// table and the format flag so the encoding can be settled from data.
		void dump_texture_debug(int32_t tex_id, const uint8_t* src);

		// SYS.COL, loaded once. The uploaded pages are palette indices, not colour.
		void ensure_palette();

		// Lowers the game's once-per-36 Hz-tick render gate.
		static void patch_render_rate();

		void ensure_white_texture(IDirect3DDevice9* dev);
		void evict_stale_geometry();
		void release_all();

		std::vector<queued_instance> m_queue;
		std::unordered_map<game::ign_mesh*, mesh_geometry> m_geometry;

		// Snapshot of the scene taken at capture time; the game mutates it during the call.
		game::ign_scene m_scene{};
		bool m_scene_valid = false;

		IDirect3DTexture9* m_white_texture = nullptr;
		IDirect3DVertexDeclaration9* m_vertex_decl = nullptr;

		// Raw 8bpp pages captured at upload, and the D3D textures built from them on demand.
		std::unordered_map<int32_t, std::vector<uint8_t>> m_texture_pages;
		std::unordered_map<int32_t, IDirect3DTexture9*> m_textures;
		uint32_t m_textures_built = 0;
		uint32_t m_texture_misses = 0;

		// Why faces fall back to white: an index past the 512-entry table, a table slot the game
		// never filled, or an id whose upload we never saw.
		uint32_t m_tex_index_oob = 0;
		uint32_t m_tex_entry_unset = 0;
		std::set<int32_t> m_missing_ids;
		uint32_t m_debug_dumps = 0;

		uint32_t m_palette[256]{};
		bool m_palette_loaded = false;

		// Silent failure paths that would otherwise look identical to "the geometry vanished".
		uint32_t m_vb_create_failed = 0;
		uint32_t m_tex_create_failed = 0;
		uint32_t m_obj_no_mesh = 0;
		uint32_t m_obj_insane_counts = 0;
		uint32_t m_obj_extract_failed = 0;


		uint32_t m_viewport_index = 0;
		uint32_t m_scenes_submitted = 0;

		// Geometry untouched for this many scenes is released. Ignition swaps whole track
		// object sets between races, so stale meshes must not accumulate.
		static constexpr uint32_t GEOMETRY_EVICT_AFTER_SCENES = 900;

		// A mesh larger than this is almost certainly a bad pointer rather than real geometry.
		static constexpr int32_t MAX_SANE_VERTICES = 65536;
		static constexpr int32_t MAX_SANE_FACES = 65536;

		uint32_t m_last_draws = 0;
		uint32_t m_last_vertices = 0;

		// Diagnostics: why a frame produced nothing is otherwise invisible.
		uint32_t m_captures = 0;
		uint32_t m_captures_no_device = 0;
		uint32_t m_captures_no_scene = 0;
		uint32_t m_captures_skipped_viewport = 0;
		uint32_t m_captures_merged_pass = 0;
		uint32_t m_end_scenes = 0;
		uint32_t m_submits = 0;
		uint32_t m_presents = 0;
		uint64_t m_rate_window_start = 0;
		uint32_t m_rate_presents = 0;
		uint32_t m_rate_captures = 0;
		double m_submit_ms_total = 0.0;

		// Recorded once and re-captured per frame rather than rebuilt.
		IDirect3DStateBlock9* m_state_block = nullptr;
		HRESULT m_last_draw_error = S_OK;
		bool m_logged_first_submit = false;
	};
}
