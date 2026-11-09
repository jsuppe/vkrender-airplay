// M19: AirPlay receiver -> vkrender producer (Windows native).
//
// Ports the same vendored UxPlay RAOP + mirroring core as the Android APK
// (third_party/uxplay/lib) to MSVC, and turns the mirror into a framelink
// producer stream: the raop video_process callback hands over decrypted H.264
// access units, a small GStreamer pipeline decodes them on the GPU
// (d3d11h264dec, on the ring's own D3D11 device), and a GPU colour pass
// renders each frame straight into a texture of a framelink input slot. No
// pixel touches the CPU on the way to the renderer. Discovery uses the native
// Win32 DnsServiceRegister (dnssd_win.c) - no Bonjour needed.
//
// M38: framelink, not vkrender_client. This repo now depends on a published
// library (github: framelink, MIT) rather than on another application's client
// DLL, so the receiver feeds anything that creates framelink input slots.
//
//   vkr_airplay [pipe] [name]
//   vkr_airplay --selftest [frames] [pipe]   (synthetic mirror, see runSelfTest)
//
// Requirements: the vkrenderer service (--serve) must be running; a
// Miracast/AirPlay-capable network (multicast reachable). Video only.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#ifndef GST_USE_UNSTABLE_API
#define GST_USE_UNSTABLE_API // gst/d3d11 is unstable API; CMake defines this too
#endif
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/d3d11/gstd3d11.h>
#include <gst/gst.h>
#include <gst/video/video.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <thread>

extern "C" {
#include "dnssd.h"
#include "logger.h"
#include "raop.h"
#include "stream.h"
}

#include "framelink.h"
#include "framelink_d3d11.h"
#include "framelink_slots.h"
#include "pin_overlay.h"

using Microsoft::WRL::ComPtr;

