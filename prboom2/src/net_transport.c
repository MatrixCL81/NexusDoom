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
//  Every packet is one datagram: [u16 type][u16 payload_length][payload].
//  Delivery is not guaranteed; d_client.c resends what the peer hasn't
//  acknowledged.
//

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET socket_t;
  #define SOCKET_INVALID INVALID_SOCKET
  #define SOCKET_ERROR_VAL SOCKET_ERROR
  #define NET_CLOSESOCKET closesocket
  #define NET_LAST_ERROR WSAGetLastError()
  #ifndef SIO_UDP_CONNRESET
    #define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
  #endif
#else
  #include <sys/socket.h>
  #include <sys/types.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int socket_t;
  #define SOCKET_INVALID (-1)
  #define SOCKET_ERROR_VAL (-1)
  #define NET_CLOSESOCKET close
  #define NET_LAST_ERROR errno
#endif

#include <string.h>
#include <stdio.h>

#include "lprintf.h"
#include "i_system.h"
#include "net_transport.h"

void net_transport_init(void)
{
  static int initialized = 0;
  if (initialized)
    return;
  initialized = 1;

#ifdef _WIN32
  WSADATA wsa_data;
  if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
    lprintf(LO_ERROR, "net_transport_init: WSAStartup failed\n");
  }
#endif

  I_AtExit(net_transport_shutdown, true, "net_transport_shutdown", exit_priority_normal);
}

void net_transport_shutdown(void)
{
#ifdef _WIN32
  WSACleanup();
#endif
}

// A failed receive that only means "nothing usable right now": no datagram
// waiting, an interrupted call, or an ICMP "port unreachable" reported for an
// earlier send (the peer isn't running yet, or has quit).
static int net_recv_error_is_transient(void)
{
  int err = NET_LAST_ERROR;

#ifdef _WIN32
  return err == WSAEWOULDBLOCK || err == WSAECONNRESET || err == WSAEMSGSIZE ||
         err == WSAEINTR;
#else
  return err == EWOULDBLOCK || err == EAGAIN || err == ECONNREFUSED ||
         err == EINTR;
#endif
}

int net_udp_open(int port)
{
  socket_t sock;
  struct sockaddr_in addr;

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock == SOCKET_INVALID) {
    lprintf(LO_ERROR, "net_udp_open: socket() failed\n");
    return -1;
  }

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((unsigned short)port);

  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR_VAL) {
    lprintf(LO_ERROR, "net_udp_open: bind() failed on UDP port %d\n", port);
    NET_CLOSESOCKET(sock);
    return -1;
  }

#ifdef _WIN32
  {
    u_long mode = 1;
    BOOL report_connreset = FALSE;
    DWORD bytes = 0;

    ioctlsocket(sock, FIONBIO, &mode);
    // Otherwise every later recv fails with WSAECONNRESET once an ICMP "port
    // unreachable" came back for one of our datagrams.
    WSAIoctl(sock, SIO_UDP_CONNRESET, &report_connreset, sizeof(report_connreset),
             NULL, 0, &bytes, NULL, NULL);
  }
#else
  fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif

  if (port)
    lprintf(LO_INFO, "net_udp_open: listening on UDP port %d\n", port);
  return (int)sock;
}

int net_udp_connect(int socket, const char *address, int port)
{
  struct addrinfo hints, *res;
  char port_str[16];
  int result;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  snprintf(port_str, sizeof(port_str), "%d", port);

  if (getaddrinfo(address, port_str, &hints, &res) != 0 || !res) {
    lprintf(LO_ERROR, "net_udp_connect: cannot resolve '%s'\n", address);
    return -1;
  }

  result = connect((socket_t)socket, res->ai_addr, (int)res->ai_addrlen);
  freeaddrinfo(res);

  if (result == SOCKET_ERROR_VAL) {
    lprintf(LO_ERROR, "net_udp_connect: connect() to %s:%d failed\n", address, port);
    return -1;
  }

  return 0;
}

int net_udp_connect_addr(int socket, const net_addr_t *addr)
{
  struct sockaddr_in sin;

  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = addr->host;
  sin.sin_port = addr->port;

  if (connect((socket_t)socket, (struct sockaddr *)&sin, sizeof(sin)) == SOCKET_ERROR_VAL) {
    lprintf(LO_ERROR, "net_udp_connect_addr: connect() to %s failed\n",
            net_addr_string(addr));
    return -1;
  }

  return 0;
}

