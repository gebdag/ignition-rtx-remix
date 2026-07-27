#include "std_include.hpp"
#include "shared/common/flags.hpp"

namespace comp::game
{
	// Ign_3dfx.exe has no .reloc section and every address we need is a fixed constant, so
	// there is nothing to pattern-scan for. game.hpp resolves each one through rebase() at
	// the point of use, which keeps working if the module ever does load somewhere else.
	void init_game_addresses()
	{
		const auto scene_ptr = *reinterpret_cast<void**>(rebase(ADDR_g_scene));

		shared::common::log("Game", std::format(
			"Ignition addresses fixed at base 0x{:08X} (scene ptr currently {})",
			shared::globals::exe_module_addr, scene_ptr ? "live" : "null"),
			shared::common::LOG_TYPE::LOG_TYPE_DEFAULT, true);
	}
}
