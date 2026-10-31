/* Winsock (WS2_32) for Windows programs, on top of the Chamows network stack:
 * TCP client sockets, UDP sockets, blocking and non-blocking modes, select,
 * WSAAsyncSelect (window messages, which Qt uses), names and addresses.
 * Blocking calls let the other threads and the desktop run while they wait.
 * Included from win32.c after wgui.inc (64-bit only). */

#define NSOCK 32
#define SOCK_BASE 0x100
#define WSAEWOULDBLOCK 10035
#define WSAEINPROGRESS 10036
#define WSAENOTSOCK 10038
#define WSAEAFNOSUPPORT 10047
#define WSAEISCONN 10056
#define WSAENOTCONN 10057
#define WSAECONNREFUSED 10061
#define WSAEHOSTUNREACH 10065
#define WSAECONNRESET 10054
#define WSAETIMEDOUT 10060
#define WSAHOST_NOT_FOUND 11001

typedef struct {
    int used, type;          /* 1 TCP, 2 UDP */
    w32proc_t *owner;
    int tcp, udp;            /* ids in the stack */
    int nonblock, connecting, was_connected, shut_wr;
    u32 rip; u16 rport, lport; /* peer (UDP: default peer after connect) */
    void *hwnd; u32 amsg, aevents, sent_events; /* WSAAsyncSelect */
} wsock_t;
static wsock_t socks[NSOCK];
static int wsa_err;

static wsock_t *S(u64 s) { u64 i = s - SOCK_BASE; return i < NSOCK && socks[i].used ? &socks[i] : NULL; }
static int sock_fail(int e) { wsa_err = e; last_error = (u32)e; return -1; }

static WINAPI int ws_WSAStartup(u16 ver, u8 *data) {
    if (data) {
        memset(data, 0, 408);
        *(u16 *)data = 0x0202;
        *(u16 *)(data + 2) = 0x0202;
        *(u16 *)(data + 4) = NSOCK; /* max sockets */
        strncpy0((char *)data + 16, "Chamows Winsock 2.2", 257);
        strncpy0((char *)data + 273, net_up() ? "Running" : "Sin red", 129);
    }
    return 0;
}
static WINAPI int ws_WSAGetLastError(void) { return wsa_err; }
static WINAPI void ws_WSASetLastError(int e) { wsa_err = e; }

static WINAPI u64 ws_socket(int af, int type, int proto) {
    if (af != 2) { sock_fail(WSAEAFNOSUPPORT); return (u64)-1; } /* IPv4 only */
    if (type != 1 && type != 2) { sock_fail(10044); return (u64)-1; }
    for (int i = 0; i < NSOCK; i++)
        if (!socks[i].used) {
            memset(&socks[i], 0, sizeof socks[i]);
            socks[i].used = 1;
            socks[i].type = type;
            socks[i].owner = cur;
            socks[i].tcp = socks[i].udp = -1;
            return SOCK_BASE + (u64)i;
        }
    sock_fail(10024);
    return (u64)-1;
}
static WINAPI u64 ws_WSASocketW(int af, int type, int proto, void *info, u32 g, u32 fl) { return ws_socket(af, type, proto); }

static void sock_release(wsock_t *k) {
    if (k->tcp >= 0) tcp_close(k->tcp);
    if (k->udp >= 0) udp_close(k->udp);
    k->used = 0;
}
static WINAPI int ws_closesocket(u64 s) {
    wsock_t *k = S(s);
    if (!k) return sock_fail(WSAENOTSOCK);
    sock_release(k);
    return 0;
}

static int udp_ready(wsock_t *k) {
    if (k->udp < 0) { k->udp = udp_open(k->lport); if (k->udp < 0) return -1; }
    return 0;
}
static WINAPI int ws_bind(u64 s, const u8 *sa, int len) {
    wsock_t *k = S(s);
    if (!k) return sock_fail(WSAENOTSOCK);
    k->lport = (u16)(sa[2] << 8 | sa[3]);
    return 0;
}

/* blocking waits: let everything else run until cond() or the timeout */
#define WAIT_UNTIL(cond, ms) do { u32 t0_ = ticks_ms; while (!(cond)) { if ((u32)(ms) != 0xFFFFFFFF && ticks_ms - t0_ >= (u32)(ms)) break; flush_windows(cur); yield(W_RUN); } } while (0)

