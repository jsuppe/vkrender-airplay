// framelink slot sets: attaching a producer to a channel somebody else made.
//
// A producer cannot create a channel - the ring belongs to the consumer, which
// is what Android's BufferQueue forces and therefore what every backend here
// does. That leaves a gap: something has to create channels before any producer
// exists, and framelink has no discovery and no broker.
//
// A slot set is the answer, and it is deliberately the boring one. The consumer
// pre-creates N channels named `<prefix>.0` .. `<prefix>.N-1`; a producer walks
// them and takes the first free one. No registry, no daemon, no protocol - just
// a naming convention both sides agree on, which is the only kind of discovery
// that cannot itself fail.
//
// This lives in the library rather than in each application because the
// ownership model forces the pattern on everyone, and because the walk has one
// subtlety worth getting right once: FL_BUSY and FL_NOT_FOUND mean different
// things. "Taken" must not end the walk, and an exhausted set must not look
// like a misconfigured name.
#pragma once
#include <stdint.h>

#include "framelink.h"

#ifdef __cplusplus
extern "C" {
#endif

// Take the first free slot in `<prefix>.0` .. `<prefix>.slots-1`.
//
// Retries the whole sweep until `timeoutMs` elapses, because the consumer may
// not be running yet - the normal case for a receiver started at boot.
// timeoutMs = 0 means one sweep and no waiting, which is what a caller holding
// a just-decoded frame wants.
//
// `width`/`height` state what this producer intends to send, and are passed
// straight to flRequestGeometry on the slot it takes; pass 0 to accept
// whatever the consumer allocated. flQuery afterwards is still the truth.
//
// `slotIndex` (optional) receives which slot was taken - worth logging, since
// "which tile is this" is the first question when something looks wrong.
//
// Returns FL_NOT_FOUND when no channel of that name exists at all (the
// consumer is not running), and FL_BUSY when they all exist but are occupied.
// Those are different operational problems and deserve different messages.
FL_API flResult flOpenProducerSlot(const char* prefix, uint32_t slots, uint32_t width,
                                   uint32_t height, flFormat format, uint32_t timeoutMs,
                                   flChannel** out, uint32_t* slotIndex);

#ifdef __cplusplus
}
#endif
