# Android

TimeSplitters 2 plays on an Android handheld: an AYANEO Pocket Fit (Snapdragon
8 Gen 3, Adreno 750, Android 14) runs it at 60 fps in Siberia, with sound, the
console's pacing, widescreen, an in-game menu and touch controls. The work is on
`feat/android` (toolkit) and `feat/android` in split2-recomp. This page is what
someone picking it up needs: how the app is put together, how to build and test
it, and what is not done.

The port is the Linux one on arm64 with Android's C library: memory, faults,
threads and the guest lock are the macOS arm64 paths (`memory-layout.md`), the
renderer is the Vulkan RHI (`vulkan-backend.md`), and the window, sound and pads
are SDL3. What is Android's own is below.

## Building an APK

Once, DXC for Android (nobody publishes one; the renderer compiles HLSL at run
time):

```bash
git clone --depth 1 --branch v1.9.2609 --recurse-submodules --shallow-submodules \
    https://github.com/microsoft/DirectXShaderCompiler.git dxc-src
tools/android/build_dxc.sh dxc-src dxc-build ~/Library/Android/sdk/ndk/27.3.13750724
cp -R dxc-build/android/out/* third_party/dxc-android/     # ignored by git
```

It builds the host's `llvm-tblgen` and `clang-tblgen` first, then
`libdxcompiler.so` for arm64 with the NDK (about 28 MB stripped).
`HLSL_DISABLE_SOURCE_GENERATION` is on: regenerating DXC's headers runs the
host's clang-format, and a newer one fails the build.

Then, from a title repository whose game is already lifted for the desktop (the
lifted C is the same on every host):

```bash
python3 xboxrecomp/tools/android/build_apk.py --package org.xboxrecomp.ts2.dev --label "TS2 (dev)"
```

That configures `build/android` with the NDK (arm64-v8a, API 30, static libc++),
builds `libmain.so`, and packages it (`tools/android/make_apk.py`). There is no
Gradle: SDL's Java activity and `android/java` are compiled with javac and d8,
the manifest (`android/AndroidManifest.xml.in`) is linked with aapt2, and the
libraries are stored uncompressed, aligned to 16 KB and mapped from the APK. The
APK is signed with a key made once and kept in the per-user folder
(`android/signing.p12`): an update signed with another key is refused, and
replacing the app to change it deletes its saves. The icon is the toolkit's
unless the title's release recipe names one (`"android": {"icon": ...}`; see
`self-build-release.md`, "Android"). Needs the Android SDK (build tools,
platform 34+), an NDK and a JDK.

A title's `CMakeLists.txt` builds a `SHARED` library named `main` on Android,
compiles its `main.c` with `main=recomp_title_main`, and links
`xbox_android_main`; split2-recomp's is the example. Nothing else in the title
changes.

Install with `adb install -r <apk>`; `-r` keeps the app's folder and saves.

## How the app works

**Two activities.** `SetupActivity` is the launcher entry and finds the game;
`RecompActivity` is SDL's activity with the engine and title linked into one
`libmain.so` (SDL is static, so there is no `libSDL3.so`).

**Finding the game** (`SetupActivity.java`). A folder chosen before, then a copy
in the app's own folder (`<files>/game`, where `adb push` puts one), then: it
asks for all-files access and looks under `roms/`, `Download/`, `games/` and SD
cards for a `default.xbe` whose SHA-1 is the one the APK was built from (the
manifest's `xboxrecomp.xbe_sha1`). One match starts and is remembered, several
are listed, none asks for a path. Shared storage ignores case, so `roms/` and
`ROMs/` are one folder and matches are de-duplicated by case. The game is read
where it is and never written to.

**The entry** (`src/host/android_main.c`). `SDL_main` sends stdout and stderr
down a pipe to logcat (tag `xboxrecomp`) and to `<files>/xboxrecomp.log`, reads
`RECOMP_*` switches from `<files>/recomp_env.txt`, sets the folders, and calls
the title's `main` (renamed `recomp_title_main`). `<files>` is
`/storage/emulated/0/Android/data/<package>/files`: no permission needed, adb can
reach it, an uninstall deletes it.

| Folder | Default | Holds |
|---|---|---|
| `RECOMP_GAME_DIR` | what SetupActivity found, else `<files>/game` | the game, read only |
| `RECOMP_USER_DIR` | `<files>/user` | settings (`titles/<id>.conf`), input bindings, caches, saves |
| `RECOMP_HDD_DIR` | `<files>/hdd` | E: (`\Device\Harddisk0\Partition1`): TS2's `UDATA` profile |

E: is the game's own folder on a desktop. Here the game may be a ROM folder
other programs share, so E: is the app's.

**The window and the swap chain.** The window is fullscreen with the system bars
hidden. The Pocket Fit's panel is portrait underneath: the surface reports a 90
degree transform with a landscape extent. The swap chain asks for no
pre-rotation and lets the system rotate the frame, and the `SUBOPTIMAL` Android
then returns on every acquire and present is not a reason to rebuild it
(`SC.unrotated`, `rhi_vulkan.c`). Drawing pre-rotated ourselves would save the
compositor's rotation; it has not been needed.

**Pause and resume.** Android destroys the window when the app is paused and
makes a new one on resume. The host hands the renderer the window as SDL has it
now (`RhiNativeWindow.current`, read under SDL's property lock with a reference
taken, because SDL releases the window whether or not a swap chain uses it);
before each frame the renderer compares, drops the surface and swap chain when
the window is gone and makes both again when it is back. Meanwhile the title is
held at its next `Swap` (`host_suspended`, set from an SDL event watch on
`WILL_ENTER_BACKGROUND` / `DID_ENTER_FOREGROUND`), so the game does not play on
unseen.

**The in-game menu** (`GameMenu.java`). Back opens a panel down the right:
Resume, Settings, Quit game. A controller's own Back button is left to the game.
The title is held while the menu is open (`HOST_SUSPEND_MENU`; the reasons are
bits, so coming back from the background with the menu open keeps it held).
Settings call into `android_main.c` over JNI and apply at once where the runtime
can change them: the frame rate counter and cap (`hle_settings.h`, the desktop's
F9 and F10) and the touch controls. Each change asks for a redraw
(`host_request_redraw`): the held thread presents its frame again with the
change (`xbox_D3D8Represent`), so it shows behind the menu while the game stays
where it is. Widescreen and resolution are read only at start and are named
"(requires restart)". Every change is written to the title's settings file.
Quit game asks first, in a modal with No focused.

The menu moves its own focus for a pad (d-pad between rows, left and right on
sliders and in the question, A or Enter to press, B back a page, Start closes).
Two things learned the hard way: SDL's activity takes the arrow keys before
Android's focus movement sees them, and since Android 8 any clickable view is
focusable, so the menu's shade held the focus and A "pressed" it.

**Touch controls** (`src/input/touch_controls.c`, `src/d3d/d3d8_touch.c`). A
whole Xbox pad in the "Edges" layout, chosen from mock-ups: triggers in the top
corners, White and Black under them, d-pad and face buttons at mid height,
sticks at the bottom, Back and Start top centre, sized from the screen height
with each side measured from its own edge. How each control reads follows Steam
Link's controller as DroidDeck uses it (a stick is its offset clamped to the
ring, eight 67.5 degree d-pad sectors, a finger slides between buttons). They
are added to controller 1 in `recomp_bindings_sample` and drawn over the
presented frame, bars included, as distance functions in one pass. `auto` (the
Android default) shows them with no pad connected and hides them when a pad is
connected or pressed; a touch brings them back. `touch_controls`,
`touch_opacity` and `touch_scale` are in the settings file and the menu.

