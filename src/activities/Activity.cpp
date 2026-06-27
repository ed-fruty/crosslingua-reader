#include "Activity.h"

#include <exception>

void Activity::renderTaskTrampoline(void* param) {
  auto* self = static_cast<Activity*>(param);
  self->renderTaskLoop();
}

void Activity::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    {
      RenderLock lock(*this);
      // A render() must never abort the whole device. Decoding a corrupt/half-written
      // cover image (e.g. a preview interrupted by power loss) can throw std::bad_alloc
      // or a parse error; if it escapes the render task it calls std::terminate()/abort()
      // and the device boot-loops. Swallow it here so the frame is just skipped instead.
      try {
        render(std::move(lock));
      } catch (const std::exception& e) {
        LOG_ERR("ACT", "render() threw in '%s': %s (frame skipped)", name.c_str(), e.what());
      } catch (...) {
        LOG_ERR("ACT", "render() threw unknown exception in '%s' (frame skipped)", name.c_str());
      }
    }
  }
}

void Activity::onEnter() {
  xTaskCreate(&renderTaskTrampoline, name.c_str(),
              8192,              // Stack size
              this,              // Parameters
              1,                 // Priority
              &renderTaskHandle  // Task handle
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
  LOG_DBG("ACT", "Entering activity: %s", name.c_str());
}

void Activity::onExit() {
  RenderLock lock(*this);  // Ensure we don't delete the task while it's rendering
  if (renderTaskHandle) {
    vTaskDelete(renderTaskHandle);
    renderTaskHandle = nullptr;
  }

  LOG_DBG("ACT", "Exiting activity: %s", name.c_str());
}

void Activity::requestUpdate() {
  // Using direct notification to signal the render task to update
  // Increment counter so multiple rapid calls won't be lost
  if (renderTaskHandle) {
    xTaskNotify(renderTaskHandle, 1, eIncrement);
  }
}

void Activity::requestUpdateAndWait() {
  // FIXME @ngxson : properly implement this using freeRTOS notification
  delay(100);
}

// RenderLock

Activity::RenderLock::RenderLock(Activity& activity) : activity(activity) {
  xSemaphoreTake(activity.renderingMutex, portMAX_DELAY);
}

Activity::RenderLock::~RenderLock() { xSemaphoreGive(activity.renderingMutex); }
