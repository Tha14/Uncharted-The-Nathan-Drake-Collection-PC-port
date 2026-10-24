/* gow3 24 Oct 2026: co-op and multiplayer research for Uncharted 2 (and later 3).
 *
 * Everything here is off unless an environment knob turns it on (the launcher sets none).
 *
 * GOW3_COOP_CMD=<file>: a live command console for tests. The loader puts a jump at the entry
 *   of the game's per-frame GameLoopUpdate (U2 1.02 image offset 0x3facb0); the stub calls
 *   coop_frame() on the game's own thread, once per frame. coop_frame() re-reads the file when
 *   it changes and runs its lines in order (output: "Coop: ..." lines in boot.log):
 *     r <addr> [n]            hex dump (n bytes, at most 512)
 *     rd <addr> [n]           n dwords;  rq <addr> [n]  n qwords;  rf <addr> [n]  n floats
 *     rs <addr>               a C string
 *     w8|w16|w32|w64 <addr> <value>     wf <addr> <float>     (through WriteProcessMemory)
 *     call <addr> [arg...]    calls a guest function (SysV): integer/pointer args go in
 *                             rdi..r9, args written f:<float> go in xmm0..xmm3; prints rax
 *     mon <name> <fmt> <addr> prints the value once a second (fmt: d u x q f f3 f4 h s);
 *                             "mon clear" removes them all
 *     wait <frames>           runs the remaining lines that many frames later
 *     dump <addr> <len> <file>  writes guest memory to a host file (unreadable parts as zeros)
 *     find32 <addr> <len> <value>  aligned dwords equal to value (at most 64 matches)
 *   Uncharted 2 helpers (1.02 addresses):
 *     levels                  every loaded package record (0xa60 bytes each at [0x1130390]):
 *                             index, name id, address, spawner count
 *     spawners <level>        the level's spawners (<level> = record index or a name) with
 *                             their world locators (0x4bdfe0)
 *     spawnat <spawner> [art] kills the player (if any) and spawns the hero at the spawner
 *                             (name or hex id; found with 0x56c7b0) through 0x975190, art
 *                             "hero-default" unless given
 *     spawncam                the debug spawn at the camera (0x969320)
 *     echo <text>
 *   Address and value expressions: hex numbers (0x optional), i:<hex> = image offset,
 *   [<expr>] = the 8 bytes at <expr>, h:<expr> = an ND process handle at <expr> (the process
 *   pointer when the handle is valid, else 0), sid:<name> = ND string id, s:<text> = a pointer to
 *   that text, $ / $d = rax / rdx of the last call, b: = a 4 KiB scratch buffer (64-byte aligned, kept between commands), followed by
 *   any number of +<hex> / -<hex>. call takes up to 8 integer args (the 7th and 8th on the stack)
 *   and prints rax and rdx (16-byte struct returns, e.g. ND process handles).
 * GOW3_COOP_BOOT=<file>: the same commands, run once when the game's frame count reaches the
 *   number on the file's first line ("at <frames>"), for unattended tests.
 */
#include "runtime.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#ifdef _WIN32
static unsigned char *image;
static uint64_t image_size;
static const char *cmd_path, *boot_path;
static uint64_t frames;

typedef struct {
    const char *game;
    uint64_t offset;
    unsigned length;                 /* bytes replaced (whole instructions, >= 5) */
    unsigned char expected[16];      /* original bytes at the site (all compared) */
} CoopSite;
/* U2 1.02 GameLoopUpdate (the "GameLoopUpdate" job of big.cpp, registered at 0x3f5b0b):
 * push rbp; mov rbp, rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp, 0x1b8 */
static const CoopSite sites[] = {
    {"Uncharted 2", 0x3facb0, 6, {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec}},
};