static WINAPI int ws_connect(u64 s, const u8 *sa, int len) {
    wsock_t *k = S(s);
    if (!k) return sock_fail(WSAENOTSOCK);
    if (*(const u16 *)sa != 2) return sock_fail(WSAEAFNOSUPPORT);
    u32 ip;
    memcpy(&ip, sa + 4, 4);
    u16 port = (u16)(sa[2] << 8 | sa[3]);
    k->rip = ip;
    k->rport = port;
    if (k->type == 2) return 0; /* UDP: just remember the peer */
    if (k->tcp >= 0) return sock_fail(tcp_state(k->tcp) == TCP_ESTABLISHED ? WSAEISCONN : 10037);
    if (!net_up()) return sock_fail(WSAEHOSTUNREACH);
    k->tcp = tcp_connect(ip, port);
    if (k->tcp < 0) return sock_fail(10055);
    k->connecting = 1;
    if (k->nonblock) return sock_fail(WSAEWOULDBLOCK);
    WAIT_UNTIL(tcp_state(k->tcp) != TCP_SYN_SENT, 0xFFFFFFFF);
    k->connecting = 0;
    if (tcp_state(k->tcp) != TCP_ESTABLISHED) return sock_fail(WSAECONNREFUSED);
    k->was_connected = 1;
    return 0;
}
static WINAPI int ws_WSAConnect(u64 s, const u8 *sa, int len, void *a, void *b, void *c, void *d) { return ws_connect(s, sa, len); }

