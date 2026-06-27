#include <Arduino.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <SPI.h>
#include <builtinFonts/all.h>

#include <cstring>

#include "Battery.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "activities/boot_sleep/BootActivity.h"
#include "activities/boot_sleep/SleepActivity.h"
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/home/HomeActivity.h"
#include "activities/home/LibraryActivity.h"
#include "activities/home/MyLibraryActivity.h"
#include "activities/home/RecentBooksActivity.h"
#include "activities/network/CrossPointWebServerActivity.h"
#include "activities/reader/ReaderActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

HalDisplay display;
HalGPIO gpio;
MappedInputManager mappedInputManager(gpio);
GfxRenderer renderer(display);
Activity* currentActivity;

// Fonts
EpdFont bookerly14RegularFont(&bookerly_14_regular);
EpdFont bookerly14BoldFont(&bookerly_14_bold);
EpdFont bookerly14ItalicFont(&bookerly_14_italic);
EpdFont bookerly14BoldItalicFont(&bookerly_14_bolditalic);
EpdFontFamily bookerly14FontFamily(&bookerly14RegularFont, &bookerly14BoldFont, &bookerly14ItalicFont,
                                   &bookerly14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont bookerly12RegularFont(&bookerly_12_regular);
EpdFont bookerly12BoldFont(&bookerly_12_bold);
EpdFont bookerly12ItalicFont(&bookerly_12_italic);
EpdFont bookerly12BoldItalicFont(&bookerly_12_bolditalic);
EpdFontFamily bookerly12FontFamily(&bookerly12RegularFont, &bookerly12BoldFont, &bookerly12ItalicFont,
                                   &bookerly12BoldItalicFont);
EpdFont bookerly16RegularFont(&bookerly_16_regular);
EpdFont bookerly16BoldFont(&bookerly_16_bold);
EpdFont bookerly16ItalicFont(&bookerly_16_italic);
EpdFont bookerly16BoldItalicFont(&bookerly_16_bolditalic);
EpdFontFamily bookerly16FontFamily(&bookerly16RegularFont, &bookerly16BoldFont, &bookerly16ItalicFont,
                                   &bookerly16BoldItalicFont);
EpdFont bookerly18RegularFont(&bookerly_18_regular);
EpdFont bookerly18BoldFont(&bookerly_18_bold);
EpdFont bookerly18ItalicFont(&bookerly_18_italic);
EpdFont bookerly18BoldItalicFont(&bookerly_18_bolditalic);
EpdFontFamily bookerly18FontFamily(&bookerly18RegularFont, &bookerly18BoldFont, &bookerly18ItalicFont,
                                   &bookerly18BoldItalicFont);

#endif  // OMIT_FONTS

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// EdsLab: dedicated Regular/Bold/Italic/BoldItalic weights. Size 10 (UI titles) uses Ubuntu bold.
EpdFont edslab10Font(&edslab_10_regular);
EpdFontFamily edslab10FontFamily(&edslab10Font, &ui10BoldFont, &edslab10Font, &ui10BoldFont);
#ifndef OMIT_FONTS
EpdFont edslab12Font(&edslab_12_regular);
EpdFont edslab12BoldFont(&edslab_12_bold);
EpdFont edslab12ItalicFont(&edslab_12_italic);
EpdFont edslab12BoldItalicFont(&edslab_12_bolditalic);
EpdFontFamily edslab12FontFamily(&edslab12Font, &edslab12BoldFont, &edslab12ItalicFont, &edslab12BoldItalicFont);
EpdFont edslab14Font(&edslab_14_regular);
EpdFont edslab14BoldFont(&edslab_14_bold);
EpdFont edslab14ItalicFont(&edslab_14_italic);
EpdFont edslab14BoldItalicFont(&edslab_14_bolditalic);
EpdFontFamily edslab14FontFamily(&edslab14Font, &edslab14BoldFont, &edslab14ItalicFont, &edslab14BoldItalicFont);
EpdFont edslab16Font(&edslab_16_regular);
EpdFont edslab16BoldFont(&edslab_16_bold);
EpdFont edslab16ItalicFont(&edslab_16_italic);
EpdFont edslab16BoldItalicFont(&edslab_16_bolditalic);
EpdFontFamily edslab16FontFamily(&edslab16Font, &edslab16BoldFont, &edslab16ItalicFont, &edslab16BoldItalicFont);
EpdFont edslab18Font(&edslab_18_regular);
EpdFont edslab18BoldFont(&edslab_18_bold);
EpdFont edslab18ItalicFont(&edslab_18_italic);
EpdFont edslab18BoldItalicFont(&edslab_18_bolditalic);
EpdFontFamily edslab18FontFamily(&edslab18Font, &edslab18BoldFont, &edslab18ItalicFont, &edslab18BoldItalicFont);
// Caecilia: regular weight only — borrow EdsLab bold/italic for styled text
EpdFont caecilia12Font(&caecilia_12_regular);
EpdFontFamily caecilia12FontFamily(&caecilia12Font, &edslab12BoldFont, &edslab12ItalicFont, &edslab12BoldItalicFont);
EpdFont caecilia14Font(&caecilia_14_regular);
EpdFontFamily caecilia14FontFamily(&caecilia14Font, &edslab14BoldFont, &edslab14ItalicFont, &edslab14BoldItalicFont);
EpdFont caecilia16Font(&caecilia_16_regular);
EpdFontFamily caecilia16FontFamily(&caecilia16Font, &edslab16BoldFont, &edslab16ItalicFont, &edslab16BoldItalicFont);
EpdFont caecilia18Font(&caecilia_18_regular);
EpdFontFamily caecilia18FontFamily(&caecilia18Font, &edslab18BoldFont, &edslab18ItalicFont, &edslab18BoldItalicFont);
// GPro: regular weight only — borrow EdsLab bold/italic for styled text
EpdFont gpro12Font(&gpro_12_regular);
EpdFontFamily gpro12FontFamily(&gpro12Font, &edslab12BoldFont, &edslab12ItalicFont, &edslab12BoldItalicFont);
EpdFont gpro14Font(&gpro_14_regular);
EpdFontFamily gpro14FontFamily(&gpro14Font, &edslab14BoldFont, &edslab14ItalicFont, &edslab14BoldItalicFont);
EpdFont gpro16Font(&gpro_16_regular);
EpdFontFamily gpro16FontFamily(&gpro16Font, &edslab16BoldFont, &edslab16ItalicFont, &edslab16BoldItalicFont);
EpdFont gpro18Font(&gpro_18_regular);
EpdFontFamily gpro18FontFamily(&gpro18Font, &edslab18BoldFont, &edslab18ItalicFont, &edslab18BoldItalicFont);
#endif  // OMIT_FONTS

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

void exitActivity() {
  if (currentActivity) {
    currentActivity->onExit();
    delete currentActivity;
    currentActivity = nullptr;
  }
}

void enterNewActivity(Activity* activity) {
  currentActivity = activity;
  currentActivity->onEnter();
}

// Verify power button press duration on wake-up from deep sleep
// Pre-condition: isWakeupByPowerButton() == true
void verifyPowerButtonDuration() {
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) {
    // Fast path for short press
    // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
    return;
  }

  // Give the user up to 1000ms to start holding the power button, and must hold for SETTINGS.getPowerButtonDuration()
  const auto start = millis();
  bool abort = false;
  // Subtract the current time, because inputManager only starts counting the HeldTime from the first update()
  // This way, we remove the time we already took to reach here from the duration,
  // assuming the button was held until now from millis()==0 (i.e. device start time).
  const uint16_t calibration = start;
  const uint16_t calibratedPressDuration =
      (calibration < SETTINGS.getPowerButtonDuration()) ? SETTINGS.getPowerButtonDuration() - calibration : 1;

  gpio.update();
  // Needed because inputManager.isPressed() may take up to ~500ms to return the correct state
  while (!gpio.isPressed(HalGPIO::BTN_POWER) && millis() - start < 1000) {
    delay(10);  // only wait 10ms each iteration to not delay too much in case of short configured duration.
    gpio.update();
  }

  t2 = millis();
  if (gpio.isPressed(HalGPIO::BTN_POWER)) {
    do {
      delay(10);
      gpio.update();
    } while (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() < calibratedPressDuration);
    abort = gpio.getHeldTime() < calibratedPressDuration;
  } else {
    abort = true;
  }

  if (abort) {
    // Button released too early. Returning to sleep.
    // IMPORTANT: Re-arm the wakeup trigger before sleeping again
    gpio.startDeepSleep();
  }
}