namespace {

raop_t* g_raop = nullptr;
dnssd_t* g_dnssd = nullptr;
const char* g_pipe = "vkrender";
// M25: true while the pairing PIN overlay is up (hidden once media flows).
// Deliberately global: there is one screen, so one overlay, however many
// senders are pairing.
std::atomic<bool> g_pinShowing{false};

// One presenter. Everything that used to be a global lives here, because a
// single set of globals is exactly what limited AirPlay to one sender while
// every other vkrender ingress is multi-producer: a second device's H.264
// would have landed in the first one's decoder.
//
// The core hands this back as `cls` to the callbacks of the connection that
// created it (see raop_callbacks_t::session_init), so a frame always reaches
// the pipeline and the scene ring belonging to ITS OWN sender.
struct Session {
    flChannel* channel = nullptr;
    GstElement* pipeline = nullptr;
    GstElement* appsrc = nullptr;
    GstElement* appsink = nullptr;
    GstBus* bus = nullptr;
    // GPU path (see buildGpuPipeline): the ring's D3D11 device wrapped for
    // GStreamer, and the NV12 -> BGRA converter that draws into ring textures.
    GstD3D11Device* gstDevice = nullptr;
    GstD3D11Converter* converter = nullptr;
    GstVideoInfo converterIn{};        // input caps the converter was built for
    uint64_t converterGeneration = 0;  // ring generation it targets
    bool gpu = false;                  // false: CPU decode + upload fallback
    bool geometryRequested = false;    // flRequestGeometry done for this mirror
    // Decode error: the tile is released and further AUs are ignored, but the
    // Session stays alive - the raop core still holds it as `cls`.
    std::atomic<bool> failed{false};
    std::atomic<bool> warned{false};   // one-shot diagnostics on the frame path
    std::atomic<bool> ready{false};
    // Set on a raop thread when the sender goes away; the main pump thread
    // does the actual teardown. Closing the producer connection is what makes
    // the renderer free the stream - without it the LAST FRAME stays
    // composited forever after the sender stops (observed live).
    std::atomic<bool> ended{false};
    // A sender opens SEVERAL connections and only one of them carries the
    // mirror, so most sessions never see a frame. Only the ones that do are
    // presenters: they own a tile, and they alone count against the budget.
    // Counting connections instead meant two devices filled all four slots.
    std::atomic<bool> producing{false};
    std::atomic<uint64_t> received{0}; // H.264 AUs handed over by the core
    uint64_t frames = 0;               // frames actually delivered to the scene
    uint64_t decoded = 0;              // frames pulled from the decoder
    uint64_t ringFull = 0;             // decoded frames dropped: no free ring image
    uint64_t lastRingFull = 0;
    uint64_t lastFrames = 0, lastReceived = 0; // for per-interval rates
    // Arrival pacing. A 5s average cannot tell steady 36 fps from bursts
    // separated by stalls, and the reported stutter is ~1 Hz - invisible in
    // aggregate. Track the gaps themselves.
    std::atomic<uint64_t> lastArrivalUs{0};
    std::atomic<uint64_t> maxGapUs{0};
    std::atomic<uint64_t> longGaps{0}; // arrivals more than 40 ms apart
    std::string label;
};

// Four is the presentation layout's tile budget (1/2/3/4 regions); beyond that
// the scene has nowhere to put a presenter. Refusing at the door is deliberate:
// the old build accepted nothing beyond the first sender and simply never
// answered, so the second device sat on a spinner forever.
constexpr size_t kMaxSessions = 4;

std::mutex g_sessionsMutex;
std::vector<Session*> g_sessions; // owned; reaped on the pump thread

// ---- GStreamer decode: H.264 AUs -> framelink slot -------------------------
//
// Two decode paths, chosen once at startup (VKR_AIRPLAY_DECODE=gpu|cpu; the
// default is gpu whenever d3d11h264dec is installed):
//
//   gpu  appsrc -> h264parse -> d3d11h264dec -> appsink (D3D11Memory, NV12)
//        The decoder is created on the RING'S D3D11 device (handed over as a
//        GstContext before the pipeline starts), so its output textures and
//        the framelink textures live on one device. A GstD3D11Converter then
//        renders NV12 -> BGRA directly into the acquired ring texture, and
//        flSubmit's ready-fence signal is queued behind that draw on the same
//        immediate context. Zero-copy: DXVA output -> one GPU colour pass ->
//        the exact allocation the renderer's Vulkan side samples. The colour
//        pass is not an extra hop - the renderer takes BGRA slots, so somebody
//        has to do NV12 -> BGRA, and doing it here keeps the renderer's input
//        on its GPU-import path rather than its CPU-conversion fallback.
//
//   cpu  appsrc -> h264parse -> avdec_h264 -> videoconvert -> appsink (BGRA)
//        One UpdateSubresource per frame into the ring texture. Kept for
//        machines without a D3D11 video device; never chosen silently.

enum class DecodeMode { Gpu, Cpu };
DecodeMode g_decodeMode = DecodeMode::Gpu;

// Take a slot. width/height 0 accepts whatever the consumer allocated; the
// mirror's true size is requested later (ensureGeometry) once a decoded frame
// says what it is - which is the earliest AirPlay tells us, and an iPad's
// 752x1080 is nothing like a MacBook's 1920x1080.
bool openSlot(Session* s, uint32_t w, uint32_t h) {
    uint32_t slot = 0;
    const std::string prefix = std::string(g_pipe) + ".in";
    const flResult r = flOpenProducerSlot(prefix.c_str(), 12, w, h, FL_FORMAT_BGRA8, 0,
                                          &s->channel, &slot);
    if (r != FL_OK || !s->channel) {
        fprintf(stderr, "airplay: '%s' got no input slot on '%s.in' - %s\n", s->label.c_str(),
                g_pipe, flResultString(r));
        s->channel = nullptr;
        return false;
    }
    s->geometryRequested = w != 0 && h != 0; // flOpenProducerSlot already asked
    printf("airplay: '%s' producing on '%s.in.%u'\n", s->label.c_str(), g_pipe, slot);
    return true;
}

// Ask the consumer to size the ring to the mirror, once per presenter. The
// consumer may decline; the GPU path scales into whatever it kept, the CPU
// path clamps. flQuery afterwards is the truth either way.
void ensureGeometry(Session* s, int w, int h) {
    if (s->geometryRequested || w <= 0 || h <= 0) return;
    s->geometryRequested = true;
    const flResult r = flRequestGeometry(s->channel, (uint32_t)w, (uint32_t)h, FL_FORMAT_BGRA8);
    flChannelInfo info{};
    flQuery(s->channel, &info);
    printf("airplay: '%s' mirror is %dx%d, ring is %ux%u%s\n", s->label.c_str(), w, h,
           info.width, info.height, r == FL_OK ? "" : " (geometry request declined)");
}

void teardownPipeline(Session* s) {
    if (s->pipeline) {
        gst_element_set_state(s->pipeline, GST_STATE_NULL);
        if (s->appsrc) gst_object_unref(s->appsrc);
        if (s->appsink) gst_object_unref(s->appsink);
        if (s->bus) gst_object_unref(s->bus);
        gst_object_unref(s->pipeline);
        s->pipeline = nullptr;
        s->appsrc = s->appsink = nullptr;
        s->bus = nullptr;
    }
    if (s->converter) {
        gst_object_unref(s->converter);
        s->converter = nullptr;
    }
    if (s->gstDevice) {
        gst_object_unref(s->gstDevice);
        s->gstDevice = nullptr;
    }
}

bool launch(Session* s, const char* desc, GstContext* ctx) {
    GError* err = nullptr;
    s->pipeline = gst_parse_launch(desc, &err);
    if (!s->pipeline || err) {
        fprintf(stderr, "airplay: pipeline build failed: %s\n", err ? err->message : "?");
        if (err) g_error_free(err);
        if (s->pipeline) gst_object_unref(s->pipeline);
        s->pipeline = nullptr;
        return false;
    }
    // Must precede the state change: elements pick their device going to READY.
    if (ctx) gst_element_set_context(s->pipeline, ctx);
    s->appsrc = gst_bin_get_by_name(GST_BIN(s->pipeline), "src");
    s->appsink = gst_bin_get_by_name(GST_BIN(s->pipeline), "sink");
    s->bus = gst_element_get_bus(s->pipeline);
    if (gst_element_set_state(s->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "airplay: pipeline refused to start\n");
        teardownPipeline(s);
        return false;
    }
    return true;
}

bool buildGpuPipeline(Session* s) {
    // The slot comes first: the decoder has to be created on the ring's device.
    if (!s->channel && !openSlot(s, 0, 0)) return false;
    ID3D11Device* dev = flChannelD3D11Device(s->channel);
    ComPtr<ID3D11VideoDevice> video;
    if (!dev || FAILED(dev->QueryInterface(IID_PPV_ARGS(&video)))) {
        // The E_NOINTERFACE that used to take d3d11h264dec down mid state
        // change, caught up front; this presenter decodes on the CPU instead.
        fprintf(stderr, "airplay: '%s' ring device has no D3D11 video support - CPU decode\n",
                s->label.c_str());
        return false;
    }
    s->gstDevice = gst_d3d11_device_new_wrapped(dev);
    if (!s->gstDevice) {
        fprintf(stderr, "airplay: '%s' cannot wrap the ring's D3D11 device - CPU decode\n",
                s->label.c_str());
        return false;
    }
    // compliance=flexible: output each picture as soon as it is decoded instead
    // of holding the spec's worst-case reorder depth in the DPB. A mirror has no
    // B-frames, and the stock behaviour kept 5 frames (~170 ms at 30 fps) back
    // until the stream ended - measured on caspar with the self-test.
    // max-buffers=1 drop=true: a mirror is latest-wins; never queue behind us.
    const char* desc =
        "appsrc name=src is-live=true format=time do-timestamp=true "
        "  caps=video/x-h264,stream-format=byte-stream,alignment=au ! "
        "h264parse ! d3d11h264dec compliance=flexible ! "
        "video/x-raw(memory:D3D11Memory),format=NV12 ! "
        "appsink name=sink max-buffers=1 drop=true sync=false enable-last-sample=false";
    GstContext* ctx = gst_d3d11_context_new(s->gstDevice);
    const bool ok = launch(s, desc, ctx);
    gst_context_unref(ctx);
    if (!ok) {
        teardownPipeline(s); // drops the wrapped device too
        return false;
    }
    s->gpu = true;
    return true;
}

bool buildCpuPipeline(Session* s) {
    const char* desc =
        "appsrc name=src is-live=true format=time do-timestamp=true "
        "  caps=video/x-h264,stream-format=byte-stream,alignment=au ! "
        "h264parse ! avdec_h264 ! videoconvert ! "
        "video/x-raw,format=BGRA ! appsink name=sink max-buffers=4 drop=true sync=false";
    if (!launch(s, desc, nullptr)) return false;
    s->gpu = false;
    return true;
}

bool buildPipeline(Session* s) {
    if (g_decodeMode == DecodeMode::Gpu && buildGpuPipeline(s)) {
        printf("airplay: '%s' decoding on the GPU (d3d11h264dec -> BGRA ring, zero-copy)\n",
               s->label.c_str());
    } else if (buildCpuPipeline(s)) {
        printf("airplay: '%s' decoding on the CPU (avdec_h264, one upload per frame)\n",
               s->label.c_str());
    } else {
        return false;
    }
    s->ready = true;
    return true;
}

// Tear one presenter down. Main pump thread only - GStreamer state changes and
// the framelink detach must not race the pull loop.
void destroySession(Session* s) {
    teardownPipeline(s); // decoder textures live on the ring's device: it goes first
    if (s->channel) {
        // Releases the slot for the next presenter; the channel is the
        // renderer's and outlives us.
        flClose(s->channel);
        s->channel = nullptr;
    }
    printf("airplay: session '%s' released after %llu frames\n", s->label.c_str(),
           (unsigned long long)s->frames);
    delete s;
}

void warnOnce(Session* s, const char* what) {
    if (!s->warned.exchange(true))
        fprintf(stderr, "airplay: '%s' %s - frames are being dropped\n", s->label.c_str(), what);
}

// GPU path: decoder NV12 texture -> BGRA ring texture, one draw on the ring's
// device, then submit. Nothing is mapped.
void deliverGpu(Session* s, GstSample* sample) {
    GstBuffer* src = gst_sample_get_buffer(sample);
    GstMemory* mem = src && gst_buffer_n_memory(src) ? gst_buffer_peek_memory(src, 0) : nullptr;
    GstVideoInfo in;
    if (!mem || !gst_is_d3d11_memory(mem) ||
        !gst_video_info_from_caps(&in, gst_sample_get_caps(sample))) {
        warnOnce(s, "decoder output is not D3D11 memory");
        return;
    }
    ensureGeometry(s, in.width, in.height);
    flChannelInfo info{};
    if (flQuery(s->channel, &info) != FL_OK) return;
    flBuffer buf{};
    // Ring full means the consumer is behind: latest-wins, drop this one.
    if (flAcquireBuffer(s->channel, &buf, 5) != FL_OK) {
        ++s->ringFull;
        return;
    }

    // (Re)build the converter when the stream or the ring changed shape.
    if (!s->converter || info.generation != s->converterGeneration ||
        !gst_video_info_is_equal(&in, &s->converterIn)) {
        if (s->converter) gst_object_unref(s->converter);
        GstVideoInfo out;
        gst_video_info_set_format(&out, GST_VIDEO_FORMAT_BGRA, info.width, info.height);
        out.colorimetry.range = GST_VIDEO_COLOR_RANGE_0_255;
        s->converter = gst_d3d11_converter_new(s->gstDevice, &in, &out, nullptr);
        s->converterIn = in;
        s->converterGeneration = info.generation;
        if (!s->converter) {
            flCancelBuffer(s->channel, &buf);
            warnOnce(s, "cannot create the NV12 -> BGRA GPU converter");
            return;
        }
    }

    // Borrow the ring texture as GstD3D11Memory on the SAME GstD3D11Device, so
    // the converter's render target is the shared allocation itself. The
    // wrapper does not own the texture; framelink does.
    GstMemory* dstMem = gst_d3d11_allocator_alloc_wrapped(nullptr, s->gstDevice,
                                                          flImageD3D11(buf.image), 0, nullptr,
                                                          nullptr);
    guint pitch = 0;
    if (!dstMem || !gst_d3d11_memory_get_resource_stride(GST_D3D11_MEMORY_CAST(dstMem), &pitch)) {
        if (dstMem) gst_memory_unref(dstMem);
        flCancelBuffer(s->channel, &buf);
        warnOnce(s, "cannot wrap the ring texture for GStreamer");
        return;
    }
    // GPU side is the truth; a CPU map (nobody does one) would have to download.
    GST_MINI_OBJECT_FLAG_SET(dstMem, GST_D3D11_MEMORY_TRANSFER_NEED_DOWNLOAD);
    GstBuffer* dst = gst_buffer_new();
    gst_buffer_append_memory(dst, dstMem);
    gsize offsets[1] = {0};
    gint strides[1] = {(gint)pitch};
    gst_buffer_add_video_meta_full(dst, GST_VIDEO_FRAME_FLAG_NONE, GST_VIDEO_FORMAT_BGRA,
                                   info.width, info.height, 1, offsets, strides);
    const gboolean drawn = gst_d3d11_converter_convert_buffer(s->converter, src, dst);
    gst_buffer_unref(dst);
    if (!drawn) {
        flCancelBuffer(s->channel, &buf);
        warnOnce(s, "GPU colour conversion failed");
        return;
    }
    // Same immediate context as the draw above: framelink's ready-fence signal
    // is ordered after it, and the decoder's reuse of its output texture is
    // queued behind our read on that context as well - so no completion wait.
    gst_d3d11_device_lock(s->gstDevice);
    const flResult r = flSubmit(s->channel, &buf, -1);
    gst_d3d11_device_unlock(s->gstDevice);
    if (r == FL_OK) ++s->frames;
    else warnOnce(s, flResultString(r));
}

// CPU fallback: system-memory BGRA, UpdateSubresource'd into the ring texture
// on the ring's device (one upload per frame).
void deliverCpu(Session* s, GstSample* sample) {
    GstCaps* caps = gst_sample_get_caps(sample);
    GstStructure* st = caps ? gst_caps_get_structure(caps, 0) : nullptr;
    int w = 0, h = 0;
    if (st) {
        gst_structure_get_int(st, "width", &w);
        gst_structure_get_int(st, "height", &h);
    }
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (w <= 0 || h <= 0 || !buffer || !gst_buffer_map(buffer, &map, GST_MAP_READ)) return;
    if (s->channel || openSlot(s, (uint32_t)w, (uint32_t)h)) {
        ensureGeometry(s, w, h);
        flBuffer buf{};
        if (flAcquireBuffer(s->channel, &buf, 5) != FL_OK) {
            ++s->ringFull;
        } else {
            ID3D11Device* dev = flChannelD3D11Device(s->channel);
            ComPtr<ID3D11DeviceContext> ctx;
            dev->GetImmediateContext(&ctx);
            ID3D11Texture2D* tex = flImageD3D11(buf.image);
            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);
            // Upload the top-left min(src,dst) region: normally the whole
            // frame, clamped in case the consumer declined the geometry.
            UINT cw = (UINT)w < td.Width ? (UINT)w : td.Width;
            UINT chh = (UINT)h < td.Height ? (UINT)h : td.Height;
            D3D11_BOX box{0, 0, 0, cw, chh, 1};
            ctx->UpdateSubresource(tex, 0, &box, map.data, (UINT)w * 4, (UINT)w * 4 * chh);
            if (flSubmit(s->channel, &buf, -1) == FL_OK) ++s->frames;
        }
    }
    gst_buffer_unmap(buffer, &map);
}

