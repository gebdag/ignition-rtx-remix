#pragma once

namespace comp::game
{
	// Ignition keeps its whole 3D pipeline in fixed-point integers. Everything below is the
	// authored, pre-transform representation -- see patches/Ignition/findings.md for how each
	// offset was established and verified against the running game.

	// Object-space position. Small integers, a few hundred units across a car.
	struct ign_vertex
	{
		int32_t x, y, z;
	};

	// One entry of a model's face stream. The stream is *variable stride*: the low byte of
	// `opcode` selects both the emitter and how far the cursor advances, so it can only be
	// walked sequentially, never indexed. Only the 0x2C-stride opcodes carry textured
	// triangles; the shorter records are lines, points and sprites we do not raytrace.
	struct ign_face_tri
	{
		int32_t opcode;          // low byte = kind, upper 24 bits = material/shading flags
		int32_t i0, i1, i2;      // indices into the model's vertex array
		int32_t u0, v0;          // per-FACE texture coords, 24.8 fixed point texels
		int32_t u1, v1;
		int32_t u2, v2;
		int32_t tex_sel;         // >> 16, then + object->tex_page * 0x20 into g_texTable
	};
	static_assert(sizeof(ign_face_tri) == 0x2C, "textured face record is 0x2C bytes");

	// A model: counts, then the vertex array, then the face stream.
	//   vertices : (int32*)(mesh + 2)
	//   faces    : (int32*)(mesh + 2 + vertex_count * 3)
	// `face_count` is an UPPER BOUND -- a terminator opcode can end the stream early.
	struct ign_mesh
	{
		int32_t vertex_count;
		int32_t face_count;
		// int32_t data[];
	};

	inline const ign_vertex* mesh_vertices(const ign_mesh* m) {
		return reinterpret_cast<const ign_vertex*>(reinterpret_cast<const int32_t*>(m) + 2);
	}

	inline const void* mesh_faces(const ign_mesh* m) {
		return reinterpret_cast<const int32_t*>(m) + 2 + m->vertex_count * 3;
	}

	// One placed instance. Supplies the WORLD transform: integer translation plus an Euler
	// triple in units of 1/3600 turn. When all three angles are zero the game takes a
	// translation-only fast path, which is how track scenery separates from cars.
	struct ign_object
	{
		int32_t field_00;
		ign_mesh* mesh;          // +0x04
		int32_t tex_page;        // +0x08  base index into g_texTable
		int32_t pos_x;           // +0x0C  world translation
		int32_t pos_y;           // +0x10
		int32_t pos_z;           // +0x14
		int16_t rot_x;           // +0x18  Euler, 3600 == full turn
		int16_t rot_y;           // +0x1A
		int16_t rot_z;           // +0x1C
		int16_t lod_dist;        // +0x1E
	};
	static_assert(offsetof(ign_object, mesh) == 0x04, "object layout drifted");
	static_assert(offsetof(ign_object, pos_x) == 0x0C, "object layout drifted");
	static_assert(offsetof(ign_object, rot_x) == 0x18, "object layout drifted");

	inline bool object_is_unrotated(const ign_object* o) {
		return o->rot_x == 0 && o->rot_y == 0 && o->rot_z == 0;
	}

	// Camera + projection for one viewport. Reached through *(ign_scene**)0x006236A8; the
	// game swaps this pointer between the menu view and each split-screen race viewport.
	struct ign_scene
	{
		double cam_x;            // +0x00
		double cam_y;            // +0x08
		double cam_z;            // +0x10
		double pitch_deg;        // +0x18  NOT normalised -- 587.5 and 3143.7 observed live
		double yaw_deg;          // +0x20
		double roll_deg;         // +0x28
		double zoom;             // +0x30  scales row 0; its sign flips backface winding
		uint8_t pad_38[0x5C - 0x38];
		int32_t vert_cursor;     // +0x5C  running write index into the transformed pool
		int32_t object_count;    // +0x60
		int32_t pad_64;
		int32_t node_count;      // +0x68  display-list nodes produced
		uint8_t pad_6C[0x80 - 0x6C];
		int32_t focal_x;         // +0x80  <= 0xFA -> 0x44D020 path, else 0x44D640
		int32_t focal_y;         // +0x84
		int32_t near_clamp;      // +0x88  << 2 gives the near plane in w units
		uint8_t pad_8C[0x9C - 0x8C];
		int32_t center_x;        // +0x9C  << 8 gives g_screenCenterX
		int32_t center_y;        // +0xA0
	};
	static_assert(offsetof(ign_scene, object_count) == 0x60, "scene layout drifted");
	static_assert(offsetof(ign_scene, focal_x) == 0x80, "scene layout drifted");
	static_assert(offsetof(ign_scene, center_x) == 0x9C, "scene layout drifted");

