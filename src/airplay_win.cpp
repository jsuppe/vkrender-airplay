// M19: AirPlay receiver -> vkrender producer (Windows native).
//
// Ports the same vendored UxPlay RAOP + mirroring core as the Android APK
// (third_party/uxplay/lib) to MSVC, and turns the mirror into a vkrender
// producer stream: the raop video_process callback hands over decrypted H.264
// access units, which a small GStreamer pipeline (appsrc -> h264parse ->
// d3d11h264dec -> convert -> BGRA appsink) decodes, and each decoded frame is
// uploaded into a dequeued vkrender_client ring texture. Discovery uses the
// native Win32 DnsServiceRegister (dnssd_win.c) - no Bonjour needed.
//
//   vkr_airplay [pipe] [name]
//
// Requirements: the vkrenderer service (--serve) must be running; a
// Miracast/AirPlay-capable network (multicast reachable). Video only.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d11.h>
#include <wrl/client.h>

#define GST_USE_UNSTABLE_API
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

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

#include "pin_overlay.h"
#include "vkrender_client.h"

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
    vkrClient* client = nullptr;
    GstElement* pipeline = nullptr;
    GstElement* appsrc = nullptr;
    GstElement* appsink = nullptr;
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

// ---- GStreamer decode: H.264 AUs -> BGRA frames -> vkrClient ring ----------

bool buildPipeline(Session* s) {
    GError* err = nullptr;
    // System-memory BGRA out: the frame is UpdateSubresource'd into the ring
    // texture on the client's own device, so no cross-device sharing is
    // needed (one upload per frame - fine for a phone mirror).
    // Software decode (avdec_h264): the d3d11 hardware decoder crashed during
    // state change on this stack (E_NOINTERFACE on the video device); CPU
    // decode of a phone mirror is comfortably fast and avoids the d3d11
    // device juggling. The frame is uploaded to the ring texture anyway.
    const char* desc =
        "appsrc name=src is-live=true format=time do-timestamp=true "
        "  caps=video/x-h264,stream-format=byte-stream,alignment=au ! "
        "h264parse ! avdec_h264 ! videoconvert ! "
        "video/x-raw,format=BGRA ! appsink name=sink max-buffers=4 drop=true sync=false";
    s->pipeline = gst_parse_launch(desc, &err);
    if (!s->pipeline || err) {
        fprintf(stderr, "airplay: pipeline build failed: %s\n", err ? err->message : "?");
        if (err) g_error_free(err);
        return false;
    }
    s->appsrc = gst_bin_get_by_name(GST_BIN(s->pipeline), "src");
    s->appsink = gst_bin_get_by_name(GST_BIN(s->pipeline), "sink");
    gst_element_set_state(s->pipeline, GST_STATE_PLAYING);
    s->ready = true;
    return true;
}

// Tear one presenter down. Main pump thread only - GStreamer state changes and
// vkrDisconnect must not race the pull loop.
void destroySession(Session* s) {
    if (s->client) {
        vkrDisconnect(s->client);
        s->client = nullptr;
    }
    if (s->pipeline) {
        gst_element_set_state(s->pipeline, GST_STATE_NULL);
        if (s->appsrc) gst_object_unref(s->appsrc);
        if (s->appsink) gst_object_unref(s->appsink);
        gst_object_unref(s->pipeline);
        s->pipeline = nullptr;
        s->appsrc = s->appsink = nullptr;
    }
    printf("airplay: session '%s' released after %llu frames\n", s->label.c_str(),
           (unsigned long long)s->frames);
    delete s;
}

void pumpSession(Session* s) {
    if (!s->appsink) return;
    // Pull every ready BGRA frame and push it to the renderer.
    for (;;) {
        GstSample* sample =
            gst_app_sink_try_pull_sample(GST_APP_SINK(s->appsink), 0); // non-blocking
        if (!sample) return;
        GstCaps* caps = gst_sample_get_caps(sample);
        GstStructure* st = caps ? gst_caps_get_structure(caps, 0) : nullptr;
        int w = 0, h = 0;
        if (st) {
            gst_structure_get_int(st, "width", &w);
            gst_structure_get_int(st, "height", &h);
        }
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (w > 0 && h > 0 && buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            // Lazy connect at the mirror's true resolution (a fixed-size ring
            // cropped larger mirrors to its top-left corner).
            if (!s->client) {
                s->client = vkrConnect(g_pipe, nullptr, w, h);
                if (s->client)
                    printf("airplay: '%s' producing %dx%d\n", s->label.c_str(), w, h);
            }
            vkrBuffer buf;
            if (s->client && vkrDequeueBuffer(s->client, &buf, 5) == 0) {
                ComPtr<ID3D11Device> dev = vkrGetDevice(s->client);
                ComPtr<ID3D11DeviceContext> ctx;
                dev->GetImmediateContext(&ctx);
                D3D11_TEXTURE2D_DESC td{};
                buf.texture->GetDesc(&td);
                // Upload the top-left min(src,dst) region (ring is 1280x720;
                // mirrors are commonly larger - a full letterbox/scale pass is
                // a future refinement).
                UINT cw = (UINT)w < td.Width ? (UINT)w : td.Width;
                UINT chh = (UINT)h < td.Height ? (UINT)h : td.Height;
                D3D11_BOX box{0, 0, 0, cw, chh, 1};
                ctx->UpdateSubresource(buf.texture, 0, &box, map.data, (UINT)w * 4,
                                       (UINT)w * 4 * chh);
                vkrQueueBuffer(s->client, &buf, -1);
                ++s->frames;
            }
            gst_buffer_unmap(buffer, &map);
        }
        gst_sample_unref(sample);
    }
}

// Service every presenter, and reap the ones whose sender has gone. Teardown
// happens HERE rather than in the raop callback so GStreamer teardown and
// vkrDisconnect never race this loop.
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
        // Only call it dropping if it is worth noticing: a +/-1 difference is
        // just a frame landing either side of the interval boundary.
        const bool dropping = dTx + 5 < dRx;
        printf("airplay: '%s' 5s: %llu rx, %llu to scene (%.1f fps), worst gap %.0f ms, "
               "%llu stalls >40ms%s\n",
               s->label.c_str(), (unsigned long long)dRx, (unsigned long long)dTx, dRx / 5.0,
               maxGap / 1000.0, (unsigned long long)stalls, dropping ? "  <-- DROPPING" : "");
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
    // No pipeline yet. Most connections never carry video, and building an
    // avdec_h264 graph for each one wasted a decoder per handshake.
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
    if (!s || s->ended.load() || data->data_len <= 0) return;
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
    g_pipe = argc > 1 ? argv[1] : "vkrender";
    const char* name = argc > 2 ? argv[2] : "vkrender";

    gst_init(&argc, &argv);

    // Fail fast if the service is down; the real connection is made lazily at
    // the mirror's true resolution, and released when the sender stops.
    vkrClient* probe = vkrConnect(g_pipe, nullptr, 64, 64);
    if (!probe) {
        fprintf(stderr, "airplay: cannot reach vkrenderer service on pipe '%s'\n", g_pipe);
        return 1;
    }
    vkrDisconnect(probe);
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
