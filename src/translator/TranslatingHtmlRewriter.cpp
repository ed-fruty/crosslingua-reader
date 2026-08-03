#include "TranslatingHtmlRewriter.h"

#include <Epub/htmlEntities.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "ParagraphTranslator.h"
#include "network/HttpDownloader.h"

static constexpr size_t PARSE_CHUNK = 1024;

const char* TranslatingHtmlRewriter::BLOCK_TAGS[] = {"p", "h1", "h2", "h3", "h4", "h5", "h6", "li", "blockquote"};
const int TranslatingHtmlRewriter::NUM_BLOCK_TAGS = static_cast<int>(sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]));

bool TranslatingHtmlRewriter::isBlockTag(const char* name) {
  for (int i = 0; i < NUM_BLOCK_TAGS; i++) {
    if (strcmp(name, BLOCK_TAGS[i]) == 0) return true;
  }
  return false;
}

void TranslatingHtmlRewriter::appendEscaped(const char* s, size_t len, std::string& buf) {
  for (size_t i = 0; i < len; i++) {
    char c = s[i];
    if (c == '&')
      buf += "&amp;";
    else if (c == '<')
      buf += "&lt;";
    else if (c == '>')
      buf += "&gt;";
    else
      buf += c;
  }
}

void TranslatingHtmlRewriter::writeOut(const char* s, size_t len) { pendingHtml.append(s, len); }

void TranslatingHtmlRewriter::writeOut(const std::string& s) {
  if (!s.empty()) pendingHtml.append(s);
}

void TranslatingHtmlRewriter::writeRaw(const char* s, size_t len) {
  out->write(reinterpret_cast<const uint8_t*>(s), len);
}

void TranslatingHtmlRewriter::writeRaw(const std::string& s) {
  if (!s.empty()) writeRaw(s.c_str(), s.size());
}

std::vector<std::string> TranslatingHtmlRewriter::splitByDoubleLF(const std::string& s) {
  std::vector<std::string> parts;
  size_t start = 0;
  // N separators always yield N+1 pieces, including a trailing EMPTY piece when the
  // string ends with a separator. The old `while (start < s.size())` form dropped that
  // last piece, so a batch whose final paragraph translated to "" came back one piece
  // short — harmless while the write-out loop tolerated a short reply, but the exact
  // count check in flushBatch() now depends on the piece count faithfully reflecting
  // the separator count, so it would have sunk every such batch.
  while (true) {
    size_t pos = s.find("\n\n", start);
    if (pos == std::string::npos) {
      parts.push_back(s.substr(start));
      break;
    }
    parts.push_back(s.substr(start, pos - start));
    start = pos + 2;
  }
  return parts;
}

bool TranslatingHtmlRewriter::shouldRetryAfterFailure(int httpCode) {
  // Auth failures never recover on retry — abort the whole chapter immediately.
  if (httpCode == 401 || httpCode == 403) {
    LOG_ERR("HtmlRW", "Aborting: auth error %d", httpCode);
    abortedOnErrors = true;
    return false;
  }
  // Rate limiting: retry, but trip a fast abort after too many in a row.
  if (httpCode == 429) {
    consecutive429++;
    if (consecutive429 >= enginePolicy->maxConsecutiveRateLimits) {
      LOG_ERR("HtmlRW", "Aborting: %d consecutive 429s", consecutive429);
      abortedOnErrors = true;
      return false;
    }
    return true;
  }
  consecutive429 = 0;
  // Permanent client errors: no point retrying, but not fatal to the rest of the chapter.
  if (httpCode == 400 || httpCode == 404) {
    return false;
  }
  // Transient: 5xx, 0 (no response), negative (connect/DNS/timeout), or unclassified.
  return true;
}

std::string TranslatingHtmlRewriter::makeOpenTag(const XML_Char* name, const XML_Char** atts) {
  std::string tag = "<";
  tag += name;
  if (atts) {
    for (int i = 0; atts[i]; i += 2) {
      tag += ' ';
      tag += atts[i];  // attribute name
      tag += "=\"";
      for (const char* v = atts[i + 1]; *v; v++) {
        if (*v == '"')
          tag += "&quot;";
        else if (*v == '&')
          tag += "&amp;";
        else if (*v == '<')
          tag += "&lt;";
        else
          tag += *v;
      }
      tag += '"';
    }
  }
  tag += '>';
  return tag;
}

