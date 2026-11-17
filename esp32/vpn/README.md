# VPN configs (Dreamcast Live)

DreamPi 2.1's OpenVPN client configs (`*.conf`) and `server.list`, from `/etc/openvpn/client/`
of the DreamPi 2.1 image. Shuouma (Dreamcast Live) okayed using them here. The build embeds
them in the ESP32 firmware (`main/CMakeLists.txt`); `main/vpn.c` tries the servers of
`server.list` in order, then any other profile.

Newer profiles can be uploaded on the box's page without a rebuild (stored in the `vpnprof`
partition, kept across firmware updates). `tools/dreampi_vpn_extract.py` makes the upload
file from a DreamPi image (also works in a-Shell on an iPhone).
