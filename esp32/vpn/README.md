# VPN configs (local only)

Put DreamPi's OpenVPN client configs here (`*.conf` and `server.list`, from
`/etc/openvpn/client/` of the DreamPi 2.1 image) to build them into the ESP32 firmware.
Everything in this folder except this README is ignored by git: the configs belong to
Dreamcast Live and stay out of GitHub until they say otherwise.

The build embeds them (`main/CMakeLists.txt`); `main/vpn.c` tries the servers of `server.list` in order.
Fetching newer configs from Dreamcast Live over the tunnel, as DreamPi 2.1's updater does, is not built yet.