void TranslatingHtmlRewriter::flushBlock(const char* endTagName) {
  // Append closing tag to pendingHtml via writeOut (buffered)
  writeOut(blockHtml);
  writeOut("</", 2);
  writeOut(endTagName, strlen(endTagName));
  writeOut(">\n", 2);

  // Trim block text for translation, and collapse every internal newline run to a
  // single '\n'.
  //
  // The collapse is load-bearing, not cosmetics: "\n\n" is the separator flushBatch()
  // uses to merge paragraphs into one request and to split the reply back apart, and
  // the reply is written back POSITIONALLY. blockText is raw XML character data
  // (onChars appends it verbatim), so a single block can easily contain "\n\n" of its
  // own — a blank line in the pretty-printed XHTML source, or simply
  // `<p>line\n<br/>\nline</p>`, where the two onChars callbacks around the <br/>
  // concatenate to "line\n" + "\nline". Trimming only the ENDS left that inside the
  // text, so one such paragraph made splitByDoubleLF() produce more pieces than
  // paragraphs sent and shifted every later translation in the batch by one.
  //
  // Fixed here, at the single point where trimmedText is produced, rather than in
  // flushBatch(): this is the one string that feeds the batch merge, the individual
  // (non-merged) calls, and the "translation == original" skip comparison, so the
  // invariant "no trimmedText ever contains \n\n" holds for every engine and every
  // path at once. It cannot regress the other engines — the only change they see is a
  // paragraph-internal blank line arriving as a single line break, which removes a
  // FALSE paragraph separator that the batch prompt explicitly tells the model to
  // preserve (and which also made translateOpenAICompat's `text.find("\n\n")` treat a
  // lone paragraph as a batch).
  const std::string trimmed = [&] {
    const size_t s = blockText.find_first_not_of(" \t\n\r");
    if (s == std::string::npos) return std::string{};
    const size_t e = blockText.find_last_not_of(" \t\n\r");
    std::string collapsed;
    collapsed.reserve(e - s + 1);  // one allocation; the result can only shrink
    for (size_t i = s; i <= e; i++) {
      const char c = blockText[i];
      if (c != '\n' && c != '\r') {
        collapsed += c;
        continue;
      }
      // Start of a line-break run: emit exactly one '\n' and swallow the rest of the
      // run (further newlines plus any indentation whitespace between them).
      collapsed += '\n';
      while (i + 1 <= e) {
        const char next = blockText[i + 1];
        if (next != '\n' && next != '\r' && next != ' ' && next != '\t') break;
        i++;
      }
    }
    return collapsed;
  }();

  // Native-array providers must stay on their native batch path. Flush before
  // adding an item that would exceed the policy boundary; otherwise the generic
  // oversized-batch fallback would turn one array request into a burst of
  // request-per-paragraph calls.
  if (enginePolicy->requiresBoundedBatch() && !trimmed.empty() &&
      trimmed.size() <= enginePolicy->maxMergedTextBytes && batchTextBytes > 0) {
    constexpr size_t separatorBytes = 2;
    if (batchTextBytes + separatorBytes + trimmed.size() > enginePolicy->batchTargetBytes) {
      flushBatch();
    }
  }

  // Create batch entry: move pendingHtml into htmlBefore, store trimmedText
  BatchEntry entry;
  entry.htmlBefore = std::move(pendingHtml);
  entry.blockTagName = blockTagName;
  entry.blockClass = std::move(blockClass);
  pendingHtml.clear();
  if (!trimmed.empty() && trimmed.size() <= ParagraphTranslator::MAX_TEXT_BYTES) {
    entry.trimmedText = trimmed;
    batchTextBytes += trimmed.size();
  }
  batch.push_back(std::move(entry));

  LOG_DBG("HtmlRW", "Block <%s> text=%u bytes, batch=%u entries, batchBytes=%u", endTagName, (unsigned)blockText.size(),
          (unsigned)batch.size(), (unsigned)batchTextBytes);

  // Flush batch if we've accumulated enough text
  if (batchTextBytes >= enginePolicy->batchTargetBytes) {
    flushBatch();
  }

  blockHtml.clear();
  blockText.clear();
  blockDepth = -1;
}

