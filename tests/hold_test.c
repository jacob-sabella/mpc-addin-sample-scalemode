/* Host test for src/hold.h: the frames MPC gets for a short press, a long hold and Shift+16 Levels. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define LOG(fmt, ...) fprintf(stderr, "hold: " fmt "\n", ##__VA_ARGS__)
static struct { int page, hold_note, hold_ms; } C = { 0, 40, 800 };
static volatile int g_scale_on;
#include "../src/hold.h"
static void page_popup(void) {}

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static void frame(unsigned char *f, int on, int note)
{
    memset(f, 0, HOLD_FRAME);
    f[1] = 3;
    f[HOLD_DATA] = on ? 0x90 : 0x80; f[HOLD_DATA + 1] = (unsigned char)note; f[HOLD_DATA + 2] = on ? 0x7f : 0;
}
static int note_of(const unsigned char *f) { return f[HOLD_DATA + 1]; }
static int on_of(const unsigned char *f) { return f[HOLD_DATA] == 0x90; }
static ssize_t feed(int on, int note, unsigned char *out)
{
    frame(out, on, note);
    return hold_filter(7, out, HOLD_FRAME);
}

int main(void)
{
    unsigned char b[HOLD_FRAME * 2];

    /* short press: the press is held back, then press now and release queued */
    CHECK(feed(1, 40, b) == 0 && g_hold_held);
    CHECK(feed(0, 40, b) == HOLD_FRAME && note_of(b) == 40 && on_of(b));
    CHECK(g_hold_npend == HOLD_FRAME && g_hold_pend_fd == 7 && !on_of(g_hold_pend));
    g_hold_npend = 0;
    CHECK(!g_scale_on);

    /* long hold: the thread's check fires, the release is dropped */
    CHECK(feed(1, 40, b) == 0);
    g_hold_since -= 900;
    if (g_hold_held && !g_hold_fired && hold_now_ms() - g_hold_since >= C.hold_ms) { g_hold_fired = 1; g_scale_on = !g_scale_on; }
    CHECK(g_scale_on);
    CHECK(feed(0, 40, b) == 0 && g_hold_npend == 0);

    /* Shift down: 16 Levels passes straight through, both ways */
    CHECK(feed(1, 49, b) == HOLD_FRAME && g_shift_down);
    CHECK(feed(1, 40, b) == HOLD_FRAME && on_of(b) && !g_hold_held);
    CHECK(feed(0, 40, b) == HOLD_FRAME && !on_of(b) && g_hold_npend == 0);
    CHECK(feed(0, 49, b) == HOLD_FRAME && !g_shift_down);

    /* other buttons and a mixed read pass untouched; 16 Levels inside it is held back */
    frame(b, 1, 36); frame(b + HOLD_FRAME, 1, 40);
    CHECK(hold_filter(7, b, 2 * HOLD_FRAME) == HOLD_FRAME && note_of(b) == 36);
    g_hold_held = 0;

    /* an unframed read is left alone */
    unsigned char raw[3] = { 0x90, 40, 0x7f };
    CHECK(hold_filter(7, raw, 3) == 3);

    (void)hold_start;
    printf(fails ? "hold_test: %d failed\n" : "hold_test: ok\n", fails);
    return fails != 0;
}