void pumpSession(Session* s) {
    if (!s->appsink || s->failed.load()) return;
    // A decoder error would otherwise look like a frozen mirror. Release the
    // tile (only this thread touches the channel) and stop feeding; the
    // pipeline itself is torn down with the session, because raop threads may
    // still be pushing into its appsrc.
    if (GstMessage* m = gst_bus_pop_filtered(s->bus, GST_MESSAGE_ERROR)) {
        GError* e = nullptr;
        gchar* dbg = nullptr;
        gst_message_parse_error(m, &e, &dbg);
        fprintf(stderr, "airplay: '%s' decode error: %s%s%s\n", s->label.c_str(),
                e ? e->message : "?", dbg ? " - " : "", dbg ? dbg : "");
        if (e) g_error_free(e);
        g_free(dbg);
        gst_message_unref(m);
        s->failed = true;
        s->producing = false;
        if (s->channel) {
            flClose(s->channel);
            s->channel = nullptr;
        }
        return;
    }
    // Pull every ready frame and push it to the renderer.
    for (;;) {
        GstSample* sample =
            gst_app_sink_try_pull_sample(GST_APP_SINK(s->appsink), 0); // non-blocking
        if (!sample) return;
        ++s->decoded;
        if (s->gpu) deliverGpu(s, sample);
        else deliverCpu(s, sample);
        gst_sample_unref(sample);
    }
}