static int rd(uint64_t address, void *out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)(uintptr_t)address, out, n, &got) && got == n;
}
static int wr(uint64_t address, const void *in, size_t n) {
    SIZE_T put = 0;
    return WriteProcessMemory(GetCurrentProcess(), (LPVOID)(uintptr_t)address, in, n, &put) && put == n;
}
static uint64_t rq(uint64_t address) {
    uint64_t v = 0;
    rd(address, &v, 8);
    return v;
}

/* ND string id: CRC-32 (polynomial 0x04c11db7, MSB first, initial value 0, no final xor). */
static uint32_t sid(const char *s, size_t n) {
    uint32_t c = 0;
    for (size_t i = 0; i < n; ++i) {
        c ^= (uint32_t)(unsigned char)s[i] << 24;
        for (int k = 0; k < 8; ++k) c = c & 0x80000000u ? (c << 1) ^ 0x04c11db7u : c << 1;
    }
    return c;
}

/* Texts for s:<text> arguments (kept until the next command file). */
static char strings[16][128];
static int string_count;
static __attribute__((aligned(64))) unsigned char scratch[4096];
static uint64_t last_rax, last_rdx;

static const char *skip(const char *p) {
    while (*p == ' ' || *p == '\t') ++p;
    return p;
}
static int is_end(char c) { return !c || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ']'; }

static uint64_t expr(const char **pp, int *ok);
static uint64_t primary(const char **pp, int *ok) {
    const char *p = skip(*pp);
    uint64_t v = 0;
    if (*p == '[') {
        ++p;
        const uint64_t a = expr(&p, ok);
        p = skip(p);
        if (*p == ']') ++p; else *ok = 0;
        v = rq(a);
    } else if (!strncmp(p, "i:", 2)) {
        char *end;
        v = (uint64_t)(uintptr_t)image + strtoull(p + 2, &end, 16);
        if (end == p + 2) *ok = 0;
        p = end;
    } else if (!strncmp(p, "h:", 2)) {
        p += 2;
        const uint64_t a = primary(&p, ok);
        const uint64_t ptr = rq(a);
        const uint32_t id = (uint32_t)rq(a + 8);
        const uint64_t proc = ptr ? rq(ptr) : 0;
        v = proc && id && (uint32_t)rq(proc + 0x44) == id ? proc : 0;
    } else if (!strncmp(p, "sid:", 4)) {
        p += 4;
        const char *s = p;
        while (!is_end(*p)) ++p;
        v = sid(s, (size_t)(p - s));
    } else if (*p == '$') {
        ++p;
        if (*p == 'd') { ++p; v = last_rdx; } else v = last_rax;
    } else if (!strncmp(p, "b:", 2)) {
        p += 2;
        v = (uint64_t)(uintptr_t)scratch;
    } else if (!strncmp(p, "s:", 2)) {
        p += 2;
        const char *s = p;
        while (!is_end(*p)) ++p;
        if (string_count < 16) {
            size_t n = (size_t)(p - s) < 127 ? (size_t)(p - s) : 127;
            memcpy(strings[string_count], s, n);
            strings[string_count][n] = 0;
            v = (uint64_t)(uintptr_t)strings[string_count++];
        } else *ok = 0;
    } else {
        char *end;
        v = strtoull(p, &end, 16);
        if (end == p) *ok = 0;
        p = end;
    }
    *pp = p;
    return v;
}
static uint64_t expr(const char **pp, int *ok) {
    uint64_t v = primary(pp, ok);
    for (;;) {
        const char *p = *pp;
        if (*p == '+' || *p == '-') {
            const int minus = *p == '-';
            ++p;
            const uint64_t w = primary(&p, ok);
            v = minus ? v - w : v + w;
            *pp = p;
        } else break;
    }
    return v;
}

typedef struct { char name[24]; char fmt[4]; char text[96]; } Monitor;
static Monitor monitors[24];
static int monitor_count;

