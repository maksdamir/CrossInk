#pragma once

#include <Print.h>
#include <expat.h>

#include <climits>
#include <cstddef>
#include <cstdint>

// In-book text search over raw spine XHTML.
//
// ChapterScanner streams one spine item through expat without laying it out,
// so a chapter that has never been opened costs the same as one that has.
// Each hit is reported as a visible-text offset: the layout-independent
// codepoint coordinate ChapterHtmlSlimParser records at every page start
// (Section::getPageForVisibleTextOffset). The reader resolves a hit to a page
// with the same restore path it uses for saved progress.
//
// To keep that coordinate exact, offsets are counted with the parser's rules:
// every codepoint of character data outside skipped subtrees, whitespace
// included, with HTML named entities expanded through the same table, plus the
// alt text the parser shows for images it does not draw.
//
// The layout settings that change what the parser counts are passed in as
// ScanOptions. Three cases cannot be mirrored because they depend on state the
// scanner does not have: content hidden only through stylesheet rules (the
// scanner never loads the book's CSS), images that fail to decode at layout
// time, and a chapter the reader rebuilt in low-memory safe mode (which drops
// publisher styles). Each shifts the parser's count relative to the scanner's
// for the rest of that spine item, so later hits there can resolve a page off.
//
// Matching runs on a separate folded stream: ASCII/Latin/Greek/Cyrillic case
// and accents are folded, whitespace runs and block boundaries become a single
// space, typographic quotes and dashes match their ASCII forms, and soft
// hyphens, zero-width characters and combining marks are ignored.
//
// Memory: a scanner is ~1.8 KB of fixed storage plus expat's own parser state;
// it never allocates per character or per hit.
namespace booksearch {

constexpr size_t MAX_QUERY_BYTES = 48;  // Same cap as the Library search keyboard.
constexpr size_t MAX_QUERY_UNITS = 64;  // Folded units; "ß" -> "ss" can expand a query.
constexpr size_t MAX_FOLD_UNITS = 3;    // Most units one codepoint folds into ("ﬃ" -> "ffi").
constexpr size_t SNIPPET_BYTES = 128;
constexpr size_t CONTEXT_BEFORE_UNITS = 28;
constexpr size_t CONTEXT_AFTER_UNITS = 64;

bool isSearchWhitespace(uint32_t cp);
// Characters that do not take part in matching at all (soft hyphen, zero-width
// joiners, combining marks, variation selectors).
bool isIgnorableForSearch(uint32_t cp);
// Fold one non-whitespace codepoint into 0..MAX_FOLD_UNITS matching units and
// return how many were written to `out`.
size_t foldCodepoint(uint32_t cp, uint32_t* out);

class Query {
 public:
  // Folds `utf8` (at most MAX_QUERY_BYTES bytes are read), collapses whitespace
  // and trims it. Returns false when nothing searchable remains.
  bool assign(const char* utf8);
  bool empty() const { return length_ == 0; }
  uint8_t length() const { return length_; }
  uint32_t unit(const size_t index) const { return units_[index]; }
  // KMP failure function: length of the longest proper prefix of units[0..index]
  // that is also a suffix of it.
  uint8_t fallback(const size_t index) const { return fallback_[index]; }

 private:
  uint32_t units_[MAX_QUERY_UNITS] = {};
  uint8_t fallback_[MAX_QUERY_UNITS] = {};
  uint8_t length_ = 0;
};

struct Hit {
  // Offset of the first matched codepoint within its spine item.
  uint32_t visibleTextOffset = 0;
  // NUL-terminated UTF-8 context around the match, with "…" where text was cut.
  char snippet[SNIPPET_BYTES] = {};
};

// Called once per hit, in document order. Return false to stop the scan; the
// hit passed to that call is still counted.
using HitCallback = bool (*)(const Hit& hit, void* context);

// Reader settings that change which text ChapterHtmlSlimParser counts. Pass the
// values the book is being laid out with (ReaderRenderSpec).
struct ScanOptions {
  // ReaderRenderSpec::imageRendering: 0 display, 1 placeholder, 2 suppress.
  // Decides when the parser shows "[Image: alt]" in place of a picture.
  uint8_t imageRendering = 0;
  // ReaderRenderSpec::embeddedStyle (publisher styles). The parser resolves
  // element styles, inline ones included, only when this is on, so an inline
  // `style="display:none"` hides text only then.
  bool embeddedStyle = true;

