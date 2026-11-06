/*
 * mpc-addin-enable-scalemode
 *
 * Enables the dark-shipped Pad Perform / Scale Mode on MPC devices where the
 * full infrastructure is compiled in but gated by hardware-type checks.
 *
 * The MPC firmware contains the complete PadPerform system — settings,
 * dialogue, note filtering, pad lighting, settings persistence — and the
 * pad-mode cycling already includes PadPerform as mode 1 (Full Level → 16
 * Level → Pad Perform → Full Level). On some models the mode either isn't
 * reachable or doesn't function because certain plumbing functions are
 * gated on AcvxHardware::Type.
 *
 * This library locates those gates at runtime via signature scanning and
 * patches them so the native Pad Perform code path activates.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define LOG(fmt, ...) fprintf(stderr, "enable-scalemode: " fmt "\n", ##__VA_ARGS__)

/* ── conf ── */

struct conf {
    int enabled;
    int padperform_patch; /* force the PadPerform "supported" bool (blocks 16 Levels) */
};

static struct conf C = { .enabled = 1, .padperform_patch = 0 };

static void addin_dir(char *out, size_t n)
{
    out[0] = 0;
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) return;
    uintptr_t me = (uintptr_t)&addin_dir;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long a, b;
        char *path = strchr(line, '/');
        if (!path || sscanf(line, "%lx-%lx", &a, &b) != 2 || me < a || me >= b)
            continue;
        path[strcspn(path, "\n")] = 0;
        char *slash = strrchr(path, '/');
        if (slash) *slash = 0;
        snprintf(out, n, "%s", path);
        break;
    }
    fclose(f);
}

static void load_conf(void)
{
    char dir[256], path[512];
    addin_dir(dir, sizeof dir);
    if (!dir[0]) return;
    snprintf(path, sizeof path, "%s/mpc_enable_scalemode.conf", dir);

    FILE *f = fopen(path, "re");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq++ = 0;
        char *val = eq;
        val[strcspn(val, "\n\r")] = 0;
        if (!strcmp(line, "enabled")) C.enabled = atoi(val);
        else if (!strcmp(line, "padperform_patch")) C.padperform_patch = atoi(val);
    }
    fclose(f);
}

/* ── process memory helpers ── */

struct mpc_mapping {
    uintptr_t base;       /* PIE load base (first mapping of the binary) */
    uintptr_t text_start; /* first r-x page */
    uintptr_t text_end;   /* end of last r-x page */
    uintptr_t ro_start;   /* first r-- page (contains .rodata) */
    uintptr_t ro_end;     /* end of readable range */
};

static struct mpc_mapping find_mpc(void)
{
    struct mpc_mapping m = {0};
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) return m;

    char line[512];
    int found = 0;

    while (fgets(line, sizeof line, f)) {
        unsigned long a, b;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %4s", &a, &b, perms) < 3) continue;

        char *path = strchr(line, '/');
        if (path) {
            path[strcspn(path, "\n")] = 0;
            const char *bn = strrchr(path, '/');
            bn = bn ? bn + 1 : path;
            if (!strcmp(bn, "MPC") || !strcmp(bn, "MPC Sample"))
                found = 1;
            else if (found)
                break;
        }

        if (!found) continue;

        if (!m.base) m.base = (uintptr_t)a;

        if (perms[0] == 'r') {
            if (!m.ro_start) m.ro_start = (uintptr_t)a;
            m.ro_end = (uintptr_t)b;
        }
        if (perms[0] == 'r' && perms[2] == 'x') {
            if (!m.text_start) m.text_start = (uintptr_t)a;
            m.text_end = (uintptr_t)b;
        }
    }
    fclose(f);
    return m;
}

/* ── AArch64 instruction helpers ── */

static inline uint32_t insn_at(uintptr_t addr)
{
    return *(volatile uint32_t *)addr;
}

static int make_writable(uintptr_t addr, size_t len)
{
    uintptr_t page = addr & ~(uintptr_t)0xFFF;
    size_t span = (addr + len) - page;
    span = (span + 0xFFF) & ~(size_t)0xFFF;
    return mprotect((void *)page, span, PROT_READ | PROT_WRITE | PROT_EXEC);
}

static int patch_insn(uintptr_t addr, uint32_t expected, uint32_t replacement)
{
    uint32_t actual = insn_at(addr);
    if (actual != expected) {
        LOG("  instruction mismatch at %p: expected 0x%08x, got 0x%08x",
            (void *)addr, expected, actual);
        return -1;
    }
    if (make_writable(addr, 4) != 0) {
        LOG("  mprotect failed at %p: %s", (void *)addr, strerror(errno));
        return -1;
    }
    *(volatile uint32_t *)addr = replacement;
    __builtin___clear_cache((void *)addr, (void *)(addr + 4));
    return 0;
}

/* ── signature scanning ── */

static uintptr_t find_string_in_range(uintptr_t start, uintptr_t end, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || end <= start) return 0;
    for (uintptr_t p = start; p <= end - nlen; p++) {
        if (memcmp((const void *)p, needle, nlen) == 0 &&
            *(const char *)(p + nlen) == '\0')
            return p;
    }
    return 0;
}

static uintptr_t find_adrp_add_ref(uintptr_t text_base, size_t text_size, uintptr_t target)
{
    uintptr_t target_page = target & ~(uintptr_t)0xFFF;
    unsigned target_off = target & 0xFFF;

    for (uintptr_t pc = text_base; pc < text_base + text_size - 4; pc += 4) {
        uint32_t w = insn_at(pc);
        if ((w & 0x9F000000) != 0x90000000) continue;

        unsigned rd = w & 0x1F;
        int immhi = (int32_t)(w & 0x00FFFFE0) >> 5;
        unsigned immlo = (w >> 29) & 3;
        int64_t imm = ((int64_t)immhi << 2) | immlo;
        uintptr_t adrp_page = (pc & ~(uintptr_t)0xFFF) + (uintptr_t)(imm << 12);

        if (adrp_page != target_page) continue;

        uint32_t w2 = insn_at(pc + 4);
        if ((w2 & 0xFFC00000) != 0x91000000) continue;
        unsigned rn2 = (w2 >> 5) & 0x1F;
        unsigned rd2 = w2 & 0x1F;
        unsigned imm12 = (w2 >> 10) & 0xFFF;

        if (rn2 != rd || rd2 != rd) continue;
        if (imm12 != target_off) continue;

        return pc;
    }
    return 0;
}

static uintptr_t decode_bl(uintptr_t pc)
{
    uint32_t w = insn_at(pc);
    if ((w & 0xFC000000) != 0x94000000) return 0;
    int32_t imm26 = (int32_t)(w & 0x03FFFFFF);
    if (imm26 & 0x02000000) imm26 |= (int32_t)0xFC000000;
    return pc + ((int64_t)imm26 << 2);
}

/*
 * Decode an ADRP at `pc`: returns the page address it produces.
 * Returns 0 if the instruction isn't ADRP.
 */
static uintptr_t decode_adrp(uintptr_t pc, unsigned *rd_out)
{
    uint32_t w = insn_at(pc);
    if ((w & 0x9F000000) != 0x90000000) return 0;
    if (rd_out) *rd_out = w & 0x1F;
    int immhi = (int32_t)(w & 0x00FFFFE0) >> 5;
    unsigned immlo = (w >> 29) & 3;
    int64_t imm = ((int64_t)immhi << 2) | immlo;
    return (pc & ~(uintptr_t)0xFFF) + (uintptr_t)(imm << 12);
}

/* ── patch logic ── */

/*
 * ADA2PadMode enum:
 *   0 = Full Level (normal)
 *   1 = Pad Perform (scale/chord mode)
 *   2 = 16 Level
 *
 * The native pad-mode cycling function (scale_cycle) already includes mode 1.
 * The cycle order is 0 → 2 → 1 → 0 (Full Level → 16 Level → Pad Perform →
 * Full Level). On models where PadPerform works, pressing the 16 Level button
 * twice enters Pad Perform mode.
 *
 * The issue on gated models is that the PadPerform plumbing functions either
 * don't initialize, have wrong behaviour for the hardware, or the mode is
 * visible but non-functional.
 *
 * Our approach: find the specific plumbing functions that need adjustment
 * and apply the minimum patches to make the native code functional.
 */

/*
 * Verify the pad-mode cycling function has the expected 3-mode switch.
 *
 * Expected pattern at cycle_func+0x30:
 *   LDR W0, [Xn, #0x34]   ; load current mode
 *   CMP W0, #1             ; mode == Pad Perform?
 *   B.EQ ...               ; handle mode 1
 *   CBZ W0, ...            ; mode == 0?
 *   CMP W0, #2             ; mode == 16 Level?
 *   B.EQ ...               ; handle mode 2
 */