void TranslatingHtmlRewriter::flushBatch() {
  if (batch.empty()) return;

  // If cancelled or aborted due to errors, write all HTML without translations
  if (wasCancelled || abortedOnErrors || (cancelled && *cancelled)) {
    for (auto& entry : batch) {
      writeRaw(entry.htmlBefore);
      if (!entry.trimmedText.empty()) paragraphsSkipped++;
      blocksProcessed++;
      if (progressOut) *progressOut = blocksProcessed;
    }
    batch.clear();
    batchTextBytes = 0;
    return;
  }

  // Collect indices of translatable entries and build merged text
  std::vector<size_t> translatableIndices;
  std::string mergedText;
  for (size_t i = 0; i < batch.size(); i++) {
    if (!batch[i].trimmedText.empty()) {
      if (!mergedText.empty()) mergedText += "\n\n";
      mergedText += batch[i].trimmedText;
      translatableIndices.push_back(i);
    }
  }

  // Translate the merged text.
  // Batch merging needs an engine that takes N "\n\n"-separated parts and gives N back:
  //  - the LLM engines are *asked* to preserve the separators (best-effort, model-dependent);
  //  - Azure splits the merged text into a native array of text items and gets one result
  //    object per item in input order, which is an API guarantee rather than a request.
  const bool canBatchMerge = enginePolicy->supportsBatching();
  std::vector<std::string> translations;
  if (!translatableIndices.empty()) {
    // ── Heap backpressure ──────────────────────────────────────────────────
    // Wait (bounded) for the heap to support this batch's TLS request before
    // firing it. If it never recovers within the window we do NOT allocate (the
    // design directive: "if memory is insufficient, do not try to allocate; leave
    // it for the next loop iteration"). We count the exhausted wait and, after
    // MAX_CONSECUTIVE_HEAP_TIMEOUTS in a row, abort the run cleanly (low-memory).
    // A healthy wait resets the streak so a run that recovers keeps going.
    const bool heapReady = !httpSession || httpSession->waitForHeapReady(HEAP_WAIT_TIMEOUT_MS, cancelled);
    if (!heapReady) {
      if (cancelled && *cancelled) {
        wasCancelled = true;  // wait was broken by a user cancel, not a timeout
      } else {
        consecutiveHeapTimeouts++;
        // Count as genuine failures (not silent skips) so a chapter that translated
        // nothing because of low memory is never committed as a valid passthrough
        // (the activity's zero-translated-with-failures guard catches it).
        translateFailures += static_cast<int>(translatableIndices.size());
        LOG_INF("HtmlRW", "Heap backpressure: batch of %u paragraphs deferred, low memory (%d/%d consecutive)",
                (unsigned)translatableIndices.size(), consecutiveHeapTimeouts, MAX_CONSECUTIVE_HEAP_TIMEOUTS);
        if (consecutiveHeapTimeouts >= MAX_CONSECUTIVE_HEAP_TIMEOUTS) {
          LOG_ERR("HtmlRW", "Aborting: %d consecutive low-memory waits", consecutiveHeapTimeouts);
          abortedOnErrors = true;
          abortedLowMemory = true;
        }
      }
      // Fall through untranslated: `translations` stays empty, so the write-out
      // loop below emits each entry's original HTML only.
    } else if (canBatchMerge && mergedText.size() <= enginePolicy->maxMergedTextBytes) {
      consecutiveHeapTimeouts = 0;  // heap healthy -> reset the low-memory streak
      // Single batched API call (LLM/DeepL engines that can handle merged text)
      std::string translated;
      bool ok = false;
      for (int attempt = 0; attempt < enginePolicy->maxAttempts && !ok; attempt++) {
        if (attempt > 0) delay(enginePolicy->retryDelayMs(HttpDownloader::lastHttpCode, attempt - 1));
        ok = ParagraphTranslator::translate(mergedText, sourceLang, targetLang, engine, apiKey, translated, &lastError,
                                            httpSession);
        if (ok) {
          consecutive429 = 0;
        } else if (!shouldRetryAfterFailure(HttpDownloader::lastHttpCode)) {
          break;
        }
      }
      bool batchUsable = ok && !translated.empty();
      if (batchUsable) {
        translations = splitByDoubleLF(translated);
        // Exactly-N or drop the batch. The write-out loop below is POSITIONAL, so a
        // reply that splits into a different number of pieces than we sent does not
        // just lose one paragraph — every piece after the discrepancy lands on the
        // WRONG paragraph, and that corruption is written straight into the book's
        // XHTML where the reader cannot tell it is wrong. An untranslated paragraph is
        // visibly untranslated and can be retried, so it is the strictly safer failure.
        // (This is the same rule parseAzureResponse already enforces on the JSON side,
        // now applied uniformly to every batching engine.) Checked here rather than in
        // the write-out loop so the individual-call path — which fills `translations`
        // itself and may legitimately stop short on cancel — is unaffected.
        if (translations.size() != translatableIndices.size()) {
          LOG_ERR("HtmlRW", "Batch reply split into %u pieces for %u paragraphs; dropping batch",
                  (unsigned)translations.size(), (unsigned)translatableIndices.size());
          translations.clear();
          batchUsable = false;
        }
      }
      if (batchUsable) {
        consecutiveFailures = 0;
        LOG_DBG("HtmlRW", "Batch: sent %u paragraphs, got %u back, response %.120s",
                (unsigned)translatableIndices.size(), (unsigned)translations.size(), translated.c_str());
      } else {
        translateFailures += static_cast<int>(translatableIndices.size());
        if (!abortedOnErrors) {
          consecutiveFailures++;
          LOG_ERR("HtmlRW", "Batch translate failed (%d consecutive)", consecutiveFailures);
          if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
            LOG_ERR("HtmlRW", "Aborting: %d consecutive failures", consecutiveFailures);
            abortedOnErrors = true;
          }
        }
      }
    } else {
      consecutiveHeapTimeouts = 0;  // heap healthy -> reset the low-memory streak
      // Individual calls — either engine doesn't support batch merging, or merged text too large
      LOG_DBG("HtmlRW", "Individual calls: %u paragraphs (canBatch=%d, mergedBytes=%u)",
              (unsigned)translatableIndices.size(), canBatchMerge, (unsigned)mergedText.size());
      for (size_t i = 0; i < translatableIndices.size(); i++) {
        if (wasCancelled || abortedOnErrors || (cancelled && *cancelled)) break;
        std::string translated;
        bool ok = false;
        for (int attempt = 0; attempt < enginePolicy->maxAttempts && !ok; attempt++) {
          if (attempt > 0) delay(enginePolicy->retryDelayMs(HttpDownloader::lastHttpCode, attempt - 1));
          ok = ParagraphTranslator::translate(batch[translatableIndices[i]].trimmedText, sourceLang, targetLang, engine,
                                              apiKey, translated, &lastError, httpSession);
          if (ok) {
            consecutive429 = 0;
          } else if (!shouldRetryAfterFailure(HttpDownloader::lastHttpCode)) {
            break;
          }
        }
        if (ok) {
          consecutiveFailures = 0;
        } else {
          translateFailures++;
          if (!abortedOnErrors) {
            consecutiveFailures++;
            LOG_ERR("HtmlRW", "Translate failed (%d consecutive)", consecutiveFailures);
            if (consecutiveFailures >= MAX_CONSECUTIVE_FAILURES) {
              LOG_ERR("HtmlRW", "Aborting: %d consecutive failures", consecutiveFailures);
              abortedOnErrors = true;
            }
          }
        }
        translations.push_back(ok ? std::move(translated) : std::string{});
        // Optimistic progress + boundary check per paragraph: blocksProcessed only advances
        // in the write-out phase below (which overwrites this with the accurate count —
        // always >= this optimistic value, so the bar stays monotonic). Without this the
        // first repaint waits a whole batch (~30 s of silence on slow engines).
        if (progressOut) *progressOut = blocksProcessed + static_cast<int>(i) + 1;
        if (onBatchBoundary) onBatchBoundary(batchBoundaryCtx);
        if (i + 1 < translatableIndices.size()) {
          delay(ok ? 100 : 2000);  // longer delay after error to let heap recover
        }
      }
    }
  } else {
    // A batch with nothing to translate issues no network request, so it says nothing
    // about the heap: only genuinely back-to-back exhausted TLS waits may count toward
    // the low-memory abort.
    consecutiveHeapTimeouts = 0;
  }

  // Write all entries to output, interleaving translations
  size_t tIdx = 0;
  for (size_t i = 0; i < batch.size(); i++) {
    writeRaw(batch[i].htmlBefore);

    if (!batch[i].trimmedText.empty()) {
      // Positional pickup. `translations` now holds either exactly one entry per
      // translatable paragraph or fewer (a dropped batch leaves it empty; the
      // individual path stops short on cancel/abort) — never more, since the batch
      // path above rejects a mismatched split outright. A missing entry simply leaves
      // the paragraph untranslated. The old "merge the excess into the last
      // paragraph" salvage is gone with it: it papered over exactly the split
      // mismatch that now fails the batch, while silently misattributing every piece
      // in between whenever the extra separator came from the MIDDLE of the reply.
      std::string thisTranslation;
      if (tIdx < translations.size()) {
        thisTranslation = std::move(translations[tIdx]);
      }
      tIdx++;

      if (!thisTranslation.empty() && thisTranslation != batch[i].trimmedText) {
        // Use original block's tag name and class so translated paragraph inherits same CSS spacing
        const auto& tag = batch[i].blockTagName.empty() ? "p" : batch[i].blockTagName.c_str();
        writeRaw("<", 1);
        writeRaw(tag, strlen(tag));
        if (!batch[i].blockClass.empty()) {
          writeRaw(" class=\"", 8);
          writeRaw(batch[i].blockClass);
          writeRaw("\"", 1);
        }
        static constexpr char kLangAttr[] = " lang=\"";
        static constexpr char kTagAttrs[] = "\" data-translation=\"true\" dir=\"auto\">";
        writeRaw(kLangAttr, sizeof(kLangAttr) - 1);
        writeRaw(targetLang, strlen(targetLang));
        writeRaw(kTagAttrs, sizeof(kTagAttrs) - 1);
        std::string escaped;
        appendEscaped(thisTranslation.c_str(), thisTranslation.size(), escaped);
        writeRaw(escaped);
        writeRaw("</", 2);
        writeRaw(tag, strlen(tag));
        writeRaw(">\n", 2);
        paragraphsTranslated++;
      } else {
        paragraphsSkipped++;
      }
    }

    blocksProcessed++;
    if (progressOut) *progressOut = blocksProcessed;
  }

  delay(consecutiveFailures > 0 ? 2000 : 100);  // longer delay after errors to let heap recover
  batch.clear();
  batchTextBytes = 0;

  // Azure's bearer token expires mid-chapter on a long one. Renew it HERE, before the
  // caller's repaint: this batch's HTTP/TLS transients are already freed and the
  // framebuffer is still released, whereas the repaint hook below hands off to the main
  // task, which restores the 48 KB framebuffer to draw. This is the cleanest heap of the
  // whole run, and the static GET the token fetch uses has to stand up its own TLS
  // context alongside the chapter's live session. No-op unless the token is close to
  // expiring; a refusal keeps the current token and translateAzure() still refreshes
  // lazily as a fallback. Skipped for a run that is already ending — a failure inside
  // THIS batch can have set abortedOnErrors after the fast-path check at the top, and
  // that run will not issue another request.
  if (engine == CrossPointSettings::ENGINE_AZURE && !abortedOnErrors && !wasCancelled && !(cancelled && *cancelled)) {
    ParagraphTranslator::refreshAzureTokenIfExpiring();
  }

  // Between-batch boundary: this batch's HTTP/TLS transients are freed and progressOut
  // reflects the blocks just written, so the heap has a clean hole. Let the caller
  // repaint here if it wants to (ChapterTranslator uses this for periodic progress
  // updates during a single chapter). No-op when no callback was registered. Skipped on
  // the cancelled/aborted fast-path above, which returns before reaching here.
  if (onBatchBoundary) onBatchBoundary(batchBoundaryCtx);
}

