/*
    c64.c

    Stripped down C64 emulator running in a (xterm-256color) terminal.
*/
#define _XOPEN_SOURCE_EXTENDED
#define _XOPEN_SOURCE
#define _POSIX_C_SOURCE 199309L
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <ncursesw/ncurses.h>
#include <unistd.h>
#include <ctype.h>
#include <signal.h>
#include <locale.h>
#include <wchar.h>
#include <time.h>
#include <string.h>
#define CHIPS_IMPL
#include "chips_common.h"
#include "m6502.h"
#include "m6526.h"
#include "m6569.h"
#include "m6581.h"
#include "beeper.h"
#include "kbd.h"
#include "mem.h"
#include "clk.h"
#include "c1530.h"
#include "m6522.h"
#include "c1541.h"
#include "c64.h"
#include "c64-roms.h"
#include "sixel.h"

static c64_t c64;
static const char *prg_filename = "file.prg";
static bool auto_run_file = false;
static bool auto_tape_file = false;
static bool auto_disk_file = false;
static bool file_loaded = false;
static int char_width = 1;  // 1 = narrow (default), 2 = wide

static gfx_mode_t  gfx_mode  = GFXMODE_AUTO;
static gfx_state_t gfx_state;

// Use existing VIC-II and C64 timing constants from headers
#define PAL_FRAME_USEC ((M6569_VTOTAL * M6569_HTOTAL * 1000000L) / C64_FREQUENCY)

// Clock utility functions
static inline void clock_get_time(struct timespec *ts) {
    clock_gettime(CLOCK_MONOTONIC_RAW, ts);
}

static inline void clock_add_microseconds(struct timespec *ts, long microseconds) {
    ts->tv_nsec += microseconds * 1000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += ts->tv_nsec / 1000000000L;
        ts->tv_nsec %= 1000000000L;
    }
}

static inline long clock_diff_microseconds(const struct timespec *end, const struct timespec *start) {
    return (end->tv_sec - start->tv_sec) * 1000000L + 
           (end->tv_nsec - start->tv_nsec) / 1000L;
}

static inline bool clock_is_before(const struct timespec *current, const struct timespec *target) {
    return (current->tv_sec < target->tv_sec) || 
           (current->tv_sec == target->tv_sec && current->tv_nsec < target->tv_nsec);
}


// VIC-II scanline ranges using official constants
#define PAL_TOP_BORDER_START (0)
#define PAL_TOP_BORDER_END (_M6569_RSEL1_BORDER_TOP - 1)        // 50
#define PAL_VISIBLE_START (_M6569_RSEL1_BORDER_TOP)             // 51  
#define PAL_VISIBLE_END (_M6569_RSEL1_BORDER_BOTTOM - 1)        // 250
#define PAL_BOTTOM_BORDER_START (_M6569_RSEL1_BORDER_BOTTOM)    // 251
#define PAL_BOTTOM_BORDER_END (M6569_VTOTAL - 1)                // 311

#define SCANLINES_PER_CHAR_ROW (8)      // 8 scanlines per character row
#define BORDER_CHUNK_SIZE (8)           // Process border in 8-line chunks
// border size
#define BORDER_HORI (5)
#define BORDER_VERT (3)

// Screen buffer for double buffering and dirty checking
typedef struct {
    const char *chr;        // Unicode character string
    int color_pair;         // ncurses color pair
    bool reverse;           // reverse video flag
} screen_cell_t;

#define C64_TEXT_COLS (40)
#define C64_TEXT_ROWS (25)
#define SCREEN_WIDTH (C64_TEXT_COLS + 2*BORDER_HORI)
#define SCREEN_HEIGHT (C64_TEXT_ROWS + 2*BORDER_VERT)

static screen_cell_t screen_buffer[SCREEN_HEIGHT][SCREEN_WIDTH];
static screen_cell_t prev_buffer[SCREEN_HEIGHT][SCREEN_WIDTH];

// Dynamic character width functions
static inline void clear_pos(int x, int y) {
    if (char_width == 1) {
        mvaddch(y, x, ' ');
    } else {
        mvaddch(y, x * 2, ' ');
        mvaddch(y, x * 2 + 1, ' ');
    }
}

