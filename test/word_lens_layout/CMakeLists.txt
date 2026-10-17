add_executable(WordLensLayoutTest
  WordLensLayoutTest.cpp
  ${REPO_ROOT}/test/chapter_html_slim_parser/ParserLinkStubs.cpp
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
  ${REPO_ROOT}/src/modules/lingua/modes/word_lens/WordLensLayout.cpp
  ${REPO_ROOT}/src/modules/lingua/modes/word_lens/WordLensStrip.cpp
  ${REPO_ROOT}/src/modules/lingua/modes/word_lens/WordLensNavigation.cpp
  ${REPO_ROOT}/src/modules/lingua/wordlens/LensText.cpp
)

target_include_directories(WordLensLayoutTest PRIVATE
  # The parser suite's renderer/storage stand-ins: 8 px per glyph, 16 px lines.
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
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

target_link_libraries(WordLensLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  expat
)

gtest_discover_tests(WordLensLayoutTest)
