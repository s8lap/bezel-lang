// Codegen tests for stdlib issue #654, three wins:
//   Win 1 — bool.TryParse / Boolean.TryParse  (stdlib §9.4)
//   Win 2 — BitConverter (stdlib §2.12)
//   Win 3 — string.ToCharArray(): owned Array<char>  (stdlib §9.6)
//
// All tests use the JIT harness (runJit/runJitChecked) and return int exit
// codes.  The ASan/LSan verdict for ToCharArray is exercised by the test
// named codegen_654_tochararray_asan_no_leak.
//
// Spec ref:
//   stdlib §9.4 (Boolean) — TryParse(s: string, out result: bool): bool;
//                            case-insensitive "true"/"false"; all else → false.
//   stdlib §2.12 (BitConverter) — FloatToInt32Bits / Int32BitsToFloat /
//                                  DoubleToInt64Bits / Int64BitsToDouble.
//   stdlib §9.6 (String)      — ToCharArray(): owned Array<char>.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;
using bezel::test::runJitChecked;

namespace {

int run(const std::string& program) {
    auto r = runJit(program);
    return r.ran ? r.exitCode : -999;
}

bool hasSemaError(const std::string& program) {
    auto r = runJit(program);
    return !r.ran && r.diagnostics.hasErrors();
}

std::string prog(const std::string& mainBody) {
    return "module M; "
           "class C public static method Main(): int "
           + mainBody +
           " end Main; end C;";
}

} // namespace

// ===========================================================================
// Win 1 — bool.TryParse / Boolean.TryParse  (stdlib §9.4)
// ===========================================================================

// stdlib §9.4: TryParse("true") → ok=true, result=true.
BEZEL_TEST(codegen_654_bool_tryparse, tryparse_true_lowercase_ok) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool; "
        "var ok := bool.TryParse(\"true\", out result); "
        "return (1 if ok else 0); ")), 1);
}

// stdlib §9.4: TryParse("true") sets result to true (value check).
BEZEL_TEST(codegen_654_bool_tryparse, tryparse_true_value) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool; "
        "var ok := bool.TryParse(\"true\", out result); "
        "if ok then return (1 if result else 0); end; "
        "return -1; ")), 1);
}

// stdlib §9.4: TryParse("false") → ok=true, result=false.
BEZEL_TEST(codegen_654_bool_tryparse, tryparse_false_lowercase_ok) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool; "
        "var ok := bool.TryParse(\"false\", out result); "
        "return (1 if ok else 0); ")), 1);
}

// stdlib §9.4: TryParse("false") sets result to false.
BEZEL_TEST(codegen_654_bool_tryparse, tryparse_false_value) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool; "
        "var ok := bool.TryParse(\"false\", out result); "
        "if ok then return (0 if result else 1); end; "
        "return -1; ")), 1);
}

// stdlib §9.4: TryParse("True") case-insensitive → ok=true.
BEZEL_TEST(codegen_654_bool_tryparse, tryparse_true_uppercase) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool; "
        "var ok := bool.TryParse(\"True\", out result); "
        "return (1 if ok else 0); ")), 1);
}

// stdlib §9.4: TryParse("FALSE") case-insensitive → ok=true, result=false.
BEZEL_TEST(codegen_654_bool_tryparse, tryparse_false_uppercase) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool; "
        "var ok := bool.TryParse(\"FALSE\", out result); "
        "if ok then return (0 if result else 1); end; "
        "return -1; ")), 1);
}

// stdlib §9.4: TryParse("yes") → ok=false (garbage input).
BEZEL_TEST(codegen_654_bool_tryparse, tryparse_garbage_returns_false) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool; "
        "var ok := bool.TryParse(\"yes\", out result); "
        "return (0 if ok else 1); ")), 1);
}

// stdlib §9.4: TryParse("1") → ok=false (only "true"/"false", not "1").
// Per §9.4: only the two canonical spellings; "1" is accepted by Convert.ToBoolean
// but NOT by Boolean.TryParse.
BEZEL_TEST(codegen_654_bool_tryparse, tryparse_one_is_not_accepted) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool; "
        "var ok := bool.TryParse(\"1\", out result); "
        "return (0 if ok else 1); ")), 1);
}

// stdlib §9.4: Boolean.TryParse (boxed-type name spelling) also works.
BEZEL_TEST(codegen_654_bool_tryparse, boolean_tryparse_name_spelling) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool; "
        "var ok := Boolean.TryParse(\"true\", out result); "
        "return (1 if ok else 0); ")), 1);
}

// ===========================================================================
// Win 2 — BitConverter  (stdlib §2.12)
// ===========================================================================
// All four methods are pure bit-reinterpretation (no arithmetic).

// stdlib §2.12: FloatToInt32Bits(0.0) == 0.
BEZEL_TEST(codegen_654_bitconverter, float_to_int32bits_zero) {
    BEZEL_EXPECT_EQ(run(prog(
        "var bits := BitConverter.FloatToInt32Bits(0.0); "
        "return (1 if bits = 0 else 0); ")), 1);
}

// stdlib §2.12: FloatToInt32Bits(1.0) == 0x3F800000 (1065353216).
BEZEL_TEST(codegen_654_bitconverter, float_to_int32bits_one) {
    BEZEL_EXPECT_EQ(run(prog(
        "var bits := BitConverter.FloatToInt32Bits(1.0); "
        "return (1 if bits = 1065353216 else 0); ")), 1);
}

