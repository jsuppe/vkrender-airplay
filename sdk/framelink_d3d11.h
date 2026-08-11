// framelink D3D11 interop (Windows). The ONLY platform-specific header a
// Windows integrator needs; the call sequence in framelink.h is unchanged.
//
// The Linux and Android equivalents (framelink_dmabuf.h, framelink_ahb.h) and
// the portable framelink_vulkan.h are not in the spike.
#pragma once
#include <d3d11.h>

#include "framelink.h"

#ifdef __cplusplus
extern "C" {
#endif

// The texture backing an acquired image. Borrowed - do NOT Release() it; it
// dies with the channel.
FL_API ID3D11Texture2D* flImageD3D11(flImage*);

// The device the channel opened the shared textures on. Render with this (or
// with your own device on the same adapter).
FL_API ID3D11Device* flChannelD3D11Device(flChannel*);

// The raw shared NT handle, for importing into D3D12, Vulkan or CUDA instead.
// Borrowed: do NOT CloseHandle() it. This is the escape hatch that stops
// ID3D11Texture2D in the ABI from locking integrators into D3D11.
FL_API void* flImageSharedHandle(flImage*);

#ifdef __cplusplus
}
#endif
