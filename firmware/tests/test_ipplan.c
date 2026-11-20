/* ipplan_model.h: the box's addresses and the Dreamcast network test. */
#include <stdio.h>
#include "ipplan_model.h"

static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)
#define A(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

int main(void) {
    CHECK(IPP_SETUP == A(192, 168, 220, 1));
    CHECK(IPP_BOX == A(192, 168, 221, 1) && IPP_PPP_DC == A(192, 168, 221, 2));
    CHECK(IPP_DHCP_FIRST == A(192, 168, 221, 2) && IPP_DHCP_LAST == A(192, 168, 221, 99));
    CHECK(ipp_dc_host(A(192, 168, 221, 2)) && ipp_dc_host(A(192, 168, 221, 100)) && ipp_dc_host(A(192, 168, 221, 254)));
    CHECK(!ipp_dc_host(IPP_BOX));                                     /* the box itself */
    CHECK(!ipp_dc_host(A(192, 168, 221, 0)) && !ipp_dc_host(A(192, 168, 221, 255)));
    CHECK(!ipp_dc_host(A(192, 168, 220, 5)) && !ipp_dc_host(A(192, 168, 1, 98)) && !ipp_dc_host(A(10, 202, 0, 1)));
    CHECK(ipp_in_dcnet(IPP_BOX) && !ipp_in_dcnet(IPP_SETUP));
    /* lwIP / esp_ip4_addr_t keep addresses in network order: on the ESP32 (little-endian) the
       first byte is the lowest */
    uint32_t e = IPP_ESP(IPP_BOX);
    CHECK((e & 0xFF) == 192 && ((e >> 8) & 0xFF) == 168 && ((e >> 16) & 0xFF) == 221 && (e >> 24) == 1);
    printf("%s: %d failures\n", bad ? "FAIL" : "ok", bad);
    return bad != 0;
}
