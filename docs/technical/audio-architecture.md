# Audio: how Xbox sound works, and a design that covers every title

Status (14 October 2026, branch feat/audio-rework): steps 0 to 3 of the plan
are built and TimeSplitters 2 plays through the APU path; see "Built" below.
The native path is still the default.

## Built

Choose the path with `RECOMP_AUDIO=apu|native` (or `audio = ...` in the
settings file). Native is the default.

| Piece | Where | Checked |
| --- | --- | --- |
| Envelope index (SetEG's dwEG 1 is amplitude) | `hle_dsound_3d.c` | TS2's effects heard on the native path |
| A slowed buffer's pre-roll in playing time; voice queues of 64 | `hle_dsound.c`, both backends | no queue-full refusals in a TS2 run |
| One fold-down from Xbox speakers to the host's, SDL included | `src/hle/audio_speakers.c` | `tests/audio_dsp`; SDL no longer plays mixed voices 10 dB low |
| `[AUDIO] health` line; regress and run_and_report judge it | `src/hle/audio_health.c`, `scripts/` | prints "effects silent" for the envelope bug |
| Run-time switch: grouped HLE falls through to the XDK's body | `tools/recomp/hle.py` (`HLE_GROUP`, `HLE_APU_OBSERVE`), `src/hle/audio_path.c` | TS2: 29 replacements fall through, 2 observe |
| xemu's current voice processor, real libsamplerate, HRTF on | `src/apu/apu_vp_xemu.c`, `third_party/libsamplerate` | builds on clang; MSVC patches marked |
| Physical addresses through `xbox_PhysToGuest` | `src/apu/apu_shim.h` | buffers in the XBE image read from low RAM |
| The APU interrupt on vector 5 (it was 6, the AC'97's) | `src/kernel/kernel_bridge.c` | Flush no longer waits 5 s; ISR claims and acknowledges |
| Speaker stage, one host stream, paced by the device | `src/apu/apu_core.c`, `recomp_audio_output_mix_*` | TS2 to Siberia: every voice heard, 60 fps |
| Reverb: the room observed on the APU path; XAudio2's reverb, and one for SDL | `hle_dsound_3d.c`, `src/hle/audio_reverb.c` | `tests/audio_dsp` (T60 within 3%) |

Not yet:

- Not compiled on Windows: the XAudio2 half of the APU stream and the
  health hooks, and MSVC's view of `apu_vp_xemu.c`. Build and run there
  before merging.
- Not listened to by a person on the APU path. Scripted runs measure that
  every voice is heard; they cannot say it sounds right.
- Step 4 (every title on the APU path) has one title. The others in
  `games/` are not on this machine.
- Step 5 (the GP) is not started: xemu's DSP interpreter is 13.7k lines,
  its GP output is stereo, and it would replace the speaker stage rather
  than add to it. Worth it for titles with their own effects image.
- Level: the two paths now measure the same in TS2. The native stream path
  had left out the voice headroom DirectSound folds into a voice's volume
  (TS2's music: -602 asked, -1202 kept), playing music 6 dB over the APU
  path; it reads the kept value now (XDK 4721's layout, checked before it is
  trusted). Whether both are the console's level is for the GP (step 5) or
  a console to say.
- The native path on SDL has no reverb (no shared send to feed one).

Sound is the subsystem where this toolkit keeps paying per title. Each new
title has found something the last one did not use, and each finding has been
fixed where it was found. The latest: TimeSplitters 2 lost every sound effect
when DirectSound envelopes were added for MK Shaolin Monks, because the
envelope index was read the wrong way round (below). This document explains
how sound works on the console, where the toolkit sits on that path today and
why that position produces this pattern, and proposes a design in which a new
title gets correct sound without anyone learning its audio first.

## How sound works on an Xbox

Five layers, top to bottom. Only the first is the title's own code; the second
is a Microsoft library statically linked into every XBE; the rest is hardware.

| Layer | What it is | Who varies it |
| --- | --- | --- |
| 1. The title | Its own mixer or sound manager, middleware (CRI ADX/Sofdec, Miles, FMOD, Bink, RenderWare Audio), XACT in later XDKs | Every title |
| 2. DirectSound | `CDirectSound`, `CDirectSoundBuffer`, `CDirectSoundStream`, `CDirectSoundVoice` (the settings), and under them `CMcpxVoiceClient`, `CMcpxBuffer`, `CMcpxStream`, `CMcpxAPU`, the GP DSP manager | Every XDK build. Under LTCG, functions are inlined or merged and lose their names |
| 3. The APU front end (FE) | The CPU writes *methods* to the MCPX APU at `0xFE800000`: pick a voice, set its parameters, voice on, voice off, set the stream segment list | Fixed: one chip |
| 4. The voice processor (VP) | Up to 256 hardware voices, each a structure in memory: format (PCM 8/16/24/32, Xbox ADPCM), pitch, buffer and loop addresses or a stream's segment list, two envelope generators (amplitude, and multi for pitch and filter), a filter, a volume for each of up to 8 of 32 mix bins, HRTF for 3D voices. It runs 32-sample frames at 48 kHz and writes the 32 mix bins | Fixed |
| 5. The DSPs and the output | The GP (a DSP56300) runs an effects image, almost always Microsoft's standard one (`dsstdfx`): I3DL2 reverb, crosstalk, mixing the bins to speakers. The EP (another DSP56300) formats the output, AC3 for Dolby Digital. Then the AC97 controller and codec | The effects image is the title's choice; most use the stock one |

Every title's sound passes through layers 3 and 4, and almost every title's
passes through layer 2. Middleware sits on DirectSound streams and buffers;
titles that program the APU directly are vanishingly rare.

## Where the toolkit is today

### All audible sound comes from replacing DirectSound by name

`src/hle` replaces DirectSound functions by their XDK names (85
`HLE_EXPORT`s in `hle_dsound*.c`) and plays what it models through a host
voice per DirectSound voice: `audio_output_xaudio2.cpp` on Windows,
`audio_output_sdl.c` elsewhere. A buffer's PCM is decoded, given volume,
pitch, 3D mix, envelopes and filters by toolkit code that reconstructs what the
hardware would have done, and submitted to its host voice.

### The emulated APU exists and plays nothing

`src/apu` is an extract of xemu's MCPX APU: front end, voice processor
(`apu_vp.c`), DSP stubs. It traps the APU's registers and answers the title,
which is why DirectSound starts up. But it never produces sound, on any OS:

- `mcpx_apu_monitor_frame` (`apu_core.c`) clears the output frame and then
  mixes in only a test tone and `src/audio`'s software mixer, so whatever the
  VP computed is thrown away.
- On the native path no hardware voice is ever started: `Play` is replaced, so
  the XDK's `CMcpxBuffer_Play` never runs.
- The GP and EP are stubbed ("passthrough mode").
- `apu_vp.c` is 1,247 lines against xemu's current `vp.c` at 1,881 (ADPCM,
  both envelopes, the filters, HRTF), an extract that has not been refreshed.

So "native versus emulated" is not a choice the toolkit makes today. Every
title is on the native path.

### Why the native path turns into whack-a-mole

The boundary is in the wrong place for a toolkit that wants every title.

1. **Layer 2 differs per XDK, and the code has to know each difference.** The
   buffer settings move 4 bytes between DSOUND 5344 and 5455 (`buf_shift`);
   Halo's 3936 keeps a pointer where the rate should be, so its buffers are not
   modelled at all (`settings_in_layout`); Tony Hawk's 2X (3947) needs its own
   check. Under LTCG a replaced function may not exist as a function at all.
2. **Each replacement has to reproduce semantics nobody wrote down.** The
   hardware's behaviour is reconstructed from what titles were seen to need:
   Pause modes, Max Payne's repeated `SetFrequency`, Dino Crisis 3's volume
   fade, MK Shaolin Monks' filter modes. Twelve titles are named in the
   audio HLE files. Each fix is right for the title that found it and unchecked
   for the others.
3. **Two models of one voice.** Many replacements run the XDK's body and then
   record what it was told (`HLE_ORIGINAL` plus a capture). The XDK keeps its
   own copy of the voice in its settings and in the hardware voice structure; the
   toolkit keeps another. When the two disagree nothing notices.
4. **Two host backends with different semantics.** SDL bakes the mix into the
   PCM at submission and remakes a voice (dropping what is queued) when a mix is
   switched on or off; XAudio2 applies a mix at once and keeps the voice. SDL has
   no reverb. Every feature of layer 4 is implemented twice, differently.
5. **Nothing measures sound across titles.** A title can lose all its sound
   effects and every run still "passes".

### Case study: TimeSplitters 2's sound effects

From a traced run (`RECOMP_DS3D_TRACE=1`) on toolkit `760eba0`, and the code at
`f6eac56` and `760eba0`:

- TS2 gives each effect buffer two envelopes: `dwEG=1` with sustain 255 (full
  volume) and `dwEG=0` with everything 0. In TS2's own `CMcpxVoiceClient_SetEG`
  (XDK 4721), the `dwEG == 0` branch is the only one that reads the pitch and
  filter scales (`desc+0x20`, `desc+0x24`): **0 is the multi envelope and 1 the
  amplitude**.
- `hle_dsound_3d.c` `envelope()` stores `which = dwEG & 1` with 0 as
  amplitude, and `hle_dsound.c`'s pump applies slot 0 as the amplitude. So the
  all-zero multi envelope became the volume, `eg_step` settled at sustain 0, and
  `eg_apply` multiplied every effect sample by 0 before it reached the host.
  Music survived because streams never get `SetEG`.
- It came in with 4730ea2 ("directsound-effects", #60). Before that `SetEG` was
  not replaced, its XDK body programmed a hardware voice that never started, and
  the native path played the buffer at full level. No switch restores it.
- It is not a macOS problem: the bug is above the host backend, so Windows on
  `760eba0` is silent too. A harness built from `audio_output_sdl.c` showed the
  SDL backend drains correctly; the silence is in the PCM handed to it.
- Separately, the effect slots are fed faster than their voices play (the host
  logs `reason=queue-full`), which the harness reproduces only when the feed
  outruns rate x ratio. That is a second fault in the buffer pump's mixed path,
  not yet explained.

On the hardware path this bug cannot happen: the XDK's own `SetEG` writes the
amplitude envelope into the amplitude registers, and the VP runs it.

## The design

**Move the boundary to the hardware.** Run each title's own DirectSound,
unmodified, as lifted code, and emulate layers 3 and 4 (the front end and the
voice processor) once, from xemu. Mix the 32 bins to the speakers on the host.
Treat layer 5 (the DSP effects) as a later, separate piece.

Why this ends the per-title work:

- Layers 3 and 4 are one chip. Its interface does not change with the XDK,
  LTCG, the middleware or the title, so nothing above it needs to be known.
- The semantics come with the code. xemu's VP has been checked against real
  titles for years; envelopes, pitch, ADPCM, loop points, stream segment lists,
  filters and HRTF are all there. A disagreement with xemu is a bug with a
  reference to compare against, not a design question.
- There is one model of each voice: the hardware structure the XDK wrote.
- There is one host path: a single stereo (or 5.1) stream, so the backends
  stop carrying audio semantics. SDL and XAudio2 differ only in how they open a
  device.
- It does not need the DSP56300s. The VP writes the 32 mix bins; turning bins
  into speakers is the stock effects image's job, and that job is small and
  known (below), so the toolkit does it natively. **Not** xemu's "DSP off"
  mode (`MCPX_APU_DEBUG_MON_VP`): that takes each voice's loudest bin volume
  and plays the voice equally in both speakers (`vp.c`, "not how the hardware
  works anyway"), which keeps distance but loses all panning and the rear
  speakers. The design uses the full bin path.

### Mixing, fading and proximity on this path

All of it is computed by the title's own DirectSound and applied by the VP, so
none of it needs reimplementing:

| Feature | Who computes it | Who applies it |
| --- | --- | --- |
| Volume, mixing several sounds | DirectSound writes each voice's volume for each of its (up to 8) bins | VP: `mixbins[bin] += volume x sample`, summed over every voice |
| Fades | `SetVolume` repeated by the title, or an amplitude envelope (`SetEG`) | VP: the new volume, or the envelope generator (`ea_value`) per frame |
| Panning, 2D | The title's mix-bin volumes (`SetMixBinVolumes`) | VP, into the speaker bins |
| Proximity, 3D: distance, rolloff, cones, Doppler | DirectSound's 3D calculator on the CPU, from listener and source positions | VP: per-bin volumes (distance, cone), pitch (Doppler) |
| Direction, 3D | DirectSound picks HRTF coefficients for the source's direction | VP: the HRTF filter on the voice, into the crosstalk bins (`hrtf_filter_process`) |
| Occlusion, obstruction | DirectSound's filter settings (DLS2, I3DL2 source) | VP: the per-voice low-pass filter |
| Room reverb | The title's I3DL2 listener; each voice's send level into the I3DL2 bin | The GP (the effects image), so the speaker stage below |

Today's native path recomputes the 3D column itself ("the XDK's own distance,
cone and Doppler arithmetic" in `hle_dsound_3d.c`). On this path the XDK's
code does it, the same code that ran on the console.

### Every kind of sound a title makes

| Sound | How the title plays it | On this path |
| --- | --- | --- |
| Effects, one-shot and looping | DirectSound buffers, PCM or Xbox ADPCM | The VP decodes and plays them |
| Music | DirectSound streams: packets handed to a hardware voice | The VP plays them; packet completion needs the APU interrupt (step 2) |
| Compressed music (WMA and others) | The title's own decoder on the CPU, feeding a stream | Lifted code, then a stream |
| Middleware (CRI ADX, Miles, FMOD, RenderWare Audio, XACT) | DirectSound buffers and streams underneath | Nothing per middleware |
| A title's own software mix | One stream | A stream |
| Movie audio | Bink and Sofdec feed a DirectSound stream | A stream. **XMV is the exception**: the toolkit replaces the XMV player above DirectSound (FFmpeg), so its audio enters the speaker stage as an extra input |
| Reverb and DSP effects | The GP's effects image | The speaker stage (step 3); a title's own image, step 5 |

Not on the APU at all: headset voice (the Communicator, over USB), and
finding custom soundtracks on the hard disk (the title's playback of one is a
stream as above). Dolby Digital output is not needed; the host takes 5.1 PCM.

### The speaker stage

What the stock effects image does with the bins, done natively on the host,
mostly from code the toolkit already has in `hle_dsound_3d.c`:

- Bins 0-5 (front left, front right, centre, LFE, back left, back right) go to
  the matching speakers, folded to stereo when the device has two.
- Bins 6-9 (crosstalk, where HRTF-filtered 3D voices land) go to the four
  corner speakers.
- Bin 10 (I3DL2) feeds the room reverb with the listener's I3DL2 parameters:
  XAudio2's reverb on Windows as today, and a reverb of our own for SDL (it has
  none today).
- Bins 11 and up (effect sends) feed whatever the title's effects image does
  with them. For the stock image that is a short known list; a title with its
  own image is the case for running the GP itself (step 5).

### Pieces

1. **The VP, current.** Re-extract `vp.c`, `vp.h`, `adpcm.h`, `svf.h` and
   `hrtf.h` from `third_party/xemu` (LGPL-2.1-or-later, compatible with this
   repository's GPL-3) behind the existing `apu_shim.h`, replacing the old
   `apu_vp.c`. Keep it a straight extract so later xemu fixes come across by
   diff.
2. **Output.** `mcpx_apu_monitor_frame` stops clearing the VP's output; the 32
   bins go through the speaker stage (above) into one 5.1 host stream through
   `audio_output.h`. The frame clock comes from the host
   device's pull, as xemu's does, not from guest timers.
3. **Interrupts and notifications.** DirectSound learns that a stream packet is
   done, or a voice has stopped, from the APU's interrupt and its notifier
   memory. Raise the APU interrupt into the title's connected ISR the way
   `src/usb/ohci.c` and `src/kernel/xbox_nic.c` do. This is the piece most
   likely to need care, because stream-driven titles stall without it.
4. **Physical memory.** The VP reads voice data and segment lists by physical
   address. The toolkit keeps low RAM and the contiguous window as separate
   storage, so a physical address alone does not say where the data is. System
   Link hit the same problem and solved it in `xbox_PhysToGuest` (track, per
   page, which storage `MmGetPhysicalAddress` answered for). The VP uses that.
5. **Stop replacing DirectSound by name**, per title or globally, behind a
   switch (`RECOMP_AUDIO=apu|hle`), so both paths can be compared on the same
   build. The lifter replaces names at lift time, so the HLE functions need a
   runtime fall-through to `HLE_ORIGINAL` when the APU path is chosen.
6. **Later: the GP.** Most titles download Microsoft's standard effects image.
   Identify it by hash and either run xemu's DSP56300 (interpreter or JIT,
   both vendored) or implement its handful of effects natively. A title with its
   own image needs the DSP. The EP's AC3 encoding is never needed: the host
   takes PCM.

### What stays native

The native DirectSound path stays as a fallback while the APU path is proven,
and is retired for each title as the APU path is shown to match it. If a
measured need appears (a title whose DirectSound cannot run, or a CPU budget the
VP does not fit), a native fast path can come back for that case, checked
against the APU path rather than against memory of what a title once needed.

### Costs and risks

- **CPU.** 256 voices of 32 samples, 1,500 frames a second, with resampling and
  filters. xemu does this on one host thread on ordinary hardware; it runs off the
  guest lock on arm64, since it is host code reading guest memory. Measure on
  the M-series and on the slowest Windows machine before committing.
- **Timing.** The XDK's DirectSound expects the VP to consume at hardware
  rate. A frame clock driven by the host device keeps that honest; a guest-timer
  clock would drift.
- **Interrupt re-entrancy.** As with the NIC, the APU interrupt must be
  delivered at a point where the interrupted code's state is safe.
- **Fidelity is xemu's.** Where xemu is wrong, this will be too; fixes go
  upstream and come back by re-extraction.

## Proving parity: an audio check for every title

Whatever the boundary, sound needs a test that fails. Proposed, in
`scripts/regress.py` and `compat_matrix.py`:

- Per scripted run, per title: voices (or slots) started, how many produced any
  non-silent sample, drops and their reasons, ran-dry counts, and the output
  peak. Most of this is already logged (`[audio-output] submitted ... nonzero=`,
  `RECOMP_AUDIO_METER`); it needs collecting.
- A title fails when it starts voices that never sound, or when the share of
  silent voices rises against its last recorded run. TS2's regression would have
  failed this the day it landed: dozens of effect voices started, none sounded.
- With both paths built, the same run under `RECOMP_AUDIO=apu` and `=hle` gives
  a per-voice comparison: format, pitch, volume and envelope as the hardware was
  told, against what the native model played.
- `RECOMP_AUDIO_CAPTURE` (today Windows-only) on both backends, so a run's PCM
  can be compared across machines.

## Plan

| Step | What | Size |
| --- | --- | --- |
| 0 | Fix the envelope index in `envelope()` (`dwEG` 1 is amplitude), correct the comments; check MK Shaolin Monks' `CMcpxVoiceClient_SetEG` reads the same way | Hours |
| 0 | The audio check above, run over every title in `games/` | 2-4 days |
| 0 | Find the buffer pump's queue-full fault (feed against rate x ratio on the mixed path); SDL keeping a voice across mix on/off | 1-3 days |
| 1 | Current xemu VP behind `apu_shim.h`; the speaker stage for bins 0-9 into one host stream; `RECOMP_AUDIO=apu` with DirectSound falling through to the XDK | 1-2 weeks |
| 2 | APU interrupts and notifier memory; physical addresses through `xbox_PhysToGuest`; TS2 (buffers and streams) and one middleware title (CRI) audible on the APU path | 1-3 weeks |
| 3 | Reverb in the speaker stage (bin 10), on both backends | 1 week |
| 4 | Every title in `games/` on the APU path, compared with the native path; the native path retired title by title | Ongoing, per title, small |
| 5 | The GP for titles with their own effects image: identify images by hash, run xemu's DSP56300 for the rest | 2-4 weeks, as titles need it |

Confidence: high for step 0, medium for steps 1-2 (the VP is proven code; the
interrupt and physical-memory pieces are this runtime's own and have surprised
before), low for the size of step 5.

## Open questions

- How many titles in `games/` ship their own effects image instead of the stock
  one? (`XAudioDownloadEffectsImage` and the image's hash, per title.)
- Does any title bypass DirectSound and program the APU itself? On the APU path
  it would simply work; on the native path it never could.
- XACT titles (later XDKs) sit on DirectSound and should need nothing extra on
  the APU path; worth confirming with one.
