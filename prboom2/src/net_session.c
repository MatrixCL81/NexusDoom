//
// Copyright(C) 2026 dsda-doom contributors
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// DESCRIPTION:
//  Multiplayer session management (connection protocol)
//

#include <stdio.h>
#include <string.h>
#include <signal.h>
#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
#else
  #include <sys/select.h>
  #include <sys/time.h>
#endif

#include "doomstat.h"
#include "d_main.h"
#include "dsda/args.h"
#include "dsda/global.h"
#include "dsda/demo.h"
#include "dsda/mapinfo.h"
#include "dsda/settings.h"
#include "i_main.h"
#include "i_system.h"
#include "lprintf.h"
#include "m_random.h"

#include "d_net.h"
#include "net_transport.h"
#include "net_serialize.h"
#include "net_session.h"

static volatile int net_interrupted = 0;

// Timeout for individual handshake recv calls (ms). Long enough for any
// reasonable WAN connection; short enough that a stalled peer can't freeze
// the process indefinitely.
#define NET_HANDSHAKE_TIMEOUT_MS 30000

// Guard so we register the disconnect atexit handler only once, even if
// net_session_host_start / net_session_client_start are called multiple times.
static int net_disconnect_atexit_registered = 0;

static void net_session_register_disconnect_atexit(void)
{
  if (!net_disconnect_atexit_registered) {
    I_AtExit(net_session_disconnect, true, "net_session_disconnect",
             exit_priority_first);
    net_disconnect_atexit_registered = 1;
  }
}

static void net_sigint_handler(int sig)
{
  (void)sig;
  net_interrupted = 1;
}

net_session_t net_session;

static int server_socket = -1;

static void net_session_reset(void)
{
  memset(&net_session, 0, sizeof(net_session));
  net_session.socket = -1;
  net_session.state = NET_STATE_DISCONNECTED;
}

static void net_session_build_setup(net_setup_t *setup)
{
  extern int startskill, startepisode, startmap;
  dsda_arg_t *arg;

  // Resolve -warp early: HandleWarp() runs much later in D_DoomMainSetup,
  // so startmap/startepisode are still at defaults here. We need the host's
  // resolved warp target in the setup message sent to the client.
  arg = dsda_Arg(dsda_arg_warp);
  if (arg->found) {
    int ep, map;
    dsda_ResolveWarp(arg->value.v_int_array, arg->count, &ep, &map);
    if (map == -1)
      dsda_FirstMap(&ep, &map);
    startepisode = ep;
    startmap = map;
  }

  setup->skill      = startskill;
  setup->episode    = startepisode;
  setup->map        = startmap;
  setup->complevel  = compatibility_level;
  setup->deathmatch = deathmatch;
  setup->nomonsters = nomonsters;
  setup->fast       = fastparm;
  setup->respawn    = respawnparm;
  setup->longtics   = 0;
  setup->game_speed = dsda_GameSpeed();
  // Already randomized by the G_ReloadDefaults call before D_InitNetGame.
  // G_ReloadDefaults leaves it alone from here on while a session is active.
  setup->rngseed    = rngseed;

  // -from_key_frame: send the filename so the client restores from the same file
  memset(setup->from_key_frame, 0, NET_KF_FILENAME_MAX);
  arg = dsda_Arg(dsda_arg_from_key_frame);
  if (arg->found && arg->value.v_string) {
    strncpy(setup->from_key_frame, arg->value.v_string, NET_KF_FILENAME_MAX - 1);
    setup->from_key_frame[NET_KF_FILENAME_MAX - 1] = '\0';
  }
}

