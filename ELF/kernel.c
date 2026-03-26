// kernel.c – ELF ładowany przez Twój bootloader
#include <stdint.h>
#include "font.h"
#include "interrupts.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

typedef struct {
    uint32_t *fb;
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_pitch;   // pixels per line
} exports_t;

static uint32_t *g_fb;
static uint32_t g_fb_width;
static uint32_t g_fb_height;
static uint32_t g_fb_pitch;

static int cursor_x = 0;
static int cursor_y = 0;
static const int CHAR_W = 8;
static const int CHAR_H = 16;

// ================ SCROLL ==================
static void fb_scroll(uint32_t bg) {
    uint32_t line_bytes = g_fb_pitch * sizeof(uint32_t) * CHAR_H;

    // przesuwamy pamięć o CHAR_H linii w górę
    uint8_t *dst = (uint8_t*)g_fb;
    uint8_t *src = (uint8_t*)g_fb + line_bytes;

    uint32_t total_bytes = (g_fb_height - CHAR_H) * g_fb_pitch * sizeof(uint32_t);

    // szybkie kopiowanie blokowe
    for (uint32_t i = 0; i < total_bytes; i++)
        dst[i] = src[i];

    // czyścimy ostatnie CHAR_H linii
    uint32_t start = (g_fb_height - CHAR_H) * g_fb_pitch;
    for (uint32_t y = start; y < g_fb_height * g_fb_pitch; y++)
        g_fb[y] = bg;

    cursor_y -= CHAR_H;
}

static void fb_put_char(int x, int y, char c, uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = font8x16[(unsigned char)c];
    for (int row = 0; row < CHAR_H; ++row) {
        uint8_t bits = glyph[row];
        uint32_t *dst = g_fb + (y + row) * g_fb_pitch + x;
        for (int col = 0; col < CHAR_W; ++col) {
            uint32_t color = (bits & (0x80 >> col)) ? fg : bg;
            dst[col] = color;
        }
    }
}

static void fb_put_char_at_cursor(char c, uint32_t fg, uint32_t bg) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y += CHAR_H;
    } else {
        fb_put_char(cursor_x, cursor_y, c, fg, bg);
        cursor_x += CHAR_W;
        if (cursor_x + CHAR_W > (int)g_fb_width) {
            cursor_x = 0;
            cursor_y += CHAR_H;
        }
    }

    if (cursor_y + CHAR_H > (int)g_fb_height) {
        fb_scroll(0x00000000);
    }

}

static void fb_puts(const char *s, uint32_t fg, uint32_t bg) {
    while (*s) {
        fb_put_char_at_cursor(*s++, fg, bg);
    }
}

// ==================== PS/2 SUROWE CZYTANIE SCANCODE Z PORTU OX60 (SET 1) ===================

static uint8_t ps2_read_scancode(void) {
    uint8_t status, sc;
    for (;;) {
        __asm__ volatile ("inb $0x64, %0" : "=a"(status));
        if (status & 0x01) {
            __asm__ volatile ("inb $0x60, %0" : "=a"(sc));
            return sc;
        }
    }
}

// ======================= MAPPING SCANCODE ==========================

static char scancode_to_ascii(uint8_t sc) {
    switch (sc) {
        case 0x02: return '1';
        case 0x03: return '2';
        case 0x04: return '3';
        case 0x05: return '4';
        case 0x06: return '5';
        case 0x07: return '6';
        case 0x08: return '7';
        case 0x09: return '8';
        case 0x0A: return '9';
        case 0x0B: return '0';
        case 0x10: return 'q';
        case 0x11: return 'w';
        case 0x12: return 'e';
        case 0x13: return 'r';
        case 0x14: return 't';
        case 0x15: return 'y';
        case 0x16: return 'u';
        case 0x17: return 'i';
        case 0x18: return 'o';
        case 0x19: return 'p';
        case 0x1E: return 'a';
        case 0x1F: return 's';
        case 0x20: return 'd';
        case 0x21: return 'f';
        case 0x22: return 'g';
        case 0x23: return 'h';
        case 0x24: return 'j';
        case 0x25: return 'k';
        case 0x26: return 'l';
        case 0x2C: return 'z';
        case 0x2D: return 'x';
        case 0x2E: return 'c';
        case 0x2F: return 'v';
        case 0x30: return 'b';
        case 0x31: return 'n';
        case 0x32: return 'm';
        case 0x39: return ' ';   // spacja
        case 0x1C: return '\n';  // Enter
        default:   return 0;
    }
}

// =========== STRUKTURA BUFORA =============

#define LINEBUF_MAX 256

