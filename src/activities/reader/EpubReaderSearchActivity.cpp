#include "EpubReaderSearchActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#ifndef SIMULATOR
#include <esp_task_wdt.h>
#endif

#include <FsHelpers.h>
#include <HalStorage.h>

#include <algorithm>
#include <cstdio>
#include <utility>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr int NEW_SEARCH_ROW = 0;
// Read size for one chapter. ZipFile allocates a read and an output buffer of
// this size per chapter; the cached-HTML path reuses one buffer of it. With
// PSRAM (X4 Pro, Sticky, X4 Classic) larger reads mean fewer SD transactions,
// inflate calls and XML_Parse() calls per chapter. Without PSRAM keep the 1 KB
// the KOReader sync resolver already streams with from this reader context.
constexpr size_t STREAM_CHUNK_BYTES_PSRAM = 16 * 1024;
constexpr size_t STREAM_CHUNK_BYTES_INTERNAL = 1024;
// A fast e-ink refresh blocks the scan for ~0.5 s. Repaint progress at most this
// often (plus once when the first hits arrive and once when the scan ends).
constexpr unsigned long SCAN_RENDER_INTERVAL_MS = 5000;

// A run of unreadable spine items after which the scan gives up.
constexpr uint8_t MAX_CONSECUTIVE_READ_FAILURES = 3;
// Inflating and parsing a large chapter is one long call on the main loop.
// Same budget SDCardManager uses for long reads: let lower-priority tasks run
// and feed the task watchdog (on boards where it watches this task) every
// 100 ms, instead of once per 1 KB chunk, which would add seconds per book.
constexpr unsigned long COOPERATIVE_YIELD_MS = 100;

struct HitContext {
  BookSearchSession* session;
  uint16_t spineIndex;
};

// Forwards the zip stream to the scanner, yielding on a time budget.
class CooperativeSink final : public Print {
 public:
  explicit CooperativeSink(Print& target) : target(target), lastYieldMs(millis()) {
#ifndef SIMULATOR
    // esp_task_wdt_reset() logs an error for unsubscribed tasks; ask once.
    watchdogWatchesThisTask = esp_task_wdt_status(nullptr) == ESP_OK;
#endif
  }

  size_t write(const uint8_t c) override { return write(&c, 1); }

  size_t write(const uint8_t* buffer, const size_t size) override {
    const unsigned long startedUs = micros();
    const size_t written = target.write(buffer, size);
    targetUs += micros() - startedUs;
    if (millis() - lastYieldMs >= COOPERATIVE_YIELD_MS) {
#ifndef SIMULATOR
      if (watchdogWatchesThisTask) esp_task_wdt_reset();
#endif
      delay(1);
      lastYieldMs = millis();
    }
    return written;
  }

 private:
  Print& target;
  unsigned long lastYieldMs;
  bool watchdogWatchesThisTask = false;

 public:
  // Time spent inside the scanner (parse + match), for the end-of-search log.
  unsigned long targetUs = 0;
};
}  // namespace

