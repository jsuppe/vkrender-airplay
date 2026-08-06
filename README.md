# vkrender-airplay

A standalone **AirPlay screen-mirroring receiver for Windows** that feeds the
mirror into a [vkrender](https://github.com/jsuppe/vkrender) scene as one more
producer stream. Apple devices (macOS, iPhone, iPad) see the machine as
"vkrender" in Screen Mirroring; the cast lands on the renderer's Vulkan scene
zero-copy — the D3D11 texture this receiver writes is the exact allocation the
renderer samples.

```
Apple sender ──AirPlay/RAOP──► vkr_airplay.exe ─────► vkrenderer.exe --serve
              (mDNS, pairing,   H.264 depacketize +    (named pipe + shared
               FairPlay, RTP)   GStreamer decode ->     D3D11 texture ring +
                                BGRA ring upload)       shared fences)
```

The receiver and the renderer are **fully independent processes**. The only
contract between them is the small producer SDK (`sdk/vkrender_client.h`):
dequeue a shared texture, write a frame, queue it back. Kill either process
and the other keeps running (the renderer frees the stream; the receiver
redials).

## Features

- Native Windows: discovery via `DnsServiceRegister` (**no Bonjour install**),
  MSVC build, Win32 pthread shim for the vendored core.
- Full mirror session: pairing, FairPlay, RTP mirror stream, H.264 decode
  (GStreamer `avdec_h264`), NTP-based timestamps.
- Stream lifecycle: connects to the renderer lazily at the mirror's true
  resolution; stopping the mirror releases the stream (the scene face frees
  up rather than freezing on the last frame).
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

Prereqs: Visual Studio (MSVC), CMake, and the GStreamer **MSVC x86_64**
runtime + development MSIs (which also provide OpenSSL).

```bash
cmake -S . -B build -A x64
cmake --build build --config Release
```

## Run

1. Start a vkrender renderer in service mode: `vkrenderer --serve vkrender`
2. `build/Release/vkr_airplay.exe [pipeName] [advertisedName]`
   (defaults: `vkrender` / `vkrender`; GStreamer's `bin` must be on `PATH`)
3. Pick **vkrender** in Screen Mirroring on the Apple device.

## The vkrender producer SDK

`sdk/` holds a snapshot of the producer interface (header + import lib + DLL)
so this repo builds standalone. The canonical copy is the **client-sdk release
asset** on the vkrender repository; refresh with:

```bash
gh release download client-sdk --repo jsuppe/vkrender --dir sdk --clobber
```

API contract (see `sdk/vkrender_client.h` for details): one stream per
connection; `vkrConnect(pipe, device|NULL, w, h)` → ring of shared BGRA
textures; `vkrDequeueBuffer`/`vkrQueueBuffer` with fence-based hand-off; all
calls for one client from one thread; disconnect frees the stream.

## License

**GPL-3.0** (see `LICENSE`) — this repository vendors the
[UxPlay](https://github.com/FDH2/UxPlay) RAOP/mirroring core. See `NOTICE.md`
for full lineage and per-component licenses. The vkrender SDK under `sdk/` is
provided under the vkrender project's own (permissive) terms and is not
covered by the GPL.
