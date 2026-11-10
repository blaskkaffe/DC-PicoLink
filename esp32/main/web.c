/*
 * Web page on http://192.168.4.1 (setup network) or the address on the home network.
 *   GET  /                      the page
 *   GET  /api/status?log=N&ser=M  link, cable, Wi-Fi, status lines >= N, serial bytes >= M
 *   POST /api/wifi              ssid=...&pass=...   (form encoded)
 *   POST /api/dc1               a dc1 message for the cable (text); answer shows in status
 *   POST /api/serial            raw bytes to send to the Dreamcast's serial port (testing)
 *   POST /api/net               ppp=0|1 (Internet over serial on/off), dcnow=0|1 (DCNow! reports), vpn=0|1
 */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "link_model.h"
#include "app.h"

static const char PAGE[] =
"<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>DC-PicoLink</title><style>"
":root{--bg:#fafafa;--fg:#1c1c1c;--mut:#6b6b6b;--card:#fff;--line:#e3e3e3;--acc:#e8711c;--ok:#2e8b57;--bad:#c0392b}"
"@media(prefers-color-scheme:dark){:root{--bg:#141414;--fg:#ececec;--mut:#9a9a9a;--card:#1e1e1e;--line:#333}}"
"body{margin:0;font:15px/1.4 system-ui,sans-serif;background:var(--bg);color:var(--fg)}"
"header{padding:14px 16px;border-bottom:3px solid var(--acc);display:flex;gap:10px;align-items:center}"
"h1{font-size:18px;margin:0}main{padding:12px;display:grid;gap:12px;max-width:900px;margin:auto}"
".c{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px}"
"h2{font-size:14px;margin:0 0 8px;color:var(--mut);text-transform:uppercase;letter-spacing:.04em}"
".dot{display:inline-block;width:10px;height:10px;border-radius:50%;background:var(--bad);margin-right:6px}"
".up .dot{background:var(--ok)}pre{margin:0;white-space:pre-wrap;word-break:break-word;font:12px/1.35 ui-monospace,monospace}"
"#log,#ser{max-height:320px;overflow:auto}.m{color:var(--mut)}.dc{color:var(--acc)}"
"input,button{font:inherit;padding:6px 8px;border-radius:6px;border:1px solid var(--line);background:var(--bg);color:var(--fg)}"
"button{background:var(--acc);color:#fff;border:0}form{display:flex;gap:6px;flex-wrap:wrap}"
"</style></head><body><header><h1>DC-PicoLink</h1><span id=lk><span class=dot></span><span id=lkt>link: ?</span></span></header><main>"
"<div class=c><h2>Cable</h2><pre id=cab class=m>waiting for the cable...</pre><pre id=st class=m></pre></div>"
"<div class=c><h2>Status lines</h2><pre id=log></pre></div>"
"<div class=c><h2>Serial port (orange: from the Dreamcast)</h2><pre id=ser></pre>"
"<form id=sf><input id=sv placeholder='text to the Dreamcast, e.g. AT' style='flex:1'><button>Send</button></form></div>"
"<div class=c><h2>dc1 to the cable</h2><form id=df><input id=dv value='get link.*' style='flex:1'><button>Send</button></form><pre id=ans class=m></pre></div>"
"<div class=c><h2>Internet for the Dreamcast</h2><pre id=net class=m></pre><form id=nf>"
"<label><input type=checkbox id=np> Internet over serial (PPP)</label><label><input type=checkbox id=nd> report to DCNow!</label><label><input type=checkbox id=nv> VPN (Dreamcast Live)</label><button>Save</button></form></div>"
"<div class=c><h2>Wi-Fi</h2><pre id=wf class=m></pre><form id=wfm><input id=ws placeholder='home network name'>"
"<input id=wp type=password placeholder=password><button>Save</button></form></div>"
"</main><script>"
"let ln=0,sn=0,asq=-1,nfx=false;const $=i=>document.getElementById(i);"
"function hexs(h){let s='';for(let i=0;i<h.length;i+=2){const c=parseInt(h.substr(i,2),16);s+=c==13?'':c==10?'\\n':(c>=32&&c<127)?String.fromCharCode(c):'\\u00b7'}return s}"
"async function tick(){try{const r=await fetch('/api/status?log='+ln+'&ser='+sn);const j=await r.json();"
"$('lk').className=j.link.up?'up':'';$('lkt').textContent='link: '+(j.link.up?'up':'down');"
"$('cab').textContent=j.cable||'waiting for the cable...';"
"const s=j.link;$('st').textContent=`packets ${s.good} good, ${s.crc_bad} bad checksum, ${s.broken} broken, ${s.lost} lost; sent ${s.sent} (${s.send_full} queue full); serial in ${s.ser_in} B, out ${s.ser_out} B; network frames in ${s.eth_in}`;"
"const L=$('log'),at=L.scrollTop+L.clientHeight>=L.scrollHeight-8;for(const e of j.log){const d=document.createElement('div');"
"d.textContent=(e.t/1000).toFixed(1).padStart(8)+'  '+(e.src=='esp32'?'[esp32] ':'')+e.text;if(e.src=='esp32')d.className='m';L.appendChild(d)}"
"while(L.childNodes.length>400)L.removeChild(L.firstChild);if(at)L.scrollTop=L.scrollHeight;ln=j.lognext;"
"const S=$('ser');for(const e of j.ser){const sp=document.createElement('span');sp.className=e.d=='dc'?'dc':'';sp.textContent=hexs(e.hex);S.appendChild(sp)}"
"while(S.childNodes.length>600)S.removeChild(S.firstChild);S.scrollTop=S.scrollHeight;sn=j.sernext;"
"if(j.answer.seq!=asq){asq=j.answer.seq;$('ans').textContent=j.answer.text}"
"const n=j.net,e=j.eth,v=j.vpn;$('net').textContent=(n.on?`serial PPP: ${n.mode=='ppp'?(n.up?'online':'connecting'):'waiting for the Dreamcast to dial'}; calls ${n.calls}\nPPP frames ${n.frames}, TTL set ${n.ttl_fixed}, double SYN dropped ${n.syn_dropped}, bad checksum ${n.fcs_bad}`:'serial PPP: off (serial bytes only go to the monitor above)')+`\ncable network (W5500): ${e.active?'Dreamcast active':'quiet'}, ${e.frames_in} frames in, ${e.frames_out} out (${e.out_dropped} dropped), DHCP leases ${e.leases}${e.dc_ip?' ('+e.dc_ip+')':''}, TTL set ${e.ttl_fixed}, double SYN dropped ${e.syn_dropped}\nDNS: ${n.dns.queries} questions (${n.dns.forwarded} sent on, ${n.dns.overridden} answered here, ${n.dns.refused} refused), ${n.dns.overrides} overrides, ${n.dns.servers} servers; last: ${n.dns.last||'-'}\nDCNow!: ${n.dcnow.on?`on, ${n.dcnow.ok} sent, ${n.dcnow.failed} failed`:'off'}\nVPN: ${!v.profiles?'no VPN profiles in this firmware':!v.on?'off':v.up?`up to ${v.server} for ${v.up_s} s, tunnel address ${v.ip}, ${v.ping_ms>=0?v.ping_ms+' ms to the gateway':'no answer from the gateway yet'}`:`connecting (${v.server||'-'})${v.error?': '+v.error:''}`}${v.profiles?` [${v.connects} connected, ${v.fails} failed]`:''}`;"
"if(!nfx){$('np').checked=n.on;$('nd').checked=n.dcnow.on;$('nv').checked=v.on;$('nv').disabled=!v.profiles}"
"const w=j.wifi;$('wf').textContent=`setup network: ${w.ap} (http://192.168.4.1)\\nhome network: ${w.ssid||'none saved'}`+(w.ssid?(w.up?` - connected, ${w.ip}`:' - not connected'):'');"
"}catch(e){$('lkt').textContent='page lost the ESP32'}setTimeout(tick,1000)}"
"$('sf').onsubmit=e=>{e.preventDefault();fetch('/api/serial',{method:'POST',body:$('sv').value+'\\r'})};"
"$('df').onsubmit=e=>{e.preventDefault();fetch('/api/dc1',{method:'POST',body:'dc1\\n'+$('dv').value.replace(/;\\s*/g,'\\n')+'\\n'})};"
"$('nf').onchange=()=>{nfx=true};$('nf').onsubmit=e=>{e.preventDefault();nfx=false;fetch('/api/net',{method:'POST',body:new URLSearchParams({ppp:$('np').checked?1:0,dcnow:$('nd').checked?1:0,vpn:$('nv').checked?1:0})})};"
"$('wfm').onsubmit=e=>{e.preventDefault();fetch('/api/wifi',{method:'POST',body:new URLSearchParams({ssid:$('ws').value,pass:$('wp').value})})};"
"tick();</script></body></html>";

