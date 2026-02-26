#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <string>

/**
 * Translates an entire EPUB file paragraph-by-paragraph using Google Translate.
 * Creates a new bilingual EPUB on the SD card.
 *
 * Runs asynchronously in a FreeRTOS task. Check getProgress() from the UI task.
 */
class EpubTranslator {
 public:
  struct Progress {
    int currentChapter = 0;
    int totalChapters = 0;
    bool done = false;
    bool failed = false;
    bool cancelled = false;
    char statusMsg[80] = {};
  };

  EpubTranslator();
  ~EpubTranslator();

  // Start translation asynchronously. Returns false if already running.
  bool start(const std::string& epubPath, const char* targetLang, const std::string& outputPath);

  // Request cancellation. The task will stop at the next paragraph boundary.
  void cancel();

  Progress getProgress() const;
  bool isRunning() const;

 private:
  mutable SemaphoreHandle_t mutex;
  volatile bool cancelRequested = false;
  volatile bool running = false;
  Progress progress;

  struct TaskParams {
    EpubTranslator* self;
    std::string epubPath;
    std::string targetLang;
    std::string outputPath;
  };

  static void taskFunc(void* param);
  void run(const std::string& epubPath, const char* targetLang, const std::string& outputPath);
  void setProgress(const Progress& p);
};