static void show(const char *name, const char *fmt, uint64_t a) {
    char line[512];
    int n = snprintf(line, sizeof(line), "Coop: %s @%llx =", name, (unsigned long long)a);
    if (!strcmp(fmt, "d")) { int32_t v = 0; rd(a, &v, 4); n += snprintf(line + n, sizeof(line) - n, " %d", v); }
    else if (!strcmp(fmt, "u")) { uint32_t v = 0; rd(a, &v, 4); n += snprintf(line + n, sizeof(line) - n, " %u", v); }
    else if (!strcmp(fmt, "x")) { uint32_t v = 0; rd(a, &v, 4); n += snprintf(line + n, sizeof(line) - n, " %#x", v); }
    else if (!strcmp(fmt, "q")) n += snprintf(line + n, sizeof(line) - n, " %#llx", (unsigned long long)rq(a));
    else if (fmt[0] == 'f') {
        int count = fmt[1] ? fmt[1] - '0' : 1;
        if (count < 1 || count > 8) count = 1;
        float v[8] = {0};
        rd(a, v, (size_t)count * 4);
        for (int i = 0; i < count; ++i) n += snprintf(line + n, sizeof(line) - n, " %.3f", v[i]);
    } else if (!strcmp(fmt, "s")) {
        char s[96] = {0};
        rd(a, s, sizeof(s) - 1);
        n += snprintf(line + n, sizeof(line) - n, " \"%s\"", s);
    }
    puts(line);
}

/* Guest call: integer args in rdi..r9 then the stack, floats in xmm0..xmm3 (SysV); a 16-byte
 * struct of two integers comes back in rax:rdx. */
typedef struct { uint64_t rax, rdx; } GuestPair;
typedef GuestPair (ABI *GuestCall)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                   uint64_t, uint64_t, float, float, float, float);


/* ---- Uncharted 2 (1.02) helpers ---- */
#define U2_PACKAGES      0x1130390   /* [this] = 180 package records of 0xa60 bytes */
#define U2_PLAYER        0x115bd20   /* player process handle */
typedef struct { uint64_t rax, rdx; } GuestPair2;
typedef GuestPair2 (ABI *U2Spawn)(void *, void *, const char *, uint64_t, uint64_t, uint64_t, uint64_t);
typedef void *(ABI *U2Locator)(void *out, uint64_t spawner);
typedef uint64_t (ABI *U2Lookup)(uint64_t mgr, uint32_t id);
typedef uint64_t (ABI *U2Dc)(uint32_t id);
typedef void (ABI *U2Void)(void);

static uint64_t u2_record(const char *arg) {
    const uint64_t base = rq((uint64_t)(uintptr_t)image + U2_PACKAGES);
    if (!base) return 0;
    char *end;
    const unsigned long index = strtoul(arg, &end, 10);
    if (end != arg && is_end(*end)) return index < 180 ? base + index * 0xa60 : 0;
    size_t n = 0;
    while (!is_end(arg[n])) ++n;
    const uint32_t id = sid(arg, n);
    for (int i = 0; i < 180; ++i) {
        uint32_t v = 0;
        rd(base + (uint64_t)i * 0xa60 + 0x74, &v, 4);
        if (v == id) return base + (uint64_t)i * 0xa60;
    }
    return 0;
}
static uint32_t name_or_id(const char *arg) {
    size_t n = 0;
    while (!is_end(arg[n])) ++n;
    if (n > 2 && arg[0] == '0' && arg[1] == 'x') return (uint32_t)strtoul(arg, NULL, 16);
    return sid(arg, n);
}
/* A spawner by id in any loaded package record (0x56c7b0 only searches the gameplay levels, not
 * e.g. the co-op net pak village-wide-coop). */
