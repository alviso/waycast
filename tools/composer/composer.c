/* composer.c — the laptop place-beacon: any PC/Mac + a USB LoRa
 * dongle becomes a Waycast injector AND place agent (Tier 0 + 2½).
 *
 * Two faces, one binary:
 *   CLI:  ./build/waycast-composer --port /dev/cu.X --name "Shop" ...
 *   UI:   ./build/waycast-composer --ui        (opens http://127.0.0.1:8477)
 *
 * The UI face is the desktop "captive portal": a local web form for
 * the shopkeeper — pick the dongle, describe the place, hit
 * broadcast. Works with no radio attached (compose first, connect
 * later). The browser is the cross-platform layer; porting to
 * Windows means porting only the serial open/read (win32 COM).
 *
 * Same PHY profile (SF7/BW125/CH65=915MHz) and DTU framing as every node.
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "lora_dtu.h"
#include "vmesh_msg.h"
#include "vmesh_wire.h"

#define UI_PORT 8477
#define LOG_LINES 48
#define LOG_W 120

/* ---------------- shared state ---------------- */

static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char port[128], name[64], note[64];
    char hours[64], wait[64], today[64];
    uint8_t cat;
    double lat, lon;
    int interval_s, ttl_s, radius_m;
    int running; /* beaconing on/off */
} s_cfg = { .cat = VMESH_LC_INFO, .interval_s = 120,
            .ttl_s = 3600, .radius_m = 3000 };

static int s_fd = -1;
static uint32_t s_origin;
static uint16_t s_seq;
static char s_log[LOG_LINES][LOG_W];
static int s_log_n;

static void logln(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&s_mx);
    char *dst = s_log[s_log_n % LOG_LINES];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    int off = (int)strftime(dst, LOG_W, "%H:%M:%S  ", &tmv);
    vsnprintf(dst + off, (size_t)(LOG_W - off), fmt, ap);
    s_log_n++;
    pthread_mutex_unlock(&s_mx);
    va_end(ap);
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    fflush(stdout);
    va_end(ap);
}

static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h | 0x40000000u;
}

/* ---------------- radio ---------------- */

static int radio_open(const char *dev)
{
    int fd = open(dev, O_RDWR | O_NOCTTY);
    if (fd < 0) return -1;
    struct termios t;
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    cfsetspeed(&t, B115200);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 1;
    tcsetattr(fd, TCSANOW, &t);
    char cmds[256];
    int n = lora_dtu_init_cmds(cmds, sizeof(cmds), 7, 0, 65, 22); /* CH65 = 915.0 MHz US ISM, SF7 */
    if (n > 0) write(fd, cmds, (size_t)n);
    return fd;
}

static void send_msg(const vmesh_msg_t *m)
{
    if (s_fd < 0) return;
    uint8_t wire[VMESH_WIRE_MAX], fr[VMESH_WIRE_MAX + LORA_DTU_OVERHEAD];
    int n = vmesh_wire_encode(m, wire, sizeof(wire));
    if (n <= 0) return;
    int f = lora_dtu_frame(wire, (size_t)n, fr, sizeof(fr));
    if (f > 0) write(s_fd, fr, (size_t)f);
}

static void beacon_now(void)
{
    vmesh_msg_t m;
    memset(&m, 0, sizeof(m));
    pthread_mutex_lock(&s_mx);
    m.version      = VMESH_PROTO_VERSION;
    m.msg_type     = VMESH_MT_TEXT;
    m.channel      = VMESH_CH_LOCAL;
    m.hazard_type  = s_cfg.cat;
    m.origin_id    = s_origin;
    m.seq          = ++s_seq;
    m.lat_e7       = (int32_t)(s_cfg.lat * 1e7);
    m.lon_e7       = (int32_t)(s_cfg.lon * 1e7);
    m.created_s    = (uint32_t)time(NULL);
    m.ttl_s        = (uint16_t)s_cfg.ttl_s;
    m.radius_m_x10 = (uint16_t)(s_cfg.radius_m / 10);
    snprintf(m.note, sizeof(m.note), "%s", s_cfg.note);
    pthread_mutex_unlock(&s_mx);
    send_msg(&m);
    logln("[beacon] \"%s\" (seq %u)", m.note, m.seq);
}

static const char *answer_for(const char *q)
{
    /* under s_mx */
    if ((strstr(q, "hour") || strstr(q, "open")) && s_cfg.hours[0])
        return s_cfg.hours;
    if (strstr(q, "wait") && s_cfg.wait[0]) return s_cfg.wait;
    if ((strstr(q, "today") || strstr(q, "special")) && s_cfg.today[0])
        return s_cfg.today;
    return s_cfg.note;
}

