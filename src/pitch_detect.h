/* pitch_detect.h: the pitch of a sample, for samples MPC has no root note for. Plain C, no MPC state: a host test feeds it
 * WAV files (tests/pitch_detect_test.c).
 *
 * YIN (de Cheveigne and Kawahara, 2002): the cumulative mean normalised difference of a window with itself, its first dip
 * under a threshold is the period, refined by a parabola. It is run on a few windows past the attack, and the clearest one
 * wins. Noise, most drums and dense chords have no clear dip, and then there is no pitch: the caller keeps MPC's root note.
 * A drum with a tuned tail (an 808 kick) reads as that tail's note. */
#include <fcntl.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#define PD_WIN      2048   /* frames per window (46 ms at 44.1 kHz) */
#define PD_MIN_HZ   30.0f
#define PD_MAX_HZ   2000.0f
#define PD_CLEAR    0.15f  /* YIN threshold */
#define PD_ACCEPT   0.10f  /* the most aperiodicity a result may have (host test: pitched sounds up to 0.08) */

/* read(user, frame, n, out): n mono frames from frame on into out; 0 on success */
typedef int (*pd_read_fn)(void *user, long frame, int n, float *out);

/* the period of one window, in frames; *aper is its aperiodicity (0 = perfectly periodic). 0 when there is none. */
static float pd_yin(const float *x, int w, int tmin, int tmax, float *aper)
{
    static float d[4096];
    if (tmax >= (int)(sizeof d / sizeof d[0])) tmax = (int)(sizeof d / sizeof d[0]) - 1;
    float run = 0;
    d[0] = 1;
    for (int t = 1; t <= tmax; t++) {
        float s = 0;
        for (int i = 0; i < w; i++) { float e = x[i] - x[i + t]; s += e * e; }
        run += s;
        d[t] = run > 0 ? s * t / run : 1;
    }
    int best = -1;
    for (int t = tmin; t <= tmax; t++) {
        if (d[t] < PD_CLEAR) {
            while (t + 1 <= tmax && d[t + 1] < d[t]) t++;
            best = t;
            break;
        }
    }
    if (best < 0) {   /* no dip under the threshold: report the deepest, for the aperiodicity */
        float m = 2; for (int t = tmin; t <= tmax; t++) if (d[t] < m) m = d[t];
        *aper = m;
        return 0;
    }
    *aper = d[best];
    float p = (float)best;
    if (best > tmin && best < tmax) {
        float a = d[best - 1], b = d[best], c = d[best + 1], den = a - 2 * b + c;
        if (den > 0) p += 0.5f * (a - c) / den;
    }
    return p;
}

/* The sample's pitch as a MIDI note (fractional), or -1 when it has none clear enough. rate: frames per second. */
static float pitch_detect(pd_read_fn rd, void *user, long frames, int rate, float *aper_out)
{
    static float x[PD_WIN + 4096];
    if (aper_out) *aper_out = 1;
    if (rate < 8000 || rate > 192000) return -1;
    int tmin = (int)(rate / PD_MAX_HZ), tmax = (int)(rate / PD_MIN_HZ) + 2;
    if (tmax > 4095) tmax = 4095;
    int need = PD_WIN + tmax + 1;
    if (frames < need) return -1;

    /* the loudest stretch of the first two seconds, then windows from just after it: the attack has the click, the tail the
     * noise floor */
    long span = frames < 2L * rate ? frames : 2L * rate, peak_at = 0;
    float peak = 0;
    for (long f = 0; f + PD_WIN <= span; f += PD_WIN) {
        if (rd(user, f, PD_WIN, x)) return -1;
        float e = 0; for (int i = 0; i < PD_WIN; i++) e += x[i] * x[i];
        if (e > peak) { peak = e; peak_at = f; }
    }
    if (peak / PD_WIN < 1e-7f) return -1;   /* silent (under -70 dBFS) */

    float best_p = 0, best_a = 2;
    static const float at[] = { 0.03f, 0.10f, 0.25f, 0.45f };   /* seconds after the loudest stretch starts */
    for (size_t k = 0; k < sizeof at / sizeof at[0]; k++) {
        long f = peak_at + (long)(at[k] * rate);
        if (f + need > frames) f = frames - need;
        if (f < 0) f = 0;
        if (rd(user, f, need, x)) return -1;
        float e = 0; for (int i = 0; i < PD_WIN; i++) e += x[i] * x[i];
        if (e / PD_WIN < peak / PD_WIN * 0.01f) continue;   /* 20 dB under the loudest: faded out */
        float a, p = pd_yin(x, PD_WIN, tmin, tmax, &a);
        if (a < best_a) { best_a = a; best_p = p; }
    }
    if (aper_out) *aper_out = best_a;
    if (best_p <= 0 || best_a > PD_ACCEPT) return -1;
    return 69.0f + 12.0f * log2f((float)rate / best_p / 440.0f);
}