static int verify_cycle_switch(uintptr_t func)
{
    /* The switch is after the prologue + register saves.
     * Scan for CMP W0, #1 followed by B.EQ, CBZ W0, CMP W0, #2. */
    for (unsigned off = 0x20; off < 0x60; off += 4) {
        uint32_t w = insn_at(func + off);
        if (w != 0x7100041F) continue; /* CMP W0, #1 */

        uint32_t w2 = insn_at(func + off + 4);
        if ((w2 & 0xFF000010) != 0x54000000 || (w2 & 0xF) != 0) continue; /* B.EQ */

        uint32_t w3 = insn_at(func + off + 8);
        if ((w3 & 0x7FE00000) != 0x34000000) continue; /* CBZ Wn */

        uint32_t w4 = insn_at(func + off + 12);
        if (w4 != 0x7100081F) continue; /* CMP W0, #2 */

        LOG("  mode switch verified at %p+0x%x", (void *)func, off);
        return 1;
    }
    return 0;
}

/*
 * Find the pad-mode cycling function by locating its unique string reference.
 *
 * After the mode switch, the notification code loads "16 Level" via ADRP+ADD.
 * We find the "16 Level" string, find its code reference, then walk back to
 * find the cycling function that contains the 3-mode switch.
 */
static uintptr_t find_cycle_func(struct mpc_mapping *m)
{
    uintptr_t str_addr = find_string_in_range(m->ro_start, m->ro_end, "16 Level");
    if (!str_addr) {
        LOG("  '16 Level' string not found");
        return 0;
    }

    /* The cycling function references "16 Level" as a notification title.
     * Search for ADRP+ADD refs and check which one is inside a function
     * that has the 3-mode switch. */
    uintptr_t search_start = m->text_start;
    for (int attempt = 0; attempt < 10; attempt++) {
        uintptr_t ref = find_adrp_add_ref(search_start,
                                           m->text_end - search_start, str_addr);
        if (!ref) break;

        /* Walk back to find function start */
        uintptr_t func = 0;
        for (uintptr_t pc = ref; pc >= m->text_start && pc > ref - 4096; pc -= 4) {
            uint32_t w = insn_at(pc);
            if (w == 0xD503233F) { /* PACIASP */
                func = pc;
                break;
            }
            if ((w & 0xFFE003FF) == 0xA98003FD) { /* STP X29, X30, [SP, #-N]! */
                func = pc;
                break;
            }
        }

        if (func && verify_cycle_switch(func)) {
            LOG("  pad-mode cycling function at %p", (void *)func);
            return func;
        }

        search_start = ref + 4;
    }
    return 0;
}

/*
 * Find where PadPerformSettingsBase is constructed by locating a reference
 * to the "padPerformButton" string in the same mega-constructor that sets
 * up the control surface. Near this reference, there's a BL to the function
 * that registers the button with a mode enum value.
 */

static int try_patch(struct mpc_mapping *m)
{
    int patches_applied = 0;

    /*
     * Step 1: Verify the binary has PadPerform infrastructure.
     */
    uintptr_t ppb_str = find_string_in_range(m->ro_start, m->ro_end, "padPerformButton");
    if (!ppb_str) {
        LOG("padPerformButton not found — firmware may not have PadPerform");
        return -1;
    }
    LOG("padPerformButton at %p", (void *)ppb_str);

    uintptr_t ppb_ref = find_adrp_add_ref(m->text_start, m->text_end - m->text_start, ppb_str);
    if (!ppb_ref) {
        LOG("no code reference to padPerformButton");
        return -1;
    }
    LOG("padPerformButton code ref at %p", (void *)ppb_ref);

    uintptr_t pps_str = find_string_in_range(m->ro_start, m->ro_end, "PadPerform.Type");
    if (pps_str)
        LOG("PadPerform.Type at %p", (void *)pps_str);

    /*
     * Step 2: Find and verify the pad-mode cycling function.
     */
    uintptr_t cycle_func = find_cycle_func(m);
    if (cycle_func) {
        LOG("pad-mode cycle function at %p (mode 1 = Pad Perform)", (void *)cycle_func);
    } else {
        LOG("warning: could not locate pad-mode cycling function");
    }

    /*
     * Step 2b: Patch set_mode notification crash.
     *
     * set_mode (called by the cycling function) crashes at +0xF4 during
     * the notification listener loop. The code does:
     *   +0xEC: LDR X2, [X0]          ; load vtable from listener
     *   +0xF0: MOV W1, W22           ; mode arg
     *   +0xF4: LDR X2, [X2, #0x28]  ; load vfunc  ← CRASH: bad vtable
     *   +0xF8: BLR X2                ; call
     *   +0xFC: B → loop
     *
     * On the Sample, one listener's vtable pointer is invalid (points at
     * executable code instead of a vtable). NOP the vtable deref and the
     * call so the mode gets stored (at +0x60) and the notification loop
     * completes without crashing.
     */
    if (cycle_func) {
        uintptr_t set_mode = decode_bl(cycle_func + 0x84);
        if (set_mode) {
            LOG("  set_mode at %p (from cycle+0x84)", (void *)set_mode);

            /*
             * The notification listener loop starts at +0x64 with a
             * sentinel check:
             *   +0x60: STR W22, [X20, #0x1C]  ; store new mode ← already done
             *   +0x64: ADD X21, X20, #8
             *   +0x68: CMP X19, X21
             *   +0x6C: B.EQ +0x34 → +0xA0     ; skip if list empty
             *
             * Patch B.EQ to unconditional B so the loop is always
             * skipped. The mode is stored at +0x60 before this point.
             */
            uint32_t w_6c = insn_at(set_mode + 0x6C);
            if (w_6c == 0x540001A0) {
                if (patch_insn(set_mode + 0x6C, 0x540001A0, 0x1400000D) == 0) {
                    patches_applied++;
                    LOG("  notification loop skipped — set_mode won't crash");
                }
            } else {
                LOG("  set_mode+0x6C: 0x%08x (expected 0x540001A0 B.EQ)", w_6c);
            }
        }
    }

    /*
     * Step 3: Find the PadPerformSettings "supported" bool.
     *
     * The engine init loads a bool from the capability struct via
     *   LDRB Wn, [Xm, #432]
     * where Xm was set earlier by
     *   ADD Xm, X_, #0xB40
     * (engine 'this' + 0xB40 = capability sub-struct; byte at +432
     * within that sub-struct = PadPerform supported flag).
     *
     * We scan .text for every LDRB with offset 432 and confirm each by
     * looking backward for the matching ADD with offset 0xB40 on the
     * same destination register.  The combination is unique.
     */
    for (uintptr_t pc = m->text_start; C.padperform_patch && pc < m->text_end; pc += 4) {
        uint32_t w = insn_at(pc);
        if ((w & 0xFFFFFC00) != 0x3946C000) continue;

        unsigned rt = w & 0x1F;
        unsigned rn = (w >> 5) & 0x1F;

        /* Confirm: ADD X<rn>, X<any>, #0xB40 within 2048 bytes before */
        uint32_t add_check = 0x912D0000 | rn;  /* ADD Xrn, X?, #0xB40 */
        int confirmed = 0;
        for (uintptr_t s = pc - 4; s >= m->text_start && s >= pc - 2048; s -= 4) {
            if ((insn_at(s) & 0xFFFFFC1F) == add_check) {
                confirmed = 1;
                break;
            }
        }
        if (!confirmed) continue;

        LOG("  LDRB W%u, [X%u, #432] at %p (offset 0x%lx) — PadPerform bool",
            rt, rn, (void *)pc, (unsigned long)(pc - m->base));

        uint32_t mov_w1 = 0x52800020 | rt;
        if (patch_insn(pc, w, mov_w1) == 0) {
            patches_applied++;
            LOG("  PadPerform bool forced to true");
        }
        break;
    }
    /*
     * Step 4: Look for the hardware type comparison gate.
     *
     * The MPC firmware uses a generation mapper that buckets hardware types
     * into tiers (0-4) by comparing the enum value against thresholds:
     *   CMP Wn, #4  → gen 0
     *   CMP Wn, #9  → gen 1
     *   CMP Wn, #19 → gen 2
     *   CMP Wn, #24 → gen 3
     *   CMP Wn, #29 → gen 4
     *
     * This mapper is inlined at each use site. PadPerform is available on
     * Gen2+ models (ACVA2, ACVB, ACVC, ACVF, ACVG, ACVM, ACVR).
     *
     * We search for an inlined generation check near PadPerform-related code.
     * If we find one that gates PadPerform, we can patch the comparison
     * threshold or the conditional branch.
     */

    /* Search a region around the padPerformButton reference for the
     * generation-mapper pattern: CMP Wn, #4 followed within ~20 insns
     * by CMP Wn, #9. */
    uintptr_t gen_gate = 0;
    for (uintptr_t pc = ppb_ref - 4096; pc < ppb_ref + 4096; pc += 4) {
        if (pc < m->text_start || pc >= m->text_end - 80) continue;
        uint32_t w = insn_at(pc);
        /* CMP Wn, #4: 0111 0001 0000 0000 0001 00 nnnnn 11111
         * = 0x7100_101F with Rn mask */
        if ((w & 0xFFFFFC1F) != 0x7100101F) continue;
        unsigned rn = (w >> 5) & 0x1F;

        /* Look for CMP same_Rn, #9 within next 20 insns */
        for (unsigned off2 = 4; off2 <= 80; off2 += 4) {
            uint32_t w2 = insn_at(pc + off2);
            unsigned expected_cmp9 = 0x7100241F | (rn << 5);
            if (w2 == expected_cmp9) {
                /* Found CMP #4 + CMP #9 pair — this is a generation check */
                LOG("  generation check at %p (CMP W%u, #4 ... CMP W%u, #9)",
                    (void *)pc, rn, rn);
                gen_gate = pc;
                break;
            }
        }
        if (gen_gate) break;
    }

    if (!gen_gate)
        LOG("  no generation gate found near padPerformButton ref — not needed on this device");

    /*
     * Step 5: Apply patches.
     *
     * The exact patches depend on what was found above. The approach is:
     *
     * If a generation gate was found near PadPerform code:
     *   → Patch the B.LE after CMP #4 or CMP #9 to fall through
     *   → This makes the generation check return a higher tier
     *
     * If PadPerformSettingsBase(false) was found:
     *   → Patch MOV W1, #0 to MOV W1, #1
     *   → This passes true to the constructor
     *
     * If neither was found, we log diagnostic info for hardware testing.
     */

    if (gen_gate) {
        /* The generation gate typically looks like:
         *   CMP Wn, #4
         *   MOV W0, #0
         *   B.LE exit     ← patch this to NOP
         *
         * By NOP-ing the B.LE, execution falls through to the next
         * comparison, which will classify this hardware as a higher
         * generation tier.
         */
        uint32_t w_cmp = insn_at(gen_gate);
        uint32_t w_mov = insn_at(gen_gate + 4);
        uint32_t w_ble = insn_at(gen_gate + 8);

        /* Verify pattern: CMP Wn, #4; MOV W0, #0; B.LE */
        if ((w_mov & 0xFFFFFFFF) == 0x52800000 &&  /* MOV W0, #0 */
            (w_ble & 0xFF00001F) == 0x5400000D) {   /* B.LE */
            LOG("  patching generation gate at %p: NOP the B.LE", (void *)(gen_gate + 8));
            if (patch_insn(gen_gate + 8, w_ble, 0xD503201F /* NOP */) == 0) {
                patches_applied++;
                LOG("  generation gate patched ✓");
            }
        } else {
            LOG("  generation gate pattern not as expected — skipping");
            LOG("    CMP:  0x%08x", w_cmp);
            LOG("    +4:   0x%08x", w_mov);
            LOG("    +8:   0x%08x", w_ble);
        }
    }

    if (patches_applied == 0) {
        LOG("no patches applied — logging diagnostics for hardware testing:");
        LOG("  PIE base: %p", (void *)m->base);
        LOG("  padPerformButton ref: %p (offset 0x%lx)",
            (void *)ppb_ref, (unsigned long)(ppb_ref - m->base));
        if (cycle_func)
            LOG("  cycle func: %p (offset 0x%lx)",
                (void *)cycle_func, (unsigned long)(cycle_func - m->base));
        LOG("  test: press 16 Level button twice — does mode 1 (Pad Perform) activate?");
        LOG("  if the mode cycles but doesn't function, hooks are needed");
    }

    return patches_applied > 0 ? 0 : 1;
}

