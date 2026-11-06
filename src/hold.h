/* hold.h: a long hold of 16 Levels toggles Scale (included once, by enable_scalemode.c).
 *
 * MPC reads the Sample's buttons from the control-surface rawmidi port (/dev/snd/midiC0D0) in ALSA's timestamped
 * framing: whole 32-byte frames (type, length, 2 reserved, timestamps, then up to 16 MIDI bytes at offset 16), one
 * button per frame, note on/off on channel 1. MPC acts on the press, so the 16 Levels press frame is held back: let go
 * within hold_ms and MPC gets the press then, and the release on its next read (poll() and ppoll() are told about it);
 * held past hold_ms, Scale toggles and MPC sees nothing. With Shift down the press passes straight through, so
 * Shift+16 Levels (the Type cycle) is unchanged. The draw handler addin filters the same port the same way for PAD FX;
 * both wrap read() and pass on to the next library, so they chain. */
#include <fcntl.h>
#include <poll.h>
#include <signal.h>

#define HOLD_FRAME 32
#define HOLD_DATA 16
#define HOLD_MAXFD 1024
#define SHIFT_NOTE 49

static void page_popup(void);

static pthread_mutex_t g_hold_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned char g_hold_cls[HOLD_MAXFD];   /* 0 unknown, 1 the control surface, 2 not */
static unsigned char g_hold_down[HOLD_FRAME];
static unsigned char g_hold_pend[2 * HOLD_FRAME];
static int g_hold_npend, g_hold_pend_fd = -1;
static int g_hold_held, g_hold_fired, g_shift_down;
static long long g_hold_since;
static volatile int g_hold_on;

static long long hold_now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static int hold_is_surface(int fd)
{
    if (fd < 0 || fd >= HOLD_MAXFD) return 0;
    if (g_hold_cls[fd]) return g_hold_cls[fd] == 1;
    char p[64], t[128];
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(p, t, sizeof t - 1);
    if (n <= 0) return 0;
    t[n] = 0;
    g_hold_cls[fd] = strstr(t, "/dev/snd/midiC0D0") ? 1 : 2;
    return g_hold_cls[fd] == 1;
}

static int hold_framed(const unsigned char *b, ssize_t n)
{
    if (n < HOLD_FRAME || n % HOLD_FRAME) return 0;
    for (ssize_t f = 0; f < n; f += HOLD_FRAME)
        if (b[f] != 0 || b[f + 1] < 1 || b[f + 1] > HOLD_DATA || b[f + 2] || b[f + 3]) return 0;
    return 1;
}

/* Frames MPC just read (lock held): returns the new length. */
static ssize_t hold_filter(int fd, unsigned char *b, ssize_t n)
{
    if (!hold_framed(b, n)) return n;
    ssize_t o = 0;
    for (ssize_t f = 0; f + HOLD_FRAME <= n; f += HOLD_FRAME) {
        unsigned char *fr = b + f, *d = fr + HOLD_DATA;
        if (fr[1] == 3 && (d[0] == 0x90 || d[0] == 0x80)) {
            int down = d[0] == 0x90 && d[2];
            if (d[1] == SHIFT_NOTE) g_shift_down = down;
            if (d[1] == C.hold_note) {
                if (down && !g_shift_down) {
                    memcpy(g_hold_down, fr, HOLD_FRAME);
                    g_hold_held = 1; g_hold_fired = 0; g_hold_since = hold_now_ms();
                    continue;
                }
                if (!down && g_hold_held) {
                    g_hold_held = 0;
                    if (!g_hold_fired && g_hold_npend + HOLD_FRAME <= (int)sizeof g_hold_pend) {
                        memcpy(g_hold_pend + g_hold_npend, fr, HOLD_FRAME);
                        g_hold_npend += HOLD_FRAME; g_hold_pend_fd = fd;
                        memcpy(b + o, g_hold_down, HOLD_FRAME); o += HOLD_FRAME;
                    }
                    continue;
                }
            }
        }
        if (o != f) memmove(b + o, fr, HOLD_FRAME);
        o += HOLD_FRAME;
    }
    return o;
}