static inline void set_pos_str(int x, int y, const char *s) {
    if (char_width == 1) {
        mvaddstr(y, x, s);
    } else {
        mvaddch(y, x * 2 + 1, ' ');
        mvaddstr(y, x * 2, s);
    }
}

// a signal handler for Ctrl-C, for proper cleanup
static int quit_requested = 0;
static void catch_sigint(int signo) {
    (void)signo;
    quit_requested = 1;
}

// conversion table from C64 font index to UTF-8
static const char *font_map_upper[] = {
    "@", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", // 0
    "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "[", "£", "]", "↑", "←", // 16
    " ", "!", "\"", "#", "$", "%", "&", "´", "(", ")", "*", "+", ",", "-", ".", "/", // 32
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", ";", "<", "=", ">", "?", // 48
    "🭸", "♠", "🭳", "🭸", "🭸", "🭷", "🭺", "🭲", "🭴", "╮", "╰", "╯", "🭼", "╲", "╱", "🭽", // 64
    "🭾", "●", "🭻", "♥", "┃", "╭", "╳", "○", "♣", "┃", "♦", "╋", "🮌", "┃", "π", "◥", // 80
    " ", "▌", "▄", "▔", "▁", "▏", "🮐", "🭵", "🮏", "◤", "🭵", "┣", "▗", "┗", "┓", "▁", // 96
    "┏", "┻", "┳", "┫", "▏", "▍", "🮈", "🮃", "▀", "▃", "🭿", "▖", "▝", "┛", "▘", "▚", // 112
};
static const char *font_map_lower[] = {
    "@", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", // 0
    "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "[", "£", "]", "↑", "←", // 16
    " ", "!", "\"", "#", "$", "%", "&", "´", "(", ")", "*", "+", ",", "-", ".", "/", // 32
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", ";", "<", "=", ">", "?", // 48
    "🭸", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", // 64
    "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "╋", "🮌", "┃", "π", "◥", // 80
    " ", "▌", "▄", "▔", "▁", "▏", "🮐", "🭵", "🮏", "◤", "🭵", "┣", "▗", "┗", "┓", "▁", // 96
    "┏", "┻", "┳", "┫", "▏", "▍", "🮈", "🮃", "▀", "▃", "🭿", "▖", "▝", "┛", "▘", "▚", // 112
};

// C64 color palette — Colodore (https://www.colodore.com), matches sixel.h _gfx_pal_*
static const struct {
    uint8_t r, g, b;
} c64_colors[16] = {
    {0x00, 0x00, 0x00},  // 0:  black
    {0xff, 0xff, 0xff},  // 1:  white
    {0x81, 0x33, 0x38},  // 2:  red
    {0x75, 0xce, 0xc8},  // 3:  cyan
    {0x8e, 0x3c, 0x97},  // 4:  purple
    {0x56, 0xac, 0x4d},  // 5:  green
    {0x2e, 0x2c, 0x9b},  // 6:  blue
    {0xed, 0xf1, 0x71},  // 7:  yellow
    {0x8e, 0x50, 0x29},  // 8:  orange
    {0x55, 0x38, 0x00},  // 9:  brown
    {0xc4, 0x6c, 0x71},  // 10: light red
    {0x4a, 0x4a, 0x4a},  // 11: dark grey
    {0x7b, 0x7b, 0x7b},  // 12: grey
    {0xa9, 0xff, 0x9f},  // 13: light green
    {0x70, 0x6d, 0xeb},  // 14: light blue
    {0xb2, 0xb2, 0xb2},  // 15: light grey
};