static int tcp_alive(wsock_t *k) {
    int st = k->tcp >= 0 ? tcp_state(k->tcp) : TCP_CLOSED;
    return st == TCP_ESTABLISHED || st == TCP_CLOSE_WAIT;
}
static WINAPI int ws_send(u64 s, const char *buf, int len, int fl) {
    wsock_t *k = S(s);
    if (!k) return sock_fail(WSAENOTSOCK);
    if (k->type == 2) {
        if (!k->rport) return sock_fail(WSAENOTCONN);
        if (udp_ready(k) != 0) return sock_fail(10055);
        return udp_sendto(k->udp, k->rip, k->rport, buf, (u32)len) == 0 ? len : sock_fail(WSAEHOSTUNREACH);
    }
    if (k->tcp < 0 || k->shut_wr) return sock_fail(WSAENOTCONN);
    int done = 0;
    while (done < len) {
        if (!tcp_alive(k)) return done ? done : sock_fail(WSAECONNRESET);
        int n = tcp_send(k->tcp, buf + done, (u32)(len - done));
        if (n > 0) { done += n; continue; }
        if (k->nonblock) return done ? done : sock_fail(WSAEWOULDBLOCK);
        flush_windows(cur);
        yield(W_RUN); /* send buffer full: wait for acks */
    }
    return done;
}
static WINAPI int ws_recv(u64 s, char *buf, int len, int fl) {
    wsock_t *k = S(s);
    if (!k) return sock_fail(WSAENOTSOCK);
    if (k->type == 2) {
        if (udp_ready(k) != 0) return sock_fail(10055);
        int n;
        for (;;) {
            n = udp_recvfrom(k->udp, buf, (u32)len, NULL, NULL);
            if (n > 0 || k->nonblock) break;
            yield(W_RUN);
        }
        return n > 0 ? n : sock_fail(WSAEWOULDBLOCK);
    }
    if (k->tcp < 0) return sock_fail(WSAENOTCONN);
    k->sent_events &= ~1u; /* like Windows: a recv call re-arms FD_READ */
    for (;;) {
        int n = tcp_recv(k->tcp, buf, (u32)len);
        if (n > 0) return n;
        if (n < 0) return tcp_state(k->tcp) == TCP_CLOSED && !k->was_connected ? sock_fail(WSAECONNRESET) : 0; /* 0: the peer closed */
        if (k->nonblock) return sock_fail(WSAEWOULDBLOCK);
        flush_windows(cur);
        yield(W_RUN);
    }
}
static WINAPI int ws_sendto(u64 s, const char *buf, int len, int fl, const u8 *to, int tolen) {
    wsock_t *k = S(s);
    if (!k) return sock_fail(WSAENOTSOCK);
    if (k->type != 2 || !to) return ws_send(s, buf, len, fl);
    if (udp_ready(k) != 0) return sock_fail(10055);
    u32 ip;
    memcpy(&ip, to + 4, 4);
    return udp_sendto(k->udp, ip, (u16)(to[2] << 8 | to[3]), buf, (u32)len) == 0 ? len : sock_fail(WSAEHOSTUNREACH);
}
static WINAPI int ws_recvfrom(u64 s, char *buf, int len, int fl, u8 *from, int *fromlen) {
    wsock_t *k = S(s);
    if (!k) return sock_fail(WSAENOTSOCK);
    if (k->type != 2) return ws_recv(s, buf, len, fl);
    if (udp_ready(k) != 0) return sock_fail(10055);
    u32 ip;
    u16 port;
    int n;
    for (;;) {
        n = udp_recvfrom(k->udp, buf, (u32)len, &ip, &port);
        if (n > 0 || k->nonblock) break;
        yield(W_RUN);
    }
    if (n <= 0) return sock_fail(WSAEWOULDBLOCK);
    if (from && fromlen && *fromlen >= 16) {
        memset(from, 0, 16);
        *(u16 *)from = 2;
        from[2] = (u8)(port >> 8); from[3] = (u8)port;
        memcpy(from + 4, &ip, 4);
        *fromlen = 16;
    }
    return n;
}
/* WSASend/WSARecv with one or more buffers; no overlapped I/O */
typedef struct { u32 len; u32 pad; char *buf; } wsabuf_t;
static WINAPI int ws_WSASend(u64 s, wsabuf_t *b, u32 n, u32 *sent, u32 fl, void *ov, void *cb) {
    u32 total = 0;
    for (u32 i = 0; i < n; i++) {
        int r = ws_send(s, b[i].buf, (int)b[i].len, 0);
        if (r < 0) { if (total) break; return -1; }
        total += (u32)r;
        if ((u32)r < b[i].len) break;
    }
    if (sent) *sent = total;
    return 0;
}
static WINAPI int ws_WSARecv(u64 s, wsabuf_t *b, u32 n, u32 *got, u32 *fl, void *ov, void *cb) {
    int r = n ? ws_recv(s, b[0].buf, (int)b[0].len, 0) : 0;
    if (r < 0) return -1;
    if (got) *got = (u32)r;
    if (fl) *fl = 0;
    return 0;
}
static WINAPI int ws_shutdown(u64 s, int how) {
    wsock_t *k = S(s);
    if (!k) return sock_fail(WSAENOTSOCK);
    if ((how == 1 || how == 2) && k->tcp >= 0 && !k->shut_wr) { k->shut_wr = 1; tcp_close(k->tcp); }
    return 0;
}

