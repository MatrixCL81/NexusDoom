//
// Copyright(C) 2026 dsda-doom contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// DESCRIPTION:
//  Multiplayer networking definitions
//

#ifndef __NET_DEFS__
#define __NET_DEFS__

#include "doomtype.h"
#include "d_ticcmd.h"

#define NET_DEFAULT_PORT 26101
#define NET_TICCMD_SIZE  15
#define NET_KF_FILENAME_MAX 256
#define NET_SETUP_SIZE (11 * 4 + NET_KF_FILENAME_MAX)

// First field of HELLO. Change it whenever the wire protocol changes, so that
// peers running different builds get a clear error.
#define NET_PROTOCOL_MAGIC 0x4e445501 // "NDU" version 1
#define NET_HELLO_SIZE 4

// Wire protocol message types. All travel over UDP; only TICCMD is made
// reliable (by resending), the handshake messages are repeated instead.
typedef enum {
  NET_MSG_HELLO,    // client -> host: protocol magic, repeated until SETUP
  NET_MSG_SETUP,    // host -> client: game settings
  NET_MSG_READY,    // client -> host: ready to play
  NET_MSG_TICCMD,   // bidirectional: unacknowledged tics + ack + checksum
  NET_MSG_QUIT,     // bidirectional: graceful disconnect (or rejected HELLO)
} net_msg_type_t;

// Wire packet header: [u16 type][u16 payload_length][payload...]
#define NET_HEADER_SIZE 4

// TICCMD payload: every local cmd the peer hasn't acknowledged yet, so a lost
// datagram is covered by the next one. Each peer's cmds form one stream,
// numbered by sequence number, which keeps counting through key-frame restores
// (unlike tics, which jump back). See NetHandleTics in d_client.c.
//   u16 epoch         key-frame restores so far; tags the checksum, so one from
//                     before a restore is never compared with the new timeline
//   u8  flags         NET_TICS_WAITING
//   u32 ack_seq       sender has the receiver's cmds with sequence numbers below this
//   u32 checksum_tic  sender's latest desync checksum (NET_NO_CHECKSUM = none)
//   u32 checksum
//   u32 start_seq     sequence number of the first cmd
//   u8  count         number of cmds, then count * NET_TICCMD_SIZE bytes
#define NET_TICS_HEADER_SIZE 20
#define NET_TICS_MAX_CMDS 12 // size of the unacknowledged-cmd ring
#define NET_TICS_MAX_SIZE (NET_TICS_HEADER_SIZE + NET_TICS_MAX_CMDS * NET_TICCMD_SIZE)
#define NET_NO_CHECKSUM 0xffffffffu

// Set when the sender is waiting for the receiver's next cmd; the receiver
// then resends at once if it sees the sender is missing some of its cmds.
#define NET_TICS_WAITING 1

// Game settings sent from host to client during handshake
typedef struct {
  int skill;
  int episode;
  int map;
  int complevel;
  int deathmatch;
  int nomonsters;
  int fast;
  int respawn;
  int longtics;
  int game_speed; // host's -game_speed value (percent, default 100)
  unsigned int rngseed; // host's RNG seed; both peers must seed identically
  char from_key_frame[NET_KF_FILENAME_MAX]; // host's -from_key_frame filename (empty = none)
} net_setup_t;

// TICCMD message (see the layout above)
typedef struct {
  unsigned short epoch;
  byte flags;
  unsigned int ack_seq;
  unsigned int checksum_tic;
  unsigned int checksum;
  unsigned int start_seq;
  int count;
  ticcmd_t cmds[NET_TICS_MAX_CMDS];
} net_tics_msg_t;

// Session state
typedef enum {
  NET_STATE_DISCONNECTED,
  NET_STATE_CONNECTING,
  NET_STATE_WAITING,
  NET_STATE_PLAYING,
} net_state_t;

// Session info (global multiplayer state)
typedef struct {
  int socket;
  int is_host;
  int local_player;
  int remote_player;
  net_state_t state;
} net_session_t;

extern net_session_t net_session;

#endif