static uint64_t u2_find_spawner(uint32_t id) {
    const uint64_t base = rq((uint64_t)(uintptr_t)image + U2_PACKAGES);
    for (int i = 0; i < 180 && base; ++i) {
        const uint64_t t = rq(base + (uint64_t)i * 0xa60 + 0x9a0);
        if (!t) continue;
        int32_t n = 0;
        rd(t + 0x20, &n, 4);
        const uint64_t arr = rq(t + 0x18);
        for (int k = 0; k < n && arr; ++k) {
            uint32_t v = 0;
            rd(arr + (uint64_t)k * 16, &v, 4);
            if (v == id) return rq(arr + (uint64_t)k * 16 + 8);
        }
    }
    return 0;
}
static int u2_helper(const char *op, const char *p) {
    if (!strcmp(op, "levels")) {
        const uint64_t base = rq((uint64_t)(uintptr_t)image + U2_PACKAGES);
        for (int i = 0; i < 180 && base; ++i) {
            const uint64_t r = base + (uint64_t)i * 0xa60;
            uint32_t id = 0;
            rd(r + 0x74, &id, 4);
            if (!id) continue;
            const uint64_t t = rq(r + 0x9a0);
            int32_t n = 0;
            if (t) rd(t + 0x20, &n, 4);
            printf("Coop: level %d id %08x at %llx spawners %d\n", i, id, (unsigned long long)r, n);
        }
        return 1;
    }
    if (!strcmp(op, "spawners")) {
        const uint64_t r = u2_record(skip(p));
        const uint64_t t = r ? rq(r + 0x9a0) : 0;
        int32_t n = 0;
        if (t) rd(t + 0x20, &n, 4);
        const uint64_t arr = t ? rq(t + 0x18) : 0;
        printf("Coop: spawners of %llx: %d\n", (unsigned long long)r, n);
        for (int i = 0; i < n && arr; ++i) {
            uint32_t id = 0;
            rd(arr + (uint64_t)i * 16, &id, 4);
            const uint64_t sp = rq(arr + (uint64_t)i * 16 + 8);
            float *loc = (float *)scratch;
            memset(loc, 0, 32);
            if (sp) ((U2Locator)(image + 0x4bdfe0))(loc, sp);
            printf("Coop: spawner %08x at %llx pos %.2f %.2f %.2f rot %.3f %.3f %.3f %.3f\n", id,
                   (unsigned long long)sp, loc[0], loc[1], loc[2], loc[4], loc[5], loc[6], loc[7]);
        }
        return 1;
    }
    if (!strcmp(op, "spawncam")) {
        ((U2Void)(image + 0x969320))();
        printf("Coop: spawncam done\n");
        return 1;
    }
    if (!strcmp(op, "spawnat")) {
        p = skip(p);
        const uint32_t id = name_or_id(p);
        while (!is_end(*p)) ++p;
        p = skip(p);
        static char art[64];
        size_t n = 0;
        while (!is_end(p[n]) && n < 63) { art[n] = p[n]; ++n; }
        art[n] = 0;
        if (!n) strcpy(art, "hero-default");
        uint64_t sp = ((U2Lookup)(image + 0x56c7b0))((uint64_t)(uintptr_t)image + U2_PACKAGES, id);
        if (!sp) sp = u2_find_spawner(id);
        if (!sp) { printf("Coop: spawnat: spawner %08x not found\n", id); return 1; }
        float *loc = (float *)scratch;
        ((U2Locator)(image + 0x4bdfe0))(loc, sp);
        float *loc2 = (float *)(scratch + 64);
        memcpy(loc2, loc, 32);
        /* the player, if any, goes first (the debug toggle's kill path) */
        if (rq((uint64_t)(uintptr_t)image + U2_PLAYER) &&
            (uint32_t)rq((uint64_t)(uintptr_t)image + U2_PLAYER + 8)) ((U2Void)(image + 0x9691d0))();
        const uint64_t weapons = ((U2Dc)(image + 0xa15e70))(0xc66bbbc7u); /* *player-debug-weapons* */
        const GuestPair2 h = ((U2Spawn)(image + 0x975190))(loc, loc2, art, weapons, 0, 0, 0); /* spawner 0: with it the spawn fails */
        printf("Coop: spawnat %08x (%s) pos %.2f %.2f %.2f -> %llx %llx\n", id, art, loc[0], loc[1], loc[2],
               (unsigned long long)h.rax, (unsigned long long)h.rdx);
        return 1;
    }
    return 0;
}

