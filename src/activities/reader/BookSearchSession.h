#pragma once

#include <Epub/search/BookTextSearch.h>
#include <Memory.h>

#include <cstddef>
#include <cstdint>
#include <string>

// One in-book search hit: where it is, and the context shown in the list.
struct BookSearchResult {
  uint16_t spineIndex;
  uint32_t visibleTextOffset;
  char snippet[booksearch::SNIPPET_BYTES];
};

// State of the reader's current in-book search. EpubReaderActivity owns it for
// as long as the book stays open, so reopening Search in Book returns to the
// same results, and an interrupted scan resumes where it stopped.
//
// Results live in one fixed buffer allocated when the first search starts and
// reused by later searches, so scanning never allocates per hit. PSRAM devices
// keep up to MAX_RESULTS_PSRAM hits there (~27 KB); devices without PSRAM keep
// MAX_RESULTS_INTERNAL in internal RAM (~6.8 KB), which leaves headroom for
// section rebuilds on the X3/X4 after a jump.
class BookSearchSession {
 public:
  static constexpr size_t MAX_RESULTS_PSRAM = 200;
  static constexpr size_t MAX_RESULTS_INTERNAL = 50;

  // Why a scan ended before reaching the last spine item.
  enum class Failure : uint8_t { None, OutOfMemory, ReadError };

  // The layout settings the open book is laid out with, as the scanner needs them.
  static booksearch::ScanOptions optionsFromSettings();

  // Begin a new search for `text` over `spineCount` spine items, counting text
  // the way a layout with `options` does. Returns false (after logging) when
  // nothing in `text` is searchable or the result buffer cannot be allocated;
  // outOfMemory() tells the two apart.
  bool start(const std::string& text, int spineCount, const booksearch::ScanOptions& options);

  bool hasQuery() const { return started_; }
  const std::string& queryText() const { return queryText_; }
  const booksearch::Query& query() const { return query_; }
  // Settings the hits' offsets were counted with. If the book's layout settings
  // change, the offsets no longer line up with its pages and the search must
  // run again.
  const booksearch::ScanOptions& options() const { return options_; }
  bool outOfMemory() const { return outOfMemory_; }

  bool scanning() const { return started_ && !complete_; }
  bool complete() const { return complete_; }
  bool limitReached() const { return limitReached_; }
  int nextSpine() const { return nextSpine_; }
  int spineCount() const { return spineCount_; }
  // Advance past the spine item just scanned; completes the search after the last one.
  void finishSpine();
  // End the scan early, keeping the hits found so far. A New Search clears it.
  void fail(Failure reason);
  Failure failure() const { return failure_; }

  size_t resultCount() const { return count_; }
  const BookSearchResult& result(const size_t index) const { return results()[index]; }
  // Store a hit from spine item `spineIndex`. Returns false once the buffer is
  // full, which also marks the search complete.
  bool addHit(uint16_t spineIndex, const booksearch::Hit& hit);

  // List position, restored when the results are reopened.
  int selectedRow = 0;
  int topRow = 0;

 private:
  BookSearchResult* results() const { return reinterpret_cast<BookSearchResult*>(storage_.get()); }
  bool ensureStorage();

  std::string queryText_;
  booksearch::Query query_;
  booksearch::ScanOptions options_;
  HeapByteBuffer storage_;
  size_t capacity_ = 0;
  size_t count_ = 0;
  int nextSpine_ = 0;
  int spineCount_ = 0;
  bool started_ = false;
  bool complete_ = false;
  bool limitReached_ = false;
  bool outOfMemory_ = false;
  Failure failure_ = Failure::None;
};
