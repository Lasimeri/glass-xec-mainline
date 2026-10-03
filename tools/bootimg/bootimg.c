/* bootimg: Android boot image (header v0, as Glass's bootloader reads it)
 * info, unpack and pack, in C (no Python: AOSP's mkbootimg is a Python
 * script). Build: tcc -o bootimg bootimg.c
 *
 *   bootimg info IMG
 *   bootimg unpack IMG DIR      kernel, ramdisk, second (if any), header.txt
 *   bootimg repack DIR OUT      from an unpacked DIR, id recomputed
 *   bootimg pack OUT KERNEL RAMDISK [options]
 *       --base 0x80000000 --kernel-offset 0x8000 --ramdisk-offset 0x01000000
 *       --second-offset 0x00f00000 --tags-offset 0x100 --pagesize 2048
 *       --second FILE --name STR --cmdline STR
 *
 * Header v0: magic "ANDROID!", kernel/ramdisk/second size and load address,
 * tags address, page size, two unused words, name[16], cmdline[512], id[32]
 * (SHA-1 of kernel, its size, ramdisk, its size, second, its size, as the
 * C mkbootimg of Android 4.x computed it), then extra_cmdline[1024] in later
 * versions. Each section starts on a page boundary; padding is zero.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>

/* ---- SHA-1 (FIPS 180-4) ---- */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; size_t n; } sha1_t;
static uint32_t rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
static void sha1_block(sha1_t *s, const uint8_t *p) {
    uint32_t w[80], a, b, c, d, e, t;
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | (uint32_t)p[4*i+1] << 16 | (uint32_t)p[4*i+2] << 8 | p[4*i+3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
        else { f = b ^ c ^ d; k = 0xCA62C1D6; }
        t = rol(a, 5) + f + e + k + w[i]; e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_init(sha1_t *s) {
    s->h[0] = 0x67452301; s->h[1] = 0xEFCDAB89; s->h[2] = 0x98BADCFE; s->h[3] = 0x10325476; s->h[4] = 0xC3D2E1F0;
    s->len = 0; s->n = 0;
}
static void sha1_update(sha1_t *s, const void *data, size_t n) {
    const uint8_t *p = data;
    s->len += n;
    while (n--) { s->buf[s->n++] = *p++; if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; } }
}
static void sha1_final(sha1_t *s, uint8_t out[20]) {
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, z = 0;
    sha1_update(s, &pad, 1);
    while (s->n != 56) sha1_update(s, &z, 1);
    for (int i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (8 * i)); sha1_update(s, &b, 1); }
    for (int i = 0; i < 5; i++) { out[4*i] = s->h[i] >> 24; out[4*i+1] = s->h[i] >> 16; out[4*i+2] = s->h[i] >> 8; out[4*i+3] = s->h[i]; }
}

/* ---- header ---- */
#define HDR_SIZE 1632   /* v0 with extra_cmdline; older images end at 608 */
typedef struct {
    uint32_t kernel_size, kernel_addr, ramdisk_size, ramdisk_addr;
    uint32_t second_size, second_addr, tags_addr, page_size, unused0, unused1;
    char name[17], cmdline[513], extra[1025];
    uint8_t id[32];
} hdr_t;

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static uint8_t *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "bootimg: %s: %s\n", path, strerror(errno)); exit(1); }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(len ? len : 1);
    if (!b || fread(b, 1, len, f) != (size_t)len) { fprintf(stderr, "bootimg: read %s failed\n", path); exit(1); }
    fclose(f);
    *n = len;
    return b;
}
static void spit(const char *path, const void *b, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(b, 1, n, f) != n || fclose(f)) { fprintf(stderr, "bootimg: write %s failed\n", path); exit(1); }
}
static size_t pages(size_t n, uint32_t ps) { return (n + ps - 1) / ps * ps; }

static int parse(const uint8_t *b, size_t n, hdr_t *h) {
    if (n < 608 || memcmp(b, "ANDROID!", 8)) { fprintf(stderr, "bootimg: not an Android boot image\n"); return -1; }
    h->kernel_size = rd32(b + 8);  h->kernel_addr = rd32(b + 12);
    h->ramdisk_size = rd32(b + 16); h->ramdisk_addr = rd32(b + 20);
    h->second_size = rd32(b + 24); h->second_addr = rd32(b + 28);
    h->tags_addr = rd32(b + 32);   h->page_size = rd32(b + 36);
    h->unused0 = rd32(b + 40);     h->unused1 = rd32(b + 44);
    memcpy(h->name, b + 48, 16); h->name[16] = 0;
    memcpy(h->cmdline, b + 64, 512); h->cmdline[512] = 0;
    memcpy(h->id, b + 576, 32);
    if (n >= HDR_SIZE) { memcpy(h->extra, b + 608, 1024); h->extra[1024] = 0; } else h->extra[0] = 0;
    if (h->page_size < 2048 || (h->page_size & (h->page_size - 1))) { fprintf(stderr, "bootimg: odd page size %u\n", h->page_size); return -1; }
    return 0;
}

