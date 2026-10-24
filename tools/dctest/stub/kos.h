/* Declarations of the KOS calls dctest.c uses, as I understand KOS 2.x, so that the Dreamcast
   build path is at least syntax-checked without a KOS toolchain. Not a replacement for
   `make` in a KOS environment. */
#pragma once
#include <stdint.h>
#include <stdarg.h>
#define KOS_INIT_FLAGS(x)
#define INIT_DEFAULT 0
typedef uint8_t uint8;
uint64_t timer_us_gettime64(void);
void thd_sleep(int ms);
int scif_read(void);
int scif_write_buffer(const uint8 *data, int len, int xmit);
void scif_flush(void);
int scif_init(void);
void scif_shutdown(void);
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