static esp_err_t page(httpd_req_t *r) {
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, PAGE, sizeof PAGE - 1);
}

static uint32_t query_u32(httpd_req_t *r, const char *key) {
    char q[96], v[16];
    if (httpd_req_get_url_query_str(r, q, sizeof q) != ESP_OK) return 0;
    if (httpd_query_key_value(q, key, v, sizeof v) != ESP_OK) return 0;
    return (uint32_t)strtoul(v, NULL, 10);
}

static esp_err_t status(httpd_req_t *r) {
    const size_t cap = 48 * 1024;
    char *o = malloc(cap);
    if (!o) return httpd_resp_send_500(r);
    size_t n = 0;
    link_stats_t s; link_get_stats(&s);
    n += (size_t)snprintf(o + n, cap - n,
        "{\"link\":{\"up\":%s,\"good\":%lu,\"crc_bad\":%lu,\"broken\":%lu,\"lost\":%lu,\"sent\":%lu,\"send_full\":%lu,"
        "\"ser_in\":%lu,\"ser_out\":%lu,\"eth_in\":%lu},\"cable\":\"",
        link_is_up() ? "true" : "false", (unsigned long)s.good, (unsigned long)s.crc_bad, (unsigned long)s.broken,
        (unsigned long)s.lost, (unsigned long)s.sent, (unsigned long)s.send_full,
        (unsigned long)s.ser_in, (unsigned long)s.ser_out, (unsigned long)s.eth_in);
    char t[1024];
    size_t k = link_cable_info(t, sizeof t);
    n += json_escape(o + n, cap - n, t, k);
    n += (size_t)snprintf(o + n, cap - n, "\",\"wifi\":");
    n += wifi_info_json(o + n, cap - n);
    n += (size_t)snprintf(o + n, cap - n, ",\"net\":");
    n += net_info_json(o + n, cap - n);
    n += (size_t)snprintf(o + n, cap - n, ",\"eth\":");
    n += eth_info_json(o + n, cap - n);
    n += (size_t)snprintf(o + n, cap - n, ",\"vpn\":");
    n += vpn_info_json(o + n, cap - n);
    uint32_t aseq;
    k = link_last_answer(t, sizeof t, &aseq);
    n += (size_t)snprintf(o + n, cap - n, ",\"answer\":{\"seq\":%lu,\"text\":\"", (unsigned long)aseq);
    n += json_escape(o + n, cap - n, t, k);
    uint32_t next;
    n += (size_t)snprintf(o + n, cap - n, "\"},\"ser\":");
    n += ser_mon_since(query_u32(r, "ser"), o + n, (cap - n) / 3, &next);   /* leave room for the log */
    n += (size_t)snprintf(o + n, cap - n, ",\"sernext\":%lu,\"log\":", (unsigned long)next);
    n += log_since(query_u32(r, "log"), o + n, cap - n - 32, &next);
    n += (size_t)snprintf(o + n, cap - n, ",\"lognext\":%lu}", (unsigned long)next);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_send(r, o, (ssize_t)n);
    free(o);
    return e;
}