typedef struct {
    char buf[LINEBUF_MAX];
    int len;        // ile znaków w linii
    int cursor;     // gdzie jest kursor
} linebuf_t;

static linebuf_t line;

// ============= PISANIE W ŚRODKU LINII ================

static void linebuf_insert(linebuf_t *l, char c) {
    if (l->len >= LINEBUF_MAX - 1)
        return;

    for (int i = l->len; i > l->cursor; --i)
        l->buf[i] = l->buf[i - 1];

    l->buf[l->cursor] = c;
    l->cursor++;
    l->len++;
}

// =============== BACKSPACE ==============

static void linebuf_backspace(linebuf_t *l) {
    if (l->cursor == 0)
        return;

    for (int i = l->cursor - 1; i < l->len - 1; ++i)
        l->buf[i] = l->buf[i + 1];

    l->cursor--;
    l->len--;
}

// ============= DELETE ==============

static void linebuf_delete(linebuf_t *l) {
    if (l->cursor >= l->len)
        return;

    for (int i = l->cursor; i < l->len - 1; ++i)
        l->buf[i] = l->buf[i + 1];

    l->len--;
}

// ================ PRZESUWANIE KURSORA ==================

static void linebuf_left(linebuf_t *l) {
    if (l->cursor > 0)
        l->cursor--;
}

static void linebuf_right(linebuf_t *l) {
    if (l->cursor < l->len)
        l->cursor++;
}

// ==================== KURSOR ===================

static int cursor_visible = 1;

static void fb_draw_cursor(uint32_t fg, uint32_t bg) {
    int x = cursor_x;
    int y = cursor_y;
    fb_put_char(x, y, (line.cursor < line.len) ? line.buf[line.cursor] : ' ', bg, fg);
}

// ================= RENDER LINII ===================

static void fb_render_line(const linebuf_t *l, uint32_t fg, uint32_t bg) {
    int y = cursor_y;

    // wyczyść linię
    for (int col = 0; col < g_fb_width / CHAR_W; ++col)
        fb_put_char(col * CHAR_W, y, ' ', fg, bg);

    // wypisz bufor
    for (int i = 0; i < l->len; ++i)
        fb_put_char(i * CHAR_W, y, l->buf[i], fg, bg);

    // ustaw kursor
    cursor_x = l->cursor * CHAR_W;

    // rysuj kursor
    fb_draw_cursor(fg, bg);

}

// ==================== OBSŁUGA STRZAŁEK ==================

static uint16_t ps2_read_key(void) {
    uint8_t sc = ps2_read_scancode();
    if (sc == 0xE0) {
        uint8_t sc2 = ps2_read_scancode();
        return 0xE000 | sc2; // specjalny kod
    }
    return sc;
}

// ==================== HISTORIA LINII ========================

#define HISTORY_MAX 32

static char history[HISTORY_MAX][LINEBUF_MAX];
static int history_len = 0;
static int history_pos = -1; // -1 = edytujesz bieżącą linię

// ==================== LOADER LINII Z HISTORII ===================

static void linebuf_load(linebuf_t *l, const char *src) {
    l->len = 0;
    l->cursor = 0;

    while (*src && l->len < LINEBUF_MAX - 1) {
        l->buf[l->len++] = *src++;
    }
    l->cursor = l->len;
}

// ==================== OBSŁUGA STRZAŁEK W LINE EDITORZE ====================

static void handle_arrow(uint16_t code) {
    if (code == 0xE04B) {        // LEFT
        linebuf_left(&line);
    } else if (code == 0xE04D) { // RIGHT
        linebuf_right(&line);
    } else if (code == 0xE048) { // UP (historia w górę)
        if (history_len > 0 && history_pos + 1 < history_len) {
            history_pos++;
            linebuf_load(&line, history[history_len - 1 - history_pos]);
        }
    } else if (code == 0xE050) { // DOWN (historia w dół)
        if (history_pos > 0) {
            history_pos--;
            linebuf_load(&line, history[history_len - 1 - history_pos]);
        } else if (history_pos == 0) {
            history_pos = -1;
            line.len = 0;
            line.cursor = 0;
        }
    }
    fb_render_line(&line, 0xFFFFFF, 0x000000);
}

// ==================== ZAPIS DO HISTORII PRZEZ ENTER ====================

static void run_command(const char *cmd);

static void handle_enter(void) {
    fb_put_char_at_cursor('\n', 0xFFFFFF, 0x000000);

    if (line.len > 0) {
        line.buf[line.len] = 0;

        if (history_len < HISTORY_MAX) {
            for (int i = 0; i < line.len; i++)
                history[history_len][i] = line.buf[i];
            history[history_len][line.len] = 0;
            history_len++;
        }

        run_command(line.buf);
        fb_puts("> ", 0xFFFFFF, 0x000000);

    }

    history_pos = -1;
    line.len = 0;
    line.cursor = 0;
}

