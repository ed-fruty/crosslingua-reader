#pragma once

#include <cstdint>

// Heap backpressure for the translation path.
//
// The TLS handshake and the 48 KB framebuffer restore are the two largest
// allocations during a translation run, and they happen right after the TLS
// churn that most fragments the heap. Rather than fire them into a low or
// fragmented heap and crash (a failed framebuffer realloc triggers a restart;
// wolfSSL OOMs mid-handshake), the caller waits — the design directive: "if
// memory is insufficient, do not try to allocate; leave it for the next loop
// iteration, maybe it frees up and the process continues."
namespace heapbp {

// Poll every 100 ms until BOTH ESP.getFreeHeap() >= minFree AND
// ESP.getMaxAllocHeap() >= minLargestBlock, or until timeoutMs elapses, or until
// *cancelFlag becomes true (cancelFlag may be null). ESP.getMaxAllocHeap() is
// Arduino-ESP32's wrapper for the largest contiguous free block (the same metric
// the TLS heap guards already gate on), so this reuses the repo's existing heap
// API rather than calling heap_caps_get_largest_free_block() directly.
//
// The 100 ms delay yields to FreeRTOS, so it feeds the watchdog. Returns true as
// soon as the thresholds hold (or immediately if they already do); false on
// timeout or cancel. Emits at most two LOG_INF lines per call (enter-wait, then
// resolved-or-timed-out) so a long run's device log shows backpressure working
// without flooding.
bool waitForHeap(uint32_t minFree, uint32_t minLargestBlock, uint32_t timeoutMs, volatile const bool* cancelFlag,
                 const char* moduleTag, const char* waitingFor);

}  // namespace heapbp
