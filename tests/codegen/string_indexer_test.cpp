// CG27e: string indexer `s[i]` (§4.6, stdlib §9.6).
// `s[i]` returns the i-th char (Unicode code point) as `char` (i32).
// For ASCII strings this equals the byte at position i. Full O(1)
// code-point indexing for multi-byte Unicode is implementation-defined
// and deferred; the current implementation reads the i-th byte.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"
#include "support/aot_support.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

// §4.6: `s[0]` is the first code point (ASCII value 65 for 'A').
BEZEL_TEST(codegen_cg27e, string_index_first_char) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"ABC\"; return s[0]; "
        "end Main; end C;"), 65);  // 'A' = 65
}

// §4.6: `s[1]` is the second code point.
BEZEL_TEST(codegen_cg27e, string_index_second_char) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"ABC\"; return s[1]; "
        "end Main; end C;"), 66);  // 'B' = 66
}

// §4.6: last char of "Hello" is 'o' = 111.
BEZEL_TEST(codegen_cg27e, string_index_last_char) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; return s[4]; "
        "end Main; end C;"), 111);  // 'o' = 111
}

// §4.6: `s[i]` compared to a char literal via `=`.
BEZEL_TEST(codegen_cg27e, string_index_equals_char_literal) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; return (1 if s[0] = 'H' else 0); "
        "end Main; end C;"), 1);
}

// §4.6: `s[i]` from a computed index.
BEZEL_TEST(codegen_cg27e, string_index_computed) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"abcde\"; var i := 3; return s[i]; "
        "end Main; end C;"), 100);  // 'd' = 100
}

// §4.6: indexing a single-char string.
BEZEL_TEST(codegen_cg27e, string_index_single_char) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Z\"; return s[0]; "
        "end Main; end C;"), 90);  // 'Z' = 90
}

}  // namespace
