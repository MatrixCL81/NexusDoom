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
#define NET_CHECKSUM_SIZE 8
#define NET_KF_FILENAME_MAX 256
#define NET_SETUP_SIZE (11 * 4 + NET_KF_FILENAME_MAX)

// Wire protocol message types
typedef enum {
  NET_MSG_SETUP,    // host -> client: game settings
  NET_MSG_READY,    // client -> host: ready to play
  NET_MSG_TICCMD,   // bidirectional: one tic of input
  NET_MSG_CHECKSUM, // bidirectional: periodic desync checksum
  NET_MSG_ADVANCE,  // bidirectional: frame advance request (build mode)
  NET_MSG_QUIT,     // bidirectional: graceful disconnect
} net_msg_type_t;

// Wire packet header: [u16 type][u16 payload_length][payload...]
#define NET_HEADER_SIZE 4

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

// Ticcmd message: sequence number + ticcmd
typedef struct {
  unsigned int gametic;
  ticcmd_t cmd;
} net_ticcmd_msg_t;

// Periodic desync checksum message
typedef struct {
  unsigned int gametic;
  unsigned int checksum;
} net_checksum_msg_t;

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
