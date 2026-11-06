/* Host test for src/pitch_detect.h: made-up sounds with a known pitch (or none), then any WAV files given, each printed.
 *   cc -O2 -o /tmp/pdt tests/pitch_detect_test.c -lm && /tmp/pdt [file.wav[=midi|=none] ...]
 * A WAV (read by pd_wav_open, as on the device) is 16, 24 or 32-bit PCM or 32-bit float; =midi checks the pitch class it detects, =none that it detects none. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../src/pitch_detect.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

struct buf { float *x; long n; };
static int rd(void *u, long f, int n, float *out)
{
    struct buf *b = u;
    if (f < 0 || f + n > b->n) return -1;
    memcpy(out, b->x + f, (size_t)n * sizeof *out);
    return 0;
}

static unsigned rnd = 1;
static float noise(void) { rnd = rnd * 1103515245u + 12345u; return ((rnd >> 9) & 0xffff) / 32768.0f - 1; }

enum { SAW, SINE, SQUARE, NOISE, SNARE, KICK, PLUCK };
static struct buf make(int kind, float midi, int rate, float secs)
{
    struct buf b = { malloc(sizeof(float) * (size_t)(rate * secs)), (long)(rate * secs) };
    float hz = 440.0f * powf(2, (midi - 69) / 12), ph = 0;
    for (long i = 0; i < b.n; i++) {
        float t = (float)i / rate, env = t < 0.005f ? t / 0.005f : 1, v;
        ph += hz / rate; ph -= floorf(ph);
        switch (kind) {
        case SAW:    v = 2 * ph - 1; break;
        case SINE:   v = sinf(6.2831853f * ph); break;
        case SQUARE: v = ph < 0.5f ? 1 : -1; break;
        case NOISE:  v = noise(); break;
        case KICK:   v = sinf(6.2831853f * (50 * t + 120 * (1 - expf(-t * 30)) / 30)) * expf(-t * 8) + noise() * expf(-t * 60); break;
        case SNARE:  v = noise() * expf(-t * 12) + 0.3f * sinf(6.2831853f * 190 * t) * expf(-t * 40); break;
        default:     v = (2 * ph - 1) * expf(-t * 3) + noise() * 0.02f; break;  /* PLUCK: a decaying saw with hiss */
        }
        b.x[i] = 0.5f * env * v;
    }
    return b;
}

static float detect(struct buf b, int rate, float *aper) { return pitch_detect(rd, &b, b.n, rate, aper); }

static int pc_ok(float got, int want) { int g = (int)lrintf(got); return ((g - want) % 12 + 12) % 12 == 0; }

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    static const char *kn[] = { "saw", "sine", "square", "noise", "snare", "kick", "pluck" };
    float a;
    for (int kind = SAW; kind <= PLUCK; kind++) {
        if (kind == KICK) continue;
        int pitched = kind != NOISE && kind != SNARE;
        for (int m = 28; m <= 96; m += 5) {   /* E1 to C7 */
            if (!pitched && m > 33) break;
            for (int ri = 0; ri < 2; ri++) {
                int rate = ri ? 48000 : 44100;
                struct buf b = make(kind, (float)m, rate, 1.5f);
                float got = detect(b, rate, &a);
                if (pitched) CHECK(got >= 0 && fabsf(got - m) < 0.2f, "%s midi %d at %d: got %.2f (aper %.3f)", kn[kind], m, rate, got, a);
                else CHECK(got < 0, "%s at %d: got %.2f (aper %.3f), wanted none", kn[kind], rate, got, a);
                free(b.x);
            }
        }
    }
    /* a kick whose tail settles at 50 Hz reads as G1, like a tuned 808 */
    struct buf k = make(KICK, 0, 44100, 1.0f);
    float kg = detect(k, 44100, &a);
    CHECK(pc_ok(kg, 31), "kick: got %.2f, wanted G (50 Hz tail)", kg);
    free(k.x);
    /* detuned by 30 cents: still that note's name */
    struct buf b = make(SAW, 52.3f, 44100, 1.0f);
    float got = detect(b, 44100, &a);
    CHECK(pc_ok(got, 52), "saw E3+30c: got %.2f", got);
    free(b.x);
    /* too short and silent: none */
    b = make(SAW, 60, 44100, 0.05f);
    CHECK(detect(b, 44100, &a) < 0, "50 ms: wanted none");
    for (long i = 0; i < b.n; i++) b.x[i] = 0;
    free(b.x);
    b = make(SINE, 60, 44100, 1.0f);
    for (long i = 0; i < b.n; i++) b.x[i] = 0;
    CHECK(detect(b, 44100, &a) < 0, "silence: wanted none");
    free(b.x);

    for (int i = 1; i < argc; i++) {
        char path[1024]; snprintf(path, sizeof path, "%s", argv[i]);
        char *eq = strrchr(path, '='); int want = -2;
        if (eq) { *eq = 0; want = !strcmp(eq + 1, "none") ? -1 : atoi(eq + 1); }
        struct pd_wav w;
        if (pd_wav_open(path, &w)) { CHECK(0, "%s: can't read", path); continue; }
        got = pitch_detect(pd_wav_read, &w, w.frames, w.rate, &a);
        printf("%s: %.2f (aper %.3f)\n", path, got, a);
        if (want == -1) CHECK(got < 0, "%s: wanted none, got %.2f", path, got);
        else if (want >= 0) CHECK(got >= 0 && pc_ok(got, want), "%s: wanted pitch class of %d, got %.2f", path, want, got);
        close(w.fd);
    }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
