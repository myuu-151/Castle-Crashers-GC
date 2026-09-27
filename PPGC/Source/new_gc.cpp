// operator new for the GameCube. When main memory has no piece big enough,
// display lists go to ARAM (render::gx_release_memory) until one is, rather
// than the game ending there; only when none are left does it throw.
#include <malloc.h>

#include <cstdlib>
#include <new>

#include <SDL3/SDL_log.h>

namespace render {
bool gx_release_memory();  // renderer_gx.cpp
}

namespace {

// Logging may need memory itself.
bool g_logging = false;

void log(const char* what, std::size_t size, unsigned lists) {
    if (g_logging) return;
    g_logging = true;
    const struct mallinfo info = mallinfo();
    SDL_Log("memory: %s a %u-byte block (%u display lists to ARAM, %u KB free in pieces)", what, unsigned(size),
            lists, unsigned(info.fordblks / 1024));
    g_logging = false;
}

}  // namespace

void* operator new(std::size_t size) {
    if (size == 0) size = 1;
    unsigned lists = 0;
    for (;;) {
        if (void* p = std::malloc(size)) {
            if (lists) log("made room for", size, lists);
            return p;
        }
        if (!render::gx_release_memory()) break;
        lists++;
    }
    log("no room for", size, lists);
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* p) noexcept {
    std::free(p);
}

void operator delete[](void* p) noexcept {
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}