static char pending[16384];
static int pending_wait;

static void run_line(const char *line) {
    char op[16] = {0};
    const char *p = skip(line);
    int k = 0;
    while (!is_end(*p) && k < 15) op[k++] = *p++;
    op[k] = 0;
    if (!op[0] || op[0] == '#') return;
    int ok = 1;
    if (!strcmp(op, "echo")) { printf("Coop: %s\n", skip(p)); return; }
    if (!strcmp(op, "mon")) {
        p = skip(p);
        if (!strncmp(p, "clear", 5)) { monitor_count = 0; puts("Coop: monitors cleared"); return; }
        if (monitor_count >= 24) return;
        Monitor *m = &monitors[monitor_count];
        if (sscanf(p, "%23s %3s %95[^\r\n]", m->name, m->fmt, m->text) == 3) ++monitor_count;
        return;
    }
    if (!strcmp(op, "r") || !strcmp(op, "rd") || !strcmp(op, "rq") || !strcmp(op, "rf")) {
        const uint64_t a = expr(&p, &ok);
        p = skip(p);
        uint64_t n = *p ? strtoull(p, NULL, 0) : (op[1] ? 4 : 64);
        if (!ok) { printf("Coop: bad address in '%s'\n", line); return; }
        unsigned char buf[512];
        const size_t unit = op[1] == 'd' || op[1] == 'f' ? 4 : op[1] == 'q' ? 8 : 1;
        if (n * unit > sizeof(buf)) n = sizeof(buf) / unit;
        if (!rd(a, buf, n * unit)) { printf("Coop: %llx not readable\n", (unsigned long long)a); return; }
        for (uint64_t i = 0; i < n * unit; i += 32) {
            char out[400];
            int m = snprintf(out, sizeof(out), "Coop: %llx:", (unsigned long long)(a + i));
            for (uint64_t j = i; j < i + 32 && j < n * unit; j += unit) {
                if (unit == 1) m += snprintf(out + m, sizeof(out) - m, " %02x", buf[j]);
                else if (op[1] == 'd') { uint32_t v; memcpy(&v, buf + j, 4); m += snprintf(out + m, sizeof(out) - m, " %08x", v); }
                else if (op[1] == 'f') { float v; memcpy(&v, buf + j, 4); m += snprintf(out + m, sizeof(out) - m, " %.4g", v); }
                else { uint64_t v; memcpy(&v, buf + j, 8); m += snprintf(out + m, sizeof(out) - m, " %016llx", (unsigned long long)v); }
            }
            puts(out);
        }
        return;
    }
    if (!strcmp(op, "dump")) {
        const uint64_t a = expr(&p, &ok);
        const uint64_t n = expr(&p, &ok);
        p = skip(p);
        char path[260];
        int k2 = 0;
        while (*p && *p != '\r' && *p != '\n' && k2 < 259) path[k2++] = *p++;
        path[k2] = 0;
        FILE *f = ok && n <= (UINT64_C(64) << 20) ? fopen(path, "wb") : NULL;
        if (!f) { printf("Coop: dump failed '%s'\n", line); return; }
        static unsigned char chunk[65536];
        uint64_t bad = 0;
        for (uint64_t at = 0; at < n; at += sizeof(chunk)) {
            const size_t c = n - at < sizeof(chunk) ? (size_t)(n - at) : sizeof(chunk);
            if (!rd(a + at, chunk, c)) {
                /* page by page */
                for (size_t q = 0; q < c; q += 4096) {
                    const size_t m = c - q < 4096 ? c - q : 4096;
                    if (!rd(a + at + q, chunk + q, m)) { memset(chunk + q, 0, m); bad += m; }
                }
            }
            fwrite(chunk, 1, c, f);
        }
        fclose(f);
        printf("Coop: dumped %llx+%llx to %s (%llu bytes unreadable)\n", (unsigned long long)a,
               (unsigned long long)n, path, (unsigned long long)bad);
        return;
    }
    if (!strcmp(op, "find32")) {
        const uint64_t a = expr(&p, &ok);
        const uint64_t n = expr(&p, &ok);
        const uint32_t v = (uint32_t)expr(&p, &ok);
        if (!ok) { printf("Coop: bad find32 '%s'\n", line); return; }
        static uint32_t chunk[16384];
        int hits = 0;
        for (uint64_t at = 0; at < n && hits < 64; at += sizeof(chunk)) {
            const size_t c = n - at < sizeof(chunk) ? (size_t)(n - at) : sizeof(chunk);
            if (!rd(a + at, chunk, c)) continue;
            for (size_t i = 0; i < c / 4 && hits < 64; ++i)
                if (chunk[i] == v) { printf("Coop: find32 %x at %llx\n", v, (unsigned long long)(a + at + i * 4)); ++hits; }
        }
        printf("Coop: find32 %x: %d matches\n", v, hits);
        return;
    }
    if (!strcmp(op, "rs")) {
        const uint64_t a = expr(&p, &ok);
        show("string", "s", a);
        return;
    }
    if (op[0] == 'w') {
        const uint64_t a = expr(&p, &ok);
        p = skip(p);
        int done = 0;
        if (!strcmp(op, "wf")) { float f = strtof(p, NULL); done = wr(a, &f, 4); }
        else {
            const uint64_t v = expr(&p, &ok);
            const size_t n = !strcmp(op, "w8") ? 1 : !strcmp(op, "w16") ? 2 : !strcmp(op, "w32") ? 4 : 8;
            if (ok) done = wr(a, &v, n);
        }
        printf("Coop: %s %llx %s\n", op, (unsigned long long)a, done ? "ok" : "FAILED");
        return;
    }
    if (!strcmp(op, "call")) {
        const uint64_t fn = expr(&p, &ok);
        uint64_t a[8] = {0};
        float f[4] = {0};
        int na = 0, nf = 0;
        for (p = skip(p); *p && *p != '\r' && *p != '\n'; p = skip(p)) {
            if (!strncmp(p, "f:", 2)) {
                char *end;
                const float v = strtof(p + 2, &end);
                if (nf < 4) f[nf++] = v;
                p = end;
            } else {
                const uint64_t v = expr(&p, &ok);
                if (na < 8) a[na++] = v;
            }
            if (!ok) break;
        }
        if (!ok || !fn) { printf("Coop: bad call '%s'\n", line); return; }
        printf("Coop: call %llx (%d args)\n", (unsigned long long)fn, na);
        fflush(stdout);
        const GuestPair r = ((GuestCall)(uintptr_t)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], f[0], f[1], f[2], f[3]);
        last_rax = r.rax;
        last_rdx = r.rdx;
        printf("Coop: call %llx -> rax %#llx rdx %#llx\n", (unsigned long long)fn, (unsigned long long)r.rax, (unsigned long long)r.rdx);
        return;
    }
    if (u2_helper(op, p)) return;
    printf("Coop: unknown command '%s'\n", op);
}

