# Getting your own Xbox dashboard files

To sign up for Xbox Live (Insignia) without a console backup, you bring one
thing: **the files of the Xbox dashboard, version 5960**, from an Xbox you own.
The recompiled online dashboard in them signs you up, and the online updater
in them installs title updates. Nothing else of the console is used: not its
BIOS, its EEPROM or its hard disk key.

These are Microsoft's files. They are never shipped with a game built here;
each player brings their own, as with the game itself.

## What you need

The dashboard lives on the Xbox's **C: drive**, about 100 MB. Copy all of it;
these are the parts that are used:

| On C: | What it is |
| --- | --- |
| `xboxdash.xbe` | the dashboard |
| `xodash\xonlinedash.xbe` | the online dashboard: Xbox Live sign-up |
| `xodash\update.xbe` | the online updater: title updates |
| `xodash\media\`, `xodash\audio\` | the online dashboard's graphics and sounds |
| `media\`, `audio\`, `fonts\`, `Xbox.xtf`, `XBox Book.xtf` | the dashboard's graphics, sounds and fonts |

**It must be version 5960**, the last dashboard, which Insignia also requires.
On the Xbox, **Settings, System Info** shows the version once it has loaded:
it should read `D:1.00.5960.01`.

## From an Xbox

You need a way to copy files off it, which means a console that runs an FTP
server: a **softmodded** or **modchipped** Xbox running a dashboard such as
EvoX, UnleashX or XBMC. A stock, unmodified Xbox has no way to copy files off.
Insignia's guide lists softmods known to work with Xbox Live
([insignia.live/guide/connect](https://insignia.live/guide/connect)), and
[consolemods.org](https://consolemods.org/wiki/Xbox:Softmodding_Guide) covers
softmodding.

1. **Update the dashboard to 5960** if System Info shows an older one:
   Insignia's guide says how.
2. **Connect** the Xbox to your network and note its IP address (the modded
   dashboard shows it).
3. **Copy C: over FTP** with any FTP client (FileZilla, Cyberduck, `ftp`). The
   usual login is `xbox` / `xbox`. C: appears as a folder named `C` at the
   top. Copy the whole drive into a folder on your computer, keeping its
   folders as they are.
4. **Check** that the folder holds `xboxdash.xbe` and `xodash/xonlinedash.xbe`.

## From an xemu hard disk image

xemu's own disk image ships a different dashboard (xemu-dashboard), not
Microsoft's. If you have installed your own console's 5960 dashboard into an
xemu disk image (xemu's guide: [xemu.app/docs/dashboard](https://xemu.app/docs/dashboard/)),
its C: drive can be copied out with [fatx](https://github.com/XboxDev/fatx):

```bash
qemu-img convert -O raw xbox_hdd.qcow2 xbox_hdd.img   # xemu's image is qcow2
pip install pyfatx
mkdir dashboard && cd dashboard
python -m pyfatx -x ../xbox_hdd.img                   # extracts C: (the default drive)
```

`gfatx`, in the same project, does the same with a window. FATXplorer is a
Windows tool that does it too.

## Keep it private

The C: drive holds no secrets of your console (those are in its EEPROM and
the first sectors of its disk, which this does not need), but it is
Microsoft's software: do not share it or commit it to a repository.

## Then

Go back to the Xbox Live guide, `docs/xbox-live-guide.md`, "Without a
console's backup", Step 3.