void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

// Enter deep sleep mode
void enterDeepSleep() {
  APP_STATE.lastSleepFromReader = currentActivity && currentActivity->isReaderActivity();
  APP_STATE.saveToFile();
  exitActivity();
  enterNewActivity(new SleepActivity(renderer, mappedInputManager));

  display.deepSleep();
  LOG_DBG("MAIN", "Power button press calibration value: %lu ms", t2 - t1);
  LOG_DBG("MAIN", "Entering deep sleep");

  gpio.startDeepSleep();
}

void onGoHome();
void onGoToMyLibraryWithPath(const std::string& path);
void onGoToRecentBooks();
void onGoToReader(const std::string& initialEpubPath) {
  exitActivity();
  enterNewActivity(new ReaderActivity(renderer, mappedInputManager, initialEpubPath, onGoHome,
                                      onGoToMyLibraryWithPath));
}

void onGoToFileTransfer() {
  exitActivity();
  enterNewActivity(new CrossPointWebServerActivity(renderer, mappedInputManager, onGoHome));
}

void onGoToSettings() {
  exitActivity();
  enterNewActivity(new SettingsActivity(renderer, mappedInputManager, onGoHome));
}

void onGoToLibrary() {
  exitActivity();
  enterNewActivity(new LibraryActivity(renderer, mappedInputManager, onGoHome, onGoToReader));
}

