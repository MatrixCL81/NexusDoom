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
 *   Fake networking stuff.
 *
 *-----------------------------------------------------------------------------*/

#ifndef __D_NET__
#define __D_NET__

#include "d_player.h"
#include "m_fixed.h"

// Create any new ticcmds
void FakeNetUpdate (void);

//? how many ticks to run?
void TryRunTics (void);

// CPhipps - move to header file
void D_InitFakeNetGame (void); // This does the setup

// Multiplayer-aware init: delegates to FakeNetGame or real net
void D_InitNetGame(void);

// Multiplayer-aware singletics handler (called from D_DoomLoop)
void NetSingleTic(void);

// Purge all per-session multiplayer state (checksums, loop counters).
// Called on disconnect and at game init so reconnects start with a clean slate.
void NetResetState(void);

// Return the renderer's interpolation fraction, measured from when the latest
// tic was executed rather than from the wall clock. Only meaningful when
// net_session_active() is true.
fixed_t NetGetTimeFrac(void);

// Queue a key frame operation (KF_OP_STORE / KF_OP_RESTORE) to be stamped
// into the next outgoing ticcmd for synchronized execution in multiplayer.
void NetRequestKeyFrameOp(int op);

#endif
