#pragma once
/*
    sixel.h - Terminal graphics output for docker-c64.

    Supports sixel and kitty graphics protocols.
    Self-contained, no external library dependencies beyond standard POSIX.

    Framebuffer format: uint8_t* with palette-index values 0-15 (C64 colors).
    Framebuffer dimensions: GFX_FB_W x GFX_FB_H (504 x 312, full PAL frame).

    Rendering model:
      sixel — one DCS per frame covering the full 504×312 image.
              Single continuous DCS avoids inter-band character-cell gaps.
              Frame is skipped entirely if the framebuffer is unchanged.
      kitty — one APC per frame, full RGBA (504×312×4 = ~629 KB base64).
              Frame is skipped entirely if the framebuffer is unchanged.

    Known limitation: cursor is positioned with ESC[H (character-cell
    origin).  The pixel dimensions of the resulting image depend on the
    terminal's font cell size.  For most terminals the aspect ratio will
    differ from a real C64 monitor; this is a cosmetic issue only.
*/
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>

/*
    Framebuffer layout (matches _C64_SCREEN_* in c64.h):
      GFX_FB_W      rendered pixel width  (must match _C64_SCREEN_WIDTH)
      GFX_FB_H      rendered pixel height (must match _C64_SCREEN_HEIGHT)
      GFX_FB_STRIDE full row width of c64.fb (M6569_FRAMEBUFFER_WIDTH = 504)

    The VIC-II writes rendered pixels into c64.fb starting at column 0,
    row 0, but with a stride of 504 — bytes GFX_FB_W..503 in each row
    are never written and stay zero.
*/
#define GFX_FB_W      (384)   /* TV crop: 32px L border + 320px active + 32px R border */
#define GFX_FB_H      (270)   /* TV crop: 35 lines top + 200 active + 35 lines bottom  */
#define GFX_FB_STRIDE (504)   /* M6569_FRAMEBUFFER_WIDTH — row stride of c64.fb        */

/* Dirty-block tile dimensions */
#define GFX_BLK_W   (8)     /* 1 VIC timing cycle = 8 pixels wide */
#define GFX_BLK_H   (6)     /* 1 sixel band        = 6 pixels tall */
#define GFX_COLS    (GFX_FB_W / GFX_BLK_W)   /* 48 */
#define GFX_ROWS    (GFX_FB_H / GFX_BLK_H)   /* 45 */

typedef enum {
    GFXMODE_NONE  = 0,
    GFXMODE_SIXEL = 1,
    GFXMODE_KITTY = 2,
    GFXMODE_AUTO  = 3,
} gfx_mode_t;

typedef struct {
    gfx_mode_t mode;
    bool       first_frame;
    int        cell_h;    /* kitty: terminal character cell height in pixels, 0=unknown */
    uint8_t    prev_fb[GFX_FB_STRIDE * GFX_FB_H];  /* 504*270 = 136,080 bytes, stride-matched */
    char       out[1024 * 1024];                    /* 1 MB output buffer */
    int        out_len;
} gfx_state_t;

/*
    C64 Pepto palette — RGB components matching m6569.h _m6569_colors.
    Index matches C64 color register values 0-15.
*/
static const uint8_t _gfx_pal_r[16] = {
    0x00, 0xff, 0x81, 0x75, 0x8e, 0x56, 0x2e, 0xed,
    0x8e, 0x55, 0xc4, 0x4a, 0x7b, 0xa9, 0x70, 0xb2
};
static const uint8_t _gfx_pal_g[16] = {
    0x00, 0xff, 0x33, 0xce, 0x3c, 0xac, 0x2c, 0xf1,
    0x50, 0x38, 0x6c, 0x4a, 0x7b, 0xff, 0x6d, 0xb2
};
static const uint8_t _gfx_pal_b[16] = {
    0x00, 0xff, 0x38, 0xc8, 0x97, 0x4d, 0x9b, 0x71,
    0x29, 0x00, 0x71, 0x4a, 0x7b, 0x9f, 0xeb, 0xb2
};

/* ------------------------------------------------------------------ */
/* Output buffer helpers                                               */
/* ------------------------------------------------------------------ */

static inline void _gfx_flush(gfx_state_t *st) {
    if (st->out_len > 0) {
        write(STDOUT_FILENO, st->out, st->out_len);
        st->out_len = 0;
    }
}

static inline void _gfx_write(gfx_state_t *st, const char *data, int len) {
    if (st->out_len + len > (int)sizeof(st->out))
        _gfx_flush(st);
    memcpy(st->out + st->out_len, data, len);
    st->out_len += len;
}

