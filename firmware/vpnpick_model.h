/*
 * VPN server order in auto mode, as DreamPi's openvpn-client: one ping to every server,
 * the fastest answer first. Servers that did not answer follow in the list's own order
 * (server.list: shumania first), so with ping blocked everywhere it is the list order.
 * rtt_ms[i] < 0 = no answer. Equal times keep the list order.
 */
#pragma once
#include <stdint.h>

static inline void vpnpick_order(const int32_t *rtt_ms, int n, int *ord) {
    int k = 0;
    for (int i = 0; i < n; i++) if (rtt_ms[i] >= 0) ord[k++] = i;
    for (int a = 1; a < k; a++) {                 /* insertion sort: stable, n <= 6 */
        int v = ord[a], b = a;
        while (b > 0 && rtt_ms[ord[b - 1]] > rtt_ms[v]) { ord[b] = ord[b - 1]; b--; }
        ord[b] = v;
    }
    for (int i = 0; i < n; i++) if (rtt_ms[i] < 0) ord[k++] = i;
}
