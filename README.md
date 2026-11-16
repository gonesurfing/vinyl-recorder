# vinyl_recorder

A terminal audio recorder built for digitizing vinyl, but useful for any
line-level capture. Records 16-bit stereo PCM from an ALSA (Linux) or
CoreAudio (macOS) input device, shows live stereo level meters in an
ncurses UI, and encodes finished takes to FLAC in the background.

## What it does

- **Capture** from any input the OS exposes: USB phono pre, line-in,
  built-in mic, etc. List devices with `-L`.
- **Live metering**: per-channel peak + RMS bars in dBFS, with peak-hold
  ticks. The scale is non-linear (more pixels per dB near 0) so you can
  set levels precisely.
- **3-second pre-roll**: a rolling lookback buffer is always running, so
  the first revolution of the record isn't clipped off when you start
  capture late.
- **Split mid-recording** (`N`) to close the current file and immediately
  start the next one — handy for one-pass per-side captures.
- **FLAC encode** runs as a child `flac` process after each take, so the
  recorder thread never blocks on compression. Files land in
  `~/recordings/` by default.

### Controls

| Key            | Action                                              |
|----------------|-----------------------------------------------------|
| `R` / `Space`  | Start / stop recording                              |
| `N`            | Split: close current file, start a new one         |
| `Q`            | Quit                                                |

### Command-line flags

```
-d, --device <name>   capture device (default: "default")
-r, --rate <hz>       sample rate (default: 44100)
-o, --output <dir>    output directory (default: $HOME/recordings)
-L, --list-devices    list available capture devices and exit
-P, --probe           capture for 3s without UI; print levels (diagnostic)
-v, --version         print version and exit
-h, --help            show help
```

## Build

### macOS

CoreAudio and the system ncurses are already on the box. You only need
`pkg-config`, `libsndfile`, and the `flac` encoder:

```sh
brew install pkg-config libsndfile flac
make
```

### Linux (Debian / Ubuntu / Raspberry Pi OS)

```sh
sudo apt install build-essential pkg-config \
                 libasound2-dev libncursesw5-dev libsndfile1-dev \
                 flac
make
```

`libatomic` ships with gcc on Debian-family distros and is linked
automatically; it's needed for `_Atomic uint64_t` on 32-bit ARM
(Raspberry Pi OS armhf) and harmless elsewhere.

### Linux (Fedora / RHEL)

```sh
sudo dnf install gcc make pkgconf-pkg-config \
                 alsa-lib-devel ncurses-devel libsndfile-devel \
                 flac
make
```

### Tests

```sh
make test
```

The test suite only depends on `libsndfile` (for a WAV round-trip
check), so it builds on any host even without ALSA installed.

## Real-time priority (Linux / Raspberry Pi)

The capture thread asks for `SCHED_FIFO` scheduling so that other work on
the box (an ssh or tmux reattach redrawing the screen, `flac` encoding,
SD-card writeback) can't starve it into an overrun. The ALSA buffer is
also ~500 ms deep, giving extra headroom either way. Check
`~/recordings/vinyl_recorder.log` at startup to see whether you got it:

```
ALSA: capture thread at SCHED_FIFO priority 60
```

If instead it says `could not get real-time priority`, grant it one of
two ways:

**Option A, rtprio limit (persists across rebuilds).** Put your user in
the `audio` group and allow that group real-time priority:

```sh
sudo usermod -aG audio "$USER"
echo '@audio - rtprio 95' | sudo tee /etc/security/limits.d/99-audio-rt.conf
```

Log out and back in, and confirm with `ulimit -r` (it should print `95`).
A tmux server that was already running keeps the old limits. Run
`tmux kill-server` and start a new one.

**Option B, file capability (must be redone after every `make`):**

```sh
sudo setcap cap_sys_nice+ep ./vinyl_recorder
```

Every xrun is also written to the log with its cause. *ALSA capture
overrun* means the capture thread was starved. *Ring full* means the
recorder thread stalled writing to disk for over ~2 s.

On Raspberry Pi OS Bookworm the `default` device goes through PipeWire,
which has its own buffering. To capture straight from the hardware,
pass the card name, for example `-d plughw:CARD=CODEC` (list the names
with `-L`).

## Layout

```
src/
  main.c             argument parsing, thread wiring, signal handling
  audio.h            backend-agnostic capture interface
  audio_alsa.c       ALSA capture backend (Linux)
  audio_coreaudio.c  CoreAudio / AUHAL capture backend (macOS)
  ring.{c,h}         lock-free SPSC ring buffer (capture → recorder)
  level.{c,h}        peak / RMS / dBFS conversion for the meters
  recorder.{c,h}     lookback buffer, WAV writer, FLAC spawn/poll
  ui.{c,h}           ncurses render loop + key handling
  common.h           shared types, app_state_t (atomic flags & meters)
tests/               unit tests for ring, level, recorder
```

Three threads, no locks on the audio path: the capture backend writes
int16 frames into the ring, the recorder thread drains them into a WAV
and then hands the closed file to a `flac` subprocess, and the UI
thread reads atomic state to render at 30 Hz.

## License

MIT — see [LICENSE](LICENSE).