/* FIONBIO 0x8004667E, FIONREAD 0x4004667F */
static WINAPI int ws_ioctlsocket(u64 s, u32 cmd, u32 *arg) {
    wsock_t *k = S(s);
    if (!k) return sock_fail(WSAENOTSOCK);
    if (cmd == 0x8004667E) { k->nonblock = *arg != 0; return 0; }
    if (cmd == 0x4004667F) { *arg = k->tcp >= 0 ? tcp_pending(k->tcp) : 0; return 0; }
    return 0;
}
static WINAPI int ws_WSAIoctl(u64 s, u32 code, void *in, u32 inl, void *out, u32 outl, u32 *ret, void *ov, void *cb) {
    if (ret) *ret = 0;
    return sock_fail(10045); /* no extension functions (ConnectEx...) */
}
static WINAPI int ws_setsockopt(u64 s, int lvl, int opt, const char *v, int len) { return S(s) ? 0 : sock_fail(WSAENOTSOCK); }
static WINAPI int ws_getsockopt(u64 s, int lvl, int opt, char *v, int *len) {
    wsock_t *k = S(s);
    if (!k) return sock_fail(WSAENOTSOCK);
    if (v && len && *len >= 4) {
        u32 val = 0;
        if (lvl == 0xFFFF && opt == 0x1007) /* SO_ERROR: how a non-blocking connect went */
            val = k->tcp >= 0 && k->connecting && tcp_state(k->tcp) == TCP_CLOSED ? WSAECONNREFUSED : 0;
        else if (lvl == 0xFFFF && opt == 0x1008) val = (u32)k->type; /* SO_TYPE */
        else if (lvl == 0xFFFF && (opt == 0x1001 || opt == 0x1002)) val = 65536; /* buffer sizes */
        memcpy(v, &val, 4);
        *len = 4;
    }
    return 0;
}
static void fill_sa(u8 *sa, int *len, u32 ip, u16 port) {
    if (!sa || !len || *len < 16) return;
    memset(sa, 0, 16);
    *(u16 *)sa = 2;
    sa[2] = (u8)(port >> 8); sa[3] = (u8)port;
    memcpy(sa + 4, &ip, 4);
    *len = 16;
}
static WINAPI int ws_getpeername(u64 s, u8 *sa, int *len) {
    wsock_t *k = S(s);
    if (!k) return sock_fail(WSAENOTSOCK);
    if (!k->rport) return sock_fail(WSAENOTCONN);
    fill_sa(sa, len, k->rip, k->rport);
    return 0;
}
static WINAPI int ws_getsockname(u64 s, u8 *sa, int *len) {
    wsock_t *k = S(s);
    if (!k) return sock_fail(WSAENOTSOCK);
    fill_sa(sa, len, net_ip, k->lport);
    return 0;
}
static WINAPI int ws_listen(u64 s, int n) { return sock_fail(10045); }  /* no servers yet */
static WINAPI u64 ws_accept(u64 s, u8 *sa, int *len) { sock_fail(10045); return (u64)-1; }

/* ---- readiness: select and WSAAsyncSelect ---- */
static int can_read(wsock_t *k) {
    if (k->type == 2) return k->udp >= 0 && udp_pending(k->udp) > 0;
    if (k->tcp < 0) return 0;
    if (tcp_pending(k->tcp)) return 1;
    return k->was_connected && tcp_eof(k->tcp); /* closed: recv returns 0 */
}
static int can_write(wsock_t *k) {
    if (k->type == 2) return 1;
    if (k->tcp < 0) return 0;
    if (k->connecting && tcp_state(k->tcp) == TCP_ESTABLISHED) { k->connecting = 0; k->was_connected = 1; }
    return tcp_alive(k) && !k->connecting;
}
static int failed(wsock_t *k) { return k->type == 1 && k->connecting && k->tcp >= 0 && tcp_state(k->tcp) == TCP_CLOSED; }

