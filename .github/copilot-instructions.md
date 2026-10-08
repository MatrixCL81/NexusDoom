## Golden rule: game mechanics must stay identical to DSDA-Doom

Speedruns are only valid if the game behaves exactly like the original. **Never change how the engine executes a ticcmd:** nothing in the play simulation (`p_*.c`, `m_random.c`, `r_*.c` where it affects gameplay, `G_Ticker`'s command handling, demo encoding/decoding, compatibility levels). Multiplayer code may only:

- decide *which* ticcmds are fed in and *when* (input building, networking, lockstep, key-frame ops, self-replay),
- read game state (e.g. checksums),
- make sure both peers feed identical ticcmds and settings into the unchanged engine.

Rounding input to demo precision (`G_NormalizeTiccmd`) is allowed: vanilla Doom does exactly that while recording. It changes the input, not the mechanics. If a change touches anything beyond input and networking, stop and ask the user first.

**How to verify:** build unmodified DSDA-Doom (the upstream commit NexusDoom branched from) and NexusDoom. Play every demo from the replays folder in both with `-iwad <doom.wad> -timedemo <demo> -nodraw -nosound -levelstat -analysis`, each in its own working directory with its own `HOME`. The `Timed N gametics` line, `levelstat.txt` and `analysis.txt` must be identical for every demo. Last verified 2026-09-27: all 1,725 demos (1,721 playable, 630 finished levels, 277 NexusDoom multiplayer recordings) were identical.

## Building

Build out-of-tree against **this** repo's `prboom2/`:

```bash
mkdir -p build && cd build
cmake ../prboom2 -DCMAKE_BUILD_TYPE=Release
make -j"$(nproc)"      # produces ./nexusdoom
```

### Local instructions

`local_instructions.md`, if it exists, provides guidance for local setup and environment-specific instructions. This file is in `.gitignore`, so that it can contain sensitive or machine-specific information without being committed to the repository. The paths to WAD files, replays, the upstream DSDA-Doom checkout and other local resources are specified there. **Read it before building, running the game or verifying demos.**

## Multiplayer architecture

NexusDoom multiplayer is a small, custom **2-player UDP lockstep** (host = player 0, client = player 1). The host must forward **UDP** port 26101 (or `-port`). All paths below are relative to `prboom2/src/`.

### Files

| File | Role |
|---|---|
| `net_defs.h` | Wire constants, `NET_PROTOCOL_MAGIC` (change it whenever the wire format changes), message types (`HELLO`, `SETUP`, `READY`, `TICCMD`, `QUIT`), the TICCMD layout (`net_tics_msg_t`, see below), `net_setup_t`, global `net_session_t net_session` (`is_host`, `local_player`, `remote_player`, `socket`, `state`). |
| `net_transport.c/.h` | Non-blocking UDP sockets (Win/POSIX), `connect()`ed to the peer once known. One datagram = one packet `[u16 type][u16 len][payload]`; malformed datagrams are skipped. Send errors are ignored (resending covers them), and ICMP "port unreachable" errors are treated as "no data" (`SIO_UDP_CONNRESET` off on Windows). `net_wait_for_packet` (select). |
| `net_serialize.c/.h` | Big-endian (de)serialization of ticcmds (`NET_TICCMD_SIZE` = 15 bytes), TICCMD messages (`net_write_tics`/`net_read_tics`), HELLO, setup (`NET_SETUP_SIZE`). **When you add a field to `ticcmd_t`/`net_setup_t`, update the size constant and both read/write functions here.** |
| `net_session.c/.h` | Handshake over UDP: the client sends `HELLO` (protocol magic) every 500 ms until `SETUP` arrives, applies it and sends `READY` 3×. The host waits for a `HELLO` with the right magic (a wrong one gets `QUIT`, which the client reports as a version mismatch), `connect()`s to that client, sends `SETUP` (again for every repeated `HELLO`) and waits for `READY` or a first `TICCMD` (the client resends that itself). `net_session_apply_setup` makes the host authoritative: it overrides the client's `-complevel`, `-skill`, `-fast`, `-respawn`, `-nomonsters`, `-warp`, `-from_key_frame`, game speed and the **RNG seed** (`rngseed`). `G_ReloadDefaults` in `g_game.c` skips its clock-based reseeding while a session is active; without that, the peers desync from tic 0 at complevel 9+. `net_session_disconnect` finishes any demo, sends `QUIT` 3×, and drops the remote player so the local player can keep playing solo. |
| `d_client.c` | **The game-loop core.** `D_InitNetGame` (parses `-host 2` / `-join addr` / `-port`), `TryRunTics` → `NetRunDueTics` / `NetSingleTic` → `NetRunOneTic` (lockstep), `NetUpdate` (build and queue the local ticcmd), `NetSendTics` / `NetPollNetwork` / `NetHandleTics` (reliable cmd stream over UDP, see below), `NetRecvRemoteTic` (wait for the remote ticcmd, resend timer, peer timeout), desync checksums, pacing gate and render clock, synchronized key-frame ops, post-rewind self-replay. |
| `d_net.h` | Public API of `d_client.c`: `D_InitNetGame`, `NetSingleTic`, `NetResetState`, `NetGetTimeFrac`, `NetRequestKeyFrameOp`. |
| `d_ticcmd.h` | `ticcmd_t` and `excmd_t`. NexusDoom added the **wire-only** fields `ex.net_game_speed` and `ex.key_frame_op`, plus the `KF_OP_*` bit flags (`STORE`=1, `RESTORE`=2, `REWIND`=4). They are sent over the network but never written to demos. |

### Hooks in engine code

- `d_main.c`: calls `D_InitNetGame()` during startup. `D_DoomLoop` → `NetSingleTic()` / `TryRunTics()`. The wipe-at-full-speed check is skipped in MP.
- `SDL/i_system.c` `I_GetTimeFrac`: in MP, the render interpolation fraction comes from `NetGetTimeFrac()` (render clock, see below), not the wall clock.
- `dsda/time.c` `dsda_LimitFPS`: in MP the background FPS limit (`dsda_background_fps_limit`, default 35) isn't applied. An unfocused peer at 35 fps sends its cmds up to a frame late, which made the *other* player's view stutter.
- `m_menu.c` (~L5370, ~L5485): the game-speed keys work only for the host in MP. The store / restore / rewind key-frame keys call `NetRequestKeyFrameOp(KF_OP_*)` instead of acting directly.
- `dsda/args.c/.h`: `-host`, `-join`, `-port` (default 26101), `-netdesyncdiag`, `-netframelog <file>` (per-frame/per-tic timing; analyse with `tools/netframelog_analyze.py`, which reports how far the rendered view deviates from the wall clock). The existing `-from_key_frame` and `-game_speed` are synced via the setup message.
- `dsda/key_frame.c`: `dsda_StoreQuickKeyFrame`, `dsda_RestoreQuickKeyFrame`, `dsda_RewindAutoKeyFrame` (upstream DSDA logic, called from `d_client.c`). Restoring a key frame also restores each peer's own demo-recording buffer position and calls `dsda_QueueJoin()`.
- `dsda/demo.c/.h`: `dsda_DemoBufferOffset`, `dsda_GetDemoBuffer`, `dsda_BytesPerTic` (used by self-replay). `dsda_InitDemoRecording` doesn't require `-skill` in a netgame.
- `p_mobj.c`: `P_CountLivingMonsters()` (feeds the desync checksum).
- `g_game.c` `G_Ticker`: copies `local_cmds[i][gametic % BACKUPTICS]` into `players[i].cmd`. This is where both peers consume the same inputs.
- `g_game.c` `G_NormalizeTiccmd`: runs a cmd through the lossy demo encode/decode (angleturn → 8 bits without `-longtics`). `NetUpdate` applies it to every live local cmd. **Why:** a recording peer round-trips *every* player's cmd in `G_Ticker` (`G_WriteDemoTiccmd`), so un-normalized cmds from a non-recording peer would execute differently on the two machines → desync as soon as that player turns. `G_BuildTiccmd` also uses its angle-carry logic in MP for the same reason.

### Reliable cmd stream over UDP (`d_client.c`)

- Each peer numbers the cmds it sends 0, 1, 2, … (`net_local_seq`). Unlike tics, **sequence numbers keep counting through key-frame restores**, so a datagram from before a rewind can never be mistaken for one after it. A restore resets `maketic`/`remote_maketic`, but the streams just continue.
- Every TICCMD datagram carries all our cmds the peer hasn't acknowledged (`net_send_ring`, from `net_peer_ack` up to `net_local_seq`), our ack (`net_remote_seq`), our latest desync checksum and `net_epoch` (restores so far). So a lost datagram is covered by the next one. Lockstep keeps the unacknowledged range to a few cmds (max `NET_TICS_MAX_CMDS` = 12, else disconnect).
- `NetPollNetwork` (start of every `NetRunOneTic`) reads all waiting datagrams. `NetHandleTics` takes cmds strictly in sequence. **It won't take the cmd after a not-yet-executed tic whose cmds contain `RESTORE`/`REWIND`**: that cmd belongs to the restored timeline, so it's only accepted once we've restored too. The peer keeps resending it meanwhile.
- While waiting for the remote cmd, `NetRecvRemoteTic` resends every 20 ms (`NET_RESEND_US`), flagged `NET_TICS_WAITING`. A peer that receives such a request and sees cmds missing that it already sent resends at once. That recovers a loss in either direction within ~20 ms plus one trip, where TCP waited for its ≥200 ms retransmission timeout.
- Checksums are only compared when the datagram's epoch matches ours and the checksum is newer than the last one seen.
- No datagram from the peer for 30 s while waiting (`NET_PEER_TIMEOUT_US`) → "connection lost", and the session ends. A received `QUIT` ends it at once.
- `tools/netimpair.py` is a local UDP relay that adds delay, jitter, loss, duplication and reordering, for testing (`-join` its port instead of the host's).

### One lockstep tic (`NetRunOneTic` in `d_client.c`)

`NetRunOneTic` never blocks: whenever it has to wait it returns 0, so `D_DoomLoop` keeps rendering frames. `TryRunTics` calls it via `NetRunDueTics`, which runs up to 4 tics per frame when the peer has fallen behind its schedule (a peer with fewer frames than tics per second would otherwise slow both players down).

1. **Pacing gate** (skipped while a tic is already due). Wait until `net_pacing_next_tic_us`; the interval comes from `dsda_GameSpeed()`. While waiting: sleep ≤5 ms, run `M_Ticker`, and return 0. When it fires, the tic becomes *due* (`net_tic_due`, planned time `net_tic_due_plan_us`).
2. **`NetUpdate`.** Build the local cmd for `maketic` (only one tic ahead). The cmd comes from the self-replay buffer if one is active, otherwise from `G_BuildTiccmd`. The host stamps `ex.net_game_speed`. Any pending `key_frame_op` is stamped into `ex.key_frame_op`. Queue the cmd in the send ring and send `TICCMD`.
3. **`NetRecvRemoteTic`.** Check whether the remote cmd for `gametic` has arrived (received by `NetPollNetwork`). If not, resend on the 20 ms timer, return 0 and render a frame. After 1 s it shows "waiting for peer" and also waits on the socket for up to 20 ms per call. After 30 s of silence it disconnects.
3b. **Hold (render clock).** The remote cmd arrives some time after our gate fired (gate phase offset between the peers, network latency, the other peer's frame rate), and that delay varies per tic. Executing on arrival made the tics unevenly spaced, so the interpolated view froze and jumped every tic. This was the "host stutters" bug: the old code even blocked rendering while waiting, and computed the fraction from the gate's schedule. Now each tic executes at `plan + net_render_lag_us`. The lag rises at once to any new maximum delay, decays by 1/64 interval per tic, and is capped at ⅞ interval: holding past the next gate would delay our next cmd and make the peer wait in turn. A delay ≥ 1 interval counts as a stall and executes immediately. `NetGetTimeFrac` = (now − execution time of the latest tic) / interval, computed at render time.
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

- **Latency limit (lockstep without input delay):** a tic can't complete faster than the one-way network latency, because each peer only sends cmd g+1 after executing tic g. So at 100% speed (28.6 ms/tic), more than ~28 ms one-way latency (RTT > ~57 ms, e.g. a mobile hotspot) slows the game below 35 tics/s; a simulated 40 ms one-way delay gave ~24 tics/s. At 25% speed the limit is ~114 ms. This is inherent to lockstep without input delay, which is a deliberate design choice. Measured with a simulated 138 ms ping (Finland ↔ California): at most ~13.7 tics/s whatever the speed setting; 35% and slower run smoothly at full rate, and 30% leaves room for jitter.
- Render smoothness was verified with headless runs (`SDL_VIDEODRIVER=dummy`, two local instances, `-netframelog`), including simulated peer frame rates of 35/60/240 fps and 10–40 ms latency (via a delaying proxy). Mean view error went from 0.5–44 ms per frame to ~0.1 ms. The user confirmed it visually.
- UDP transport verified headless through `tools/netimpair.py`: up to 20% loss with duplication and reordering, plus a temporary test hook (not committed) that made both peers request ~20 quick restores/rewinds per run. Result: no desyncs, the demos recorded on both peers had identical tic data, and the host detected a killed client after 30 s. The Windows (MSVC) build compiles without warnings in the net code, and a local host + client test on Windows (2026-10-06, headless, 420 tics) handshaked over UDP and had identical checksums. **Not yet verified:** a real internet session.
- A quick key-frame restore plays the screen wipe (`dsda_RestoreKeyFrame(&quick_kf, false)`), which in MP runs at game speed: ~1.1 s at 100%, ~3.8 s at 30%. Rewinds skip it. This is unchanged behaviour, not a network stall.
- Self-replay only happens on a peer that is recording a demo (`demorecording`). This is harmless for sync, because replay only changes where the local cmd comes from.
- Out of sync after two quick rewinds: likely fixed together with the rewind-during-replay desync (restore/rewind used to be skipped on a peer that was still replaying). Needs retesting. If it persists, suspect auto-key-frame settings or the wall-clock "slow key framing" timeout, which differ per machine.
