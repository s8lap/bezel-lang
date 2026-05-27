# Test executable. Phase tests are added to BEZEL_TEST_SOURCES as each milestone
# lands its spec-derived tests; the per-phase directories alongside this file
# are placeholders for them.
set(BEZEL_TEST_SOURCES
    test_main.cpp
    sanity_test.cpp
    lexer/identifiers_test.cpp
    lexer/keywords_test.cpp
    lexer/comments_test.cpp
    lexer/whitespace_test.cpp)

add_executable(bezel_tests ${BEZEL_TEST_SOURCES})
target_include_directories(bezel_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(bezel_tests PRIVATE bezel_core bezel_warnings)

add_test(NAME bezel_tests COMMAND bezel_tests)
