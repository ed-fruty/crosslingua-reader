# CrossLingua Reader

> **A bilingual-first fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).**
> Read in two languages without turning an e-reader into a phone.

CrossLingua keeps the capable, open-source CrossPoint reading experience and
builds a complete bilingual workflow around it: translate an EPUB directly on
the device or prepare it with the
[Ebook Translator Calibre plugin](https://github.com/bookfere/Ebook-Translator-Calibre-Plugin),
then choose how much of each language you want to see.

**Runs on:** Xteink [X3](https://www.xteink.com/products/xteink-x3),
[X4](https://www.xteink.com/products/xteink-x4), X4 Pro, X4 Classic, M5Stack
Paper Mono, Seeed Studio XIAO ePaper Display Board (Sticky), Metalio E-Ink 4,
and EEGO Reader A4. Every device except X3/X4 uses a dedicated build with its
device-specific display, touch, key, power, and frontlight integrations.

<table>
  <tr>
    <td align="center" width="33%">
      <img src="./docs/images/crosslingua/interlinear.jpg" alt="Interlinear bilingual reading on CrossLingua" /><br/>
      <strong>Interlinear</strong><br/>
      <sub>Translation follows the source line by line.</sub>
    </td>
    <td align="center" width="33%">
      <img src="./docs/images/crosslingua/side-by-side-landscape.jpg" alt="Side-by-side bilingual reading on CrossLingua" /><br/>
      <strong>Side by Side</strong><br/>
      <sub>Two languages, aligned in parallel columns.</sub>
    </td>
    <td align="center" width="33%">
      <img src="./docs/images/crosslingua/bookshelf.jpg" alt="CrossLingua Bookshelf with book cover previews" /><br/>
      <strong>Bookshelf</strong><br/>
      <sub>Browse books visually by their covers.</sub>
    </td>
  </tr>
</table>

## What makes CrossLingua different?

## Lingua: bilingual reading built into the reader

Lingua can translate the current chapter or an entire EPUB over Wi-Fi. The
result is cached on the SD card, so after translation the book can be read
offline and its display mode can be changed without sending the text again.
The original EPUB is never modified.

CrossLingua also recognizes translations embedded in an EPUB as
language-tagged paragraphs. That makes the same reading modes available for
bilingual books prepared outside the device, including books produced with the
[Ebook Translator Calibre plugin](https://github.com/bookfere/Ebook-Translator-Calibre-Plugin).

### Two translation workflows

- **On the device:** choose the source and target languages, translate one
  chapter or the whole book, and keep the generated bilingual copy in the
  book's SD-card cache. A single chapter usually takes **1–3 minutes**. A full
  book usually takes **10–30 minutes**. Actual time depends on your Wi-Fi
  connection, translation-engine availability, the amount of text, and similar
  conditions.
- **With Calibre:** prepare a bilingual EPUB with the
  [Ebook Translator plugin](https://github.com/bookfere/Ebook-Translator-Calibre-Plugin)
  and transfer it to the reader. CrossLingua detects its language-tagged
  translation paragraphs and applies the same Lingua modes without translating
  the book again.

### Translation engines

| Engine | API key | Notes |
|---|---:|---|
| **Google v2** | No | Default free Google backend |
| **Google v1 / HTML** | No | Free Google backend with HTML-aware requests |
| **Google legacy** | No | Compatibility option for older settings; currently routed through Google v2 |
| **Azure** | No | Keyless Microsoft Edge translation endpoint |
| **DeepL** | Yes | DeepL API Free |
| **DeepL Pro** | Yes | DeepL paid API |
| **OpenAI** | Yes | Chat Completions translation |
| **DeepSeek** | Yes | DeepSeek chat translation |
| **Gemini** | Yes | Google Gemini translation |

> Free, undocumented translation endpoints may be rate-limited, changed, or
> withdrawn by their providers. Text is sent to the selected provider only
> while a translation is being created.

## Eight ways to read

The same translated book can be viewed in any of these modes. Mode-specific
options appear directly below the active mode in the **Lingua** menu.

### Interlinear

Compact translation annotations sit above the source lines they belong to.
This keeps both languages in one reading flow without duplicating full
paragraph blocks. The annotation colour can be set to **Black**, **Grey**, or
**Light Grey**.

<p align="center">
  <img src="./docs/images/crosslingua/interlinear.jpg" width="420" alt="CrossLingua Interlinear mode"/>
</p>

### Side by Side

The source and translation are laid out as synchronized left and right
columns. Only the translation column's colour is configurable: **Black**,
**Grey**, or **Light Grey**. It works in both landscape and portrait
orientations.

<table>
  <tr>
    <td align="center">
      <img src="./docs/images/crosslingua/side-by-side-landscape.jpg" alt="Side-by-side mode in landscape orientation"/>
    </td>
    <td align="center">
      <img src="./docs/images/crosslingua/side-by-side-portrait.jpg" alt="Side-by-side mode in portrait orientation"/>
    </td>
  </tr>
</table>

### Interleaved

Original and translated paragraphs alternate in the normal page flow.
Translations can use **Dimmed** or **Dimmed Light** ink and either the
**Same** or a **Smaller** text size.

### Tooltip

Read the uncluttered original page and reveal one sentence translation at a
time in a small overlay. Choose the **front** or **side** button pair for
sentence stepping, choose whether the control **loops on the page** or
**turns the page and continues**, and set translation text to the **Same** or
a **Smaller** size.

<table>
  <tr>
    <td align="center">
      <img src="./docs/images/crosslingua/tooltip-landscape.jpg" alt="CrossLingua Tooltip mode in landscape orientation"/><br/>
      <sub>Tooltip in landscape orientation.</sub>
    </td>
    <td align="center">
      <img src="./docs/images/crosslingua/tooltip-portrait.jpg" alt="CrossLingua Tooltip mode in portrait orientation"/><br/>
      <sub>Tooltip in portrait orientation.</sub>
    </td>
  </tr>
</table>

### Page Translation

Keep the original page visible and open a larger translation overlay for the
text on that page. Choose the **front** or **side** buttons for the overlay
controls and use the **Same** or a **Smaller** translation size.

### Original Only

Show only the source-language text. This is useful for immersion reading
while keeping translations available for a quick switch to Tooltip or Page
Translation.

### Translation Only

Show only the translated text as a clean monolingual book.

### Normal

Render the source and translation together in the EPUB's normal flow, using
the same text colour. This is the neutral compatibility mode for already
bilingual EPUBs.

## Bookshelf with cover previews

**Bookshelf** is a visual alternative to the file browser and to CrossPoint's
list-based **Library** (both stay on the home screen). It renders a 3×3
grid, generates and caches EPUB cover thumbnails in the background, preserves
folder navigation, and lets you open a book directly from its cover.

<table>
  <tr>
    <td align="center" width="33%">
      <img src="./docs/images/crosslingua/bookshelf-folders.jpg" alt="CrossLingua Bookshelf series and folder navigation"/><br/>
      <sub>Organize the library into series and folders.</sub>
    </td>
    <td align="center" width="33%">
      <img src="./docs/images/crosslingua/bookshelf-series.jpg" alt="A complete Mistborn series in CrossLingua Bookshelf"/><br/>
      <sub>Browse a complete series by its covers.</sub>
    </td>
    <td align="center" width="33%">
      <img src="./docs/images/crosslingua/bookshelf.jpg" alt="CrossLingua Bookshelf loading and caching cover previews"/><br/>
      <sub>Cover previews are generated and cached in the background.</sub>
    </td>
  </tr>
</table>

## A custom font made for CrossLingua

We developed **EdsLab**, a custom reading font created specifically for
CrossLingua and comfortable bilingual reading on an e-ink screen. It is the
default font, but you are not locked into it: custom `.cpfont` families can
still be installed from the SD card. See
[Custom SD-card fonts](./docs/sd-card-fonts.md).

## Everything inherited from CrossPoint

CrossLingua remains a full CrossPoint Reader fork, not a translation-only
experiment. It includes the Library view (indexed title/author search,
recent, alphabetical and per-author views), EPUB 2/3 rendering, `.xtc/.xtch`, `.txt` and `.bmp`
support, images, embedded styles, hyphenation, kerning, footnotes, bookmarks,
StarDict dictionaries, chapter navigation, go-to-percent, focus reading,
auto page turn, orientation control, screenshots, custom fonts, themes,
button remapping, KOReader progress sync, OPDS 1 and 2, SD-card plugins, a Bluetooth page
turner (X3/X4), WebDAV, Calibre wireless
transfer, a web file manager and settings UI, EPUB optimization, OTA updates,
24 UI languages and RTL support.

## Roadmap

CrossLingua is actively evolving. This section will track the next Lingua
modes, translation workflows, library improvements, and device support as
they are planned.

- [ ] Roadmap details coming soon

---

## USB-locked devices (Xteink Unlocker)

Some Xteink units purchased from third-party stores (e.g. AliExpress) ship with
USB flashing locked from the factory. If your device is locked, use the Xteink
Unlocker at https://crosspointreader.com/#unlock-tool before flashing.

**You do not need this tool if you bought your device directly from xteink.com.** Those units are not locked.

**Not sure if your device is locked?** Power it on, connect the USB-C cable, and try flashing via the web flasher first (see
[Install firmware](#install-firmware) below). If the browser's serial device picker does not show your device, try a different
USB port or browser before assuming the device is locked. Only reach for the unlocker if the device still doesn't appear.

## Install firmware

### Choose the correct firmware file

Each hardware family has its own release binary. Download the file that exactly
matches your device from the
[CrossLingua releases page](https://github.com/ed-fruty/crosslingua-reader/releases).
`<version>` is the CrossLingua version of the release, e.g. `1.5.0` for the
release tagged `v1.5.0+crosspoint.1.6.5`:

| Device | Release file | Chip |
| --- | --- | --- |
| Xteink X3 / X4 | `crosslingua-<version>-x3-x4.bin` | ESP32-C3 |
| Xteink X4 Pro | `crosslingua-<version>-x4pro.bin` | ESP32-S3 |
| Xteink X4 Classic | `crosslingua-<version>-x4c.bin` | ESP32-S3 |
| M5Stack Paper Mono | `crosslingua-<version>-papermono.bin` | ESP32-S3 |
| Seeed Studio XIAO ePaper Display Board (Sticky) | `crosslingua-<version>-sticky.bin` | ESP32-S3 |
| Metalio E-Ink 4 | `crosslingua-<version>-metalio_eink4.bin` | ESP32-S3 |
| EEGO Reader A4 | `crosslingua-<version>-eego_a4.bin` | ESP32-S3 |

Each release also carries the same binaries under their pre-1.5 names
(`firmware.bin` for X3/X4, `firmware-<board>.bin` for the others, e.g.
`firmware-x4pro.bin`). They exist so devices still running CrossLingua 1.4 or
older can find the update over the air; for a manual install, either name
works.

> [!WARNING]
> Never flash a binary intended for another device. CrossLingua validates the
> chip and board identity during in-device updates, but external flashing tools
> may not provide the same protection.

### Update from an existing CrossLingua installation (recommended)

Connect the device to Wi-Fi, then open **Settings → System → Check for
updates**. The built-in OTA updater checks the latest CrossLingua release and
automatically selects the correct file for the detected hardware, so you do
not need to download or choose a `.bin` manually. Devices on CrossLingua 1.4
or older update the same way. Confirm the update when
prompted, keep the device powered while it is installing, and wait for it to
restart.

To update from an SD card instead:

1. Download the matching file from the table above.
2. Copy it anywhere on the SD card.
3. Open the firmware update option on the device and select the downloaded
   `.bin` file.
4. Confirm the update and keep the device powered until it restarts.

The SD-card picker accepts any `.bin` filename, so files such as
`crosslingua-<version>-x4pro.bin` do **not** need to be renamed.

### Web installer — Xteink X3 / X4

The web-flasher procedure below is for Xteink X3 and X4 only. Do not select an
X3/X4 profile for any other device.

1. Connect your device to your computer via USB-C and wake/unlock the device
2. Download `crosslingua-<version>-x3-x4.bin` from the
   [CrossLingua releases page](https://github.com/ed-fruty/crosslingua-reader/releases).
3. Open https://crosspointreader.com/#flash-tools, select X3 or X4, choose
   **Custom .bin**, and upload the downloaded firmware.

To install a specific version, download the X3/X4 file from that version's
release instead of the latest release and follow the same steps.

Some Xteink devices purchased from third-party stores must be unlocked first;
see [USB-locked devices](#usb-locked-devices-xteink-unlocker).

### First installation on other devices

The X3/X4 web-flasher and ESP32-C3 command below are not compatible with these
ESP32-S3 devices. If CrossPoint or CrossLingua is already installed, use its
SD-card firmware updater with the matching release file. For development or a
first installation from source, use the corresponding PlatformIO environment:

```bash
pio run -e x4pro -t upload          # Xteink X4 Pro
pio run -e x4c -t upload            # Xteink X4 Classic
pio run -e papermono -t upload      # M5Stack Paper Mono
pio run -e sticky -t upload         # Seeed Sticky
pio run -e metalio_eink4 -t upload  # Metalio E-Ink 4
pio run -e eego_a4 -t upload        # EEGO Reader A4
```

Connect only the target device while uploading. Board-specific USB boot mode,
drivers, and connection steps depend on the hardware revision; follow the
device manufacturer's instructions if PlatformIO cannot detect it.

### Revert to Official Firmware

To revert to the official firmware, you can also flash the latest official firmware using https://crosspointreader.com/#flash-tools.

### Command line — Xteink X3 / X4 only

This command writes the shared X3/X4 ESP32-C3 application image. Do not use it
for any other device.

1. Install [`esptool`](https://github.com/espressif/esptool):

```bash
pip install esptool
```

2. Download `crosslingua-<version>-x3-x4.bin` from the
   [CrossLingua releases page](https://github.com/ed-fruty/crosslingua-reader/releases).
3. Connect your device via USB-C.
4. Find the device port. On Linux, run `dmesg` after connecting. On macOS:

```bash
log stream --predicate 'subsystem == "com.apple.iokit"' --info
```

5. Flash:

```bash
esptool.py --chip esp32c3 --port /dev/ttyACM0 --baud 921600 write_flash 0x10000 /path/to/crosslingua-<version>-x3-x4.bin
```

Adjust `/dev/ttyACM0` to match your system.

### Manual

See [Development quick start](#development-quick-start) below.

---

## Custom SD-card fonts

On devices with external RAM enabled in CrossPoint, copy `.ttf`, `.otf`, or `.ttc` files to the SD card and select them as reader fonts. Put one file in `/fonts/` or `/.fonts/`, or put one family's files in a subfolder. See the [SD card font guide](./docs/sd-card-fonts.md) for the folder layout and styles.

On other devices, convert the font to `.cpfont` first. `.cpfont` files also work on devices with external RAM enabled and have better performance. No firmware reflash is needed to add fonts.

To make `.cpfont` files:

1. Go to https://crosspointreader.com/fonts and open the "SD-card font builder" form.
2. Upload up to four styles (regular, bold, italic, bold-italic), set the family name, point sizes, and Unicode range.
3. Download the generated `.cpfont` files.
4. Copy them to your SD card under `/fonts/YourFont/` (or `/.fonts/YourFont/` to hide the folder).
5. Select the font on the device from the font settings.

Conversion runs the firmware repo's `lib/EpdFont/scripts/fontconvert_sdcard.py` script unmodified, so output matches a local host build.

---

## Documentation

- [User Guide](./USER_GUIDE.md)
- [Web server usage](./docs/webserver.md)
- [Web server endpoints](./docs/webserver-endpoints.md)
- [Project scope](./SCOPE.md)
- [Contributing docs](./docs/contributing/README.md)
- [Touch and UI development](./docs/contributing/touch-and-ui.md) - how to build new screens on the FreeInkUI activity bases (UiListActivity and friends), plus build envs for the non-Xteink touch devices

---

## Development quick start

### Prerequisites

- [pioarduino](https://github.com/pioarduino/pioarduino) or VS Code + pioarduino plugin
- Python 3.8+
- `clang-format` 21
- USB-C cable supporting data transfer

### Setup

```bash
git clone --recursive https://github.com/ed-fruty/crosslingua-reader
cd crosslingua-reader

# if cloned without --recursive:
git submodule update --init --recursive
```

### Nix/NixOS

Nix/NixOS users can enter the development shell with either `nix develop` (flakes) or `nix-shell`:

```bash
nix develop -f nix
# or
nix-shell nix
```

To flash a connected ESP32-C3 device, enable PlatformIO's udev rules in your NixOS configuration:

```nix
services.udev.packages = with pkgs; [ platformio-core.udev ];
```

After rebuilding the system configuration, reconnect the device or reload udev rules.

### Build / flash / monitor

```bash
pio run --target upload
```

### Contributor pre-PR checks

```bash
./bin/clang-format-fix
pio check -e default
pio run -e default
```

### Debugging

After flashing the new features, it’s recommended to capture detailed logs from the serial port.

First, make sure all required Python packages are installed:

```python
python3 -m pip install pyserial colorama matplotlib
```

After that run the script:

```sh
# For Linux
# This was tested on Debian and should work on most Linux systems.
python3 scripts/debugging_monitor.py

# For macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```

Minor adjustments may be required for Windows.

---

## Internals

CrossLingua is pretty aggressive about caching data down to the SD card to
minimise RAM usage. The ESP32-C3 only has ~380KB of usable RAM, so we have to
be careful. A lot of the decisions made in the design of the firmware were
based on this constraint.

### Data caching

The first time chapters of a book are loaded, they are cached to the SD card. Subsequent loads are served from the
cache. This cache directory exists at `.crosspoint` on the SD card. The structure is as follows:

```text
.crosspoint/
├── epub_<hash>/         # one directory per book, named by content hash
│   ├── progress.bin     # reading position (chapter, page, etc.)
│   ├── cover.bmp        # generated cover image
│   ├── book.bin         # metadata: title, author, spine, TOC
│   ├── css_rules.cache  # parsed CSS rule cache
│   ├── img_*            # rendered image cache files
│   └── sections/        # per-chapter layout cache
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── settings.json        # device settings
├── state.json           # resume/runtime state
└── recent.json          # recent books list
```

Removing `/.crosspoint` clears all cached metadata and forces a full regeneration on next open. Book deletes, overwrites, and moves done through the firmware or web UI clear or re-key matching caches; manual SD-card edits may leave stale cache directories behind.

For more details on the internal file structures, see the [file formats document](./docs/file-formats.md).

---

## Support the project

If CrossLingua is useful to you and you would like to support further development, testing, and maintenance, you can leave a donation here:

[![Support on Ko-fi](https://img.shields.io/badge/Support_on_Ko--fi-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/edfruty)
[![Support on Donatello](https://img.shields.io/badge/Support_on_Donatello-F9C80E?style=for-the-badge&logo=buymeacoffee&logoColor=111111)](https://donatello.to/EdFruty)

Donations are optional. Bug reports, testing results, and suggestions are valuable contributions too.

---

## Contributing

Contributions are welcome. If you're new to the codebase, start with the
[contributing docs](./docs/contributing/README.md). For upstream CrossPoint
work, see its [ideas discussion board](https://github.com/crosspoint-reader/crosspoint-reader/discussions/categories/ideas).

Everyone here is a volunteer, so please be respectful and patient. For governance and community expectations, see [GOVERNANCE.md](./GOVERNANCE.md).

---

CrossLingua Reader is **not affiliated with Xteink or any device manufacturer**.

Huge shoutout to [diy-esp32-epub-reader](https://github.com/atomic14/diy-esp32-epub-reader), which inspired this project.