static void handle_rx(const uint8_t *pl, size_t n)
{
    vmesh_msg_t m;
    if (vmesh_wire_decode(pl, n, &m) != 0) return;
    if (m.msg_type != VMESH_MT_QUERY) return;
    if (m.ref_origin != s_origin) {
        logln("[heard] a query for someone else (0x%08X)", m.ref_origin);
        return;
    }
    logln("[query] \"%s\" from 0x%08X", m.note, m.origin_id);

    vmesh_msg_t r;
    memset(&r, 0, sizeof(r));
    pthread_mutex_lock(&s_mx);
    r.version    = VMESH_PROTO_VERSION;
    r.msg_type   = VMESH_MT_REPLY;
    r.channel    = VMESH_CH_LOCAL;
    r.origin_id  = s_origin;
    r.seq        = ++s_seq;
    r.ref_origin = m.origin_id;
    r.ref_seq    = m.seq;
    r.created_s  = (uint32_t)time(NULL);
    snprintf(r.note, sizeof(r.note), "%s", answer_for(m.note));
    pthread_mutex_unlock(&s_mx);
    usleep(400 * 1000);
    send_msg(&r);
    logln("[reply] \"%s\"", r.note);
}

static void *radio_thread(void *arg)
{
    (void)arg;
    lora_dtu_rx_t rx;
    memset(&rx, 0, sizeof(rx));
    uint8_t pl[LORA_DTU_MAX_PAYLOAD];
    time_t next_beacon = 0;

    for (;;) {
        pthread_mutex_lock(&s_mx);
        int running = s_cfg.running;
        int iv = s_cfg.interval_s;
        pthread_mutex_unlock(&s_mx);

        if (s_fd < 0) { usleep(200 * 1000); continue; }

        if (running && time(NULL) >= next_beacon) {
            beacon_now();
            next_beacon = time(NULL) + iv;
        }
        uint8_t b;
        ssize_t r = read(s_fd, &b, 1);
        if (r == 1) {
            int got = lora_dtu_rx_feed(&rx, b, pl, sizeof(pl));
            if (got > 0) handle_rx(pl, (size_t)got);
        } else {
            usleep(20 * 1000);
        }
        if (!running) next_beacon = 0; /* re-beacon promptly on start */
    }
    return NULL;
}

/* ---------------- tiny local web UI ---------------- */

