#include <stdint.h>

struct exports {
    uint32_t* fb;
    uint32_t  fb_width;
    uint32_t  fb_height;
    uint32_t  fb_pitch;
};

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "dN"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "dN"(port));
}

static void io_wait(void)
{
    // proste opóźnienie – odczyt z nieużywanego portu
    (void)inb(0x80);
}

static void fill(struct exports* e, uint32_t color)
{
    for (uint32_t y = 0; y < e->fb_height; y++) {
        uint32_t* row = e->fb + y * e->fb_pitch;
        for (uint32_t x = 0; x < e->fb_width; x++) {
            row[x] = color;
        }
    }
}

static uint8_t ps2_read_scancode(void)
{
    // czekamy aż output buffer będzie pełny (bit 0 w 0x64)
    for (;;) {
        uint8_t status = inb(0x64);
        if (status & 0x01) {
            return inb(0x60);
        }
    }
}

void kmain(void* exp_ptr)
{
    struct exports* e = (struct exports*)exp_ptr;

    fill(e, 0x00000000); // czarny na start

    for (;;) {
        uint8_t sc = ps2_read_scancode();

        // ignorujemy break codes (>= 0x80)
        if (sc & 0x80)
            continue;

        if (sc == 0x1E) {          // 'a'
            fill(e, 0x000000FF);   // niebieski
        } else if (sc == 0x20) {   // 'd'
            fill(e, 0x00FF0000);   // czerwony
        }
    }
}
