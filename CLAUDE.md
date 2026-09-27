## About me, the user

I am a senior full-stack web developer with experience in C# and other languages, but not with C. Although the goal is real, which is to actually be able to do tool-assisted speedruns with others over the internet, this is also a test to see how far I can get with vibe-coding.

## About the repository

I have two GitHub accounts: `github.com/MaartenCL` is my professional account and `github.com/MatrixCL81` my personal account. This repository was originally forked from `github.com/kraflab/dsda-doom` to my professional account, but it has since been transferred to my personal account.

## Golden rule: game mechanics must stay identical to DSDA-Doom

Speedruns are only valid if the game behaves exactly like the original. **Never change how the engine executes a ticcmd:** nothing in the play simulation (`p_*.c`, `m_random.c`, `r_*.c` where it affects gameplay, `G_Ticker`'s command handling, demo encoding/decoding, compatibility levels). Multiplayer code may only:

- decide *which* ticcmds are fed in and *when* (input building, networking, lockstep, key-frame ops, self-replay),
- read game state (e.g. checksums),
- make sure both peers feed identical ticcmds and settings into the unchanged engine.

Rounding input to demo precision (`G_NormalizeTiccmd`) is allowed: vanilla Doom does exactly that while recording. It changes the input, not the mechanics. If a change touches anything beyond input and networking, stop and ask the user first.

**How to verify:** build unmodified DSDA-Doom (`/data/repositories/dsda-doom`, the upstream commit NexusDoom branched from) and NexusDoom. Play every demo in `/data/games/doom/replays` in both with `-iwad /data/games/doom/wads/doom.wad -timedemo <demo> -nodraw -nosound -levelstat -analysis`, each in its own working directory with its own `HOME`. The `Timed N gametics` line, `levelstat.txt` and `analysis.txt` must be identical for every demo. Last verified 2026-09-27: all 1,725 demos (1,721 playable, 630 finished levels, 277 NexusDoom multiplayer recordings) were identical.

## Building

Build out-of-tree against **this** repo's `prboom2/`:

```bash
mkdir -p build && cd build
cmake ../prboom2 -DCMAKE_BUILD_TYPE=Release
make -j"$(nproc)"      # produces ./nexusdoom
```

## Multiplayer architecture

NexusDoom multiplayer is a small, custom **2-player TCP lockstep** (host = player 0, client = player 1). All paths below are relative to `prboom2/src/`.

### Files

| File | Role |
|---|---|
| `net_defs.h` | Wire constants, message types (`SETUP`, `READY`, `TICCMD`, `CHECKSUM`, `ADVANCE` (unused), `QUIT`), `net_setup_t`, global `net_session_t net_session` (`is_host`, `local_player`, `remote_player`, `socket`, `state`). |
| `net_transport.c/.h` | Raw TCP sockets (Win/POSIX), `TCP_NODELAY`. Packet framing `[u16 type][u16 len][payload]`, blocking/timeout receive, `net_wait_for_packet` (select). |
| `net_serialize.c/.h` | Big-endian (de)serialization of ticcmds (`NET_TICCMD_SIZE` = 15 bytes), setup (`NET_SETUP_SIZE`), checksums. **When you add a field to `ticcmd_t`/`net_setup_t`, update the size constant and both read/write functions here.** |
| `net_session.c/.h` | Handshake: host listens → sends `SETUP` → waits `READY`; client retries connect every 1s → applies setup → sends `READY`. `net_session_apply_setup` makes the host authoritative: it overrides the client's `-complevel`, `-skill`, `-fast`, `-respawn`, `-nomonsters`, `-warp`, `-from_key_frame`, game speed and the **RNG seed** (`rngseed`). `G_ReloadDefaults` in `g_game.c` skips its clock-based reseeding while a session is active; without that, the peers desync from tic 0 at complevel 9+. `net_session_disconnect` finishes any demo, sends `QUIT`, and drops the remote player so the local player can keep playing solo. |
| `d_client.c` | **The game-loop core.** `D_InitNetGame` (parses `-host 2` / `-join addr` / `-port`), `TryRunTics`/`NetSingleTic` → `NetRunOneTic` (lockstep), `NetUpdate` (build and send the local ticcmd), `NetRecvRemoteTic` (receive the remote ticcmd and checksums), desync checksums, pacing gate, synchronized key-frame ops, post-rewind self-replay. |
| `d_net.h` | Public API of `d_client.c`: `D_InitNetGame`, `NetSingleTic`, `NetResetState`, `NetGetTimeFrac`, `NetRequestKeyFrameOp`. |
| `d_ticcmd.h` | `ticcmd_t` and `excmd_t`. NexusDoom added the **wire-only** fields `ex.net_game_speed` and `ex.key_frame_op`, plus the `KF_OP_*` bit flags (`STORE`=1, `RESTORE`=2, `REWIND`=4). They are sent over the network but never written to demos. |

### Hooks in engine code

- `d_main.c`: calls `D_InitNetGame()` during startup. `D_DoomLoop` → `NetSingleTic()` / `TryRunTics()`. The wipe-at-full-speed check is skipped in MP.
- `SDL/i_system.c` `I_GetTimeFrac`: in MP, the render interpolation fraction comes from `NetGetTimeFrac()` (pacing gate), not the wall clock.
- `m_menu.c` (~L5370, ~L5485): the game-speed keys work only for the host in MP. The store / restore / rewind key-frame keys call `NetRequestKeyFrameOp(KF_OP_*)` instead of acting directly.
- `dsda/args.c/.h`: `-host`, `-join`, `-port` (default 26101), `-netdesyncdiag`. The existing `-from_key_frame` and `-game_speed` are synced via the setup message.
- `dsda/key_frame.c`: `dsda_StoreQuickKeyFrame`, `dsda_RestoreQuickKeyFrame`, `dsda_RewindAutoKeyFrame` (upstream DSDA logic, called from `d_client.c`). Restoring a key frame also restores each peer's own demo-recording buffer position and calls `dsda_QueueJoin()`.
- `dsda/demo.c/.h`: `dsda_DemoBufferOffset`, `dsda_GetDemoBuffer`, `dsda_BytesPerTic` (used by self-replay). `dsda_InitDemoRecording` doesn't require `-skill` in a netgame.
- `p_mobj.c`: `P_CountLivingMonsters()` (feeds the desync checksum).
- `g_game.c` `G_Ticker`: copies `local_cmds[i][gametic % BACKUPTICS]` into `players[i].cmd`. This is where both peers consume the same inputs.
- `g_game.c` `G_NormalizeTiccmd`: runs a cmd through the lossy demo encode/decode (angleturn → 8 bits without `-longtics`). `NetUpdate` applies it to every live local cmd. **Why:** a recording peer round-trips *every* player's cmd in `G_Ticker` (`G_WriteDemoTiccmd`), so un-normalized cmds from a non-recording peer would execute differently on the two machines → desync as soon as that player turns. `G_BuildTiccmd` also uses its angle-carry logic in MP for the same reason.

### One lockstep tic (`NetRunOneTic` in `d_client.c`)

1. **Pacing gate.** Wait until `net_pacing_next_tic_us`; the interval comes from `dsda_GameSpeed()`. While waiting: sleep ≤5 ms, run `M_Ticker`, update the interpolation fraction, and return 0.
2. **`NetUpdate`.** Build the local cmd for `maketic` (only one tic ahead). The cmd comes from the self-replay buffer if one is active, otherwise from `G_BuildTiccmd`. The host stamps `ex.net_game_speed`. Any pending `key_frame_op` is stamped into `ex.key_frame_op`. Send `TICCMD`.
3. **`NetRecvRemoteTic`.** Read packets until the remote cmd for `gametic` has arrived. It handles `CHECKSUM`/`QUIT` along the way. On a timeout it shows "waiting for peer" and returns 0, so the UI stays responsive.
4. **Game speed.** Apply the host's `net_game_speed` from that tic's cmd on both peers.
5. **Key-frame ops.** `combined_op = local_op | remote_op`, executed on both peers at the same `gametic`. `STORE` saves a quick key frame. `RESTORE`/`REWIND` restore it, then `NetResetAfterRestore()` sets `maketic = remote_maketic = gametic` and resets pacing and checksums. The function then returns 0 without ticking, so this tic's cmds are discarded and rebuilt.
6. `M_Ticker`, `G_Ticker`, **`NetMaybeSendChecksum`** (every 35 tics: an FNV hash of the RNG index, both players' x/y/z/health, and the living-monster count; a mismatch shows "out of sync"). With `-netdesyncdiag`, each peer also logs a `Net sim settings` line (complevel, rngseed, skill, …) at the first checksum, so both terminals can be compared directly. Note: `rng.rndindex` counts *gameplay* random calls and `prndindex` counts `pr_misc` (non-gameplay) calls, the opposite of what the names suggest, then `gametic++`.

The HUD status line (`NetUpdateOutOfSyncMessage`) shows, in priority order: connection lost > waiting for peer > out of sync > replay.

### Key-frame features

- **Store/restore:** a quick key frame stored in memory on both peers (identical game state; each peer's own demo buffer).
- **Rewind:** `dsda_RewindAutoKeyFrame` jumps back to the previous *auto* key frame. Auto key frames are stored every `auto_key_frame_interval` seconds, based on a **local config** value, so both peers must use the same interval and depth.
- **`-from_key_frame file`:** the host's filename is sent in `SETUP`. Both peers need an identical `.kf` file on disk.
- **Post-rewind self-replay** (`NetStartSelfReplay` / `NetEndSelfReplay`): after a restore or rewind, each *recording* peer copies its own player's cmds from the restored point up to the pre-rewind point out of its demo buffer. Those cmds then feed `NetUpdate` instead of the keyboard. Restore/rewind always execute on both peers, even mid-replay. A replay still in progress has its unplayed tail appended to the new replay, so it still leads back to the original point. Replayed cmds are zeroed before `G_ReadOneTick`, which only fills demo fields; otherwise stale wire-only fields such as `key_frame_op` would be resent. Pressing `input_join_demo` ends the replay for the local player only. It's handled entirely in `NetUpdate`: nothing is sent over the network, because the replay only changes where the local cmd comes from.

## Current state / known issues (update as work progresses)

- Self-replay only happens on a peer that is recording a demo (`demorecording`). This is harmless for sync, because replay only changes where the local cmd comes from.
- Out of sync after two quick rewinds: likely fixed together with the rewind-during-replay desync (restore/rewind used to be skipped on a peer that was still replaying). Needs retesting. If it persists, suspect auto-key-frame settings or the wall-clock "slow key framing" timeout, which differ per machine.