/* Runs lines from text; a "wait N" keeps the rest for N frames later. */
static void run_text(const char *text) {
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[512];
        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = 0;
        p = e ? e + 1 : p + n;
        const char *q = skip(line);
        if (!strncmp(q, "wait", 4) && is_end(q[4])) {
            pending_wait = atoi(q + 4);
            snprintf(pending, sizeof(pending), "%s", p);
            return;
        }
        run_line(line);
    }
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static char text[16384];
    const size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = 0;
    return text;
}

static void ABI coop_frame(void) {
    ++frames;
    static ULONGLONG last_check, last_monitor;
    static FILETIME last_write;
    static int boot_done;
    const ULONGLONG now = GetTickCount64();
    if (pending_wait > 0 && --pending_wait == 0) {
        static char text[sizeof(pending)];
        memcpy(text, pending, sizeof(text));
        pending[0] = 0;
        run_text(text);
        fflush(stdout);
    }
    if (boot_path && !boot_done) {
        static char boot_text[16384], *boot;
        static unsigned long long at = ~0ull;
        if (!boot) {
            const char *text = read_file(boot_path);
            if (text) {
                snprintf(boot_text, sizeof(boot_text), "%s", text);
                boot = boot_text;
                if (!strncmp(boot, "at ", 3)) at = strtoull(boot + 3, NULL, 10);
            } else boot_done = 1;
        }
        if (boot && frames >= at) {
            boot_done = 1;
            static char text[16384];
            snprintf(text, sizeof(text), "%s", strchr(boot, '\n') ? strchr(boot, '\n') + 1 : "");
            printf("Coop: boot commands at frame %llu\n", frames);
            string_count = 0;
            run_text(text);
            fflush(stdout);
        }
    }
    if (cmd_path && now - last_check >= 250) {
        last_check = now;
        WIN32_FILE_ATTRIBUTE_DATA info;
        if (GetFileAttributesExA(cmd_path, GetFileExInfoStandard, &info) &&
            CompareFileTime(&info.ftLastWriteTime, &last_write)) {
            last_write = info.ftLastWriteTime;
            char *text = read_file(cmd_path);
            if (text) {
                printf("Coop: commands at frame %llu\n", frames);
                string_count = 0;
                run_text(text);
                fflush(stdout);
            }
        }
    }
    if (monitor_count && now - last_monitor >= 1000) {
        last_monitor = now;
        printf("Coop: frame %llu\n", frames);
        for (int i = 0; i < monitor_count; ++i) {
            const char *p = monitors[i].text;
            int ok = 1;
            const uint64_t a = expr(&p, &ok);
            if (!strcmp(monitors[i].fmt, "h")) {
                printf("Coop: %s = %#llx\n", monitors[i].name, (unsigned long long)a);
                continue;
            }
            if (ok) show(monitors[i].name, monitors[i].fmt, a);
        }
        fflush(stdout);
    }
}