/* ── PACIASP function hook infrastructure ── */

#define NOTE_MAP_OFFSET   0x21472f0
#define NOTE_SETTER_OFFSET 0x27e4400

static const int scale_intervals[][12] = {
    {0,1,2,3,4,5,6,7,8,9,10,11}, /* Chromatic  (12) */
    {0,2,4,5,7,9,11,0,0,0,0,0},  /* Major       (7) */
    {0,2,3,5,7,8,10,0,0,0,0,0},  /* Natural Min (7) */
    {0,2,3,5,7,8,11,0,0,0,0,0},  /* Harm Minor  (7) */
    {0,2,4,5,7,9,10,0,0,0,0,0},  /* Dorian      (7) */
    {0,3,5,6,7,10,0,0,0,0,0,0},  /* Blues        (6) */
    {0,1,3,5,7,8,10,0,0,0,0,0},  /* Phrygian    (7) */
    {0,2,4,5,7,9,10,0,0,0,0,0},  /* Mixolydian  (7) */
};
static const int scale_npo[] = {12, 7, 7, 7, 7, 6, 7, 7};
#define NUM_SCALES ((int)(sizeof scale_npo / sizeof *scale_npo))

static const char *scale_names[] = {
    "Chromatic","Major","Minor","Harm Minor",
    "Dorian","Blues","Phrygian","Mixolydian",
};

static volatile int g_scale_type    = 5;  /* Blues */
static volatile int g_root_note     = 48; /* C3 */
static volatile int g_octave_offset = 2;
static volatile int g_pad_root      = 60; /* sample root fallback (C4) */

typedef void (*note_setter_fn_t)(void *, int, int, int);
static note_setter_fn_t g_note_setter;

static volatile int g_hook_hits;

static int parse_note_name(const char *s)
{
    static const char *sharps[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    static const char *flats[]  = {"C","Db","D","Eb","E","F","Gb","G","Ab","A","Bb","B"};
    int note_class = -1;
    const char *rest = s;
    for (int i = 0; i < 12; i++) {
        size_t slen = strlen(sharps[i]);
        if (strncasecmp(s, sharps[i], slen) == 0 && slen >= (size_t)(note_class >= 0 ? strlen(sharps[note_class]) : 0))
            { note_class = i; rest = s + slen; }
        size_t flen = strlen(flats[i]);
        if (strncasecmp(s, flats[i], flen) == 0 && flen >= (size_t)(note_class >= 0 ? strlen(sharps[note_class]) : 0))
            { note_class = i; rest = s + flen; }
    }
    if (note_class < 0) return -1;
    int octave = (*rest >= '0' && *rest <= '9') ? *rest - '0' : 3;
    int midi = note_class + (octave + 1) * 12;
    return (midi >= 0 && midi <= 127) ? midi : -1;
}

static void load_scale_conf(void)
{
    char dir[256], path[512];
    addin_dir(dir, sizeof dir);
    if (!dir[0]) return;
    snprintf(path, sizeof path, "%s/mpc_enable_scalemode.conf", dir);
    FILE *f = fopen(path, "re");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || line[0] == '#') continue;
        *eq++ = 0;
        char *val = eq;
        val[strcspn(val, "\n\r")] = 0;
        if (!strcmp(line, "scale")) {
            for (int i = 0; i < NUM_SCALES; i++) {
                if (strcasecmp(val, scale_names[i]) == 0) { g_scale_type = i; break; }
            }
        } else if (!strcmp(line, "root")) {
            int midi = parse_note_name(val);
            if (midi < 0) midi = atoi(val);
            if (midi >= 0 && midi <= 127) g_root_note = midi;
        } else if (!strcmp(line, "octave_offset")) {
            int oct = atoi(val);
            if (oct >= -4 && oct <= 4) g_octave_offset = oct;
        } else if (!strcmp(line, "pad_root")) {
            int midi = parse_note_name(val);
            if (midi < 0) midi = atoi(val);
            if (midi >= 0 && midi <= 127) g_pad_root = midi;
        }
    }
    fclose(f);
    LOG("conf: scale=%s root=%d pad_root=%d octave_offset=%d",
        scale_names[g_scale_type], g_root_note, g_pad_root, g_octave_offset);
}