static void net_session_apply_setup(const net_setup_t *setup)
{
  extern int startskill, startepisode, startmap;
  char buf[16];
  dsda_arg_t *arg;

  startskill    = setup->skill;
  startepisode  = setup->episode;
  startmap      = setup->map;
  // Apply host's compatibility level for multiplayer clients
  compatibility_level = setup->complevel;
  dsda_MarkCompatibilityLevelSpecified();
  deathmatch    = setup->deathmatch;
  nomonsters    = clnomonsters  = setup->nomonsters;
  fastparm      = clfastparm    = setup->fast;
  respawnparm   = clrespawnparm = setup->respawn;
  // Apply host's game speed. Do NOT call dsda_ResetTimeFunctions() here;
  // the MP pacing gate in NetRunOneTic owns throttling for the net loop.
  dsda_UpdateGameSpeed(setup->game_speed);
  // Host's RNG seed. Without this, each peer seeds from its own clock
  // and the simulations diverge immediately (complevel 9+).
  rngseed = setup->rngseed;

  // Override client CLI args with host's authoritative values so that
  // downstream code (G_ReloadDefaults, dsda_CompatibilityLevel,
  // dsda_InitGameModifiers, etc.) cannot clobber the host's settings.

  // -complevel
  arg = dsda_Arg(dsda_arg_complevel);
  if (arg->found && arg->value.v_int != setup->complevel)
    lprintf(LO_WARN, "Ignoring local -complevel %d, using host's %d\n",
            arg->value.v_int, setup->complevel);
  snprintf(buf, sizeof(buf), "%d", setup->complevel);
  dsda_UpdateIntArg(dsda_arg_complevel, buf);

  // -skill (CLI is 1-based, startskill is 0-based)
  arg = dsda_Arg(dsda_arg_skill);
  if (arg->found && arg->value.v_int != setup->skill + 1)
    lprintf(LO_WARN, "Ignoring local -skill %d, using host's %d\n",
            arg->value.v_int, setup->skill + 1);
  snprintf(buf, sizeof(buf), "%d", setup->skill + 1);
  dsda_UpdateIntArg(dsda_arg_skill, buf);

  // -fast
  if (dsda_Flag(dsda_arg_fast) && !setup->fast)
    lprintf(LO_WARN, "Ignoring local -fast, host does not use it\n");
  else if (!dsda_Flag(dsda_arg_fast) && setup->fast)
    lprintf(LO_INFO, "Host uses -fast\n");
  dsda_UpdateFlag(dsda_arg_fast, setup->fast != 0);

  // -respawn
  if (dsda_Flag(dsda_arg_respawn) && !setup->respawn)
    lprintf(LO_WARN, "Ignoring local -respawn, host does not use it\n");
  else if (!dsda_Flag(dsda_arg_respawn) && setup->respawn)
    lprintf(LO_INFO, "Host uses -respawn\n");
  dsda_UpdateFlag(dsda_arg_respawn, setup->respawn != 0);

  // -nomonsters
  if (dsda_Flag(dsda_arg_nomonsters) && !setup->nomonsters)
    lprintf(LO_WARN, "Ignoring local -nomonsters, host does not use it\n");
  else if (!dsda_Flag(dsda_arg_nomonsters) && setup->nomonsters)
    lprintf(LO_INFO, "Host uses -nomonsters\n");
  dsda_UpdateFlag(dsda_arg_nomonsters, setup->nomonsters != 0);

  // -warp: host's startepisode/startmap have already been applied above.
  // Clear the client's -warp arg so HandleWarp() (called later) won't
  // overwrite them with the client's local warp target.
  arg = dsda_Arg(dsda_arg_warp);
  if (arg->found) {
    int local_ep = (arg->count >= 2) ? arg->value.v_int_array[0] : 0;
    int local_map = (arg->count >= 2) ? arg->value.v_int_array[1]
                                      : (arg->count >= 1 ? arg->value.v_int_array[0] : 0);
    if (local_ep != setup->episode || local_map != setup->map)
      lprintf(LO_WARN, "Ignoring local -warp, using host's E%dM%d\n",
              setup->episode, setup->map);
    dsda_UpdateFlag(dsda_arg_warp, false);
  }

  // -from_key_frame: host's filename is authoritative.
  // Both peers must have an identical .kf file on disk under this name.
  if (setup->from_key_frame[0] != '\0') {
    arg = dsda_Arg(dsda_arg_from_key_frame);
    if (arg->found && strcmp(arg->value.v_string, setup->from_key_frame) != 0)
      lprintf(LO_WARN, "Ignoring local -from_key_frame %s, using host's %s\n",
              arg->value.v_string, setup->from_key_frame);
    dsda_UpdateStringArg(dsda_arg_from_key_frame, setup->from_key_frame);
    lprintf(LO_INFO, "Host uses -from_key_frame %s\n", setup->from_key_frame);
  }
}