static void init_c64_colors(void) {
    start_color();

    // Check if terminal supports color changes
    if (can_change_color()) {
        // Define exact C64 colors using RGB values (0-1000 scale for ncurses)
        for (int i = 0; i < 16; i++) {
            init_color(i + 16, // Use colors 16-31 to avoid conflicts
                      (c64_colors[i].r * 1000) / 255,
                      (c64_colors[i].g * 1000) / 255,
                      (c64_colors[i].b * 1000) / 255);
        }

        // Create color pairs using our custom colors
        for (int fg = 0; fg < 16; fg++) {
            for (int bg = 0; bg < 16; bg++) {
                int cp = (fg*16 + bg) + 1;
                init_pair(cp, fg + 16, bg + 16);
            }
        }
    } else {
        // Fallback to xterm-256color mapping
        static const int color_fallback[16] = {
            16,     // black
            231,    // white
            88,     // red
            73,     // cyan
            54,     // purple
            71,     // green
            18,     // blue
            185,    // yellow
            136,    // orange
            58,     // brown
            131,    // light-red
            59,     // dark-grey
            102,    // grey
            150,    // light green
            62,     // light blue
            145,    // light grey
        };

        for (int fg = 0; fg < 16; fg++) {
            for (int bg = 0; bg < 16; bg++) {
                int cp = (fg*16 + bg) + 1;
                init_pair(cp, color_fallback[fg], color_fallback[bg]);
            }
        }
    }
}

static bool load_file_name(const char *filename) {
    // map file to memory
    int fd = open(filename, O_RDONLY);
    if (fd == -1) return false;
    struct stat sb;
    if (fstat(fd, &sb) == -1) { close(fd); return false; }
    off_t size = sb.st_size;
    void *ptr = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (ptr == MAP_FAILED) { close(fd); return false; }

    // load from memory
    bool rc = c64_quickload(&c64, (chips_range_t){
        .size = size,
        .ptr = ptr
    });

    // cleanup
    munmap(ptr, size);
    close(fd);
    return rc;
}

/*
    D64 fast-loader: parse a .d64 disk image, extract the first PRG file,
    and inject it directly via c64_quickload().

    D64 format: 35 tracks, variable sectors per track, 256 bytes per sector.
    Track 18 sector 0 = BAM, sector 1 = first directory sector.
    Each directory sector holds 8 entries of 32 bytes each.
    Entry byte 2 = file type (0x82 = closed PRG), bytes 3-4 = first track/sector.
    Sector link: byte 0 = next track (0 = last sector), byte 1 = next sector
    (or, when track=0, last byte offset used: data runs offsets 2..byte1 inclusive).
*/
static const int _d64_spt[35] = {
    21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21,21, /* tracks 1-17 */
    19,19,19,19,19,19,19,                                /* tracks 18-24 */
    18,18,18,18,18,18,                                   /* tracks 25-30 */
    17,17,17,17,17                                       /* tracks 31-35 */
};

static const uint8_t *_d64_sector(const uint8_t *d64, int track, int sector) {
    int off = 0;
    for (int t = 1; t < track; t++) off += _d64_spt[t - 1];
    return d64 + (off + sector) * 256;
}

static bool load_d64_file(const char *filename) {
    int fd = open(filename, O_RDONLY);
    if (fd == -1) return false;
    struct stat sb;
    if (fstat(fd, &sb) == -1) { close(fd); return false; }
    void *ptr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (ptr == MAP_FAILED) return false;

    const uint8_t *d64 = (const uint8_t *)ptr;
    bool ok = false;

    if (sb.st_size < 174848) goto done;

    /* scan directory for first PRG entry */
    int dt = 18, ds = 1;
    int first_t = 0, first_s = 0;
    while (dt && !first_t) {
        const uint8_t *dir = _d64_sector(d64, dt, ds);
        for (int e = 0; e < 8 && !first_t; e++) {
            const uint8_t *ent = dir + e * 32;
            if ((ent[2] & 0x07) == 0x02) { /* PRG */
                first_t = ent[3];
                first_s = ent[4];
            }
        }
        dt = dir[0];
        ds = dir[1];
    }
    if (!first_t) goto done;

    /* follow sector chain into buffer */
    static uint8_t prg[65536];
    int len = 0;
    int t = first_t, s = first_s;
    while (t && len < (int)sizeof(prg)) {
        const uint8_t *sec = _d64_sector(d64, t, s);
        int nt = sec[0], ns = sec[1];
        /* when nt==0: data runs from offset 2 to ns inclusive */
        int nb = nt ? 254 : (ns - 1);
        if (nb < 0) nb = 0;
        if (len + nb > (int)sizeof(prg)) nb = (int)sizeof(prg) - len;
        memcpy(prg + len, sec + 2, nb);
        len += nb;
        t = nt; s = ns;
    }

    if (len >= 2)
        ok = c64_quickload(&c64, (chips_range_t){ .ptr = prg, .size = len });

done:
    munmap(ptr, sb.st_size);
    return ok;
}