static const char UI_HTML[] =
"<!doctype html><html><head><meta charset=utf-8>"
"<meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>Waycast Composer</title><style>"
":root{--bg:#14161B;--pn:#1A1D23;--ink:#E9ECEF;--dim:#868E96;"
"--grn:#51CF66;--tea:#0CA678;--amb:#F59F00}"
"*{margin:0;padding:0;box-sizing:border-box}"
"body{background:var(--bg);color:var(--ink);font:15px/1.5 -apple-system,"
"'Segoe UI',Roboto,sans-serif;max-width:760px;margin:0 auto;padding:28px 16px}"
"h1{font-size:24px}h1 b{color:var(--tea)}"
".sub{color:var(--dim);margin:2px 0 20px}"
".card{background:var(--pn);border-radius:12px;padding:18px;margin-bottom:14px}"
".card h2{font-size:15px;color:var(--dim);text-transform:uppercase;"
"letter-spacing:.06em;margin-bottom:12px}"
"label{display:block;color:var(--dim);font-size:13px;margin:10px 0 3px}"
"input,select{width:100%;background:#22262E;color:var(--ink);border:1px solid"
" #3B4252;border-radius:8px;padding:9px 10px;font-size:15px}"
".row{display:flex;gap:12px}.row>div{flex:1}"
"button{width:100%;padding:14px;border:0;border-radius:10px;font-size:16px;"
"font-weight:600;cursor:pointer;margin-top:14px}"
"#go{background:var(--grn);color:#08130a}"
"#go.on{background:#66341A;color:var(--ink)}"
"#status{margin-top:10px;color:var(--dim);font-size:14px}"
"#log{background:#0e1013;border-radius:8px;padding:10px;height:180px;"
"overflow-y:auto;font:12px/1.6 ui-monospace,Menlo,monospace;color:#9ae6b4;"
"white-space:pre-wrap}"
"</style></head><body>"
"<h1><b>Waycast</b> Composer</h1>"
"<div class=sub>Put your place on the neighborhood network. No account,"
" no internet — just your radio.</div>"
"<div class=card><h2>Radio</h2><div class=row><div>"
"<label>USB LoRa dongle</label><select id=port></select></div></div>"
"<div id=status>checking…</div></div>"
"<div class=card><h2>Your place</h2>"
"<div class=row><div><label>Name</label>"
"<input id=name placeholder='Orenco Bakery'></div>"
"<div><label>Category</label><select id=cat>"
"<option value=4>info / shop</option><option value=0>lodging</option>"
"<option value=1>fuel</option><option value=2>event</option>"
"<option value=3>aid</option></select></div></div>"
"<div class=row><div><label>Latitude</label>"
"<input id=lat placeholder='45.52'></div>"
"<div><label>Longitude</label><input id=lon placeholder='-122.89'>"
"</div></div></div>"
"<div class=card><h2>On the air</h2>"
"<label>Notice (what drivers see — max 40 chars)</label>"
"<input id=note maxlength=40 placeholder='fresh sourdough till 18:00'>"
"<div class=row><div><label>Answer: hours?</label>"
"<input id=hours maxlength=40 placeholder='7-18 daily'></div>"
"<div><label>Answer: wait?</label>"
"<input id=wait maxlength=40 placeholder='~5 min'></div></div>"
"<label>Answer: today?</label>"
"<input id=today maxlength=40 placeholder='cardamom buns just out'>"
"<label>Re-beacon every (seconds)</label><input id=interval value=120>"
"<button id=go>Start broadcasting</button></div>"
"<div class=card><h2>Activity</h2><div id=log></div></div>"
"<script>"
"let on=false;"
"async function refresh(){"
" const r=await fetch('/state');const s=await r.json();"
" const sel=document.getElementById('port');"
" if(sel.options.length!==s.ports.length+1){sel.innerHTML='';"
"  const o=document.createElement('option');o.value='';"
"  o.text='— choose port —';sel.add(o);"
"  for(const p of s.ports){const o=document.createElement('option');"
"   o.value=p;o.text=p;sel.add(o);}}"
" if(s.port&&sel.value!==s.port)sel.value=s.port;"
" on=s.running;"
" document.getElementById('status').textContent="
"  s.connected?('connected: '+s.port+'  (on air as 0x'+s.origin+')')"
"  :'no radio connected — plug in the dongle and choose it above';"
" const g=document.getElementById('go');"
" g.textContent=on?'Stop broadcasting':'Start broadcasting';"
" g.className=on?'on':'';"
" document.getElementById('log').textContent=s.log;"
"}"
"document.getElementById('go').onclick=async()=>{"
" const f=id=>document.getElementById(id).value;"
" const body=new URLSearchParams({port:f('port'),name:f('name'),"
"  cat:f('cat'),lat:f('lat'),lon:f('lon'),note:f('note'),"
"  hours:f('hours'),wait:f('wait'),today:f('today'),"
"  interval:f('interval'),run:on?'0':'1'});"
" await fetch('/apply',{method:'POST',body});refresh();};"
"setInterval(refresh,2000);refresh();"
"</script></body></html>";

static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '+') { *o++ = ' '; s++; }
        else if (*s == '%' && s[1] && s[2]) {
            int v;
            sscanf(s + 1, "%2x", &v);
            *o++ = (char)v;
            s += 3;
        } else *o++ = *s++;
    }
    *o = 0;
}

static void form_get(const char *body, const char *key, char *out,
                     size_t cap)
{
    out[0] = 0;
    char pat[40];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = strstr(body, pat);
    if (!p) return;
    /* must be at start or after & */
    if (p != body && p[-1] != '&') {
        p = strstr(p + 1, pat);
        if (!p || (p != body && p[-1] != '&')) return;
    }
    p += strlen(pat);
    size_t i = 0;
    while (p[i] && p[i] != '&' && i < cap - 1) { out[i] = p[i]; i++; }
    out[i] = 0;
    url_decode(out);
}

static void list_ports(char *out, size_t cap)
{
    out[0] = 0;
    DIR *d = opendir("/dev");
    if (!d) return;
    struct dirent *e;
    size_t used = 0;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "cu.", 3) != 0) continue;
        if (!strstr(e->d_name, "usb") && !strstr(e->d_name, "SLAB") &&
            !strstr(e->d_name, "wch"))
            continue;
        int n = snprintf(out + used, cap - used, "%s\"/dev/%s\"",
                         used ? "," : "", e->d_name);
        if (n < 0 || (size_t)n >= cap - used) break;
        used += (size_t)n;
    }
    closedir(d);
}

