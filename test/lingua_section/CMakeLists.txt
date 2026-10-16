# CrossLingua fork: Section-level tests for translated sidecars (source kind, offset spaces).
set(PARSER_TEST_DIR ${REPO_ROOT}/test/chapter_html_slim_parser)

add_executable(LinguaSectionTest
  LinguaSectionTest.cpp
  ${PARSER_TEST_DIR}/ParserLinkStubs.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Section.cpp
  ${REPO_ROOT}/src/modules/lingua/services/LinguaSectionSupport.cpp
  ${REPO_ROOT}/src/modules/lingua/services/TranslatedContentDetector.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Page.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/WordStore.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDimsProbe.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/src/modules/lingua/layout/LinguaTextFlow.cpp
  ${REPO_ROOT}/src/modules/lingua/layout/PairedLayoutCoordinator.cpp
  ${REPO_ROOT}/src/modules/lingua/layout/ParsedTextLingua.cpp
  ${REPO_ROOT}/src/modules/lingua/modes/interlinear/InterlinearLayout.cpp
  ${REPO_ROOT}/src/modules/lingua/modes/side_by_side/SideBySideLayout.cpp
)

target_include_directories(LinguaSectionTest PRIVATE
  # Richer Epub / FontCacheManager stubs first, then the parser suite's shared stubs.
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${PARSER_TEST_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Epub/Epub/parsers
  ${REPO_ROOT}/lib/Epub/Epub/css
  ${REPO_ROOT}/lib/AsciiText
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/XmlParserUtils
)

target_link_libraries(LinguaSectionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  expat
)

gtest_discover_tests(LinguaSectionTest)