static void save_conf(void)
{
    char dir[256], path[512], tmp[520];
    addin_dir(dir, sizeof dir);
    if (!dir[0]) return;
    snprintf(path, sizeof path, "%s/mpc_enable_scalemode.conf", dir);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "we");
    if (!f) return;
    fprintf(f, "enabled=1\nscale=%s\nroot=%d\noctave_offset=%d\n",
            scale_names[g_scale_type], g_root_note, g_octave_offset);
    if (g_pad_root != 60)
        fprintf(f, "pad_root=%d\n", g_pad_root);
    fclose(f);
    rename(tmp, path);
}

static int scale_map_hook(void *ctx, uint64_t pad_val, void *pad_ctx)
{
    int mode = *(volatile int *)((uintptr_t)ctx + 0x88);
    int hits = __sync_add_and_fetch(&g_hook_hits, 1);
    if (hits <= 16 || (hits & 0xFF) == 0)
        LOG("hook[%d]: mode=%d pad_val=0x%lx ctx=%p pad_ctx=%p",
            hits, mode, (unsigned long)pad_val, ctx, pad_ctx);

    if (mode != 1) return 0;

    int pad = (int)(pad_val & 0xF);
    int sc  = g_scale_type;
    if (sc < 0 || sc >= NUM_SCALES) sc = 0;

    int npo      = scale_npo[sc];
    int octave   = pad / npo;
    int degree   = pad % npo;
    int interval = scale_intervals[sc][degree];

    int root_off = (g_root_note % 12) - (g_pad_root % 12);
    if (root_off > 5)  root_off -= 12;
    if (root_off < -6) root_off += 12;

    int note = interval + (octave + g_octave_offset) * 12 + root_off - 24;
    if (note < -120) note = -120;
    if (note > 120)  note = 120;

    if (hits <= 16)
        LOG("hook[%d]: pad=%d scale=%s offset=%d (root_off=%d interval=%d oct=%d)",
            hits, pad, scale_names[sc], note, root_off, interval, octave);

    *(volatile uint8_t *)((uintptr_t)pad_ctx + 0x31) = 0;

    if (g_note_setter)
        g_note_setter((void *)((uintptr_t)pad_ctx + 0x14), 0, 0, note);

    return 1;
}

static void *hook_alloc_near(uintptr_t target)
{
    long pgsz = sysconf(_SC_PAGESIZE);
    uintptr_t base = target & ~(uintptr_t)(pgsz - 1);

    for (uintptr_t off = 0x200000; off < 0x7000000; off += (uintptr_t)pgsz) {
        void *p = mmap((void *)(base - off), pgsz, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) continue;
        if (p == (void *)(base - off)) return p;
        munmap(p, pgsz);
    }
    for (uintptr_t off = 0x200000; off < 0x7000000; off += (uintptr_t)pgsz) {
        void *p = mmap((void *)(base + off), pgsz, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p == MAP_FAILED) continue;
        if (p == (void *)(base + off)) return p;
        munmap(p, pgsz);
    }
    return NULL;
}

static int install_paciasp_hook(uintptr_t target, void *callback, const char *name)
{
    uint32_t first = *(volatile uint32_t *)target;
    if (first != 0xd503233f) {
        LOG("%s: target %p not PACIASP (0x%08x)", name, (void *)target, first);
        return -1;
    }

    void *tramp = hook_alloc_near(target);
    if (!tramp) {
        LOG("%s: alloc_near failed", name);
        return -1;
    }

    uint64_t *t = (uint64_t *)tramp;
    t[0]  = 0xa90007e0d10143ffULL; /* SUB SP,#0x50; STP X0,X1,[SP]       */
    t[1]  = 0xa90217e4a9010fe2ULL; /* STP X2,X3,[SP,#0x10]; STP X4,X5    */
    t[2]  = 0xa9047be8a9031fe6ULL; /* STP X6,X7,[SP,#0x30]; STP X8,X30   */
    t[3]  = 0xd63f0200580001d0ULL; /* LDR X16,[PC,#0x38]; BLR X16        */
    t[4]  = 0xa94007e02a0003f1ULL; /* MOV W17,W0; LDP X0,X1,[SP]         */
    t[5]  = 0xa94217e4a9410fe2ULL; /* LDP X2,X3; LDP X4,X5               */
    t[6]  = 0xa9447be8a9431fe6ULL; /* LDP X6,X7; LDP X8,X30              */
    t[7]  = 0x35000071910143ffULL; /* ADD SP,#0x50; CBNZ W17,+12         */
    *(uint32_t *)((char *)tramp + 0x40) = 0xd503233f; /* PACIASP (orig)  */

    int64_t back = ((int64_t)(target + 4) - (int64_t)((uintptr_t)tramp + 0x44)) / 4;
    *(uint32_t *)((char *)tramp + 0x44) = 0x14000000 | ((uint32_t)back & 0x3FFFFFF);

    t[9]  = 0xd503201fd65f03c0ULL; /* RET; NOP                           */
    t[10] = (uint64_t)(uintptr_t)callback;

    long pgsz = sysconf(_SC_PAGESIZE);
    if (mprotect((void *)((uintptr_t)tramp & ~(uintptr_t)(pgsz - 1)),
                 pgsz, PROT_READ | PROT_EXEC) != 0) {
        LOG("%s: mprotect trampoline failed", name);
        munmap(tramp, pgsz);
        return -1;
    }
    __builtin___clear_cache(tramp, (char *)tramp + 0x58);

    int64_t fwd = ((int64_t)(uintptr_t)tramp - (int64_t)target) / 4;
    if (fwd < -0x2000000 || fwd > 0x1FFFFFF) {
        LOG("%s: trampoline out of B range (%ld)", name, (long)fwd);
        return -1;
    }

    uint32_t b_insn = 0x14000000 | ((uint32_t)fwd & 0x3FFFFFF);
    if (patch_insn(target, 0xd503233f, b_insn) != 0) {
        LOG("%s: patch target failed", name);
        return -1;
    }

    LOG("%s: hook installed target=%p trampoline=%p", name, (void *)target, tramp);
    return 0;
}

static int install_scale_hook(struct mpc_mapping *m)
{
    uintptr_t note_map = m->base + NOTE_MAP_OFFSET;
    uintptr_t note_set = m->base + NOTE_SETTER_OFFSET;

    uint32_t nm_first = insn_at(note_map);
    if (nm_first != 0xd503233f) {
        LOG("note-map at %p not PACIASP (0x%08x) — skipping hook",
            (void *)note_map, nm_first);
        return -1;
    }

    uint32_t ns_first = insn_at(note_set);
    if (ns_first != 0xd503233f) {
        LOG("note-setter at %p not PACIASP (0x%08x) — skipping hook",
            (void *)note_set, ns_first);
        return -1;
    }
    g_note_setter = (note_setter_fn_t)note_set;
    LOG("note-setter at %p", (void *)note_set);

    return install_paciasp_hook(note_map, (void *)scale_map_hook, "scale_map");
}

/* ── pad-mode cycling trigger ── */

/*
 * The cycling function is a C++ method: the first argument (X0) is the
 * `this` pointer.  To call it we need to find that pointer at runtime.
 *
 * Strategy: find a BL to the cycling function, then walk backward to see
 * where X0 was loaded.  The typical pattern is:
 *   ADRP Xn, <global-page>
 *   LDR  X0, [Xn, #off]          ; load this from a global
 *   ... (maybe a few instructions)
 *   BL   cycle_func
 *
 * We scan for all BLs to the cycling function, then for each one look
 * backward for an LDR X0,[Xn,#off] preceded by an ADRP that resolves to
 * a valid data address.
 */

static uintptr_t g_cycle_func;
static uintptr_t g_set_mode_func; /* set_mode(inner+0x18, mode) called by the cycling function */
static uintptr_t g_this_global; /* address of the global that holds `this` */
static void *g_this_direct;     /* direct this pointer (vtable scan) */
static pthread_t g_main_thread; /* main thread ID for signal-based dispatch */
static volatile int g_pending_setmode = -1; /* mode to set from main thread, -1 = idle */
static volatile int g_setmode_result = -99; /* result written by signal handler */
static volatile int g_direct_target = -1;   /* >= 0: call set_mode directly instead of cycle */

static void *resolve_this_ptr(void)
{
    if (g_this_direct) return g_this_direct;
    if (g_this_global) return *(void *volatile *)g_this_global;
    return NULL;
}

typedef void (*set_mode_fn_t)(void *obj, int mode);
typedef void (*cycle_fn_t)(void *self);

