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
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define LOG(fmt, ...) fprintf(stderr, "enable-scalemode: " fmt "\n", ##__VA_ARGS__)

/* ── conf ── */

struct conf {
    int enabled;
};

static struct conf C = { .enabled = 1 };

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
     * Step 3: Find the PadPerformSettings "supported" bool.
     *
     * The engine init function constructs PadPerformSettings and passes a
     * bool that controls whether PadPerform is available. This bool is
     * loaded from a capability struct: LDRB Wn, [Xm, #432].
     *
     * The padPerformButton string is referenced by the PadPerformSettings
     * constructor, which is called from the engine init. We find the
     * constructor's BL callers in the engine init, then look backward for
     * the LDRB that loads the bool.
     *
     * If we find a LDRB Wn, [Xm, #432] near a BL to a function that
     * also references "padPerformButton", we can patch it to MOV Wn, #1
     * to force PadPerform enabled.
     */

    /* Find the function containing the padPerformButton reference. This is
     * the PadPerformSettings constructor (or a close helper). */
    uintptr_t ctor_func = 0;
    for (uintptr_t pc = ppb_ref; pc >= m->text_start && pc > ppb_ref - 8192; pc -= 4) {
        uint32_t w = insn_at(pc);
        if (w == 0xD503233F || /* PACIASP */
            (w & 0xFFE003FF) == 0xA98003FD) { /* STP X29, X30, [SP, #-N]! */
            ctor_func = pc;
            break;
        }
    }
    if (ctor_func)
        LOG("PadPerformSettings ctor at %p", (void *)ctor_func);

    /* Find callers of the constructor in .text — these are in the engine init. */
    if (ctor_func) {
        for (uintptr_t pc = m->text_start; pc < m->text_end; pc += 4) {
            uint32_t w = insn_at(pc);
            if ((w & 0xFC000000) != 0x94000000) continue;
            uintptr_t target = decode_bl(pc);
            if (target != ctor_func) continue;

            LOG("  BL to ctor at %p", (void *)pc);

            /* Search backward from this BL for LDRB Wn, [Xm, #432].
             * The bool is loaded ~16-60 instructions before the BL. */
            for (uintptr_t scan = pc - 4; scan >= pc - 256 && scan >= m->text_start; scan -= 4) {
                uint32_t sw = insn_at(scan);
                /* LDRB Wt, [Xn, #432]: 0x39400000 | (432<<10) = 0x3946C000 */
                if ((sw & 0xFFFFFC00) == 0x3946C000) {
                    unsigned rt = sw & 0x1F;
                    unsigned rn = (sw >> 5) & 0x1F;
                    LOG("  found LDRB W%u, [X%u, #432] at %p (PadPerform supported flag)",
                        rt, rn, (void *)scan);

                    /* Patch: LDRB → MOV Wrt, #1 (0x52800020 | rt) */
                    uint32_t mov_w1 = 0x52800020 | rt;
                    if (patch_insn(scan, sw, mov_w1) == 0) {
                        patches_applied++;
                        LOG("  PadPerform bool forced to true");
                    }
                    goto after_ctor_search;
                }
            }
        }
    }
after_ctor_search:
    ;
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

    pthread_t tid;
    if (pthread_create(&tid, NULL, worker, NULL) == 0)
        pthread_detach(tid);
    else
        LOG("pthread_create failed: %s", strerror(errno));
}
