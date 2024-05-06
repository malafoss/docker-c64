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

// run the emulator and render-loop at 30fps
#define FRAME_USEC (33333)
// border size
#define BORDER_HORI (5)
#define BORDER_VERT (3)
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
    const char *filename = "file.prg";
    return load_file_name(filename);
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
    const char *filename = "file.prg";
    uint16_t ptr = 0x0801;
    uint16_t endptr = mem_rd16(&c64.mem_cpu, 0x002d);
    return save_file_name(filename, ptr, endptr);
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
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
    noecho();
    curs_set(FALSE);
    cbreak();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    attron(A_BOLD);

    // set upper case font
    bool isLower = false;

    // run the emulation/input/render loop
    while (!quit_requested) {
        struct timespec frame_start_time;
        clock_gettime(CLOCK_MONOTONIC_RAW, &frame_start_time);

        // tick the emulator for 1 frame
        c64_exec(&c64, FRAME_USEC);

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
        // render the PETSCII buffer
        int cur_color_pair = -1;
        int bg = c64.vic.gunit.bg[0] & 0xF;
        int bc = c64.vic.brd.bc & 0xF;
        for (uint32_t yy = 0; yy < 25+2*BORDER_VERT; yy++) {
            for (uint32_t xx = 0; xx < 40+2*BORDER_HORI; xx++) {
                if ((xx < BORDER_HORI) || (xx >= 40+BORDER_HORI) ||
                    (yy < BORDER_VERT) || (yy >= 25+BORDER_VERT))
                {
                    // border area
                    int color_pair = bc+1;
                    if (color_pair != cur_color_pair) {
                        attron(COLOR_PAIR(color_pair));
                        cur_color_pair = color_pair;
                    }
                    CLEAR_POS(xx,yy);
                }
                else {
                    // bitmap area (not border)
                    int x = xx - BORDER_HORI;
                    int y = yy - BORDER_VERT;

                    // get color byte (only lower 4 bits wired)
                    int fg = c64.color_ram[y*40+x] & 15;
                    int color_pair = (fg*16+bg)+1;
                    if (color_pair != cur_color_pair) {
                        attron(COLOR_PAIR(color_pair));
                        cur_color_pair = color_pair;
                    }

                    // get character index
                    uint16_t addr = 0x0400 + y*40 + x;
                    uint8_t font_code = mem_rd(&c64.mem_vic, addr);
                    const char *chr = (isLower ? font_map_lower : font_map_upper)[font_code & 127];

                    // invert upper half of character set
                    if (font_code > 127) {
                        attron(A_REVERSE);
                    }

                    // unicode character
                    SET_POS_STR(xx, yy, chr);

                    // invert upper half of character set
                    if (font_code > 127) {
                        attroff(A_REVERSE);
                    }
                }
            }
        }
        refresh();

        struct timespec frame_end_time;
        clock_gettime(CLOCK_MONOTONIC_RAW, &frame_end_time);

        // calculate consumed usecs
        long usecs = (frame_end_time.tv_nsec - frame_start_time.tv_nsec) / 1000;
        if (frame_end_time.tv_sec > frame_start_time.tv_sec)
            usecs += (frame_end_time.tv_sec - frame_start_time.tv_sec) * 1000000;

        // sleep until next frame
        long usecs_left = FRAME_USEC - usecs;
        if (usecs_left > 0) usleep(usecs_left);
    }
    endwin();
    return 0;
}
