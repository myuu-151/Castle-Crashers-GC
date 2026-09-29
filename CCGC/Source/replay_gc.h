// A recorded session played again (test builds, make REPLAY=1): the input
// changes in Scripts/Data/replay.bin (tools/make_replay.py) applied as the
// game makes the same updates, as `castle.exe --replay` does
// (engine/main.cpp's KeyReplay), from the save the session began with. Read
// from the disc a few at a time. Until its fast-forward update, the game runs
// several ticks a frame (CastleGame::Update).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace player {
class Game;
class Player;
}  // namespace player

namespace replay {

// The file's header and first changes; false if there is none.
bool open();
bool active();              // opened, and changes still to apply
uint32_t fast_forward();    // the update to run fast until (0: none)
uint32_t updates();         // every movie's updates so far, as a session counts them
const std::vector<uint8_t>& storage();  // the save the session began with
const std::string& gamer_tag();

// The changes due before any update, once the game has started.
void start(player::Game& game);
// Player::on_updated: the changes tied to this update.
void on_update(player::Game& game, player::Player& p);

}  // namespace replay