static inline void _gfx_writec(gfx_state_t *st, char c) {
    if (st->out_len + 1 > (int)sizeof(st->out))
        _gfx_flush(st);
    st->out[st->out_len++] = c;
}

static void _gfx_printf(gfx_state_t *st, const char *fmt, ...) {
    char tmp[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    _gfx_write(st, tmp, n);
}

/* ------------------------------------------------------------------ */
/* Base64 encoder (for kitty protocol)                                */
/* ------------------------------------------------------------------ */

static const char _gfx_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int _gfx_b64_encode(const uint8_t *in, int len, char *out) {
    int i, n = 0;
    for (i = 0; i < len; i += 3) {
        uint32_t b  = (uint32_t)in[i] << 16;
        if (i + 1 < len) b |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) b |= (uint32_t)in[i + 2];
        out[n++] = _gfx_b64[(b >> 18) & 63];
        out[n++] = _gfx_b64[(b >> 12) & 63];
        out[n++] = (i + 1 < len) ? _gfx_b64[(b >>  6) & 63] : '=';
        out[n++] = (i + 2 < len) ? _gfx_b64[ b        & 63] : '=';
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Sixel partial-update emitter                                       */
/* ------------------------------------------------------------------ */
/*
    Emits one DCS sixel sequence starting at the terminal origin (ESC[H),
    using P2=1 (transparent background).

    Dirty bands (8×6 px blocks changed since prev frame) are emitted with
    full color data.  Clean bands are skipped with a single '-' (sixel band
    advance) — with P2=1 transparent, undrawn pixels in a clean band keep
    their previous screen content, so no redraw is needed.

    The DCS always starts from the top and stops after the last dirty band,
    avoiding any per-band cursor repositioning (which would reintroduce
    character-cell-height gaps).  Skipping N clean bands costs N bytes.

    On first frame, all bands are marked dirty (full redraw with P2=0 so
    the terminal background is fully covered rather than showing through).

    Typical savings:
      Static screen (BASIC prompt): 1–2 bands/frame → ~99% less data
      Sprite moving:               ~8 dirty bands → ~80% less data
      Full-screen demo effect:     all bands dirty → same as full frame
*/
static void _gfx_present_sixel(gfx_state_t *st, const uint8_t *fb,
                                bool first_frame) {
    /* Mark dirty bands by comparing rows against prev_fb */
    bool dirty[GFX_ROWS];
    int  last_dirty = -1;
    for (int by = 0; by < GFX_ROWS; by++) {
        if (first_frame) {
            dirty[by] = true;
        } else {
            dirty[by] = false;
            int py0 = by * GFX_BLK_H;
            for (int r = 0; r < GFX_BLK_H && !dirty[by]; r++) {
                if (memcmp(fb   + (py0 + r) * GFX_FB_STRIDE,
                           st->prev_fb + (py0 + r) * GFX_FB_STRIDE,
                           GFX_FB_W) != 0)
                    dirty[by] = true;
            }
        }
        if (dirty[by]) last_dirty = by;
    }
    if (last_dirty < 0) return;  /* nothing changed */

    /* Cursor to terminal origin */
    _gfx_write(st, "\033[H", 3);

    /* P2=1 for partial updates (transparent bg keeps clean bands intact).
       P2=0 on first frame to fully cover any terminal background. */
    if (first_frame)
        _gfx_write(st, "\033P0;0;0q", 8);
    else
        _gfx_write(st, "\033P0;1;0q", 8);

    /* Define all 16 palette colors once (RGB scaled to 0-100) */
    for (int ci = 0; ci < 16; ci++) {
        int r = (_gfx_pal_r[ci] * 100 + 127) / 255;
        int g = (_gfx_pal_g[ci] * 100 + 127) / 255;
        int b = (_gfx_pal_b[ci] * 100 + 127) / 255;
        _gfx_printf(st, "#%d;2;%d;%d;%d", ci, r, g, b);
    }

    /* Emit bands from 0 to last_dirty.
       Clean bands: just '-' (advance, transparent — no screen change).
       Dirty bands: full color-interlace sixel data. */
    for (int by = 0; by <= last_dirty; by++) {
        if (by > 0) _gfx_writec(st, '-');

        if (!dirty[by]) continue;  /* clean: '-' already emitted, nothing more */

        int py0 = by * GFX_BLK_H;

        /* Which colors appear in this band? */
        bool used[16] = {false};
        for (int r = 0; r < GFX_BLK_H; r++)
            for (int c = 0; c < GFX_FB_W; c++)
                used[fb[(py0 + r) * GFX_FB_STRIDE + c]] = true;

        /* One sixel pass per present color, $ (CR) between passes */
        bool first = true;
        for (int ci = 0; ci < 16; ci++) {
            if (!used[ci]) continue;
            if (!first) _gfx_writec(st, '$');
            first = false;
            _gfx_printf(st, "#%d", ci);
            for (int col = 0; col < GFX_FB_W; col++) {
                uint8_t bits = 0;
                for (int row = 0; row < GFX_BLK_H; row++) {
                    if (fb[(py0 + row) * GFX_FB_STRIDE + col] == (uint8_t)ci)
                        bits |= (uint8_t)(1u << row);
                }
                _gfx_writec(st, (char)(bits + 63));
            }
        }
    }

    /* End DCS (ST) */
    _gfx_write(st, "\033\\", 2);
}

/* ------------------------------------------------------------------ */
/* Kitty emitter helpers                                              */
/* ------------------------------------------------------------------ */

#define _GFX_KITTY_CHUNK_B64  (4096)
#define _GFX_KITTY_CHUNK_RAW  (_GFX_KITTY_CHUNK_B64 * 3 / 4)

/*
    Send one kitty image strip: pixel rows [py0, py0+strip_h) of the frame,
    placed at terminal character row cell_row (1-based).
    Uses chunked APC protocol so no single write exceeds 4096 b64 chars.
*/
static void _gfx_kitty_send_strip(gfx_state_t *st, const uint8_t *fb,
                                   int py0, int strip_h, int cell_row) {
    /* Strip RGBA buffer — max one cell height × full width */
    static uint8_t rgba[64 * GFX_FB_W * 4];   /* 64px max cell_h */
    char chunk_b64[_GFX_KITTY_CHUNK_B64 + 4];

    for (int r = 0; r < strip_h; r++) {
        for (int c = 0; c < GFX_FB_W; c++) {
            uint8_t idx = fb[(py0 + r) * GFX_FB_STRIDE + c];
            uint8_t *p  = &rgba[(r * GFX_FB_W + c) * 4];
            p[0] = _gfx_pal_r[idx];
            p[1] = _gfx_pal_g[idx];
            p[2] = _gfx_pal_b[idx];
            p[3] = 0xff;
        }
    }

    /* Move cursor to the target row, column 1 before the image.
       Kitty places the image at the current cursor position when no X=/Y=
       are specified — explicit cursor positioning is more reliable than
       X=/Y= keys, which interact with cursor movement after prior images. */
    _gfx_printf(st, "\033[%d;1H", cell_row);

    int total_raw = GFX_FB_W * strip_h * 4;
    int offset    = 0;
    bool first    = true;

    while (offset < total_raw) {
        int raw_len = total_raw - offset;
        if (raw_len > _GFX_KITTY_CHUNK_RAW) raw_len = _GFX_KITTY_CHUNK_RAW;
        int b64_len = _gfx_b64_encode(rgba + offset, raw_len, chunk_b64);
        int more    = (offset + raw_len < total_raw) ? 1 : 0;

        if (first) {
            _gfx_printf(st, "\033_Ga=T,f=32,t=d,s=%d,v=%d,q=2,m=%d;",
                        GFX_FB_W, strip_h, more);
            first = false;
        } else {
            _gfx_printf(st, "\033_Gm=%d;", more);
        }
        _gfx_write(st, chunk_b64, b64_len);
        _gfx_write(st, "\033\\", 2);
        _gfx_flush(st);
        offset += raw_len;
    }
}

/* ------------------------------------------------------------------ */
/* Kitty emitter — partial strip updates                              */
/* ------------------------------------------------------------------ */
/*
    When cell_h is known (queried via CSI 16 t at init):
      Divide the 270px image into character-cell-height strips.
      For each strip, compare rows against prev_fb.
      Send only dirty strips using Y=<cell_row> for exact placement.
      On first frame all strips are sent.

    When cell_h is 0 (query failed):
      Fall back to full-frame send (original behavior).

    Strip-based updates are most effective for:
      - Sprites moving on a static background (2-4 strips/frame)
      - Scrolling text (top/bottom strips only)
      - Static screens with cursor blink (1 strip/frame)
*/
static void _gfx_present_kitty(gfx_state_t *st, const uint8_t *fb,
                                bool first_frame) {
    if (st->cell_h <= 0) {
        /* No cell size known — full frame */
        _gfx_kitty_send_strip(st, fb, 0, GFX_FB_H, 1);
        return;
    }

    int cell_h   = st->cell_h;
    int n_strips = (GFX_FB_H + cell_h - 1) / cell_h;

    for (int s = 0; s < n_strips; s++) {
        int py0     = s * cell_h;
        int strip_h = cell_h;
        if (py0 + strip_h > GFX_FB_H) strip_h = GFX_FB_H - py0;

        /* Dirty check: compare every row of this strip */
        bool dirty = first_frame;
        if (!dirty) {
            for (int r = 0; r < strip_h && !dirty; r++) {
                if (memcmp(fb        + (py0 + r) * GFX_FB_STRIDE,
                           st->prev_fb + (py0 + r) * GFX_FB_STRIDE,
                           GFX_FB_W) != 0)
                    dirty = true;
            }
        }
        if (!dirty) continue;

        _gfx_kitty_send_strip(st, fb, py0, strip_h, s + 1);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

static const char *gfx_mode_name(gfx_mode_t m) {
    switch (m) {
        case GFXMODE_SIXEL: return "sixel";
        case GFXMODE_KITTY: return "kitty";
        case GFXMODE_AUTO:  return "auto";
        default:            return "none";
    }
}

/*
    Parse --mode=VALUE argument.
    Graphics values (auto/kitty/sixel/none) set *gfx_out.
    Text values (narrow/wide) set *char_width_out (1 or 2).
    Returns true on any recognised --mode= argument.
*/
static bool gfx_parse_arg(const char *arg, gfx_mode_t *gfx_out, int *char_width_out) {
    if (strncmp(arg, "--mode=", 7) != 0) return false;
    const char *val = arg + 7;
    if      (strcmp(val, "auto")   == 0) *gfx_out = GFXMODE_AUTO;
    else if (strcmp(val, "sixel")  == 0) *gfx_out = GFXMODE_SIXEL;
    else if (strcmp(val, "kitty")  == 0) *gfx_out = GFXMODE_KITTY;
    else if (strcmp(val, "narrow") == 0) { *gfx_out = GFXMODE_NONE; *char_width_out = 1; }
    else if (strcmp(val, "wide")   == 0) { *gfx_out = GFXMODE_NONE; *char_width_out = 2; }
    else return false;
    return true;
}

/*
    Probe the terminal for graphics capability.
    Temporarily switches stdin to raw/non-blocking for the probe read.

    Detection priority: kitty > sixel > none.

    Kitty probe:  ESC _ G a=q,i=31,s=1,v=1,t=d ; AAAA ESC \
      Response:   ESC _ G i=31 ; ok ESC \
    DA1 probe:    ESC [ c
      Response:   ESC [ ? <attrs> c   (attribute 4 = sixel support)

    Both probes are sent before reading so only one round-trip is needed.
    $TERM / $TERM_PROGRAM are intentionally NOT used — the Dockerfile sets
    TERM=xterm-256color unconditionally regardless of the host terminal.
*/
static gfx_mode_t gfx_detect(void) {
    /* Temporarily put stdin in raw non-blocking mode for the probe read */
    struct termios saved;
    bool saved_ok = (tcgetattr(STDIN_FILENO, &saved) == 0);
    if (saved_ok) {
        struct termios raw = saved;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    /* Send XTVERSION + DA1 probes back-to-back (one round-trip).
       XTVERSION (CSI > q): response is ESC P > | <name>(version) ESC \
         kitty responds: ESC P > | kitty(0.xx.x) ESC \
         xterm responds: ESC P > | XTerm(nnn) ESC \
       DA1 (CSI c): response is ESC [ ? <attrs> c
         attribute 4 = sixel support */
    static const char probe[] =
        "\033[>q"   /* XTVERSION */
        "\033[c";   /* DA1       */
    write(STDOUT_FILENO, probe, sizeof(probe) - 1);

    /* Collect terminal responses.
       Both probes are in-flight simultaneously.  We need to wait for both
       the DA1 response (fast) and the kitty a=q response (may lag by
       100-200ms through a double pty stack: kitty→podman→container).

       Strategy: loop with a hard 1s absolute deadline.  Break early only
       when the kitty response is confirmed (";ok") — never on DA1 alone,
       because the kitty response could still be in transit.  The deadline
       is the fallback for terminals that only send DA1 (xterm, etc.); those
       typically respond in <50ms so the 1s is rarely paid in full. */
    char buf[256];
    int  total = 0;
    {
        struct timeval deadline;
        gettimeofday(&deadline, NULL);
        deadline.tv_sec  += 1;   /* 1s absolute deadline */

        while (total < (int)sizeof(buf) - 1) {
            struct timeval now, tv;
            gettimeofday(&now, NULL);
            tv.tv_sec  = deadline.tv_sec  - now.tv_sec;
            tv.tv_usec = deadline.tv_usec - now.tv_usec;
            if (tv.tv_usec < 0) { tv.tv_sec--; tv.tv_usec += 1000000; }
            if (tv.tv_sec < 0)  break;  /* deadline expired */

            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) break;
            int n = (int)read(STDIN_FILENO, buf + total, sizeof(buf) - 1 - total);
            if (n <= 0) break;
            total += n;
            buf[total] = '\0';
            /* Early exit once we see "kitty" in XTVERSION response */
            if (strstr(buf, "kitty") || strstr(buf, "Kitty")) break;
        }
    }

    /* Drain any remaining bytes left in the pty buffer (e.g. the DA1 response
       arriving after an early kitty exit, or trailing bytes of any response).
       Without this drain those bytes get read by the emulation loop, fed to
       the C64 keyboard matrix, and fill the keyboard buffer — preventing
       is_c64_basic_ready() from seeing an empty buffer and triggering autoload. */
    {
        char junk[64];
        for (;;) {
            struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0) break;
            if (read(STDIN_FILENO, junk, sizeof(junk)) <= 0) break;
        }
    }

    if (saved_ok)
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);

    if (total <= 0) return GFXMODE_NONE;
    buf[total] = '\0';

    /* Kitty: XTVERSION response contains "kitty" (case-insensitive) */
    if (strstr(buf, "kitty") || strstr(buf, "Kitty") || strstr(buf, "KITTY"))
        return GFXMODE_KITTY;

    /* Sixel: DA1 response ESC [ ? <semicolon-separated numbers> c
       Attribute 4 signals sixel support. */
    char *da1 = strchr(buf, '?');
    if (da1) {
        char *p = da1 + 1;
        while (*p && *p != 'c') {
            long val = 0;
            while (*p >= '0' && *p <= '9') val = val * 10 + (*p++ - '0');
            if (val == 4) return GFXMODE_SIXEL;
            if (*p == ';') p++;
            else break;
        }
    }

    return GFXMODE_NONE;
}

/* Initialise graphics state.  Call once after mode is resolved.
   Must be called with stdin already in raw non-blocking mode so the
   CSI 16 t response can be read back. */
static void gfx_init(gfx_state_t *st, gfx_mode_t mode) {
    memset(st, 0, sizeof(*st));
    st->mode        = mode;
    st->first_frame = true;
    st->cell_h      = 0;

    if (mode == GFXMODE_KITTY) {
        /* Query terminal character cell size in pixels: CSI 16 t
           Response: CSI 6 ; <cell_h> ; <cell_w> t */
        write(STDOUT_FILENO, "\033[16t", 5);
        char buf[64];
        int total = 0;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 500000 };
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0)
            total = (int)read(STDIN_FILENO, buf, sizeof(buf) - 1);
        if (total > 0) {
            buf[total] = '\0';
            /* Parse \033[6;<h>;<w>t */
            char *p = strstr(buf, "[6;");
            if (p) {
                int h = 0;
                p += 3;
                while (*p >= '0' && *p <= '9') h = h * 10 + (*p++ - '0');
                if (h > 0 && h <= 64) st->cell_h = h;
            }
        }
    }
}