EpubReaderSearchActivity::EpubReaderSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                   std::shared_ptr<Epub> epub, BookSearchSession& session)
    : Activity("EpubReaderSearch", renderer, mappedInput),
      epub(std::move(epub)),
      session(session),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void EpubReaderSearchActivity::onEnter() {
  Activity::onEnter();
  mappedInput.setReaderTouchscreenOverride(true);
  // ~1.8 KB of fixed scan state, alive only while this screen is open.
  scanner = makeUniqueNoThrow<booksearch::ChapterScanner>();
  if (!scanner) {
    LOG_ERR("SRCH", "Could not allocate the chapter scanner (free=%u, maxAlloc=%u)", ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
  }
  // One reusable read buffer for cached chapters; PSRAM keeps it off the
  // internal heap the layout code needs.
  streamChunkBytes = psramHeapAvailable() ? STREAM_CHUNK_BYTES_PSRAM : STREAM_CHUNK_BYTES_INTERNAL;
  readBuffer = psramHeapAvailable() ? makePsramByteBufferNoThrow(streamChunkBytes)
                                    : makeInternalByteBufferNoThrow(streamChunkBytes);
  if (!readBuffer) LOG_ERR("SRCH", "No read buffer; cached chapters will be read from the EPUB instead");
  initialViewportPending = true;
  uiReady = false;
  lastScanRenderMs = 0;
  renderedResultCount = session.resultCount();
  renderedSinceEnter = false;
  consecutiveReadFailures = 0;
  pass = ScanPass{};
  applySharedUiTheme(app, uiTarget);
  app.on(ACTION_ROW, &EpubReaderSearchActivity::onRowEvent, this);
  app.setScreen(&EpubReaderSearchActivity::resultsScreen, this);
  requestUpdate();
}

void EpubReaderSearchActivity::onExit() {
  // Dropping the scanner releases its expat parser if a scan was interrupted;
  // the session keeps nextSpine() so reopening resumes from that chapter.
  scanner.reset();
  closeZip();
  readBuffer.reset();
  mappedInput.setReaderTouchscreenOverride(false);
  Activity::onExit();
}

bool EpubReaderSearchActivity::onHit(const booksearch::Hit& hit, void* user) {
  const auto* context = static_cast<HitContext*>(user);
  return context->session->addHit(context->spineIndex, hit);
}

void EpubReaderSearchActivity::scanNextSpine() {
  if (!session.scanning()) return;
  if (!scanner) {
    // Without a scanner nothing can progress; stop rather than spin.
    session.fail(BookSearchSession::Failure::OutOfMemory);
    requestUpdate();
    return;
  }

  // Show the "Searching…" state before the first chapter blocks the loop.
  // Waiting (rather than requestUpdate()) also guarantees the render task a
  // turn: this loop re-takes the render lock straight after every chapter.
  if (!renderedSinceEnter) {
    const unsigned long waitStartedMs = millis();
    requestUpdateAndWait();
    pass.redrawMs += millis() - waitStartedMs;
  }
  if (pass.chapters == 0) {
    pass.startedMs = millis();
    pass.minFreeHeap = ESP.getFreeHeap();
    pass.minMaxAlloc = ESP.getMaxAllocHeap();
  }

  const size_t hitsBefore = session.resultCount();
  {
    // The render task reads the session and the EPUB's TOC; hold the lock so
    // neither the results nor the SD card are touched from both tasks at once.
    RenderLock lock(*this);
    const int spineIndex = session.nextSpine();
    const auto href = epub->getSpineItem(spineIndex).href;
    HitContext context{&session, static_cast<uint16_t>(spineIndex)};
    const unsigned long startedMs = millis();
    // Count text with the layout settings the search started with, so every
    // chapter's offsets are in the same coordinate as the book's pages.
    if (!scanner->begin(session.query(), &EpubReaderSearchActivity::onHit, &context, session.options())) {
      // begin() only fails when expat cannot allocate; later chapters would too.
      session.fail(BookSearchSession::Failure::OutOfMemory);
    } else {
      CooperativeSink sink(*scanner);
      bool fromCache = false;
      const unsigned long streamStartedUs = micros();
      const bool streamed = streamSpine(spineIndex, href, sink, fromCache);
      const unsigned long streamUs = micros() - streamStartedUs;
      pass.scanUs += sink.targetUs;
      pass.readUs += streamUs > sink.targetUs ? streamUs - sink.targetUs : 0;
      if (fromCache) pass.cachedChapters++;
      // Sample while the zip inflater is still allocated: the scan's peak.
      pass.minFreeHeap = std::min<uint32_t>(pass.minFreeHeap, ESP.getFreeHeap());
      pass.minMaxAlloc = std::min<uint32_t>(pass.minMaxAlloc, ESP.getMaxAllocHeap());
      scanner->finish();
      pass.chars += scanner->visibleTextLength();
      LOG_DBG("SRCH", "Spine %d/%d: %lu ms, %lu chars, %u hits", spineIndex + 1, session.spineCount(),
              millis() - startedMs, static_cast<unsigned long>(scanner->visibleTextLength()),
              static_cast<unsigned>(session.resultCount() - hitsBefore));
      if (streamed) {
        consecutiveReadFailures = 0;
      } else {
        // One unreadable item (a missing file) is skipped. Several in a row
        // usually means the inflater cannot get memory or the card is failing,
        // and every remaining chapter would fail the same way.
        LOG_ERR("SRCH", "Could not read spine %d (%s); free=%u maxAlloc=%u", spineIndex, href.c_str(),
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        if (++consecutiveReadFailures >= MAX_CONSECUTIVE_READ_FAILURES) {
          session.fail(BookSearchSession::Failure::ReadError);
        }
      }
    }
    scanner->end();
    pass.chapters++;
    if (session.scanning()) session.finishSpine();
  }

  const unsigned long now = millis();
  const bool firstHits = renderedResultCount == 0 && session.resultCount() > 0;
  if (!session.scanning() || firstHits || now - lastScanRenderMs >= SCAN_RENDER_INTERVAL_MS) {
    if (!session.scanning()) {
      closeZip();
      LOG_INF("SRCH",
              "Search finished: %u hits%s; %u chapters (%u cached), %lu chars in %lu ms (read+inflate %lu ms, "
              "scan %lu ms, redraw %lu ms); min free=%u maxAlloc=%u",
              static_cast<unsigned>(session.resultCount()), session.limitReached() ? " (limit reached)" : "",
              static_cast<unsigned>(pass.chapters), static_cast<unsigned>(pass.cachedChapters),
              static_cast<unsigned long>(pass.chars), now - pass.startedMs,
              static_cast<unsigned long>(pass.readUs / 1000), static_cast<unsigned long>(pass.scanUs / 1000),
              pass.redrawMs, static_cast<unsigned>(pass.minFreeHeap), static_cast<unsigned>(pass.minMaxAlloc));
    }
    lastScanRenderMs = now;
    const unsigned long waitStartedMs = millis();
    requestUpdateAndWait();
    pass.redrawMs += millis() - waitStartedMs;
  }
}

bool EpubReaderSearchActivity::streamSpine(const int spineIndex, const std::string& href, Print& sink,
                                           bool& fromCache) {
  fromCache = false;
  // A chapter the reader has laid out keeps an uncompressed copy of its XHTML
  // (Section's html/<spine>.html: written byte for byte from the zip and only
  // renamed into place once complete). Reading it skips inflate entirely. If
  // the reader still holds that file open for an unfinished layout, the open
  // fails and the chapter is read from the EPUB instead.
  if (readBuffer) {
    const std::string cachedPath = epub->getCachePath() + "/html/" + std::to_string(spineIndex) + ".html";
    HalFile file;
    if (Storage.exists(cachedPath.c_str()) && Storage.openFileForRead("SRCH", cachedPath, file)) {
      fromCache = true;
      bool ok = true;
      while (true) {
        const int bytes = file.read(readBuffer.get(), streamChunkBytes);
        if (bytes < 0) {
          LOG_ERR("SRCH", "Read error in cached chapter %s", cachedPath.c_str());
          ok = false;
          break;
        }
        if (bytes == 0) break;
        // Fewer bytes accepted means the scanner stopped (result limit or parse error).
        if (sink.write(readBuffer.get(), static_cast<size_t>(bytes)) != static_cast<size_t>(bytes)) break;
      }
      file.close();
      return ok;
    }
  }

  if (!zip) {
    zip = makeUniqueNoThrow<ZipFile>(epub->getPath());
    if (zip && !zip->open()) {
      LOG_ERR("SRCH", "Could not open %s for searching", epub->getPath().c_str());
      zip.reset();
    }
  }
  // Same entry-name mapping as Epub::readItemContentsToStream().
  if (zip) return zip->readFileToStream(FsHelpers::normalisePath(href).c_str(), sink, streamChunkBytes, true);
  return epub->readItemContentsToStream(href, sink, streamChunkBytes, true);
}

void EpubReaderSearchActivity::closeZip() {
  if (!zip) return;
  zip->close();
  zip.reset();
}

void EpubReaderSearchActivity::cancel() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
}

void EpubReaderSearchActivity::promptNewSearch() {
  auto keyboard = makeUniqueNoThrow<KeyboardEntryActivity>(
      renderer, mappedInput, tr(STR_SEARCH_IN_BOOK), session.queryText(), booksearch::MAX_QUERY_BYTES, InputType::Text);
  if (!keyboard) {
    LOG_ERR("SRCH", "Could not allocate the search keyboard");
    return;
  }
  startActivityForResult(std::move(keyboard), [this](const ActivityResult& result) {
    const auto* text = std::get_if<KeyboardResult>(&result.data);
    if (!result.isCancelled && text) {
      RenderLock lock(*this);
      if (session.start(text->text, epub->getSpineItemsCount(), BookSearchSession::optionsFromSettings())) {
        initialViewportPending = true;
        renderedResultCount = 0;
        // Draw the new query's "Searching…" header before its first chapter
        // blocks the loop, instead of leaving the old results on screen.
        renderedSinceEnter = false;
        lastScanRenderMs = millis();
        consecutiveReadFailures = 0;
        pass = ScanPass{};
      }
    }
    requestUpdate();
  });
}

void EpubReaderSearchActivity::activateRow(const int row) {
  if (row == NEW_SEARCH_ROW) {
    promptNewSearch();
    return;
  }
  const size_t index = static_cast<size_t>(row - 1);
  if (index >= session.resultCount()) return;
  const BookSearchResult& hit = session.result(index);
  session.selectedRow = row;
  setResult(BookSearchJumpResult{hit.spineIndex, hit.visibleTextOffset});
  finish();
}

void EpubReaderSearchActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EpubReaderSearchActivity*>(user);
  if (event.value < 0 || event.value >= self->totalRows()) return;
  self->session.selectedRow = event.value;
  self->app.clearTapFlash();
  self->activateRow(event.value);
}

