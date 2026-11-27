#include "BookTextSearch.h"

#include <Logging.h>
#include <Utf8.h>
#include <XmlParserUtils.h>

#include <cstring>
#include <iterator>
#include <string_view>

#include "Epub/htmlEntities.h"

namespace booksearch {

namespace {

constexpr char ELLIPSIS[] = "\xE2\x80\xA6";
constexpr size_t ELLIPSIS_BYTES = sizeof(ELLIPSIS) - 1;

// Elements ChapterHtmlSlimParser never lays out (rt is consumed as ruby
// annotation text, which is not part of the visible-offset coordinate either).
constexpr const char* const SKIPPED_TAGS[] = {"head", "style", "script", "title", "rp", "rt"};

// Elements whose start or end separates words on screen. Matching treats them
// like whitespace so "end.</p><p>Next" can match "end. Next". They do not
// change visible offsets.
constexpr const char* const BOUNDARY_TAGS[] = {
    "address", "article", "aside",    "blockquote", "body",   "br",     "caption", "center", "dd", "div",
    "dl",      "dt",      "fieldset", "figcaption", "figure", "footer", "h1",      "h2",     "h3", "h4",
    "h5",      "h6",      "header",   "hgroup",     "hr",     "li",     "main",    "nav",    "ol", "p",
    "pre",     "section", "table",    "td",         "th",     "tr",     "ul"};

bool matchesAny(const char* name, const char* const* list, const size_t count) {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(name, list[i]) == 0) return true;
  }
  return false;
}

const char* getAttribute(const char** atts, const char* name) {
  if (!atts) return nullptr;
  for (size_t i = 0; atts[i]; i += 2) {
    if (strcmp(atts[i], name) == 0) return atts[i + 1] ? atts[i + 1] : "";
  }
  return nullptr;
}