static bool load_file(void) {
    return load_file_name(prg_filename);
}

static bool save_file_name(const char *filename, uint16_t ptr, uint16_t endptr) {
    // open or create file
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd == -1) return false;

    // write header
    uint8_t lobyte = (uint8_t)ptr;
    uint8_t hibyte = (uint8_t)(ptr>>8);
    if (write(fd, &lobyte, 1) == -1 || write(fd, &hibyte, 1) == -1) { close(fd); return false; }

    // write data
    while (ptr < endptr) {
        uint8_t byte = mem_rd(&c64.mem_cpu, ptr++);
        if (write(fd, &byte, 1) == -1) { close(fd); return false; }
    }

    // cleanup
    return close(fd) == 0;
}

static bool save_file(void) {
    uint16_t ptr = 0x0801;
    uint16_t endptr = mem_rd16(&c64.mem_cpu, 0x002d);
    return save_file_name(prg_filename, ptr, endptr);
}

static void inject_run_command(void) {
    c64_basic_run(&c64);
}

static inline bool screen_cells_equal(const screen_cell_t *a, const screen_cell_t *b) {
    return (a->chr == b->chr && 
            a->color_pair == b->color_pair && 
            a->reverse == b->reverse);
}


// Check if VIC-II is using lowercase character set (hardware-accurate)
static inline bool is_lowercase_charset(void) {
    // VIC-II $D018 bits 1-3: 3 = lowercase/uppercase charset, others = uppercase/graphics
    return ((c64.vic.reg.mem_ptrs & 0x0E) >> 1) == 3;
}

// Toggle character set (simulates SHIFT+CBM)
static void toggle_charset(void) {
    uint8_t mem_ptrs = c64.vic.reg.mem_ptrs;
    if (is_lowercase_charset()) {
        // Switch to uppercase/graphics
        mem_ptrs = (mem_ptrs & 0xF1) | 0x04; // bits 1-3 = 010
    } else {
        // Switch to lowercase/uppercase
        mem_ptrs = (mem_ptrs & 0xF1) | 0x06; // bits 1-3 = 011
    }
    c64.vic.reg.mem_ptrs = mem_ptrs;
    _m6569_io_update_memory_unit(&c64.vic.mem, mem_ptrs, c64.vic.reg.ctrl_1);
}

static void update_screen_line(int text_row) {
    const int bg = c64.vic.gunit.bg[0] & 0xF;
    const int border_color_pair = (c64.vic.brd.bc & 0xF) + 1;
    const int screen_row = text_row + BORDER_VERT;
    
    // Pre-compute all invariants
    const uint16_t screen_addr = (uint16_t)(c64.vic.reg.mem_ptrs & 0xf0) << 6;
    const uint16_t base_addr = (screen_addr + text_row * C64_TEXT_COLS) | c64.vic_bank_select;
    const char * const * const font_map = is_lowercase_charset() ? font_map_lower : font_map_upper;
    const uint16_t color_addr = (screen_addr + text_row * C64_TEXT_COLS) & 0x03FF;
    const uint8_t * const color_base = &c64.color_ram[color_addr];
    
    screen_cell_t * const row = &screen_buffer[screen_row][0];
    
    // Left border - direct array access
    for (uint32_t x = 0; x < BORDER_HORI; x++) {
        row[x] = (screen_cell_t){NULL, border_color_pair, false};
    }
    
    // Character area - optimized loop with direct memory access
    for (uint32_t x = 0; x < C64_TEXT_COLS; x++) {
        const uint32_t screen_x = x + BORDER_HORI;
        const int fg = color_base[x] & 0xF;
        const uint8_t font_code = mem_rd(&c64.mem_vic, base_addr + x);
        
        row[screen_x] = (screen_cell_t){
            .chr = font_map[font_code & 127],
            .color_pair = (fg << 4) + bg + 1,
            .reverse = (font_code > 127)
        };
    }
    
    // Right border - direct array access
    const screen_cell_t border_cell = {NULL, border_color_pair, false};
    for (uint32_t x = C64_TEXT_COLS + BORDER_HORI; x < SCREEN_WIDTH; x++) {
        row[x] = border_cell;
    }
}