static void json_escape(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 2 < cap; in++) {
        if (*in == '"' || *in == '\\') out[o++] = '\\';
        if ((unsigned char)*in < 0x20) { out[o++] = ' '; continue; }
        out[o++] = *in;
    }
    out[o] = 0;
}

static void http_serve(int cfd)
{
    static char req[8192];
    ssize_t n = read(cfd, req, sizeof(req) - 1);
    if (n <= 0) return;
    req[n] = 0;

    char resp_hdr[256];
    if (!strncmp(req, "GET /state", 10)) {
        char ports[1024], logbuf[LOG_LINES * LOG_W], esc[LOG_LINES * LOG_W];
        list_ports(ports, sizeof(ports));
        pthread_mutex_lock(&s_mx);
        logbuf[0] = 0;
        size_t used = 0;
        int start = s_log_n > LOG_LINES ? s_log_n - LOG_LINES : 0;
        for (int i = start; i < s_log_n; i++) {
            int w = snprintf(logbuf + used, sizeof(logbuf) - used, "%s\n",
                             s_log[i % LOG_LINES]);
            if (w < 0 || (size_t)w >= sizeof(logbuf) - used) break;
            used += (size_t)w;
        }
        int running = s_cfg.running;
        char port[128];
        snprintf(port, sizeof(port), "%s", s_cfg.port);
        pthread_mutex_unlock(&s_mx);
        json_escape(logbuf, esc, sizeof(esc));

        static char body[LOG_LINES * LOG_W + 2048];
        int bl = snprintf(body, sizeof(body),
            "{\"ports\":[%s],\"port\":\"%s\",\"connected\":%s,"
            "\"running\":%s,\"origin\":\"%08X\",\"log\":\"%s\"}",
            ports, port, s_fd >= 0 ? "true" : "false",
            running ? "true" : "false", s_origin, esc);
        snprintf(resp_hdr, sizeof(resp_hdr),
                 "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                 "Content-Length: %d\r\nConnection: close\r\n\r\n", bl);
        write(cfd, resp_hdr, strlen(resp_hdr));
        write(cfd, body, (size_t)bl);
        return;
    }

    if (!strncmp(req, "POST /apply", 11)) {
        char *body = strstr(req, "\r\n\r\n");
        body = body ? body + 4 : (char *)"";
        char v[128];

        pthread_mutex_lock(&s_mx);
        form_get(body, "name", s_cfg.name, sizeof(s_cfg.name));
        form_get(body, "note", s_cfg.note, sizeof(s_cfg.note));
        form_get(body, "hours", s_cfg.hours, sizeof(s_cfg.hours));
        form_get(body, "wait", s_cfg.wait, sizeof(s_cfg.wait));
        form_get(body, "today", s_cfg.today, sizeof(s_cfg.today));
        form_get(body, "cat", v, sizeof(v));
        if (v[0]) s_cfg.cat = (uint8_t)atoi(v);
        form_get(body, "lat", v, sizeof(v));
        if (v[0]) s_cfg.lat = atof(v);
        form_get(body, "lon", v, sizeof(v));
        if (v[0]) s_cfg.lon = atof(v);
        form_get(body, "interval", v, sizeof(v));
        if (atoi(v) >= 30) s_cfg.interval_s = atoi(v);
        form_get(body, "run", v, sizeof(v));
        int want_run = atoi(v);
        char want_port[128];
        form_get(body, "port", want_port, sizeof(want_port));
        s_origin = fnv1a(s_cfg.name);
        int need_open = want_port[0] &&
                        strcmp(want_port, s_cfg.port) != 0;
        if (need_open)
            snprintf(s_cfg.port, sizeof(s_cfg.port), "%s", want_port);
        s_cfg.running = want_run && s_cfg.name[0] && s_cfg.note[0];
        pthread_mutex_unlock(&s_mx);

        if (need_open) {
            if (s_fd >= 0) close(s_fd);
            s_fd = radio_open(want_port);
            logln(s_fd >= 0 ? "[radio] %s connected"
                            : "[radio] %s failed to open", want_port);
        }
        logln(s_cfg.running ? "[on air] \"%s\" broadcasting"
                            : "[paused] broadcasting stopped",
              s_cfg.name);

        const char *ok = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                         "Connection: close\r\n\r\nok";
        write(cfd, ok, strlen(ok));
        return;
    }

    /* default: the app */
    snprintf(resp_hdr, sizeof(resp_hdr),
             "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
             "Content-Length: %zu\r\nConnection: close\r\n\r\n",
             sizeof(UI_HTML) - 1);
    write(cfd, resp_hdr, strlen(resp_hdr));
    write(cfd, UI_HTML, sizeof(UI_HTML) - 1);
}

static int ui_main(int http_port)
{
    int sfd = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* localhost ONLY */
    a.sin_port = htons((uint16_t)http_port);
    if (bind(sfd, (struct sockaddr *)&a, sizeof(a)) < 0 ||
        listen(sfd, 8) < 0) {
        perror("bind/listen");
        return 1;
    }

    pthread_t th;
    pthread_create(&th, NULL, radio_thread, NULL);
    pthread_detach(th);

    printf("Waycast Composer UI: http://127.0.0.1:%d\n", http_port);
    logln("[ui] composer up — waiting for your place details");
#ifdef __APPLE__
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "open http://127.0.0.1:%d", http_port);
    system(cmd);
#endif

    for (;;) {
        int cfd = accept(sfd, NULL, NULL);
        if (cfd < 0) continue;
        http_serve(cfd);
        close(cfd);
    }
    return 0;
}