Rect EpubReaderSearchActivity::headerRect() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  return Rect{safe.x, safe.y + metrics.topPadding, safe.width, TouchHeaderBackButton::height(metrics, mappedInput)};
}

Rect EpubReaderSearchActivity::statusRect() const {
  // The status gets its own row under the title. The header's right-hand label
  // slot is not used: the touch header draws its title without reserving that
  // label's width, so a long query would overprint the status.
  const Rect header = headerRect();
  return Rect{header.x, header.y + header.height, header.width, UITheme::getInstance().getMetrics().tabBarHeight};
}

void EpubReaderSearchActivity::loop() {
  const int rows = totalRows();
  if (TouchHeaderBackButton::wasTapped(mappedInput, headerRect()) ||
      mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    cancel();
    return;
  }

  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
    const int next = scrollListBy(session.topRow, delta, visibleRows, rows);
    if (next != session.topRow) {
      session.topRow = next;
      requestUpdate();
    }
    return;
  }

  const auto moveSelection = [this, rows](const int index) {
    session.selectedRow = index;
    session.topRow = followListSelection(session.selectedRow, session.topRow, visibleRows, rows);
    requestUpdate();
  };
  buttonNavigator.onNextRelease(
      [this, rows, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(session.selectedRow, rows)); });
  buttonNavigator.onPreviousRelease(
      [this, rows, &moveSelection] { moveSelection(ButtonNavigator::previousIndex(session.selectedRow, rows)); });
  buttonNavigator.onNextContinuous([this, rows, &moveSelection] {
    moveSelection(ButtonNavigator::nextPageIndex(session.selectedRow, rows, visibleRows));
  });
  buttonNavigator.onPreviousContinuous([this, rows, &moveSelection] {
    moveSelection(ButtonNavigator::previousPageIndex(session.selectedRow, rows, visibleRows));
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    activateRow(session.selectedRow);
    return;
  }

  scanNextSpine();
}

