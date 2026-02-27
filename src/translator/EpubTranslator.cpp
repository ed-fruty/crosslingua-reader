#include "EpubTranslator.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <miniz.h>

#include <cstring>

#include "TranslatingHtmlRewriter.h"

// ─── miniz FsFile callbacks ───────────────────────────────────────────────────

static size_t zipReadFunc(void* pOpaque, mz_uint64 file_ofs, void* pBuf, size_t n) {
  FsFile* f = static_cast<FsFile*>(pOpaque);
  if (!f->seek(file_ofs)) return 0;
  return f->read(pBuf, n);
}

static size_t zipWriteFunc(void* pOpaque, mz_uint64 file_ofs, const void* pBuf, size_t n) {
  FsFile* f = static_cast<FsFile*>(pOpaque);
  if (!f->seek(file_ofs)) return 0;
  return f->write(pBuf, n);
}

// ─── StringPrint — collects output into a std::string ────────────────────────

class StringPrint : public Print {
 public:
  size_t write(uint8_t c) override {
    buf += static_cast<char>(c);
    return 1;
  }
  size_t write(const uint8_t* b, size_t n) override {
    buf.append(reinterpret_cast<const char*>(b), n);
    return n;
  }
  const std::string& str() const { return buf; }
  void clear() { buf.clear(); }

 private:
  std::string buf;
};

// ─── EpubTranslator ───────────────────────────────────────────────────────────

EpubTranslator::EpubTranslator() { mutex = xSemaphoreCreateMutex(); }

EpubTranslator::~EpubTranslator() {
  if (mutex) vSemaphoreDelete(mutex);
}

bool EpubTranslator::isRunning() const { return running; }

void EpubTranslator::setProgress(const Progress& p) {
  xSemaphoreTake(mutex, portMAX_DELAY);
  progress = p;
  xSemaphoreGive(mutex);
}

EpubTranslator::Progress EpubTranslator::getProgress() const {
  xSemaphoreTake(mutex, portMAX_DELAY);
  Progress p = progress;
  xSemaphoreGive(mutex);
  return p;
}

void EpubTranslator::cancel() { cancelRequested = true; }

bool EpubTranslator::start(const std::string& epubPath, const char* targetLang, const std::string& outputPath) {
  if (running) return false;
  cancelRequested = false;
  running = true;
  progress = {};

  auto* params = new TaskParams{this, epubPath, targetLang, outputPath};
  xTaskCreate(taskFunc, "EpubTranslate", 12288, params, 1, nullptr);
  return true;
}

void EpubTranslator::taskFunc(void* param) {
  auto* p = static_cast<TaskParams*>(param);
  p->self->run(p->epubPath, p->targetLang.c_str(), p->outputPath);
  p->self->running = false;
  delete p;
  vTaskDelete(nullptr);
}

// Modify the OPF XML to append a language tag to the book title.
static std::string patchOpf(const char* data, size_t size, const char* targetLang) {
  std::string opf(data, size);
  const char* titleOpen = "<dc:title>";
  const char* titleClose = "</dc:title>";
  size_t start = opf.find(titleOpen);
  if (start == std::string::npos) return opf;
  size_t end = opf.find(titleClose, start);
  if (end == std::string::npos) return opf;
  // Insert suffix before closing tag
  std::string suffix = std::string(" [") + targetLang + "]";
  opf.insert(end, suffix);
  return opf;
}

static bool isHtmlEntry(const char* filename) {
  const char* dot = strrchr(filename, '.');
  if (!dot) return false;
  return strcmp(dot, ".html") == 0 || strcmp(dot, ".xhtml") == 0 || strcmp(dot, ".htm") == 0;
}

