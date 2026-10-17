add_executable(WordLensFormatTest
  WordLensFormatTest.cpp
  ${REPO_ROOT}/src/modules/lingua/wordlens/LensFormat.cpp
  ${REPO_ROOT}/src/modules/lingua/wordlens/LensReader.cpp
  ${REPO_ROOT}/src/modules/lingua/wordlens/LensText.cpp
  ${REPO_ROOT}/src/modules/lingua/wordlens/GlossClean.cpp
  ${REPO_ROOT}/src/modules/lingua/wordlens/WordLensGlosser.cpp
  ${REPO_ROOT}/src/modules/lingua/wordlens/LensBuilder.cpp
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp
)

target_include_directories(WordLensFormatTest PRIVATE
  # Logging.h stand-in.
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/include
)

target_compile_definitions(WordLensFormatTest PRIVATE
  WORD_LENS_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

target_link_libraries(WordLensFormatTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(WordLensFormatTest)

# The Python reference implementation: deterministic conversion, fixture reproduction and the
# en-uk translation-cache reuse rules. No network.
add_test(NAME WordLensPythonTools
  COMMAND ${Python3_EXECUTABLE} -m unittest -v test_wordlens
  WORKING_DIRECTORY ${REPO_ROOT}/scripts/wordlens)