// stdlib §2.12: Int32BitsToFloat(0) == 0.0.
BEZEL_TEST(codegen_654_bitconverter, int32bits_to_float_zero) {
    BEZEL_EXPECT_EQ(run(prog(
        "var f := BitConverter.Int32BitsToFloat(0); "
        "return (1 if f = 0.0 else 0); ")), 1);
}

// stdlib §2.12: Int32BitsToFloat(1065353216) == 1.0.
BEZEL_TEST(codegen_654_bitconverter, int32bits_to_float_one) {
    BEZEL_EXPECT_EQ(run(prog(
        "var f := BitConverter.Int32BitsToFloat(1065353216); "
        "return (1 if f = 1.0 else 0); ")), 1);
}

// stdlib §2.12: FloatToInt32Bits round-trips: FloatToInt32Bits(Int32BitsToFloat(n)) = n.
// Use known int-bit pattern 0x3F800000 (=1065353216) which represents 1.0f.
BEZEL_TEST(codegen_654_bitconverter, float_roundtrip) {
    BEZEL_EXPECT_EQ(run(prog(
        "var f := BitConverter.Int32BitsToFloat(1065353216); "
        "var bits := BitConverter.FloatToInt32Bits(f); "
        "return (1 if bits = 1065353216 else 0); ")), 1);
}

// stdlib §2.12: DoubleToInt64Bits(0.0) == 0.
BEZEL_TEST(codegen_654_bitconverter, double_to_int64bits_zero) {
    BEZEL_EXPECT_EQ(run(prog(
        "var bits := BitConverter.DoubleToInt64Bits(0.0); "
        "return (1 if bits = 0 else 0); ")), 1);
}

// stdlib §2.12: DoubleToInt64Bits(1.0) == 0x3FF0000000000000 (4607182418800017408L).
BEZEL_TEST(codegen_654_bitconverter, double_to_int64bits_one) {
    BEZEL_EXPECT_EQ(run(prog(
        "var bits := BitConverter.DoubleToInt64Bits(1.0); "
        "return (1 if bits = 4607182418800017408L else 0); ")), 1);
}

// stdlib §2.12: Int64BitsToDouble(0) == 0.0.
BEZEL_TEST(codegen_654_bitconverter, int64bits_to_double_zero) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := BitConverter.Int64BitsToDouble(0); "
        "return (1 if d = 0.0 else 0); ")), 1);
}

// stdlib §2.12: Double round-trips: DoubleToInt64Bits(Int64BitsToDouble(n)) = n.
// Use known bit pattern 0x4000000000000000L (= 2.0 as double).
BEZEL_TEST(codegen_654_bitconverter, double_roundtrip) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := BitConverter.Int64BitsToDouble(4611686018427387904L); "
        "var bits := BitConverter.DoubleToInt64Bits(d); "
        "return (1 if bits = 4611686018427387904L else 0); ")), 1);
}

// ===========================================================================
// Win 3 — string.ToCharArray(): owned Array<char>  (stdlib §9.6)
// ===========================================================================

// stdlib §9.6: ToCharArray() on "abc" returns an array of length 3.
BEZEL_TEST(codegen_654_tochararray, length_is_correct) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := \"abc\"; "
        "using var a := s.ToCharArray(); "
        "return a.Count; ")), 3);
}

// stdlib §9.6: ToCharArray()[0] is the first code point.
BEZEL_TEST(codegen_654_tochararray, first_char_correct) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := \"abc\"; "
        "using var a := s.ToCharArray(); "
        "var c := a.Get(0); "
        "return (1 if c = 'a' else 0); ")), 1);
}

// stdlib §9.6: ToCharArray()[1] is the second code point.
BEZEL_TEST(codegen_654_tochararray, second_char_correct) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := \"abc\"; "
        "using var a := s.ToCharArray(); "
        "var c := a.Get(1); "
        "return (1 if c = 'b' else 0); ")), 1);
}

// stdlib §9.6: ToCharArray() on empty string returns empty array (Count = 0).
BEZEL_TEST(codegen_654_tochararray, empty_string_empty_array) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := \"\"; "
        "using var a := s.ToCharArray(); "
        "return a.Count; ")), 0);
}

// stdlib §9.6: round-trip — construct a string from its own char array (checks
// all characters, not just the first).
BEZEL_TEST(codegen_654_tochararray, all_chars_correct) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := \"hello\"; "
        "using var a := s.ToCharArray(); "
        "if a.Count <> 5 then return -1; end; "
        "if a.Get(0) <> 'h' then return -2; end; "
        "if a.Get(1) <> 'e' then return -3; end; "
        "if a.Get(2) <> 'l' then return -4; end; "
        "if a.Get(3) <> 'l' then return -5; end; "
        "if a.Get(4) <> 'o' then return -6; end; "
        "return 1; ")), 1);
}

// stdlib §9.6: `using var` form disposes the array at scope end — LeakSanitizer
// must report no leaks.  This is the designated ASan/LSan verdict test.
// (Run with ASAN_OPTIONS=detect_leaks=1 to exercise LSan.)
BEZEL_TEST(codegen_654_tochararray_asan_no_leak, using_var_disposes_array) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := \"leak-test-string\"; "
        "using var a := s.ToCharArray(); "
        "var n := a.Count; "
        "return n; ")), 16);
}
