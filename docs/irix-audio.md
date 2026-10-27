# IRIX audio under QuickTransit

QuickTransit has no IRIX audio. SGI's `libaudio` (the Audio Library, AL)
talks to the `hdsp` kernel driver, which QuickTransit doesn't emulate:
`audiostate` says "This machine has no audio driver at all", and a program
that opens a port can take QuickTransit down. For example, SDL's Irix audio
driver dies with SIGBUS in its audio thread.

`install-extras.sh` replaces `libaudio` with one that plays through ALSA.
The IRIX program becomes an ordinary ALSA client. Its sound goes wherever
ALSA's `default` device goes, with no daemon or per-program setup.

```
IRIX program ─► /usr/lib32/libaudio.so.1  (kit, IRIX n32)
                    │  QuickTransit native call ("SCall")
                    ▼
                /usr/QuickTransit/lib/ia64/libRCAudio.so  (native, in the
                    │                                     QuickTransit process)
                    ▼
                ALSA "default"  (or $QTAUDIO_DEVICE)
```

## Pieces

| File | What |
|---|---|
| `extras/src/irixaudio/libaudio.c` | IRIX side: the AL API (configs, ports, parameters, errors, AL1 compatibility calls). Exports the same 97 public functions as SGI's lib32 `libaudio.so.1`. |
| `extras/src/irixaudio/rcstubs.S` | the native-call marker blocks (see [qt-internals.md](qt-internals.md#native-library-binding-scall)) |
| `extras/src/irixaudio/rcaudio.c` | native side: one ALSA PCM per IRIX port |

The IRIX side is prebuilt (`extras/target/usr/QuickTransit/lib/irix/lib32/libaudio.so`),
because building it needs the SGUG gcc. `install-extras.sh` copies it over
`/irix/usr/lib32/libaudio.so.1` and keeps SGI's library as
`libaudio.so.1.sgi`. It builds `libRCAudio.so` with the host gcc (needs
alsa-lib's headers; T2's `alsa-lib` has them).

Rebuilding the IRIX side (on the ia64 host, see [irix-gcc.md](irix-gcc.md)):

    /irix/bin/sh -c 'PATH=/usr/sgug/bin:/usr/bin:/bin; export PATH; cd /root/irixaudio &&
        gcc -O2 -Wall -shared -fPIC -static-libgcc -Wl,-soname,libaudio.so \
            -o libaudio.so libaudio.c rcstubs.S'

## Using it

Programs need nothing special. SDL programs pick SDL's Irix driver by
themselves, so leave `SDL_AUDIODRIVER` unset.

- **Device:** ALSA's `default`. To use another device everywhere, set it
  in `/etc/asound.conf`. For one run, set `QTAUDIO_DEVICE` (for example
  `plughw:Wireless`), on the Linux side or in an IRIX shell; QuickTransit
  passes the environment through. Use a `plug` device, or `default`: the
  samples are big-endian and often at a rate the hardware doesn't run.
- **Several programs at once** (IRIX or Linux) need a device that mixes.
  A `hw` device, or a `plug` straight onto one, plays one stream at a time:
  the second program's `alOpenPort` fails with "cannot access device", and
  the first keeps playing. ALSA's own software mixer, dmix, is enough; no
  sound server is needed. For example, in `/etc/asound.conf`:

      pcm.!default {
          type plug
          slave.pcm {
              type dmix
              ipc_key 5678
              ipc_perm 0666
              slave { pcm "hw:Wireless"; period_size 1024; buffer_size 8192 }
          }
      }
      ctl.!default { type hw; card Wireless }

  Set `period_size`: dmix's default period is 125 ms, and no client gets
  a buffer shorter than two periods. SDL asks for about 46 ms, would get
  250 ms, and its sound would trail the picture by that much. With the
  settings above, SDL gets 43 ms and programs with long queues get up to
  170 ms. PipeWire or PulseAudio, through their ALSA plugins, mix as well.
- **`QTAUDIO_DEBUG=1`** prints each port's ALSA setup (rate, channels,
  format, buffer and period sizes) and every underrun.

## What is emulated

- **Output:** AL2 (`alOpenPort`, `alWriteFrames`, `alWriteBuffers`,
  `alZeroFrames`, `alGetFillable`/`alGetFilled`, fill points (stored only; see `alGetFD`),
  `alGetFrameNumber`/`alGetFrameTime`, configs with queue size, channels
  1–8, width 8/16/24-bit, float and double with `floatmax`) and the same
  through AL1 (`ALopenport`, `ALwritesamps`, ... with counts in samples).
  `alGetFrameTime` uses UST from `syssgi(SGI_GET_UST)`.
- **Rate:** `AL_RATE` on any resource (`alSetParams`) and AL1
  `AL_OUTPUT_RATE` set the output rate for the process. Open ports switch
  at their next write: what is queued plays out, then the port reopens.
- **Parameters:** reading `AL_RATE`, `AL_GAIN` (0 dB), `AL_MUTE`,
  `AL_NAME`/`AL_LABEL`, `AL_TYPE` and the port counts works, and so does
  `alGetParamInfo` for rate, gain and mute. Setting gain, mute and the
  master clock is accepted and ignored: use the Linux mixer. The AL1 calls
  `ALgetparams`, `ALgetminmax`, `ALgetdefault`, `ALqueryparams` and
  `ALgetname` answer for a stereo device.
- **Resources:** `alGetResourceByName` maps names and types that look like
  input to `AL_DEFAULT_INPUT` and everything else to `AL_DEFAULT_OUTPUT`.

Not emulated:
- **Input.** `alOpenPort(..., "r", ...)` fails with `AL_BAD_DEVICE_ACCESS`.
- **Event queues** open, but no events ever arrive (`alNextEvent` blocks).
- **`alConnect`, `alQueryValues`** fail with "not supported under
  QuickTransit".
- **`alGetFD`** returns a descriptor that is always ready. A program that
  selects on it before writing still works, because the write blocks
  instead.
- **o32 and n64 programs.** Only `/usr/lib32` (n32) is replaced.
  `/usr/lib/libaudio.so.1` and `/usr/lib64/libaudio.so.1` are still SGI's.

## How it fits ALSA

- Each port is one PCM (`snd_pcm_open` on the device, interleaved
  read/write access). Integer samples go to ALSA as `S8`, `S16_BE` and
  `S24_BE` (24-bit in 32, as AL stores them), read in place from IRIX
  memory. Float and double are scaled by `1/floatmax`, clipped and
  converted to host `S32`. Not float: a `plug` in front of dmix finds no
  configuration for float at 44.1 kHz (aplay fails the same way), but
  takes `S32` at any rate.
- The ALSA buffer follows the port's queue size (80 ms to 1 s; under 80
  ms, a dmix buffer of two ~20 ms periods underran constantly). Filled and
  fillable come from ALSA's queue, so a program that paces itself on
  `alGetFillable` keeps about one queue's worth buffered.
