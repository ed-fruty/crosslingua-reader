#include "HeapBackpressure.h"

#include <Arduino.h>
#include <Logging.h>

namespace heapbp {

bool waitForHeap(uint32_t minFree, uint32_t minLargestBlock, uint32_t timeoutMs, volatile const bool* cancelFlag,
                 const char* moduleTag, const char* waitingFor) {
  auto ready = [&]() { return ESP.getFreeHeap() >= minFree && ESP.getMaxAllocHeap() >= minLargestBlock; };

  if (ready()) return true;                     // common case: enough headroom, no wait
  if (cancelFlag && *cancelFlag) return false;  // user is already cancelling

  const uint32_t start = millis();
  LOG_INF(moduleTag, "Heap backpressure: waiting for %s (free=%u/%u, block=%u/%u)", waitingFor,
          (unsigned)ESP.getFreeHeap(), (unsigned)minFree, (unsigned)ESP.getMaxAllocHeap(), (unsigned)minLargestBlock);

  while ((uint32_t)(millis() - start) < timeoutMs) {
    delay(100);  // vTaskDelay under the hood: yields to FreeRTOS and feeds the watchdog
    if (cancelFlag && *cancelFlag) return false;
    if (ready()) {
      LOG_INF(moduleTag, "Heap backpressure: %s available after %u ms", waitingFor, (unsigned)(millis() - start));
      return true;
    }
  }

  LOG_INF(moduleTag, "Heap backpressure: timed out after %u ms waiting for %s (free=%u, block=%u)",
          (unsigned)(millis() - start), waitingFor, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  return false;
}

}  // namespace heapbp