/* Stub: saves rax, rcx, rdx, rsi, rdi, r8-r11 and xmm0-7, calls the handler (rsp 16-byte aligned:
 * the entry's rsp is 8 mod 16, 9 pushes make it 0), restores them, replays the replaced bytes and
 * jumps back. */
static unsigned build_stub(unsigned char *p, void *handler, const unsigned char *replayed, unsigned length, uintptr_t resume) {
    static const unsigned char save[] = {
        0x50, 0x51, 0x52, 0x56, 0x57, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,
        0x48, 0x81, 0xec, 0x80, 0x00, 0x00, 0x00,
        0xf3, 0x0f, 0x7f, 0x04, 0x24,       0xf3, 0x0f, 0x7f, 0x4c, 0x24, 0x10,
        0xf3, 0x0f, 0x7f, 0x54, 0x24, 0x20, 0xf3, 0x0f, 0x7f, 0x5c, 0x24, 0x30,
        0xf3, 0x0f, 0x7f, 0x64, 0x24, 0x40, 0xf3, 0x0f, 0x7f, 0x6c, 0x24, 0x50,
        0xf3, 0x0f, 0x7f, 0x74, 0x24, 0x60, 0xf3, 0x0f, 0x7f, 0x7c, 0x24, 0x70,
    };
    static const unsigned char restore[] = {
        0xf3, 0x0f, 0x6f, 0x04, 0x24,       0xf3, 0x0f, 0x6f, 0x4c, 0x24, 0x10,
        0xf3, 0x0f, 0x6f, 0x54, 0x24, 0x20, 0xf3, 0x0f, 0x6f, 0x5c, 0x24, 0x30,
        0xf3, 0x0f, 0x6f, 0x64, 0x24, 0x40, 0xf3, 0x0f, 0x6f, 0x6c, 0x24, 0x50,
        0xf3, 0x0f, 0x6f, 0x74, 0x24, 0x60, 0xf3, 0x0f, 0x6f, 0x7c, 0x24, 0x70,
        0x48, 0x81, 0xc4, 0x80, 0x00, 0x00, 0x00,
        0x41, 0x5b, 0x41, 0x5a, 0x41, 0x59, 0x41, 0x58, 0x5f, 0x5e, 0x5a, 0x59, 0x58,
    };
    unsigned n = 0;
    memcpy(p + n, save, sizeof(save)); n += sizeof(save);
    p[n++] = 0x48; p[n++] = 0xb8; /* mov rax, handler */
    const uint64_t h = (uint64_t)(uintptr_t)handler;
    memcpy(p + n, &h, 8); n += 8;
    p[n++] = 0xff; p[n++] = 0xd0; /* call rax */
    memcpy(p + n, restore, sizeof(restore)); n += sizeof(restore);
    memcpy(p + n, replayed, length); n += length;
    static const unsigned char jump[] = {0xff, 0x25, 0x00, 0x00, 0x00, 0x00}; /* jmp [rip+0] */
    memcpy(p + n, jump, sizeof(jump)); n += sizeof(jump);
    const uint64_t target = (uint64_t)resume;
    memcpy(p + n, &target, 8); n += 8;
    return n;
}

