# vlc-u64stream

> **Note on origin:** this project was developed with AI assistance (Anthropic
> Claude, via the Claude Code CLI). All design decisions, protocol choices,
> licensing, and final code review are by the human author; the AI did the
> bulk of the typing. Please treat it as you would any other contributor's
> code: read it, test it, file issues if it bites you.

VLC plugin that decodes the **Ultimate 64 / Commodore 64 Ultimate** video and
audio stream so you can watch your C64 picture and hear it natively in VLC,
with no external viewer.

The Ultimate-64 (FPGA-based C64 reimplementation, gideoncrafts) can broadcast
its VIC video output as raw UDP datagrams. This plugin parses those datagrams
in-kernel… well, in-VLC, and feeds RGBA frames straight into the standard VLC
video pipeline.

Status: **video only** (50 Hz PAL or 60 Hz NTSC). Audio (port 11001) is on the
roadmap.

## Usage

After installing the plugin (see below) and pointing the U64's video stream at
your machine on UDP port 11000:

```
vlc u64://@:11000
```

Or, equivalently, drop a one-line `.m3u`:

```
u64://@:11000
```

URL forms accepted:

| URL                                | Effect                                       |
|------------------------------------|----------------------------------------------|
| `u64://`                           | bind \*:11000 unicast                        |
| `u64://@:11000`                    | bind \*:11000 unicast                        |
| `u64://@192.168.1.50:11000`        | bind a specific local interface              |
| `u64://:11500`                     | bind \*:11500                                |
| `u64://239.0.1.64@:11000`          | join multicast 239.0.1.64 on \* (audio auto: 239.0.1.65) |
| `u64://239.0.1.64@192.168.1.50:11000` | multicast on a specific local interface  |

The U64's default streaming setup uses a multicast pair, e.g. `239.0.1.64`
for video and `239.0.1.65` for audio. The plugin auto-derives the audio
multicast group from the video group by incrementing the last IPv4 octet,
so you don't normally need `--u64stream-audio-group`.

### Options

```
--u64stream-port=N            Override video port from the URL (default 11000)
--u64stream-audio-port=N      Audio UDP port (default 0 = video-port + 1)
--u64stream-audio-group=IP    Audio multicast group (default empty:
                              auto-derive from video group, last octet +1)
--u64stream-no-audio          Disable audio
--u64stream-mode=N            -1 = auto-detect (default), 0 = PAL, 1 = NTSC
--u64stream-source=IP         Only accept packets from this source IP
                              (useful when several U64s share a LAN)
--u64stream-sar-num=N         Pixel aspect ratio numerator (0 = mode default)
--u64stream-sar-den=N         Pixel aspect ratio denominator (0 = mode default)
```

Aspect-ratio defaults match VICE's authentic VIC-II values:
- PAL: 117:125 (≈0.936)
- NTSC: 3:4 (0.75)

Pass `--u64stream-sar-num=1 --u64stream-sar-den=1` for raw square pixels
(the picture will look slightly wide-ish).

## Building

Requires GCC/Clang, Meson, Ninja, and the VLC plugin SDK headers.

### Ubuntu / Debian

```sh
sudo apt install meson ninja-build build-essential libvlccore-dev libvlc-dev
meson setup build
meson compile -C build
```

### Install

**Per-user (no root required):**

```sh
meson compile -C build install-user
```

This installs to `~/.local/share/vlc/plugins/`. Note: the Ubuntu/Debian
build of VLC does **not** auto-scan that directory, so you have to point VLC
at it:

```sh
VLC_PLUGIN_PATH=~/.local/share/vlc/plugins vlc u64://@:11000
```

For convenience you may want a shell alias:

```sh
alias vlc-u64='VLC_PLUGIN_PATH=$HOME/.local/share/vlc/plugins vlc'
```

**System-wide (auto-detected, no env var):**

```sh
sudo meson install -C build
```

By default that installs to `$libdir/vlc/plugins/access/`. Override with
`-Dvlc_plugin_dir=…` at `meson setup`.

### Smoke test

```sh
vlc -p u64stream
```

Should print the module description and its options.

## How it works

Each UDP datagram on the U64 video port carries:

```
   off  size  field
    0    2   sequence number       (u16 LE)
    2    2   frame number          (u16 LE)
    4    2   line number           (u16 LE; bit15 = last packet of frame)
    6    2   pixels per line       (u16 LE; constant 384)
    8    1   lines per packet      (u8;     constant 4)
    9    1   bits per pixel        (u8;     constant 4)
   10    2   encoding              (u16;    constant 0)
   12  768   pixel payload (4 lines x 384 px x 4 bits, nibble-packed:
             low nibble = leftmost pixel)
```

A frame is 68 packets (PAL, 272 lines) or 60 packets (NTSC, 240 lines). The
plugin:

1. Opens a UDP socket itself (single combined `access_demux` module).
2. For each datagram, blits 4 lines into a 384×height RGBA framebuffer at the
   header's `line` offset (using the line number rather than packet ordering
   so mild reordering doesn't corrupt the picture).
3. On the bit-15 (last-of-frame) packet, emits the framebuffer as a `block_t`
   with a paced PTS, and updates the input clock via `ES_OUT_SET_PCR`.

Reference: [Ultimate-64 data streams documentation](https://1541u-documentation.readthedocs.io/en/latest/data_streams.html).

## License

Apache-2.0. See [LICENSE](LICENSE).

## Acknowledgements

- **Gideon's Logic** for the Ultimate 64 and the publicly documented protocol.
- **DusteD's [u64view](https://github.com/DusteDdk/u64view)** (WTFPL) — clear
  reference C implementation that confirmed packet layout and audio framing.
