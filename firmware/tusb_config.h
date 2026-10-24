#pragma once
#define CFG_TUSB_RHPORT0_MODE   (OPT_MODE_DEVICE)
#define CFG_TUSB_OS             OPT_OS_PICO
#define CFG_TUD_ENDPOINT0_SIZE  64
#define CFG_TUD_CDC             1   /* coder's cable serial */
#define CFG_TUD_ECM_RNDIS       0
#define CFG_TUD_NCM             1
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          1   /* SD block link (raw USB, not a tty: modem
                                       probes like wvdialconf can't touch it) */
#define CFG_TUD_VENDOR_RX_BUFSIZE 2048
#define CFG_TUD_VENDOR_TX_BUFSIZE 2048
#define CFG_TUD_VENDOR_EPSIZE     64
#define CFG_TUD_CDC_RX_BUFSIZE  2048
#define CFG_TUD_CDC_TX_BUFSIZE  2048
#define CFG_TUD_CDC_EP_BUFSIZE  64

/* NCM buffering tuned for ~1 MB/s (TinyUSB notes: OUT 2 / IN 3 is the sweet spot on RP2040) */
#define CFG_TUD_NCM_OUT_NTB_N   2
#define CFG_TUD_NCM_IN_NTB_N    3