	// Face opcode -> cursor advance, indexed by (opcode & 0xFF). Zero marks a terminator:
	// dispatch routes it to 0x0044E8C0, which ends the model's face stream.
	// Recovered from each dispatch target's advance of g_curFace (0x004E56AC).
	inline constexpr uint8_t FACE_STRIDE[0x1E] = {
		/*00*/ 0,    /*01*/ 0x0C, /*02*/ 0x0C, /*03*/ 0,
		/*04*/ 0,    /*05*/ 0,    /*06*/ 0,    /*07*/ 0x24,
		/*08*/ 0x24, /*09*/ 0,    /*0A*/ 0,    /*0B*/ 0x10,
		/*0C*/ 0,    /*0D*/ 0x18, /*0E*/ 0,    /*0F*/ 0x14,
		/*10*/ 0,    /*11*/ 0x2C, /*12*/ 0x2C, /*13*/ 0x2C,
		/*14*/ 0x14, /*15*/ 0x2C, /*16*/ 0x2C, /*17*/ 0x2C,
		/*18*/ 0x2C, /*19*/ 0x2C, /*1A*/ 0x24, /*1B*/ 0x24,
		/*1C*/ 0x14, /*1D*/ 0x14,
	};

	// The opcodes whose record is an ign_face_tri.
	inline bool face_is_textured_tri(const uint32_t op) {
		switch (op) {
		case 0x11: case 0x12: case 0x13: case 0x15:
		case 0x16: case 0x17: case 0x18: case 0x19:
			return true;
		default:
			return false;
		}
	}

	// 0x16/0x17/0x19 reach their emitter through a stub that pushes a depth bias of 0x50,
	// pushing them back in the painter's-algorithm sort -- decals and overlays.
	inline bool face_is_decal(const uint32_t op) {
		return op == 0x16 || op == 0x17 || op == 0x19;
	}

	// How a face is composited. Glide has one blend state at a time, so each rasterizer sets it
	// on entry; the mode is a property of the opcode, not of the texture or the object.
	enum class face_blend : uint8_t
	{
		opaque,     // grAlphaBlendFunction(ONE, ZERO, ...)
		alpha,      // grAlphaBlendFunction(SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ...)
		additive,   // grAlphaBlendFunction(SRC_ALPHA, ONE, ...)
	};

	// Everything the compositing of one face depends on.
	//
	// `opacity` is the alpha byte of the Glide constant colour, which is what the blend actually
	// multiplies by: the rasterizers select GR_ALPHASOURCE_CC_ALPHA, so texture and vertex alpha
	// play no part. `chroma_keyed` is the separate grChromakeyMode test that cuts palette index 0
	// out instead of drawing it black.
	struct face_material
	{
		face_blend blend;
		uint8_t opacity;
		bool chroma_keyed;
	};

	// Face opcode -> compositing, traced from g_faceDispatch (0x0048FA90) through each emitter to
	// the rasterizer that runs for the display-list opcode it writes:
	//
	//   face 0x11,0x15 -> emitter 0x0044F780 -> dl 0x11 -> 0x00452070
	//   face 0x12,0x16 -> emitter 0x0044FF10 -> dl 0x12 -> 0x004523E0
	//   face 0x13,0x17 -> emitter 0x00450250 -> dl 0x13 -> 0x00452730
	//   face 0x18,0x19 -> emitter 0x00450560 -> dl 0x18 -> 0x00452A90
	//
	// The second opcode of each pair is the same emitter reached through a stub that pushes a
	// depth bias of 0x50, so it composites identically. The blend arguments and the constant
	// colour below are the literal operands of those rasterizers' grAlphaBlendFunction and
	// grConstantColorValue calls.
	inline face_material face_material_for(const uint32_t op)
	{
		switch (op) {
		case 0x11: case 0x15:
			return { face_blend::opaque,   0xFF, false };
		case 0x12: case 0x16:
			return { face_blend::opaque,   0xFF, true };
		case 0x13: case 0x17:
			return { face_blend::alpha,    0x7F, true };   // 0x00452730: push 0x7F000000
		case 0x18: case 0x19:
			return { face_blend::additive, 0x4F, true };   // 0x00452A90: push 0x4F000000
		default:
			return { face_blend::opaque,   0xFF, false };
		}
	}
}