/* fd_set on 64-bit Windows: u32 count, padding, then SOCKETs (u64) */
typedef struct { u32 count, pad; u64 fd[64]; } fdset_t;
static int fd_filter(fdset_t *set, int (*test)(wsock_t *)) {
    if (!set) return 0;
    u32 kept = 0;
    for (u32 i = 0; i < set->count && i < 64; i++) {
        wsock_t *k = S(set->fd[i]);
        if (k && test(k)) set->fd[kept++] = set->fd[i];
    }
    set->count = kept;
    return (int)kept;
}
static int fd_any(const fdset_t *set, int (*test)(wsock_t *)) {
    if (!set) return 0;
    for (u32 i = 0; i < set->count && i < 64; i++) { wsock_t *k = S(set->fd[i]); if (k && test(k)) return 1; }
    return 0;
}
static WINAPI int ws_select(int n, fdset_t *r, fdset_t *w, fdset_t *e, const i32 *tv) {
    u32 ms = tv ? (u32)tv[0] * 1000 + (u32)tv[1] / 1000 : 0xFFFFFFFF;
    WAIT_UNTIL(fd_any(r, can_read) || fd_any(w, can_write) || fd_any(e, failed), ms);
    return fd_filter(r, can_read) + fd_filter(w, can_write) + fd_filter(e, failed);
}
static WINAPI int ws___WSAFDIsSet(u64 s, const fdset_t *set) {
    for (u32 i = 0; i < set->count && i < 64; i++) if (set->fd[i] == s) return 1;
    return 0;
}
/* window messages when a socket becomes readable, writable, connected or closed */
static WINAPI int ws_WSAAsyncSelect(u64 s, void *hwnd, u32 msg, i32 events) {
    wsock_t *k = S(s);
    if (!k) return sock_fail(WSAENOTSOCK);
    k->hwnd = hwnd;
    k->amsg = msg;
    k->aevents = (u32)events;
    k->sent_events = 0;
    if (events) k->nonblock = 1;
    return 0;
}
static void wsock_poll(w32proc_t *p) {
    for (int i = 0; i < NSOCK; i++) {
        wsock_t *k = &socks[i];
        if (!k->used || k->owner != p || !k->aevents || !k->hwnd) continue;
        u64 s = SOCK_BASE + (u64)i;
        if ((k->aevents & 16) && k->connecting && k->tcp >= 0 && tcp_state(k->tcp) != TCP_SYN_SENT) { /* FD_CONNECT */
            int ok = tcp_state(k->tcp) == TCP_ESTABLISHED;
            if (ok) { k->connecting = 0; k->was_connected = 1; } else k->connecting = 0;
            post(p, k->hwnd, k->amsg, s, 16 | (ok ? 0 : (i64)WSAECONNREFUSED << 16));
        }
        if ((k->aevents & 1) && !(k->sent_events & 1) && k->tcp >= 0 && tcp_pending(k->tcp)) { k->sent_events |= 1; post(p, k->hwnd, k->amsg, s, 1); }
        if ((k->aevents & 2) && !(k->sent_events & 2) && k->was_connected && tcp_alive(k)) { k->sent_events |= 2; post(p, k->hwnd, k->amsg, s, 2); }
        if ((k->aevents & 32) && !(k->sent_events & 32) && k->was_connected && tcp_eof(k->tcp) && !tcp_pending(k->tcp)) {
            k->sent_events |= 32;
            post(p, k->hwnd, k->amsg, s, 32);
        }
    }
}
static void wsock_free(w32proc_t *p) {
    for (int i = 0; i < NSOCK; i++) if (socks[i].used && socks[i].owner == p) sock_release(&socks[i]);
}

/* ---- names and addresses ---- */
static WINAPI u16 ws_htons(u16 v) { return htons(v); }
static WINAPI u32 ws_htonl(u32 v) { return htonl(v); }
static WINAPI u32 ws_inet_addr(const char *s) { u32 v = ip_parse(s); return v ? v : 0xFFFFFFFF; }
static WINAPI char *ws_inet_ntoa(u32 ip) { static char b[16]; ip_str(ip, b); return b; }
static WINAPI int ws_inet_pton(int af, const char *s, void *out) { u32 v = ip_parse(s); if (af != 2 || !v) return 0; memcpy(out, &v, 4); return 1; }
static WINAPI const char *ws_inet_ntop(int af, const void *src, char *out, u64 n) {
    char b[16];
    u32 v;
    if (af != 2) return NULL;
    memcpy(&v, src, 4);
    ip_str(v, b);
    strncpy0(out, b, n);
    return out;
}
static WINAPI int ws_gethostname(char *out, int n) { strncpy0(out, "chamows", (u64)n); return 0; }

static int lookup(const char *name, u32 *ip) {
    if (!name || !*name) { *ip = net_ip; return 0; }
    return net_resolve(name, ip, 5000);
}
static WINAPI void *ws_gethostbyname(const char *name) {
    static struct { char *name; char **aliases; i16 type, len; u32 pad; char **list; } he;
    static char hname[64];
    static u32 addr;
    static char *list[2], *aliases[1];
    if (lookup(name, &addr) != 0) { sock_fail(WSAHOST_NOT_FOUND); return NULL; }
    strncpy0(hname, name ? name : "chamows", sizeof hname);
    list[0] = (char *)&addr; list[1] = NULL; aliases[0] = NULL;
    he.name = hname; he.aliases = aliases; he.type = 2; he.len = 4; he.list = list;
    return &he;
}
static WINAPI void *ws_gethostbyaddr(const char *a, int len, int type) { sock_fail(WSAHOST_NOT_FOUND); return NULL; }

