/* Emacs style mode select   -*- C -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *    Contains the main wait loop, waiting for the next tic.
 *    Rewritten for LxDoom, but based around bits of the old code.
 *
 *-----------------------------------------------------------------------------
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#include <string.h>

#include "doomtype.h"
#include "doomstat.h"
#include "d_net.h"
#include "m_random.h"
#include "hu_stuff.h"
#include "p_mobj.h"
#include "z_zone.h"

#include "d_main.h"
#include "g_game.h"
#include "m_menu.h"

#include "i_system.h"
#include "i_main.h"
#include "i_video.h"
#include "r_fps.h"
#include "lprintf.h"
#include "e6y.h"

#include "m_fixed.h"

#include "dsda/args.h"
#include "dsda/demo.h"
#include "dsda/input.h"
#include "dsda/key_frame.h"
#include "dsda/settings.h"
#include "dsda/time.h"

#include "net_session.h"
#include "net_transport.h"
#include "net_serialize.h"

ticcmd_t local_cmds[MAX_MAXPLAYERS][BACKUPTICS];
int maketic;
int solo_net = 0;
static int remote_maketic;

static int pending_key_frame_op;

// ---------------------------------------------------------------------------
// Post-rewind self-replay
//
// After a rewind or restore, the consoleplayer's ticcmds from the restored
// point to the pre-rewind point are replayed automatically so the game fast-
// forwards through the undone segment without live keyboard input.  The
// lockstep continues unchanged: the replayed cmd is sent to the remote peer
// each tic and the remote player's new live ticcmds are received as normal.
// The player presses input_join_demo at any point to take over with live
// input early.
// ---------------------------------------------------------------------------
static byte*        net_replay_buf;
static const byte*  net_replay_p;
static const byte*  net_replay_end;

static void NetEndSelfReplay(void)
{
  if (net_replay_buf) Z_Free(net_replay_buf);
  net_replay_buf = NULL;
  net_replay_p   = NULL;
  net_replay_end = NULL;
  SetCustomMessage(displayplayer, "", 0, 0);
}

// Called after every restore/rewind. If a replay was still in progress, its
// unplayed tail is appended, so the new replay still leads back to the point
// the player had originally reached.
static void NetStartSelfReplay(int pre_rewind_offset)
{
  int bpt;
  int stride;
  int tics;
  const byte* buf;
  int local;
  int t;
  byte* old_buf;
  const byte* tail;
  int tail_len;
  byte* new_buf;

  old_buf  = net_replay_buf;
  tail     = net_replay_p;
  tail_len = tail ? (int)(net_replay_end - tail) : 0;

  bpt    = dsda_BytesPerTic();
  stride = 2 * bpt; // NexusDoom lockstep is always 2-player
  tics   = (pre_rewind_offset - dsda_DemoBufferOffset()) / stride;
  buf    = dsda_GetDemoBuffer();
  local  = net_session.local_player;

  if (!demorecording || tics < 0 || !buf) {
    tics = 0;
  }

  new_buf = NULL;
  if (tics + tail_len > 0) {
    new_buf = (byte*)Z_Malloc(tics * bpt + tail_len + 1);

    for (t = 0; t < tics; t++) {
      int src = dsda_DemoBufferOffset() + t * stride + local * bpt;
      memcpy(new_buf + t * bpt, buf + src, bpt);
    }
    if (tail_len)
      memcpy(new_buf + tics * bpt, tail, tail_len);
    new_buf[tics * bpt + tail_len] = 0x80; // DEMOMARKER
  }

  if (old_buf) Z_Free(old_buf);

  net_replay_buf = new_buf;
  net_replay_p   = new_buf;
  net_replay_end = new_buf ? new_buf + tics * bpt + tail_len : NULL;
}

static dboolean net_out_of_sync;
static dboolean net_desync_diag;
static dboolean net_waiting_for_peer;
static dboolean net_connection_lost;
#define NET_WAIT_INITIAL_TIMEOUT_MS 1000
#define NET_WAIT_POLL_TIMEOUT_MS 50
static unsigned int net_local_checksum_tic[BACKUPTICS];
static unsigned int net_local_checksum_value[BACKUPTICS];
static dboolean net_local_checksum_valid[BACKUPTICS];
static unsigned int net_remote_checksum_tic[BACKUPTICS];
static unsigned int net_remote_checksum_value[BACKUPTICS];
static dboolean net_remote_checksum_valid[BACKUPTICS];

typedef struct {
  unsigned int tic;
  unsigned int full;
  unsigned int rng_full;
  unsigned int rng_index;
  unsigned int player0;
  unsigned int player1;
  unsigned int monsters;
  unsigned int alive_monsters;
  int p0_x;
  int p0_y;
  int p0_z;
  int p0_health;
  int p1_x;
  int p1_y;
  int p1_z;
  int p1_health;
  int rndindex;
  int prndindex;
  dboolean valid;
} net_desync_diag_snapshot_t;

static net_desync_diag_snapshot_t net_diag_local;
static unsigned int net_diag_last_remote_tic;
static unsigned int net_diag_last_remote_checksum;

// -netdesyncdiag: log the settings that determine the simulation once per
// checksum epoch, so the two peers' terminals can be compared line by line.
static dboolean net_diag_settings_logged;

static void NetLogSimSettings(void)
{
  if (!net_desync_diag || net_diag_settings_logged)
    return;

  net_diag_settings_logged = true;
  lprintf(LO_INFO,
          "Net sim settings tic=%d complevel=%d demo_compatibility=%d demo_insurance=%d "
          "rngseed=%u skill=%d episode=%d map=%d fast=%d respawn=%d nomonsters=%d "
          "deathmatch=%d consoleplayer=%d\n",
          gametic, compatibility_level, demo_compatibility, demo_insurance,
          rngseed, gameskill, gameepisode, gamemap, fastparm, respawnparm,
          nomonsters, deathmatch, consoleplayer);
}

static void NetResetChecksumState(void)
{
  net_diag_settings_logged = false;
  memset(net_local_checksum_valid, 0, sizeof(net_local_checksum_valid));
  memset(net_remote_checksum_valid, 0, sizeof(net_remote_checksum_valid));
  net_out_of_sync = false;
  net_waiting_for_peer = false;
  net_connection_lost = false;
  memset(&net_diag_local, 0, sizeof(net_diag_local));
  net_diag_last_remote_tic = 0;
  net_diag_last_remote_checksum = 0;
}

// Timestamp (microseconds from dsda_timer_realtime) at which the next tic is
// allowed to fire. Zero means "not yet armed" — gate initialises on first use.
static unsigned long long net_pacing_next_tic_us = 0;

// Network-derived interpolation fraction for the renderer. Updated each frame
// by the pacing gate so I_GetTimeFrac() can bypass the wall clock in MP.
static fixed_t net_interpolation_frac = FRACUNIT;

fixed_t NetGetTimeFrac(void)
{
  return net_interpolation_frac;
}

// Purge all per-session multiplayer state. Called on game init and on every
// disconnect so that a reconnect (or future rewind) starts with a clean slate.
void NetResetState(void)
{
  remote_maketic = 0;
  net_pacing_next_tic_us = 0;
  net_interpolation_frac = FRACUNIT;
  NetResetChecksumState();
}

// Reset network loop state after a key frame restore. Unlike NetResetState
// (which is for disconnect/init), this sets maketic and remote_maketic to the
// restored gametic rather than zero.
static void NetResetAfterRestore(void)
{
  maketic = gametic;
  remote_maketic = gametic;
  net_pacing_next_tic_us = 0;
  net_interpolation_frac = FRACUNIT;
  NetResetChecksumState();
}

void NetRequestKeyFrameOp(int op)
{
  pending_key_frame_op = op;
}

static unsigned int NetChecksumBytes(unsigned int hash, const void *data, size_t size)
{
  const unsigned char *p;
  size_t i;

  p = (const unsigned char *)data;

  for (i = 0; i < size; i++) {
    hash ^= p[i];
    hash *= 16777619u;
  }

  return hash;
}

static unsigned int NetBuildChecksum(unsigned int tic)
{
  unsigned int hash;
  int alive_monsters;
  fixed_t p0_x, p0_y, p0_z;
  fixed_t p1_x, p1_y, p1_z;
  int p0_health, p1_health;
  unsigned int rng_full_hash;
  unsigned int rng_index_hash;
  unsigned int player0_hash;
  unsigned int player1_hash;
  unsigned int monsters_hash;

  hash = 2166136261u;

  p0_x = p0_y = p0_z = 0;
  p1_x = p1_y = p1_z = 0;

  if (players[0].mo) {
    p0_x = players[0].mo->x;
    p0_y = players[0].mo->y;
    p0_z = players[0].mo->z;
  }

  if (players[1].mo) {
    p1_x = players[1].mo->x;
    p1_y = players[1].mo->y;
    p1_z = players[1].mo->z;
  }

  p0_health = players[0].health;
  p1_health = players[1].health;

  // Hash each field of rng_t explicitly rather than taking &rng directly.
  // A raw sizeof(rng) hash would include any compiler-inserted padding bytes
  // whose values are undefined, producing false desync alerts between builds.
  rng_full_hash = 2166136261u;
  rng_full_hash = NetChecksumBytes(rng_full_hash, rng.seed, sizeof(rng.seed));
  rng_full_hash = NetChecksumBytes(rng_full_hash, &rng.rndindex, sizeof(rng.rndindex));
  rng_full_hash = NetChecksumBytes(rng_full_hash, &rng.prndindex, sizeof(rng.prndindex));
  rng_index_hash = NetChecksumBytes(2166136261u, &rng.rndindex, sizeof(rng.rndindex));

  player0_hash = 2166136261u;
  player0_hash = NetChecksumBytes(player0_hash, &p0_x, sizeof(p0_x));
  player0_hash = NetChecksumBytes(player0_hash, &p0_y, sizeof(p0_y));
  player0_hash = NetChecksumBytes(player0_hash, &p0_z, sizeof(p0_z));
  player0_hash = NetChecksumBytes(player0_hash, &p0_health, sizeof(p0_health));

  player1_hash = 2166136261u;
  player1_hash = NetChecksumBytes(player1_hash, &p1_x, sizeof(p1_x));
  player1_hash = NetChecksumBytes(player1_hash, &p1_y, sizeof(p1_y));
  player1_hash = NetChecksumBytes(player1_hash, &p1_z, sizeof(p1_z));
  player1_hash = NetChecksumBytes(player1_hash, &p1_health, sizeof(p1_health));

  alive_monsters = P_CountLivingMonsters();
  monsters_hash = NetChecksumBytes(2166136261u, &alive_monsters, sizeof(alive_monsters));

  hash = NetChecksumBytes(hash, &rng.rndindex, sizeof(rng.rndindex));
  hash = NetChecksumBytes(hash, &p0_x, sizeof(p0_x));
  hash = NetChecksumBytes(hash, &p0_y, sizeof(p0_y));
  hash = NetChecksumBytes(hash, &p0_z, sizeof(p0_z));
  hash = NetChecksumBytes(hash, &p0_health, sizeof(p0_health));
  hash = NetChecksumBytes(hash, &p1_x, sizeof(p1_x));
  hash = NetChecksumBytes(hash, &p1_y, sizeof(p1_y));
  hash = NetChecksumBytes(hash, &p1_z, sizeof(p1_z));
  hash = NetChecksumBytes(hash, &p1_health, sizeof(p1_health));
  hash = NetChecksumBytes(hash, &alive_monsters, sizeof(alive_monsters));

  net_diag_local.tic = tic;
  net_diag_local.full = hash;
  net_diag_local.rng_full = rng_full_hash;
  net_diag_local.rng_index = rng_index_hash;
  net_diag_local.player0 = player0_hash;
  net_diag_local.player1 = player1_hash;
  net_diag_local.monsters = monsters_hash;
  net_diag_local.alive_monsters = (unsigned int)alive_monsters;
  net_diag_local.p0_x = (int)p0_x;
  net_diag_local.p0_y = (int)p0_y;
  net_diag_local.p0_z = (int)p0_z;
  net_diag_local.p0_health = p0_health;
  net_diag_local.p1_x = (int)p1_x;
  net_diag_local.p1_y = (int)p1_y;
  net_diag_local.p1_z = (int)p1_z;
  net_diag_local.p1_health = p1_health;
  net_diag_local.rndindex = rng.rndindex;
  net_diag_local.prndindex = rng.prndindex;
  net_diag_local.valid = true;

  return hash;
}

static void NetCompareChecksumForTic(unsigned int tic)
{
  int idx;

  idx = tic % BACKUPTICS;

  if (!net_local_checksum_valid[idx] || !net_remote_checksum_valid[idx])
    return;

  if (net_local_checksum_tic[idx] != tic || net_remote_checksum_tic[idx] != tic)
    return;

  if (net_local_checksum_value[idx] != net_remote_checksum_value[idx]) {
      lprintf(LO_ERROR, "Desync detected at tic %u (local=%08x remote=%08x)\n",
        tic, net_local_checksum_value[idx], net_remote_checksum_value[idx]);

      if (net_diag_local.valid && net_diag_local.tic == tic) {
        lprintf(LO_ERROR,
          "Desync diag local tic=%u full=%08x rng_full=%08x rng_index=%08x p0=%08x p1=%08x monsters=%08x alive=%u rnd=%d prnd=%d\n",
          net_diag_local.tic,
          net_diag_local.full,
          net_diag_local.rng_full,
          net_diag_local.rng_index,
          net_diag_local.player0,
          net_diag_local.player1,
          net_diag_local.monsters,
          net_diag_local.alive_monsters,
          net_diag_local.rndindex,
          net_diag_local.prndindex);

        lprintf(LO_ERROR,
          "Desync diag local p0_xyz=(%d,%d,%d) p0_health=%d p1_xyz=(%d,%d,%d) p1_health=%d\n",
          net_diag_local.p0_x,
          net_diag_local.p0_y,
          net_diag_local.p0_z,
          net_diag_local.p0_health,
          net_diag_local.p1_x,
          net_diag_local.p1_y,
          net_diag_local.p1_z,
          net_diag_local.p1_health);
      }

      lprintf(LO_ERROR, "Desync diag remote tic=%u full=%08x\n",
        net_diag_last_remote_tic,
        net_diag_last_remote_checksum);

    net_out_of_sync = true;
  }

  net_local_checksum_valid[idx] = false;
  net_remote_checksum_valid[idx] = false;
}

static void NetMaybeSendChecksum(void)
{
  unsigned int tic;
  unsigned int checksum;
  int idx;
  unsigned char buf[NET_CHECKSUM_SIZE];
  net_checksum_msg_t msg;

  tic = (unsigned int)gametic;
  if ((tic % TICRATE) != 0)
    return;

  NetLogSimSettings();

  checksum = NetBuildChecksum(tic);
  idx = tic % BACKUPTICS;

  net_local_checksum_tic[idx] = tic;
  net_local_checksum_value[idx] = checksum;
  net_local_checksum_valid[idx] = true;

  msg.gametic = tic;
  msg.checksum = checksum;
  net_write_checksum(buf, &msg);

  if (net_send_packet(net_session.socket, NET_MSG_CHECKSUM, buf, NET_CHECKSUM_SIZE) != 0) {
    lprintf(LO_ERROR, "NetMaybeSendChecksum: failed to send checksum\n");
    net_waiting_for_peer = false;
    net_connection_lost = true;
    net_session_disconnect();
    return;
  }

  if (net_desync_diag) {
    lprintf(LO_INFO,
            "Net checksum tic=%u local=%08x rng_full=%08x rng_index=%08x p0=%08x p1=%08x monsters=%08x alive=%u\n",
            tic,
            checksum,
            net_diag_local.rng_full,
            net_diag_local.rng_index,
            net_diag_local.player0,
            net_diag_local.player1,
            net_diag_local.monsters,
            net_diag_local.alive_monsters);
  }

  NetCompareChecksumForTic(tic);
}

static void NetUpdateOutOfSyncMessage(void)
{
  if (net_connection_lost)
    SetCustomMessage(displayplayer, "connection lost", 2 * TICRATE, 0);
  else if (net_waiting_for_peer)
    SetCustomMessage(displayplayer, "waiting for peer", 2 * TICRATE, 0);
  else if (net_out_of_sync)
    SetCustomMessage(displayplayer, "out of sync", 2 * TICRATE, 0);
  else if (net_replay_p)
    SetCustomMessage(displayplayer, "replay", 2 * TICRATE, 0);
}

void D_InitFakeNetGame (void)
{
  int i;

  consoleplayer = displayplayer = 0;
  solo_net = dsda_Flag(dsda_arg_solo_net);
  coop_spawns = dsda_Flag(dsda_arg_coop_spawns);
  netgame = solo_net;

  playeringame[0] = true;
  for (i = 1; i < g_maxplayers; i++)
    playeringame[i] = false;
}

void D_InitNetGame(void)
{
  dsda_arg_t *arg_host, *arg_join, *arg_port;
  int port;

  NetResetState();

  arg_host = dsda_Arg(dsda_arg_host);
  arg_join = dsda_Arg(dsda_arg_join);
  arg_port = dsda_Arg(dsda_arg_port);
  net_desync_diag = dsda_Flag(dsda_arg_netdesyncdiag);
  port = NET_DEFAULT_PORT;

  if (arg_port->found)
    port = arg_port->value.v_int;

  if (arg_host->found && arg_join->found) {
    I_Error("Cannot use -host and -join at the same time");
  }

  if (arg_host->found) {
    if (arg_host->value.v_int != 2) {
      I_Error("Only 2-player multiplayer is currently supported (use -host 2)");
    }

    if (net_session_host_start(port) != 0) {
      I_Error("Failed to start multiplayer host on port %d", port);
    }

    consoleplayer = displayplayer = 0;
    playeringame[0] = true;
    playeringame[1] = true;
    netgame = true;
    solo_net = 0;
    coop_spawns = 1;
  }
  else if (arg_join->found) {
    const char *addr_str = arg_join->value.v_string;
    char address[256];

    if (!addr_str || !addr_str[0]) {
      I_Error("-join requires an address (e.g., 127.0.0.1)");
    }

    if (strchr(addr_str, ':')) {
      I_Error("Use -port to set the network port (e.g., -join 127.0.0.1 -port 26101)");
    }

    strncpy(address, addr_str, sizeof(address) - 1);
    address[sizeof(address) - 1] = '\0';

    if (net_session_client_start(address, port) != 0) {
      I_Error("Failed to connect to %s:%d", address, port);
    }

    consoleplayer = displayplayer = 1;
    playeringame[0] = true;
    playeringame[1] = true;
    netgame = true;
    solo_net = 0;
    coop_spawns = 1;
  }
  else {
    D_InitFakeNetGame();
  }
}

void FakeNetUpdate(void)
{
  static int lastmadetic;

  if (isExtraDDisplay)
    return;

  if (net_session_active())
    return;

  { // Build new ticcmds
    int newtics = dsda_GetTick() - lastmadetic;
    lastmadetic += newtics;

    while (newtics--) {
      I_StartTic();
      if (maketic - gametic > BACKUPTICS/2) break;

      // e6y
      // Eliminating the sudden jump of six frames(BACKUPTICS/2)
      // after change of game_speed.
      if (maketic - gametic && gametic <= force_singletics_to && dsda_GameSpeed() < 200) break;

      G_BuildTiccmd(&local_cmds[consoleplayer][maketic%BACKUPTICS]);
      maketic++;
    }
  }
}

// Build local ticcmd, send it to remote, receive remote's ticcmd.
// For multiplayer: builds exactly one tic per call.
static void NetUpdate(void)
{
  unsigned char buf[64];
  int local = net_session.local_player;

  if (isExtraDDisplay)
    return;

  // Only build one tic ahead
  if (maketic > gametic)
    return;


  // End self-replay when the buffer is exhausted, or when the local player
  // presses input_join_demo. This only changes where the local ticcmd comes
  // from, so it needs no network synchronization: the remote peer receives
  // our ticcmd either way.
  if (net_replay_p) {
    if (net_replay_p >= net_replay_end || dsda_InputActive(dsda_input_join_demo)) {
      NetEndSelfReplay();
    }
  }

  // Build local ticcmd — use self-replay buffer when active, else live keyboard.
  if (net_replay_p) {
    // G_ReadOneTick only sets the fields stored in the demo. Clear the slot
    // first so wire-only fields (e.g. a stale key_frame_op from BACKUPTICS
    // tics ago) are not resent to the remote peer.
    memset(&local_cmds[local][maketic % BACKUPTICS], 0, sizeof(ticcmd_t));
    G_ReadOneTick(&local_cmds[local][maketic % BACKUPTICS], &net_replay_p);
  } else {
    G_BuildTiccmd(&local_cmds[local][maketic % BACKUPTICS]);
    // Round to demo precision so recording and non-recording peers execute
    // identical cmds (replayed cmds come from the demo and already are).
    G_NormalizeTiccmd(&local_cmds[local][maketic % BACKUPTICS]);
  }

  // Host stamps its authoritative game_speed into every ticcmd so the client
  // can update its pacing gate on the exact same execution tic.
  if (net_session.is_host)
    local_cmds[local][maketic % BACKUPTICS].ex.net_game_speed =
        (unsigned short)dsda_GameSpeed();

  // Stamp pending key frame op (store/restore intent) for synchronized execution.
  if (pending_key_frame_op) {
    local_cmds[local][maketic % BACKUPTICS].ex.key_frame_op =
        (byte)pending_key_frame_op;
    pending_key_frame_op = KF_OP_NONE;
  }

  // Send to remote
  net_write_ticcmd(buf, &local_cmds[local][maketic % BACKUPTICS]);
  if (net_send_packet(net_session.socket, NET_MSG_TICCMD, buf, NET_TICCMD_SIZE) != 0) {
    lprintf(LO_ERROR, "NetUpdate: failed to send ticcmd\n");
    net_waiting_for_peer = false;
    net_connection_lost = true;
    net_session_disconnect();
    return;
  }

  maketic++;
}

// Receive remote player's ticcmd for the given tic.
// Blocks until data arrives (hard stall). Returns 0 on success, -1 on error.
static int NetRecvRemoteTic(void)
{
  unsigned char buf[64];
  int len;
  int msg_type;
  int remote = net_session.remote_player;

  while (remote_maketic <= gametic) {
    int timeout_ms = net_waiting_for_peer ? NET_WAIT_POLL_TIMEOUT_MS : NET_WAIT_INITIAL_TIMEOUT_MS;
    int wait_result = net_wait_for_packet(net_session.socket, timeout_ms);

    if (wait_result == 0) {
      if (!net_waiting_for_peer) {
        lprintf(LO_INFO, "NetRecvRemoteTic: waiting for remote tic %d\n", gametic);
      }

      net_waiting_for_peer = true;
      NetUpdateOutOfSyncMessage();
      return 1;
    }

    if (wait_result < 0) {
      lprintf(LO_ERROR, "NetRecvRemoteTic: socket wait failed\n");
      net_waiting_for_peer = false;
      net_connection_lost = true;
      net_session_disconnect();
      return -1;
    }

    msg_type = net_recv_packet(net_session.socket, buf, &len, sizeof(buf));

    if (msg_type == NET_MSG_TICCMD) {
      net_read_ticcmd(buf, &local_cmds[remote][remote_maketic % BACKUPTICS]);
      remote_maketic++;
      net_waiting_for_peer = false;
    }
    else if (msg_type == NET_MSG_CHECKSUM) {
      net_checksum_msg_t msg;
      int idx;

      if (len != NET_CHECKSUM_SIZE)
        continue;

      net_read_checksum(buf, &msg);
      idx = msg.gametic % BACKUPTICS;
      net_remote_checksum_tic[idx] = msg.gametic;
      net_remote_checksum_value[idx] = msg.checksum;
      net_remote_checksum_valid[idx] = true;
      net_diag_last_remote_tic = msg.gametic;
      net_diag_last_remote_checksum = msg.checksum;

      if (net_desync_diag) {
        lprintf(LO_INFO, "Net checksum recv tic=%u remote=%08x\n",
                msg.gametic, msg.checksum);
      }

      NetCompareChecksumForTic(msg.gametic);
    }
    else if (msg_type == NET_MSG_QUIT || msg_type < 0) {
      lprintf(LO_ERROR, "NetRecvRemoteTic: remote disconnected\n");
      net_waiting_for_peer = false;
      net_connection_lost = true;
      net_session_disconnect();
      return -1;
    }
    // Ignore unknown message types
  }

  return 0;
}

// Unified multiplayer tic-advance protocol. Encapsulates the entire lockstep
// sequence: poll input, send local ticcmd, receive remote ticcmd, advance the
// world. This is the single intercept point for future Rewind/Rollback hooks.
//
// Returns:
//   1  — tic advanced successfully (gametic incremented)
//   0  — still waiting for remote peer (caller should yield)
//  -1  — disconnected (session torn down, caller should bail)
static int NetRunOneTic(void)
{
  // ---------------------------------------------------------------------------
  // Pacing gate: throttle the lockstep loop to the synchronized game_speed.
  //
  // We measure real-world elapsed time between tic generations entirely within
  // this function, touching no global time accumulator. This avoids the
  // catch-up burst that occurs when the global clock is compared against wall
  // time accumulated during the handshake/connection phase.
  // ---------------------------------------------------------------------------
  {
    int speed = dsda_GameSpeed();
    unsigned long long now = dsda_ElapsedTime(dsda_timer_realtime);
    unsigned long long interval_us;

    if (speed <= 0) speed = 100;
    // interval_us = 1 / (TICRATE * speed/100)  expressed in microseconds
    interval_us = (unsigned long long)1000000ULL * 100 / (35 * speed);

    if (net_pacing_next_tic_us == 0)
      net_pacing_next_tic_us = now; // first tic: arm the gate without waiting

    if (now < net_pacing_next_tic_us)
    {
      // Not yet time for the next tic. Compute the interpolation fraction
      // representing how far we are through the current interval.
      unsigned long long elapsed, wait_us;
      fixed_t frac;

      elapsed = now - (net_pacing_next_tic_us - interval_us);
      frac = (fixed_t)(elapsed * FRACUNIT / interval_us);
      net_interpolation_frac = BETWEEN(0, FRACUNIT, frac);

      // Sleep in small chunks so input and menus stay responsive, then yield.
      wait_us = net_pacing_next_tic_us - now;
      if (wait_us > 5000) wait_us = 5000;
      I_uSleep((unsigned long)wait_us);
      M_Ticker();
      return 0;
    }

    // Tic is about to fire — fraction is complete.
    net_interpolation_frac = FRACUNIT;

    // Advance the schedule by exactly one interval from the *planned* time so
    // the cadence stays accurate. If we have fallen more than one interval
    // behind (e.g. after a remote stall or the initial handshake), clamp to
    // avoid a burst of back-to-back tics to "catch up".
    net_pacing_next_tic_us += interval_us;
    if (net_pacing_next_tic_us < now)
      net_pacing_next_tic_us = now + interval_us;
  }

  I_StartTic();

  // Build local ticcmd and send to remote
  NetUpdate();
  if (!net_session_active())
    return -1;

  // Wait for remote ticcmd
  {
    int recv_result = NetRecvRemoteTic();

    if (recv_result == 1) {
      // No remote data yet — tick menus/UI so the game stays responsive
      M_Ticker();
      NetUpdateOutOfSyncMessage();
      return 0;
    }

    if (recv_result != 0)
      return -1;
  }

  // Both players have ticcmds for gametic — advance one tic

  // Synchronize game_speed from the host's ticcmd. Both peers apply this on
  // the same execution tic (host and client both possess the host's ticcmd for
  // gametic at this point), so their pacing gates update simultaneously.
  {
    int host = net_session.is_host ? net_session.local_player
                                   : net_session.remote_player;
    unsigned short synced_speed =
        local_cmds[host][gametic % BACKUPTICS].ex.net_game_speed;

    if (synced_speed > 0 && (int)synced_speed != dsda_GameSpeed())
      dsda_UpdateGameSpeed((int)synced_speed);
  }

  // Synchronized key frame store/restore: if either player signalled an op,
  // execute it on both clients at the same gametic before G_Ticker() advances.
  // The game-state portion of the key frame is byte-identical on both clients
  // under lockstep; the demo-buffer portion may differ (each client records
  // from its own consoleplayer) but each client only restores its own buffer.
  {
    int local = net_session.local_player;
    int remote = net_session.remote_player;
    byte local_op = local_cmds[local][gametic % BACKUPTICS].ex.key_frame_op;
    byte remote_op = local_cmds[remote][gametic % BACKUPTICS].ex.key_frame_op;
    byte combined_op = local_op | remote_op;

    if (combined_op & KF_OP_STORE)
      dsda_StoreQuickKeyFrame();

    if (combined_op & KF_OP_RESTORE) {
      int pre_rewind_offset = dsda_DemoBufferOffset();
      dsda_RestoreQuickKeyFrame();
      NetResetAfterRestore();
      NetStartSelfReplay(pre_rewind_offset);
      return 0;
    }

    if (combined_op & KF_OP_REWIND) {
      int pre_rewind_offset = dsda_DemoBufferOffset();
      dsda_RewindAutoKeyFrame();
      NetResetAfterRestore();
      NetStartSelfReplay(pre_rewind_offset);
      return 0;
    }
  }

  if (advancedemo)
    D_DoAdvanceDemo();
  M_Ticker();
  G_Ticker();
  NetMaybeSendChecksum();
  if (!net_session_active())
    return -1;
  gametic++;
  // Reset to 0 so the first rendered frame after the tic fires starts at the
  // beginning of the new interval (frac=0 = prev_viewangle = pre-turn state).
  // This mirrors the vanilla wall-clock path where dsda_TickElapsedTime()
  // returns ~0 immediately after a tic, giving visual continuity without the
  // FRACUNIT-then-near-0 snap that caused the one-frame overshoot.
  net_interpolation_frac = 0;
  NetUpdateOutOfSyncMessage();
  return 1;
}

// Implicitly tracked whenever we check the current tick
int ms_to_next_tick;

void TryRunTics (void)
{
  int runtics;
  int entertime = dsda_GetTick();

  if (net_session_active()) {
    NetRunOneTic();
    return;
  }

  // Single-player path (unchanged)
  // Wait for tics to run
  while (1) {
    FakeNetUpdate();
    runtics = maketic - gametic;
    if (!runtics) {
      if (!movement_smooth) {
          I_uSleep(ms_to_next_tick*1000);
      }
      if (dsda_GetTick() - entertime > 10) {
        M_Ticker(); return;
      }

      if (gametic > 0)
      {
        WasRenderedInTryRunTics = true;
        if (movement_smooth && gamestate==wipegamestate)
        {
          isExtraDDisplay = true;
          D_Display(-1);
          isExtraDDisplay = false;
        }
      }
    } else break;
  }

  while (runtics--) {
    if (advancedemo)
      D_DoAdvanceDemo ();
    M_Ticker ();
    G_Ticker ();
    gametic++;
    NetUpdateOutOfSyncMessage();
    FakeNetUpdate();
  }
}

// Multiplayer-aware singletics handler (called from D_DoomLoop)
void NetSingleTic(void)
{
  if (!net_session_active()) {
    // Original singletics path
    I_StartTic();
    G_BuildTiccmd(&local_cmds[consoleplayer][maketic % BACKUPTICS]);
    if (advancedemo)
      D_DoAdvanceDemo();
    M_Ticker();
    G_Ticker();
    gametic++;
    maketic++;
    NetUpdateOutOfSyncMessage();
    return;
  }

  // Multiplayer singletics: unified lockstep
  NetRunOneTic();
}
