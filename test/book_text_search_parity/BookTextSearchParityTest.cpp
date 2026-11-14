// Parity between booksearch::ChapterScanner and ChapterHtmlSlimParser.
//
// Search hits are resolved to pages through the visible-text offsets the
// layout parser records. If the two ever count characters differently, a hit
// lands on the wrong page, so this test runs both over the same XHTML and
// requires identical totals.
#include <GfxRenderer.h>
#include <gtest/gtest.h>

// Every standard header the parser pulls in must be included before the
// class/private redefinition below, or the library's own templates break.
#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#define class struct
#define private public
#include "Epub/parsers/ChapterHtmlSlimParser.h"
#undef private
#undef class

#include <Epub.h>

#include "BookTextSearch.h"

namespace {

// Built the way Section::startBuild() does: the CSS parser is only handed to
// the layout parser when publisher styles are on.
uint32_t parserVisibleLength(const std::string& xhtml, const uint8_t imageRendering = 0,
                             const bool embeddedStyle = true) {
  Epub epub;
  GfxRenderer renderer;
  CssParser cssParser{"/tmp"};
  const std::string filepath = "unused.xhtml";
  const auto ignorePage = [](std::unique_ptr<Page>, uint16_t, uint16_t, uint32_t, uint32_t) {};
  ChapterHtmlSlimParser parser{epub,
                               filepath,
                               renderer,
                               0,
                               1.0f,
                               false,
                               false,
                               0,
                               480,
                               800,
                               false,
                               false,
                               false,
                               0,
                               ignorePage,
                               embeddedStyle,
                               "",
                               "",
                               imageRendering,
                               {},
                               nullptr,
                               embeddedStyle ? &cssParser : nullptr};
  std::array<ChapterHtmlSlimParser::StyleStackEntry, ChapterHtmlSlimParser::MAX_INLINE_STYLE_DEPTH> inlineStyles{};
  std::array<BlockStyle, ChapterHtmlSlimParser::MAX_BLOCK_STYLE_DEPTH> blockStyles{};
  parser.currentTextBlock = std::make_unique<ParsedText>(false);
  parser.inlineStyleBuf_ = inlineStyles.data();
  parser.blockStyleBuf_ = blockStyles.data();
  parser.blockStyleCount_ = 1;

  // Same handler set ChapterHtmlSlimParser::parseAndBuildPages() installs.
  XML_Parser xml = XML_ParserCreate(nullptr);
  parser.activeParser = xml;
  XML_SetDefaultHandlerExpand(xml, ChapterHtmlSlimParser::defaultHandlerExpand);
  XML_SetUserData(xml, &parser);
  XML_SetElementHandler(xml, ChapterHtmlSlimParser::startElement, ChapterHtmlSlimParser::endElement);
  XML_SetCharacterDataHandler(xml, ChapterHtmlSlimParser::characterData);
  XML_Parse(xml, xhtml.data(), static_cast<int>(xhtml.size()), XML_TRUE);
  XML_ParserFree(xml);
  parser.activeParser = nullptr;
  parser.inlineStyleBuf_ = nullptr;
  parser.blockStyleBuf_ = nullptr;
  return parser.visibleTextOffset;
}

uint32_t scannerVisibleLength(const std::string& xhtml, const uint8_t imageRendering = 0,
                              const bool embeddedStyle = true) {
  booksearch::Query query;
  EXPECT_TRUE(query.assign("\xF0\x9F\x9B\xB8"));  // a codepoint no fixture contains
  booksearch::ChapterScanner scanner;
  booksearch::ScanOptions options;
  options.imageRendering = imageRendering;
  options.embeddedStyle = embeddedStyle;
  EXPECT_TRUE(scanner.begin(query, nullptr, nullptr, options));
  scanner.write(reinterpret_cast<const uint8_t*>(xhtml.data()), xhtml.size());
  scanner.finish();
  return scanner.visibleTextLength();
}

const char* const XHTML_DOCTYPE =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE html PUBLIC \"-//W3C//DTD XHTML 1.1//EN\" \"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd\">\n";

std::vector<std::string> inlineFixtures() {
  const std::string open = std::string(XHTML_DOCTYPE) +
                           "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:epub=\"http://www.idpf.org/2007/ops\">\n"
                           "<head><title>Title</title><style>p{}</style></head>\n<body>\n";
  const std::string close = "\n</body>\n</html>\n";
  return {
      open + "<p>Hello world</p>" + close,
      open + "<p>A<span hidden=\"\">secret</span>B</p><div hidden=\"hidden\"><p>gone</p></div>" + close,
      open +
          "<p>page<span epub:type=\"pagebreak\" title=\"12\">12</span>break <span "
          "role=\"doc-pagebreak\">13</span></p>" +
          close,
      open +
          "<p><span style=\"display:none\">x</span><span style=\"color:red; DISPLAY: none !important\">y</span>z</p>" +
          close,
      open + "<p><ruby>\xE6\xBC\xA2<rp>(</rp><rt>kan</rt><rp>)</rp></ruby>ji</p>" + close,
      open + "<p>a&nbsp;b&mdash;c &amp; d &#8217; e &bogus; f</p>" + close,
      open + "<table><tr><td>outer<table><tr><td>inner</td></tr></table>tail</td></tr></table><p>after</p>" + close,
      open +
          "<p>soft\xC2\xAD"
          "hyphen and \xEF\xAC\x81"
          "ligature, caf\xC3\xA9, \xD0\x81\xD0\xBB\xD0\xBA\xD0\xB0</p>" +
          close,
      open + "<p>before<!-- comment -->after<![CDATA[ raw <text> ]]>end</p><script>var x = 1;</script>" + close,
      open + "<h1>Heading</h1><ul><li>one</li><li>two</li></ul><blockquote><p>quote</p></blockquote><br/>" + close,
      // No DOCTYPE: an undeclared entity stops expat in both parsers at the same point.
      "<html><body><p>first</p><p>a&nbsp;b</p><p>never reached</p></body></html>",
  };
}

std::vector<std::filesystem::path> repoFixtures() {
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(CROSSINK_FIXTURE_ROOT)) {
    const auto ext = entry.path().extension();
    if (entry.is_regular_file() && (ext == ".xhtml" || ext == ".html")) files.push_back(entry.path());
  }
  return files;
}

}  // namespace