/*
    Render the framebuffer to the terminal.

    fb: pointer to GFX_FB_W * GFX_FB_H bytes, each a C64 color index 0-15.

    The entire frame is re-emitted as one image sequence whenever the
    framebuffer content changes.  Frames are skipped when nothing changed
    (important for static screens — saves the full re-encode cost).
*/
static void gfx_present(gfx_state_t *st, const uint8_t *fb) {
    if (st->mode == GFXMODE_SIXEL) {
        /* Sixel: partial band updates — dirty detection is inside the emitter */
        _gfx_present_sixel(st, fb, st->first_frame);
    } else {
        /* Kitty: partial strip updates when cell_h known, full frame otherwise.
           When all strips are clean, skip entirely. */
        if (!st->first_frame && st->cell_h > 0) {
            /* Quick full-frame check to skip entirely when nothing changed */
            bool any = false;
            for (int r = 0; r < GFX_FB_H && !any; r++)
                if (memcmp(fb + r * GFX_FB_STRIDE, st->prev_fb + r * GFX_FB_STRIDE, GFX_FB_W))
                    any = true;
            if (!any) return;
        } else if (!st->first_frame) {
            if (memcmp(fb, st->prev_fb, (size_t)GFX_FB_STRIDE * GFX_FB_H) == 0)
                return;
        }
        _gfx_present_kitty(st, fb, st->first_frame);
    }

    _gfx_flush(st);
    memcpy(st->prev_fb, fb, (size_t)GFX_FB_STRIDE * GFX_FB_H);
    st->first_frame = false;
}