void onGoToMyLibrary() {
  exitActivity();
  enterNewActivity(new MyLibraryActivity(renderer, mappedInputManager, onGoHome, onGoToReader));
}

void onGoToRecentBooks() {
  exitActivity();
  enterNewActivity(new RecentBooksActivity(renderer, mappedInputManager, onGoHome, onGoToReader));
}

void onGoToMyLibraryWithPath(const std::string& path) {
  exitActivity();
  enterNewActivity(new MyLibraryActivity(renderer, mappedInputManager, onGoHome, onGoToReader, path));
}

void onGoToBrowser() {
  exitActivity();
  enterNewActivity(new OpdsBookBrowserActivity(renderer, mappedInputManager, onGoHome));
}

void onGoHome() {
  exitActivity();
  enterNewActivity(new HomeActivity(renderer, mappedInputManager, onGoToReader, onGoToMyLibrary, onGoToLibrary,
                                    onGoToRecentBooks, onGoToSettings, onGoToFileTransfer, onGoToBrowser));
}

void setupDisplayAndFonts() {
  display.begin();
  renderer.begin();
  LOG_DBG("MAIN", "Display initialized");
  renderer.insertFont(BOOKERLY_14_FONT_ID, bookerly14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(BOOKERLY_12_FONT_ID, bookerly12FontFamily);
  renderer.insertFont(BOOKERLY_16_FONT_ID, bookerly16FontFamily);
  renderer.insertFont(BOOKERLY_18_FONT_ID, bookerly18FontFamily);

  // Glyph fallback: the custom book fonts (EdsLab/Caecilia/GPro) are missing some
  // typographic glyphs — e.g. the “ ” double curly quotes and U+2003 em-space (paragraph indent)
  // — which makes them silently disappear. Route misses to the same-size Bookerly (full coverage)
  // so they render instead. Must be set BEFORE insertFont, which copies the family into the map.
  // (EdsLab-10 has no Bookerly-10; it borrows Bookerly-12, the nearest size.)
  edslab10FontFamily.setFallback(&bookerly12FontFamily);
  edslab12FontFamily.setFallback(&bookerly12FontFamily);
  edslab14FontFamily.setFallback(&bookerly14FontFamily);
  edslab16FontFamily.setFallback(&bookerly16FontFamily);
  edslab18FontFamily.setFallback(&bookerly18FontFamily);
  caecilia12FontFamily.setFallback(&bookerly12FontFamily);
  caecilia14FontFamily.setFallback(&bookerly14FontFamily);
  caecilia16FontFamily.setFallback(&bookerly16FontFamily);
  caecilia18FontFamily.setFallback(&bookerly18FontFamily);
  gpro12FontFamily.setFallback(&bookerly12FontFamily);
  gpro14FontFamily.setFallback(&bookerly14FontFamily);
  gpro16FontFamily.setFallback(&bookerly16FontFamily);
  gpro18FontFamily.setFallback(&bookerly18FontFamily);

  renderer.insertFont(EDSLAB_10_FONT_ID, edslab10FontFamily);
  renderer.insertFont(EDSLAB_12_FONT_ID, edslab12FontFamily);
  renderer.insertFont(EDSLAB_14_FONT_ID, edslab14FontFamily);
  renderer.insertFont(EDSLAB_16_FONT_ID, edslab16FontFamily);
  renderer.insertFont(EDSLAB_18_FONT_ID, edslab18FontFamily);

  renderer.insertFont(CAECILIA_12_FONT_ID, caecilia12FontFamily);
  renderer.insertFont(CAECILIA_14_FONT_ID, caecilia14FontFamily);
  renderer.insertFont(CAECILIA_16_FONT_ID, caecilia16FontFamily);
  renderer.insertFont(CAECILIA_18_FONT_ID, caecilia18FontFamily);

  renderer.insertFont(GPRO_12_FONT_ID, gpro12FontFamily);
  renderer.insertFont(GPRO_14_FONT_ID, gpro14FontFamily);
  renderer.insertFont(GPRO_16_FONT_ID, gpro16FontFamily);
  renderer.insertFont(GPRO_18_FONT_ID, gpro18FontFamily);
#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);
  LOG_DBG("MAIN", "Fonts setup");
}

void setup() {
  t1 = millis();

  gpio.begin();

  // Only start serial if USB connected
  if (gpio.isUsbConnected()) {
    Serial.begin(115200);
    // Wait up to 3 seconds for Serial to be ready to catch early logs
    unsigned long start = millis();
    while (!Serial && (millis() - start) < 3000) {
      delay(10);
    }
  }

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts();
    exitActivity();
    enterNewActivity(new FullScreenMessageActivity(renderer, mappedInputManager, "SD card error", EpdFontFamily::BOLD));
    return;
  }

  SETTINGS.loadFromFile();
  I18N.loadSettings();
  KOREADER_STORE.loadFromFile();
  UITheme::getInstance().reload();
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  switch (gpio.getWakeupReason()) {
    case HalGPIO::WakeupReason::PowerButton:
      // For normal wakeups, verify power button press duration
      LOG_DBG("MAIN", "Verifying power button press duration");
      verifyPowerButtonDuration();
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      gpio.startDeepSleep();
      break;
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossPoint version " CROSSPOINT_VERSION);

  setupDisplayAndFonts();

  exitActivity();
  enterNewActivity(new BootActivity(renderer, mappedInputManager));

  APP_STATE.loadFromFile();
  RECENT_BOOKS.loadFromFile();

  // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
  // crashed (indicated by readerActivityLoadCount > 0)
  if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
      mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    onGoHome();
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    onGoToReader(path);
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  gpio.update();

  renderer.setFadingFix(SETTINGS.fadingFix);
  renderer.setColorTextGrayLevel(SETTINGS.colorTextStyle <= 2 ? SETTINGS.colorTextStyle : (uint8_t)0);

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes", ESP.getFreeHeap(), ESP.getHeapSize(),
            ESP.getMinFreeHeap());
    lastMemPrint = millis();
  }

  // Handle incoming serial commands,
  // nb: we use logSerial from logging to avoid deprecation warnings
  if (logSerial.available() > 0) {
    String line = logSerial.readStringUntil('\n');
    if (line.startsWith("CMD:")) {
      String cmd = line.substring(4);
      cmd.trim();
      if (cmd == "SCREENSHOT") {
        logSerial.printf("SCREENSHOT_START:%d\n", HalDisplay::BUFFER_SIZE);
        uint8_t* buf = display.getFrameBuffer();
        logSerial.write(buf, HalDisplay::BUFFER_SIZE);
        logSerial.printf("SCREENSHOT_END\n");
      }
    }
  }

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || (currentActivity && currentActivity->preventAutoSleep())) {
    lastActivityTime = millis();  // Reset inactivity timer
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() > SETTINGS.getPowerButtonDuration()) {
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  const unsigned long activityStartTime = millis();
  if (currentActivity) {
    currentActivity->loop();
  }
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  if (currentActivity && currentActivity->skipLoopDelay()) {
    yield();  // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;  // 3 seconds
    if (millis() - lastActivityTime >= IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      delay(50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(10);
    }
  }
}