- **Whole periods only.** ALSA's resampler (in `plug`) converts a period at
  a time. The last partial period of a sound sits in the plug, counted as
  played, until an underrun throws it away. Without a fix, every sound that
  doesn't end on a period boundary lost its tail: 0.15 s of a 1 s tone in
  testing. `libRCAudio` holds a partial period itself until more data
  completes it, and sends it padded with silence once the program stops
  writing: on a write into an empty device (a one-shot sound), or, when
  the program polls `alGetFilled`, once ALSA has run dry or is down to its
  last period with no write for a period's time. Programs that close a
  port after waiting for `alGetFilled() == 0` hear everything.
- Held frames don't count as filled. SDL keeps its queue between half and
  completely full (46 ms for secondreality). With dmix's 940-frame periods,
  counting the held part left ALSA nearly empty: 64 underruns in 40 s.
  Padding mid-stream, as an earlier version did whenever ALSA was down to
  a period, put gaps in the sound as well.
- `alClosePort` discards whatever is still queued. Programs that want their
  sound to finish wait for `alGetFilled()` to reach 0 first. If only a held
  partial period is left, it is played before the port closes.

## Testing

`tools/irix-probes/altone.c` (in the project directory, outside the kit)
plays a 1 s 440 Hz tone in each format: AL2 16-bit stereo 44.1k, 8-bit
mono 22.05k, 24-bit mono 48k, float stereo with floatmax 2, double mono
32k, then AL1 `ALwritesamps`. `altone N` plays test N alone.

Output was checked by capturing it with an ALSA `file` PCM, through a
`.asoundrc` in a scratch `HOME`, and measuring it:

    pcm.qtatest {
        type plug
        slave {
            pcm { type file; slave.pcm "default"; file "/root/out.raw"; format "raw" }
            format S16_LE
            rate 48000
            channels 2
        }
    }

    HOME=/root/scratch QTAUDIO_DEVICE=qtatest /irix/bin/sh -c './altone 0'

Results on pople, 2026-10-27, with ALSA `default` = `plug` → HDMI:

| Test | Result |
|---|---|
| `altone`, every format | 1.000 s, 439.5 Hz, right level (floatmax applied), both channels; no underruns |
| `sfplay`, `playaiff` with `/usr/share/data/sounds/demos/Slogan.aiff` | 48 kHz mono; captured audio starts and ends where the file's does (0.004–1.013 s), same peak |
| SDL2 `secondreality` (sgug), Xvfb, 40 s | music through SDL's Irix driver with no `SDL_AUDIODRIVER`; no crash. SDL asks for a small queue (46 ms ALSA buffer); with Xvfb's software GL loading both CPUs it underran twice in 38 s. |
| two programs on the exclusive HDMI `hw` device | second `alOpenPort` fails cleanly, first keeps playing |
| two `altone`s and an `aplay` at once through `plug` → dmix on HDMI | all three play; every format opens (float, after the S32 change) |
| `aplay` and two `sfplay`s at once, `default` = the dmix example above on a USB headset | all three play; the IRIX ports get 940-frame periods |
| secondreality on the real X server, same dmix `default`, 40 s | no underruns (ALSA buffer 3763 frames, 85 ms) |

## Background: the old workaround

Before this, SDL programs could play by running SDL's disk driver
(`SDL_AUDIODRIVER=disk`, `SDL_DISKAUDIOFILE=<fifo>`) into a FIFO, with a
host reader piping the FIFO into `aplay` at a hard-coded format. That
worked only for SDL, needed a daemon, and needed the format known in
advance. It isn't needed any more.
