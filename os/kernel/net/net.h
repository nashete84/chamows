/* Chamows networking: an Intel e1000 card (what QEMU, VirtualBox and UTM
 * emulate by default), and a small IPv4 stack on top: ARP, ICMP (ping), UDP,
 * DHCP, DNS and TCP. Everything is polled from the desktop loop; there are
 * no interrupts. Addresses are kept in network byte order. */
#ifndef CHAMOWS_NET_H
#define CHAMOWS_NET_H
#include "../kernel.h"

/* ---- the card (e1000.c) ---- */
int nic_init(void);                          /* 0 when a card was found */
int nic_send(const void *frame, u32 len);    /* 0 on success */
int nic_recv(void *frame, u32 max);          /* bytes of one received frame, 0 if none */
extern u8 nic_mac[6];
extern int nic_ready;

/* ---- the stack (net.c) ---- */
void net_init(void);                         /* card + DHCP in the background */
void net_poll(void);                         /* call often: handles incoming packets and timers */
extern u32 net_ip, net_mask, net_gw, net_dns; /* 0 until DHCP answers */
extern u16 net_dns_port;                     /* 53; "red dns" can change server and port */
int net_up(void);                            /* card found and address assigned */
void net_wait(u32 ms);                       /* polls the network for a while */
void net_set_dns(u32 ip, u16 port);          /* a DNS server chosen by hand */

static inline u16 htons(u16 v) { return (u16)(v << 8 | v >> 8); }
static inline u32 htonl(u32 v) { return v << 24 | (v & 0xFF00) << 8 | (v >> 8 & 0xFF00) | v >> 24; }
#define ntohs htons
#define ntohl htonl
u32 ip_parse(const char *s);                 /* "10.0.2.2" -> address, 0 if not one */
void ip_str(u32 ip, char *out);              /* address -> "10.0.2.2" (16 bytes) */

int net_ping(u32 ip, u32 timeout_ms);        /* round trip in ms, or -1 */
int net_resolve(const char *name, u32 *ip, u32 timeout_ms); /* name or dotted address; 0 on success */

/* UDP: one port per socket */
int udp_open(u16 local_port);                /* socket id, or -1 (port 0 = any) */
int udp_sendto(int s, u32 ip, u16 port, const void *data, u32 len);
int udp_recvfrom(int s, void *buf, u32 max, u32 *ip, u16 *port); /* bytes, 0 if nothing yet */
void udp_close(int s);
u32 udp_pending(int s);                      /* datagrams waiting */

/* TCP client connections */
enum { TCP_CLOSED, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT, TCP_CLOSE_WAIT, TCP_LAST_ACK, TCP_TIME_WAIT };
int tcp_connect(u32 ip, u16 port);           /* socket id (connecting), or -1 */
int tcp_state(int s);
int tcp_send(int s, const void *data, u32 len); /* bytes queued (may be fewer), -1 if closed */
int tcp_recv(int s, void *buf, u32 max);     /* bytes, 0 if nothing yet, -1 when the peer closed and all was read */
u32 tcp_pending(int s);                      /* bytes waiting to be read */
int tcp_eof(int s);                          /* the peer closed (or the connection broke) */
void tcp_close(int s);                       /* sends FIN; the id is freed when done */

#endif