static void update_border_lines(int start_screen_row, int end_screen_row) {
    const int color_pair = (c64.vic.brd.bc & 0xF) + 1;
    const screen_cell_t border_cell = {NULL, color_pair, false};
    
    for (int screen_row = start_screen_row; screen_row <= end_screen_row; screen_row++) {
        screen_cell_t * const row = &screen_buffer[screen_row][0];
        for (uint32_t x = 0; x < SCREEN_WIDTH; x++) {
            row[x] = border_cell;
        }
    }
}

static void flush_screen_changes(void) {
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        const screen_cell_t * const current_row = &screen_buffer[y][0];
        screen_cell_t * const prev_row = &prev_buffer[y][0];
        
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            const screen_cell_t * const current = &current_row[x];
            screen_cell_t * const previous = &prev_row[x];
            
            if (!screen_cells_equal(current, previous)) {
                // Set color
                if (current->color_pair != -1) {
                    attron(COLOR_PAIR(current->color_pair));
                }
                
                // Set reverse video
                if (current->reverse) {
                    attron(A_REVERSE);
                }
                
                // Draw character
                if (current->chr) {
                    set_pos_str(x, y, current->chr);
                } else {
                    clear_pos(x, y);
                }
                
                // Restore attributes
                if (current->reverse) {
                    attroff(A_REVERSE);
                }
                
                // Copy to previous buffer
                *previous = *current;
            }
        }
    }
}

static void execute_scanlines(int start_line, int end_line) {
    // Calculate exact ticks for the specified scanline range to avoid rounding errors
    int num_lines = end_line - start_line + 1;
    uint32_t exact_ticks = num_lines * M6569_HTOTAL;
    
    // Convert back to microseconds that will give us exactly the right number of ticks
    // Using the same formula as clk_us_to_ticks but in reverse: ticks * 1000000 / freq
    uint32_t microseconds = (exact_ticks * 1000000L) / C64_FREQUENCY;
    
    // Execute emulator for this scanline range
    c64_exec(&c64, microseconds);
}