void EpubReaderSearchActivity::formatHeader() {
  snprintf(titleText, sizeof(titleText), "\xE2\x80\x9C%s\xE2\x80\x9D", session.queryText().c_str());
  const unsigned count = static_cast<unsigned>(session.resultCount());
  if (session.failure() == BookSearchSession::Failure::OutOfMemory) {
    snprintf(statusText, sizeof(statusText), "%s", tr(STR_SEARCH_OUT_OF_MEMORY));
  } else if (session.failure() == BookSearchSession::Failure::ReadError) {
    snprintf(statusText, sizeof(statusText), "%s", tr(STR_SEARCH_READ_FAILED));
  } else if (session.scanning()) {
    snprintf(statusText, sizeof(statusText), tr(STR_SEARCH_PROGRESS_FORMAT), session.nextSpine() + 1,
             session.spineCount(), count);
  } else if (count == 0) {
    snprintf(statusText, sizeof(statusText), "%s", tr(STR_SEARCH_NO_MATCHES));
  } else if (session.limitReached()) {
    snprintf(statusText, sizeof(statusText), tr(STR_SEARCH_LIMIT_FORMAT), count);
  } else if (count == 1) {
    snprintf(statusText, sizeof(statusText), "%s", tr(STR_SEARCH_ONE_MATCH));
  } else {
    snprintf(statusText, sizeof(statusText), tr(STR_SEARCH_MATCHES_FORMAT), count);
  }
}