/*
 * poll() interposition: the MPC event loop calls poll() on every iteration.
 * We hook it to run the cycling function on the main thread during normal
 * execution (not from a signal handler), which lets the full notification
 * chain work safely.
 */
#include <dlfcn.h>
#include <poll.h>

static int (*real_poll)(struct pollfd *, nfds_t, int);
static volatile sig_atomic_t s_in_poll_cycle;

static sigjmp_buf s_poll_jmpbuf;

static void poll_segv_handler(int sig, siginfo_t *si, void *ctx)
{
    if (s_in_poll_cycle) {
        uintptr_t fault_addr = si ? (uintptr_t)si->si_addr : 0;
        uintptr_t pc = 0;
#ifdef __aarch64__
        if (ctx) {
            ucontext_t *uc = (ucontext_t *)ctx;
            pc = uc->uc_mcontext.pc;
        }
#endif
        LOG("SEGV: fault_addr=%p pc=%p", (void *)fault_addr, (void *)pc);
        siglongjmp(s_poll_jmpbuf, sig);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

__attribute__((visibility("default")))
int poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    if (!real_poll) real_poll = dlsym(RTLD_NEXT, "poll");

    if (!s_in_poll_cycle &&
        g_pending_setmode >= 0 &&
        pthread_self() == g_main_thread &&
        g_cycle_func && resolve_this_ptr())
    {
        void *volatile self = resolve_this_ptr();

        struct sigaction sa = {0}, old_segv;
        sa.sa_sigaction = poll_segv_handler;
        sa.sa_flags = SA_SIGINFO;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, &old_segv);

        s_in_poll_cycle = 1;
        int direct = g_direct_target;
        int crashed = sigsetjmp(s_poll_jmpbuf, 1);
        if (!crashed) {
            if (direct >= 0 && g_set_mode_func) {
                uintptr_t inner = *(volatile uintptr_t *)((uintptr_t)self + 0x50);
                void *obj = (void *)(inner + 0x18);
                LOG("calling set_mode(%p, %d) directly", obj, direct);
                ((set_mode_fn_t)g_set_mode_func)(obj, direct);
            } else {
                ((cycle_fn_t)g_cycle_func)(self);
            }
        } else {
            LOG("%s crashed in poll hook (signal %d)",
                direct >= 0 ? "set_mode" : "cycle", crashed);
        }
        s_in_poll_cycle = 0;
        g_direct_target = -1;

        sigaction(SIGSEGV, &old_segv, NULL);

        uintptr_t inner = *(volatile uintptr_t *)((uintptr_t)self + 0x50);
        int mode = inner ? *(volatile int32_t *)(inner + 0x34) : -1;
        g_setmode_result = (crashed ? 100 + mode : mode);
        g_pending_setmode = -1;
    }

    return real_poll(fds, nfds, timeout);
}

