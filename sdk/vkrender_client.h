#pragma once

// vkrender client library (M14, Windows): a BufferQueue-shaped producer API
// over the vkrenderer service. The renderer owns the buffer ring (like
// Android's AImageReader); the client dequeues a shared D3D11 texture,
// renders into it with its own device, and queues it back. Zero copies:
// the texture the client writes is the exact allocation the renderer's
// Vulkan samples.
//
//   vkrClient* c = vkrConnect("vkrender", NULL, 640, 360);   // NULL = library
//   ID3D11Device* dev = vkrGetDevice(c);                     //   creates the device
//   for (;;) {
//       vkrBuffer buf;
//       if (vkrDequeueBuffer(c, &buf, 100) != 0) continue;   // backpressure
//       /* render into buf.texture with dev's immediate context */
//       vkrQueueBuffer(c, &buf, ptsNs);                      // fence + deliver
//   }
//   vkrDisconnect(c);
//
// Threading: all calls for one vkrClient must come from one thread (or be
// externally serialized). Rendering must use the device's immediate context -
// vkrQueueBuffer signals the shared fence there, ordering it after your draws.

#ifdef _WIN32

#include <d3d11.h>

#ifdef VKR_CLIENT_EXPORTS
#define VKR_API __declspec(dllexport)
#else
#define VKR_API __declspec(dllimport)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct vkrClient vkrClient;

typedef struct vkrBuffer {
    unsigned slot;
    ID3D11Texture2D* texture; // owned by the library; valid until vkrDisconnect
} vkrBuffer;

// Connects to the renderer service's named pipe and requests a width x height
// BGRA ring. device may be NULL: the library then creates a D3D11 device on
// the renderer's adapter (retrieve it with vkrGetDevice). If you pass your
// own device it must live on the same adapter as the renderer.
// Returns NULL on failure.
VKR_API vkrClient* vkrConnect(const char* pipeName, ID3D11Device* device, unsigned width,
                              unsigned height);

// The device rendering must happen on (yours, or the library-created one).
VKR_API ID3D11Device* vkrGetDevice(vkrClient* client);

// Dequeues a free buffer, waiting up to timeoutMs for the renderer to release
// one (backpressure). 0 = success, 1 = timeout, -1 = disconnected.
VKR_API int vkrDequeueBuffer(vkrClient* client, vkrBuffer* out, unsigned timeoutMs);

// Publishes the buffer: signals the shared fence after your queued rendering
// and notifies the renderer. ptsNs may be -1 (unknown). 0 = success.
VKR_API int vkrQueueBuffer(vkrClient* client, const vkrBuffer* buffer, long long ptsNs);

VKR_API void vkrDisconnect(vkrClient* client);

#ifdef __cplusplus
}
#endif

#endif // _WIN32
