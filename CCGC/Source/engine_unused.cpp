// Parts of the game's engine the GameCube never uses, stood in for (as
// octave_unused.cpp does for Octave's), so their code and what they pull in
// stay out of memory (docs/memory-roadmap.md, 1b).
//
// The PC's replacement graphics (player/mods.cpp, left out by Makefile_GCN's
// EXCLUDE): a mod is applied only if Game::mod_dir is set, which only the
// PC's main.cpp does. It brought stb_image, std::filesystem's directory
// walking and C++ streams with it.
#include "player/mods.h"

namespace player {

int apply_mod(swf::Movie&, const std::filesystem::path&) {
    return 0;
}

}  // namespace player