static int find_this_pointer(struct mpc_mapping *m, uintptr_t cycle_func)
{
    int bl_count = 0;

    for (uintptr_t pc = m->text_start; pc < m->text_end; pc += 4) {
        if (decode_bl(pc) != cycle_func) continue;
        bl_count++;

        LOG("  BL to cycle func at %p (offset 0x%lx)",
            (void *)pc, (unsigned long)(pc - m->base));

        /*
         * Walk backward looking for how X0 (this) was set.
         * Pattern 1: LDR X0, [Xn, #off] where Xn came from ADRP (global pointer)
         * Pattern 2: MOV X0, Xm then trace Xm the same way
         */
        unsigned target_reg = 0; /* register we're tracing (start with X0) */
        for (uintptr_t s = pc - 4; s >= m->text_start && s >= pc - 256; s -= 4) {
            uint32_t w = insn_at(s);

            /* MOV Xd, Xm  (ORR Xd, XZR, Xm): 0xAA0003E0 | Rm<<16 | Rd */
            if ((w & 0xFFE0FFE0) == 0xAA0003E0) {
                unsigned rd = w & 0x1F;
                unsigned rm = (w >> 16) & 0x1F;
                if (rd == target_reg) {
                    LOG("    MOV X%u, X%u at %p", rd, rm, (void *)s);
                    target_reg = rm;
                    continue;
                }
            }

            /* LDR Xt, [Xn, #imm12] (64-bit): F940_0000 | imm12<<10 | Rn<<5 | Rt */
            if ((w & 0xFFC00000) == 0xF9400000) {
                unsigned rt = w & 0x1F;
                unsigned rn = (w >> 5) & 0x1F;
                unsigned imm12 = (w >> 10) & 0xFFF;
                uintptr_t offset = (uintptr_t)imm12 << 3;

                if (rt != target_reg) continue;

                LOG("    LDR X%u, [X%u, #0x%lx] at %p",
                    rt, rn, (unsigned long)offset, (void *)s);

                /* Find ADRP Xrn within a few instructions before. */
                for (uintptr_t a = s - 4; a >= m->text_start && a >= s - 48; a -= 4) {
                    unsigned adrp_rd;
                    uintptr_t page = decode_adrp(a, &adrp_rd);
                    if (!page || adrp_rd != rn) continue;

                    uintptr_t global = page + offset;
                    LOG("    ADRP X%u → page %p, global = %p",
                        rn, (void *)page, (void *)global);

                    /* The global could be in .data/.bss (rw), not just .rodata */
                    if (global < m->base) continue;

                    void *ptr = *(void *volatile *)global;
                    if (!ptr) {
                        LOG("    global is NULL (object not constructed yet?)");
                        continue;
                    }

                    uintptr_t this_addr = (uintptr_t)ptr;
                    int32_t mode = *(volatile int32_t *)(this_addr + 0x34);

                    LOG("    *global = %p, *(this+0x34) = %d", ptr, mode);

                    if (mode < 0 || mode > 2) continue;

                    LOG("  this global at %p → %p (mode=%d)",
                        (void *)global, ptr, mode);
                    g_this_global = global;
                    return 0;
                }

                /* If ADRP wasn't found, the base register might come from
                 * further back — continue tracing it. */
                target_reg = rn;
            }
        }
    }

    if (bl_count == 0)
        LOG("  no BL to cycle func — trying vtable scan");
    else {
        LOG("  found %d BL(s) but could not resolve this pointer — trying vtable scan",
            bl_count);
    }

    /*
     * Pointer-scan approach: the cycling function's address must appear
     * somewhere in readable memory — a vtable, a std::function, a JUCE
     * listener list, or a heap-allocated callback.  Scan ALL readable
     * non-executable segments (not just MPC binary pages) for the raw
     * function pointer.
     *
     * Re-parse /proc/self/maps so we only touch mapped pages.
     */
    uintptr_t fptr_loc = 0;

    FILE *mf = fopen("/proc/self/maps", "re");
    if (mf) {
        char line[512];
        while (fgets(line, sizeof line, mf) && !fptr_loc) {
            unsigned long a, b;
            char perms[8] = {0};
            if (sscanf(line, "%lx-%lx %4s", &a, &b, perms) < 3) continue;
            if (perms[0] != 'r' || perms[2] == 'x') continue;
            if ((b - a) > (256UL << 20)) continue; /* cap at 256 MB */

            for (uintptr_t p = (uintptr_t)a; p < (uintptr_t)b - 8; p += 8) {
                if (*(volatile uintptr_t *)p == cycle_func) {
                    fptr_loc = p;
                    LOG("  cycle func pointer at %p (segment %lx-%lx)",
                        (void *)p, a, b);
                    break;
                }
            }
        }
        fclose(mf);
    }

    if (!fptr_loc) {
        LOG("  cycle func address not found in any readable segment");
        return -1;
    }

    /* Dump context around the function pointer for data-structure analysis. */
    LOG("  context around fptr at %p:", (void *)fptr_loc);
    for (int off = -32; off <= 64; off += 8) {
        uintptr_t addr = fptr_loc + off;
        uintptr_t val = *(volatile uintptr_t *)addr;
        const char *tag = "";
        if (val >= m->text_start && val < m->text_end) tag = " [.text]";
        else if (val >= m->base && val < m->ro_end) tag = " [MPC]";
        LOG("    [%+d] %p = %p%s", off, (void *)addr, (void *)val, tag);
    }

    /*
     * The function pointer is stored in a callback registration node,
     * not a C++ vtable.  The `this` pointer is likely stored NEAR the
     * function pointer in the same structure (typical for std::function,
     * JUCE listeners, signal/slot, etc.).
     *
     * Strategy: check each pointer stored near fptr_loc (offsets -64
     * to +64) to see if it leads to a valid pad-mode controller object
     * (one whose *(ptr+0x50) is readable and *(*(ptr+0x50)+0x34) is 0–2).
     */

    /* Build range table with permissions for safe pointer validation. */
    mf = fopen("/proc/self/maps", "re");
    if (!mf) return -1;

    #define MAX_RANGES 512
    struct { uintptr_t lo, hi; int writable; } ranges[MAX_RANGES];
    int nr = 0;
    {
        char line[512];
        while (fgets(line, sizeof line, mf) && nr < MAX_RANGES) {
            unsigned long a, b;
            char perms[8] = {0};
            if (sscanf(line, "%lx-%lx %4s", &a, &b, perms) < 3) continue;
            if (perms[0] == 'r')
                ranges[nr++] = (typeof(ranges[0])){ (uintptr_t)a, (uintptr_t)b,
                                                     perms[1] == 'w' };
        }
    }
    fclose(mf);

    #define IS_READABLE(addr, sz) ({ \
        int _ok = 0; uintptr_t _a = (uintptr_t)(addr); \
        for (int _i = 0; _i < nr; _i++) \
            if (_a >= ranges[_i].lo && _a + (sz) <= ranges[_i].hi) { _ok = 1; break; } \
        _ok; })

    #define IS_WRITABLE(addr, sz) ({ \
        int _ok = 0; uintptr_t _a = (uintptr_t)(addr); \
        for (int _i = 0; _i < nr; _i++) \
            if (ranges[_i].writable && \
                _a >= ranges[_i].lo && _a + (sz) <= ranges[_i].hi) { _ok = 1; break; } \
        _ok; })

    /* Check pointers stored near the function pointer location. */
    for (int off = -64; off <= 64; off += 8) {
        if (off == 0) continue; /* skip the function pointer itself */
        uintptr_t neighbor = fptr_loc + off;
        if (!IS_READABLE(neighbor, 8)) continue;

        uintptr_t candidate = *(volatile uintptr_t *)neighbor;
        if (!candidate || !IS_READABLE(candidate, 0x58)) continue;

        /* Check the inner-object indirection: *(candidate+0x50) → inner.
         * Both candidate and inner must be in WRITABLE pages (heap/bss).
         * Extra checks, from the firmware's code:
         *   this+0x48 → different valid writable pointer
         *   inner+0x34 → mode 0-2
         *   inner+0x18 → valid writable pointer (set_mode target)
         *   this+0x68 → valid readable pointer (notification target) */
        if (!IS_WRITABLE(candidate, 0x70)) continue;

        uintptr_t inner = *(volatile uintptr_t *)(candidate + 0x50);
        if (!inner || !IS_WRITABLE(inner, 0x2A4)) continue;

        uintptr_t other = *(volatile uintptr_t *)(candidate + 0x48);
        if (!other || other == inner || !IS_WRITABLE(other, 8)) continue;

        int32_t mode_val = *(volatile int32_t *)(inner + 0x34);
        if (mode_val < 0 || mode_val > 2) continue;

        uintptr_t set_tgt = *(volatile uintptr_t *)(inner + 0x18);
        if (!set_tgt || !IS_WRITABLE(set_tgt, 8)) continue;

        uintptr_t notify_ptr = *(volatile uintptr_t *)(candidate + 0x68);
        if (!notify_ptr || !IS_READABLE(notify_ptr, 8)) continue;

        LOG("  neighbor [%+d] = %p → inner=%p mode=%d (rw-validated)",
            off, (void *)candidate, (void *)inner, mode_val);
        g_this_direct = (void *)candidate;
        return 0;
    }

    /* Fallback: scan rw segments via /proc/self/mem in large blocks
     * (safe reads — returns errors instead of SIGSEGV on unmapped pages). */
    LOG("  no neighbor match — scanning rw segments via /proc/self/mem");

    int memfd = open("/proc/self/mem", O_RDONLY);
    if (memfd < 0) {
        LOG("  cannot open /proc/self/mem");
        goto out;
    }

    mf = fopen("/proc/self/maps", "re");
    if (mf) {
        char line[512];
        int candidates = 0;
        uintptr_t found_this[8];
        uintptr_t found_inner[8];
        int32_t found_mode[8];
        /* 64KB block buffer — read large chunks, scan locally */
        enum { BLK = 65536 };
        uintptr_t *blk = malloc(BLK);
        if (!blk) { fclose(mf); close(memfd); goto out; }

        while (fgets(line, sizeof line, mf)) {
            unsigned long a, b;
            char perms[8] = {0};
            if (sscanf(line, "%lx-%lx %4s", &a, &b, perms) < 3) continue;
            if (perms[0] != 'r' || perms[1] != 'w') continue;
            if ((b - a) > (256UL << 20)) continue;
            if (strstr(line, "[stack") || strstr(line, ".so")) continue;

            for (uintptr_t base = (uintptr_t)a; base < (uintptr_t)b; base += BLK) {
                size_t chunk = (uintptr_t)b - base;
                if (chunk > BLK) chunk = BLK;
                ssize_t got = pread(memfd, blk, chunk, base);
                if (got < (ssize_t)(0x70)) continue;
                size_t words = got / 8;

                for (size_t i = 0; i + 0x68/8 < words; i++) {
                    uintptr_t inner = blk[i + 0x50/8];
                    if (!inner || !IS_WRITABLE(inner, 0x2A4)) continue;

                    /* this+0x48 must be a different valid writable pointer */
                    uintptr_t other = blk[i + 0x48/8];
                    if (!other || other == inner || !IS_WRITABLE(other, 8)) continue;

                    int32_t mode_val;
                    if (pread(memfd, &mode_val, 4, inner + 0x34) != 4) continue;
                    if (mode_val < 0 || mode_val > 2) continue;

                    /* inner+0x18 must be a valid writable pointer (set_mode target) */
                    uintptr_t set_tgt;
                    if (pread(memfd, &set_tgt, 8, inner + 0x18) != 8) continue;
                    if (!set_tgt || !IS_WRITABLE(set_tgt, 8)) continue;

                    uintptr_t notify = blk[i + 0x68/8];
                    if (!notify || !IS_READABLE(notify, 8)) continue;

                    uintptr_t p = base + i * 8;
                    if (candidates < 8) {
                        found_this[candidates] = p;
                        found_inner[candidates] = inner;
                        found_mode[candidates] = mode_val;
                    }
                    candidates++;
                    if (candidates <= 8)
                        LOG("  rw-scan candidate this=%p inner=%p mode=%d "
                            "other=%p set_tgt=%p notify=%p",
                            (void *)p, (void *)inner, mode_val,
                            (void *)other, (void *)set_tgt, (void *)notify);
                }
            }
        }
        free(blk);
        fclose(mf);
        close(memfd);
        memfd = -1;
        LOG("  rw-scan found %d candidates", candidates);

        if (candidates == 1) {
            LOG("  rw-scan resolved this=%p inner=%p",
                (void *)found_this[0], (void *)found_inner[0]);
            g_this_direct = (void *)found_this[0];
            return 0;
        }

        /* If exactly one candidate has mode != 0, that's likely the real
         * controller (other objects have 0 by coincidence).  Especially
         * after the PadPerform bool patch, the controller may init to mode 1. */
        int nonzero_count = 0;
        int nonzero_idx = -1;
        int top = candidates < 8 ? candidates : 8;
        for (int i = 0; i < top; i++) {
            if (found_mode[i] != 0) { nonzero_count++; nonzero_idx = i; }
        }
        if (nonzero_count == 1 && nonzero_idx >= 0) {
            LOG("  rw-scan: unique non-zero mode candidate this=%p inner=%p mode=%d",
                (void *)found_this[nonzero_idx], (void *)found_inner[nonzero_idx],
                found_mode[nonzero_idx]);
            g_this_direct = (void *)found_this[nonzero_idx];
            return 0;
        }
        goto out;
    }
    if (memfd >= 0) close(memfd);

out:

    LOG("  could not find this pointer");
    #undef IS_READABLE
    #undef IS_WRITABLE
    #undef MAX_RANGES
    return -1;
}

#define SOCK_PATH "/tmp/padmode.sock"

static sigjmp_buf s_call_jmpbuf;
static volatile sig_atomic_t s_in_unsafe_call;

