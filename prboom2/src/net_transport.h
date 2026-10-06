//
// Copyright(C) 2026 dsda-doom contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// DESCRIPTION:
//  UDP transport layer for multiplayer
//

#ifndef __NET_TRANSPORT__
#define __NET_TRANSPORT__

#include "doomtype.h"
#include "net_defs.h"

// Return value from net_recv_packet_timeout when the select() deadline expires.
#define NET_RECV_TIMEOUT (-2)
// Return value from the receive functions when no valid datagram is waiting.
#define NET_RECV_NONE (-3)

// Largest datagram we send or accept.
#define NET_MAX_PACKET 1400

// An IPv4 address and port, both in network byte order.
typedef struct {
  unsigned int host;
  unsigned short port;
} net_addr_t;

// Initialize / shutdown platform socket library
void net_transport_init(void);
void net_transport_shutdown(void);

// Open a non-blocking UDP socket bound to the given port (0 = any free port).
// Returns the socket or -1.
int net_udp_open(int port);

// Resolve address:port and connect the UDP socket to it, so that plain sends
// go there and only datagrams from there are received. Returns 0 or -1.
int net_udp_connect(int socket, const char *address, int port);

// Connect the UDP socket to an address received with net_recv_packet_from.
int net_udp_connect_addr(int socket, const net_addr_t *addr);

// Format an address as "a.b.c.d:port" (static buffer).
const char *net_addr_string(const net_addr_t *addr);

// Send one packet as a single datagram: [u16 type][u16 length][payload].
// UDP send errors are transient, so this only fails (-1) for an invalid socket.
int net_send_packet(int socket, int type, const void *data, int length);

// Like net_send_packet, for a socket that isn't connected.
int net_send_packet_to(int socket, const net_addr_t *addr, int type,
                       const void *data, int length);

// Receive the next valid datagram without blocking. Malformed datagrams are
// skipped. Returns the message type and fills data and length, NET_RECV_NONE
// if nothing is waiting, or -1 on a socket error.
int net_recv_packet(int socket, void *data, int *length, int max_length);

// Like net_recv_packet, also returning the sender's address.
int net_recv_packet_from(int socket, void *data, int *length, int max_length,
                         net_addr_t *from);

// Like net_recv_packet, but waits up to timeout_ms for a datagram first and
// returns NET_RECV_TIMEOUT if none arrives.
int net_recv_packet_timeout(int socket, void *data, int *length, int max_length,
                            int timeout_ms);

// Wait for the socket to become readable.
// Returns 1 if data is ready, 0 on timeout, -1 on error.
int net_wait_for_packet(int socket, int timeout_ms);

// Close a socket
void net_close(int socket);

#endif