static void execute_frame_by_scanlines(void) {
    // 1. Execute top border/vblank area in chunks and update border colors
    for (int line = PAL_TOP_BORDER_START; line <= PAL_TOP_BORDER_END; line += BORDER_CHUNK_SIZE) {
        int end_line = (line + BORDER_CHUNK_SIZE - 1);
        if (end_line > PAL_TOP_BORDER_END) end_line = PAL_TOP_BORDER_END;
        
        execute_scanlines(line, end_line);
        
        // Update top border rows (map VIC scanlines to display rows 0 to BORDER_VERT-1)
        int display_row_start = (line * BORDER_VERT) / (PAL_TOP_BORDER_END + 1);
        int display_row_end = ((end_line + 1) * BORDER_VERT) / (PAL_TOP_BORDER_END + 1) - 1;
        if (display_row_end >= BORDER_VERT) display_row_end = BORDER_VERT - 1;
        if (display_row_start <= display_row_end) {
            update_border_lines(display_row_start, display_row_end);
        }
    }
    
    // 2. Execute visible area - one character row (8 scanlines) at a time
    int text_row = 0;
    for (int line = PAL_VISIBLE_START; line <= PAL_VISIBLE_END; line += SCANLINES_PER_CHAR_ROW) {
        int end_line = (line + SCANLINES_PER_CHAR_ROW - 1);
        if (end_line > PAL_VISIBLE_END) end_line = PAL_VISIBLE_END;
        
        // Execute scanlines for this character row
        execute_scanlines(line, end_line);
        
        // Immediately update screen buffer for this text row
        if (text_row < C64_TEXT_ROWS) {
            update_screen_line(text_row);
        }
        text_row++;
    }
    
    // 3. Execute bottom border/vblank area in chunks and update border colors
    for (int line = PAL_BOTTOM_BORDER_START; line <= PAL_BOTTOM_BORDER_END; line += BORDER_CHUNK_SIZE) {
        int end_line = (line + BORDER_CHUNK_SIZE - 1);
        if (end_line > PAL_BOTTOM_BORDER_END) end_line = PAL_BOTTOM_BORDER_END;
        
        execute_scanlines(line, end_line);
        
        // Update bottom border lines
        int border_start_row = C64_TEXT_ROWS + BORDER_VERT;
        int total_bottom_lines = PAL_BOTTOM_BORDER_END - PAL_BOTTOM_BORDER_START + 1;
        int bottom_display_lines = SCREEN_HEIGHT - border_start_row;
        
        int rel_line = line - PAL_BOTTOM_BORDER_START;
        int rel_end_line = end_line - PAL_BOTTOM_BORDER_START;
        
        int display_row_start = border_start_row + (rel_line * bottom_display_lines) / total_bottom_lines;
        int display_row_end = border_start_row + ((rel_end_line + 1) * bottom_display_lines) / total_bottom_lines - 1;
        if (display_row_end >= SCREEN_HEIGHT) display_row_end = SCREEN_HEIGHT - 1;
        if (display_row_start <= display_row_end) {
            update_border_lines(display_row_start, display_row_end);
        }
    }
}

static bool is_c64_basic_ready(void) {
    // Check multiple indicators that BASIC is ready for input
    
    // 1. Keyboard buffer should be empty (count at 0xC6 should be 0)
    uint8_t kbd_buffer_count = mem_rd(&c64.mem_cpu, 0xC6);
    if (kbd_buffer_count != 0) {
        return false;
    }
    
    // 2. BASIC program start should be initialized (0x801)
    uint16_t basic_start = mem_rd16(&c64.mem_cpu, 0x2B);
    if (basic_start != 0x0801) {
        return false;
    }
    
    // 3. Look for "READY." prompt on screen - this is the definitive indicator
    uint16_t screen_addr = (uint16_t)(c64.vic.reg.mem_ptrs & 0xf0) << 6;
    
    // Check for "READY." at beginning of a line (PETSCII codes)
    // R=18, E=5, A=1, D=4, Y=25, .=46
    for (int line = 0; line < C64_TEXT_ROWS; line++) {
        uint16_t line_addr = screen_addr + line * C64_TEXT_COLS;
        uint8_t char1 = mem_rd(&c64.mem_vic, line_addr + 0);
        uint8_t char2 = mem_rd(&c64.mem_vic, line_addr + 1);
        uint8_t char3 = mem_rd(&c64.mem_vic, line_addr + 2);
        uint8_t char4 = mem_rd(&c64.mem_vic, line_addr + 3);
        uint8_t char5 = mem_rd(&c64.mem_vic, line_addr + 4);
        uint8_t char6 = mem_rd(&c64.mem_vic, line_addr + 5);
        
        // Check for "READY." pattern
        if (char1 == 18 && char2 == 5 && char3 == 1 && 
            char4 == 4 && char5 == 25 && char6 == 46) {
            return true;
        }
    }
    
    return false;
}

static void print_usage(const char *program_name) {
    printf("Usage: %s [OPTIONS] [filename]\n", program_name);
    printf("\n");
    printf("A C64 emulator running in terminal with PETSCII graphics.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help         Show this help message\n");
    printf("  --mode=MODE        Output mode (default: auto)\n");
    printf("                       auto   detect kitty or sixel, fall back to text\n");
    printf("                       kitty  kitty graphics protocol\n");
    printf("                       sixel  sixel graphics protocol\n");
    printf("                       narrow text mode, narrow characters (1:1 aspect)\n");
    printf("                       wide   text mode, wide characters (2:1 aspect)\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  filename      File to auto-load and run:\n");
    printf("                  .prg  inject directly into memory\n");
    printf("                  .tap  load via datasette (slow, real-time)\n");
    printf("                  .d64  load first PRG from disk image\n");
    printf("                (default: file.prg for manual loading)\n");
    printf("\n");
    printf("Controls:\n");
    printf("  PageDown      Load file\n");
    printf("  PageUp        Save file\n");
    printf("  End           Toggle upper/lower case\n");
    printf("  Escape        RUN/STOP key\n");
    printf("  Ctrl+C        Exit emulator\n");
}