/* ---------------- CLI face (headless) ---------------- */

static uint8_t cat_parse(const char *s)
{
    if (!strcmp(s, "lodging")) return VMESH_LC_LODGING;
    if (!strcmp(s, "fuel"))    return VMESH_LC_FUEL;
    if (!strcmp(s, "event"))   return VMESH_LC_EVENT;
    if (!strcmp(s, "aid"))     return VMESH_LC_AID;
    return VMESH_LC_INFO;
}

int main(int argc, char **argv)
{
    int ui = 0, http_port = UI_PORT;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ui")) ui = 1;
        else if (!strcmp(argv[i], "--http-port") && i + 1 < argc)
            http_port = atoi(argv[++i]);
        else if (i + 1 < argc) {
            const char *v = argv[i + 1];
            if (!strcmp(argv[i], "--port"))
                snprintf(s_cfg.port, sizeof(s_cfg.port), "%s", v);
            else if (!strcmp(argv[i], "--name"))
                snprintf(s_cfg.name, sizeof(s_cfg.name), "%s", v);
            else if (!strcmp(argv[i], "--note"))
                snprintf(s_cfg.note, sizeof(s_cfg.note), "%s", v);
            else if (!strcmp(argv[i], "--hours"))
                snprintf(s_cfg.hours, sizeof(s_cfg.hours), "%s", v);
            else if (!strcmp(argv[i], "--wait"))
                snprintf(s_cfg.wait, sizeof(s_cfg.wait), "%s", v);
            else if (!strcmp(argv[i], "--today"))
                snprintf(s_cfg.today, sizeof(s_cfg.today), "%s", v);
            else if (!strcmp(argv[i], "--cat"))
                s_cfg.cat = cat_parse(v);
            else if (!strcmp(argv[i], "--lat")) s_cfg.lat = atof(v);
            else if (!strcmp(argv[i], "--lon")) s_cfg.lon = atof(v);
            else if (!strcmp(argv[i], "--interval"))
                s_cfg.interval_s = atoi(v);
            else if (!strcmp(argv[i], "--ttl")) s_cfg.ttl_s = atoi(v);
            else if (!strcmp(argv[i], "--radius-m"))
                s_cfg.radius_m = atoi(v);
            else continue;
            i++;
        }
    }

    if (ui) return ui_main(http_port);

    if (!s_cfg.port[0] || !s_cfg.name[0] || !s_cfg.note[0] ||
        s_cfg.lat == 0.0) {
        fprintf(stderr,
            "usage: waycast-composer --ui   (local web UI)\n"
            "   or: waycast-composer --port /dev/cu.X --name \"Shop\" "
            "--lat .. --lon .. --note \"...\"\n"
            "       [--cat lodging|fuel|event|aid|info] [--hours ..] "
            "[--wait ..] [--today ..] [--interval s]\n");
        return 1;
    }

    s_origin = fnv1a(s_cfg.name);
    s_fd = radio_open(s_cfg.port);
    if (s_fd < 0) { perror("open port"); return 1; }
    s_cfg.running = 1;
    printf("waycast-composer: \"%s\" on air as 0x%08X (every %ds)\n",
           s_cfg.name, s_origin, s_cfg.interval_s);

    radio_thread(NULL); /* never returns */
    return 0;
}