/* addrinfo (64-bit): flags, family, socktype, protocol, addrlen (u64), canonname, addr, next */
typedef struct addrinfo_ { i32 flags, family, socktype, protocol; u64 addrlen; char *canon; u8 *addr; struct addrinfo_ *next; } addrinfo_t;
static int make_ai(const char *node, const char *service, const addrinfo_t *hints, addrinfo_t **res) {
    u32 ip = 0;
    if (hints && hints->family != 0 && hints->family != 2) return 10047; /* EAI_FAMILY */
    if (!node && hints && (hints->flags & 1)) ip = 0;                    /* AI_PASSIVE: any */
    else if (!node) ip = 0x0100007F;
    else if (lookup(node, &ip) != 0) return WSAHOST_NOT_FOUND;
    u16 port = 0;
    for (const char *q = service; q && *q >= '0' && *q <= '9'; q++) port = (u16)(port * 10 + (*q - '0'));
    if (service && !port) { /* a few well-known names */
        if (!strcmp(service, "http")) port = 80;
        else if (!strcmp(service, "https")) port = 443;
    }
    addrinfo_t *ai = p_alloc(sizeof *ai + 16, 1);
    u8 *sa = (u8 *)(ai + 1);
    *(u16 *)sa = 2;
    sa[2] = (u8)(port >> 8); sa[3] = (u8)port;
    memcpy(sa + 4, &ip, 4);
    ai->family = 2;
    ai->socktype = hints && hints->socktype ? hints->socktype : 1;
    ai->protocol = hints && hints->protocol ? hints->protocol : (ai->socktype == 2 ? 17 : 6);
    ai->addrlen = 16;
    ai->addr = sa;
    *res = ai;
    return 0;
}
static WINAPI int ws_getaddrinfo(const char *node, const char *service, const addrinfo_t *hints, addrinfo_t **res) {
    int e = make_ai(node, service, hints, res);
    if (e) wsa_err = e;
    return e;
}
static WINAPI int ws_GetAddrInfoW(const u16 *node, const u16 *service, const addrinfo_t *hints, addrinfo_t **res) {
    char n[256], sv[32];
    if (node) w2a(node, n, sizeof n);
    if (service) w2a(service, sv, sizeof sv);
    int e = make_ai(node ? n : NULL, service ? sv : NULL, hints, res);
    if (e) wsa_err = e;
    return e;
}
static WINAPI void ws_freeaddrinfo(addrinfo_t *ai) { while (ai) { addrinfo_t *n = ai->next; p_free(ai); ai = n; } }

static const struct { const char *name; void *fn; } exports_sock[] = {
    {"WSAStartup", ws_WSAStartup}, {"WSACleanup", k_zero}, {"WSAGetLastError", ws_WSAGetLastError},
    {"WSASetLastError", ws_WSASetLastError}, {"socket", ws_socket}, {"WSASocketW", ws_WSASocketW}, {"WSASocketA", ws_WSASocketW},
    {"closesocket", ws_closesocket}, {"bind", ws_bind}, {"connect", ws_connect}, {"WSAConnect", ws_WSAConnect},
    {"send", ws_send}, {"recv", ws_recv}, {"sendto", ws_sendto}, {"recvfrom", ws_recvfrom}, {"WSASend", ws_WSASend},
    {"WSARecv", ws_WSARecv}, {"shutdown", ws_shutdown}, {"ioctlsocket", ws_ioctlsocket}, {"WSAIoctl", ws_WSAIoctl},
    {"setsockopt", ws_setsockopt}, {"getsockopt", ws_getsockopt}, {"getpeername", ws_getpeername},
    {"getsockname", ws_getsockname}, {"listen", ws_listen}, {"accept", ws_accept}, {"select", ws_select},
    {"__WSAFDIsSet", ws___WSAFDIsSet}, {"WSAAsyncSelect", ws_WSAAsyncSelect}, {"htons", ws_htons}, {"ntohs", ws_htons},
    {"htonl", ws_htonl}, {"ntohl", ws_htonl}, {"inet_addr", ws_inet_addr}, {"inet_ntoa", ws_inet_ntoa},
    {"inet_pton", ws_inet_pton}, {"inet_ntop", ws_inet_ntop}, {"gethostname", ws_gethostname},
    {"gethostbyname", ws_gethostbyname}, {"gethostbyaddr", ws_gethostbyaddr}, {"getaddrinfo", ws_getaddrinfo},
    {"GetAddrInfoW", ws_GetAddrInfoW}, {"freeaddrinfo", ws_freeaddrinfo}, {"FreeAddrInfoW", ws_freeaddrinfo},
};