static void parse_arguments(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        }
        else if (gfx_parse_arg(argv[i], &gfx_mode, &char_width)) {
            // handled
        }
        else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Use -h or --help for usage information.\n");
            exit(1);
        }
        else {
            prg_filename = argv[i];
            const char *dot = strrchr(argv[i], '.');
            if (dot && (strcmp(dot, ".tap") == 0 || strcmp(dot, ".TAP") == 0))
                auto_tape_file = true;
            else if (dot && (strcmp(dot, ".d64") == 0 || strcmp(dot, ".D64") == 0))
                auto_disk_file = true;
            else
                auto_run_file = true;
        }
    }
}

int main(int argc, char* argv[]) {

    parse_arguments(argc, argv);
    c64_init(&c64, &(c64_desc_t){
        .c1530_enabled = true,
        .roms = {
            .chars = { .ptr=dump_c64_char_bin, .size=sizeof(dump_c64_char_bin) },
            .basic = { .ptr=dump_c64_basic_bin, .size=sizeof(dump_c64_basic_bin) },
            .kernal = { .ptr=dump_c64_kernalv3_bin, .size=sizeof(dump_c64_kernalv3_bin) }
        }
    });

    // install a Ctrl-C signal handler
    signal(SIGINT, catch_sigint);

    // Resolve auto-detection (gfx_detect handles raw mode internally)
    if (gfx_mode == GFXMODE_AUTO) {
        gfx_mode = gfx_detect();
    }

    setlocale(LC_ALL, "C.utf8");
    if (gfx_mode != GFXMODE_NONE) {
        // Graphics mode: open /dev/tty for ncurses so it has a real terminal
        // for input and terminal-mode setup, leaving stdout clean for graphics.
        FILE *tty = fopen("/dev/tty", "r+");
        newterm(NULL, tty ? tty : stderr, tty ? tty : stdin);
        noecho();
        curs_set(FALSE);
        cbreak();
        nodelay(stdscr, TRUE);
        keypad(stdscr, TRUE);
        // Hide cursor and clear screen on the real stdout
        write(STDOUT_FILENO, "\033[?25l\033[2J\033[H", 13);
        gfx_init(&gfx_state, gfx_mode);
    } else {
        // Text mode: full ncurses setup
        initscr();
        init_c64_colors();
        assume_default_colors(231, 16);  // white on true black
        noecho();
        curs_set(FALSE);
        cbreak();
        nodelay(stdscr, TRUE);
        keypad(stdscr, TRUE);
        attron(A_BOLD);
    }

    // Initialize screen buffers - previous buffer gets invalid values so first frame draws
    memset(prev_buffer, 0xFF, sizeof(prev_buffer));

    // Initialize timing for 50.125 Hz frame rate
    struct timespec next_frame_time;
    clock_get_time(&next_frame_time);

    // run the emulation/input/render loop at exactly 50.125 Hz
    while (!quit_requested) {
        struct timespec frame_start_time;
        clock_get_time(&frame_start_time);

        // Execute one complete frame by scanlines with per-line buffer updates
        execute_frame_by_scanlines();

        // Auto-load and optionally run file when BASIC is ready
        if (auto_run_file && !file_loaded) {
            if (is_c64_basic_ready()) {
                if (load_file_name(prg_filename)) {
                    inject_run_command();
                }
                file_loaded = true;
            }
        }
        if (auto_tape_file && !file_loaded) {
            if (is_c64_basic_ready()) {
                int fd = open(prg_filename, O_RDONLY);
                if (fd != -1) {
                    struct stat sb;
                    if (fstat(fd, &sb) == 0) {
                        void *ptr = mmap(NULL, sb.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
                        if (ptr != MAP_FAILED) {
                            if (c64_insert_tape(&c64, (chips_range_t){ .ptr=ptr, .size=sb.st_size })) {
                                c64_tape_play(&c64);
                                c64_basic_load(&c64);  /* injects LOAD + ENTER */
                            }
                            munmap(ptr, sb.st_size);
                        }
                    }
                    close(fd);
                }
                file_loaded = true;
            }
        }
        if (auto_disk_file && !file_loaded) {
            if (is_c64_basic_ready()) {
                if (load_d64_file(prg_filename))
                    inject_run_command();
                file_loaded = true;
            }
        }

        // Keyboard input — both modes use ncurses getch()
        int ch = getch();

        if (ch != ERR) {
            switch (ch) {
                case 10:  ch = C64_KEY_RETURN; break; // ENTER
                case 127: ch = C64_KEY_DEL; break;    // DEL (raw mode backspace)
                case KEY_BACKSPACE: ch = C64_KEY_DEL; break;
                case 27:  ch = C64_KEY_STOP; break; // ESC / RUN-STOP
                case KEY_LEFT: ch = C64_KEY_CSRLEFT; break;
                case KEY_RIGHT: ch = C64_KEY_CSRRIGHT; break;
                case KEY_UP: ch = C64_KEY_CSRUP; break;
                case KEY_DOWN: ch = C64_KEY_CSRDOWN; break;
                case KEY_IC: ch = C64_KEY_INST; break;
                case KEY_HOME: ch = C64_KEY_HOME; break;
                case KEY_DC: ch = C64_KEY_CLR; break;
                case KEY_STAB: ch = C64_KEY_RUN; break;
                case '|': ch = C64_KEY_LEFT; break;
                case KEY_F(1): ch = C64_KEY_F1; break;
                case KEY_F(2): ch = C64_KEY_F2; break;
                case KEY_F(3): ch = C64_KEY_F3; break;
                case KEY_F(4): ch = C64_KEY_F4; break;
                case KEY_F(5): ch = C64_KEY_F5; break;
                case KEY_F(6): ch = C64_KEY_F6; break;
                case KEY_F(7): ch = C64_KEY_F7; break;
                case KEY_F(8): ch = C64_KEY_F8; break;
                case KEY_END:
                    toggle_charset();
                    ch = -1; // Don't send to C64
                    break;
                case KEY_NPAGE:
                    ch = load_file() ? 'l' : 'e';
                    break;
                case KEY_PPAGE:
                    ch = save_file() ? 's' : 'e';
                    break;
                default:
                    // Handle case conversion for printable characters
                    if (ch > 32 && ch < 127) {
                        ch = islower(ch) ? toupper(ch) : (isupper(ch) ? tolower(ch) : ch);
                    }
                    break;
            }
            if (ch > 0 && ch < 256) {
                c64_key_down(&c64, ch);
                c64_key_up(&c64, ch);
            }
        }

        // Render
        if (gfx_mode != GFXMODE_NONE) {
            gfx_present(&gfx_state, c64.fb);
        } else {
            flush_screen_changes();
            refresh();
        }

        // Calculate next frame time for 50.125 Hz
        clock_add_microseconds(&next_frame_time, PAL_FRAME_USEC);

        // Sleep until next frame to maintain real-time 50.125 Hz
        struct timespec current_time;
        clock_get_time(&current_time);
        if (clock_is_before(&current_time, &next_frame_time)) {
            long sleep_usec = clock_diff_microseconds(&next_frame_time, &current_time);
            if (sleep_usec > 0) {
                usleep(sleep_usec);
            }
        }
    }

    // Cleanup
    if (gfx_mode != GFXMODE_NONE) {
        static const char reset[] =
            "\033[?25h"   /* restore cursor */
            "\033[2J"     /* clear screen   */
            "\033[H"      /* cursor home    */
            "\033[0m";    /* reset colors   */
        write(STDOUT_FILENO, reset, sizeof(reset) - 1);
    }
    endwin();
    return 0;
}
