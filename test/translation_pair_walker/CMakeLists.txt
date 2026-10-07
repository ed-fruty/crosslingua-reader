add_executable(TranslationPairWalkerTest
  TranslationPairWalkerTest.cpp
  ${REPO_ROOT}/src/modules/lingua/parsers/TranslationPairWalker.cpp
  ${REPO_ROOT}/src/modules/lingua/services/TranslatedContentDetector.cpp
  ${REPO_ROOT}/src/modules/lingua/utils/TextNormalize.cpp
)

target_include_directories(TranslationPairWalkerTest PRIVATE
  # HalStorage/Logging stubs are shared with the parser suite rather than copied.
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  # Sources include as "modules/lingua/..." and <Epub/parsers/ParagraphBoundary.h>.
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(TranslationPairWalkerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  expat
)

gtest_discover_tests(TranslationPairWalkerTest)
