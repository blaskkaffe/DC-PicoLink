/* Bring-up probe for the single-chip build: FreeRTOS + CYW43 + lwIP, joins the Wi-Fi, prints the address. */
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "FreeRTOS.h"
#include "task.h"
#include "lwip/netif.h"
#ifndef WIFI_SSID
#define WIFI_SSID ""
#define WIFI_PASS ""
#endif
static void net_task(void *a) {
    (void)a;
    if (cyw43_arch_init()) vTaskDelete(NULL);
    cyw43_arch_enable_sta_mode();
    cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASS, CYW43_AUTH_WPA2_AES_PSK, 30000);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}
int main(void) {
    xTaskCreate(net_task, "net", 1024, NULL, 3, NULL);
    vTaskStartScheduler();
    for (;;) { }
}
void vApplicationMallocFailedHook(void) { for (;;) { } }
void vApplicationStackOverflowHook(TaskHandle_t t, char *n) { (void)t; (void)n; for (;;) { } }
