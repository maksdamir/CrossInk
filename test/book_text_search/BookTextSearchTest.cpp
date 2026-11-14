#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "BookTextSearch.h"

namespace {

struct FoundHit {
  uint32_t offset;
  std::string snippet;
};

struct Collector {
  std::vector<FoundHit> hits;
  size_t stopAfter = 0;  // 0 = never stop

  static bool onHit(const booksearch::Hit& hit, void* context) {
    auto* self = static_cast<Collector*>(context);
    self->hits.push_back({hit.visibleTextOffset, hit.snippet});
    return self->stopAfter == 0 || self->hits.size() < self->stopAfter;
  }
};

struct ScanResult {
  std::vector<FoundHit> hits;
  uint32_t visibleLength = 0;
  bool parsed = false;
  bool stopped = false;
};

// Feeds `xhtml` in `chunk`-byte writes, the way Epub::readItemContentsToStream
// does, honoring the scanner's early-stop signal.
ScanResult scan(const std::string& xhtml, const char* query, const size_t chunk = 1024, const size_t stopAfter = 0,
                const uint8_t imageRendering = 0, const bool embeddedStyle = true) {
  booksearch::Query q;
  EXPECT_TRUE(q.assign(query)) << query;
  Collector collector;
  collector.stopAfter = stopAfter;
  booksearch::ChapterScanner scanner;
  booksearch::ScanOptions options;
  options.imageRendering = imageRendering;
  options.embeddedStyle = embeddedStyle;
  EXPECT_TRUE(scanner.begin(q, &Collector::onHit, &collector, options));
  size_t offset = 0;
  while (offset < xhtml.size()) {
    const size_t size = std::min(chunk, xhtml.size() - offset);
    if (scanner.write(reinterpret_cast<const uint8_t*>(xhtml.data() + offset), size) != size) break;
    offset += size;
  }
  ScanResult result;
  result.parsed = scanner.finish();
  result.stopped = scanner.stopped();
  result.visibleLength = scanner.visibleTextLength();
  result.hits = collector.hits;
  return result;
}

std::string doc(const std::string& body) {
  return "<html><head><title>T</title></head><body>" + body + "</body></html>";
}

// Visible offset of `needle` inside the visible text `visible` (codepoints).
uint32_t codepointOffset(const std::string& visible, const std::string& needle) {
  const size_t byteOffset = visible.find(needle);
  EXPECT_NE(byteOffset, std::string::npos);
  uint32_t count = 0;
  for (size_t i = 0; i < byteOffset; ++i) {
    if ((static_cast<unsigned char>(visible[i]) & 0xC0) != 0x80) count++;
  }
  return count;
}

}  // namespace

