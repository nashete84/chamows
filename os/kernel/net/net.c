/* A small IPv4 stack: Ethernet, ARP, ICMP echo, UDP, DHCP client, DNS
 * resolver and TCP client connections. Polled; one network card. */
#include "net.h"

u32 net_ip, net_mask, net_gw, net_dns;
u16 net_dns_port = 53;
static int dns_fixed; /* chosen by hand: DHCP keeps it */
static u8 frame[1600];
static u16 ip_id = 1;
static const u8 BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

int net_up(void) { return nic_ready && net_ip; }

/* ---------- helpers ---------- */
static u16 rd16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static u32 rd32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void wr16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void wr32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }

static u32 sum_add(u32 s, const u8 *p, u32 n) {
    for (u32 i = 0; i + 1 < n; i += 2) s += (u32)(p[i] << 8 | p[i + 1]);
    if (n & 1) s += (u32)p[n - 1] << 8;
    return s;
}
static u16 sum_fold(u32 s) {
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (u16)~s;
}
/* UDP/TCP checksum with the IPv4 pseudo header (addresses in network order) */
static u16 l4_sum(u32 src, u32 dst, u8 proto, const u8 *seg, u32 n) {
    u8 ph[12];
    memcpy(ph, &src, 4);
    memcpy(ph + 4, &dst, 4);
    ph[8] = 0; ph[9] = proto;
    wr16(ph + 10, (u16)n);
    return sum_fold(sum_add(sum_add(0, ph, 12), seg, n));
}

