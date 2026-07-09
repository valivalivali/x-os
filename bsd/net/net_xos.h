#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Network stack initialization and polling */
void net_init(void);
void net_poll(void);

/* IP output */
int ip_output(uint32_t dst_ip, uint8_t proto, const void *data, int len);

/* ICMP */
int icmp_echo_request(uint32_t dst_ip, uint16_t id, uint16_t seq,
                      const void *data, int data_len);

/* UDP */
int  udp_create(void);
void udp_close(int pcb_idx);
int  udp_bind(int pcb_idx, uint16_t port);
int  udp_connect(int pcb_idx, uint32_t ip, uint16_t port);
int  udp_send(int pcb_idx, const void *data, int len);
int  udp_recv(int pcb_idx, void *buf, int maxlen, uint32_t *src_ip, uint16_t *src_port);

/* TCP */
int  tcp_create(void);
void tcp_close(int pcb_idx);
int  tcp_bind(int pcb_idx, uint16_t port);
int  tcp_listen(int pcb_idx, int backlog);
int  tcp_accept(int pcb_idx);
int  tcp_connect(int pcb_idx, uint32_t ip, uint16_t port);
int  tcp_send(int pcb_idx, const void *data, int len);
int  tcp_recv(int pcb_idx, void *buf, int maxlen);
