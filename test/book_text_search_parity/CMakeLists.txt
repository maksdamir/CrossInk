# Runs ChapterHtmlSlimParser and booksearch::ChapterScanner over the same XHTML
# and requires identical visible-text counts. Reuses the layout parser test's
# stubs, and builds the bundled expat so no system package is needed.
set(PARSER_TEST_DIR ${REPO_ROOT}/test/chapter_html_slim_parser)

add_executable(BookTextSearchParityTest
  BookTextSearchParityTest.cpp
  ${PARSER_TEST_DIR}/ParserLinkStubs.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/Epub/Epub/search/BookTextSearch.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)

target_include_directories(BookTextSearchParityTest PRIVATE
  ${PARSER_TEST_DIR}/stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Epub/Epub/parsers
  ${REPO_ROOT}/lib/Epub/Epub/css
  ${REPO_ROOT}/lib/Epub/Epub/search
  ${REPO_ROOT}/lib/Epub/Epub/tables
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/expat
)

target_compile_definitions(BookTextSearchParityTest PRIVATE
  XML_CONTEXT_BYTES=1024
  XML_GE=0
  CROSSINK_FIXTURE_ROOT="${REPO_ROOT}/test/epubs-src"
)

target_link_libraries(BookTextSearchParityTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookTextSearchParityTest)