static void unsafe_call_segv(int sig, siginfo_t *si, void *ctx)
{
    (void)ctx;
    if (s_in_unsafe_call) {
        LOG("SEGV in call — si_addr=%p", si ? si->si_addr : NULL);
        siglongjmp(s_call_jmpbuf, sig);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static int read_mode(void *self)
{
    uintptr_t inner = *(volatile uintptr_t *)((uintptr_t)self + 0x50);
    return inner ? *(volatile int32_t *)(inner + 0x34) : -1;
}

static const char *mode_name(int m)
{
    static const char *names[] = {"Full Level", "Pad Perform", "16 Level"};
    return (m >= 0 && m <= 2) ? names[m] : "?";
}

static void find_set_mode_func(void)
{
    if (!g_cycle_func || g_set_mode_func) return;
    /* The cycling function's mode==0 handler at +0x78 is:
     *   ADD X0, X20, #24      ; inner+0x18
     *   MOV W1, #2
     *   STR X21, [SP, #64]
     *   BL  set_mode           ; at +0x84
     * Decode the BL at cycle_func + 0x84. */
    uint32_t bl = *(volatile uint32_t *)(g_cycle_func + 0x84);
    if ((bl >> 26) != 0x25) {
        LOG("  expected BL at cycle+0x84, got %08x", bl);
        return;
    }
    int32_t imm = (int32_t)(bl << 6) >> 4;
    g_set_mode_func = (g_cycle_func + 0x84) + imm;
    LOG("  set_mode at %p (decoded from cycle+0x84)", (void *)g_set_mode_func);
}

static void *socket_worker(void *arg)
{
    (void)arg;
    /* Unmask synchronous fault signals so our handler can recover */
    sigset_t mask;
    sigfillset(&mask);
    sigdelset(&mask, SIGSEGV);
    sigdelset(&mask, SIGBUS);
    pthread_sigmask(SIG_SETMASK, &mask, NULL);

    struct sigaction sa = {0}, old_segv, old_bus;
    sa.sa_sigaction = unsafe_call_segv;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGBUS,  &sa, &old_bus);

    find_set_mode_func();

    unlink(SOCK_PATH);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { LOG("socket failed: %s", strerror(errno)); return NULL; }

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", SOCK_PATH);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        LOG("bind %s failed: %s", SOCK_PATH, strerror(errno));
        close(fd);
        return NULL;
    }
    listen(fd, 2);
    LOG("listening on %s", SOCK_PATH);

    for (;;) {
        int volatile c = accept(fd, NULL, NULL);
        if (c < 0) continue;

        char buf[64] = {0};
        ssize_t n = read(c, buf, sizeof buf - 1);
        buf[n > 0 ? n : 0] = 0;
        char *nl = strchr(buf, '\n');
        if (nl) *nl = 0;

        if (!strcmp(buf, "cycle") || !strncmp(buf, "setmode ", 8)) {
            int is_cycle = !strcmp(buf, "cycle");
            void *self = resolve_this_ptr();
            if (!self || !g_set_mode_func || !g_main_thread) {
                const char *msg = "not ready\n";
                write(c, msg, strlen(msg));
            } else {
                int cur = read_mode(self);
                int target;
                if (is_cycle) {
                    static const int next_mode[] = { 2, 0, 1 };
                    target = (cur >= 0 && cur <= 2) ? next_mode[cur] : 0;
                } else {
                    target = atoi(buf + 8);
                    if (target < 0 || target > 2) {
                        const char *msg = "mode must be 0-2\n";
                        write(c, msg, strlen(msg));
                        close(c);
                        continue;
                    }
                }

                g_setmode_result = -99;
                g_direct_target = target;
                g_pending_setmode = 0;
                __sync_synchronize();

                for (int w = 0; w < 100 && g_setmode_result == -99; w++)
                    usleep(10000);

                int mode = read_mode(self);
                char reply[128];
                int rlen;
                if (g_setmode_result == -99) {
                    rlen = snprintf(reply, sizeof reply,
                        "timeout — mode=%d (%s)\n", mode, mode_name(mode));
                } else if (g_setmode_result >= 100) {
                    rlen = snprintf(reply, sizeof reply,
                        "crashed, mode=%d (%s)\n", mode, mode_name(mode));
                } else {
                    rlen = snprintf(reply, sizeof reply,
                        "mode=%d (%s)\n", mode, mode_name(mode));
                }
                LOG("%s %d→%d (%s)%s",
                    is_cycle ? "cycle" : "setmode",
                    cur, mode, mode_name(mode),
                    g_setmode_result >= 100 ? " [crashed]" : "");
                write(c, reply, rlen);
            }

        } else if (!strncmp(buf, "directmode ", 11)) {
            int target = atoi(buf + 11);
            if (target < 0 || target > 2) {
                const char *msg = "mode must be 0-2\n";
                write(c, msg, strlen(msg));
            } else if (!resolve_this_ptr() || !g_set_mode_func || !g_main_thread) {
                char reply[128];
                int rlen = snprintf(reply, sizeof reply,
                    "not ready: self=%p set_mode=%p main_tid=%lu\n",
                    resolve_this_ptr(), (void *)g_set_mode_func,
                    (unsigned long)g_main_thread);
                write(c, reply, rlen);
            } else {
                void *self = resolve_this_ptr();
                int cur = read_mode(self);
                g_setmode_result = -99;
                g_direct_target = target;
                g_pending_setmode = 0;
                __sync_synchronize();

                for (int w = 0; w < 100 && g_setmode_result == -99; w++)
                    usleep(10000);

                int mode = read_mode(self);
                char reply[128];
                int rlen;
                if (g_setmode_result == -99) {
                    rlen = snprintf(reply, sizeof reply,
                        "timeout — poll hook not firing? mode=%d (%s)\n",
                        mode, mode_name(mode));
                } else if (g_setmode_result >= 100) {
                    rlen = snprintf(reply, sizeof reply,
                        "set_mode crashed, mode=%d (%s) [was %d]\n",
                        mode, mode_name(mode), cur);
                } else {
                    rlen = snprintf(reply, sizeof reply,
                        "mode=%d (%s)\n", mode, mode_name(mode));
                }
                LOG("directmode %d: %d→%d (%s)%s", target, cur, mode,
                    mode_name(mode),
                    g_setmode_result >= 100 ? " [crashed]" : "");
                write(c, reply, rlen);
            }

        } else if (!strncmp(buf, "writemode ", 10)) {
            int target = atoi(buf + 10);
            if (target < 0 || target > 2) {
                const char *msg = "mode must be 0-2\n";
                write(c, msg, strlen(msg));
            } else {
                void *self = resolve_this_ptr();
                if (!self) {
                    const char *msg = "no this pointer\n";
                    write(c, msg, strlen(msg));
                } else {
                    uintptr_t inner = *(volatile uintptr_t *)((uintptr_t)self + 0x50);
                    if (!inner) {
                        const char *msg = "inner pointer is null\n";
                        write(c, msg, strlen(msg));
                    } else {
                        volatile int32_t *mp = (volatile int32_t *)(inner + 0x34);
                        s_in_unsafe_call = 1;
                        int crashed = sigsetjmp(s_call_jmpbuf, 1);
                        if (!crashed) {
                            int old = *mp;
                            *mp = target;
                            int now = *mp;
                            s_in_unsafe_call = 0;
                            char reply[96];
                            int rlen = snprintf(reply, sizeof reply,
                                "wrote mode %d→%d (read back %d) at %p\n",
                                old, target, now, (void *)mp);
                            write(c, reply, rlen);
                            LOG("writemode %d→%d at %p", old, target, (void *)mp);
                        } else {
                            s_in_unsafe_call = 0;
                            char reply[96];
                            int rlen = snprintf(reply, sizeof reply,
                                "CRASHED writing to %p (signal %d)\n",
                                (void *)mp, crashed);
                            write(c, reply, rlen);
                            LOG("writemode crashed at %p", (void *)mp);
                        }
                    }
                }
            }

        } else if (!strcmp(buf, "perms")) {
            void *self = resolve_this_ptr();
            char reply[512];
            int rlen = 0;
            if (!self) {
                rlen = snprintf(reply, sizeof reply, "no this pointer\n");
            } else {
                uintptr_t inner = *(volatile uintptr_t *)((uintptr_t)self + 0x50);
                uintptr_t mode_addr = inner + 0x34;
                FILE *maps = fopen("/proc/self/maps", "r");
                if (!maps) {
                    rlen = snprintf(reply, sizeof reply, "can't read maps\n");
                } else {
                    char line[256];
                    int found = 0;
                    while (fgets(line, sizeof line, maps)) {
                        unsigned long lo, hi;
                        char perms[8];
                        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perms) == 3) {
                            if (mode_addr >= lo && mode_addr < hi) {
                                rlen = snprintf(reply, sizeof reply,
                                    "mode@%p in %lx-%lx %s\nthis@%p inner@%p\n",
                                    (void *)mode_addr, lo, hi, perms,
                                    self, (void *)inner);
                                found = 1;
                                break;
                            }
                        }
                    }
                    fclose(maps);
                    if (!found)
                        rlen = snprintf(reply, sizeof reply,
                            "mode@%p not found in maps!\n", (void *)mode_addr);
                }
            }
            write(c, reply, rlen);

        } else if (!strncmp(buf, "scale ", 6)) {
            const char *name = buf + 6;
            int found = -1;
            for (int i = 0; i < NUM_SCALES; i++) {
                if (strcasecmp(name, scale_names[i]) == 0) { found = i; break; }
            }
            if (found < 0) {
                /* try prefix match */
                size_t len = strlen(name);
                for (int i = 0; i < NUM_SCALES; i++) {
                    if (strncasecmp(name, scale_names[i], len) == 0) { found = i; break; }
                }
            }
            char reply[128];
            int rlen;
            if (found >= 0) {
                g_scale_type = found;
                save_conf();
                rlen = snprintf(reply, sizeof reply, "scale=%s (%d)\n",
                                scale_names[found], found);
            } else {
                rlen = snprintf(reply, sizeof reply, "unknown scale '%s'\n", name);
            }
            write(c, reply, rlen);

        } else if (!strncmp(buf, "root ", 5)) {
            char reply[128];
            int rlen;
            int midi = parse_note_name(buf + 5);
            if (midi < 0) midi = atoi(buf + 5);
            if (midi >= 0 && midi <= 127) {
                g_root_note = midi;
                save_conf();
                rlen = snprintf(reply, sizeof reply, "root=MIDI %d\n", midi);
            } else {
                rlen = snprintf(reply, sizeof reply,
                    "usage: root <note>[octave] (e.g. C3, A#2) or root <midi_num>\n");
            }
            write(c, reply, rlen);

        } else if (!strncmp(buf, "octave ", 7)) {
            int oct = atoi(buf + 7);
            if (oct < -4) oct = -4;
            if (oct > 4)  oct = 4;
            g_octave_offset = oct;
            save_conf();
            char reply[64];
            int rlen = snprintf(reply, sizeof reply, "octave_offset=%d\n", oct);
            write(c, reply, rlen);

        } else if (!strncmp(buf, "testmap ", 8) || !strcmp(buf, "testmap")) {
            int pad = (!strncmp(buf, "testmap ", 8)) ? atoi(buf + 8) : 0;
            if (pad < 0) pad = 0;
            if (pad > 15) pad = 15;
            int sc = g_scale_type;
            if (sc < 0 || sc >= NUM_SCALES) sc = 0;
            int npo = scale_npo[sc];
            int octave = pad / npo;
            int degree = pad % npo;
            int interval = scale_intervals[sc][degree];
            int ro = (g_root_note % 12) - (g_pad_root % 12);
            if (ro > 5)  ro -= 12;
            if (ro < -6) ro += 12;
            int note = interval + (octave + g_octave_offset) * 12 + ro - 24;
            if (note < -120) note = -120;
            if (note > 120)  note = 120;
            char reply[256];
            int rlen = snprintf(reply, sizeof reply,
                "pad=%d scale=%s degree=%d interval=%d octave=%d root_off=%d -> offset %d\n",
                pad, scale_names[sc], degree, interval, octave, ro, note);
            write(c, reply, rlen);

        } else if (!strcmp(buf, "hookhits")) {
            char reply[128];
            int rlen = snprintf(reply, sizeof reply, "hook_hits=%d\n", g_hook_hits);
            write(c, reply, rlen);

        } else if (!strcmp(buf, "scaleinfo")) {
            char reply[256];
            int rlen = snprintf(reply, sizeof reply,
                "scale=%s (%d) root=%d octave_offset=%d note_setter=%p\n",
                (g_scale_type >= 0 && g_scale_type < NUM_SCALES)
                    ? scale_names[g_scale_type] : "?",
                g_scale_type, g_root_note, g_octave_offset,
                (void *)g_note_setter);
            write(c, reply, rlen);

        } else if (!strcmp(buf, "status")) {
            void *self = resolve_this_ptr();
            char reply[256];
            int rlen;
            if (self) {
                uintptr_t inner = *(volatile uintptr_t *)((uintptr_t)self + 0x50);
                int mode = inner ? *(volatile int32_t *)(inner + 0x34) : -1;
                rlen = snprintf(reply, sizeof reply,
                    "func=%p set_mode=%p this=%p inner=%p mode=%d (%s) "
                    "scale=%s root=%d oct=%d setter=%p main_tid=%lu\n",
                    (void *)g_cycle_func, (void *)g_set_mode_func,
                    self, (void *)inner, mode, mode_name(mode),
                    (g_scale_type >= 0 && g_scale_type < NUM_SCALES)
                        ? scale_names[g_scale_type] : "?",
                    g_root_note, g_octave_offset,
                    (void *)g_note_setter,
                    (unsigned long)g_main_thread);
            } else {
                rlen = snprintf(reply, sizeof reply, "no this pointer\n");
            }
            write(c, reply, rlen);
        }
        close(c);
    }
    return NULL;
}

