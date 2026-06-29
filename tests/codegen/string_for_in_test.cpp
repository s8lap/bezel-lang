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

// ===== fix #1220 / §5 / §9.6: LINQ operators on string =====
// string is IEnumerable<char>; Where/Select/Count/Skip/Take and other LINQ
// operators must resolve via the IEnumerable<char> extension path rather than
// the "not yet supported in codegen" catch-all.

// §9.6 / §5: s.Where((c) => ...) filters chars and iterates the result.
// "aAbBcC".Where(c='A') = one match → expect 1.
BEZEL_TEST(codegen_string_linq_1220, string_where_char_count) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"aAbBcC\"; var n := 0; "
        "  for var c in s.Where((c) => c = 'A') do n := n + 1; end; "
        "  return n; "
        "end Main; end C;"), 1);
}

// §9.6 / §5: s.Where(...) with multiple matches.
// "Hello".Where(c='l') = two 'l' chars → expect 2.
BEZEL_TEST(codegen_string_linq_1220, string_where_multiple) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; var n := 0; "
        "  for var c in s.Where((c) => c = 'l') do n := n + 1; end; "
        "  return n; "
        "end Main; end C;"), 2);
}

// §9.6 / §5: s.Count() — no-arg Count() returns string Length.
// "Hello" has 5 chars.
BEZEL_TEST(codegen_string_linq_1220, string_count_noarg) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; "
        "  return s.Count(); "
        "end Main; end C;"), 5);
}

// §9.6 / §5: s.Any((c)=>...) — quantifier over chars.
// "Hello".Any(c='x') = false → 0.
BEZEL_TEST(codegen_string_linq_1220, string_any_pred) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello\"; "
        "  return int(s.Any((c) => c = 'x')); "
        "end Main; end C;"), 0);
}

// §9.6 / §5: s.ToArray() materializes string to Array<char>.
// "ABC".ToArray().Length = 3.
BEZEL_TEST(codegen_string_linq_1220, string_toarray_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"ABC\"; "
        "  using var a := s.ToArray(); "
        "  return a.Length; "
        "end Main; end C;"), 3);
}

// Anti-regression: intrinsic string member Length still resolves as before.
BEZEL_TEST(codegen_string_linq_1220, string_length_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hello World\"; "
        "  return s.Length; "
        "end Main; end C;"), 11);
}

// Anti-regression: for-in on a string still works (direct string iter, not LINQ).
BEZEL_TEST(codegen_string_linq_1220, string_for_in_anti_regression) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var s := \"Hi\"; var cnt := 0; "
        "  for var c in s do cnt := cnt + 1; end; "
        "  return cnt; "
        "end Main; end C;"), 2);
}

}  // namespace
