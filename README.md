# vkrender-airplay

A standalone **AirPlay screen-mirroring receiver for Windows** that feeds the
mirror into any [framelink](https://github.com/jsuppe/framelink) consumer -
normally a [vkrender](https://github.com/jsuppe/vkrender) scene - as one more
producer stream. Apple devices (macOS, iPhone, iPad) see the machine as
"vkrender" in Screen Mirroring; the cast lands on the renderer's Vulkan scene
zero-copy: the frame is hardware-decoded on the GPU and colour-converted
straight into the shared D3D11 texture the renderer samples. No pixel is ever
copied through the CPU.

```
Apple sender ──AirPlay/RAOP──► vkr_airplay.exe ────────────► vkrenderer.exe --serve
              (mDNS, pairing,   H.264 AUs -> d3d11h264dec     (framelink input slots:
               FairPlay, RTP)   on the ring's D3D11 device,    shared-texture rings +
                                GPU NV12->BGRA into the ring)  shared fences)
```

The receiver and the renderer are **fully independent processes**. The only
contract between them is framelink's producer API: take a free input slot
(`flOpenProducerSlot`), acquire a ring texture, draw, `flSubmit`. Kill either
process and the other keeps running (the renderer frees the stream; the
receiver redials).

## Features

- Native Windows: discovery via `DnsServiceRegister` (**no Bonjour install**),
  MSVC build, Win32 pthread shim for the vendored core.
- Full mirror session: pairing, FairPlay, RTP mirror stream, NTP-based
  timestamps.
- **Zero-copy video path**: `d3d11h264dec` is created on the framelink ring's
  own D3D11 device (handed over as a `GstContext`), so decoder output and ring
  textures share one device; a `GstD3D11Converter` renders NV12 → BGRA directly
  into the acquired ring texture, and framelink's ready-fence signal is queued
  behind that draw on the same immediate context. Latest-wins throughout
  (`appsink max-buffers=1 drop=true`, framelink's full-ring drop), so a slow
  consumer never adds queueing latency.
- Multi-presenter: every sender gets its own decoder and its own slot (tile).
- Stream lifecycle: a slot is taken when the mirror starts and sized to the
  mirror's true resolution on the first decoded frame; stopping the mirror
  releases the slot (the scene face frees up rather than freezing on the last
  frame). A decoder error also releases the slot instead of freezing.
- CPU fallback (`avdec_h264` + one `UpdateSubresource` per frame) for machines
  without a D3D11 video device. It is never chosen silently: the startup log
  prints which path is in use, and `VKR_AIRPLAY_DECODE=cpu` forces it.
- Crash triage built in: faults print module+offset before exit
  (`VKR_AIRPLAY_DEBUG=1` for protocol-level logs).

## PIN authorization (optional)

Set `VKR_PIN` before starting the receiver:

- `VKR_PIN=auto` — the RAOP core mints a fresh 4-digit code per pairing
- `VKR_PIN=1234` — a fixed code
- unset / `off` — no PIN (open receiver)

The receiver then advertises `pw=true`, so Apple devices run the AirPlay
`pair-pin` exchange: the code appears as a topmost overlay on this machine's
screen and the user types it into the sender's password prompt. The overlay
disappears once mirror frames start flowing. The same overlay is used by the
Miracast and WebRTC receivers in the main vkrender project, so all three
modalities look identical on screen.

## Build

Prereqs: Visual Studio (MSVC), CMake ≥ 3.20, Git, and the GStreamer **MSVC
x86_64** runtime + development MSIs, 1.22 or newer (1.26.x is what CI uses).
The development package provides OpenSSL and the `gstd3d11-1.0` library the
GPU path needs.

```bash
cmake -S . -B build -A x64
cmake --build build --config Release
```

framelink is fetched and built from source during configure, at the revision
pinned by `FRAMELINK_GIT_TAG` in `CMakeLists.txt` (library only - no tests,
samples or GStreamer plugin). To build against a local checkout instead, as
the vkrender repo does, pass `-DFRAMELINK_DIR=path/to/framelink`. Keep the pin
on the same framelink line as the renderer you feed: framelink refuses a peer
with a different `FL_VERSION` at handshake.

## Run

1. Start a vkrender renderer in service mode: `vkrenderer --serve vkrender`
2. `build/Release/vkr_airplay.exe [pipeName] [advertisedName]`
   (defaults: `vkrender` / `vkrender`; GStreamer's `bin` must be on `PATH`;
   `framelink.dll` is copied next to the exe by the build)
3. Pick **vkrender** in Screen Mirroring on the Apple device.

To check the frame path on a machine without reaching for an Apple device,
run the built-in self-test against a running renderer (headless is fine:
`vkrenderer --serve vkrender --headless -1`). It encodes 60 synthetic frames,
feeds the access units through the exact code the RAOP core uses, and reports
how many reached the ring and on which path:

```
vkr_airplay.exe --selftest            # 60 frames, pipe "vkrender"
vkr_airplay.exe --selftest 120 mypipe
```

Current result on an RTX 5090 with GStreamer 1.28: the GPU path reports FAIL,
because 56 of 60 frames reached the ring and the test allows a shortfall of 2.
The cause is not yet known. No drops were counted at the ring, and end-of-stream
flushed only one more frame out of the decoder.

```
selftest: FAIL - 60 AUs pushed, 56 decoded live (+1 only flushed by EOS), 0 ring-full drops, 56 frames delivered to the ring (1280x720 BGRA, generation 4) via the GPU zero-copy path
```

The log says which decode path each presenter got:

```
airplay: decode path: GPU (d3d11h264dec -> BGRA ring, zero-copy)
airplay: 'Jon's iPad' producing on 'vkrender.in.0'
airplay: 'Jon's iPad' decoding on the GPU (d3d11h264dec -> BGRA ring, zero-copy)
airplay: 'Jon's iPad' mirror is 752x1080, ring is 752x1080
```

## License

**GPL-3.0** (see `LICENSE`) — this repository vendors the
[UxPlay](https://github.com/FDH2/UxPlay) RAOP/mirroring core. See `NOTICE.md`
for full lineage and per-component licenses. framelink is MIT-licensed, built
from its own repository at configure time, and is not covered by the GPL.