void XMLCALL TranslatingHtmlRewriter::onStart(void* ud, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<TranslatingHtmlRewriter*>(ud);
  if (self->cancelled && *self->cancelled) {
    self->wasCancelled = true;
    return;
  }

  if (strcmp(name, "head") == 0) self->insideHead = true;

  const std::string tag = makeOpenTag(name, atts);

  if (self->skipBlockDepth != -1) {
    // Inside a block being skipped (existing translation): ignore everything
  } else if (self->blockDepth != -1) {
    // Inside a block element: accumulate
    self->blockHtml += tag;
    // Also track plain text for inner inline tags (no text here, just tag)
  } else if (!self->insideHead && isBlockTag(name)) {
    // Check for existing translation (Calibre or CrossPoint) — block element with `lang` attribute
    bool hasLangAttr = false;
    if (atts) {
      for (int i = 0; atts[i]; i += 2) {
        if (strcmp(atts[i], "lang") == 0 || strcmp(atts[i], "xml:lang") == 0) {
          hasLangAttr = true;
          break;
        }
      }
    }
    if (hasLangAttr) {
      // Skip this block entirely — it's an existing translation paragraph
      self->skipBlockDepth = self->depth;
      self->paragraphsSkipped++;
    } else {
      // Start of a new block element
      self->blockDepth = self->depth;
      self->blockHtml = tag;
      self->blockText.clear();
      self->blockTagName = name;
      self->blockClass.clear();
      if (atts) {
        for (int i = 0; atts[i]; i += 2) {
          if (strcmp(atts[i], "class") == 0) {
            self->blockClass = atts[i + 1];
            break;
          }
        }
      }
    }
  } else {
    // Structural / head element: pass through directly
    self->writeOut(tag);
  }

  self->depth++;
}

