// CG27g: `for var c in s` over a string (§4.6, §29.2).
// Strings implement IEnumerable<char>, so for-in iterates each char
// (code point, as `char` i32). Current implementation iterates bytes,
// which is correct for ASCII strings.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// §4.6: for-in over "ABC" iterates chars 'A'=65, 'B'=66, 'C'=67.
// Sum = 198 (fits in exit code as 198 mod 256 = 198).
BEZEL_TEST(codegen_cg27g, string_for_in_sum) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"ABC\"; var sum := 0; "
        "  for var c in s do sum := sum + c; end; "
        "  return sum; "
        "end Main; end C;"), 65 + 66 + 67);
}

// §4.6: count iterations equals string Length.
BEZEL_TEST(codegen_cg27g, string_for_in_count) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; var cnt := 0; "
        "  for var c in s do cnt := cnt + 1; end; "
        "  return cnt; "
        "end Main; end C;"), 5);
}

// §4.6: empty string iterates zero times.
BEZEL_TEST(codegen_cg27g, empty_string_no_iterations) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var cnt := 0; "
        "  for var c in \"\" do cnt := cnt + 1; end; "
        "  return cnt; "
        "end Main; end C;"), 0);
}

// §4.6: char comparison with char literal inside the loop.
BEZEL_TEST(codegen_cg27g, string_for_in_char_comparison) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; var found := 0; "
        "  for var c in s do if c = 'H' then found := 1; end; end; "
        "  return found; "
        "end Main; end C;"), 1);
}

// §4.6: count occurrences of a specific char.
BEZEL_TEST(codegen_cg27g, string_for_in_count_char) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"banana\"; var cnt := 0; "
        "  for var c in s do if c = 'a' then cnt := cnt + 1; end; end; "
        "  return cnt; "
        "end Main; end C;"), 3);  // 'a' appears 3 times in "banana"
}

// §4.6: break inside for-in-string exits early.
BEZEL_TEST(codegen_cg27g, string_for_in_break) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; var cnt := 0; "
        "  for var c in s do "
        "    if c = 'l' then break; end; "
        "    cnt := cnt + 1; "
        "  end; "
        "  return cnt; "
        "end Main; end C;"), 2);  // 'H', 'e' before first 'l'
}

}  // namespace