const char *net_addr_string(const net_addr_t *addr)
{
  static char str[32];
  const unsigned char *ip = (const unsigned char *)&addr->host;

  snprintf(str, sizeof(str), "%u.%u.%u.%u:%u",
           ip[0], ip[1], ip[2], ip[3], (unsigned int)ntohs(addr->port));
  return str;
}

static int net_build_packet(unsigned char *packet, int type, const void *data, int length)
{
  if (length < 0 || length > NET_MAX_PACKET - NET_HEADER_SIZE) {
    lprintf(LO_ERROR, "net_build_packet: payload too large (%d)\n", length);
    return -1;
  }

  // Type and payload length, big-endian u16 each
  packet[0] = (unsigned char)((type >> 8) & 0xff);
  packet[1] = (unsigned char)(type & 0xff);
  packet[2] = (unsigned char)((length >> 8) & 0xff);
  packet[3] = (unsigned char)(length & 0xff);

  if (length > 0 && data)
    memcpy(packet + NET_HEADER_SIZE, data, length);

  return NET_HEADER_SIZE + length;
}

int net_send_packet(int socket, int type, const void *data, int length)
{
  unsigned char packet[NET_MAX_PACKET];
  int size;

  if (socket < 0)
    return -1;

  size = net_build_packet(packet, type, data, length);
  if (size < 0)
    return -1;

  // A lost or refused datagram is no different from one dropped on the way:
  // the resend logic takes care of it.
  send((socket_t)socket, (const char *)packet, size, 0);
  return 0;
}

int net_send_packet_to(int socket, const net_addr_t *addr, int type,
                       const void *data, int length)
{
  unsigned char packet[NET_MAX_PACKET];
  struct sockaddr_in sin;
  int size;

  if (socket < 0)
    return -1;

  size = net_build_packet(packet, type, data, length);
  if (size < 0)
    return -1;

  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = addr->host;
  sin.sin_port = addr->port;

  sendto((socket_t)socket, (const char *)packet, size, 0,
         (struct sockaddr *)&sin, sizeof(sin));
  return 0;
}

int net_recv_packet_from(int socket, void *data, int *length, int max_length,
                         net_addr_t *from)
{
  unsigned char packet[NET_MAX_PACKET];

  if (socket < 0)
    return -1;

  // Skip malformed datagrams until a valid one or none is left.
  while (1) {
    struct sockaddr_in sin;
    socklen_t sin_len = sizeof(sin);
    int received, type, payload_len;

    received = recvfrom((socket_t)socket, (char *)packet, sizeof(packet), 0,
                        (struct sockaddr *)&sin, &sin_len);

    if (received == SOCKET_ERROR_VAL) {
      if (net_recv_error_is_transient()) {
        // ICMP errors are reported once per datagram; look for more data.
        if (net_wait_for_packet(socket, 0) > 0)
          continue;
        return NET_RECV_NONE;
      }
      lprintf(LO_ERROR, "net_recv_packet: recv() failed (error %d)\n", NET_LAST_ERROR);
      return -1;
    }

    if (received < NET_HEADER_SIZE)
      continue;

    type = (packet[0] << 8) | packet[1];
    payload_len = (packet[2] << 8) | packet[3];

    if (payload_len != received - NET_HEADER_SIZE || payload_len > max_length)
      continue;

    if (payload_len > 0)
      memcpy(data, packet + NET_HEADER_SIZE, payload_len);
    if (length)
      *length = payload_len;
    if (from) {
      from->host = sin.sin_addr.s_addr;
      from->port = sin.sin_port;
    }

    return type;
  }
}

int net_recv_packet(int socket, void *data, int *length, int max_length)
{
  return net_recv_packet_from(socket, data, length, max_length, NULL);
}

int net_recv_packet_timeout(int socket, void *data, int *length, int max_length,
                            int timeout_ms)
{
  int wait_result = net_wait_for_packet(socket, timeout_ms);
  if (wait_result == 0)
    return NET_RECV_TIMEOUT;
  if (wait_result < 0)
    return -1;
  return net_recv_packet(socket, data, length, max_length);
}

int net_wait_for_packet(int socket, int timeout_ms)
{
  fd_set readfds;
  struct timeval tv;
  int result;

  if (socket < 0)
    return -1;

  FD_ZERO(&readfds);
  FD_SET((socket_t)socket, &readfds);

  tv.tv_sec = timeout_ms / 1000;
  tv.tv_usec = (timeout_ms % 1000) * 1000;

  result = select(socket + 1, &readfds, NULL, NULL, &tv);
  if (result > 0)
    return 1;

  if (result == 0)
    return 0;

  return -1;
}

void net_close(int socket)
{
  if (socket >= 0) {
    NET_CLOSESOCKET((socket_t)socket);
  }
}
