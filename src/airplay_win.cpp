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

vkrClient* g_client = nullptr;
GstElement* g_pipeline = nullptr;
GstElement* g_appsrc = nullptr;
GstElement* g_appsink = nullptr;
raop_t* g_raop = nullptr;
dnssd_t* g_dnssd = nullptr;
const char* g_pipe = "vkrender";
std::atomic<uint64_t> g_frames{0};
std::atomic<bool> g_pipelineReady{false};
// Set by cbConnDestroy (raop thread); handled on the main pump thread: the
// producer connection is closed so the renderer frees this stream's slots
// (otherwise the LAST FRAME stays composited on the cubes forever after the
// sender stops mirroring - observed live). Reconnected lazily on the next
// session's first decoded frame, at the mirror's true resolution.
std::atomic<bool> g_sessionEnded{false};
// M25: true while the pairing PIN overlay is up (hidden once media flows).
std::atomic<bool> g_pinShowing{false};

// ---- GStreamer decode: H.264 AUs -> BGRA frames -> vkrClient ring ----------

bool buildPipeline() {
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
    g_pipeline = gst_parse_launch(desc, &err);
    if (!g_pipeline || err) {
        fprintf(stderr, "airplay: pipeline build failed: %s\n", err ? err->message : "?");
        if (err) g_error_free(err);
        return false;
    }
    g_appsrc = gst_bin_get_by_name(GST_BIN(g_pipeline), "src");
    g_appsink = gst_bin_get_by_name(GST_BIN(g_pipeline), "sink");
    gst_element_set_state(g_pipeline, GST_STATE_PLAYING);
    g_pipelineReady = true;
    return true;
}

void pumpDecodedFrames() {
    // Sender stopped mirroring: close the producer connection so the renderer
    // frees this stream (the face falls back instead of freezing on the last
    // frame), and drop any stale queued frames.
    if (g_sessionEnded.exchange(false)) {
        if (g_client) {
            vkrDisconnect(g_client);
            g_client = nullptr;
            printf("airplay: session ended - stream released\n");
        }
        gst_element_set_state(g_pipeline, GST_STATE_READY); // flush decode queue
        gst_element_set_state(g_pipeline, GST_STATE_PLAYING);
    }
    // Pull every ready BGRA frame and push it to the renderer.
    for (;;) {
        GstSample* sample =
            gst_app_sink_try_pull_sample(GST_APP_SINK(g_appsink), 0); // non-blocking
        if (!sample) return;
        GstCaps* caps = gst_sample_get_caps(sample);
        GstStructure* s = caps ? gst_caps_get_structure(caps, 0) : nullptr;
        int w = 0, h = 0;
        if (s) {
            gst_structure_get_int(s, "width", &w);
            gst_structure_get_int(s, "height", &h);
        }
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (w > 0 && h > 0 && buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            // Lazy connect at the mirror's true resolution (a fixed-size ring
            // cropped larger mirrors to its top-left corner).
            if (!g_client) {
                g_client = vkrConnect(g_pipe, nullptr, w, h);
                if (g_client) printf("airplay: producing %dx%d\n", w, h);
            }
            vkrBuffer buf;
            if (g_client && vkrDequeueBuffer(g_client, &buf, 5) == 0) {
                ComPtr<ID3D11Device> dev = vkrGetDevice(g_client);
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
                vkrQueueBuffer(g_client, &buf, -1);
                ++g_frames;
            }
            gst_buffer_unmap(buffer, &map);
        }
        gst_sample_unref(sample);
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

void cbVideoProcess(void*, raop_ntp_t*, h264_decode_struct* data) {
    if (!g_pipelineReady || data->data_len <= 0) return;
    // Authorized: the mirror is flowing, so the code is no longer needed.
    if (g_pinShowing.exchange(false)) vkr::pinOverlayHide();
    GstBuffer* buf = gst_buffer_new_allocate(nullptr, data->data_len, nullptr);
    gst_buffer_fill(buf, 0, data->data, data->data_len);
    gst_app_src_push_buffer(GST_APP_SRC(g_appsrc), buf); // takes ownership
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
void cbConnDestroy(void*) {
    printf("airplay: client disconnected\n");
    g_sessionEnded = true; // main pump thread releases the stream
}
void cbConnReset(void*, int, bool) {}
void cbReportClient(void*, char* deviceid, char*, char* name, bool* admit) {
    printf("airplay: sender '%s' (%s)\n", name ? name : "?", deviceid ? deviceid : "?");
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
    if (!buildPipeline()) return 1;

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
    if (raop_init2(g_raop, 1, mac, keyfile.c_str())) {
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
        if (++tick % 1250 == 0) { // ~5 s
            uint64_t f = g_frames.load();
            if (f != last) {
                printf("airplay: %llu frames into the scene (+%llu in 5s)\n",
                       (unsigned long long)f, (unsigned long long)(f - last));
                last = f;
            }
        }
    }
}
