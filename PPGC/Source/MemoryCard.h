// The save on the memory card in slot A, through Octave's card functions.
#pragma once

#include <cstdint>
#include <vector>

namespace card {

enum class State {
    Exists,    // a Painter's Playground save is there
    Ready,     // no save, and room for one
    Full,      // no save, and too few free blocks (or files) for one
    NoCard,    // nothing in slot A
    Unusable,  // damaged or unformatted, another region's, or not a memory card
};

struct Status {
    State state = State::NoCard;
    int blocks_needed = 0;
    int blocks_free = 0;
};

// The card's menu entry for the save: title, description and icon.
void init();
Status query();
bool read(std::vector<uint8_t>& out);
bool write(const std::vector<uint8_t>& data);

}  // namespace card