**Faults.** The MMIO emulation's SIGSEGV/SIGBUS handlers sit behind ART's own
(libsigchain): ART takes the faults in managed code and passes the rest on. No
change was needed.

## Testing on a device

Every run follows the device rules: our own package only (never another app or
its data), `RECOMP_MUTE=1`, a time limit, fresh saves; no uninstall, settings
change or reboot without asking. Only `install`, `logcat`, `screencap`, input
injection and files pushed into the app's own folder.

```bash
P=/sdcard/Android/data/org.xboxrecomp.ts2.dev/files
R=$P/runs/<label>
adb shell "mkdir -p $R/hdd $R/saves $R/user && chmod 777 $R $R/hdd $R/saves $R/user"
printf 'RECOMP_MUTE=1\nRECOMP_SAVE_DIR=%s/saves\nRECOMP_HDD_DIR=%s/hdd\nRECOMP_USER_DIR=%s/user\n' $R $R $R > env.txt
adb push env.txt $P/recomp_env.txt
adb shell am start -n org.xboxrecomp.ts2.dev/org.xboxrecomp.app.RecompActivity
sleep 90; adb exec-out screencap -p > shot.png
adb shell am force-stop org.xboxrecomp.ts2.dev
adb pull $P/xboxrecomp.log
adb shell rm $P/recomp_env.txt          # or the player's next launch is muted
```

`RECOMP_INPUT_SEQ` (split2-recomp's `CLAUDE.md`) walks a fresh save to Siberia.
A copy of the game for testing goes in `<files>/game` (`cp -r` on the device,
then `chmod -R a+rwX`, since files the shell writes are not the app's).

Things that cost time:

- **Someone else may be holding the device.** Check
  `dumpsys activity activities | grep Resumed:` and `dumpsys power | grep
  mWakefulness` before a run. A screen that has gone to sleep sends the app to
  the background as soon as it starts (black screenshots), and keys injected
  while the app is not in front go to whatever is.
- **Files the app writes cannot be read or deleted from adb** (`Permission
  denied` under `<files>`), so give each run its own folders rather than
  cleaning up after one.
- **`adb install -r` closes the running app.**

## What the desktop shares

These changes run on every host and need a Windows regression before `main`:
the flip gate's hold and redraw (`xbox_memory_layout.c`), `host_suspended` and
`host_request_redraw` (`host_main_*.c`), the frame rate counter drawn at present
time over the swap chain (its size follows the picture; on Windows its GDI text
is scaled in whole steps), the settings file's touch lines, and the swap chain's
handling of a rotated surface (which a desktop never reports).

## Not done

- **Distribution: not decided.** Today's APK holds the title's lifted code, which
  `CLAUDE.md` ("Legal") says is never distributed. A release a player installs
  and then points at their own disc needs the game's code made somewhere other
  than inside the published APK. There will be no Google Play builds.
- **The DXT fallback and the unsupported-GPU notice.** Adreno has BC textures;
  Mali devices are untested and unsupported for now.
- **An arm64 Linux CI job**, the first CI run of the arm64 fault and memory
  paths.
- **Input remapping in the menu** (a separate branch).
- **A per-title icon for TimeSplitters 2.**
- **Building on the device.** Out of scope.

## The Pocket Fit

| | |
|---|---|
| SoC / GPU | Snapdragon 8 Gen 3 (`pineapple`), Adreno 750, driver 0762.24 (Jul 2024) |
| Vulkan | 1.3.128; BC and ETC2 textures, dual-source blend, geometry shaders |
| Android | 14 (API 34), 4 KB pages, 12 GB |
| Screen | 1920x1080 landscape (panel 1080x1920), 440 dpi |
| TS2 | 60 fps in Siberia, about 1.5 cores busy, 48.7 MB APK |