__attribute__((visibility("default"))) ssize_t read(int fd, void *buf, size_t n)
{
    static ssize_t (*real_read)(int, void *, size_t);
    if (!real_read) real_read = (ssize_t (*)(int, void *, size_t))dlsym(RTLD_NEXT, "read");
    if (!g_hold_on || n == 0 || !hold_is_surface(fd)) return real_read(fd, buf, n);
    for (;;) {
        pthread_mutex_lock(&g_hold_mu);
        if (g_hold_npend && g_hold_pend_fd == fd) {   /* the held-back release, a read of its own */
            size_t k = (size_t)g_hold_npend < n ? (size_t)g_hold_npend : n;
            memcpy(buf, g_hold_pend, k);
            memmove(g_hold_pend, g_hold_pend + k, (size_t)g_hold_npend - k);
            g_hold_npend -= (int)k;
            pthread_mutex_unlock(&g_hold_mu);
            return (ssize_t)k;
        }
        pthread_mutex_unlock(&g_hold_mu);
        ssize_t r = real_read(fd, buf, n);
        if (r <= 0) return r;
        pthread_mutex_lock(&g_hold_mu);
        ssize_t k = hold_filter(fd, buf, r);
        pthread_mutex_unlock(&g_hold_mu);
        if (k > 0) return k;
        if (fcntl(fd, F_GETFL) & O_NONBLOCK) { errno = EAGAIN; return -1; }   /* never hand MPC 0 (end of file) */
    }
}

__attribute__((visibility("default"))) int close(int fd)
{
    static int (*real_close)(int);
    if (!real_close) real_close = (int (*)(int))dlsym(RTLD_NEXT, "close");
    if (fd >= 0 && fd < HOLD_MAXFD) g_hold_cls[fd] = 0;
    return real_close(fd);
}

static int hold_pending(void)
{
    pthread_mutex_lock(&g_hold_mu);
    int fd = g_hold_npend ? g_hold_pend_fd : -1;
    pthread_mutex_unlock(&g_hold_mu);
    return fd;
}

/* A held-back release makes its fd readable. */
static int hold_mark(struct pollfd *fds, nfds_t nf, int r)
{
    int fd = hold_pending();
    if (fd < 0) return r;
    for (nfds_t i = 0; i < nf; i++)
        if (fds[i].fd == fd && (fds[i].events & POLLIN) && !(fds[i].revents & POLLIN)) {
            fds[i].revents |= POLLIN;
            r = r < 0 ? 1 : r + 1;
        }
    return r;
}

__attribute__((visibility("default"))) int ppoll(struct pollfd *fds, nfds_t nf, const struct timespec *ts,
                                                 const sigset_t *ss)
{
    static int (*real_ppoll)(struct pollfd *, nfds_t, const struct timespec *, const sigset_t *);
    if (!real_ppoll)
        real_ppoll = (int (*)(struct pollfd *, nfds_t, const struct timespec *, const sigset_t *))dlsym(RTLD_NEXT, "ppoll");
    struct timespec zero = { 0, 0 };
    if (hold_pending() >= 0) ts = &zero;
    return hold_mark(fds, nf, real_ppoll(fds, nf, ts, ss));
}

static void *hold_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int fire = 0;
        pthread_mutex_lock(&g_hold_mu);
        if (g_hold_held && !g_hold_fired && hold_now_ms() - g_hold_since >= C.hold_ms) { g_hold_fired = 1; fire = 1; }
        pthread_mutex_unlock(&g_hold_mu);
        if (fire) {
            g_scale_on = !g_scale_on;
            LOG("16 Levels held: Scale %s", g_scale_on ? "on" : "off");
            if (C.page) page_popup();
        }
        struct timespec ts = { 0, 20 * 1000000L };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void hold_start(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, hold_thread, NULL) == 0) pthread_detach(t);
    g_hold_on = 1;
}