static unsigned char *page_near_image(void) {
    const uintptr_t lo = (uintptr_t)image > UINT64_C(0x60000000) ? (uintptr_t)image - UINT64_C(0x60000000) : 0x10000;
    const uintptr_t hi = (uintptr_t)image + (uintptr_t)image_size + UINT64_C(0x60000000);
    MEMORY_BASIC_INFORMATION info;
    for (uintptr_t address = lo; address < hi && VirtualQuery((void *)address, &info, sizeof(info));
         address = (uintptr_t)info.BaseAddress + info.RegionSize) {
        if (info.State != MEM_FREE) continue;
        const uintptr_t start = ((uintptr_t)info.BaseAddress + 0xffff) & ~(uintptr_t)0xffff;
        if (start + 4096 > (uintptr_t)info.BaseAddress + info.RegionSize) continue;
        void *p = VirtualAlloc((void *)start, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (p) return p;
    }
    return NULL;
}

/* probe.c calls this while the image is still writable (before its final protection). */
void runtime_coop_install(unsigned char *base, uint64_t size) {
    cmd_path = getenv("GOW3_COOP_CMD");
    boot_path = getenv("GOW3_COOP_BOOT");
    if ((!cmd_path || !*cmd_path) && (!boot_path || !*boot_path)) return;
    if (cmd_path && !*cmd_path) cmd_path = NULL;
    if (boot_path && !*boot_path) boot_path = NULL;
    image = base;
    image_size = size;
    for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
        const CoopSite *s = &sites[i];
        if (s->offset + sizeof(s->expected) > size || memcmp(image + s->offset, s->expected, sizeof(s->expected))) continue;
        unsigned char *stub = page_near_image();
        if (!stub) { puts("Coop: no stub page in range"); return; }
        build_stub(stub, (void *)coop_frame, image + s->offset, s->length, (uintptr_t)(image + s->offset + s->length));
        DWORD old;
        VirtualProtect(stub, 4096, PAGE_EXECUTE_READ, &old);
        const int64_t delta = (int64_t)(uintptr_t)stub - (int64_t)(uintptr_t)(image + s->offset + 5);
        if (delta < INT32_MIN || delta > INT32_MAX) { puts("Coop: stub out of range"); return; }
        const int32_t rel = (int32_t)delta;
        image[s->offset] = 0xe9;
        memcpy(image + s->offset + 1, &rel, 4);
        memset(image + s->offset + 5, 0x90, s->length - 5);
        printf("Coop: %s frame hook at image offset %#llx (commands: %s, boot: %s)\n", s->game,
               (unsigned long long)s->offset, cmd_path ? cmd_path : "-", boot_path ? boot_path : "-");
        return;
    }
    puts("Coop: no known frame function in this executable");
}
#else
void runtime_coop_install(unsigned char *base, uint64_t size) { (void)base; (void)size; }
#endif