static int body(httpd_req_t *r, char *buf, size_t cap) {
    if (r->content_len >= cap) return -1;
    size_t got = 0;
    while (got < r->content_len) {
        int k = httpd_req_recv(r, buf + got, r->content_len - got);
        if (k <= 0) return -1;
        got += (size_t)k;
    }
    buf[got] = 0;
    return (int)got;
}

static void url_decode(char *s) {
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') *o++ = ' ';
        else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else *o++ = *s;
    }
    *o = 0;
}

static esp_err_t post_wifi(httpd_req_t *r) {
    char b[256], ssid[100] = "", pass[100] = "";
    if (body(r, b, sizeof b) < 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "too long");
    httpd_query_key_value(b, "ssid", ssid, sizeof ssid);
    httpd_query_key_value(b, "pass", pass, sizeof pass);
    url_decode(ssid); url_decode(pass);
    if (!ssid[0] || strlen(ssid) > 32 || strlen(pass) > 64) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad name");
    wifi_set_sta(ssid, pass);
    return httpd_resp_sendstr(r, "ok");
}

static esp_err_t post_dc1(httpd_req_t *r) {
    char b[600];
    int n = body(r, b, sizeof b);
    if (n < 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "too long");
    return httpd_resp_sendstr(r, link_dc1(b, (size_t)n) ? "sent" : "link busy");
}

static esp_err_t post_serial(httpd_req_t *r) {
    char b[512];
    int n = body(r, b, sizeof b);
    if (n < 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "too long");
    if (!link_send(LINK_CH_SER, b, (size_t)n)) return httpd_resp_sendstr(r, "link busy");
    ser_mon_add(false, (const uint8_t *)b, (size_t)n);
    link_note_ser_out((size_t)n);
    return httpd_resp_sendstr(r, "sent");
}

static esp_err_t post_net(httpd_req_t *r) {
    char b[64], v[8];
    if (body(r, b, sizeof b) < 0) return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "too long");
    if (httpd_query_key_value(b, "ppp", v, sizeof v) == ESP_OK) ppp_srv_set_enabled(v[0] == '1');
    if (httpd_query_key_value(b, "dcnow", v, sizeof v) == ESP_OK) dcnow_set_enabled(v[0] == '1');
    if (httpd_query_key_value(b, "vpn", v, sizeof v) == ESP_OK) vpn_set_enabled(v[0] == '1');
    return httpd_resp_sendstr(r, "ok");
}

void web_start(void) {
    httpd_handle_t h = NULL;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.stack_size = 8192;
    if (httpd_start(&h, &c) != ESP_OK) return;
    static const httpd_uri_t u[] = {
        { .uri = "/", .method = HTTP_GET, .handler = page },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status },
        { .uri = "/api/wifi", .method = HTTP_POST, .handler = post_wifi },
        { .uri = "/api/dc1", .method = HTTP_POST, .handler = post_dc1 },
        { .uri = "/api/serial", .method = HTTP_POST, .handler = post_serial },
        { .uri = "/api/net", .method = HTTP_POST, .handler = post_net },
    };
    for (size_t i = 0; i < sizeof u / sizeof u[0]; i++) httpd_register_uri_handler(h, &u[i]);
}
