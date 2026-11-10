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
static struct { int page, hold_note, hold_ms, lock_input; unsigned char pass[128]; } C = { 0, 40, 800, 0, {0} };
static volatile int g_scale_on;
static void *g_pg;
static int g_shown;
static int sixteen_on(void) { return 1; }
#include "../src/hold.h"
static void page_popup(void) {}
static int page_shown(void) { return g_shown; }
static void cc(unsigned char *f, int num, int val)
{
    memset(f, 0, HOLD_FRAME);
    f[1] = 3;
    f[HOLD_DATA] = 0xB0; f[HOLD_DATA + 1] = (unsigned char)num; f[HOLD_DATA + 2] = (unsigned char)val;
}

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

    g_scale_on = 0;
    /* other buttons and a mixed read pass untouched; 16 Levels inside it is held back */
    frame(b, 1, 36); frame(b + HOLD_FRAME, 1, 40);
    CHECK(hold_filter(7, b, 2 * HOLD_FRAME) == HOLD_FRAME && note_of(b) == 36);
    g_hold_held = 0;

    /* input lock, Scale on: buttons and knobs kept from MPC, except 16 Levels, pass_notes, pads, releases */
    C.lock_input = 1; C.pass[49] = C.pass[82] = 1; g_scale_on = 1; g_shown = 0;
    CHECK(feed(1, 7, b) == 0);                       /* MAIN: dropped */
    CHECK(feed(0, 7, b) == HOLD_FRAME);              /* its release passes */
    CHECK(feed(1, 82, b) == HOLD_FRAME);             /* Play */
    CHECK(feed(1, 49, b) == HOLD_FRAME); feed(0, 49, b);   /* Shift */
    CHECK(feed(1, 12, b) == 0);                      /* soft key, page not up: dropped */
    cc(b, 19, 64); CHECK(hold_filter(7, b, HOLD_FRAME) == 0);   /* a knob */
    cc(b, 16, 64); CHECK(hold_filter(7, b, HOLD_FRAME) == 0);   /* K1, page not up */
    frame(b, 1, 37); b[HOLD_DATA] = 0x99; CHECK(hold_filter(7, b, HOLD_FRAME) == HOLD_FRAME);   /* a pad (channel 10) */
    g_shown = 1;   /* the SCALE page is up: the controls the draw handler gives it pass on to it */
    CHECK(feed(1, 12, b) == HOLD_FRAME && feed(1, 111, b) == HOLD_FRAME);
    cc(b, 16, 64); CHECK(hold_filter(7, b, HOLD_FRAME) == HOLD_FRAME);
    cc(b, 100, 1); CHECK(hold_filter(7, b, HOLD_FRAME) == HOLD_FRAME);
    cc(b, 19, 64); CHECK(hold_filter(7, b, HOLD_FRAME) == 0);
    {   /* two messages in one frame: the dropped one is cut out */
        frame(b, 1, 7); b[1] = 6; b[HOLD_DATA + 3] = 0x90; b[HOLD_DATA + 4] = 82; b[HOLD_DATA + 5] = 0x7f;
        CHECK(hold_filter(7, b, HOLD_FRAME) == HOLD_FRAME && b[1] == 3 && note_of(b) == 82 && b[HOLD_DATA + 3] == 0);
    }
    g_scale_on = 0;   /* Scale off: everything passes */
    CHECK(feed(1, 7, b) == HOLD_FRAME);
    cc(b, 19, 64); CHECK(hold_filter(7, b, HOLD_FRAME) == HOLD_FRAME);

    /* an unframed read is left alone */
    unsigned char raw[3] = { 0x90, 40, 0x7f };
    CHECK(hold_filter(7, raw, 3) == 3);

    /* test input: queued as a frame for the surface fd, and poll is told */
    CHECK(hold_inject(0x99, 37, 100) == -1);   /* no surface seen yet */
    g_surface_fd = 7;
    CHECK(hold_inject(0x99, 37, 100) == 0 && g_inj_n == 1 && hold_pending() == 7);
    CHECK(g_inj[g_inj_head][1] == 3 && g_inj[g_inj_head][HOLD_DATA] == 0x99 && g_inj[g_inj_head][HOLD_DATA + 1] == 37);
    struct pollfd pf = { 7, POLLIN, 0 };
    CHECK(hold_watches(&pf, 1) && hold_mark(&pf, 1, 0) == 1 && (pf.revents & POLLIN));
    (void)hold_start;
    /* LED writes: lit notes listed */
    {
        int pfd[2]; CHECK(pipe(pfd) == 0);
        g_hold_on = 1; g_hold_cls[pfd[1]] = 1;   /* as if it were the surface */
        unsigned char led[6] = { 0xB0, 73, 3, 0xB0, 82, 0 };
        CHECK(write(pfd[1], led, sizeof led) == 6);
        char o[200]; hold_leds(o, sizeof o);
        CHECK(!strncmp(o, "73=3 \nwrites=1 last: b0 49 03 b0 52 00", 38));
        g_hold_cls[pfd[1]] = 0; close(pfd[0]); close(pfd[1]);
    }

    printf(fails ? "hold_test: %d failed\n" : "hold_test: ok\n", fails);
    return fails != 0;
}