static void compute_id(const uint8_t *k, uint32_t kn, const uint8_t *r, uint32_t rn, const uint8_t *s, uint32_t sn, uint8_t id[32]) {
    sha1_t c; uint8_t d[20];
    sha1_init(&c);
    sha1_update(&c, k, kn); sha1_update(&c, &kn, 4);   /* sizes as little-endian words, like mkbootimg on x86/ARM */
    sha1_update(&c, r, rn); sha1_update(&c, &rn, 4);
    sha1_update(&c, s, sn); sha1_update(&c, &sn, 4);
    sha1_final(&c, d);
    memset(id, 0, 32);
    memcpy(id, d, 20);
}

/* The image: header page, kernel, ramdisk, second, each page-aligned. */
static void write_image(const char *out, const hdr_t *h, const uint8_t *k, const uint8_t *r, const uint8_t *s) {
    uint32_t ps = h->page_size;
    size_t total = ps + pages(h->kernel_size, ps) + pages(h->ramdisk_size, ps) + pages(h->second_size, ps);
    uint8_t *img = calloc(1, total), *p = img;
    memcpy(p, "ANDROID!", 8);
    wr32(p + 8, h->kernel_size);   wr32(p + 12, h->kernel_addr);
    wr32(p + 16, h->ramdisk_size); wr32(p + 20, h->ramdisk_addr);
    wr32(p + 24, h->second_size);  wr32(p + 28, h->second_addr);
    wr32(p + 32, h->tags_addr);    wr32(p + 36, ps);
    wr32(p + 40, h->unused0);      wr32(p + 44, h->unused1);
    strncpy((char *)p + 48, h->name, 16);
    strncpy((char *)p + 64, h->cmdline, 512);
    memcpy(p + 576, h->id, 32);
    if (h->extra[0]) strncpy((char *)p + 608, h->extra, 1024);
    p += ps;
    memcpy(p, k, h->kernel_size);  p += pages(h->kernel_size, ps);
    memcpy(p, r, h->ramdisk_size); p += pages(h->ramdisk_size, ps);
    if (h->second_size) memcpy(p, s, h->second_size);
    spit(out, img, total);
    free(img);
}

static void hex(FILE *f, const uint8_t *b, int n) { for (int i = 0; i < n; i++) fprintf(f, "%02x", b[i]); }

static void print_hdr(FILE *f, const hdr_t *h) {
    fprintf(f, "kernel_size=%u\nkernel_addr=0x%08x\nramdisk_size=%u\nramdisk_addr=0x%08x\n", h->kernel_size, h->kernel_addr, h->ramdisk_size, h->ramdisk_addr);
    fprintf(f, "second_size=%u\nsecond_addr=0x%08x\ntags_addr=0x%08x\npage_size=%u\n", h->second_size, h->second_addr, h->tags_addr, h->page_size);
    fprintf(f, "unused0=0x%08x\nunused1=0x%08x\nname=%s\ncmdline=%s\nextra_cmdline=%s\nid=", h->unused0, h->unused1, h->name, h->cmdline, h->extra);
    hex(f, h->id, 32);
    fprintf(f, "\n");
}

static uint32_t num(const char *s) { return (uint32_t)strtoul(s, NULL, 0); }

