// M25: the PIN overlay - a topmost, click-through window that shows the
// pairing PIN on the Windows device while a sender is waiting to be
// authorized. Shared by every receiver (Miracast / WHIP / AirPlay) so all
// modalities look identical on screen.
//
// Threading: show()/hide() may be called from any thread (the receivers'
// protocol callbacks); the window lives on its own UI thread inside.
//
// Also carries the PIN policy helper: pins come from VKR_PIN (fixed) or are
// generated per connection when VKR_PIN=auto / VKR_PIN_AUTO=1.
#ifndef VKR_PIN_OVERLAY_H
#define VKR_PIN_OVERLAY_H

#include <string>

namespace vkr {

// Is PIN authorization enabled at all? (VKR_PIN set and not "off"/"0")
bool pinEnabled();

// The PIN to require for the next connection: the fixed VKR_PIN value, or a
// fresh 4-digit random one in auto mode. Empty when disabled.
std::string pinForConnection();

// Show/replace the overlay ("Enter PIN on your device" + big digits) with an
// optional protocol label ("AirPlay", "Miracast", "WebRTC"). Idempotent.
void pinOverlayShow(const std::string& pin, const std::string& label);

// Hide the overlay (connection authorized, rejected, or abandoned).
void pinOverlayHide();

} // namespace vkr

#endif // VKR_PIN_OVERLAY_H