// ==================== STRCMP ===================

static int strcmp2(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

// ==================== CLS ======================

static void cls(uint32_t bg) {
    for (uint32_t y = 0; y < g_fb_height; ++y) {
        uint32_t *row = g_fb + y * g_fb_pitch;
        for (uint32_t x = 0; x < g_fb_width; ++x)
            row[x] = bg;
    }

    cursor_x = 0;
    cursor_y = 0;

    line.len = 0;
    line.cursor = 0;

    fb_puts("> ", 0xFFFFFF, bg);
    cursor_x = 2 * CHAR_W;
}

// ================== LEXER ===========================

#define MAX_TOKENS 8
#define MAX_TOKEN_LEN 64

typedef struct {
    char tokens[MAX_TOKENS][MAX_TOKEN_LEN];
    int count;
} parsed_cmd;

static parsed_cmd lex(const char *input) {
    parsed_cmd out = {0};
    int i = 0; // index w input
    int t = 0; // numer tokena
    int c = 0; // index w tokenie

    while (input[i] != '\0' && (input[i] == ' ' || input[i] == '\t'))
        i++;

    while (input[i] != '\0' && t < MAX_TOKENS) {

        if (input[i] == ' ' || input[i] == '\t') {
            if (c > 0) {
                out.tokens[t][c] = '\0';
                t++;
                c = 0;
            }

            while (input[i] != '\0' && (input[i] == ' ' || input[i] == '\t'))
                i++;

            continue;
        }

        if (c < MAX_TOKEN_LEN - 1) {
            out.tokens[t][c++] = input[i++];
        } else {
            i++;
        }
    }

    if (c > 0 && t < MAX_TOKENS) {
        out.tokens[t][c] = '\0';
        t++;
    }

    out.count = t;
    return out;
}

// ==================== MAIN =======================

static void fb_puts_ln(const char *s) {
    fb_puts(s, 0xFFFFFF, 0x000000);
    fb_put_char_at_cursor('\n', 0xFFFFFF, 0x000000);
}

static void run_command(const char *line_in) {
    parsed_cmd p = lex(line_in);
    if (p.count == 0)
        return;

    char *cmd = p.tokens[0];
    char *arg = (p.count >= 2 ? p.tokens[1] : NULL);

    if (strcmp2(cmd, "help")) {
        fb_puts_ln("cls - czysci ekran");
        fb_puts_ln("re - brak");
        fb_puts_ln("echo - powtarza input");
        fb_puts_ln("exit - brak");
    } else if (strcmp2(cmd, "cls")) {
        cls(0x00000000);
    } else if (strcmp2(cmd, "exit")) {
        fb_puts_ln("exit: not implemented yet");
    } else if (strcmp2(cmd, "echo")) {
        if (arg)
            fb_puts_ln(arg);
        else
            fb_puts_ln("");
    } else {
        fb_puts_ln("unknown command");
    }
}

// ==================== ENTRY POINT ELF =======================

void kmain(exports_t *exp) {

    interrupts_init();

    g_fb        = exp->fb;
    g_fb_width  = exp->fb_width;
    g_fb_height = exp->fb_height;
    g_fb_pitch  = exp->fb_pitch;

    cursor_x = 0;
    cursor_y = 0;

    line.len = 0;
    line.cursor = 0;

    fb_puts("kernel ELF PS/2 linebuf (8x16)\n", 0x00FFFFFF, 0x00000000);

    fb_puts("> ", 0xFFFFFF, 0x000000);
    cursor_x = 2 * CHAR_W;
    cursor_y = 0;   // ta sama linia co prompt


    for (;;) {
        uint16_t key = ps2_read_key();

        // break code
        if ((key & 0xFF) & 0x80)
            continue;

        // strzałki
        if (key & 0xE000) {
            handle_arrow(key);
            continue;
        }

        // backspace
        if (key == 0x0E) {
            linebuf_backspace(&line);
            fb_render_line(&line, 0xFFFFFF, 0x000000);
            continue;
        }

        // delete (E0 53)
        if (key == 0xE053) {
            linebuf_delete(&line);
            fb_render_line(&line, 0xFFFFFF, 0x000000);
            continue;
        }

        char ch = scancode_to_ascii(key);
        if (ch == 0)
            continue;

        if (ch == '\n') {
            handle_enter();
            continue;
        }

        // normalny znak
        linebuf_insert(&line, ch);
        fb_render_line(&line, 0xFFFFFF, 0x000000);
    }

}
