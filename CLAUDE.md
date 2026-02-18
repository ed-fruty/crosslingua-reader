# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

CrossPoint Reader is open-source firmware for the **Xteink X4** e-paper reader, targeting **ESP32-C3** (~380KB usable RAM). It supports EPUB 2/3, TXT, and XTC reading on a 480×800 e-ink display. Built with PlatformIO + Arduino framework, C++20.

## Build Commands

```sh
pio run                    # Build (default env, debug logging)
pio run -e gh_release      # Build release (error-only logging)
pio run --target upload    # Build and flash via USB-C
```

Pre-build scripts run automatically: `scripts/gen_i18n.py` (i18n code generation) and `scripts/build_html.py` (web server HTML compression).

## I18n Workflow

Translations live in `lib/I18n/translations/*.yaml` (8 languages). After modifying YAML files, run:

```sh
python3 scripts/gen_i18n.py
```

This regenerates `lib/I18n/I18nKeys.h`, `I18nStrings.h`, and `I18nStrings.cpp`. Strings are referenced via `StrId::STR_*` enums and accessed with `tr(STR_*)`.

## Code Formatting

Uses clang-format (Google-derived style, 120 column limit, 2-space indent). PRs are checked for semantic commit message titles (conventional commits via `amannn/action-semantic-pull-request`).

## Architecture

### Activity Pattern

Each screen is an `Activity` subclass (similar to Android Activities):
- **Lifecycle:** `onEnter()` → `loop()` (each frame) → `onExit()`
- **Rendering:** `render(RenderLock&&)` runs on a dedicated FreeRTOS task. Call `requestUpdate()` to trigger re-render.
- **Nesting:** `ActivityWithSubactivity` supports overlay screens (e.g., reader menu over reader).
- **Navigation:** Function-pointer callbacks, not a stack. Activities are heap-allocated, deleted on exit, replaced by new instances. `main.cpp` wires up `onGoHome()`, `onGoToReader()`, etc.

Key activity groups: `boot_sleep/`, `home/`, `reader/`, `settings/`, `browser/`, `network/`, `util/`.

### Reader Activities

- `EpubReaderActivity` — EPUB rendering with chapter-based pagination, cached to SD card
- `TxtReaderActivity` — Plain text with word-wrap pagination
- `XtcReaderActivity` — XTC format
- Each has `renderStatusBar()` with configurable modes (enum `STATUS_BAR_MODE` in `CrossPointSettings.h`)

### Settings System

- `CrossPointSettings` — singleton (`SETTINGS` macro), ~30 options persisted as binary to SD card
- `SettingsList.h` — declarative setting definitions shared between device UI and web API. Types: `Enum`, `Toggle`, `Value`, `String`, `DynamicString`, `DynamicEnum`
- `CrossPointState` — separate singleton (`APP_STATE` macro) for transient state (open book path, crash counter)
- Settings enums use `*_COUNT` sentinels for validation — new enum values go before the `_COUNT` entry

### Rendering

- `GfxRenderer` wraps the e-ink display; supports BW + grayscale anti-aliasing (two-pass)
- `RenderLock` (RAII mutex) prevents section deletion during rendering
- 4 orientations supported; orientation is applied via `renderer.setOrientation()`
- Fonts registered by ID: Bookerly (4 weights × 4 sizes), EdsLab (1 weight × 4 sizes), Ubuntu UI fonts

### Hardware Abstraction

- `open-x4-sdk/` — git submodule with low-level drivers (display, GPIO, SD card)
- `MappedInputManager` — translates physical buttons to logical buttons per user config
- `HalDisplay`, `HalGPIO`, `HalStorage` — HAL wrappers in `lib/hal/`

### Libraries (`lib/`)

- `Epub/` — EPUB parsing (spine, TOC, sections, pages). `Section` handles chapter layout/caching; `Page` renders a single page.
- `I18n/` — internationalization (generated from YAML)
- `GfxRenderer/` — display drawing primitives and font rendering
- `EpdFont/` — e-paper font format and rendering
- `miniz/` + `ZipFile/` — ZIP extraction for EPUB
- `expat/` — XML parsing
- `KOReaderSync/` — KOReader progress sync protocol
- `OpdsParser/` — OPDS catalog browsing

## Key Constraints

- **ESP32-C3 with only 380KB usable RAM** — this is an extremely memory-constrained embedded device. Avoid large heap allocations, prefer stack variables, and cache data to SD card rather than holding it in memory.
- **Single-buffer e-ink** — rendering is single-buffered for memory conservation
- EPUB chapters are parsed once and cached as binary files under `.crosspoint/` on SD card
- **Before implementing any "heavy" feature** (frequent timers, background tasks, large buffers, continuous polling, extra FreeRTOS tasks, persistent WiFi connections, etc.), warn the user and ask for confirmation — these can impact RAM usage, CPU load, and battery life on this constrained device.
