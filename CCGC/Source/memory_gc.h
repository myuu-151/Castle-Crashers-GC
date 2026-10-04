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

// A region set aside at start for one kind of big block, so that a long
// session's heap, in pieces, can't keep one out: the biggest blocks the game
// needs are a level's file (1.16 MB for level 35) and a sky's textures
// (512 KB each), and late in a session the heap's biggest piece fell to
// 840 KB (from 4.3 MB) with 2.9 MB free. First fit among a few blocks; a
// block that doesn't fit is the caller's to find elsewhere (the heap, as
// before). Empty, with no room, if it couldn't be set aside.
class Region {
public:
    bool init(std::size_t bytes);
    void* alloc(std::size_t size);  // 32-byte aligned; null if there's no room
    bool free(void* p);             // false if it isn't this region's
    bool owns(const void* p) const;
    uint32_t size_kb() const { return size_ / 1024; }
    uint32_t used_kb() const;

private:
    static constexpr int kBlocks = 32;
    uint8_t* base_ = nullptr;
    uint32_t size_ = 0;
    uint32_t at_[kBlocks] = {}, len_[kBlocks] = {};  // the blocks given, by address
    int count_ = 0;
};

// The level region: levels' files, their skies' and mountains', and the world
// map's are read into it (files_gc.cpp): every block of 64 KB or more that
// operator new is asked for on the main thread while a RegionScope is open.
Region& level_region();

class RegionScope {
public:
    explicit RegionScope(Region* region);
    ~RegionScope();
    RegionScope(const RegionScope&) = delete;
    RegionScope& operator=(const RegionScope&) = delete;

private:
    Region* was_;
};

}  // namespace memory
