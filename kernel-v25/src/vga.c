#include "vga.h"
#include "gfx.h"
#include "io.h"

static volatile uint16_t *vga_mem = (uint16_t *)0xB8000;
static uint8_t cursor_x = 0;
static uint8_t cursor_y = 0;
static uint8_t fg = VGA_LIGHT_GREY;
static uint8_t bg = VGA_BLACK;
static int gfx_on = 0;

/* switch the whole console to/from VGA graphics mode (320x200x256) */
void vga_gfx_enable(void) {
    gfx_on = 1;
    gfx_mode13h();
    gfx_set_palette();
    gfx_term_init();
    vga_write("CatOS graphics mode: 320x200x256 (gfx)  [text to switch back]\n");
}

void vga_regdump(void) {
    char buf[8];
    vga_write("VGA regs:\n");
    outb(0x3D4, 0x11); outb(0x3D5, 0x0E);
    for (int i = 0; i < 6; i++) {
        unsigned v = 0;
        if (i == 0) { outb(0x3C4, 0x01); v = inb(0x3C5); }
        else if (i == 1) { outb(0x3CE, 0x05); v = inb(0x3CF); }
        else if (i == 2) { outb(0x3CE, 0x06); v = inb(0x3CF); }
        else if (i == 3) { outb(0x3D4, 0x09); v = inb(0x3D5); }
        else if (i == 4) { outb(0x3D4, 0x13); v = inb(0x3D5); }
        else { (void)inb(0x3DA); outb(0x3C0, 0x10); v = inb(0x3C1); }
        buf[0] = "0123456789ABCDEF"[v >> 4]; buf[1] = "0123456789ABCDEF"[v & 15];
        buf[2] = ' '; buf[3] = 0;
        vga_write("  ");
        vga_write(buf);
    }
    vga_write("\nattrpal:");
    (void)inb(0x3DA);
    for (int i = 0; i < 8; i++) {
        outb(0x3C0, i); vga_write(" ");
        unsigned v = inb(0x3C1);
        buf[0] = "0123456789ABCDEF"[v >> 4]; buf[1] = "0123456789ABCDEF"[v & 15]; buf[2] = 0;
        vga_write(buf);
        outb(0x3C0, i);   /* keep index selected */
    }
    vga_write("\ndac0-2:");
    outb(0x3C7, 0);
    for (int i = 0; i < 3; i++) {
        unsigned v = inb(0x3C9);
        buf[0] = "0123456789ABCDEF"[v >> 4]; buf[1] = "0123456789ABCDEF"[v & 15]; buf[2] = 0;
        vga_write(" "); vga_write(buf);
    }
    vga_set_cursor(0, 3);
    vga_write("glyphA: ");
    outb(0x3CE, 0x05); outb(0x3CF, 0x00);   /* latched read mode */
    volatile uint8_t *pp = (volatile uint8_t *)0xB8000;
    unsigned char gv[8];
    for (int i = 0; i < 8; i++) {
        outb(0x3CE, 0x04); outb(0x3CF, 0x02);   /* plane 2: 8px row */
        gv[i] = pp[65 * 32 + i];
    }
    outb(0x3CE, 0x04); outb(0x3CF, 0x00);
    outb(0x3CE, 0x05); outb(0x3CF, 0x10);   /* restore odd/even before printing */
    for (int i = 0; i < 8; i++) {
        buf[0] = "0123456789ABCDEF"[gv[i] >> 4]; buf[1] = "0123456789ABCDEF"[gv[i] & 15]; buf[2] = 0;
        vga_write(buf);
    }
    vga_set_cursor(0, 5);
}

void vga_gfx_disable(void) {
    gfx_on = 0;
    gfx_text_mode();
    vga_init();
}

static inline uint16_t vga_entry(char c) {
    return (uint16_t)(unsigned char)c | ((uint16_t)(bg & 0x0F) << 12) | ((uint16_t)(fg & 0x0F) << 8);
}

static void move_cursor_hw(void) {
    uint16_t pos = cursor_y * 80 + cursor_x;
    outb(0x3D4, 14); outb(0x3D5, pos >> 8);
    outb(0x3D4, 15); outb(0x3D5, pos & 0xFF);
}

void vga_init(void) {
    cursor_x = 0; cursor_y = 0;
    vga_clear();
}

void vga_clear(void) {
    if (gfx_on) { gfx_term_clear(); return; }
    for (int y = 0; y < 25; y++)
        for (int x = 0; x < 80; x++)
            vga_mem[y * 80 + x] = vga_entry(' ');
    cursor_x = 0; cursor_y = 0;
    move_cursor_hw();
}

static void scroll(void) {
    if (cursor_y < 25) return;
    for (int y = 1; y < 25; y++)
        for (int x = 0; x < 80; x++)
            vga_mem[(y - 1) * 80 + x] = vga_mem[y * 80 + x];
    for (int x = 0; x < 80; x++)
        vga_mem[24 * 80 + x] = vga_entry(' ');
    cursor_y = 24;
}

void vga_putc(char c) {
    if (gfx_on) { gfx_term_putc(c); return; }
    switch (c) {
    case '\n':
        cursor_x = 0; cursor_y++;
        break;
    case '\r':
        cursor_x = 0;
        break;
    case '\b':
        if (cursor_x > 0) {
            cursor_x--;
            vga_mem[cursor_y * 80 + cursor_x] = vga_entry(' ');
        }
        break;
    case '\t':
        cursor_x = (cursor_x + 4) & ~3;
        break;
    default:
        vga_mem[cursor_y * 80 + cursor_x] = vga_entry(c);
        cursor_x++;
        break;
    }
    if (cursor_x >= 80) { cursor_x = 0; cursor_y++; }
    scroll();
    move_cursor_hw();
}

void vga_write(const char *s) {
    while (*s) vga_putc(*s++);
}

void vga_set_color(uint8_t f, uint8_t b) { fg = f; bg = b; if (gfx_on) gfx_term_set_color(f, b); }
void vga_set_fg(uint8_t f) { fg = f; if (gfx_on) gfx_term_set_color(f, bg); }
uint8_t vga_get_fg(void) { return fg; }
void vga_move_cursor(void) { move_cursor_hw(); }

void vga_set_cursor(uint8_t x, uint8_t y) {
    cursor_x = x; cursor_y = y;
    move_cursor_hw();
}

void vga_putc_at(uint8_t x, uint8_t y, char c) {
    if (x >= 80 || y >= 25) return;
    vga_mem[y * 80 + x] = vga_entry(c);
}

void vga_write_at(uint8_t x, uint8_t y, const char *s) {
    while (*s && x < 80) { vga_putc_at(x++, y, *s++); }
}
