/* dc_pio_relocate() on the real assembled programs (needs a firmware build dir:
   gcc -DPICO_NO_HARDWARE=1 -I<build> ...; run by tools/run-tests.sh <build>). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../dc_pio_reloc.h"
#include "w5500_spi.pio.h"
#include "sd_spi.pio.h"
#include "uart.pio.h"

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define N(a) (sizeof a / sizeof a[0])

/* every 'wait gpio' must name one of the four Dreamcast SPI pins after the move */
static void check_prog(const uint16_t *p, unsigned n, int base, unsigned want_changed) {
    uint16_t d[32];
    CHECK(n <= 32);
    CHECK(dc_pio_relocate(p, d, n, base - 2) == (base == 2 ? 0 : want_changed));
    unsigned waits = 0;
    for (unsigned i = 0; i < n; i++) {
        if ((p[i] & 0xE060) == 0x2000) {
            waits++;
            unsigned from = p[i] & 31, to = d[i] & 31;
            CHECK(from >= 2 && from <= 5);                     /* SCLK or CS in the source */
            CHECK(to == from + (unsigned)(base - 2));
            CHECK((d[i] & ~31u) == (p[i] & ~31u));             /* polarity, delay untouched */
        } else {
            CHECK(d[i] == p[i]);                               /* everything else untouched */
        }
    }
    CHECK(waits == want_changed);
}

int main(void) {
    for (int base = 2; base <= 9; base += 7) {
        check_prog(w5500_spi_program_instructions, N(w5500_spi_program_instructions), base, 6);
        check_prog(sd_spi_program_instructions, N(sd_spi_program_instructions), base, 4);
        uint16_t d[32];
        /* the UART programs use pin-relative waits: never touched */
        CHECK(dc_pio_relocate(uart_rx_program_instructions, d, N(uart_rx_program_instructions), base - 2) == 0);
        CHECK(dc_pio_relocate(uart_tx_program_instructions, d, N(uart_tx_program_instructions), base - 2) == 0);
        CHECK(dc_pio_relocate(dc_tx_meter_program_instructions, d, N(dc_tx_meter_program_instructions), base - 2) == 0);
        CHECK(dc_pio_relocate(la_capture_program_instructions, d, N(la_capture_program_instructions), base - 2) == 0);
    }
    printf("OK: w5500_spi (6 waits) and sd_spi (4 waits) move to GPIO9-12 exactly; UART/meter/capture programs untouched\n");
    return 0;
}