void XMLCALL TranslatingHtmlRewriter::onEnd(void* ud, const XML_Char* name) {
  auto* self = static_cast<TranslatingHtmlRewriter*>(ud);
  self->depth--;

  if (self->skipBlockDepth == self->depth) {
    // Done skipping an existing translation block
    self->skipBlockDepth = -1;
  } else if (self->skipBlockDepth != -1) {
    // Still inside a skipped block — ignore
  } else if (self->blockDepth == self->depth) {
    // Closing the outermost block element we're tracking
    if (self->cancelled && *self->cancelled) {
      self->wasCancelled = true;
      self->blockHtml.clear();
      self->blockText.clear();
      self->blockDepth = -1;
    } else {
      self->flushBlock(name);
    }
  } else if (self->blockDepth != -1) {
    // Closing an inner element within a block
    self->blockHtml += "</";
    self->blockHtml += name;
    self->blockHtml += '>';
  } else {
    // Structural close: pass through
    if (strcmp(name, "head") == 0) self->insideHead = false;
    self->writeOut("</", 2);
    self->writeOut(name, strlen(name));
    self->writeOut(">", 1);
    if (strcmp(name, "html") == 0) self->writeOut("\n", 1);
  }
}

void XMLCALL TranslatingHtmlRewriter::onChars(void* ud, const XML_Char* s, int len) {
  auto* self = static_cast<TranslatingHtmlRewriter*>(ud);
  if (self->skipBlockDepth != -1) return;  // inside a skipped translation block
  if (self->blockDepth != -1) {
    // Inside a block: add to both HTML output and plain text
    appendEscaped(s, static_cast<size_t>(len), self->blockHtml);
    self->blockText.append(s, static_cast<size_t>(len));
  } else if (!self->insideHead) {
    // Outside blocks (e.g. whitespace between tags): pass through escaped
    std::string escaped;
    appendEscaped(s, static_cast<size_t>(len), escaped);
    self->writeOut(escaped);
  }
}

