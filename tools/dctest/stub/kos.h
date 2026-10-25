/* Declarations of the KOS calls dctest.c uses, as I understand KOS 2.x, so that the Dreamcast
   build path is at least syntax-checked without a KOS toolchain. Not a replacement for
   `make` in a KOS environment. */
#pragma once
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#define KOS_INIT_FLAGS(x)
#define INIT_DEFAULT 0
typedef uint8_t uint8;
uint64_t timer_us_gettime64(void);
void thd_sleep(int ms);
void thd_pass(void);
int scif_read(void);
int scif_write_buffer(const uint8 *data, int len, int xmit);
void scif_flush(void);
int scif_init(void);
int scif_shutdown(void);
void scif_set_irq_usage(int on);
void scif_set_parameters(int baud, int fifo);
typedef struct { uint8_t ip_addr[4]; } netif_t;
extern netif_t *net_default_dev;
int net_init(uint32_t ip);
void net_shutdown(void);
typedef struct { int dummy; } maple_device_t;
typedef struct { uint32_t buttons; } cont_state_t;
#define MAPLE_FUNC_CONTROLLER 1
#define CONT_START 8
maple_device_t *maple_enum_type(int n, unsigned f);
void *maple_dev_status(maple_device_t *d);
extern int dcload_type;
#define DCLOAD_TYPE_NONE (-1)
int dbgio_dev_select(const char *name);
void scif_spi_set_cs(int v);
uint8_t scif_spi_rw_byte(uint8_t b);
int scif_spi_init(void);
int scif_spi_shutdown(void);

typedef struct { int width, height; } vid_mode_t;
extern vid_mode_t *vid_mode;
extern uint16_t *vram_s;
void vid_empty(void);
void dbgio_disable(void);
#define BFONT_HEIGHT 24
#define BFONT_CODE_ISO8859_1 0
void bfont_set_encoding(int enc);
void bfont_draw_str_ex(void *b, uint32_t width, uint32_t fg, uint32_t bg, uint8_t bpp, _Bool opaque, const char *str);