void EpubTranslator::run(const std::string& epubPath, const char* targetLang, const std::string& outputPath) {
  LOG_DBG("ET", "Starting translation: %s -> %s", epubPath.c_str(), outputPath.c_str());

  // ── 1. Open source EPUB with miniz (custom FsFile read callback) ────────────
  FsFile srcFile;
  if (!Storage.openFileForRead("ET", epubPath, srcFile)) {
    LOG_ERR("ET", "Cannot open source EPUB");
    Progress p;
    p.failed = true;
    snprintf(p.statusMsg, sizeof(p.statusMsg), "Cannot open source EPUB");
    setProgress(p);
    return;
  }

  mz_zip_archive srcZip;
  memset(&srcZip, 0, sizeof(srcZip));
  srcZip.m_pRead = zipReadFunc;
  srcZip.m_pIO_opaque = &srcFile;

  if (!mz_zip_reader_init(&srcZip, srcFile.size(), 0)) {
    LOG_ERR("ET", "Cannot open source ZIP");
    srcFile.close();
    Progress p;
    p.failed = true;
    snprintf(p.statusMsg, sizeof(p.statusMsg), "Cannot open source ZIP");
    setProgress(p);
    return;
  }

  // ── 3. Create output EPUB (custom FsFile write callback) ───────────────────
  FsFile dstFile = Storage.open(outputPath.c_str(), O_RDWR | O_CREAT | O_TRUNC);
  if (!dstFile) {
    LOG_ERR("ET", "Cannot create output file: %s", outputPath.c_str());
    mz_zip_reader_end(&srcZip);
    srcFile.close();
    Progress p;
    p.failed = true;
    snprintf(p.statusMsg, sizeof(p.statusMsg), "Cannot create output file");
    setProgress(p);
    return;
  }

  mz_zip_archive dstZip;
  memset(&dstZip, 0, sizeof(dstZip));
  dstZip.m_pWrite = zipWriteFunc;
  dstZip.m_pRead = zipReadFunc;  // needed for some internal operations
  dstZip.m_pIO_opaque = &dstFile;

  if (!mz_zip_writer_init(&dstZip, 0)) {
    LOG_ERR("ET", "Cannot init ZIP writer");
    mz_zip_reader_end(&srcZip);
    srcFile.close();
    dstFile.close();
    Storage.remove(outputPath.c_str());
    Progress p;
    p.failed = true;
    snprintf(p.statusMsg, sizeof(p.statusMsg), "Cannot init ZIP writer");
    setProgress(p);
    return;
  }

  // ── 4. Count HTML chapters for progress reporting ──────────────────────────
  int numFiles = static_cast<int>(mz_zip_reader_get_num_files(&srcZip));
  int totalChapters = 0;
  for (int i = 0; i < numFiles; i++) {
    mz_zip_archive_file_stat stat;
    if (mz_zip_reader_file_stat(&srcZip, i, &stat) && isHtmlEntry(stat.m_filename)) totalChapters++;
  }
  LOG_DBG("ET", "HTML chapters to translate: %d", totalChapters);

  // ── 5. Iterate ZIP entries ──────────────────────────────────────────────────
  int currentChapter = 0;

  // Add mimetype first (EPUB spec: must be first + uncompressed)
  int mimetypeIdx = mz_zip_reader_locate_file(&srcZip, "mimetype", nullptr, 0);
  if (mimetypeIdx >= 0) {
    mz_zip_writer_add_from_zip_reader(&dstZip, &srcZip, mimetypeIdx);
  }

  for (int i = 0; i < numFiles; i++) {
    if (cancelRequested) {
      LOG_DBG("ET", "Translation cancelled at file %d", i);
      break;
    }

    if (i == mimetypeIdx) continue;  // already added

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&srcZip, i, &stat)) continue;

    const bool isDir = mz_zip_reader_is_file_a_directory(&srcZip, i);
    if (isDir) {
      mz_zip_writer_add_from_zip_reader(&dstZip, &srcZip, i);
      continue;
    }

    const char* filename = stat.m_filename;

    // Check if this is the OPF file (ends with .opf)
    const char* dot = strrchr(filename, '.');
    const bool isOpf = dot && strcmp(dot, ".opf") == 0;

    if (isOpf) {
      // Patch OPF: update title to include language tag
      size_t extractSize = 0;
      void* data = mz_zip_reader_extract_to_heap(&srcZip, i, &extractSize, 0);
      if (data) {
        std::string patched = patchOpf(static_cast<const char*>(data), extractSize, targetLang);
        mz_free(data);
        mz_zip_writer_add_mem(&dstZip, filename, patched.c_str(), patched.size(), MZ_BEST_SPEED);
      } else {
        mz_zip_writer_add_from_zip_reader(&dstZip, &srcZip, i);
      }
    } else if (isHtmlEntry(filename)) {
      currentChapter++;
      {
        Progress p;
        p.currentChapter = currentChapter;
        p.totalChapters = totalChapters;
        snprintf(p.statusMsg, sizeof(p.statusMsg), "Chapter %d / %d", currentChapter, totalChapters);
        setProgress(p);
      }
      LOG_DBG("ET", "Translating chapter %d: %s", currentChapter, filename);

      size_t htmlSize = 0;
      void* htmlData = mz_zip_reader_extract_to_heap(&srcZip, i, &htmlSize, 0);
      if (!htmlData || htmlSize > 60 * 1024) {
        // Chapter too large for RAM processing — copy as-is
        LOG_DBG("ET", "Chapter %s too large (%u bytes), copying without translation", filename, (unsigned)htmlSize);
        if (htmlData) mz_free(htmlData);
        mz_zip_writer_add_from_zip_reader(&dstZip, &srcZip, i);
        continue;
      }

      StringPrint resultBuf;
      TranslatingHtmlRewriter rewriter;
      auto res = rewriter.rewrite(static_cast<const char*>(htmlData), htmlSize, resultBuf, targetLang,
                                  &cancelRequested);
      mz_free(htmlData);

      LOG_DBG("ET", "Chapter done: %d translated, %d skipped", res.paragraphsTranslated, res.paragraphsSkipped);

      if (res.cancelled) {
        LOG_DBG("ET", "Chapter cancelled");
        break;
      }

      const std::string& outHtml = resultBuf.str();
      if (outHtml.empty()) {
        mz_zip_writer_add_from_zip_reader(&dstZip, &srcZip, i);
      } else {
        mz_zip_writer_add_mem(&dstZip, filename, outHtml.c_str(), outHtml.size(), MZ_BEST_SPEED);
      }
    } else {
      // Non-chapter asset: copy directly (images, CSS, fonts, etc.)
      mz_zip_writer_add_from_zip_reader(&dstZip, &srcZip, i);
    }
  }

  // ── 5. Finalize ─────────────────────────────────────────────────────────────
  bool finalizeOk = false;
  if (!cancelRequested) {
    finalizeOk = mz_zip_writer_finalize_archive(&dstZip);
    if (!finalizeOk) LOG_ERR("ET", "Failed to finalize ZIP archive");
  }
  mz_zip_writer_end(&dstZip);
  mz_zip_reader_end(&srcZip);
  srcFile.close();
  dstFile.flush();
  dstFile.close();

  if (cancelRequested || !finalizeOk) {
    Storage.remove(outputPath.c_str());
  }

  Progress p;
  p.currentChapter = currentChapter;
  p.totalChapters = totalChapters;
  if (cancelRequested) {
    p.cancelled = true;
    snprintf(p.statusMsg, sizeof(p.statusMsg), "Cancelled");
  } else if (!finalizeOk) {
    p.failed = true;
    snprintf(p.statusMsg, sizeof(p.statusMsg), "Failed to write output");
  } else {
    p.done = true;
    snprintf(p.statusMsg, sizeof(p.statusMsg), "Done");
  }
  setProgress(p);
  LOG_DBG("ET", "Translation task complete. done=%d cancelled=%d failed=%d", p.done, p.cancelled, p.failed);
}
