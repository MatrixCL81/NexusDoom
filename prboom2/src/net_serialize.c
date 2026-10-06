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

#include <string.h>

#include "net_serialize.h"

// Helper: write big-endian int16 to buffer
static void write_i16(unsigned char *buf, int val)
{
  buf[0] = (unsigned char)((val >> 8) & 0xff);
  buf[1] = (unsigned char)(val & 0xff);
}

// Helper: read big-endian int16 from buffer
static int read_i16(const unsigned char *buf)
{
  return (short)((buf[0] << 8) | buf[1]);
}

// Helper: write big-endian int32 to buffer
static void write_i32(unsigned char *buf, int val)
{
  buf[0] = (unsigned char)((val >> 24) & 0xff);
  buf[1] = (unsigned char)((val >> 16) & 0xff);
  buf[2] = (unsigned char)((val >> 8) & 0xff);
  buf[3] = (unsigned char)(val & 0xff);
}

// Helper: read big-endian int32 from buffer
static int read_i32(const unsigned char *buf)
{
  return (int)((buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3]);
}

int net_write_ticcmd(unsigned char *buf, const ticcmd_t *cmd)
{
  buf[0] = (unsigned char)cmd->forwardmove;
  buf[1] = (unsigned char)cmd->sidemove;
  write_i16(&buf[2], cmd->angleturn);
  buf[4] = cmd->buttons;
  buf[5] = cmd->lookfly;
  buf[6] = cmd->arti;
  // excmd_t: actions, save_slot, load_slot, look (2 bytes)
  buf[7] = cmd->ex.actions;
  buf[8] = cmd->ex.save_slot;
  buf[9] = cmd->ex.load_slot;
  write_i16(&buf[10], cmd->ex.look);
  write_i16(&buf[12], (int)cmd->ex.net_game_speed);
  buf[14] = cmd->ex.key_frame_op;
  return NET_TICCMD_SIZE;
}

int net_read_ticcmd(const unsigned char *buf, ticcmd_t *cmd)
{
  memset(cmd, 0, sizeof(*cmd));
  cmd->forwardmove = (signed char)buf[0];
  cmd->sidemove = (signed char)buf[1];
  cmd->angleturn = (signed short)read_i16(&buf[2]);
  cmd->buttons = buf[4];
  cmd->lookfly = buf[5];
  cmd->arti = buf[6];
  cmd->ex.actions = buf[7];
  cmd->ex.save_slot = buf[8];
  cmd->ex.load_slot = buf[9];
  cmd->ex.look = (signed short)read_i16(&buf[10]);
  cmd->ex.net_game_speed = (unsigned short)read_i16(&buf[12]);
  cmd->ex.key_frame_op = buf[14];
  return NET_TICCMD_SIZE;
}

int net_write_setup(unsigned char *buf, const net_setup_t *setup)
{
  write_i32(&buf[0],  setup->skill);
  write_i32(&buf[4],  setup->episode);
  write_i32(&buf[8],  setup->map);
  write_i32(&buf[12], setup->complevel);
  write_i32(&buf[16], setup->deathmatch);
  write_i32(&buf[20], setup->nomonsters);
  write_i32(&buf[24], setup->fast);
  write_i32(&buf[28], setup->respawn);
  write_i32(&buf[32], setup->longtics);
  write_i32(&buf[36], setup->game_speed);
  write_i32(&buf[40], (int)setup->rngseed);
  memcpy(&buf[44], setup->from_key_frame, NET_KF_FILENAME_MAX);
  return NET_SETUP_SIZE;
}

int net_read_setup(const unsigned char *buf, net_setup_t *setup)
{
  setup->skill       = read_i32(&buf[0]);
  setup->episode     = read_i32(&buf[4]);
  setup->map         = read_i32(&buf[8]);
  setup->complevel   = read_i32(&buf[12]);
  setup->deathmatch  = read_i32(&buf[16]);
  setup->nomonsters  = read_i32(&buf[20]);
  setup->fast        = read_i32(&buf[24]);
  setup->respawn     = read_i32(&buf[28]);
  setup->longtics    = read_i32(&buf[32]);
  setup->game_speed  = read_i32(&buf[36]);
  setup->rngseed     = (unsigned int)read_i32(&buf[40]);
  memcpy(setup->from_key_frame, &buf[44], NET_KF_FILENAME_MAX);
  setup->from_key_frame[NET_KF_FILENAME_MAX - 1] = '\0';
  return NET_SETUP_SIZE;
}

int net_write_tics(unsigned char *buf, const net_tics_msg_t *msg)
{
  int i;
  int count = msg->count;

  if (count < 0) count = 0;
  if (count > NET_TICS_MAX_CMDS) count = NET_TICS_MAX_CMDS;

  write_i16(&buf[0], msg->epoch);
  buf[2] = msg->flags;
  write_i32(&buf[3], (int)msg->ack_seq);
  write_i32(&buf[7], (int)msg->checksum_tic);
  write_i32(&buf[11], (int)msg->checksum);
  write_i32(&buf[15], (int)msg->start_seq);
  buf[19] = (unsigned char)count;

  for (i = 0; i < count; i++)
    net_write_ticcmd(&buf[NET_TICS_HEADER_SIZE + i * NET_TICCMD_SIZE], &msg->cmds[i]);

  return NET_TICS_HEADER_SIZE + count * NET_TICCMD_SIZE;
}

int net_read_tics(const unsigned char *buf, int length, net_tics_msg_t *msg)
{
  int i;

  if (length < NET_TICS_HEADER_SIZE)
    return -1;

  msg->epoch        = (unsigned short)read_i16(&buf[0]);
  msg->flags        = buf[2];
  msg->ack_seq      = (unsigned int)read_i32(&buf[3]);
  msg->checksum_tic = (unsigned int)read_i32(&buf[7]);
  msg->checksum     = (unsigned int)read_i32(&buf[11]);
  msg->start_seq    = (unsigned int)read_i32(&buf[15]);
  msg->count        = buf[19];

  if (msg->count > NET_TICS_MAX_CMDS ||
      length != NET_TICS_HEADER_SIZE + msg->count * NET_TICCMD_SIZE)
    return -1;

  for (i = 0; i < msg->count; i++)
    net_read_ticcmd(&buf[NET_TICS_HEADER_SIZE + i * NET_TICCMD_SIZE], &msg->cmds[i]);

  return length;
}

int net_write_hello(unsigned char *buf, unsigned int magic)
{
  write_i32(&buf[0], (int)magic);
  return NET_HELLO_SIZE;
}

unsigned int net_read_hello(const unsigned char *buf, int length)
{
  if (length < NET_HELLO_SIZE)
    return 0;
  return (unsigned int)read_i32(&buf[0]);
}
