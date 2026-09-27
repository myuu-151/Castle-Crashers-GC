#include "trace_gc.h"

#include <gccore.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <ogc/semaphore.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

void OctLog(const char* format, ...);

namespace trace {

namespace {

constexpr uint32_t kLines = 512, kLineSize = 240;
constexpr uint32_t kStallMs = 3000, kStallRepeatMs = 15000;

char g_ring[kLines][kLineSize];
volatile uint32_t g_head = 0;     // next line to write
volatile uint32_t g_written = 0;  // lines the writer has put on the card
sem_t g_writer_sem = LWP_SEM_NULL;
bool g_started = false;
uint64_t g_start = 0;

struct Where {
    const char* volatile what = "start";
    char detail[96] = {};
    volatile uint32_t since = 0;  // ms
};
Where g_where[kThreads];
const char* const kThreadNames[kThreads] = {"main", "mixer", "reader"};

volatile uint32_t g_ticks = 0, g_frames = 0;

// A line into the ring (interrupts off while it is copied, so every thread
// can log).
void put(const char* text) {
    uint32_t level;
    _CPU_ISR_Disable(level);
    char* line = g_ring[g_head % kLines];
    std::snprintf(line, kLineSize, "%7u %s", unsigned(now_ms()), text);
    g_head = g_head + 1;
    _CPU_ISR_Restore(level);
    if (g_writer_sem != LWP_SEM_NULL) LWP_SemPost(g_writer_sem);
}

// Lines from `from` up to the head, appended to a file. Lines the ring has
// written over since are said to be lost.
uint32_t append(FILE* f, uint32_t from) {
    uint32_t head = g_head;
    if (head - from > kLines) {
        std::fprintf(f, "(%u lines lost)\n", unsigned(head - from - kLines));
        from = head - kLines;
    }
    char line[kLineSize];
    for (; from != head; from++) {
        uint32_t level;
        _CPU_ISR_Disable(level);
        std::memcpy(line, g_ring[from % kLines], kLineSize);
        _CPU_ISR_Restore(level);
        line[kLineSize - 1] = 0;
        std::fputs(line, f);
        std::fputc('\n', f);
    }
    return head;
}

// Below every other thread: writes when the game waits (on the GPU, the
// retrace), a batch a file open.
void* writer_main(void*) {
    for (;;) {
        LWP_SemWait(g_writer_sem);
        if (g_written == g_head) continue;
        usleep(200 * 1000);  // gather a batch
        FILE* f = std::fopen("/ppgc.log", "a");
        if (!f) {
            g_written = g_head;  // no SD card: the ring is all there is
            continue;
        }
        g_written = append(f, g_written);
        std::fclose(f);
    }
    return nullptr;
}

void stall_report(uint32_t quiet_ms) {
    FILE* f = std::fopen("/ppgc_stall.log", "a");
    char text[256];
    std::snprintf(text, sizeof(text), "watchdog: no tick or frame for %u ms (ticks %u, frames %u)", unsigned(quiet_ms),
                  unsigned(g_ticks), unsigned(g_frames));
    OctLog("%s", text);
    if (f) std::fprintf(f, "==== %7u %s\n", unsigned(now_ms()), text);
    for (int t = 0; t < kThreads; t++) {
        std::snprintf(text, sizeof(text), "watchdog: %s at %s %s (for %u ms)", kThreadNames[t], g_where[t].what,
                      g_where[t].detail, unsigned(now_ms() - g_where[t].since));
        OctLog("%s", text);
        if (f) std::fprintf(f, "%s\n", text);
    }
    if (f) {
        std::fprintf(f, "---- the last lines:\n");
        uint32_t head = g_head;
        append(f, head > 200 ? head - 200 : 0);
        std::fclose(f);
    }
}

// Above every other thread.
void* watchdog_main(void*) {
    uint32_t last_ticks = g_ticks, last_frames = g_frames, last_change = now_ms(), last_report = 0;
    for (;;) {
        usleep(250 * 1000);
        uint32_t now = now_ms();
        if (g_ticks != last_ticks || g_frames != last_frames) {
            last_ticks = g_ticks;
            last_frames = g_frames;
            last_change = now;
            last_report = 0;
            continue;
        }
        uint32_t quiet = now - last_change;
        if (quiet >= kStallMs && (last_report == 0 || now - last_report >= kStallRepeatMs)) {
            last_report = now;
            stall_report(quiet);
        }
    }
    return nullptr;
}

}  // namespace

// PpgcLog's line, into the ring.
void log_line(const char* text) {
    put(text);
}

uint32_t now_ms() {
    if (!g_start) return 0;
    return uint32_t(ticks_to_millisecs(gettime() - g_start));
}

void at(Thread thread, const char* what, const char* detail) {
    Where& w = g_where[thread];
    w.what = what;
    if (detail) {
        std::strncpy(w.detail, detail, sizeof(w.detail) - 1);
        w.detail[sizeof(w.detail) - 1] = 0;
    } else {
        w.detail[0] = 0;
    }
    w.since = now_ms();
}

void ticked() {
    g_ticks = g_ticks + 1;
}

void drew() {
    g_frames = g_frames + 1;
}

uint32_t ticks() {
    return g_ticks;
}

uint32_t frames() {
    return g_frames;
}

void start() {
    if (g_started) return;
    g_started = true;
    g_start = gettime();
    for (Where& w : g_where) w.since = 0;
    static uint8_t writer_stack[16 * 1024] __attribute__((aligned(32)));
    static uint8_t watchdog_stack[16 * 1024] __attribute__((aligned(32)));
    lwp_t thread;
    if (LWP_SemInit(&g_writer_sem, 0, 1 << 30) == 0)
        LWP_CreateThread(&thread, writer_main, nullptr, writer_stack, sizeof(writer_stack), 20);
    LWP_CreateThread(&thread, watchdog_main, nullptr, watchdog_stack, sizeof(watchdog_stack), 120);
    PpgcLog("trace: started; this log is /ppgc.log, a stall's /ppgc_stall.log");
}

}  // namespace trace

extern "C" void PpgcLog(const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    OctLog("%s", text);
    trace::log_line(text);
}
