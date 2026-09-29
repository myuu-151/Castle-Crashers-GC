// Main memory on the GameCube (new_gc.cpp).
#pragma once

#include <cstddef>
#include <cstdint>

namespace memory {

// Scratch: while a Scratch is open (on the main thread), operator new takes
// from a region of its own, made empty again when all it gave is freed.
// For short-lived work that allocates a lot (tessellating a shape), so its
// comings and goings leave no holes among the game's memory. What doesn't
// fit comes from the heap as usual.
void scratch_init(std::size_t bytes);

class Scratch {
public:
    explicit Scratch(bool on = true);
    ~Scratch();
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

private:
    bool was_;
};

// Allocations that didn't fit in scratch, so far.
uint32_t scratch_overflows();

// The biggest block the heap could give now, in KB.
uint32_t largest_free_kb();

// What the pages of small blocks take, in KB.
uint32_t small_kb();

// The heap's blocks by caller, the ten biggest, into the log.
void census_log(const char* when);

}  // namespace memory