int net_session_host_start(int port)
{
  net_setup_t setup;
  unsigned char buf[NET_SETUP_SIZE + NET_HEADER_SIZE];
  int len;
  int client_sock;
  int msg_type;
  void (*prev_sigint)(int);

  net_transport_init();
  net_session_reset();

  server_socket = net_listen(port);
  if (server_socket < 0) {
    lprintf(LO_ERROR, "net_session_host_start: failed to listen on port %d\n", port);
    return -1;
  }

  lprintf(LO_INFO, "Waiting for player to connect on port %d... (press Ctrl-C to cancel)\n", port);

  // Poll for client connection with a timeout so SIGINT can interrupt.
  // Save the existing handler so we restore it exactly: the engine installs
  // I_IntHandler for graceful shutdown before we get here, and restoring
  // SIG_DFL would kill the process immediately on any later Ctrl-C.
  net_interrupted = 0;
  prev_sigint = signal(SIGINT, net_sigint_handler);

  client_sock = -1;
  while (client_sock < 0) {
    fd_set readfds;
    struct timeval tv;
    int sel;

    if (net_interrupted) {
      lprintf(LO_INFO, "Interrupted, aborting host.\n");
      signal(SIGINT, prev_sigint);
      net_close(server_socket);
      server_socket = -1;
      I_SafeExit(0);
    }

    FD_ZERO(&readfds);
    FD_SET(server_socket, &readfds);
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    sel = select(server_socket + 1, &readfds, NULL, NULL, &tv);
    if (sel > 0)
      client_sock = net_accept(server_socket);
  }

  signal(SIGINT, prev_sigint);

  // Close the server socket — we only need the one client
  net_close(server_socket);
  server_socket = -1;

  net_session.socket = client_sock;
  net_session.is_host = 1;
  net_session.local_player = 0;
  net_session.remote_player = 1;
  net_session.state = NET_STATE_CONNECTING;

  // Send game settings to client
  net_session_build_setup(&setup);
  len = net_write_setup(buf, &setup);
  if (net_send_packet(client_sock, NET_MSG_SETUP, buf, len) != 0) {
    lprintf(LO_ERROR, "net_session_host_start: failed to send setup\n");
    net_session_disconnect();
    return -1;
  }

  lprintf(LO_INFO, "Sent game settings, waiting for client ready...\n");

  // Wait for READY from client — bounded to avoid an indefinite hang if the
  // client connects but never sends its acknowledgement.
  msg_type = net_recv_packet_timeout(client_sock, buf, &len, sizeof(buf),
                                     NET_HANDSHAKE_TIMEOUT_MS);
  if (msg_type == NET_RECV_TIMEOUT) {
    lprintf(LO_ERROR, "net_session_host_start: timed out waiting for client READY\n");
    net_session_disconnect();
    return -1;
  }
  if (msg_type != NET_MSG_READY) {
    lprintf(LO_ERROR, "net_session_host_start: expected READY, got %d\n", msg_type);
    net_session_disconnect();
    return -1;
  }

  net_session.state = NET_STATE_PLAYING;
  net_session_register_disconnect_atexit();
  lprintf(LO_INFO, "Client connected. Starting game.\n");
  return 0;
}