  bool operator==(const ScanOptions& other) const {
    return imageRendering == other.imageRendering && embeddedStyle == other.embeddedStyle;
  }
  bool operator!=(const ScanOptions& other) const { return !(*this == other); }
};

class ChapterScanner final : public Print {
 public:
  ChapterScanner() = default;
  ~ChapterScanner() override;
  ChapterScanner(const ChapterScanner&) = delete;
  ChapterScanner& operator=(const ChapterScanner&) = delete;

  // Prepare to scan one spine item. `query` must outlive the scan. Returns false
  // (after logging) when the query is empty or expat cannot be allocated.
  bool begin(const Query& query, HitCallback onHit, void* context, const ScanOptions& options = {});
  // Print sink for Epub::readItemContentsToStream(). Returns less than `size`
  // once the scan has stopped or failed, so pass allowEarlyStop = true.
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buffer, size_t size) override;
  // Flush the final hit. Returns false when the XHTML could not be parsed; hits
  // found before the parse error have still been reported.
  bool finish();
  // Release the expat parser. Safe to call repeatedly.
  void end();

  bool stopped() const { return stopped_; }
  bool parseFailed() const { return parseFailed_; }
  // Visible codepoints counted so far, in ChapterHtmlSlimParser's coordinate.
  uint32_t visibleTextLength() const { return visibleOffset_; }

 private:
  struct Unit {
    uint32_t folded;
    uint32_t display;  // Codepoint shown in snippets; 0 for the extra units of an expansion.
    uint32_t offset;
  };
  static constexpr size_t RING_UNITS = 128;
  static_assert(RING_UNITS >= MAX_QUERY_UNITS + CONTEXT_BEFORE_UNITS + 1,
                "the ring must hold a full match plus its leading context");

  static void XMLCALL onStartElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL onEndElement(void* userData, const XML_Char* name);
  static void XMLCALL onCharacterData(void* userData, const XML_Char* text, int len);
  static void XMLCALL onDefault(void* userData, const XML_Char* text, int len);

  void startElement(const char* name, const char** atts);
  void endElement(const char* name);
  void characterData(const char* text, int len);
  void imageElement(const char** atts);
  void skipCurrentElement();
  void boundary();
  void pushCodepoint(uint32_t cp, uint32_t offset);
  void pushUnit(uint32_t folded, uint32_t display, uint32_t offset);
  const Unit& unitAt(uint32_t index) const { return ring_[index % RING_UNITS]; }
  void startPendingHit();
  bool appendSnippet(const char* bytes, size_t len);
  bool appendDisplay(uint32_t cp);
  void appendAfterContext(const Unit& unit);
  void finalizePending(bool textContinues);
  void stop();

  XML_Parser parser_ = nullptr;
  const Query* query_ = nullptr;
  HitCallback onHit_ = nullptr;
  void* context_ = nullptr;

  // Visibility state, mirroring ChapterHtmlSlimParser's depth bookkeeping.
  ScanOptions options_;
  int depth_ = 0;
  int skipUntilDepth_ = INT_MAX;
  int tableDepth_ = 0;
  uint32_t visibleOffset_ = 0;

  // Folded match stream.
  Unit ring_[RING_UNITS] = {};
  uint32_t unitCount_ = 0;
  bool lastWasSpace_ = true;
  uint8_t matched_ = 0;

  // Hit whose trailing context is still being collected.
  Hit pending_;
  bool hasPending_ = false;
  size_t pendingBytes_ = 0;
  size_t pendingMatchEndBytes_ = 0;
  size_t pendingLastSpaceBytes_ = 0;
  uint8_t pendingAfterRemaining_ = 0;

  bool stopped_ = false;
  bool parseFailed_ = false;
};

}  // namespace booksearch