// Service every presenter, and reap the ones whose sender has gone. Teardown
// happens HERE rather than in the raop callback so GStreamer teardown and
// the framelink detach never race this loop.
void pumpDecodedFrames() {
    std::vector<Session*> dead;
    {
        std::lock_guard<std::mutex> hold(g_sessionsMutex);
        for (size_t i = 0; i < g_sessions.size();) {
            Session* s = g_sessions[i];
            if (s->ended.load()) {
                dead.push_back(s);
                g_sessions.erase(g_sessions.begin() + (ptrdiff_t)i);
            } else {
                ++i;
            }
        }
    }
    for (Session* s : dead) destroySession(s);

    // Copy the live set so the pull loop runs without holding the lock - a
    // raop thread admitting a new sender must not block on a decode pull.
    std::vector<Session*> live;
    {
        std::lock_guard<std::mutex> hold(g_sessionsMutex);
        live = g_sessions;
    }
    for (Session* s : live) pumpSession(s);
}

// Per-presenter rates. Reported as RECEIVED vs DELIVERED so a stutter can be
// attributed: if the two track each other the sender is pacing that way, and
// if delivered lags, we are dropping frames on the way to the scene.
//
// Deliberately per-session deltas rather than a global total: summing counters
// that vanish when a sender leaves made the total go DOWN, and the unsigned
// subtraction printed "+18446744073709548712 in 5s".
void reportRates() {
    std::lock_guard<std::mutex> hold(g_sessionsMutex);
    for (Session* s : g_sessions) {
        if (!s->producing.load()) continue;
        const uint64_t rx = s->received.load();
        const uint64_t dRx = rx - s->lastReceived;
        const uint64_t dTx = s->frames - s->lastFrames;
        s->lastReceived = rx;
        s->lastFrames = s->frames;
        if (dRx == 0 && dTx == 0) continue;
        const uint64_t maxGap = s->maxGapUs.exchange(0);
        const uint64_t stalls = s->longGaps.exchange(0);
        const uint64_t dFull = s->ringFull - s->lastRingFull;
        s->lastRingFull = s->ringFull;
        // Only call it dropping if it is worth noticing: a +/-1 difference is
        // just a frame landing either side of the interval boundary.
        const bool dropping = dTx + 5 < dRx;
        printf("airplay: '%s' 5s: %llu rx, %llu to scene (%.1f fps), worst gap %.0f ms, "
               "%llu stalls >40ms, %llu ring-full%s\n",
               s->label.c_str(), (unsigned long long)dRx, (unsigned long long)dTx, dRx / 5.0,
               maxGap / 1000.0, (unsigned long long)stalls, (unsigned long long)dFull,
               dropping ? "  <-- DROPPING" : "");
    }
}

