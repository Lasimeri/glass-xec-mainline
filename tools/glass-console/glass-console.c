/* glass-console: a shell on the Glass's display, with a blinking cursor.
 *
 *   glass-console [-s SCALE] [-f FONT.psf] [COMMAND ARGS...]   (default /bin/sh -l)
 *
 * The stock kernel has no framebuffer console (no fbcon, no /dev/tty1), so
 * the display showed whatever page a stream had left on the graphics layer.
 * glass-console draws a small terminal itself on page 0 of /dev/fb0 (the
 * graphics layer) and keeps the display controller pointed there; the video
 * stream (glass-fb -y) covers it with the overlay above and uncovers it when
 * the stream stops, so the shell is the idle picture.
 *
 * A pty runs the command. The emulation is the subset a shell prompt uses:
 * printable text, CR, LF, BS, TAB, line wrap, scrolling, the cursor moves,
 * clears and erases of VT100 (CSI H f J K A B C D G), the cursor position
 * report (CSI 6n); other escape sequences are read and dropped. No input
 * path yet: the Glass has no keyboard; the shell is a status indicator.
 *
 * The font is a PC Screen Font (PSF1 or PSF2, its Unicode table honoured
 * for Latin-1), drawn SCALE times (default 2: 8x16 glyphs as 16x32 cells,
 * 40 columns by 11 rows on 640x360). Light grey on black; black is clear
 * on the prism.
 *
 * Cost: it sleeps in poll(); the cursor blinks twice a second (one cell
 * redrawn). Started by init through /etc/glass/console display, which
 * starts it again when it ends. Compiled on the Glass (gcc from Alpine).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/* OMAP4 display controller (TRM: DISPC at 0x48041000), as glass-fb uses it:
 * the pan ioctl is accepted and never reaches the hardware on this kernel. */
#define DISPC_BASE 0x48041000u
#define DISPC_GFX_BA0 0x080
#define DISPC_GFX_BA1 0x084
#define DISPC_CONTROL2 0x238
#define DISPC_GO_LCD2 0x20

#define BLINK_MS 500

static unsigned char *font;
static unsigned fw, fh, nglyph, gbytes, grow;
static unsigned short gmap[256];

static uint32_t *fbm;
static unsigned stride, xres, yres;
static unsigned scale = 2, cw, ch, cols, rows, x0, y0;
static unsigned char *text;
static unsigned cx, cy;
static int cursor_drawn;
static const uint32_t fg = 0xffc0c0c0, bg = 0xff000000;   /* alpha opaque, in case the layer blends */

