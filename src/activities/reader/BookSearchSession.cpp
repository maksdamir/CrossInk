#include "BookSearchSession.h"

#include <Logging.h>

#include <cstring>
#include <type_traits>

#include "CrossPointSettings.h"

static_assert(std::is_trivially_copyable_v<BookSearchResult>, "results are stored in a raw byte buffer");

bool BookSearchSession::ensureStorage() {
  if (storage_) return true;
  // Prefer PSRAM so large result lists never compete with layout for internal RAM.
  if (psramHeapAvailable()) {
    storage_ = makePsramByteBufferNoThrow(MAX_RESULTS_PSRAM * sizeof(BookSearchResult));
    if (storage_) {
      capacity_ = MAX_RESULTS_PSRAM;
      return true;
    }
  }
  storage_ = makeInternalByteBufferNoThrow(MAX_RESULTS_INTERNAL * sizeof(BookSearchResult));
  if (storage_) {
    capacity_ = MAX_RESULTS_INTERNAL;
    return true;
  }
  capacity_ = 0;
  LOG_ERR("SRCH", "Could not allocate %u bytes for search results",
          static_cast<unsigned>(MAX_RESULTS_INTERNAL * sizeof(BookSearchResult)));
  return false;
}

booksearch::ScanOptions BookSearchSession::optionsFromSettings() {
  // While a book is open SETTINGS holds its effective (per-book) reader settings,
  // the same values CrossPointSettings::readerRenderSpec() lays it out with.
  booksearch::ScanOptions options;
  options.imageRendering = SETTINGS.imageRendering;
  options.embeddedStyle = SETTINGS.embeddedStyle != 0;
  return options;
}

bool BookSearchSession::start(const std::string& text, const int spineCount, const booksearch::ScanOptions& options) {
  outOfMemory_ = false;
  // Fold into a temporary (~320 bytes of stack) so an unsearchable entry leaves
  // the current query and its results untouched.
  booksearch::Query folded;
  if (!folded.assign(text.c_str())) {
    LOG_DBG("SRCH", "Ignoring search with nothing searchable");
    return false;
  }
  if (!ensureStorage()) {
    outOfMemory_ = true;
    return false;
  }
  queryText_ = text;
  query_ = folded;
  options_ = options;
  count_ = 0;
  nextSpine_ = 0;
  spineCount_ = spineCount > 0 ? spineCount : 0;
  started_ = true;
  complete_ = spineCount_ == 0;
  limitReached_ = false;
  failure_ = Failure::None;
  selectedRow = 0;
  topRow = 0;
  LOG_INF("SRCH", "Search started: %u query units over %d spine items (capacity %u)",
          static_cast<unsigned>(query_.length()), spineCount_, static_cast<unsigned>(capacity_));
  return true;
}

void BookSearchSession::finishSpine() {
  if (!scanning()) return;
  nextSpine_++;
  if (nextSpine_ >= spineCount_) complete_ = true;
}

void BookSearchSession::fail(const Failure reason) {
  if (!scanning()) return;
  failure_ = reason;
  complete_ = true;
  LOG_ERR("SRCH", "Search stopped at spine %d/%d (%s); keeping %u hits", nextSpine_ + 1, spineCount_,
          reason == Failure::OutOfMemory ? "out of memory" : "read error", static_cast<unsigned>(count_));
}

bool BookSearchSession::addHit(const uint16_t spineIndex, const booksearch::Hit& hit) {
  if (count_ >= capacity_) {
    limitReached_ = true;
    complete_ = true;
    return false;
  }
  BookSearchResult& slot = results()[count_++];
  slot.spineIndex = spineIndex;
  slot.visibleTextOffset = hit.visibleTextOffset;
  static_assert(sizeof(slot.snippet) == sizeof(hit.snippet));
  memcpy(slot.snippet, hit.snippet, sizeof(slot.snippet));
  if (count_ >= capacity_) {
    limitReached_ = true;
    complete_ = true;
    return false;
  }
  return true;
}
