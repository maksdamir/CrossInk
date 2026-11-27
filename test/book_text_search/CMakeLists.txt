add_executable(BookTextSearchTest
  BookTextSearchTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/search/BookTextSearch.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)

target_include_directories(BookTextSearchTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub/search
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/expat
)

# Match the firmware's expat configuration (platformio.ini build_flags).
target_compile_definitions(BookTextSearchTest PRIVATE
  XML_CONTEXT_BYTES=0
  XML_GE=0
)

target_link_libraries(BookTextSearchTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookTextSearchTest)