static int load_font(const char *path) {
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st)) { perror(path); return -1; }
    unsigned char *f = malloc(st.st_size);
    if (!f || read(fd, f, st.st_size) != st.st_size) { fprintf(stderr, "glass-console: %s: short read\n", path); return -1; }
    close(fd);
    size_t size = st.st_size, uni = 0;
    int has_uni = 0, psf2 = 0;
    if (size >= 32 && f[0] == 0x72 && f[1] == 0xb5 && f[2] == 0x4a && f[3] == 0x86) {
        uint32_t *h = (uint32_t *) f;   /* magic version headersize flags length charsize height width */
        psf2 = 1;
        nglyph = h[4]; gbytes = h[5]; fh = h[6]; fw = h[7];
        font = f + h[2];
        has_uni = h[3] & 1;
        uni = h[2] + (size_t) nglyph * gbytes;
    } else if (size >= 4 && f[0] == 0x36 && f[1] == 0x04) {
        nglyph = (f[2] & 1) ? 512 : 256; gbytes = f[3]; fh = f[3]; fw = 8;
        font = f + 4;
        has_uni = (f[2] & 6) != 0;
        uni = 4 + (size_t) nglyph * gbytes;
    } else {
        fprintf(stderr, "glass-console: %s: not a PSF font\n", path); return -1;
    }
    grow = (fw + 7) / 8;
    if (uni > size || fw == 0 || fh == 0 || grow * fh > gbytes) { fprintf(stderr, "glass-console: %s: bad PSF header\n", path); return -1; }
    for (unsigned i = 0; i < 256; i++) gmap[i] = i < nglyph ? i : '?';
    if (!has_uni) return 0;
    /* The Unicode table: per glyph, its code points, then a terminator
     * (PSF2: UTF-8, 0xFE starts a sequence, 0xFF ends; PSF1: 16-bit words,
     * 0xFFFE and 0xFFFF). Only single Latin-1 code points are kept. */
    size_t p = uni;
    for (unsigned g = 0; g < nglyph && p < size; g++) {
        int seq = 0;
        while (p < size) {
            uint32_t cp;
            if (psf2) {
                unsigned char b = f[p];
                if (b == 0xff) { p++; break; }
                if (b == 0xfe) { seq = 1; p++; continue; }
                int n = b < 0x80 ? 1 : b < 0xe0 ? 2 : b < 0xf0 ? 3 : 4;
                cp = n == 1 ? b : n == 2 ? b & 0x1f : n == 3 ? b & 0x0f : b & 0x07;
                for (int k = 1; k < n && p + k < size; k++) cp = cp << 6 | (f[p + k] & 0x3f);
                p += n;
            } else {
                if (p + 1 >= size) { p = size; break; }
                cp = f[p] | f[p + 1] << 8;
                p += 2;
                if (cp == 0xffff) break;
                if (cp == 0xfffe) { seq = 1; continue; }
            }
            if (!seq && cp < 256) gmap[cp] = g;
        }
    }
    return 0;
}

static void draw_cell(unsigned col, unsigned row, int inverse) {
    unsigned char c = text[row * cols + col];
    const unsigned char *g = font + (size_t) gmap[c] * gbytes;
    uint32_t *base = fbm + (size_t) (y0 + row * ch) * stride + x0 + col * cw;
    for (unsigned y = 0; y < fh; y++) {
        const unsigned char *bits = g + y * grow;
        for (unsigned s = 0; s < scale; s++) {
            uint32_t *o = base + (size_t) (y * scale + s) * stride;
            for (unsigned x = 0; x < fw; x++) {
                uint32_t v = ((bits[x >> 3] >> (7 - (x & 7))) & 1) ^ inverse ? fg : bg;
                for (unsigned t = 0; t < scale; t++) *o++ = v;
            }
        }
    }
}

static void redraw(void) {
    for (unsigned r = 0; r < rows; r++)
        for (unsigned c = 0; c < cols; c++) draw_cell(c, r, 0);
}

static void cursor(int on) {
    unsigned c = cx < cols ? cx : cols - 1;
    if (on == cursor_drawn) return;
    draw_cell(c, cy, on);
    cursor_drawn = on;
}

static void clear_cells(unsigned from, unsigned to) {   /* [from, to) in row-major order */
    for (unsigned i = from; i < to; i++) {
        text[i] = ' ';
        draw_cell(i % cols, i / cols, 0);
    }
}

static void line_feed(void) {
    if (cy + 1 < rows) { cy++; return; }
    memmove(text, text + cols, (size_t) cols * (rows - 1));
    memset(text + (size_t) cols * (rows - 1), ' ', cols);
    redraw();
}

static int master = -1;
static int state;   /* 0 text, 1 after ESC, 2 in CSI, 3 in OSC, 4 OSC after ESC */
static unsigned par[8], npar;