int net_session_client_start(const char *address, int port)
{
  net_setup_t setup;
  unsigned char buf[NET_SETUP_SIZE + NET_HEADER_SIZE];
  int len;
  int sock;
  int msg_type;
  int attempts;
  void (*prev_sigint)(int);

  net_transport_init();
  net_session_reset();

  lprintf(LO_INFO, "Connecting to %s:%d... (press Ctrl-C to cancel)\n", address, port);

  // Retry until the host is available, or user interrupts with Ctrl-C.
  net_interrupted = 0;
  prev_sigint = signal(SIGINT, net_sigint_handler);
  attempts = 0;
  sock = -1;

  while (sock < 0) {
    if (net_interrupted) {
      lprintf(LO_INFO, "Interrupted, aborting join.\n");
      signal(SIGINT, prev_sigint);
      I_SafeExit(0);
    }

    sock = net_connect(address, port);
    if (sock >= 0)
      break;

    attempts++;
    if (attempts == 1 || attempts % 5 == 0) {
      lprintf(LO_INFO, "Still waiting for host at %s:%d...\n", address, port);
    }

    I_uSleep(1000000);
  }

  signal(SIGINT, prev_sigint);

  net_session.socket = sock;
  net_session.is_host = 0;
  net_session.local_player = 1;
  net_session.remote_player = 0;
  net_session.state = NET_STATE_CONNECTING;

  // Receive game settings from host — bounded to avoid an indefinite hang if
  // the host accepts but never sends the SETUP packet.
  msg_type = net_recv_packet_timeout(sock, buf, &len, sizeof(buf),
                                     NET_HANDSHAKE_TIMEOUT_MS);
  if (msg_type == NET_RECV_TIMEOUT) {
    lprintf(LO_ERROR, "net_session_client_start: timed out waiting for host SETUP\n");
    net_session_disconnect();
    return -1;
  }
  if (msg_type != NET_MSG_SETUP) {
    lprintf(LO_ERROR, "net_session_client_start: expected SETUP, got %d\n", msg_type);
    net_session_disconnect();
    return -1;
  }

  net_read_setup(buf, &setup);
  net_session_apply_setup(&setup);

  lprintf(LO_INFO, "Received game settings (skill=%d, episode=%d, map=%d)\n",
          setup.skill, setup.episode, setup.map);

  // Send READY to host
  if (net_send_packet(sock, NET_MSG_READY, NULL, 0) != 0) {
    lprintf(LO_ERROR, "net_session_client_start: failed to send READY\n");
    net_session_disconnect();
    return -1;
  }

  net_session.state = NET_STATE_PLAYING;
  net_session_register_disconnect_atexit();
  lprintf(LO_INFO, "Connected to host. Starting game.\n");
  return 0;
}

void net_session_disconnect(void)
{
  if (net_session.state == NET_STATE_DISCONNECTED)
    return;

  // If recording, finalize the demo immediately so disconnect does not lose it.
  if (demorecording)
    dsda_EndDemoRecording();

  if (net_session.socket >= 0) {
    // Try to send QUIT (best effort, ignore errors)
    net_send_packet(net_session.socket, NET_MSG_QUIT, NULL, 0);
    net_close(net_session.socket);
  }

  if (server_socket >= 0) {
    net_close(server_socket);
    server_socket = -1;
  }

  // Remove the remote player from the game; keep the local player active
  if (net_session.remote_player >= 0 && net_session.remote_player < g_maxplayers)
    playeringame[net_session.remote_player] = false;
  // consoleplayer and playeringame[local] remain intact so play can continue
  netgame = false;

  // Purge per-session game-loop state so a reconnect or rewind starts clean.
  NetResetState();
  net_session_reset();
  lprintf(LO_INFO, "Disconnected from multiplayer session.\n");
}

int net_session_active(void)
{
  return net_session.state == NET_STATE_PLAYING;
}
