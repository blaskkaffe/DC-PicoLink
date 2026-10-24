# Host software

What runs on the computer the board is plugged into.

| Folder | Status |
|---|---|
| [`dreampi/`](dreampi) | working: udev names, `dcusb0` DHCP + NAT, Dreamcast Live VPN address (`dcusb0-vpn`), SD image server (`dcsd`), helper commands. Kept as a self-contained add-on to DreamPi: own files, services and iptables chains, nothing in DreamPi's scripts is changed. |
| [`linux/`](linux) | planned |
| [`macos/`](macos) | planned |
| [`windows/`](windows) | planned |

On any host the serial part is a standard USB serial port (CDC-ACM) and the W5500 part a
standard USB network interface (CDC-NCM): see [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md#host-side).

The one-file DreamPi installer is generated: `tools/make-dreampi-setup.sh`.