// ---- raop callbacks (invoked on raop threads) ------------------------------

// M25: the core ran pair-pin-start and wants this code shown on our screen;
// the user types it into the Apple device's password prompt.
void cbDisplayPin(void*, char* pin) {
    printf("airplay: PIN required - showing %s on screen\n", pin ? pin : "?");
    g_pinShowing = true;
    vkr::pinOverlayShow(pin ? pin : "", "AirPlay");
}

// Admit a sender. Returning NULL refuses the connection, which the core now
// turns into an immediate failure for that device instead of leaving it
// unanswered - the old behaviour showed a spinner on the sender forever.
void* cbSessionInit(void*) {
    std::lock_guard<std::mutex> hold(g_sessionsMutex);
    size_t presenters = 0;
    for (Session* s : g_sessions)
        if (s->producing.load()) ++presenters;
    if (presenters >= kMaxSessions) {
        fprintf(stderr, "airplay: refusing sender - %zu of %zu tiles in use\n", presenters,
                kMaxSessions);
        return nullptr;
    }
    // No pipeline yet. Most connections never carry video, and building a
    // decoder for each one wasted one per handshake.
    Session* s = new Session();
    s->label = "unnamed";
    g_sessions.push_back(s);
    return s;
}

// Called on a raop thread once the sender is gone. Only flag it: the pump
// thread owns teardown.
void cbSessionDestroy(void* cls) {
    if (Session* s = (Session*)cls) s->ended = true;
}

