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

static c64_t c64;
static const char *prg_filename = "file.prg";
static bool auto_run_file = false;
static bool file_loaded = false;

// Use existing VIC-II and C64 timing constants from headers
#define PAL_FRAME_USEC ((M6569_VTOTAL * M6569_HTOTAL * 1000000L) / C64_FREQUENCY)

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
static bool screen_dirty = true;  // Force full redraw initially
// character width - select bad aspect ratio 1 or bad graphics 2
#define CHAR_WIDTH 1

#if CHAR_WIDTH == 1
#define CLEAR_POS(x,y) mvaddch((y), (x), ' ');
#define SET_POS_STR(x,y,s) mvaddstr((y), (x), s);
#else
#define CLEAR_POS(x,y) mvaddch((y), (x)*CHAR_WIDTH, ' '); mvaddch((y), (x)*CHAR_WIDTH+1, ' ');
#define SET_POS_STR(x,y,s) mvaddch((y), (x)*CHAR_WIDTH+1, ' '); mvaddstr((y), (x)*CHAR_WIDTH, s);
#endif

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

// map C64 color numbers to xterm-256color colors
static const int colors[16] = {
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

static void init_c64_colors(void) {
    start_color();
    for (int fg = 0; fg < 16; fg++) {
        for (int bg = 0; bg < 16; bg++) {
            int cp = (fg*16 + bg) + 1;
            init_pair(cp, colors[fg], colors[bg]);
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

static bool screen_cells_equal(const screen_cell_t *a, const screen_cell_t *b) {
    return (a->chr == b->chr && 
            a->color_pair == b->color_pair && 
            a->reverse == b->reverse);
}

static void update_screen_cell(int x, int y, const char *chr, int color_pair, bool reverse) {
    if (x >= 0 && x < SCREEN_WIDTH && y >= 0 && y < SCREEN_HEIGHT) {
        screen_buffer[y][x].chr = chr;
        screen_buffer[y][x].color_pair = color_pair;
        screen_buffer[y][x].reverse = reverse;
    }
}

static void update_screen_line(int text_row, bool isLower) {
    // Update screen buffer for one character row (8 scanlines)
    int bg = c64.vic.gunit.bg[0] & 0xF;
    int border_color_pair = (c64.vic.brd.bc & 0xF) + 1;
    
    // Calculate screen row in our buffer (includes borders)
    int screen_row = text_row + BORDER_VERT;
    
    // Update the full width including borders
    for (uint32_t xx = 0; xx < SCREEN_WIDTH; xx++) {
        if ((xx < BORDER_HORI) || (xx >= C64_TEXT_COLS + BORDER_HORI)) {
            // border area
            update_screen_cell(xx, screen_row, NULL, border_color_pair, false);
        }
        else {
            // bitmap area (not border)
            int x = xx - BORDER_HORI;
            int y = text_row;

            // get color byte (only lower 4 bits wired)
            int fg = c64.color_ram[y*C64_TEXT_COLS+x] & 0xF;
            int color_pair = (fg*16+bg)+1;

            // get character index
            uint16_t screen_addr = (uint16_t)(c64.vic.reg.mem_ptrs & 0xf0) << 6;
            uint16_t addr = screen_addr + y*C64_TEXT_COLS + x;
            uint8_t font_code = mem_rd(&c64.mem_vic, addr);
            const char *chr = (isLower ? font_map_lower : font_map_upper)[font_code & 127];

            // check if character should be inverted
            bool reverse = (font_code > 127);
            
            // update screen buffer
            update_screen_cell(xx, screen_row, chr, color_pair, reverse);
        }
    }
}

static void update_border_lines(int start_screen_row, int end_screen_row) {
    // Update screen buffer for border lines
    int color_pair = (c64.vic.brd.bc & 0xF) + 1;
    
    for (int screen_row = start_screen_row; screen_row <= end_screen_row; screen_row++) {
        if (screen_row >= 0 && screen_row < SCREEN_HEIGHT) {
            for (uint32_t xx = 0; xx < SCREEN_WIDTH; xx++) {
                update_screen_cell(xx, screen_row, NULL, color_pair, false);
            }
        }
    }
}

static void flush_screen_changes(void) {
    for (int y = 0; y < SCREEN_HEIGHT; y++) {
        for (int x = 0; x < SCREEN_WIDTH; x++) {
            screen_cell_t *current = &screen_buffer[y][x];
            screen_cell_t *previous = &prev_buffer[y][x];
            
            // Only update if cell has changed or force redraw
            if (screen_dirty || !screen_cells_equal(current, previous)) {
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
                    SET_POS_STR(x, y, current->chr);
                } else {
                    CLEAR_POS(x, y);
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
    
    screen_dirty = false;
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

static void execute_frame_by_scanlines(bool isLower) {
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
            update_screen_line(text_row, isLower);
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
    printf("  -h, --help    Show this help message\n");
    printf("\n");
    printf("Arguments:\n");
    printf("  filename      PRG file to auto-load and run (default: file.prg for manual loading)\n");
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
        else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Use -h or --help for usage information.\n");
            exit(1);
        }
        else {
            // First non-option argument is the filename - auto-run it
            prg_filename = argv[i];
            auto_run_file = true;
        }
    }
}

int main(int argc, char* argv[]) {
    parse_arguments(argc, argv);
    c64_init(&c64, &(c64_desc_t){
        .roms = {
            .chars = { .ptr=dump_c64_char_bin, .size=sizeof(dump_c64_char_bin) },
            .basic = { .ptr=dump_c64_basic_bin, .size=sizeof(dump_c64_basic_bin) },
            .kernal = { .ptr=dump_c64_kernalv3_bin, .size=sizeof(dump_c64_kernalv3_bin) }
        }
    });

    // install a Ctrl-C signal handler
    signal(SIGINT, catch_sigint);

    // UTF-8
    setlocale(LC_ALL, "C.utf8");

    // setup curses
    initscr();
    init_c64_colors();
    
    // Set terminal background to true black for xterm-256color
    assume_default_colors(231, 16);  // white on true black
    
    noecho();
    curs_set(FALSE);
    cbreak();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    attron(A_BOLD);

    // set upper case font
    bool isLower = false;

    // Initialize timing for 50.125 Hz frame rate
    struct timespec next_frame_time;
    clock_gettime(CLOCK_MONOTONIC_RAW, &next_frame_time);
    
    // run the emulation/input/render loop at exactly 50.125 Hz
    while (!quit_requested) {
        struct timespec frame_start_time;
        clock_gettime(CLOCK_MONOTONIC_RAW, &frame_start_time);

        // Execute one complete frame by scanlines with per-line buffer updates
        execute_frame_by_scanlines(isLower);
        
        // Auto-load and optionally run file when BASIC is ready
        if (auto_run_file && !file_loaded) {
            if (is_c64_basic_ready()) {
                if (load_file_name(prg_filename)) {
                    inject_run_command();
                }
                file_loaded = true;
            }
        }

        // keyboard input
        int ch = getch();
        if (ch != ERR) {
            switch (ch) {
                case 10:  ch = C64_KEY_RETURN; break; // ENTER
                case KEY_BACKSPACE: ch = C64_KEY_DEL; break;
                case 27:  ch = C64_KEY_STOP; break; // ESCAPE
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
                case KEY_END: isLower = !isLower; break;
                case KEY_NPAGE: ch = (load_file() ? 'l' : 'e'); break;
                case KEY_PPAGE: ch = (save_file() ? 's' : 'e'); break;
            }
            if (ch > 32) {
                if (islower(ch)) {
                    ch = toupper(ch);
                }
                else if (isupper(ch)) {
                    ch = tolower(ch);
                }
            }
            if (ch < 256) {
                c64_key_down(&c64, ch);
                c64_key_up(&c64, ch);
            }
        }
        
        // Flush all screen changes once per frame for optimal performance
        flush_screen_changes();
        refresh();

        // Calculate next frame time for 50.125 Hz (19968 microseconds per frame)
        next_frame_time.tv_nsec += PAL_FRAME_USEC * 1000;
        if (next_frame_time.tv_nsec >= 1000000000) {
            next_frame_time.tv_sec += 1;
            next_frame_time.tv_nsec -= 1000000000;
        }

        // Sleep until next frame to maintain real-time 50.125 Hz
        struct timespec current_time;
        clock_gettime(CLOCK_MONOTONIC_RAW, &current_time);
        
        if (current_time.tv_sec < next_frame_time.tv_sec || 
            (current_time.tv_sec == next_frame_time.tv_sec && current_time.tv_nsec < next_frame_time.tv_nsec)) {
            
            // Calculate sleep time in microseconds
            long sleep_usec = (next_frame_time.tv_sec - current_time.tv_sec) * 1000000;
            sleep_usec += (next_frame_time.tv_nsec - current_time.tv_nsec) / 1000;
            
            if (sleep_usec > 0) {
                usleep(sleep_usec);
            }
        }
    }
    endwin();
    return 0;
}