void XMLCALL TranslatingHtmlRewriter::onDefault(void* ud, const XML_Char* s, int len) {
  auto* self = static_cast<TranslatingHtmlRewriter*>(ud);
  if (self->skipBlockDepth != -1) return;  // inside a skipped translation block
  // Handle HTML entities and pass through XML declarations / DOCTYPE
  if (len >= 2 && s[0] == '&' && s[len - 1] == ';') {
    // Entity reference — pass through as-is (expat already tried to expand)
    if (self->blockDepth != -1) {
      self->blockHtml.append(s, static_cast<size_t>(len));
      // For plain text sent to the translation engine, expand the entity to its
      // real UTF-8 value so punctuation like &mdash;/&hellip;/&laquo; survives
      // translation instead of being blanked to a space.
      const char* utf8Value = lookupHtmlEntity(s, static_cast<size_t>(len));
      if (utf8Value != nullptr) {
        self->blockText += utf8Value;
      } else {
        // Unknown entity name: fall back to a single space (previous behavior).
        self->blockText += ' ';
      }
    } else if (!self->insideHead) {
      self->writeOut(s, static_cast<size_t>(len));
    }
    return;
  }
  // Pass through everything else (XML declaration, DOCTYPE, PIs) verbatim
  if (self->blockDepth == -1) {
    self->writeOut(s, static_cast<size_t>(len));
  }
}

// ─── Block counting (for progress bar) ──────────────────────────────────────

void XMLCALL TranslatingHtmlRewriter::onStartCount(void* ud, const XML_Char* name, const XML_Char** atts) {
  (void)atts;
  auto* count = static_cast<int*>(ud);
  if (isBlockTag(name)) {
    (*count)++;
  }
}

int TranslatingHtmlRewriter::countBlocksInFile(const std::string& inputPath) {
  HalFile inputFile;
  if (!Storage.openFileForRead("HtmlRW", inputPath, inputFile)) {
    LOG_ERR("HtmlRW", "countBlocks: failed to open %s", inputPath.c_str());
    return 0;
  }

  const size_t fileSize = inputFile.size();

  XML_Parser parser = XML_ParserCreate("UTF-8");
  if (!parser) {
    inputFile.close();
    return 0;
  }

  int blockCount = 0;
  XML_SetUserData(parser, &blockCount);
  XML_SetStartElementHandler(parser, onStartCount);

  char buf[PARSE_CHUNK];
  size_t totalRead = 0;
  while (totalRead < fileSize) {
    const size_t toRead = ((fileSize - totalRead) < PARSE_CHUNK) ? (fileSize - totalRead) : PARSE_CHUNK;
    const int bytesRead = inputFile.read(reinterpret_cast<uint8_t*>(buf), toRead);
    if (bytesRead <= 0) break;
    totalRead += bytesRead;
    const bool done = (totalRead >= fileSize);
    if (XML_Parse(parser, buf, bytesRead, done ? 1 : 0) == XML_STATUS_ERROR) break;
  }

  XML_ParserFree(parser);
  inputFile.close();

  LOG_DBG("HtmlRW", "Pre-scan: %d translatable blocks in %s", blockCount, inputPath.c_str());
  return blockCount;
}