void cbVideoProcess(void* cls, raop_ntp_t*, h264_decode_struct* data) {
    Session* s = (Session*)cls;
    if (!s || s->ended.load() || s->failed.load() || data->data_len <= 0) return;
    // First frame on this connection: THIS is the one carrying the mirror, so
    // build its decoder now and mark it a presenter.
    if (!s->ready.load()) {
        if (!buildPipeline(s)) return;
        s->producing = true;
        printf("airplay: '%s' is presenting\n", s->label.c_str());
    }
    s->received.fetch_add(1);
    // Gap since the previous AU from this sender, measured where the frame
    // ENTERS us - so a stall here is the sender's or the network's, not ours.
    {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        const uint64_t us = (uint64_t)(now.QuadPart * 1000000ll / freq.QuadPart);
        const uint64_t prev = s->lastArrivalUs.exchange(us);
        if (prev) {
            const uint64_t gap = us - prev;
            if (gap > s->maxGapUs.load()) s->maxGapUs.store(gap);
            if (gap > 40000) s->longGaps.fetch_add(1);
        }
    }
    // Authorized: the mirror is flowing, so the code is no longer needed.
    if (g_pinShowing.exchange(false)) vkr::pinOverlayHide();
    GstBuffer* buf = gst_buffer_new_allocate(nullptr, data->data_len, nullptr);
    gst_buffer_fill(buf, 0, data->data, data->data_len);
    gst_app_src_push_buffer(GST_APP_SRC(s->appsrc), buf); // takes ownership
}
void cbAudioProcess(void*, raop_ntp_t*, audio_decode_struct*) {}
void cbVideoFlush(void*) {}
void cbAudioFlush(void*) {}
// Called around the first frame / stream pauses. MUST be set: raop_rtp_mirror
// invokes video_resume/video_pause unguarded (found via live-cast crash - a
// call through NULL at the first encrypted IDR after SPS/PPS; the Android
// port had these, the Windows port was missing them).
void cbVideoPause(void*) {}
void cbVideoResume(void*) {}
void cbConnInit(void*) { printf("airplay: client connected\n"); }
void cbConnDestroy(void* cls) {
    printf("airplay: client disconnected\n");
    // The session (and with it the scene tile) is released by cbSessionDestroy
    // on the pump thread; this only reports.
    (void)cls;
}
void cbConnReset(void*, int, bool) {}
void cbReportClient(void* cls, char* deviceid, char*, char* name, bool* admit) {
    printf("airplay: sender '%s' (%s)\n", name ? name : "?", deviceid ? deviceid : "?");
    // Name the tile after the device, so multi-presenter logs are readable.
    if (Session* s = (Session*)cls) {
        if (name && *name) s->label = name;
    }
    *admit = true;
}
void cbLog(void*, int level, const char* msg) {
    fprintf(level <= LOGGER_ERR ? stderr : stdout, "airplay-raop: %s\n", msg);
}

// ---- self-test: a synthetic mirror through the real frame path --------------
//
// `vkr_airplay --selftest [frames] [pipe]`: no AirPlay, no sender. Encodes N
// test frames with x264 and hands the access units to cbVideoProcess exactly as
// the RAOP core would, so the decoder, the GPU colour pass and the framelink
// submit run unchanged against a live renderer (`vkrenderer --serve <pipe>
// --headless -1` works for this). PASS means the frames reached the ring on the
// zero-copy path, or on the CPU path when that one was selected.
int runSelfTest(int frames) {
    Session* s = (Session*)cbSessionInit(nullptr);
    if (!s) return 1;
    s->label = "selftest";
    char desc[512];
    snprintf(desc, sizeof(desc),
             "videotestsrc is-live=true num-buffers=%d pattern=smpte ! "
             "video/x-raw,format=NV12,width=1280,height=720,framerate=30/1 ! "
             "x264enc tune=zerolatency bframes=0 key-int-max=30 speed-preset=ultrafast ! "
             "video/x-h264,stream-format=byte-stream,alignment=au ! "
             "appsink name=enc sync=false",
             frames);
    GError* err = nullptr;
    GstElement* enc = gst_parse_launch(desc, &err);
    if (!enc || err) {
        fprintf(stderr, "selftest: encoder pipeline failed: %s\n", err ? err->message : "?");
        return 1;
    }
    GstElement* encSink = gst_bin_get_by_name(GST_BIN(enc), "enc");
    gst_element_set_state(enc, GST_STATE_PLAYING);
    int pushed = 0;
    ULONGLONG lastAu = GetTickCount64();
    for (;;) {
        // Same cadence as main(): drain decoded frames every few ms whether or
        // not an access unit arrived. With appsink max-buffers=1 a pump that
        // only ran per AU would itself drop frames and blame the GPU path.
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(encSink), 4 * GST_MSECOND);
        if (sample) {
            GstBuffer* b = gst_sample_get_buffer(sample);
            GstMapInfo map;
            if (b && gst_buffer_map(b, &map, GST_MAP_READ)) {
                h264_decode_struct au{};
                au.nal_count = 1;
                au.data = map.data;
                au.data_len = (int)map.size;
                cbVideoProcess(s, nullptr, &au);
                ++pushed;
                gst_buffer_unmap(b, &map);
            }
            gst_sample_unref(sample);
            lastAu = GetTickCount64();
        } else if (gst_app_sink_is_eos(GST_APP_SINK(encSink)) || GetTickCount64() - lastAu > 2000) {
            break; // num-buffers reached, or the encoder stalled
        }
        pumpDecodedFrames();
    }
    // Drain: a hardware decoder holds a frame or two until the next one lands.
    ULONGLONG deadline = GetTickCount64() + 1500;
    while (s->frames + 2 < (uint64_t)pushed && GetTickCount64() < deadline) {
        pumpDecodedFrames();
        Sleep(4);
    }
    const uint64_t delivered = s->frames;
    const uint64_t decoded = s->decoded;
    // Diagnostic: what EOS flushes out is exactly what the decoder was holding
    // back during the live run - i.e. latency the mirror would have had.
    if (s->appsrc) gst_app_src_end_of_stream(GST_APP_SRC(s->appsrc));
    deadline = GetTickCount64() + 1500;
    while (s->decoded < (uint64_t)pushed && GetTickCount64() < deadline) {
        pumpDecodedFrames();
        Sleep(4);
    }
    const uint64_t heldBack = s->decoded - decoded;
    const uint64_t ringFull = s->ringFull;
    const bool gpu = s->gpu;
    const bool failed = s->failed.load();
    flChannelInfo info{};
    if (s->channel) flQuery(s->channel, &info);
    gst_element_set_state(enc, GST_STATE_NULL);
    gst_object_unref(encSink);
    gst_object_unref(enc);
    s->ended = true;
    pumpDecodedFrames(); // reaps it: closes the slot, tears the decoder down
    const bool ok = !failed && pushed == frames && delivered + 2 >= (uint64_t)pushed &&
                    (gpu || g_decodeMode == DecodeMode::Cpu);
    printf("selftest: %s - %d AUs pushed, %llu decoded live (+%llu only flushed by EOS), "
           "%llu ring-full drops, %llu frames delivered to the ring (%ux%u BGRA, generation %llu) "
           "via the %s path\n",
           ok ? "PASS" : "FAIL", pushed, (unsigned long long)decoded, (unsigned long long)heldBack,
           (unsigned long long)ringFull, (unsigned long long)delivered, info.width, info.height,
           (unsigned long long)info.generation, gpu ? "GPU zero-copy" : "CPU upload");
    return ok ? 0 : 1;
}

} // namespace