void EpubReaderSearchActivity::resultsScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<EpubReaderSearchActivity*>(user)->buildResultsScreen(screen);
}

void EpubReaderSearchActivity::buildResultsScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect status = statusRect();
  screen.setContentMargin(fui::Insets{static_cast<int16_t>(status.y + status.height),
                                      static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
                                      static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height),
                                      static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const int rows = totalRows();
  fui::ListProps props;
  props.labelText = screen.theme().bodyText;
  const auto fit = configureUiList(props, screen.theme(), screen.body(), UiListRowType::WithSubtitle);
  visibleRows = std::min(fit > 0 ? static_cast<int>(fit) : 1, static_cast<int>(ROW_WINDOW_SIZE));
  session.selectedRow = std::clamp(session.selectedRow, 0, rows - 1);
  session.topRow = initialViewportPending ? followListSelection(session.selectedRow, 0, visibleRows, rows)
                                          : scrollListBy(session.topRow, 0, visibleRows, rows);
  initialViewportPending = false;

  const int drawCount = std::min(visibleRows, rows - session.topRow);
  for (int i = 0; i < drawCount; ++i) {
    const int row = session.topRow + i;
    fui::ListItem& item = itemWindow[static_cast<size_t>(i)];
    item = fui::ListItem{};
    item.actionValue = static_cast<int16_t>(row);
    if (row == NEW_SEARCH_ROW) {
      item.label = tr(STR_NEW_SEARCH);
      continue;
    }
    const BookSearchResult& hit = session.result(static_cast<size_t>(row - 1));
    const int tocIndex = epub->getTocIndexForSpineIndex(hit.spineIndex);
    chapterWindow[static_cast<size_t>(i)] = tocIndex >= 0 ? epub->getTocItem(tocIndex).title : std::string();
    item.label = hit.snippet;
    item.subtitle = chapterWindow[static_cast<size_t>(i)].empty() ? tr(STR_UNKNOWN_CHAPTER)
                                                                  : chapterWindow[static_cast<size_t>(i)].c_str();
  }

  props.items = itemWindow.data();
  props.itemsWindowFirst = static_cast<uint16_t>(session.topRow);
  props.itemsWindowCount = static_cast<uint16_t>(drawCount);
  props.count = static_cast<uint16_t>(rows);
  props.selectedIndex = static_cast<int16_t>(session.selectedRow);
  props.topIndex = static_cast<uint16_t>(session.topRow);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  screen.list(props);
}

void EpubReaderSearchActivity::render(RenderLock&&) {
  renderedResultCount = session.resultCount();
  renderedSinceEnter = true;
  formatHeader();
  renderer.clearScreen();
  const Rect header = headerRect();
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, titleText, true);
  } else {
    GUI.drawHeader(renderer, header, titleText, nullptr, true);
  }
  const Rect status = statusRect();
  GUI.drawSubHeader(renderer, status, statusText);
  renderer.drawLine(status.x, status.y + status.height - 1, status.x + status.width - 1, status.y + status.height - 1,
                    1, true);
  uiReady = false;
  app.render();
  uiReady = true;
  const auto labels =
      mappedInput.mapLabels(mappedInput.withBackArrow(tr(STR_BACK)), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer();
}