constexpr bool isAsciiWhitespace(const char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

constexpr char asciiLower(const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// Same token semantics as ChapterHtmlSlimParser's attributeContainsToken():
// whitespace-separated, ASCII case-insensitive.
bool attributeContainsToken(const char* value, const char* token) {
  if (!value || !token || token[0] == '\0') return false;
  const size_t tokenLen = strlen(token);
  const char* pos = value;
  while (*pos != '\0') {
    while (isAsciiWhitespace(*pos)) pos++;
    const char* end = pos;
    while (*end != '\0' && !isAsciiWhitespace(*end)) end++;
    if (static_cast<size_t>(end - pos) == tokenLen) {
      size_t i = 0;
      while (i < tokenLen && asciiLower(pos[i]) == asciiLower(token[i])) i++;
      if (i == tokenLen) return true;
    }
    pos = end;
  }
  return false;
}

// True when an inline style attribute's last `display` declaration is `none`.
// ChapterHtmlSlimParser skips such elements before laying anything out.
bool inlineStyleHidesElement(const char* style) {
  if (!style) return false;
  bool hidden = false;
  const char* pos = style;
  while (*pos != '\0') {
    const char* declEnd = pos;
    while (*declEnd != '\0' && *declEnd != ';') declEnd++;
    const char* colon = pos;
    while (colon < declEnd && *colon != ':') colon++;
    if (colon < declEnd) {
      const char* nameStart = pos;
      const char* nameEnd = colon;
      while (nameStart < nameEnd && isAsciiWhitespace(*nameStart)) nameStart++;
      while (nameEnd > nameStart && isAsciiWhitespace(nameEnd[-1])) nameEnd--;
      // Not named DISPLAY: Arduino.h defines that as a macro.
      constexpr char displayProperty[] = "display";
      constexpr size_t displayPropertyLen = sizeof(displayProperty) - 1;
      bool isDisplay = static_cast<size_t>(nameEnd - nameStart) == displayPropertyLen;
      for (size_t i = 0; isDisplay && i < displayPropertyLen; ++i) {
        isDisplay = asciiLower(nameStart[i]) == displayProperty[i];
      }
      if (isDisplay) {
        const char* valueStart = colon + 1;
        const char* valueEnd = declEnd;
        while (valueStart < valueEnd && isAsciiWhitespace(*valueStart)) valueStart++;
        // Drop a trailing "!important" before comparing.
        const char* bang = valueStart;
        while (bang < valueEnd && *bang != '!') bang++;
        valueEnd = bang;
        while (valueEnd > valueStart && isAsciiWhitespace(valueEnd[-1])) valueEnd--;
        hidden = valueEnd - valueStart == 4 && asciiLower(valueStart[0]) == 'n' && asciiLower(valueStart[1]) == 'o' &&
                 asciiLower(valueStart[2]) == 'n' && asciiLower(valueStart[3]) == 'e';
      }
    }
    pos = *declEnd == ';' ? declEnd + 1 : declEnd;
  }
  return hidden;
}

bool isImageTag(const char* name) { return strcmp(name, "img") == 0 || strcmp(name, "image") == 0; }

// ASCII case-insensitive suffix test, as FsHelpers::checkFileExtension().
bool hasExtension(const std::string_view path, const char* extension) {
  const size_t extLen = strlen(extension);
  if (path.size() < extLen) return false;
  const std::string_view tail = path.substr(path.size() - extLen);
  for (size_t i = 0; i < extLen; ++i) {
    if (asciiLower(tail[i]) != asciiLower(extension[i])) return false;
  }
  return true;
}

// Mirrors the parser's isSvgImagePath(): the extension before any query or fragment.
bool isSvgPath(std::string_view path) {
  const size_t end = path.find_first_of("?#");
  if (end != std::string_view::npos) path = path.substr(0, end);
  return hasExtension(path, ".svg") || hasExtension(path, ".svgz");
}

// Mirrors ImageDecoderFactory::isFormatSupported(): formats the reader can draw.
bool isDrawableImagePath(const std::string_view path) {
  return hasExtension(path, ".jpg") || hasExtension(path, ".jpeg") || hasExtension(path, ".png") ||
         hasExtension(path, ".gif");
}

size_t emit(uint32_t* out, const char* ascii) {
  size_t count = 0;
  while (count < MAX_FOLD_UNITS && ascii[count] != '\0') {
    out[count] = static_cast<unsigned char>(ascii[count]);
    count++;
  }
  return count;
}

size_t emitOne(uint32_t* out, const uint32_t cp) {
  out[0] = cp;
  return 1;
}

uint32_t foldGreek(const uint32_t cp) {
  switch (cp) {
    case 0x0386:  // Ά
    case 0x03AC:  // ά
      return 0x03B1;
    case 0x0388:  // Έ
    case 0x03AD:  // έ
      return 0x03B5;
    case 0x0389:  // Ή
    case 0x03AE:  // ή
      return 0x03B7;
    case 0x038A:  // Ί
    case 0x0390:  // ΐ
    case 0x03AA:  // Ϊ
    case 0x03AF:  // ί
    case 0x03CA:  // ϊ
      return 0x03B9;
    case 0x038C:  // Ό
    case 0x03CC:  // ό
      return 0x03BF;
    case 0x038E:  // Ύ
    case 0x03AB:  // Ϋ
    case 0x03B0:  // ΰ
    case 0x03CB:  // ϋ
    case 0x03CD:  // ύ
      return 0x03C5;
    case 0x038F:  // Ώ
    case 0x03CE:  // ώ
      return 0x03C9;
    case 0x03C2:  // final sigma
      return 0x03C3;
    default:
      break;
  }
  if (cp >= 0x0391 && cp <= 0x03A9 && cp != 0x03A2) return cp + 0x20;
  return cp;
}

uint32_t foldCyrillic(const uint32_t cp) {
  if (cp <= 0x040F) return cp + 0x50;  // Ѐ..Џ -> ѐ..џ (Ё is folded to е before this)
  if (cp <= 0x042F) return cp + 0x20;  // А..Я -> а..я
  if (cp >= 0x0460 && cp <= 0x0481 && (cp % 2) == 0) return cp + 1;
  if (cp >= 0x048A && cp <= 0x04BF && (cp % 2) == 0) return cp + 1;
  if (cp == 0x04C0) return 0x04CF;
  if (cp >= 0x04C1 && cp <= 0x04CE && (cp % 2) == 1) return cp + 1;
  if (cp >= 0x04D0 && cp <= 0x04FF && (cp % 2) == 0) return cp + 1;
  return cp;
}

// UTF-8 encode one codepoint into `out` (at least 4 bytes); returns the length.
size_t encodeUtf8(const uint32_t cp, char* out) {
  if (cp < 0x80) {
    out[0] = static_cast<char>(cp);
    return 1;
  }
  if (cp < 0x800) {
    out[0] = static_cast<char>(0xC0 | (cp >> 6));
    out[1] = static_cast<char>(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = static_cast<char>(0xE0 | (cp >> 12));
    out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out[2] = static_cast<char>(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = static_cast<char>(0xF0 | (cp >> 18));
  out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  out[3] = static_cast<char>(0x80 | (cp & 0x3F));
  return 4;
}

}  // namespace

bool isSearchWhitespace(const uint32_t cp) {
  return (cp >= 0x0009 && cp <= 0x000D) || cp == 0x0020 || cp == 0x0085 || cp == 0x00A0 || cp == 0x1680 ||
         (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

bool isIgnorableForSearch(const uint32_t cp) {
  return cp == 0x00AD ||                    // soft hyphen
         cp == 0x034F ||                    // combining grapheme joiner
         (cp >= 0x200B && cp <= 0x200F) ||  // zero-width space/joiners, LRM/RLM
         (cp >= 0x2060 && cp <= 0x2064) ||  // word joiner and invisible operators
         cp == 0xFEFF ||                    // zero-width no-break space / BOM
         utf8IsCombiningMark(cp) || utf8IsVariationSelector(cp);
}

size_t foldCodepoint(uint32_t cp, uint32_t* out) {
  if (cp < 0x80) return emitOne(out, cp >= 'A' && cp <= 'Z' ? cp + ('a' - 'A') : cp);
  if (isIgnorableForSearch(cp)) return 0;

  switch (cp) {
    // Letters with no canonical decomposition, and multi-letter folds.
    case 0x00DF:  // ß
    case 0x1E9E:  // ẞ
      return emit(out, "ss");
    case 0x00C6:  // Æ
    case 0x00E6:  // æ
      return emit(out, "ae");
    case 0x0152:  // Œ
    case 0x0153:  // œ
      return emit(out, "oe");
    case 0x0132:  // Ĳ
    case 0x0133:  // ĳ
      return emit(out, "ij");
    case 0x00D8:  // Ø
    case 0x00F8:  // ø
      return emit(out, "o");
    case 0x0141:  // Ł
    case 0x0142:  // ł
    case 0x013F:  // Ŀ
    case 0x0140:  // ŀ
      return emit(out, "l");
    case 0x00D0:  // Ð
    case 0x00F0:  // ð
    case 0x0110:  // Đ
    case 0x0111:  // đ
      return emit(out, "d");
    case 0x00DE:  // Þ
    case 0x00FE:  // þ
      return emit(out, "th");
    case 0x0130:  // İ
    case 0x0131:  // ı
      return emit(out, "i");
    case 0x0126:  // Ħ
      return emitOne(out, 0x0127);
    case 0x014A:  // Ŋ
      return emitOne(out, 0x014B);
    case 0x0166:  // Ŧ
      return emitOne(out, 0x0167);
    // Typographic punctuation matches what a keyboard types.
    case 0x02BC:  // modifier apostrophe
    case 0x2018:
    case 0x2019:
    case 0x201A:
    case 0x201B:
    case 0x2032:
      return emit(out, "'");
    case 0x00AB:
    case 0x00BB:
    case 0x201C:
    case 0x201D:
    case 0x201E:
    case 0x201F:
    case 0x2033:
      return emit(out, "\"");
    case 0x2010:
    case 0x2011:
    case 0x2012:
    case 0x2013:
    case 0x2014:
    case 0x2015:
    case 0x2212:
      return emit(out, "-");
    case 0x2026:
      return emit(out, "...");
    // Latin presentation-form ligatures.
    case 0xFB00:
      return emit(out, "ff");
    case 0xFB01:
      return emit(out, "fi");
    case 0xFB02:
      return emit(out, "fl");
    case 0xFB03:
      return emit(out, "ffi");
    case 0xFB04:
      return emit(out, "ffl");
    case 0xFB05:
    case 0xFB06:
      return emit(out, "st");
    // Russian text uses е for ё interchangeably.
    case 0x0401:
    case 0x0451:
      return emitOne(out, 0x0435);
    default:
      break;
  }

  if ((cp >= 0x00C0 && cp <= 0x024F) || (cp >= 0x1E00 && cp <= 0x1EFF)) {
    // Strip accents through the shared NFC compose table. Restricted to Latin
    // ranges: utf8DecomposedBase() scans the table linearly, which would be
    // too slow to run on every character of a Greek or Cyrillic book.
    for (int guard = 0; guard < 4; ++guard) {
      const uint32_t base = utf8DecomposedBase(cp);
      if (base == 0) break;
      cp = base;
    }
    if (cp < 0x80) return emitOne(out, cp >= 'A' && cp <= 'Z' ? cp + ('a' - 'A') : cp);
    if (cp >= 0x00C0 && cp <= 0x00DE && cp != 0x00D7) cp += 0x20;
    return emitOne(out, cp);
  }
  if (cp >= 0x0386 && cp <= 0x03CE) return emitOne(out, foldGreek(cp));
  if (cp >= 0x0400 && cp <= 0x04FF) return emitOne(out, foldCyrillic(cp));
  return emitOne(out, cp);
}

bool Query::assign(const char* utf8) {
  length_ = 0;
  if (!utf8) return false;

  const auto* const start = reinterpret_cast<const unsigned char*>(utf8);
  const auto* pos = start;
  bool spacePending = false;
  while (*pos != '\0') {
    const auto* const before = pos;
    const uint32_t cp = utf8NextCodepoint(&pos);
    // utf8NextCodepoint() advances `pos`; guard against a decoder that stalls.
    // cppcheck-suppress knownConditionTrueFalse
    if (pos == before) break;
    if (static_cast<size_t>(pos - start) > MAX_QUERY_BYTES) break;
    if (isSearchWhitespace(cp)) {
      spacePending = length_ > 0;
      continue;
    }
    uint32_t folded[MAX_FOLD_UNITS];
    const size_t count = foldCodepoint(cp, folded);
    if (count == 0) continue;
    const size_t needed = count + (spacePending ? 1 : 0);
    if (length_ + needed > MAX_QUERY_UNITS) break;
    if (spacePending) {
      units_[length_++] = ' ';
      spacePending = false;
    }
    for (size_t i = 0; i < count; ++i) units_[length_++] = folded[i];
  }
  if (length_ == 0) return false;

  fallback_[0] = 0;
  uint8_t prefix = 0;
  for (uint8_t i = 1; i < length_; ++i) {
    while (prefix > 0 && units_[i] != units_[prefix]) prefix = fallback_[prefix - 1];
    if (units_[i] == units_[prefix]) prefix++;
    fallback_[i] = prefix;
  }
  return true;
}

ChapterScanner::~ChapterScanner() { end(); }

bool ChapterScanner::begin(const Query& query, const HitCallback onHit, void* context, const ScanOptions& options) {
  end();
  query_ = &query;
  onHit_ = onHit;
  context_ = context;
  options_ = options;
  depth_ = 0;
  skipUntilDepth_ = INT_MAX;
  tableDepth_ = 0;
  visibleOffset_ = 0;
  unitCount_ = 0;
  lastWasSpace_ = true;
  matched_ = 0;
  hasPending_ = false;
  stopped_ = false;
  parseFailed_ = false;

  if (query.empty()) {
    LOG_ERR("BTS", "Refusing to scan with an empty query");
    return false;
  }
  parser_ = XML_ParserCreate(nullptr);
  if (!parser_) {
    LOG_ERR("BTS", "Failed to create XML parser");
    return false;
  }
  XML_SetUserData(parser_, this);
  XML_SetElementHandler(parser_, &ChapterScanner::onStartElement, &ChapterScanner::onEndElement);
  XML_SetCharacterDataHandler(parser_, &ChapterScanner::onCharacterData);
  // Same handler ChapterHtmlSlimParser installs: HTML named entities such as
  // &nbsp; reach it because XHTML chapters rarely declare them.
  XML_SetDefaultHandlerExpand(parser_, &ChapterScanner::onDefault);
  return true;
}

void ChapterScanner::end() { destroyXmlParser(parser_); }

size_t ChapterScanner::write(const uint8_t c) { return write(&c, 1); }

size_t ChapterScanner::write(const uint8_t* buffer, const size_t size) {
  if (!parser_ || stopped_ || parseFailed_) return 0;
  if (XML_Parse(parser_, reinterpret_cast<const char*>(buffer), static_cast<int>(size), XML_FALSE) != XML_STATUS_OK) {
    // A hit callback may have stopped the parser from inside XML_Parse().
    // cppcheck-suppress knownConditionTrueFalse
    if (!stopped_) {
      LOG_ERR("BTS", "XML parse error at line %lu: %s", static_cast<unsigned long>(XML_GetCurrentLineNumber(parser_)),
              XML_ErrorString(XML_GetErrorCode(parser_)));
      parseFailed_ = true;
    }
    return 0;
  }
  // cppcheck-suppress knownConditionTrueFalse
  return stopped_ ? 0 : size;
}

bool ChapterScanner::finish() {
  if (parser_ && !stopped_ && !parseFailed_ && XML_Parse(parser_, "", 0, XML_TRUE) != XML_STATUS_OK && !stopped_) {
    LOG_ERR("BTS", "Final XML parse error: %s", XML_ErrorString(XML_GetErrorCode(parser_)));
    parseFailed_ = true;
  }
  if (hasPending_) finalizePending(false);
  return !parseFailed_;
}

void XMLCALL ChapterScanner::onStartElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  static_cast<ChapterScanner*>(userData)->startElement(name, atts);
}

void XMLCALL ChapterScanner::onEndElement(void* userData, const XML_Char* name) {
  static_cast<ChapterScanner*>(userData)->endElement(name);
}

void XMLCALL ChapterScanner::onCharacterData(void* userData, const XML_Char* text, const int len) {
  static_cast<ChapterScanner*>(userData)->characterData(text, len);
}

void XMLCALL ChapterScanner::onDefault(void* userData, const XML_Char* text, const int len) {
  // Mirrors ChapterHtmlSlimParser::defaultHandlerExpand(): known entities count
  // as their expansion, unknown ones as their literal "&name;" text, and any
  // other unhandled markup (comments, doctype) is ignored.
  if (len < 3 || text[0] != '&' || text[len - 1] != ';') return;
  auto* self = static_cast<ChapterScanner*>(userData);
  const char* value = lookupHtmlEntity(text, static_cast<size_t>(len));
  if (value) {
    self->characterData(value, static_cast<int>(strlen(value)));
  } else {
    self->characterData(text, len);
  }
}

void ChapterScanner::skipCurrentElement() {
  skipUntilDepth_ = depth_;
  depth_++;
}

void ChapterScanner::startElement(const char* name, const char** atts) {
  if (stopped_) return;
  if (skipUntilDepth_ < depth_) {
    depth_++;
    return;
  }
  if (matchesAny(name, BOUNDARY_TAGS, std::size(BOUNDARY_TAGS))) boundary();

  // Same order and conditions as ChapterHtmlSlimParser::startElement().
  if (getAttribute(atts, "hidden") != nullptr) {
    skipCurrentElement();
    return;
  }
  if (attributeContainsToken(getAttribute(atts, "role"), "doc-pagebreak") ||
      attributeContainsToken(getAttribute(atts, "epub:type"), "pagebreak")) {
    skipCurrentElement();
    return;
  }
  // The parser resolves styles (inline ones included) only with publisher styles on.
  if (options_.embeddedStyle && inlineStyleHidesElement(getAttribute(atts, "style"))) {
    skipCurrentElement();
    return;
  }
  if (strcmp(name, "table") == 0) tableDepth_++;
  if (isImageTag(name)) {
    imageElement(atts);
    return;
  }
  if (matchesAny(name, SKIPPED_TAGS, std::size(SKIPPED_TAGS))) {
    skipCurrentElement();
    return;
  }
  depth_++;
}

void ChapterScanner::imageElement(const char** atts) {
  // Same attribute resolution as ChapterHtmlSlimParser: "src" wins, otherwise
  // the first href / xlink:href (SVG <image>).
  std::string_view src;
  const char* alt = nullptr;
  bool kindleFallback = false;
  if (atts) {
    for (size_t i = 0; atts[i]; i += 2) {
      const char* value = atts[i + 1] ? atts[i + 1] : "";
      if (strcmp(atts[i], "src") == 0) {
        src = value;
      } else if (src.empty() && (strcmp(atts[i], "href") == 0 || strcmp(atts[i], "xlink:href") == 0)) {
        src = value;
      } else if (strcmp(atts[i], "alt") == 0) {
        alt = value;
      } else if (strncmp(atts[i], "data-AmznRemoved-M8", 19) == 0) {
        kindleFallback = true;
      }
    }
  }
  const bool hasAlt = alt && alt[0] != '\0';

  // Inside a top-level table the parser always replaces a picture with its alt
  // text, whatever the Images setting.
  if (tableDepth_ == 1) {
    if (hasAlt) {
      characterData(alt, static_cast<int>(strlen(alt)));
      boundary();
    }
    skipCurrentElement();
    return;
  }

  if (kindleFallback || options_.imageRendering == 2) {
    skipCurrentElement();
    return;
  }
  const size_t fragment = src.find('#');
  if (fragment != std::string_view::npos) src = src.substr(0, fragment);
  if (!src.empty() && options_.imageRendering != 1) {
    if (isSvgPath(src)) {
      skipCurrentElement();
      return;
    }
    if (isDrawableImagePath(src)) {
      // Drawn as a picture: no text. (A picture that then fails to decode
      // falls back to alt text on the device; that cannot be predicted here.)
      depth_++;
      return;
    }
  }
  // Placeholder mode, no source, or a format the reader cannot draw: the parser
  // shows "[Image: alt]" as its own line of text.
  if (hasAlt) {
    constexpr char PREFIX[] = "[Image: ";
    boundary();
    characterData(PREFIX, static_cast<int>(sizeof(PREFIX) - 1));
    characterData(alt, static_cast<int>(strlen(alt)));
    characterData("]", 1);
    boundary();
  }
  skipCurrentElement();
}

void ChapterScanner::endElement(const char* name) {
  if (stopped_) return;
  if (depth_ > 0) depth_--;
  if (skipUntilDepth_ < depth_) return;
  if (skipUntilDepth_ == depth_) {
    skipUntilDepth_ = INT_MAX;
    return;
  }
  if (strcmp(name, "table") == 0 && tableDepth_ > 0) tableDepth_--;
  if (matchesAny(name, BOUNDARY_TAGS, std::size(BOUNDARY_TAGS))) boundary();
}

void ChapterScanner::characterData(const char* text, const int len) {
  if (stopped_ || len <= 0) return;
  // The parser drops nested tables entirely and never counts their text.
  if (tableDepth_ > 1 || skipUntilDepth_ < depth_) return;

  const auto* pos = reinterpret_cast<const unsigned char*>(text);
  const auto* const limit = pos + len;
  while (pos < limit) {
    const auto* const before = pos;
    const uint32_t cp = utf8NextCodepoint(&pos);
    // Embedded NUL: expat never produces one, but never spin.
    // cppcheck-suppress knownConditionTrueFalse
    if (pos == before) pos++;
    const uint32_t offset = visibleOffset_++;
    if (!stopped_) pushCodepoint(cp, offset);
  }
}

void ChapterScanner::boundary() {
  if (lastWasSpace_) return;
  lastWasSpace_ = true;
  pushUnit(' ', ' ', visibleOffset_);
}

void ChapterScanner::pushCodepoint(const uint32_t cp, const uint32_t offset) {
  if (isSearchWhitespace(cp)) {
    if (lastWasSpace_) return;
    lastWasSpace_ = true;
    pushUnit(' ', ' ', offset);
    return;
  }
  uint32_t folded[MAX_FOLD_UNITS];
  const size_t count = foldCodepoint(cp, folded);
  if (count == 0) return;
  lastWasSpace_ = false;
  for (size_t i = 0; i < count && !stopped_; ++i) {
    pushUnit(folded[i], i == 0 ? cp : 0, offset);
  }
}

void ChapterScanner::pushUnit(const uint32_t folded, const uint32_t display, const uint32_t offset) {
  Unit& slot = ring_[unitCount_ % RING_UNITS];
  slot = Unit{folded, display, offset};
  unitCount_++;
  if (hasPending_) appendAfterContext(slot);
  if (stopped_) return;

  // Knuth-Morris-Pratt step. Matches do not overlap: after a hit the search
  // restarts from scratch, so "aa" counts twice in "aaaa", not three times.
  const Query& query = *query_;
  while (matched_ > 0 && query.unit(matched_) != folded) matched_ = query.fallback(matched_ - 1);
  if (query.unit(matched_) == folded) matched_++;
  if (matched_ == query.length()) {
    matched_ = 0;
    if (hasPending_) finalizePending(true);
    if (!stopped_) startPendingHit();
  }
}

void ChapterScanner::startPendingHit() {
  const uint32_t length = query_->length();
  const uint32_t first = unitCount_ - length;
  const uint32_t last = unitCount_ - 1;
  const uint32_t oldest = unitCount_ > RING_UNITS ? unitCount_ - RING_UNITS : 0;
  uint32_t from = first > CONTEXT_BEFORE_UNITS ? first - CONTEXT_BEFORE_UNITS : 0;
  if (from < oldest) from = oldest;
  const bool cutBefore = from > 0;
  if (cutBefore) {
    // Start the leading context on a word boundary instead of mid-word.
    for (uint32_t i = from; i < first; ++i) {
      if (unitAt(i).folded == ' ') {
        from = i + 1;
        break;
      }
    }
  }
  while (from < first && unitAt(from).folded == ' ') from++;

  pending_.visibleTextOffset = unitAt(first).offset;
  pending_.snippet[0] = '\0';
  pendingBytes_ = 0;
  pendingLastSpaceBytes_ = 0;
  hasPending_ = true;
  if (cutBefore) appendSnippet(ELLIPSIS, ELLIPSIS_BYTES);
  for (uint32_t i = from; i <= last; ++i) appendDisplay(unitAt(i).display);
  pendingMatchEndBytes_ = pendingBytes_;
  pendingAfterRemaining_ = CONTEXT_AFTER_UNITS;
}

bool ChapterScanner::appendSnippet(const char* bytes, const size_t len) {
  // Keep room for a trailing ellipsis and the terminator.
  if (pendingBytes_ + len + ELLIPSIS_BYTES + 1 > SNIPPET_BYTES) return false;
  memcpy(pending_.snippet + pendingBytes_, bytes, len);
  pendingBytes_ += len;
  return true;
}

bool ChapterScanner::appendDisplay(const uint32_t cp) {
  if (cp == 0) return true;
  char encoded[4] = {};
  return appendSnippet(encoded, encodeUtf8(cp, encoded));
}

void ChapterScanner::appendAfterContext(const Unit& unit) {
  if (pendingAfterRemaining_ == 0) return;
  if (unit.folded == ' ') pendingLastSpaceBytes_ = pendingBytes_;
  const bool fitted = appendDisplay(unit.display);
  pendingAfterRemaining_--;
  if (!fitted || pendingAfterRemaining_ == 0) finalizePending(true);
}

void ChapterScanner::finalizePending(const bool textContinues) {
  if (!hasPending_) return;
  hasPending_ = false;
  // Do not end the snippet in the middle of a word that continues off-screen.
  if (textContinues && pendingLastSpaceBytes_ > pendingMatchEndBytes_) pendingBytes_ = pendingLastSpaceBytes_;
  while (pendingBytes_ > pendingMatchEndBytes_ && pending_.snippet[pendingBytes_ - 1] == ' ') pendingBytes_--;
  if (textContinues) {
    memcpy(pending_.snippet + pendingBytes_, ELLIPSIS, ELLIPSIS_BYTES);
    pendingBytes_ += ELLIPSIS_BYTES;
  }
  pending_.snippet[pendingBytes_] = '\0';
  if (onHit_ && !onHit_(pending_, context_)) stop();
}

void ChapterScanner::stop() {
  stopped_ = true;
  if (parser_) XML_StopParser(parser_, XML_FALSE);
}

}  // namespace booksearch
