#ifndef GFX_HDR
#define GFX_HDR

#include <stdint.h>

/* CatOS graphics mode (VGA Mode 13h: 320x200x256, framebuffer 0xA0000) */

#define GFX_W     320
#define GFX_H     200
#define GFX_FB    0xA0000u
#define GFX_COLS  40          /* 320 / 8 */
#define GFX_ROWS  25          /* 200 / 8 */

void gfx_mode13h(void);                /* switch to 320x200x256 */
void gfx_text_mode(void);              /* switch back to 80x25 text */
void gfx_set_palette(void);            /* load 256-colour DAC */
void gfx_putpixel(int x, int y, uint8_t c);
void gfx_fillrect(int x, int y, int w, int h, uint8_t c);
void gfx_hline(int x, int y, int w, uint8_t c);
void gfx_vline(int x, int y, int h, uint8_t c);
void gfx_rect(int x, int y, int w, int h, uint8_t c);
void gfx_drawchar(int x, int y, char ch, uint8_t fg, uint8_t bg);
void gfx_text(int x, int y, const char *s, uint8_t fg, uint8_t bg);

/* graphics-mode terminal (40x25 soft terminal over the framebuffer) */
void gfx_term_init(void);
void gfx_term_clear(void);
void gfx_term_putc(char c);
void gfx_term_write(const char *s);
void gfx_term_set_color(uint8_t fg, uint8_t bg);

#endif