// Crash triage: print the faulting module+offset before dying, so a live
// cast failure leaves evidence in the log even when WER stays silent.
LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE mod = nullptr;
    char modName[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)addr, &mod) &&
        mod) {
        GetModuleFileNameA(mod, modName, sizeof(modName));
    }
    fprintf(stderr, "airplay: FATAL exception 0x%08lX at %p (%s + 0x%llx), thread %lu\n",
            ep->ExceptionRecord->ExceptionCode, addr, modName,
            (unsigned long long)((uintptr_t)addr - (uintptr_t)mod), GetCurrentThreadId());
    fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER; // die, but with the line above logged
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(crashFilter);
    const bool selfTest = argc > 1 && !strcmp(argv[1], "--selftest");
    int selfTestFrames = 60;
    if (selfTest) {
        if (argc > 2) selfTestFrames = atoi(argv[2]);
        g_pipe = argc > 3 ? argv[3] : "vkrender";
        if (selfTestFrames <= 0) selfTestFrames = 60;
    } else {
        g_pipe = argc > 1 ? argv[1] : "vkrender";
    }
    const char* name = !selfTest && argc > 2 ? argv[2] : "vkrender";

    gst_init(&argc, &argv);

    // Decode path: GPU (zero-copy) unless overridden or the decoder is absent.
    if (const char* mode = getenv("VKR_AIRPLAY_DECODE")) {
        if (!_stricmp(mode, "cpu")) g_decodeMode = DecodeMode::Cpu;
        else if (_stricmp(mode, "gpu") != 0)
            fprintf(stderr, "airplay: VKR_AIRPLAY_DECODE=%s ignored (gpu|cpu)\n", mode);
    }
    if (g_decodeMode == DecodeMode::Gpu) {
        if (GstElementFactory* f = gst_element_factory_find("d3d11h264dec")) {
            gst_object_unref(f);
        } else {
            fprintf(stderr, "airplay: d3d11h264dec not installed - CPU decode, one upload per frame\n");
            g_decodeMode = DecodeMode::Cpu;
        }
    }
    printf("airplay: decode path: %s\n",
           g_decodeMode == DecodeMode::Gpu ? "GPU (d3d11h264dec -> BGRA ring, zero-copy)"
                                           : "CPU (avdec_h264 + upload)");

    // Fail fast if the renderer is down; slots are taken lazily, per sender, at
    // the mirror's true resolution, and released when that sender stops.
    //
    // FL_BUSY is a PASS here: it means slot 0 exists and someone is using it,
    // which proves the renderer is running just as well as a free slot does.
    {
        flChannel* probe = nullptr;
        const std::string slot0 = std::string(g_pipe) + ".in.0";
        const flResult r = flOpenProducer(slot0.c_str(), &probe);
        if (r == FL_OK) flClose(probe);
        if (r != FL_OK && r != FL_BUSY) {
            fprintf(stderr, "airplay: no framelink input slots on '%s' - %s\n", g_pipe,
                    flResultString(r));
            return 1;
        }
    }
    if (selfTest) return runSelfTest(selfTestFrames);
    // No pipeline here any more - each sender builds its own in cbSessionInit.

    int error = 0;
    // Stable locally-administered MAC from the machine GUID-ish source.
    unsigned char hw[6];
    {
        DWORD v = GetTickCount();
        unsigned char seed[6] = {0x02, 0x76, 0x6b, (unsigned char)(v), (unsigned char)(v >> 8),
                                 (unsigned char)(v >> 16)};
        char comp[64];
        DWORD n = sizeof(comp);
        if (GetComputerNameA(comp, &n))
            for (DWORD i = 0; i < n; ++i) seed[i % 6] ^= (unsigned char)comp[i];
        memcpy(hw, seed, 6);
        hw[0] = (hw[0] | 0x02) & ~0x01;
    }

    // M25: optional PIN. require_pw advertises `pw=true` in the RAOP/AirPlay
    // TXT records, so Apple senders run the pair-pin exchange and the core
    // calls display_pin with the code to show on this screen.
    const bool usePin = vkr::pinEnabled();
    g_dnssd = dnssd_init(name, (int)strlen(name), (const char*)hw, 6, &error, usePin ? 1 : 0);
    if (!g_dnssd) {
        fprintf(stderr, "airplay: dnssd_init failed (%d)\n", error);
        return 1;
    }

    raop_callbacks_t cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.video_process = cbVideoProcess;
    cbs.audio_process = cbAudioProcess;
    cbs.video_flush = cbVideoFlush;
    cbs.audio_flush = cbAudioFlush;
    cbs.conn_init = cbConnInit;
    cbs.conn_destroy = cbConnDestroy;
    cbs.conn_reset = cbConnReset;
    cbs.report_client_request = cbReportClient;
    cbs.video_pause = cbVideoPause;
    cbs.video_resume = cbVideoResume;
    // Multi-presenter: one decode pipeline and scene ring PER sender.
    cbs.session_init = cbSessionInit;
    cbs.session_destroy = cbSessionDestroy;
    if (usePin) cbs.display_pin = cbDisplayPin;

    g_raop = raop_init(&cbs);
    if (!g_raop) {
        fprintf(stderr, "airplay: raop_init failed\n");
        return 1;
    }
    char mac[18];
    snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", hw[0], hw[1], hw[2], hw[3],
             hw[4], hw[5]);
    // Empty keyfile: derive a persistent ed25519 key from the device id
    // (avoids OpenSSL 3.x provider keygen + FILE* BIO across the DLL boundary).
    std::string keyfile = "";
    // Was 1, which is why a second sender hung: the core stops selecting the
    // server socket when full, so the connection was never accepted and never
    // answered. One sender needs several httpd connections, so allow a
    // generous multiple of the tile budget rather than exactly kMaxSessions.
    if (raop_init2(g_raop, (int)kMaxSessions * 4, mac, keyfile.c_str())) {
        fprintf(stderr, "airplay: raop_init2 failed\n");
        return 1;
    }
    raop_set_log_callback(g_raop, cbLog, nullptr);
    raop_set_log_level(g_raop, getenv("VKR_AIRPLAY_DEBUG") ? LOGGER_DEBUG : LOGGER_INFO);
    raop_set_plist(g_raop, "width", 1920);
    raop_set_plist(g_raop, "height", 1080);
    raop_set_plist(g_raop, "refreshRate", 60);
    raop_set_plist(g_raop, "maxFPS", 30);
    raop_set_plist(g_raop, "overscanned", 0);
    raop_set_plist(g_raop, "max_ntp_timeouts", 5);
    // A fixed VKR_PIN pins the code; auto lets the core mint one per pairing.
    //
    // The core's convention is the OPPOSITE of what this comment used to claim.
    // raop_handler_pairpinstart does:
    //     if (raop->pin > 9999) pin_4 = raop->pin % 10000;  // use ours
    //     else                  pin_4 = random_pin();       // mint a new one
    // so a bare 1234 is <= 9999 and gets silently REPLACED by a random code -
    // VKR_PIN=1234 behaved exactly like auto, and the code the user typed was
    // always refused. Offset past the sentinel so the low four digits are ours.
    if (usePin) {
        std::string fixed = vkr::pinForConnection();
        if (!fixed.empty() && getenv("VKR_PIN") && strcmp(getenv("VKR_PIN"), "auto") != 0) {
            int code = atoi(fixed.c_str()) % 10000;
            raop_set_plist(g_raop, "pin", 10000 + code);
            printf("airplay: fixed PIN %04d\n", code);
        }
    }

    unsigned short port = 0;
    if (raop_start(g_raop, &port) <= 0) { // httpd_start returns 1 on success
        fprintf(stderr, "airplay: raop_start failed\n");
        return 1;
    }
    raop_set_port(g_raop, port);
    raop_set_dnssd(g_raop, g_dnssd);
    dnssd_register_raop(g_dnssd, port);
    dnssd_register_airplay(g_dnssd, port);
    printf("airplay: receiving as '%s' on port %u - cast to see it on the scene\n", name, port);

    // Drive the decode drain + a status heartbeat on the main thread.
    uint64_t last = 0;
    int tick = 0;
    for (;;) {
        pumpDecodedFrames();
        Sleep(4);
        if (++tick % 1250 == 0) reportRates(); // ~5 s
    }
}
