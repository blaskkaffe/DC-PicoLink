# VPN configs (local only)

Put DreamPi's OpenVPN client configs here (`*.conf` and `server.list`, from
`/etc/openvpn/client/` of the DreamPi 2.1 image) to build them into the ESP32 firmware.
Everything in this folder except this README is ignored by git: the configs belong to
Dreamcast Live and stay out of GitHub until they say otherwise.

Once connected, the ESP32 fetches newer configs from Dreamcast Live the way DreamPi 2.1's
updater does (step E-7).