TEST(BookTextSearchQuery, FoldsCollapsesAndTrims) {
  booksearch::Query q;
  ASSERT_TRUE(q.assign("  Caf\xC3\xA9   NOIR "));
  const std::string expected = "cafe noir";
  ASSERT_EQ(q.length(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(q.unit(i), static_cast<uint32_t>(expected[i]));
}

TEST(BookTextSearchQuery, RejectsQueriesWithNothingSearchable) {
  booksearch::Query q;
  EXPECT_FALSE(q.assign(""));
  EXPECT_FALSE(q.assign("   \t "));
  EXPECT_FALSE(q.assign("\xC2\xAD"));  // soft hyphen only
  EXPECT_FALSE(q.assign(nullptr));
}

TEST(BookTextSearchQuery, BuildsKmpFallbackTable) {
  booksearch::Query q;
  ASSERT_TRUE(q.assign("abab"));
  EXPECT_EQ(q.fallback(0), 0);
  EXPECT_EQ(q.fallback(1), 0);
  EXPECT_EQ(q.fallback(2), 1);
  EXPECT_EQ(q.fallback(3), 2);
}

TEST(BookTextSearchFold, FoldsCaseAccentsAndPunctuation) {
  uint32_t out[booksearch::MAX_FOLD_UNITS];
  ASSERT_EQ(booksearch::foldCodepoint('Q', out), 1u);
  EXPECT_EQ(out[0], static_cast<uint32_t>('q'));
  ASSERT_EQ(booksearch::foldCodepoint(0x00C9, out), 1u);  // É
  EXPECT_EQ(out[0], static_cast<uint32_t>('e'));
  ASSERT_EQ(booksearch::foldCodepoint(0x1EBF, out), 1u);  // ế
  EXPECT_EQ(out[0], static_cast<uint32_t>('e'));
  ASSERT_EQ(booksearch::foldCodepoint(0x00DF, out), 2u);  // ß
  EXPECT_EQ(out[0], static_cast<uint32_t>('s'));
  EXPECT_EQ(out[1], static_cast<uint32_t>('s'));
  ASSERT_EQ(booksearch::foldCodepoint(0x2019, out), 1u);  // ’
  EXPECT_EQ(out[0], static_cast<uint32_t>('\''));
  ASSERT_EQ(booksearch::foldCodepoint(0x2014, out), 1u);  // —
  EXPECT_EQ(out[0], static_cast<uint32_t>('-'));
  ASSERT_EQ(booksearch::foldCodepoint(0xFB03, out), 3u);  // ﬃ
  ASSERT_EQ(booksearch::foldCodepoint(0x0410, out), 1u);  // А
  EXPECT_EQ(out[0], 0x0430u);
  ASSERT_EQ(booksearch::foldCodepoint(0x0401, out), 1u);  // Ё
  EXPECT_EQ(out[0], 0x0435u);
  ASSERT_EQ(booksearch::foldCodepoint(0x0386, out), 1u);  // Ά
  EXPECT_EQ(out[0], 0x03B1u);
  ASSERT_EQ(booksearch::foldCodepoint(0x03C2, out), 1u);  // ς
  EXPECT_EQ(out[0], 0x03C3u);
  EXPECT_EQ(booksearch::foldCodepoint(0x0301, out), 0u);  // combining acute
  EXPECT_EQ(booksearch::foldCodepoint(0x00AD, out), 0u);  // soft hyphen
  ASSERT_EQ(booksearch::foldCodepoint(0x4E2D, out), 1u);  // 中 passes through
  EXPECT_EQ(out[0], 0x4E2Du);
}

TEST(BookTextSearchScan, ReportsVisibleOffsetOfMatch) {
  const auto result = scan(doc("<p>Hello world</p>"), "world");
  ASSERT_EQ(result.hits.size(), 1u);
  // "T" in <title> is skipped; offsets start at the body text.
  EXPECT_EQ(result.hits[0].offset, 6u);
  EXPECT_TRUE(result.parsed);
  EXPECT_EQ(result.visibleLength, 11u);
}

TEST(BookTextSearchScan, CountsWhitespaceOutsideBodyLikeTheLayoutParser) {
  // Text nodes between elements count toward visible offsets even outside
  // <body>: ChapterHtmlSlimParser counts every unskipped codepoint.
  const auto result = scan("<html>\n<head><title>x</title></head>\n<body><p>needle</p></body></html>", "needle");
  ASSERT_EQ(result.hits.size(), 1u);
  EXPECT_EQ(result.hits[0].offset, 2u);
}

TEST(BookTextSearchScan, MatchesCaseAndAccentInsensitively) {
  const auto result = scan(doc("<p>Le CAF\xC3\x89 de la gare</p>"), "caf\xC3\xA9");
  ASSERT_EQ(result.hits.size(), 1u);
  EXPECT_EQ(result.hits[0].offset, 3u);
  EXPECT_EQ(scan(doc("<p>Le CAF\xC3\x89 de la gare</p>"), "cafe").hits.size(), 1u);
}

TEST(BookTextSearchScan, MatchesAcrossInlineMarkupAndCollapsedWhitespace) {
  EXPECT_EQ(scan(doc("<p>light<b>house</b> keeper</p>"), "lighthouse").hits.size(), 1u);
  EXPECT_EQ(scan(doc("<p>the\n   old\t<i>lighthouse</i></p>"), "the old lighthouse").hits.size(), 1u);
}

TEST(BookTextSearchScan, TreatsBlockBoundariesAsSpaces) {
  EXPECT_EQ(scan(doc("<p>end.</p><p>Next</p>"), "end. next").hits.size(), 1u);
  EXPECT_EQ(scan(doc("<p>end</p><p>next</p>"), "endnext").hits.size(), 0u);
  EXPECT_EQ(scan(doc("<p>line<br/>break</p>"), "line break").hits.size(), 1u);
}

TEST(BookTextSearchScan, SkipsContentTheLayoutParserHides) {
  const std::string body =
      "<p>A<span hidden=\"\">secret</span>"
      "<span epub:type=\"pagebreak\" title=\"12\">secret</span>"
      "<span role=\"doc-pagebreak\">secret</span>"
      "<span style=\"color: red; display : NONE !important\">secret</span>"
      "<ruby>\xE6\xBC\xA2<rp>(</rp><rt>secret</rt><rp>)</rp></ruby>"
      "<script>secret</script><style>.secret{}</style>"
      "<table><tr><td>shown<table><tr><td>secret</td></tr></table></td></tr></table>"
      "B target</p>";
  const auto hidden = scan(doc(body), "secret");
  EXPECT_TRUE(hidden.hits.empty());
  const auto visible = scan(doc(body), "target");
  ASSERT_EQ(visible.hits.size(), 1u);
  // Visible text: "A" + "漢" + "shown" + "B target"
  EXPECT_EQ(visible.hits[0].offset, codepointOffset("A\xE6\xBC\xA2shownB target", "target"));
}

TEST(BookTextSearchScan, KeepsCountingAfterDisplayNoneIsOverridden) {
  const auto result = scan(doc("<p><span style=\"display:none; display:inline\">shown</span></p>"), "shown");
  EXPECT_EQ(result.hits.size(), 1u);
}

TEST(BookTextSearchScan, ExpandsHtmlEntitiesLikeTheLayoutParser) {
  // &nbsp; and &mdash; are one visible codepoint each; an unknown entity keeps
  // its literal text, as ChapterHtmlSlimParser::defaultHandlerExpand() does.
  const std::string xhtml =
      "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">" +
      doc("<p>a&nbsp;b&mdash;c &bogus; target</p>");
  const auto result = scan(xhtml, "target");
  ASSERT_EQ(result.hits.size(), 1u);
  EXPECT_EQ(result.hits[0].offset, codepointOffset("a\xC2\xA0"
                                                   "b\xE2\x80\x94"
                                                   "c &bogus; target",
                                                   "target"));
  EXPECT_EQ(scan(xhtml, "a b-c").hits.size(), 1u);
}

TEST(BookTextSearchScan, IgnoresSoftHyphensButCountsThem) {
  const auto result = scan(doc("<p>hyphen\xC2\xAD"
                               "ation target</p>"),
                           "hyphenation");
  ASSERT_EQ(result.hits.size(), 1u);
  EXPECT_EQ(result.hits[0].offset, 0u);
  EXPECT_EQ(scan(doc("<p>hyphen\xC2\xAD"
                     "ation target</p>"),
                 "target")
                .hits[0]
                .offset,
            13u);
}

TEST(BookTextSearchScan, FoldsMultiLetterCharactersBothWays) {
  EXPECT_EQ(scan(doc("<p>Die Stra\xC3\x9F"
                     "e</p>"),
                 "strasse")
                .hits.size(),
            1u);
  EXPECT_EQ(scan(doc("<p>DIE STRASSE</p>"),
                 "stra\xC3\x9F"
                 "e")
                .hits.size(),
            1u);
  EXPECT_EQ(scan(doc("<p>the \xEF\xAC\x81"
                     "nal word</p>"),
                 "final")
                .hits.size(),
            1u);
}

TEST(BookTextSearchScan, MatchesTypographicQuotesAndDashes) {
  EXPECT_EQ(scan(doc("<p>I don\xE2\x80\x99t know</p>"), "don't").hits.size(), 1u);
  EXPECT_EQ(scan(doc("<p>well\xE2\x80\x94known</p>"), "well-known").hits.size(), 1u);
}

TEST(BookTextSearchScan, MatchesGreekAndCyrillicWithoutCase) {
  EXPECT_EQ(scan(doc("<p>\xCE\x86\xCE\x98\xCE\x97\xCE\x9D\xCE\x91</p>"), "\xCE\xB1\xCE\xB8\xCE\xB7\xCE\xBD\xCE\xB1")
                .hits.size(),
            1u);  // ΆΘΗΝΑ vs αθηνα
  EXPECT_EQ(scan(doc("<p>\xD0\x81\xD0\xBB\xD0\xBA\xD0\xB0</p>"), "\xD0\xB5\xD0\xBB\xD0\xBA\xD0\xB0").hits.size(),
            1u);  // Ёлка vs елка
}

TEST(BookTextSearchScan, ReportsEveryNonOverlappingHitInOrder) {
  const auto result = scan(doc("<p>aaaa</p>"), "aa");
  ASSERT_EQ(result.hits.size(), 2u);
  EXPECT_EQ(result.hits[0].offset, 0u);
  EXPECT_EQ(result.hits[1].offset, 2u);

  const auto words = scan(doc("<p>one fish</p><p>two fish</p><p>red fish</p>"), "fish");
  ASSERT_EQ(words.hits.size(), 3u);
  EXPECT_LT(words.hits[0].offset, words.hits[1].offset);
  EXPECT_LT(words.hits[1].offset, words.hits[2].offset);
}

TEST(BookTextSearchScan, StopsWhenTheCallbackDeclinesMoreHits) {
  const auto result = scan(doc("<p>x x x x x</p>"), "x", 1024, 2);
  EXPECT_EQ(result.hits.size(), 2u);
  EXPECT_TRUE(result.stopped);
  EXPECT_TRUE(result.parsed);
}

TEST(BookTextSearchScan, GivesIdenticalResultsForAnyChunkSize) {
  // Named HTML entities need a DOCTYPE; without one expat rejects them as
  // undefined, exactly as it does for ChapterHtmlSlimParser.
  const std::string xhtml =
      "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">" +
      doc("<p>Caf\xC3\xA9 au lait, caf\xC3\xA9 noir&nbsp;and CAFE cr\xC3\xA8me.</p><p>caf\xC3\xA9</p>");
  const auto whole = scan(xhtml, "cafe", 4096);
  const auto bytewise = scan(xhtml, "cafe", 1);
  ASSERT_EQ(whole.hits.size(), 4u);
  ASSERT_EQ(bytewise.hits.size(), whole.hits.size());
  for (size_t i = 0; i < whole.hits.size(); ++i) {
    EXPECT_EQ(bytewise.hits[i].offset, whole.hits[i].offset);
    EXPECT_EQ(bytewise.hits[i].snippet, whole.hits[i].snippet);
  }
  EXPECT_EQ(bytewise.visibleLength, whole.visibleLength);
}

TEST(BookTextSearchScan, BuildsSnippetWithContextAndEllipses) {
  const auto shortText = scan(doc("<p>The old lighthouse stood.</p>"), "lighthouse");
  ASSERT_EQ(shortText.hits.size(), 1u);
  EXPECT_EQ(shortText.hits[0].snippet, "The old lighthouse stood.");

  const std::string longText =
      "Far across the cold grey water of the northern bay the lighthouse keeper watched the "
      "ships come home one by one through the evening fog and the rain that would not stop.";
  const auto cut = scan(doc("<p>" + longText + "</p>"), "lighthouse");
  ASSERT_EQ(cut.hits.size(), 1u);
  const std::string& snippet = cut.hits[0].snippet;
  EXPECT_LT(snippet.size(), booksearch::SNIPPET_BYTES);
  EXPECT_EQ(snippet.rfind("\xE2\x80\xA6", 0), 0u) << snippet;  // leading ellipsis
  EXPECT_NE(snippet.find("lighthouse keeper"), std::string::npos) << snippet;
  EXPECT_EQ(snippet.compare(snippet.size() - 3, 3, "\xE2\x80\xA6"), 0) << snippet;  // trailing ellipsis
  // Context starts and ends on whole words.
  EXPECT_EQ(snippet.find(" \xE2\x80\xA6"), std::string::npos) << snippet;
  const std::string afterLead = snippet.substr(3);
  EXPECT_NE(longText.find(afterLead.substr(0, afterLead.size() - 3)), std::string::npos) << snippet;
}

TEST(BookTextSearchScan, SnippetsShowWhitespaceCollapsed) {
  const auto result = scan(doc("<p>alpha</p>\n\n<p>  beta   gamma</p>"), "beta");
  ASSERT_EQ(result.hits.size(), 1u);
  EXPECT_EQ(result.hits[0].snippet, "alpha beta gamma");
}

TEST(BookTextSearchScan, ReportsHitsBeforeAParseError) {
  const auto result = scan("<html><body><p>found it</p><p>broken</q></body></html>", "found");
  EXPECT_EQ(result.hits.size(), 1u);
  EXPECT_FALSE(result.parsed);
}

TEST(BookTextSearchScan, CanBeReusedForTheNextChapter) {
  booksearch::Query q;
  ASSERT_TRUE(q.assign("word"));
  Collector collector;
  booksearch::ChapterScanner scanner;
  for (int chapter = 0; chapter < 3; ++chapter) {
    const std::string xhtml = doc("<p>a word</p>");
    ASSERT_TRUE(scanner.begin(q, &Collector::onHit, &collector));
    EXPECT_EQ(scanner.write(reinterpret_cast<const uint8_t*>(xhtml.data()), xhtml.size()), xhtml.size());
    EXPECT_TRUE(scanner.finish());
    scanner.end();
  }
  ASSERT_EQ(collector.hits.size(), 3u);
  for (const auto& hit : collector.hits) EXPECT_EQ(hit.offset, 2u);
}

TEST(BookTextSearchScan, CountsTableImageAltTextAsTheParserDoes) {
  // Inside a top-level table the parser replaces a picture with its alt text,
  // appended to the surrounding word, in every Images mode.
  for (const uint8_t mode : {0, 1, 2}) {
    const auto result =
        scan(doc("<table><tr><td>before<img src=\"a.jpg\" alt=\"Figure\"/>after target</td></tr></table>"), "target",
             1024, 0, mode);
    ASSERT_EQ(result.hits.size(), 1u) << int(mode);
    EXPECT_EQ(result.hits[0].offset, codepointOffset("beforeFigureafter target", "target")) << int(mode);
    EXPECT_EQ(scan(doc("<table><tr><td><img alt=\"Figure\"/></td></tr></table>"), "figure", 1024, 0, mode).hits.size(),
              1u);
  }
}

TEST(BookTextSearchScan, CountsImagePlaceholdersOnlyWhenTheParserShowsThem) {
  const std::string placeholder = "[Image: Map]";
  struct Case {
    const char* image;
    uint8_t mode;
    bool showsAlt;
  };
  const Case cases[] = {
      {"<img src=\"pics/map.jpg\" alt=\"Map\"/>", 0, false},          // drawn
      {"<img src=\"pics/map.PNG#frag\" alt=\"Map\"/>", 0, false},     // drawn
      {"<img src=\"pics/map.jpg\" alt=\"Map\"/>", 1, true},           // placeholder setting
      {"<img src=\"pics/map.jpg\" alt=\"Map\"/>", 2, false},          // suppressed
      {"<img src=\"pics/map.gif\" alt=\"Map\"/>", 0, true},           // cannot draw GIF
      {"<img src=\"pics/map.png?v=2\" alt=\"Map\"/>", 0, true},       // extension check sees the query
      {"<img alt=\"Map\"/>", 0, true},                                // no source
      {"<img src=\"pics/map.svg\" alt=\"Map\"/>", 0, false},          // SVG skipped
      {"<image xlink:href=\"pics/map.gif\" alt=\"Map\"/>", 0, true},  // SVG <image> element
      {"<img src=\"pics/map.gif\" data-AmznRemoved-M8=\"true\" alt=\"Map\"/>", 0, false},  // Kindle fallback
      {"<img src=\"pics/map.gif\" alt=\"\"/>", 0, false},                                  // empty alt
  };
  for (const auto& c : cases) {
    const auto result = scan(doc(std::string("<p>a</p>") + c.image + "<p>target</p>"), "target", 1024, 0, c.mode);
    ASSERT_EQ(result.hits.size(), 1u) << c.image << " mode " << int(c.mode);
    const uint32_t expected = 1 + (c.showsAlt ? static_cast<uint32_t>(placeholder.size()) : 0);
    EXPECT_EQ(result.hits[0].offset, expected) << c.image << " mode " << int(c.mode);
    EXPECT_EQ(scan(doc(std::string("<p>a</p>") + c.image), "image: map", 1024, 0, c.mode).hits.size(),
              c.showsAlt ? 1u : 0u)
        << c.image << " mode " << int(c.mode);
  }
}

TEST(BookTextSearchScan, HonorsInlineDisplayNoneOnlyWithPublisherStyles) {
  // Without publisher styles the parser never resolves element styles, so an
  // inline display:none does not hide anything and its text is counted.
  const std::string xhtml = doc("<p>a<span style=\"display:none\">secret</span>b target</p>");
  const auto styled = scan(xhtml, "target", 1024, 0, 0, true);
  ASSERT_EQ(styled.hits.size(), 1u);
  EXPECT_EQ(styled.hits[0].offset, codepointOffset("ab target", "target"));
  EXPECT_TRUE(scan(xhtml, "secret", 1024, 0, 0, true).hits.empty());

  const auto plain = scan(xhtml, "target", 1024, 0, 0, false);
  ASSERT_EQ(plain.hits.size(), 1u);
  EXPECT_EQ(plain.hits[0].offset, codepointOffset("asecretb target", "target"));
  EXPECT_EQ(scan(xhtml, "secret", 1024, 0, 0, false).hits.size(), 1u);
  // The hidden attribute and page-break markers are skipped either way.
  EXPECT_TRUE(scan(doc("<p>a<span hidden=\"\">secret</span>b</p>"), "secret", 1024, 0, 0, false).hits.empty());
}

TEST(BookTextSearchScanOptions, CompareByEveryField) {
  booksearch::ScanOptions a;
  booksearch::ScanOptions b;
  EXPECT_EQ(a, b);
  b.imageRendering = 1;
  EXPECT_NE(a, b);
  b = a;
  b.embeddedStyle = false;
  EXPECT_NE(a, b);
}