TEST(BookTextSearchParity, InlineFixturesCountLikeTheLayoutParser) {
  const auto fixtures = inlineFixtures();
  for (const bool embeddedStyle : {true, false}) {
    for (size_t i = 0; i < fixtures.size(); ++i) {
      EXPECT_EQ(scannerVisibleLength(fixtures[i], 0, embeddedStyle), parserVisibleLength(fixtures[i], 0, embeddedStyle))
          << "inline fixture " << i << " embeddedStyle " << embeddedStyle;
    }
  }
}

TEST(BookTextSearchParity, RepositoryEpubChaptersCountLikeTheLayoutParser) {
  const auto files = repoFixtures();
  ASSERT_FALSE(files.empty()) << "no XHTML fixtures under " << CROSSINK_FIXTURE_ROOT;
  for (const auto& path : files) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream content;
    content << in.rdbuf();
    const std::string xhtml = content.str();
    for (const bool embeddedStyle : {true, false}) {
      const uint32_t parser = parserVisibleLength(xhtml, 0, embeddedStyle);
      EXPECT_GT(parser, 0u) << path;
      EXPECT_EQ(scannerVisibleLength(xhtml, 0, embeddedStyle), parser) << path << " embeddedStyle " << embeddedStyle;
    }
  }
}

TEST(BookTextSearchParity, ImageAltTextCountsLikeTheLayoutParser) {
  const std::string open = std::string(XHTML_DOCTYPE) +
                           "<html xmlns=\"http://www.w3.org/1999/xhtml\" xmlns:xlink=\"http://www.w3.org/1999/xlink\">"
                           "<head><title>T</title></head><body>\n";
  const std::string close = "\n<p>tail</p></body></html>\n";
  // Every case here is decided by markup and the Images setting alone. A JPEG
  // or PNG in display mode is excluded: whether it is drawn or falls back to
  // alt text depends on decoding it, which the host stubs cannot do.
  const std::vector<std::pair<std::string, std::vector<uint8_t>>> fixtures = {
      {"<table><tr><td>cell<img src=\"a.jpg\" alt=\"Chart\"/>text</td></tr></table>", {0, 1, 2}},
      {"<table><tr><td><table><tr><td><img alt=\"Nested\"/></td></tr></table></td></tr></table>", {0, 1, 2}},
      {"<p>a</p><img src=\"pics/map.jpg\" alt=\"Map\"/>", {1, 2}},
      {"<p>a</p><img src=\"pics/map.gif\" alt=\"Map\"/>", {0, 1, 2}},
      {"<p>a</p><img src=\"pics/map.png?v=2\" alt=\"Map\"/>", {0, 1, 2}},
      {"<p>a</p><img alt=\"No source\"/>", {0, 1, 2}},
      {"<p>a</p><img src=\"pics/map.svg\" alt=\"Map\"/>", {0, 1, 2}},
      {"<p>a</p><svg><image xlink:href=\"pics/map.gif\" alt=\"Map\"/></svg>", {0, 1, 2}},
      {"<p>a</p><img src=\"pics/map.gif\" data-AmznRemoved-M8=\"true\" alt=\"Map\"/>", {0, 1, 2}},
      {"<p>a</p><img src=\"pics/map.gif\" alt=\"\"/>", {0, 1, 2}},
      {"<p>a<span hidden=\"\"><img alt=\"Hidden\"/></span></p>", {0, 1, 2}},
  };
  for (const auto& [body, modes] : fixtures) {
    const std::string xhtml = open + body + close;
    for (const uint8_t mode : modes) {
      for (const bool embeddedStyle : {true, false}) {
        EXPECT_EQ(scannerVisibleLength(xhtml, mode, embeddedStyle), parserVisibleLength(xhtml, mode, embeddedStyle))
            << body << " mode " << int(mode) << " embeddedStyle " << embeddedStyle;
      }
    }
  }
}