static void csi(unsigned char f) {
    unsigned a = npar > 0 ? par[0] : 0, n = a ? a : 1;
    switch (f) {
    case 'H': case 'f': {
        unsigned r = a ? a - 1 : 0, c = npar > 1 && par[1] ? par[1] - 1 : 0;
        cy = r < rows ? r : rows - 1;
        cx = c < cols ? c : cols - 1;
        break;
    }
    case 'A': cy = cy >= n ? cy - n : 0; break;
    case 'B': cy = cy + n < rows ? cy + n : rows - 1; break;
    case 'C': cx = cx + n < cols ? cx + n : cols - 1; break;
    case 'D': cx = cx >= n ? cx - n : 0; break;
    case 'G': cx = n - 1 < cols ? n - 1 : cols - 1; break;
    case 'J': {
        unsigned at = cy * cols + (cx < cols ? cx : cols - 1);
        if (a == 0) clear_cells(at, rows * cols);
        else if (a == 1) clear_cells(0, at + 1);
        else { clear_cells(0, rows * cols); }
        break;
    }
    case 'K': {
        unsigned row = cy * cols, at = row + (cx < cols ? cx : cols - 1);
        if (a == 0) clear_cells(at, row + cols);
        else if (a == 1) clear_cells(row, at + 1);
        else clear_cells(row, row + cols);
        break;
    }
    case 'n':
        if (a == 6) {
            char r[32];
            int l = snprintf(r, sizeof r, "\033[%u;%uR", cy + 1, (cx < cols ? cx : cols - 1) + 1);
            if (write(master, r, l) < 0) { /* the shell is gone; read() will say so */ }
        }
        break;
    default: break;   /* colours (m), modes (h l), and the rest: dropped */
    }
}

static void feed(const unsigned char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = b[i];
        switch (state) {
        case 1:
            if (c == '[') { state = 2; npar = 0; memset(par, 0, sizeof par); }
            else if (c == ']') state = 3;
            else state = 0;   /* a two-byte sequence: dropped */
            continue;
        case 2:
            if (c >= '0' && c <= '9') { if (npar == 0) npar = 1; par[npar - 1] = par[npar - 1] * 10 + (c - '0'); }
            else if (c == ';') { if (npar == 0) npar = 1; if (npar < 8) npar++; }
            else if (c >= 0x40 && c <= 0x7e) { csi(c); state = 0; }
            continue;   /* '?' and other intermediates: ignored */
        case 3:
            if (c == 7) state = 0; else if (c == 033) state = 4;
            continue;
        case 4:
            state = c == '\\' ? 0 : 3;
            continue;
        }
        if (c == 033) { state = 1; continue; }
        if (c == '\r') { cx = 0; continue; }
        if (c == '\n' || c == 013 || c == 014) { line_feed(); continue; }
        if (c == '\b') { if (cx > 0) cx--; continue; }
        if (c == '\t') { cx = (cx + 8) & ~7u; if (cx >= cols) cx = cols - 1; continue; }
        if (c < 0x20 || c == 0x7f) continue;
        if (c >= 0x80 && c < 0xc0) continue;   /* UTF-8 continuation: the lead byte stood for it */
        if (c >= 0xc0) c = '?';
        if (cx >= cols) { cx = 0; line_feed(); }
        text[cy * cols + cx] = c;
        draw_cell(cx, cy, 0);
        cx++;
    }
}

static long now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000L + t.tv_nsec / 1000000;
}