int main(int argc, char **argv) {
    /* bootimg sha1 FILE: the SHA-1 above, to check against sha1sum. */
    if (argc >= 3 && !strcmp(argv[1], "sha1")) {
        size_t n; uint8_t *b = slurp(argv[2], &n), d[20]; sha1_t c;
        sha1_init(&c); sha1_update(&c, b, n); sha1_final(&c, d);
        hex(stdout, d, 20); printf("  %s\n", argv[2]);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "info")) {
        size_t n; uint8_t *b = slurp(argv[2], &n); hdr_t h;
        if (parse(b, n, &h)) return 1;
        print_hdr(stdout, &h);
        uint8_t id[32];
        compute_id(b + h.page_size, h.kernel_size,
                   b + h.page_size + pages(h.kernel_size, h.page_size), h.ramdisk_size,
                   b + h.page_size + pages(h.kernel_size, h.page_size) + pages(h.ramdisk_size, h.page_size), h.second_size, id);
        printf("id_computed="); hex(stdout, id, 32); printf("\nid_matches=%s\n", memcmp(id, h.id, 32) ? "no" : "yes");
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "unpack")) {
        size_t n; uint8_t *b = slurp(argv[2], &n); hdr_t h;
        if (parse(b, n, &h)) return 1;
        mkdir(argv[3], 0755);
        char p[4096];
        size_t ko = h.page_size, ro = ko + pages(h.kernel_size, h.page_size), so = ro + pages(h.ramdisk_size, h.page_size);
        if (so + h.second_size > n) { fprintf(stderr, "bootimg: image shorter than its header says\n"); return 1; }
        snprintf(p, sizeof p, "%s/kernel", argv[3]); spit(p, b + ko, h.kernel_size);
        snprintf(p, sizeof p, "%s/ramdisk", argv[3]); spit(p, b + ro, h.ramdisk_size);
        if (h.second_size) { snprintf(p, sizeof p, "%s/second", argv[3]); spit(p, b + so, h.second_size); }
        snprintf(p, sizeof p, "%s/header.txt", argv[3]);
        FILE *f = fopen(p, "w"); print_hdr(f, &h); fclose(f);
        size_t end = so + pages(h.second_size, h.page_size);
        if (n != end) printf("note: image is %zu bytes, sections end at %zu (%zd trailing)\n", n, end, (ssize_t)(n - end));
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "repack")) {
        char p[4096], line[2048]; hdr_t h; memset(&h, 0, sizeof h);
        snprintf(p, sizeof p, "%s/header.txt", argv[2]);
        FILE *f = fopen(p, "r");
        if (!f) { perror(p); return 1; }
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            char *v = strchr(line, '='); if (!v) continue; *v++ = 0;
            if (!strcmp(line, "kernel_addr")) h.kernel_addr = num(v);
            else if (!strcmp(line, "ramdisk_addr")) h.ramdisk_addr = num(v);
            else if (!strcmp(line, "second_addr")) h.second_addr = num(v);
            else if (!strcmp(line, "tags_addr")) h.tags_addr = num(v);
            else if (!strcmp(line, "page_size")) h.page_size = num(v);
            else if (!strcmp(line, "unused0")) h.unused0 = num(v);
            else if (!strcmp(line, "unused1")) h.unused1 = num(v);
            else if (!strcmp(line, "name")) strncpy(h.name, v, 16);
            else if (!strcmp(line, "cmdline")) strncpy(h.cmdline, v, 512);
            else if (!strcmp(line, "extra_cmdline")) strncpy(h.extra, v, 1024);
        }
        fclose(f);
        size_t kn, rn, sn = 0; uint8_t *s = NULL;
        snprintf(p, sizeof p, "%s/kernel", argv[2]); uint8_t *k = slurp(p, &kn);
        snprintf(p, sizeof p, "%s/ramdisk", argv[2]); uint8_t *r = slurp(p, &rn);
        snprintf(p, sizeof p, "%s/second", argv[2]);
        FILE *t = fopen(p, "rb"); if (t) { fclose(t); s = slurp(p, &sn); }
        h.kernel_size = kn; h.ramdisk_size = rn; h.second_size = sn;
        compute_id(k, kn, r, rn, s, sn, h.id);
        write_image(argv[3], &h, k, r, s);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "pack")) {
        uint32_t base = 0x80000000, ko = 0x8000, ro = 0x01000000, so = 0x00f00000, to = 0x100, ps = 2048;
        hdr_t h; memset(&h, 0, sizeof h);
        const char *second = NULL;
        for (int i = 5; i + 1 < argc; i += 2) {
            const char *o = argv[i], *v = argv[i + 1];
            if (!strcmp(o, "--base")) base = num(v);
            else if (!strcmp(o, "--kernel-offset")) ko = num(v);
            else if (!strcmp(o, "--ramdisk-offset")) ro = num(v);
            else if (!strcmp(o, "--second-offset")) so = num(v);
            else if (!strcmp(o, "--tags-offset")) to = num(v);
            else if (!strcmp(o, "--pagesize")) ps = num(v);
            else if (!strcmp(o, "--second")) second = v;
            else if (!strcmp(o, "--name")) strncpy(h.name, v, 16);
            else if (!strcmp(o, "--cmdline")) { if (strlen(v) > 511) { fprintf(stderr, "bootimg: cmdline over 511 bytes\n"); return 1; } strncpy(h.cmdline, v, 512); }
            else { fprintf(stderr, "bootimg: unknown option %s\n", o); return 1; }
        }
        size_t kn, rn, sn = 0; uint8_t *s = NULL;
        uint8_t *k = slurp(argv[3], &kn), *r = slurp(argv[4], &rn);
        if (second) s = slurp(second, &sn);
        h.kernel_size = kn; h.ramdisk_size = rn; h.second_size = sn;
        h.kernel_addr = base + ko; h.ramdisk_addr = base + ro; h.second_addr = base + so; h.tags_addr = base + to; h.page_size = ps;
        compute_id(k, kn, r, rn, s, sn, h.id);
        write_image(argv[2], &h, k, r, s);
        return 0;
    }
    fprintf(stderr, "bootimg info IMG | unpack IMG DIR | repack DIR OUT | pack OUT KERNEL RAMDISK [--base A --kernel-offset O --ramdisk-offset O --second-offset O --tags-offset O --pagesize N --second F --name S --cmdline S]\n");
    return 2;
}
