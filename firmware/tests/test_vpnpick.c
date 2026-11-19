/* vpnpick_order: fastest ping first, no answer after, list order on ties. */
#include <stdio.h>
#include "vpnpick_model.h"

static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); bad++; } } while (0)

int main(void) {
    int o[6];
    { int32_t r[] = { 40, 20, 30 }; vpnpick_order(r, 3, o); CHECK(o[0] == 1 && o[1] == 2 && o[2] == 0); }
    { int32_t r[] = { -1, -1, -1 }; vpnpick_order(r, 3, o); CHECK(o[0] == 0 && o[1] == 1 && o[2] == 2); }   /* ping blocked: list order */
    { int32_t r[] = { -1, 50, -1, 10 }; vpnpick_order(r, 4, o); CHECK(o[0] == 3 && o[1] == 1 && o[2] == 0 && o[3] == 2); }
    { int32_t r[] = { 25, 25, 5 }; vpnpick_order(r, 3, o); CHECK(o[0] == 2 && o[1] == 0 && o[2] == 1); }   /* tie: list order */
    { int32_t r[] = { 0 }; vpnpick_order(r, 1, o); CHECK(o[0] == 0); }
    vpnpick_order(NULL, 0, o);
    printf("%s: %d failures\n", bad ? "FAIL" : "ok", bad);
    return bad != 0;
}