u32 ip_parse(const char *s) {
    u32 v = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        u32 n = 0;
        while (*s >= '0' && *s <= '9') { n = n * 10 + (u32)(*s++ - '0'); if (n > 255) return 0; }
        v |= n << (8 * part);
        if (part < 3) { if (*s != '.') return 0; s++; }
    }
    return *s ? 0 : v;
}
void ip_str(u32 ip, char *out) {
    const u8 *b = (const u8 *)&ip;
    ksnprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/* ---------- Ethernet and ARP ---------- */
typedef struct { u32 ip; u8 mac[6]; u32 when; } arp_t;
static arp_t arp[16];
typedef struct { u32 next_hop; u32 len; u32 when; u8 data[1514]; } pending_t;
static pending_t pend[4];

static int eth_send(const u8 *dst, u16 type, const u8 *payload, u32 len) {
    u8 f[1514];
    if (len > 1500) return -1;
    memcpy(f, dst, 6);
    memcpy(f + 6, nic_mac, 6);
    wr16(f + 12, type);
    memcpy(f + 14, payload, len);
    return nic_send(f, 14 + len);
}
static arp_t *arp_find(u32 ip) {
    for (int i = 0; i < 16; i++) if (arp[i].ip == ip && arp[i].when) return &arp[i];
    return NULL;
}
static void arp_learn(u32 ip, const u8 *mac) {
    arp_t *e = arp_find(ip);
    if (!e) { e = &arp[0]; for (int i = 1; i < 16; i++) if (arp[i].when < e->when) e = &arp[i]; }
    e->ip = ip;
    memcpy(e->mac, mac, 6);
    e->when = ticks_ms | 1;
    for (int i = 0; i < 4; i++) /* packets that were waiting for this address */
        if (pend[i].len && pend[i].next_hop == ip) { eth_send(mac, 0x0800, pend[i].data, pend[i].len); pend[i].len = 0; }
}
static void arp_request(u32 ip) {
    u8 p[28];
    wr16(p, 1); wr16(p + 2, 0x0800); p[4] = 6; p[5] = 4; wr16(p + 6, 1);
    memcpy(p + 8, nic_mac, 6); memcpy(p + 14, &net_ip, 4);
    memset(p + 18, 0, 6); memcpy(p + 24, &ip, 4);
    eth_send(BCAST, 0x0806, p, 28);
}
static void arp_input(const u8 *p, u32 n) {
    if (n < 28 || rd16(p) != 1 || rd16(p + 2) != 0x0800) return;
    u32 sip, tip;
    memcpy(&sip, p + 14, 4);
    memcpy(&tip, p + 24, 4);
    if (sip) arp_learn(sip, p + 8);
    if (rd16(p + 6) == 1 && net_ip && tip == net_ip) { /* who has our address: answer */
        u8 r[28];
        memcpy(r, p, 8);
        wr16(r + 6, 2);
        memcpy(r + 8, nic_mac, 6); memcpy(r + 14, &net_ip, 4);
        memcpy(r + 18, p + 8, 6); memcpy(r + 24, &sip, 4);
        eth_send(p + 8, 0x0806, r, 28);
    }
}

/* ---------- IPv4 ---------- */
static void ip_input(const u8 *p, u32 n);
static int ip_send(u32 dst, u8 proto, const u8 *payload, u32 len) {
    u8 pkt[1500];
    if (len > 1480) return -1;
    memset(pkt, 0, 20);
    pkt[0] = 0x45;
    wr16(pkt + 2, (u16)(20 + len));
    wr16(pkt + 4, ip_id++);
    wr16(pkt + 6, 0x4000); /* don't fragment */
    pkt[8] = 64;
    pkt[9] = proto;
    memcpy(pkt + 12, (dst & 0xFF) == 127 ? &dst : &net_ip, 4);
    memcpy(pkt + 16, &dst, 4);
    wr16(pkt + 10, sum_fold(sum_add(0, pkt, 20)));
    memcpy(pkt + 20, payload, len);
    if ((dst & 0xFF) == 127) { ip_input(pkt, 20 + len); return 0; } /* loopback: never leaves the machine */
    if (dst == 0xFFFFFFFF) return eth_send(BCAST, 0x0800, pkt, 20 + len);
    u32 hop = (net_mask && (dst & net_mask) == (net_ip & net_mask)) ? dst : net_gw;
    arp_t *e = arp_find(hop);
    if (e) return eth_send(e->mac, 0x0800, pkt, 20 + len);
    /* unknown address: ask, and keep the packet until the answer comes */
    pending_t *q = &pend[0];
    for (int i = 0; i < 4; i++) if (!pend[i].len || pend[i].when < q->when) q = &pend[i];
    q->next_hop = hop;
    q->len = 20 + len;
    q->when = ticks_ms | 1;
    memcpy(q->data, pkt, 20 + len);
    arp_request(hop);
    return 0;
}

/* ---------- ICMP ---------- */
static u16 ping_seq;
static u32 ping_from, ping_got;
static void icmp_input(u32 src, const u8 *p, u32 n) {
    if (n < 8) return;
    if (p[0] == 8) { /* echo request: answer */
        u8 r[1480];
        if (n > sizeof r) return;
        memcpy(r, p, n);
        r[0] = 0;
        wr16(r + 2, 0);
        wr16(r + 2, sum_fold(sum_add(0, r, n)));
        ip_send(src, 1, r, n);
    } else if (p[0] == 0 && rd16(p + 4) == 0xC4A3 && rd16(p + 6) == ping_seq && src == ping_from) {
        ping_got = 1;
    }
}
int net_ping(u32 ip, u32 timeout_ms) {
    if (!net_up()) return -1;
    u8 p[40];
    memset(p, 0, sizeof p);
    p[0] = 8;
    wr16(p + 4, 0xC4A3);
    wr16(p + 6, ++ping_seq);
    memcpy(p + 8, "Chamows ping ....", 17);
    wr16(p + 2, sum_fold(sum_add(0, p, sizeof p)));
    ping_from = ip;
    ping_got = 0;
    u32 start = ticks_ms, resent = 0;
    ip_send(ip, 1, p, sizeof p);
    while (!ping_got && ticks_ms - start < timeout_ms) {
        net_poll();
        if (!resent && ticks_ms - start > 300 && !arp_find(ip) && !arp_find(net_gw)) { resent = 1; ip_send(ip, 1, p, sizeof p); }
        __asm__ volatile("pause");
    }
    return ping_got ? (int)(ticks_ms - start) : -1;
}

/* ---------- UDP ---------- */
#define NUDP 8
#define UDPQ 8
typedef struct { u32 ip; u16 port, len; u8 data[1472]; } dgram_t;
typedef struct { int used; u16 port; dgram_t q[UDPQ]; int head, count; } udpsock_t;
static udpsock_t udps[NUDP];
static u16 next_port = 49152;

static u16 ephemeral(void) { if (next_port < 49152) next_port = 49152; return next_port++; }
int udp_open(u16 port) {
    for (int i = 0; i < NUDP; i++)
        if (!udps[i].used) {
            memset(&udps[i], 0, sizeof udps[i]);
            udps[i].used = 1;
            udps[i].port = port ? port : ephemeral();
            return i;
        }
    return -1;
}
u32 udp_pending(int s) { return s >= 0 && s < NUDP && udps[s].used ? (u32)udps[s].count : 0; }
void udp_close(int s) { if (s >= 0 && s < NUDP) udps[s].used = 0; }
static int udp_out(u16 sport, u32 ip, u16 dport, const void *data, u32 len) {
    u8 seg[1480];
    if (len > 1472) return -1;
    wr16(seg, sport); wr16(seg + 2, dport); wr16(seg + 4, (u16)(8 + len)); wr16(seg + 6, 0);
    memcpy(seg + 8, data, len);
    u16 c = l4_sum(net_ip, ip, 17, seg, 8 + len);
    wr16(seg + 6, c ? c : 0xFFFF);
    return ip_send(ip, 17, seg, 8 + len);
}
int udp_sendto(int s, u32 ip, u16 port, const void *data, u32 len) {
    if (s < 0 || s >= NUDP || !udps[s].used) return -1;
    return udp_out(udps[s].port, ip, port, data, len);
}
int udp_recvfrom(int s, void *buf, u32 max, u32 *ip, u16 *port) {
    if (s < 0 || s >= NUDP || !udps[s].used || !udps[s].count) return 0;
    dgram_t *d = &udps[s].q[udps[s].head];
    u32 n = d->len < max ? d->len : max;
    memcpy(buf, d->data, n);
    if (ip) *ip = d->ip;
    if (port) *port = d->port;
    udps[s].head = (udps[s].head + 1) % UDPQ;
    udps[s].count--;
    return (int)n;
}

/* ---------- DHCP ---------- */
static int dhcp_state; /* 0 idle, 1 discover sent, 2 request sent, 3 bound */
static u32 dhcp_xid, dhcp_offer, dhcp_server, dhcp_next;
static void dhcp_send(int type) {
    u8 m[300];
    memset(m, 0, sizeof m);
    m[0] = 1; m[1] = 1; m[2] = 6;
    wr32(m + 4, dhcp_xid);
    wr16(m + 10, 0x8000); /* answer by broadcast */
    memcpy(m + 28, nic_mac, 6);
    wr32(m + 236, 0x63825363);
    u8 *o = m + 240;
    *o++ = 53; *o++ = 1; *o++ = (u8)type;
    if (type == 3) {
        *o++ = 50; *o++ = 4; memcpy(o, &dhcp_offer, 4); o += 4;
        *o++ = 54; *o++ = 4; memcpy(o, &dhcp_server, 4); o += 4;
    }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6; /* mask, router, DNS */
    *o++ = 12; *o++ = 7; memcpy(o, "chamows", 7); o += 7;
    *o++ = 255;
    u32 saved = net_ip;
    net_ip = 0;
    udp_out(68, 0xFFFFFFFF, 67, m, (u32)(o - m));
    net_ip = saved;
}
static void dhcp_input(const u8 *m, u32 n) {
    if (n < 240 || m[0] != 2 || rd32(m + 4) != dhcp_xid || rd32(m + 236) != 0x63825363) return;
    int type = 0;
    u32 mask = 0, gw = 0, dns = 0, server = 0;
    for (u32 i = 240; i < n && m[i] != 255;) {
        u8 code = m[i];
        if (code == 0) { i++; continue; }
        if (i + 1 >= n) break;
        u8 len = m[i + 1];
        const u8 *v = m + i + 2;
        if (i + 2 + len > n) break;
        if (code == 53 && len >= 1) type = v[0];
        if (code == 1 && len >= 4) memcpy(&mask, v, 4);
        if (code == 3 && len >= 4) memcpy(&gw, v, 4);
        if (code == 6 && len >= 4) memcpy(&dns, v, 4);
        if (code == 54 && len >= 4) memcpy(&server, v, 4);
        i += 2u + len;
    }
    u32 yi;
    memcpy(&yi, m + 16, 4);
    if (type == 2 && dhcp_state == 1) { /* offer */
        dhcp_offer = yi;
        dhcp_server = server;
        dhcp_state = 2;
        dhcp_send(3);
        dhcp_next = ticks_ms + 2000;
    } else if (type == 5 && dhcp_state == 2) { /* ack */
        net_ip = yi; net_mask = mask; net_gw = gw; if (!dns_fixed) net_dns = dns ? dns : gw;
        dhcp_state = 3;
        char a[16], g[16], d[16];
        ip_str(net_ip, a); ip_str(net_gw, g); ip_str(net_dns, d);
        klog("red: dirección %s, puerta de enlace %s, DNS %s\n", a, g, d);
    }
}

/* ---------- DNS ---------- */
static int dns_skip(const u8 *m, u32 n, u32 i) {
    while (i < n) {
        if (m[i] == 0) return (int)i + 1;
        if ((m[i] & 0xC0) == 0xC0) return (int)i + 2;
        i += (u32)m[i] + 1;
    }
    return -1;
}
int net_resolve(const char *name, u32 *ip, u32 timeout_ms) {
    u32 v = ip_parse(name);
    if (v) { *ip = v; return 0; }
    if (!strcmp(name, "localhost")) { *ip = 0x0100007F; return 0; }
    if (!net_up() || !net_dns) return -1;
    u8 q[300];
    u32 n = 12;
    memset(q, 0, 12);
    u16 id = (u16)(ticks_ms * 7 + 1);
    wr16(q, id); wr16(q + 2, 0x0100); wr16(q + 4, 1);
    for (const char *p = name; *p;) { /* labels */
        const char *e = p;
        while (*e && *e != '.') e++;
        u32 l = (u32)(e - p);
        if (l == 0 || l > 63 || n + l + 6 > sizeof q) return -1;
        q[n++] = (u8)l;
        memcpy(q + n, p, l);
        n += l;
        p = *e ? e + 1 : e;
    }
    q[n++] = 0;
    wr16(q + n, 1); wr16(q + n + 2, 1); /* A, IN */
    n += 4;
    int s = udp_open(0);
    if (s < 0) return -1;
    u32 start = ticks_ms, sent = 0;
    int rc = -1;
    while (ticks_ms - start < timeout_ms && rc < 0) {
        if (ticks_ms - sent > 1000) { udp_sendto(s, net_dns, net_dns_port, q, n); sent = ticks_ms; }
        net_poll();
        u8 r[1472];
        u32 from;
        u16 port;
        int got = udp_recvfrom(s, r, sizeof r, &from, &port);
        if (got < 12 || rd16(r) != id) continue;
        if ((r[3] & 0x0F) != 0) { rc = -2; break; } /* no such name */
        u32 an = rd16(r + 6), qd = rd16(r + 4);
        int i = 12;
        for (u32 k = 0; k < qd && i > 0; k++) { i = dns_skip(r, (u32)got, (u32)i); i += 4; }
        for (u32 k = 0; k < an && i > 0 && i + 10 <= got; k++) {
            i = dns_skip(r, (u32)got, (u32)i);
            if (i < 0 || i + 10 > got) break;
            u16 type = rd16(r + i), len = rd16(r + i + 8);
            if (type == 1 && len == 4 && i + 14 <= got) { memcpy(ip, r + i + 10, 4); rc = 0; break; }
            i += 10 + len;
        }
        if (rc < 0) { rc = -2; break; }
    }
    udp_close(s);
    return rc == 0 ? 0 : -1;
}

/* ---------- TCP ---------- */
#define NTCP 16
#define TBUF 65536u
#define MSS 1460u
typedef struct {
    int state, closing, fin_sent, peer_fin;
    u32 ip;
    u16 lport, rport;
    u32 iss, snd_una, snd_nxt, rcv_nxt, snd_wnd;
    u8 *sbuf, *rbuf;   /* sbuf: bytes from snd_una on; rbuf: a ring */
    u32 slen, rhead, rlen;
    u32 last_send, rto, tries, when_closed;
} tcb_t;
static tcb_t tcbs[NTCP];

static tcb_t *T(int s) { return s >= 0 && s < NTCP && tcbs[s].state != TCP_CLOSED && tcbs[s].sbuf ? &tcbs[s] : NULL; }
static u32 rfree(tcb_t *t) { return TBUF - t->rlen; }

static void tcp_seg(tcb_t *t, u32 seq, u8 flags, const u8 *data, u32 len) {
    u8 seg[20 + 4 + MSS];
    u32 hl = (flags & 2) ? 24 : 20;
    memset(seg, 0, hl);
    wr16(seg, t->lport); wr16(seg + 2, t->rport);
    wr32(seg + 4, seq);
    wr32(seg + 8, (flags & 0x10) ? t->rcv_nxt : 0);
    seg[12] = (u8)(hl / 4 << 4);
    seg[13] = flags;
    u32 w = rfree(t);
    wr16(seg + 14, (u16)(w > 65535 ? 65535 : w));
    if (hl == 24) { seg[20] = 2; seg[21] = 4; wr16(seg + 22, MSS); } /* MSS option */
    if (len) memcpy(seg + hl, data, len);
    wr16(seg + 16, l4_sum(net_ip, t->ip, 6, seg, hl + len));
    ip_send(t->ip, 6, seg, hl + len);
}
static void tcp_ack(tcb_t *t) { tcp_seg(t, t->snd_nxt, 0x10, NULL, 0); }

/* sends what the window allows; also the FIN once everything else is out */
static void tcp_output(tcb_t *t) {
    if (t->state == TCP_SYN_SENT) return;
    u32 inflight = t->snd_nxt - t->snd_una - (t->fin_sent ? 1 : 0);
    u32 wnd = t->snd_wnd < 4 * MSS ? t->snd_wnd : 4 * MSS;
    if (wnd < MSS && wnd < t->slen) wnd = wnd ? wnd : 1; /* probe a closed window with one byte */
    while (inflight < t->slen && inflight < wnd && !t->fin_sent) {
        u32 n = t->slen - inflight;
        if (n > MSS) n = MSS;
        if (n > wnd - inflight) n = wnd - inflight;
        u8 buf[MSS];
        memcpy(buf, t->sbuf + inflight, n);
        tcp_seg(t, t->snd_una + inflight, 0x18, buf, n); /* ACK|PSH */
        inflight += n;
        t->snd_nxt = t->snd_una + inflight;
        t->last_send = ticks_ms;
    }
    if (t->closing && !t->fin_sent && inflight == t->slen && (t->state == TCP_ESTABLISHED || t->state == TCP_CLOSE_WAIT)) {
        tcp_seg(t, t->snd_nxt, 0x11, NULL, 0); /* FIN|ACK */
        t->snd_nxt++;
        t->fin_sent = 1;
        t->state = t->state == TCP_CLOSE_WAIT ? TCP_LAST_ACK : TCP_FIN_WAIT;
        t->last_send = ticks_ms;
    }
}
static void tcp_free(tcb_t *t) {
    kfree(t->sbuf);
    kfree(t->rbuf);
    memset(t, 0, sizeof *t);
}

int tcp_connect(u32 ip, u16 port) {
    if (!net_up()) return -1;
    for (int i = 0; i < NTCP; i++) {
        tcb_t *t = &tcbs[i];
        if (t->state != TCP_CLOSED) continue;
        memset(t, 0, sizeof *t);
        t->sbuf = kmalloc(TBUF);
        t->rbuf = kmalloc(TBUF);
        if (!t->sbuf || !t->rbuf) { kfree(t->sbuf); kfree(t->rbuf); return -1; }
        t->ip = ip;
        t->rport = port;
        t->lport = ephemeral();
        t->iss = ticks_ms * 2654435761u + (u32)i * 7919;
        t->snd_una = t->iss;
        t->snd_nxt = t->iss + 1;
        t->rto = 1000;
        t->state = TCP_SYN_SENT;
        t->last_send = ticks_ms;
        tcp_seg(t, t->iss, 0x02, NULL, 0); /* SYN */
        return i;
    }
    return -1;
}
int tcp_state(int s) { tcb_t *t = T(s); return t ? t->state : TCP_CLOSED; }
int tcp_send(int s, const void *data, u32 len) {
    tcb_t *t = T(s);
    if (!t || t->closing || (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT && t->state != TCP_SYN_SENT)) return -1;
    u32 room = TBUF - t->slen, n = len < room ? len : room;
    memcpy(t->sbuf + t->slen, data, n);
    t->slen += n;
    tcp_output(t);
    return (int)n;
}
int tcp_eof(int s) { tcb_t *t = T(s); return !t || t->peer_fin; }
u32 tcp_pending(int s) { tcb_t *t = T(s); return t ? t->rlen : 0; }
int tcp_recv(int s, void *buf, u32 max) {
    tcb_t *t = T(s);
    if (!t) return -1;
    if (!t->rlen) return t->peer_fin || t->state == TCP_CLOSED ? -1 : 0;
    u32 before = rfree(t);
    u32 n = t->rlen < max ? t->rlen : max;
    for (u32 i = 0; i < n; i++) ((u8 *)buf)[i] = t->rbuf[(t->rhead + i) % TBUF];
    t->rhead = (t->rhead + n) % TBUF;
    t->rlen -= n;
    if (before < 2 * MSS && rfree(t) >= 2 * MSS && t->state == TCP_ESTABLISHED) tcp_ack(t); /* window opened again */
    return (int)n;
}
void tcp_close(int s) {
    tcb_t *t = T(s);
    if (!t) return;
    t->closing = 1;
    t->when_closed = ticks_ms;
    if (t->state == TCP_SYN_SENT) { tcp_free(t); return; }
    tcp_output(t);
}

static void tcp_input(u32 src, const u8 *p, u32 n) {
    if (n < 20) return;
    u16 sport = rd16(p), dport = rd16(p + 2);
    u32 seq = rd32(p + 4), ack = rd32(p + 8);
    u32 hl = (u32)(p[12] >> 4) * 4;
    u8 fl = p[13];
    if (hl < 20 || hl > n) return;
    const u8 *data = p + hl;
    u32 len = n - hl;
    tcb_t *t = NULL;
    for (int i = 0; i < NTCP; i++)
        if (tcbs[i].state != TCP_CLOSED && tcbs[i].lport == dport && tcbs[i].rport == sport && tcbs[i].ip == src) t = &tcbs[i];
    if (!t) return;
    if (fl & 0x04) { /* RST */
        t->state = TCP_CLOSED;
        t->peer_fin = 1;
        if (t->closing) tcp_free(t);
        return;
    }
    if (t->state == TCP_SYN_SENT) {
        if ((fl & 0x12) == 0x12 && ack == t->iss + 1) {
            t->rcv_nxt = seq + 1;
            t->snd_una = ack;
            t->snd_wnd = rd16(p + 14);
            t->state = TCP_ESTABLISHED;
            tcp_ack(t);
            tcp_output(t);
        }
        return;
    }
    if (fl & 0x10) { /* ACK: drop what the peer has */
        u32 acked = ack - t->snd_una, out = t->snd_nxt - t->snd_una;
        if (acked && acked <= out) {
            u32 bytes = acked;
            if (t->fin_sent && ack == t->snd_nxt) bytes--; /* the FIN is not in the buffer */
            if (bytes > t->slen) bytes = t->slen;
            memmove(t->sbuf, t->sbuf + bytes, t->slen - bytes); /* the buffer starts at snd_una */
            t->slen -= bytes;
            t->snd_una = ack;
            t->tries = 0;
            t->rto = 1000;
            t->last_send = ticks_ms;
        }
        t->snd_wnd = rd16(p + 14);
        if (t->fin_sent && ack == t->snd_nxt) {
            if (t->state == TCP_LAST_ACK) { tcp_free(t); return; }
            if (t->state == TCP_FIN_WAIT && t->peer_fin) { tcp_free(t); return; }
        }
    }
    int need_ack = 0;
    if (len) {
        if (seq == t->rcv_nxt && !t->peer_fin) {
            u32 take = len < rfree(t) ? len : rfree(t);
            for (u32 i = 0; i < take; i++) t->rbuf[(t->rhead + t->rlen + i) % TBUF] = data[i];
            t->rlen += take;
            t->rcv_nxt += take;
        }
        need_ack = 1; /* in order or not: tell the peer what we expect */
    }
    if ((fl & 0x01) && seq + len == t->rcv_nxt && !t->peer_fin) { /* FIN, in order */
        t->rcv_nxt++;
        t->peer_fin = 1;
        need_ack = 1;
        if (t->state == TCP_ESTABLISHED) t->state = TCP_CLOSE_WAIT;
        else if (t->state == TCP_FIN_WAIT) t->state = TCP_TIME_WAIT;
    }
    if (need_ack) tcp_ack(t);
    if (t->state == TCP_TIME_WAIT && t->closing) { tcp_free(t); return; }
    tcp_output(t);
}
static void tcp_timers(void) {
    for (int i = 0; i < NTCP; i++) {
        tcb_t *t = &tcbs[i];
        if (t->state == TCP_CLOSED || !t->sbuf) continue;
        if (t->closing && ticks_ms - t->when_closed > 10000) { tcp_free(t); continue; } /* give up on a slow close */
        int waiting = t->state == TCP_SYN_SENT || t->snd_nxt != t->snd_una;
        if (!waiting || ticks_ms - t->last_send < t->rto) continue;
        if (++t->tries > 6) { t->state = TCP_CLOSED; t->peer_fin = 1; if (t->closing) tcp_free(t); continue; }
        t->rto = t->rto < 8000 ? t->rto * 2 : 8000;
        t->last_send = ticks_ms;
        if (t->state == TCP_SYN_SENT) { tcp_seg(t, t->iss, 0x02, NULL, 0); continue; }
        /* go back to the oldest unacknowledged byte and send again */
        if (t->fin_sent && t->snd_nxt - t->snd_una == 1 && !t->slen) { tcp_seg(t, t->snd_una, 0x11, NULL, 0); continue; }
        t->snd_nxt = t->snd_una;
        if (t->fin_sent) { t->fin_sent = 0; t->state = t->state == TCP_LAST_ACK ? TCP_CLOSE_WAIT : TCP_ESTABLISHED; }
        tcp_output(t);
    }
}

/* ---------- input and timers ---------- */
static void ip_input(const u8 *p, u32 n) {
    if (n < 20 || (p[0] >> 4) != 4) return;
    u32 hl = (u32)(p[0] & 15) * 4, tot = rd16(p + 2);
    if (hl < 20 || tot > n || tot < hl) return;
    if (rd16(p + 6) & 0x3FFF) return; /* fragments are not supported */
    u32 src, dst;
    memcpy(&src, p + 12, 4);
    memcpy(&dst, p + 16, 4);
    if (net_ip && dst != net_ip && dst != 0xFFFFFFFF && (dst & 0xFF) != 127) return;
    const u8 *q = p + hl;
    u32 len = tot - hl;
    if (p[9] == 1) icmp_input(src, q, len);
    else if (p[9] == 6) tcp_input(src, q, len);
    else if (p[9] == 17 && len >= 8) {
        u16 sport = rd16(q), dport = rd16(q + 2), ulen = rd16(q + 4);
        if (ulen < 8 || ulen > len) return;
        if (dport == 68) { dhcp_input(q + 8, ulen - 8u); return; }
        for (int i = 0; i < NUDP; i++) {
            udpsock_t *u = &udps[i];
            if (!u->used || u->port != dport || u->count == UDPQ) continue;
            dgram_t *d = &u->q[(u->head + u->count) % UDPQ];
            d->ip = src;
            d->port = sport;
            d->len = (u16)((u32)ulen - 8u < (u32)sizeof d->data ? (u32)ulen - 8u : (u32)sizeof d->data);
            memcpy(d->data, q + 8, d->len);
            u->count++;
            break;
        }
    }
}

void net_poll(void) {
    if (!nic_ready) return;
    static int busy;
    if (busy) return; /* net_wait inside a callback */
    busy = 1;
    for (int k = 0; k < 64; k++) {
        int n = nic_recv(frame, sizeof frame);
        if (n <= 0) break;
        if (n < 14) continue;
        u16 type = rd16(frame + 12);
        if (type == 0x0806) arp_input(frame + 14, (u32)n - 14);
        else if (type == 0x0800) ip_input(frame + 14, (u32)n - 14);
    }
    if (dhcp_state != 3 && (i32)(ticks_ms - dhcp_next) >= 0) { /* (re)start DHCP */
        dhcp_state = 1;
        dhcp_xid = ticks_ms * 2246822519u + 1;
        dhcp_send(1);
        dhcp_next = ticks_ms + 3000;
    }
    for (int i = 0; i < 4; i++) /* ARP answers that never came */
        if (pend[i].len && ticks_ms - pend[i].when > 3000) pend[i].len = 0;
    tcp_timers();
    busy = 0;
}

void net_set_dns(u32 ip, u16 port) { net_dns = ip; net_dns_port = port ? port : 53; dns_fixed = 1; }

void net_wait(u32 ms) {
    u32 start = ticks_ms;
    while (ticks_ms - start < ms) { net_poll(); __asm__ volatile("pause"); }
}

void net_init(void) {
    if (nic_init() != 0) return;
    dhcp_next = ticks_ms;
    net_poll();
}
