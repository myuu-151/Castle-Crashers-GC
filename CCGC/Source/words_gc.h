// Words of the text files the data tools write (files.txt, the effects
// bank's index), read as `>>` reads them -- without C++ streams, whose locale
// code was about 400 KB of the program (docs/memory-roadmap.md, 1a).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace words {

inline bool space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

// The next word of [p, end) into `out`, p moved past it; false at the end.
inline bool next(const char*& p, const char* end, std::string& out) {
    while (p < end && space(*p)) p++;
    if (p == end) return false;
    const char* start = p;
    while (p < end && !space(*p)) p++;
    out.assign(start, p);
    return true;
}

// The next word as an unsigned decimal number; false if there's no word, or
// it isn't all digits, or it's over 32 bits (where `>>` fails too).
inline bool number(const char*& p, const char* end, uint32_t& out) {
    while (p < end && space(*p)) p++;
    if (p == end || *p < '0' || *p > '9') return false;
    uint64_t n = 0;
    while (p < end && *p >= '0' && *p <= '9') {
        n = n * 10 + uint64_t(*p++ - '0');
        if (n > 0xffffffffu) return false;
    }
    if (p < end && !space(*p)) return false;
    out = uint32_t(n);
    return true;
}

// The line starting at p: [p, its end), p moved to the next line's start.
inline const char* line(const char*& p, const char* end) {
    const char* nl = static_cast<const char*>(std::memchr(p, '\n', size_t(end - p)));
    const char* line_end = nl ? nl : end;
    p = nl ? nl + 1 : end;
    return line_end;
}

}  // namespace words