int main(int argc, char **argv) {
    const char *fontpath = "/usr/share/glass/console.psf";
    int i = 1;
    for (; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) scale = (unsigned) atoi(argv[++i]);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) fontpath = argv[++i];
        else if (!strcmp(argv[i], "--")) { i++; break; }
        else break;
    }
    char *shell[] = { "/bin/sh", "-l", NULL };
    char **cmd = i < argc ? argv + i : shell;
    if (scale < 1 || scale > 4) scale = 2;
    if (load_font(fontpath)) return 1;

    int fb = open("/dev/fb0", O_RDWR);
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (fb < 0 || ioctl(fb, FBIOGET_VSCREENINFO, &var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix)) { perror("glass-console: /dev/fb0"); return 1; }
    if (var.bits_per_pixel != 32) { fprintf(stderr, "glass-console: /dev/fb0 is %u bits a pixel, 32 expected\n", var.bits_per_pixel); return 1; }
    xres = var.xres; yres = var.yres; stride = fix.line_length / 4;
    fbm = mmap(NULL, (size_t) fix.line_length * yres, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (fbm == MAP_FAILED) { perror("glass-console: mmap"); return 1; }
    cw = fw * scale; ch = fh * scale;
    cols = xres / cw; rows = yres / ch;
    if (cols < 2 || rows < 2) { fprintf(stderr, "glass-console: %ux%u cells do not fit %ux%u\n", cw, ch, xres, yres); return 1; }
    x0 = (xres - cols * cw) / 2; y0 = (yres - rows * ch) / 2;
    text = malloc((size_t) rows * cols);
    memset(text, ' ', (size_t) rows * cols);
    for (unsigned y = 0; y < yres; y++)
        for (unsigned x = 0; x < xres; x++) fbm[(size_t) y * stride + x] = bg;

    /* Page 0 on screen: the driver's own offset (it re-applies it when the
     * panel is switched on again) and the controller's registers. */
    var.xoffset = 0; var.yoffset = 0;
    ioctl(fb, FBIOPAN_DISPLAY, &var);
    int memfd = open("/dev/mem", O_RDWR | O_SYNC);
    volatile uint32_t *dispc = NULL;
    if (memfd >= 0) {
        void *p = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, memfd, DISPC_BASE);
        if (p != MAP_FAILED) dispc = p;
    }
    if (dispc) {
        uint32_t was = dispc[DISPC_GFX_BA0 / 4];
        dispc[DISPC_GFX_BA0 / 4] = (uint32_t) fix.smem_start;
        dispc[DISPC_GFX_BA1 / 4] = (uint32_t) fix.smem_start;
        dispc[DISPC_CONTROL2 / 4] |= DISPC_GO_LCD2;
        fprintf(stderr, "glass-console: graphics layer at 0x%08x (was 0x%08x), %ux%u cells of %ux%u\n",
                dispc[DISPC_GFX_BA0 / 4], was, cols, rows, cw, ch);
    } else {
        fprintf(stderr, "glass-console: no /dev/mem: page 0 by pan only, %ux%u cells\n", cols, rows);
    }

    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) || unlockpt(master)) { perror("glass-console: pty"); return 1; }
    struct winsize ws = { .ws_row = rows, .ws_col = cols, .ws_xpixel = xres, .ws_ypixel = yres };
    ioctl(master, TIOCSWINSZ, &ws);
    const char *slave = ptsname(master);
    pid_t pid = fork();
    if (pid < 0) { perror("glass-console: fork"); return 1; }
    if (pid == 0) {
        setsid();
        int s = open(slave, O_RDWR);
        if (s < 0) _exit(127);
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        if (s > 2) close(s);
        close(master); close(fb);
        if (memfd >= 0) close(memfd);
        setenv("TERM", "vt100", 1);
        if (!getenv("HOME")) setenv("HOME", "/root", 1);
        chdir(getenv("HOME"));
        execvp(cmd[0], cmd);
        _exit(127);
    }

    long next_blink = now_ms() + BLINK_MS;
    int phase = 1;
    cursor(1);
    unsigned char buf[4096];
    for (;;) {
        long wait = next_blink - now_ms();
        struct pollfd pfd = { .fd = master, .events = POLLIN };
        int r = poll(&pfd, 1, wait > 0 ? (int) wait : 0);
        if (r < 0 && errno != EINTR) break;
        if (r > 0) {
            ssize_t n = read(master, buf, sizeof buf);
            if (n <= 0) break;   /* EIO: the shell ended */
            cursor(0);
            feed(buf, (size_t) n);
            cursor(1);
            phase = 1;
            next_blink = now_ms() + BLINK_MS;
            continue;
        }
        if (now_ms() >= next_blink) {
            phase = !phase;
            cursor(phase);
            next_blink += BLINK_MS;
        }
    }
    int st = 0;
    waitpid(pid, &st, 0);
    fprintf(stderr, "glass-console: the shell ended (status %d)\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    return 0;
}
