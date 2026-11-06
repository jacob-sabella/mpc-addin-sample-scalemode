/* scale_page.h: the SCALE page on the MPC Sample's screen, through mpc-addin-sample-draw-handler (included once, by
 * enable_scalemode.c, after the scale state it shows).
 *
 * It opens by itself when a long hold of 16 Levels toggles Scale, or with `padctl page`, and closes after PAGE_IDLE_MS without
 * input or pad hits, or on a dial press. While it is up: the soft keys pick the field the dial edits (scale, root, octave), the dial
 * steps it, K1-K3 set the three directly, and the 16 slots show their notes, the one just played lit. Pads and every
 * other button stay with MPC. Without the draw handler (not installed, or too old for input) there is no page. */
#include "sample_draw.h"
#include <stdio.h>
#include <time.h>

#define PAGE_IDLE_MS 4000
#define PAGE_SAVE_MS 800

static const uint32_t PG_BG = 0x0b0820, PG_PANEL = 0x1a1040, PG_TEXT = 0xf0e8ff, PG_DIM = 0x8a7ab8,
                      PG_PINK = 0xff2fa0, PG_CYAN = 0x22e6ff;

static const mpc_sdraw_api *g_pg_api;
static mpc_sdraw_client *g_pg;
static volatile int g_pg_want;        /* 1: take the screen */
static volatile int g_pg_close;       /* 1: give it back */
static volatile int g_pg_field;       /* 0 scale, 1 root, 2 octave */
static volatile long long g_pg_touch; /* last input, ms */
static volatile long long g_pg_dirty; /* last unsaved change, ms (0: saved) */
static volatile int g_pg_hit_slot = -1;
static volatile long long g_pg_hit_ms;

static long long pg_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static void page_popup(void) { g_pg_want = 1; }
static void page_note_hit(int slot) { long long t = pg_now_ms(); g_pg_hit_ms = t; g_pg_touch = t; g_pg_hit_slot = slot; }   /* playing keeps the page up */

static int slot_midi(int slot)
{
    int sc = g_scale_type;
    if (sc < 0 || sc >= NUM_SCALES) sc = 0;
    int npo = scale_npo[sc];
    return g_root_note + scale_intervals[sc][slot % npo] + (slot / npo + g_octave_offset - 2) * 12;
}

static void note_name(char *out, size_t n, int midi)
{
    static const char *names[] = {"C","C#","D","Eb","E","F","F#","G","Ab","A","Bb","B"};
    if (midi < 0 || midi > 127) { snprintf(out, n, "--"); return; }
    snprintf(out, n, "%s%d", names[midi % 12], midi / 12 - 1);
}

static void pg_changed(void) { long long t = pg_now_ms(); g_pg_touch = t; g_pg_dirty = t; }

static void pg_step(int field, int d)
{
    if (field == 0) {
        g_scale_type = ((g_scale_type + d) % NUM_SCALES + NUM_SCALES) % NUM_SCALES;
    } else if (field == 1) {
        int r = g_root_note + d;
        g_root_note = r < 0 ? 0 : r > 127 ? 127 : r;
    } else {
        int o = g_octave_offset + d;
        g_octave_offset = o < -4 ? -4 : o > 4 ? 4 : o;
    }
    pg_changed();
}

static void pg_input(void *user, const mpc_sdraw_event *e)
{
    (void)user;
    g_pg_touch = pg_now_ms();
    if (e->type == MPC_SDRAW_EV_KEY && e->value) {
        if (e->id >= 0 && e->id <= 2) g_pg_field = e->id;
        else if (e->id == 3) g_pg_close = 1;
    } else if (e->type == MPC_SDRAW_EV_DIAL) {
        pg_step(g_pg_field, e->value);
    } else if (e->type == MPC_SDRAW_EV_KNOB && e->id >= 0 && e->id <= 2) {
        int v = e->value < 0 ? 0 : e->value > 127 ? 127 : e->value;
        g_pg_field = e->id;
        if (e->id == 0) g_scale_type = v * NUM_SCALES / 128;
        else if (e->id == 1) g_root_note = g_root_note - g_root_note % 12 + v * 12 / 128;
        else g_octave_offset = -4 + v * 9 / 128;
        pg_changed();
    }
}

static void pg_focus(void *user, int has_screen) { (void)user; (void)has_screen; }

