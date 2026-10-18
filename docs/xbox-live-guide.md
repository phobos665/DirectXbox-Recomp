# Xbox Live (Insignia) with a recompiled game

[Insignia](https://insignia.live/) is a revival of the original Xbox Live,
which Microsoft shut down in 2010. A recompiled game carries its own Xbox
Live client (XOnline and XNet, lifted with the game), so it can sign in to
Insignia the way a console does, as long as it is given what a console has:
a network connection, the console's identity, and the accounts on its disk.

This guide covers what a player has to do, step by step. How it works
inside, and what it took, is in `docs/technical/xbox-live.md`. Each game
project has its own page for the steps specific to that game; Rainbow Six 3's
is `docs/xbox-live.md` in its repository.

## Status

Checked end to end on macOS with Rainbow Six 3: it signs in, Insignia says a
title update is required, the recompiled dashboard updater downloads and
installs it, and the updated game signs in and plays online.

| | |
| --- | --- |
| Sign-in (Kerberos, security gateway, logon) | works |
| Title updates from Insignia | works, through the recompiled 5960 dashboard updater |
| Friends, matchmaking, playing online | works (Rainbow Six 3) |
| macOS | checked |
| Windows, Linux | written, not yet run with Xbox Live |
| Other titles | not yet tried; each may need its own steps (a Live block, a title update build) |

## What you need

- **Your own original Xbox, softmodded** (running homebrew such as the EvoX
  dashboard), to register it with Insignia and back up its identity. The
  recompiled game signs in **as your console**: it uses your console's
  EEPROM, HDD key and accounts.
- **An Insignia account** for that console (free; see insignia.live).
- **FTP** to copy files off the console (EvoX, UnleashX and XBMC serve it).
- **The game's files**, built with this toolkit.

Terms used below:

- **EEPROM**: a 256-byte chip holding the console's serial number, Ethernet
  address and **online key**, the secret Xbox Live knows the console by.
- **HDD key**: a per-console key that signs data on the hard disk. Insignia
  refuses a console whose HDD key is all zeros.
- **Machine account / user account**: the console's own account
  (`SN.<serial>`) and your gamertag. Both live in the first sectors of the
  console's hard disk.

These files identify your console. **Never share or commit them.** Anyone with
them can sign in as your console.

## Step 1: sign up on the real console

Once, on the console itself:

1. If your modded BIOS blocks Xbox Live, run Insignia's **xblunblock** first.
   It patches memory only, so it is gone at power-off.
2. Run the **Insignia Setup Assistant** to register the console.
3. Sign in once from the dashboard's Xbox Live section, so the console has
   your gamertag on it.

## Step 2: back up the console

1. Run EvoX's EEPROM backup (`evoxdash.xbe`, menu **Backup eeprom**). It
   writes `eeprom.bin`, `hddinfo.txt` and `disk.bin` (the first 512 KB of the
   drive, which holds the accounts).
2. Back up **after** Step 1. EvoX may keep an older backup instead of writing
   a new one: rename any older backup folder first.
3. Copy the three files off the console over FTP.
4. For title updates (Step 5): also copy the console's **whole C: drive**
   (`C:\`, about 100 MB): the dashboard, its `xodash\` folder with the online
   updater, and its fonts and sounds.

## Step 3: put the files where the game finds them

Make a folder called `xboxbackup/` beside the game folder:

```text
<game project>/
  game/            the game's files
  xboxbackup/      your console's identity
    eeprom.bin
    hddinfo.txt
    disk.bin
```

| File | Needed | What it gives |
| --- | --- | --- |
| `eeprom.bin` | yes | serial, Ethernet address, online key (its checksums are checked) |
| `hddinfo.txt` | for the HDD key | its `XboxHDKey` line, and the drive's serial and model |
| `disk.bin` | for the accounts | the machine account and your user account |

Make it private (`chmod 700 xboxbackup`). Game projects made from the
toolkit's template keep it out of git.

Elsewhere is fine too: set `console_dir = <folder>` (or `RECOMP_CONSOLE_DIR`),
or use `console/` in the per-user settings folder. `console_dir = off` turns
the identity off.

**The HDD key.** The runtime takes the first it finds, in this order:
`RECOMP_XBOX_HD_KEY`, then `hd_key = <32 hex digits>` in `keys.ini` (per-user
settings folder), then the `XboxHDKey` line of `hddinfo.txt`, then
`hd_key.txt`. **EvoX's `hddkey.bin` is the drive's lock password, not the
HDD key.**

**The accounts** are copied from `disk.bin` onto the game's virtual disk the
first time it runs. If you sign up again later, set
`console_accounts = replace` for one run to take the new ones.

## Step 4: connect the game to the internet

Turn on the runtime's built-in router. Either set it in the game's settings
file (macOS: `~/Library/Application Support/xboxrecomp/titles/<title id>.conf`):

```ini
syslink = nat
```

or set `RECOMP_SYSLINK=nat`. The game is given an address behind the router
(10.0.2.15) and Insignia's DNS server (46.101.64.175, then 1.1.1.1;
`net_dns` changes them). Then use the game's Xbox Live menu and sign in.

Nothing needs to be opened on your home router to sign in. Hosting games
works best with UDP port 3074 forwarded to this PC, as on a console.

## Step 5: title updates

Many games need an update before Insignia lets them sign in. The game then
says **"A required update is available for the Xbox Live service"**. On a
console, choosing OK runs the dashboard's online updater, which downloads the
update and installs it on the hard disk (`E:\TDATA\<title id>\$u`); the game
then runs the updated executable from there.

The recompiled game does the same, with three more programs in the chain: the
recompiled updater, and, after it, a build of the game made from the updated
executable. The game's own page says how to build that one.

### 5a. Build the online updater (once)

The updater is the 5960 dashboard's `update.xbe`, recompiled. Its project is
`titles/xodash-update` in the toolkit; its files come from your console's C:
drive (Step 2).

1. Make its game folder, `games/xodash-update/`, from your copy of C:
   - the contents of `C:\xodash\` (`update.xbe`, `media\`, `audio\`, ...),
     with `update.xbe` renamed to `default.xbe`;
   - everything at the root of `C:\` beside them (`xboxdash.xbe`, `fonts\`,
     `xodash\`, the `.xtf` fonts, `xboxdashdata.*`). On a disk that ignores
     case, `C:\Audio` and `xodash\audio` are one folder: merge them.
2. Make its XDK symbols, as for any title:
   `.venv/bin/python -m tools.xdk_symbols games/xodash-update/default.xbe --cli <XbSymbolDatabaseCLI>`
3. Link the folder into the project, lift and build:

   ```bash
   ln -s ../../games/xodash-update titles/xodash-update/game
   .venv/bin/python scripts/recompile.py games/xodash-update/default.xbe \
       --work-dir games/_pipeline/xodash-update --project titles/xodash-update
   cmake -S titles/xodash-update -B titles/xodash-update/build/Release -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build titles/xodash-update/build/Release --target xodash_update_recomp --parallel
   ```

### 5b. The settings that join the programs up

| Setting | Environment | What it does |
| --- | --- | --- |
| `hdd_dir = <folder>` | `RECOMP_HDD_DIR` | One E: drive for every program, as on a console. The game writes its saves there, and the updater installs the update there. **Copy the game's existing `game/UDATA` and `game/TDATA` into it first**, or the game starts without its saves. |
| `launch_map = <entries>` | `RECOMP_LAUNCH_MAP` | Which recompiled program to start when one launches another. `;`-separated `<name or title id>=<executable>`, optionally `\|<game folder>`. |
| `disc_overlay = <folder>` | `RECOMP_DISC_OVERLAY` | Files that stand in for the disc's (see below). |
| `system_dir = <folder>` | `RECOMP_SYSTEM_DIR` | The C: drive. The updater sets it to its own folder; you need not. |

A launch map for a game whose title ID is `55530013`:

```text
update.xbe=<toolkit>/titles/xodash-update/build/Release/xodash_update_recomp|<game project>/game;55530013=<the update build of the game>|<game project>/game
```

The game folder after `|` is the updater's D: drive: on a console the updater
runs from the game's disc and reads the disc's files to patch them.

**`disc_overlay`, for a hard-drive copy.** An update is a set of patches
against the disc's original files, each checked afterwards. A copy of a game
made to run from a console's hard disk has its `default.xbe` patched (the
original kept as `default.bak`), and often leaves out files the disc has. Put
the originals in a folder and point `disc_overlay` at it:

- `default.xbe`: a copy of the game's `default.bak`;
- any file the update patches that your copy is missing (the game's page
  says which).

A disc ripped whole needs no overlay.

### 5c. Run it

Start the game with `syslink`, `hdd_dir`, `launch_map` and `disc_overlay`
set. Sign in, choose **OK** on "A required update is available", then **Yes**
in the updater. It downloads and installs the update ("Xbox Live is updating
your system") and starts the game again, now updated.

## What the log says

| Line | Meaning |
| --- | --- |
| `[XBOX] console identity from <folder>: eeprom.bin, Ethernet address XX:XX:XX:..., HDD key from hddinfo.txt` | identity loaded (only the maker's part of the address is printed) |
| `[XBOX] Xbox Live accounts copied from disk.bin onto the disk ...` | accounts brought in (first run only) |
| `[NET] internet through this PC: the console is 10.0.2.15 behind the router at 10.0.2.2, ...` | the game has its network address |
| `[NET] DNS: AS.XBOXLIVE.COM, asked of 46.101.64.175` | sign-in has started |
| `[NET] UDP from the console's port 3074 leaves this PC from port <n> (its own was taken)` | something else on this PC holds 3074 (another copy of a game?) |
| `[PATH] E: (\Device\Harddisk0\Partition1) is <folder> (hdd_dir)` | the shared E: drive is in use |
| `[KERNEL] HalReturnToFirmware: launching '<path>' ... as <executable>` | one program handing over to the next (`launch_map`) |
| `[KERNEL] HalReturnToFirmware: '<path>' ... has no recompiled image in launch_map` | the game asked for a program that is not recompiled or not mapped; it exits |

No log line contains a key or an account secret.

## Troubleshooting

| Symptom | Likely cause | What to do |
| --- | --- | --- |
| "Cannot connect to Xbox Live" at once, and no `[NET] DNS` line | a hard-drive copy whose `default.xbe` blocks Xbox Live | lift a copy with the block undone (the game's page says how); keep `default.bak` |
| no `[NET]` lines at all | the router is off | Step 4 |
| `accounts: none in .../disk.bin`, or no gamertag in the game | backup made before signing up | sign up, rename the old backup folder, back up again; `console_accounts = replace` once |
| `fails its checksums` | not an EEPROM dump, or damaged | back up again |
| `no HDD key` or `HDD key is all zeros` | key not found, or `hddkey.bin` used | see "The HDD key" in Step 3 |
| "A required update is available" | the game needs a title update | Step 5 |
| the game closes on OK at "required update" | no `launch_map` entry for `update.xbe` | Step 5b |
| "To use the Xbox Live Dashboard, your system must be updated" in the updater | the updater cannot see the dashboard on C: | Step 5a: `xboxdash.xbe` and `xodash\` must be in its game folder |
| "Unable to continue due to technical difficulties" in the updater | it cannot reach the game's files on the shared E: | `hdd_dir` set for both programs, with the game's `TDATA` copied in |
| the update fails part-way | a file the update patches is not the disc's original | `disc_overlay` (Step 5b) |
| two copies cannot see each other in System Link | both load one identity | `console_dir = off` on one |

## Playing fair

Insignia's Terms of Use forbid probing, fuzzing, overloading or unauthorised
access. The recompiled game behaves as an ordinary client; keep it that way,
and keep logs and packet captures (`RECOMP_NET_PCAP`) private, since they hold
your sign-in traffic.

## How it fits together

| Piece | File | Settings |
| --- | --- | --- |
| Network card on Windows and POSIX | `src/kernel/xbox_nic.c` | `syslink` |
| Router: DHCP, ARP, DNS servers, UDP (3074 kept as 3074), IP fragments, ICMP echo, TCP relayed over this PC's sockets | `src/kernel/xbox_net_nat.c` | `syslink = nat`, `net_dns`, `RECOMP_NET_LOG`, `RECOMP_NET_PCAP` |
| Console identity: EEPROM, HDD key, drive serial and model | `src/kernel/xbox_console_id.c` | `console_dir`, `RECOMP_XBOX_HD_KEY`, `keys.ini` |
| EEPROM reads, single settings and whole sections | `src/kernel/kernel_xbox.c` | none |
| Accounts onto the virtual disk | `xbox_console_import_accounts` | `console_accounts = import / replace / off` |
| One program launching another | `src/kernel/kernel_bridge.c` (`HalReturnToFirmware`) | `launch_map` |
| Shared E:, the C: drive, the disc overlay | `src/kernel/kernel_path.c` | `hdd_dir`, `system_dir`, `disc_overlay` |
| The online updater | `titles/xodash-update` | |

Diagnostics: `RECOMP_NET_PCAP=<file>` writes every network frame for
Wireshark (private: it holds sign-in traffic); `RECOMP_XC_TRACE=1` logs each
kernel crypto call and EEPROM query (caller, lengths, short fingerprints,
never data).

## Lessons learned

1. **Hard-drive copies can block Xbox Live.** "HDD-ready" copies patch
   `default.xbe` (original kept as `default.bak`). In Rainbow Six 3 the logon's
   link test is replaced by four `nop`s, so it fails without sending a packet.
   No network traffic at all points here first.
2. **XNet reads the whole EEPROM at once.** Answering single settings was not
   enough: the block reads returned zeros.
3. **`hddkey.bin` is not the HDD key.** It is the drive's lock password.
4. **A backup from before sign-up has no user account**, and EvoX may keep
   the old backup unless its folder is renamed.
5. **Title updates need TCP.** Sign-in and play tunnel everything through UDP
   3074, but the updater downloads the update over plain HTTP.
6. **Errors have to be NT's exact ones.** XNet's "would block" status, a
   missing folder versus a missing file, an empty folder's first search: the
   game and the updater branch on each, and a near miss sent them down their
   failure paths.
7. **One console is several programs.** The game, the updater and the
   updated game share an E: drive, and the updater's D: is the game's disc.

## Next steps

| Item | What it would do |
| --- | --- |
| Launcher "Xbox Live" tab | pick the backup folder, show what was found (never the secrets), set `syslink`, `hdd_dir` and `launch_map` |
| Backup importer (local only) | take an EvoX, UnleashX or XBMC backup, check it, copy only what is needed, private permissions |
| Doctor | one command that checks every step above and prints a checklist |
| Live-block detector | for any title, compare `default.xbe` with `default.bak` and lift with the block undone |
| Not mapped, so go back | when a game launches a program with no recompiled version, return to the game instead of exiting |

Sources: Tristan Miller, *Reversing the Original Xbox Live Protocols*
(DEF CON 30); feudalnate/Original-Xbox-LIVE-Account; xboxdevwiki.
