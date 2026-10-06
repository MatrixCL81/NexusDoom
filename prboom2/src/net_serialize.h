//
// Copyright(C) 2026 dsda-doom contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// DESCRIPTION:
//  Ticcmd and settings serialization for multiplayer
//

#ifndef __NET_SERIALIZE__
#define __NET_SERIALIZE__

#include "d_ticcmd.h"
#include "net_defs.h"

// Serialize a ticcmd_t to buffer. Returns bytes written (NET_TICCMD_SIZE).
int net_write_ticcmd(unsigned char *buf, const ticcmd_t *cmd);

// Deserialize a ticcmd_t from buffer. Returns bytes read (NET_TICCMD_SIZE).
int net_read_ticcmd(const unsigned char *buf, ticcmd_t *cmd);

// Serialize net_setup_t to buffer. Returns bytes written.
int net_write_setup(unsigned char *buf, const net_setup_t *setup);

// Deserialize net_setup_t from buffer. Returns bytes read.
int net_read_setup(const unsigned char *buf, net_setup_t *setup);

// Serialize a TICCMD message to buffer (at least NET_TICS_MAX_SIZE bytes).
// Returns bytes written.
int net_write_tics(unsigned char *buf, const net_tics_msg_t *msg);

// Deserialize a TICCMD message of the given length.
// Returns bytes read, or -1 if the message is malformed.
int net_read_tics(const unsigned char *buf, int length, net_tics_msg_t *msg);

// HELLO payload: the protocol magic.
int net_write_hello(unsigned char *buf, unsigned int magic);
unsigned int net_read_hello(const unsigned char *buf, int length);

#endif