// ─── rewrite (buffer) ───────────────────────────────────────────────────────

TranslatingHtmlRewriter::Result TranslatingHtmlRewriter::rewrite(const char* inputBuf, size_t inputSize,
                                                                 Print& outPrint, const char* srcLang,
                                                                 const char* tgtLang, uint8_t eng, const char* key,
                                                                 volatile const bool* cancelFlag,
                                                                 volatile int* progress) {
  out = &outPrint;
  sourceLang = srcLang;
  targetLang = tgtLang;
  engine = eng;
  enginePolicy = &translationEnginePolicy(eng);
  apiKey = key;
  cancelled = cancelFlag;
  progressOut = progress;
  onBatchBoundary = nullptr;  // buffer path has no between-batch repaint hook
  batchBoundaryCtx = nullptr;
  httpSession = nullptr;  // buffer path keeps the stateless per-call HTTP behavior
  depth = 0;
  blockDepth = -1;
  insideHead = false;
  blockHtml.clear();
  blockText.clear();
  blockTagName.clear();
  blockClass.clear();
  skipBlockDepth = -1;
  paragraphsTranslated = 0;
  paragraphsSkipped = 0;
  translateFailures = 0;
  wasCancelled = false;
  consecutiveFailures = 0;
  consecutive429 = 0;
  abortedOnErrors = false;
  consecutiveHeapTimeouts = 0;
  abortedLowMemory = false;
  lastError.clear();
  batch.clear();
  pendingHtml.clear();
  batchTextBytes = 0;

  XML_Parser parser = XML_ParserCreate("UTF-8");
  if (!parser) {
    LOG_ERR("HtmlRW", "Failed to create expat parser");
    return {0, 0, 0, false, false};
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, onStart, onEnd);
  XML_SetCharacterDataHandler(parser, onChars);
  XML_SetDefaultHandlerExpand(parser, onDefault);

  size_t offset = 0;
  bool ok = true;
  while (offset < inputSize && !wasCancelled && !abortedOnErrors) {
    size_t chunk = (inputSize - offset < PARSE_CHUNK) ? (inputSize - offset) : PARSE_CHUNK;
    bool done = (offset + chunk >= inputSize);

    if (XML_Parse(parser, inputBuf + offset, static_cast<int>(chunk), done ? 1 : 0) == XML_STATUS_ERROR) {
      LOG_ERR("HtmlRW", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      ok = false;
      break;
    }
    offset += chunk;
  }

  XML_ParserFree(parser);

  // Flush remaining batch entries (flushBatch handles cancellation internally —
  // writes HTML without translations when cancelled) and any trailing buffered HTML
  flushBatch();
  writeRaw(pendingHtml);
  pendingHtml.clear();

  Result res;
  res.paragraphsTranslated = paragraphsTranslated;
  res.paragraphsSkipped = paragraphsSkipped;
  res.translateFailures = translateFailures;
  res.cancelled = wasCancelled || (cancelled && *cancelled);
  res.abortedOnErrors = abortedOnErrors;
  res.abortedLowMemory = abortedLowMemory;
  if (abortedOnErrors && !lastError.empty()) {
    snprintf(res.errorDetail, sizeof(res.errorDetail), "%s", lastError.c_str());
  }
  return res;
}

// ─── rewriteFromFile ────────────────────────────────────────────────────────

TranslatingHtmlRewriter::Result TranslatingHtmlRewriter::rewriteFromFile(
    const std::string& inputPath, Print& outPrint, const char* srcLang, const char* tgtLang, uint8_t eng,
    const char* key, volatile const bool* cancelFlag, volatile int* progress, BatchBoundaryCb boundaryCb,
    void* boundaryCtx) {
  out = &outPrint;
  sourceLang = srcLang;
  targetLang = tgtLang;
  engine = eng;
  enginePolicy = &translationEnginePolicy(eng);
  apiKey = key;
  cancelled = cancelFlag;
  progressOut = progress;
  onBatchBoundary = boundaryCb;
  batchBoundaryCtx = boundaryCtx;
  // Azure authenticates with a bearer token fetched from a DIFFERENT host than the
  // translate endpoint. Prime it here, while no session exists yet, so that (a) the
  // token handshake peaks on its own instead of on top of a live TLS context, and
  // (b) it cannot mark the session "everConnected" and thereby downgrade the heap
  // floor guarding the session's own first (real) handshake. The token is cached for
  // 8 minutes, which covers a typical chapter end to end. A failure here is ignored:
  // the first translate() retries the fetch and the normal retry/backoff path applies.
  if (engine == CrossPointSettings::ENGINE_AZURE) {
    ParagraphTranslator::primeAzureToken();
  }
  // One reusable keep-alive connection for the whole chapter: the first translate()
  // call performs the TLS handshake, every later one reuses the socket (see
  // TranslationHttpSession). Declared here so it outlives the final flushBatch()
  // below, and its destructor closes the socket before this function returns —
  // freeing the TLS buffers ahead of the caller's post-run work (rename, framebuffer
  // restore). If it could not be set up (OOM / non-wolfSSL build) it transparently
  // falls back to per-request connections, so httpSession is always safe to use.
  TranslationHttpSession session;
  httpSession = &session;
  depth = 0;
  blockDepth = -1;
  insideHead = false;
  blockHtml.clear();
  blockText.clear();
  blockTagName.clear();
  blockClass.clear();
  skipBlockDepth = -1;
  paragraphsTranslated = 0;
  paragraphsSkipped = 0;
  translateFailures = 0;
  wasCancelled = false;
  consecutiveFailures = 0;
  consecutive429 = 0;
  abortedOnErrors = false;
  consecutiveHeapTimeouts = 0;
  abortedLowMemory = false;
  lastError.clear();
  batch.clear();
  pendingHtml.clear();
  batchTextBytes = 0;

  HalFile inputFile;
  if (!Storage.openFileForRead("HtmlRW", inputPath, inputFile)) {
    LOG_ERR("HtmlRW", "Failed to open input file: %s", inputPath.c_str());
    httpSession = nullptr;
    return {0, 0, 0, false, false};
  }

  const size_t fileSize = inputFile.size();

  XML_Parser parser = XML_ParserCreate("UTF-8");
  if (!parser) {
    LOG_ERR("HtmlRW", "Failed to create expat parser");
    inputFile.close();
    httpSession = nullptr;
    return {0, 0, 0, false, false};
  }

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, onStart, onEnd);
  XML_SetCharacterDataHandler(parser, onChars);
  XML_SetDefaultHandlerExpand(parser, onDefault);

  char buf[PARSE_CHUNK];
  size_t totalRead = 0;
  bool ok = true;
  while (totalRead < fileSize && !wasCancelled && !abortedOnErrors) {
    const size_t toRead = ((fileSize - totalRead) < PARSE_CHUNK) ? (fileSize - totalRead) : PARSE_CHUNK;
    const int bytesRead = inputFile.read(reinterpret_cast<uint8_t*>(buf), toRead);
    if (bytesRead <= 0) {
      LOG_ERR("HtmlRW", "Read error at offset %u", totalRead);
      ok = false;
      break;
    }
    totalRead += bytesRead;
    const bool done = (totalRead >= fileSize);

    if (XML_Parse(parser, buf, bytesRead, done ? 1 : 0) == XML_STATUS_ERROR) {
      LOG_ERR("HtmlRW", "Parse error at line %lu: %s", XML_GetCurrentLineNumber(parser),
              XML_ErrorString(XML_GetErrorCode(parser)));
      ok = false;
      break;
    }
  }

  XML_ParserFree(parser);
  inputFile.close();

  // Flush remaining batch entries (flushBatch handles cancellation internally —
  // writes HTML without translations when cancelled) and any trailing buffered HTML
  flushBatch();
  writeRaw(pendingHtml);
  pendingHtml.clear();
  // session (a stack local) dies when this function returns; the member must not
  // outlive it. flushBatch() above is the last user of the keep-alive connection.
  httpSession = nullptr;

  Result res;
  res.paragraphsTranslated = paragraphsTranslated;
  res.paragraphsSkipped = paragraphsSkipped;
  res.translateFailures = translateFailures;
  res.cancelled = wasCancelled || (cancelled && *cancelled);
  res.abortedOnErrors = abortedOnErrors;
  res.abortedLowMemory = abortedLowMemory;
  if (abortedOnErrors && !lastError.empty()) {
    snprintf(res.errorDetail, sizeof(res.errorDetail), "%s", lastError.c_str());
  }
  return res;
}
