#pragma once
#include <Epub.h>
#include <Epub/search/BookTextSearch.h>
#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>
#include <Memory.h>

#include <array>
#include <atomic>
#include <memory>
#include <string>

#include "BookSearchSession.h"
#include "activities/Activity.h"
#include "components/themes/BaseTheme.h"
#include "util/ButtonNavigator.h"

// In-book search results. Scans the book one spine item per loop() pass, so
// Back stays responsive between chapters and the list fills in as hits are
// found. Row 0 starts a new search; every other row jumps to its hit by
// returning a BookSearchJumpResult. The session (owned by the reader) keeps the
// results and scan position when this screen closes.
class EpubReaderSearchActivity final : public Activity {
  using UiApp = freeink::ui::FreeInkApp<20, 4>;
  static constexpr size_t ROW_WINDOW_SIZE = 20;

  std::shared_ptr<Epub> epub;
  BookSearchSession& session;
  std::unique_ptr<booksearch::ChapterScanner> scanner;
  // Buffer for reading a chapter's uncompressed cached XHTML.
  HeapByteBuffer readBuffer;
  size_t streamChunkBytes = 0;
  ButtonNavigator buttonNavigator;
  freeink::ui::GfxRendererTarget uiTarget;
  UiApp app;
  std::atomic<bool> uiReady{false};
  int visibleRows = 1;
  bool initialViewportPending = true;
  unsigned long lastScanRenderMs = 0;
  uint8_t consecutiveReadFailures = 0;
  std::atomic<size_t> renderedResultCount{0};  // Written by the render task, read by loop().
  std::atomic<bool> renderedSinceEnter{false};
  // Scan statistics since this screen opened (or the last New Search), logged on completion.
  struct ScanPass {
    unsigned long startedMs = 0;
    uint32_t chapters = 0;
    uint32_t chars = 0;
    uint32_t minFreeHeap = 0;
    uint32_t minMaxAlloc = 0;
    uint32_t cachedChapters = 0;  // read from the uncompressed HTML cache
    uint64_t readUs = 0;          // SD reads and inflate
    uint64_t scanUs = 0;          // XML parse and matching
    unsigned long redrawMs = 0;   // waiting for progress repaints
  } pass;
  std::array<std::string, ROW_WINDOW_SIZE> chapterWindow{};
  std::array<freeink::ui::ListItem, ROW_WINDOW_SIZE> itemWindow{};
  char titleText[booksearch::MAX_QUERY_BYTES + 8] = {};
  char statusText[64] = {};

  static void resultsScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  static bool onHit(const booksearch::Hit& hit, void* user);
  void buildResultsScreen(UiApp::ScreenType& screen);
  int totalRows() const { return 1 + static_cast<int>(session.resultCount()); }
  void activateRow(int row);
  void promptNewSearch();
  void scanNextSpine();
  bool streamSpine(int spineIndex, const std::string& href, Print& sink, bool& fromCache);
  void formatHeader();
  Rect headerRect() const;
  Rect statusRect() const;
  void cancel();

 public:
  EpubReaderSearchActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::shared_ptr<Epub> epub,
                           BookSearchSession& session);
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool skipLoopDelay() override { return session.scanning(); }
  bool preventAutoSleep() override { return session.scanning(); }
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
};