static void pg_render(sdc_canvas *cv)
{
    sdc_fill(cv, 0, 0, cv->w, cv->h, PG_BG);

    static const char *keys[3] = {"SCALE", "ROOT", "OCT"};
    for (int i = 0; i < 3; i++) {
        int x = i * 107, w = i == 2 ? cv->w - x : 106;
        int sel = i == g_pg_field;
        sdc_fill(cv, x, 0, w, 20, sel ? PG_PINK : PG_PANEL);
        sdc_text(cv, x + (w - sdc_text_width(keys[i], 2)) / 2, 3, 2, sel ? PG_BG : PG_TEXT, keys[i]);
    }

    int sc = g_scale_type;
    if (sc < 0 || sc >= NUM_SCALES) sc = 0;
    char root[8], line[48];
    note_name(root, sizeof root, g_root_note);
    sdc_text(cv, 8, 28, 3, g_pg_field == 0 ? PG_PINK : PG_CYAN, scale_names[sc]);
    int x = sdc_text(cv, 8, 56, 2, g_pg_field == 1 ? PG_PINK : PG_TEXT, root);
    snprintf(line, sizeof line, "OCT %d", g_octave_offset);
    sdc_text(cv, x + 18, 56, 2, g_pg_field == 2 ? PG_PINK : PG_TEXT, line);
    const char *state = g_scale_on ? "ON" : "OFF";
    sdc_text(cv, cv->w - 8 - sdc_text_width(state, 2), 56, 2, g_scale_on ? PG_CYAN : PG_DIM, state);
    sdc_text(cv, 8, 76, 1, PG_DIM, "DIAL: CHANGE   PRESS: DONE   K1-K3: SET");

    /* banks A (bottom) to D (top), slots left to right */
    int hit = g_pg_hit_slot;
    int lit = hit >= 0 && pg_now_ms() - g_pg_hit_ms < 250;
    for (int bank = 0; bank < 4; bank++) {
        int y = 236 - (bank + 1) * 36;
        char b[2] = { (char)('A' + bank), 0 };
        sdc_text(cv, 4, y + 12, 2, PG_DIM, b);
        for (int p = 0; p < 4; p++) {
            int slot = bank * 4 + p, x = 22 + p * 74;
            int on = lit && slot == hit;
            sdc_fill(cv, x, y, 70, 32, on ? PG_PINK : PG_PANEL);
            char nn[8];
            int midi = slot_midi(slot);
            note_name(nn, sizeof nn, midi);
            int root_pc = midi % 12 == g_root_note % 12;
            sdc_text(cv, x + (70 - sdc_text_width(nn, 2)) / 2, y + 9, 2,
                     on ? PG_BG : root_pc ? PG_CYAN : PG_TEXT, nn);
        }
    }
}

static void *page_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 120 && !g_pg_api; i++) {   /* preload order: the draw handler may start after us */
        const mpc_sdraw_api *api = sdc_api();
        if (api && api->version >= 2 && api->set_input) g_pg_api = api;
        else sdc_sleep_ms(500);
    }
    if (!g_pg_api) { LOG("page: no draw handler with input — no SCALE page"); return NULL; }
    g_pg = g_pg_api->open("SCALE", pg_focus, NULL);
    if (!g_pg) { LOG("page: draw handler has no screen — no SCALE page"); return NULL; }
    g_pg_api->set_input(g_pg, pg_input, NULL);
    LOG("page: SCALE page ready");

    for (;;) {
        long long now = pg_now_ms();
        if (g_pg_want) {
            g_pg_want = 0;
            g_pg_touch = now;
            if (g_pg_api->request(g_pg) != 0) LOG("page: no display yet");
        }
        int up = g_pg_api->has_screen(g_pg);
        if (up && (g_pg_close || now - g_pg_touch > PAGE_IDLE_MS)) {
            g_pg_api->release(g_pg);
            up = 0;
        }
        g_pg_close = 0;
        if (up) {
            sdc_canvas cv;
            cv.px = g_pg_api->pixels(g_pg, &cv.stride);
            cv.w = g_pg_api->width;
            cv.h = g_pg_api->height;
            pg_render(&cv);
            g_pg_api->present(g_pg, 0, 0, 0, 0);
        }
        long long dirty = g_pg_dirty;
        if (dirty && now - dirty > PAGE_SAVE_MS) {
            g_pg_dirty = 0;
            save_conf();
        }
        sdc_sleep_ms(up ? 60 : 100);
    }
    return NULL;
}

static void page_start(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, page_thread, NULL) == 0) pthread_detach(t);
}
