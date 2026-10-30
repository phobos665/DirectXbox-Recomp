# Remote play: controllers 2-4 from another machine

Split-screen and shared-screen titles (Kung Fu Chaos, TimeSplitters 2's
multiplayer, party games) played over the internet, the way Steam's Remote Play
Together or Parsec do it: the title runs once, on the host, whose player is
controller 1, and friends join as controllers 2, 3 and 4. Nothing has to stay in
step between machines, because there is only one copy of the game, so it works
for every title without per-title work.

The other way -- every machine running the title and exchanging pad inputs each
frame (netplay) -- needs every copy to compute exactly the same frames. Here
guest threads run on real host threads, game logic reads the host's clock and
audio callbacks and disc reads finish at different moments on each machine, and
there are no save states to roll back to, so two copies drift apart within
seconds. Not attempted.

## Status (30 Oct 2026)

| Piece | State |
| --- | --- |
| Remote pads into ports 2-4 (`src/input/remote_pads.c`) | Done. Checked by `tests/remote_pads` on 127.0.0.1, and end to end with Constantine as host: a joiner arrives as controller 2, the title reads its A press on port 2, and the port is unplugged when it leaves |
| The joiner, `xboxrecomp-remote` (`src/input/remote_join.c`) | Done; no game code in it. Reads the joiner's own controller 1 bindings |
| The picture back to the joiners | Not yet. Watch the host's screen some other way (a call's screen share) meanwhile |
| The sound back | Not yet |
| Launcher: a Remote play row, and joining from the launcher | Not yet |

## Hosting

```
remote_play      = host       [RECOMP_REMOTE_PLAY]       (off by default)
remote_play_port = 27620      [RECOMP_REMOTE_PLAY_PORT]
remote_play_code = <text>     [RECOMP_REMOTE_PLAY_CODE]  (optional, up to 16 characters)
```

The host listens on that UDP port and, unless `syslink_upnp = 0`, asks the
router to forward it as System Link does (`net_upnp_open_async`). The log says
what to give out:

```
[REMOTE] hosting remote play on UDP port 27620 with a code: friends join as controllers 2-4 at 192.168.1.5:27620 (from the internet, the router's address)
[REMOTE] Ann joined as controller 2
[REMOTE] Ann (controller 2) left
```

A joiner is given the lowest of ports 2-4 that no device of the host's own is on
(`recomp_bindings_present_mask` is the host's devices plus the remote players),
and keeps it until it says goodbye or goes quiet for three seconds. The title
sees a controller plugged in and pulled out, through `XGetDeviceChanges`, as on
the console. Rumble on a remote port is sent back to its player.

Only a running title hosts: the host starts at the bindings' first use, which
only the title's input paths (`src/hle`, `src/usb`) make, never the launcher.

## Joining

```
xboxrecomp-remote <host>[:port] [--code <text>] [--name <text>]
```

Built with the toolkit (`src/input`), not with a title. The window is grey while
it asks, green once in and red if refused, and its title says which controller
and the round trip. Keep it in front to play on the keyboard; a pad works
either way, and with no window at all (`[REMOTE] no window`).

## The wire

UDP, little-endian, one message a datagram: `u32 'XRRP'`, `u8 version`,
`u8 type`, then HELLO (code, name), WELCOME (port), REFUSED (full, wrong code,
other version), PAD (sequence, the whole XBOX_GAMEPAD), RUMBLE, BYE, PING/PONG.
The joiner sends the pad's whole state every 8 ms, so a lost datagram costs
nothing and a late one is dropped by its sequence number. `remote_pads.h` has
the layout.

## Next

1. The picture: the host's presented frame is 640x480, so 60 fps H.264 is about
   3-6 Mbit/s. Read it back without stalling the GPU, encode in hardware
   through FFmpeg (VideoToolbox, NVENC/AMF/QSV/Media Foundation, VAAPI), send
   in the same session; the joiner decodes (FFmpeg is already loaded at run
   time for XMV) and shows it.
2. The sound: tap the host's mix, Opus.
3. The launcher: host from the title's settings, join by address and code.
4. A relay for hosts whose router will not forward (CGNAT), if direct
   connections turn out not to be enough.