/* ── WAV files (a sample MPC streams from disk is read from its file) ── */
struct pd_wav { int fd, ch, bits, fl, rate; long frames; long data; };

static uint32_t pd_le(const unsigned char *p, int n) { uint32_t v = 0; for (int i = n - 1; i >= 0; i--) v = v << 8 | p[i]; return v; }

/* 0 and *w ready (close w->fd when done), or -1: not a WAV of 16, 24 or 32-bit PCM or 32-bit float, 1 or 2 channels */
static int pd_wav_open(const char *path, struct pd_wav *w)
{
    unsigned char h[64];
    w->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (w->fd < 0) return -1;
    w->ch = 0; w->data = 0;
    int fmt = 0;
    if (pread(w->fd, h, 12, 0) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) goto bad;
    for (long p = 12, k = 0; k < 64; k++) {   /* chunks: fmt and data, the rest (smpl, LIST...) skipped */
        if (pread(w->fd, h, 8, p) != 8) break;
        uint32_t sz = pd_le(h + 4, 4);
        if (!memcmp(h, "fmt ", 4)) {
            if (sz < 16 || pread(w->fd, h + 8, sz < 40 ? sz : 40, p + 8) < 16) goto bad;
            fmt = (int)pd_le(h + 8, 2); w->ch = (int)pd_le(h + 10, 2); w->rate = (int)pd_le(h + 12, 4); w->bits = (int)pd_le(h + 22, 2);
            if (fmt == 0xfffe && sz >= 40) fmt = (int)pd_le(h + 32, 2);   /* extensible: the subformat's tag */
        } else if (!memcmp(h, "data", 4)) {
            w->data = p + 8;
            w->frames = w->ch && w->bits ? (long)(sz / (uint32_t)(w->ch * w->bits / 8)) : 0;
            break;
        }
        p += 8 + (long)sz + (sz & 1);
    }
    w->fl = fmt == 3;
    if (!w->data || w->ch < 1 || w->ch > 2 || !(fmt == 1 || fmt == 3) || !(w->bits == 16 || w->bits == 24 || w->bits == 32)
        || (w->fl && w->bits != 32))
        goto bad;
    off_t end = lseek(w->fd, 0, SEEK_END);   /* a data size past the end (a file being written): what is there */
    long have = end > w->data ? (long)((end - w->data) / (w->ch * w->bits / 8)) : 0;
    if (have < w->frames) w->frames = have;
    return 0;
bad:
    close(w->fd);
    w->fd = -1;
    return -1;
}

/* a pd_read_fn over a struct pd_wav: n frames, the channels mixed */
static int pd_wav_read(void *user, long frame, int n, float *out)
{
    struct pd_wav *w = user;
    static unsigned char raw[(PD_WIN + 4096) * 2 * 4];
    int bps = w->bits / 8, bpf = w->ch * bps;
    if (n > PD_WIN + 4096 || frame < 0 || frame + n > w->frames) return -1;
    if (pread(w->fd, raw, (size_t)n * bpf, w->data + frame * bpf) != (ssize_t)n * bpf) return -1;
    for (int i = 0; i < n; i++) {
        float s = 0;
        for (int c = 0; c < w->ch; c++) {
            const unsigned char *q = raw + i * bpf + c * bps;
            if (bps == 2) s += (int16_t)pd_le(q, 2) / 32768.0f;
            else if (bps == 3) s += (int32_t)(pd_le(q, 3) << 8) / 2147483648.0f;
            else if (w->fl) { float v; memcpy(&v, q, 4); s += v == v && v > -8 && v < 8 ? v : 0; }
            else s += (int32_t)pd_le(q, 4) / 2147483648.0f;
        }
        out[i] = s / w->ch;
    }
    return 0;
}