/* ── entry point ── */

static int is_mpc_process(void)
{
    char exe[256];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return 0;
    exe[n] = 0;
    const char *base = strrchr(exe, '/');
    base = base ? base + 1 : exe;
    return !strcmp(base, "MPC") || !strcmp(base, "MPC Sample");
}

static void *worker(void *arg)
{
    (void)arg;

    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_SETMASK, &all, NULL);

    if (!getenv("MPC_ENABLE_SCALEMODE_SYNC"))
        usleep(500000);

    struct mpc_mapping m = find_mpc();
    if (!m.base || !m.text_start) {
        LOG("could not locate MPC binary in memory");
        return NULL;
    }
    LOG("MPC: base=%p text=%p-%p ro=%p-%p",
        (void *)m.base, (void *)m.text_start, (void *)m.text_end,
        (void *)m.ro_start, (void *)m.ro_end);

    int rc = try_patch(&m);
    if (rc == 0)
        LOG("patches applied — pad perform should be available");
    else if (rc == 1)
        LOG("no gate found to patch — test if pad perform already works");
    else
        LOG("patch failed (rc=%d)", rc);

    load_scale_conf();

    if (install_scale_hook(&m) == 0)
        LOG("scale-map hook active — pads remap in Pad Perform mode");
    else
        LOG("scale-map hook failed — note remapping unavailable");

    /* Find the cycling function and its this pointer for the socket trigger. */
    uintptr_t cycle = find_cycle_func(&m);
    if (cycle) {
        g_cycle_func = cycle;
        if (find_this_pointer(&m, cycle) != 0) {
            /* The object might not be constructed yet.  Retry at
             * increasing intervals: the MPC takes several seconds to
             * finish initialising its control-surface objects. */
            static const unsigned delays_ms[] = { 5000, 10000, 20000 };
            for (unsigned i = 0; i < sizeof delays_ms / sizeof *delays_ms; i++) {
                if (resolve_this_ptr()) break;
                LOG("retrying this-pointer scan in %u ms", delays_ms[i]);
                usleep(delays_ms[i] * 1000u);
                m = find_mpc();
                if (m.base && m.text_start)
                    find_this_pointer(&m, cycle);
            }
        }
    }

    if (resolve_this_ptr())
        LOG("this pointer resolved — socket cycle command ready");
    else if (g_cycle_func)
        LOG("this pointer NOT found — cycle command unavailable, try MIDI injection");

    /* Start socket listener if we have at least the function address. */
    if (g_cycle_func) {
        pthread_t stid;
        if (pthread_create(&stid, NULL, socket_worker, NULL) == 0)
            pthread_detach(stid);
    }

    return NULL;
}

__attribute__((constructor)) static void addin_init(void)
{
    if (getenv("MPC_ENABLE_SCALEMODE_DISABLE") || !is_mpc_process())
        return;

    load_conf();
    if (!C.enabled) {
        LOG("disabled in config");
        return;
    }

    LOG("starting");

    g_main_thread = pthread_self();

    if (getenv("MPC_ENABLE_SCALEMODE_SYNC")) {
        /* Test mode: run synchronously in the constructor, before main().
         * Useful under qemu-user where the MPC binary aborts on DRM init
         * before a background thread would wake up. */
        LOG("sync mode (test)");
        worker(NULL);
        return;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, worker, NULL) == 0)
        pthread_detach(tid);
    else
        LOG("pthread_create failed: %s", strerror(errno));
}
