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

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runJitChecked;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

bool hasSemaError(const std::string& program) {
    auto r = runAot(program);
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
        "return (if ok then 1 else 0); ")), 1);
}

// stdlib §9.4: TryParse("true") sets result to true (value check).
BEZEL_TEST(codegen_654_bool_tryparse, tryparse_true_value) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool; "
        "var ok := bool.TryParse(\"true\", out result); "
        "if ok then return (if result then 1 else 0); end; "
        "return -1; ")), 1);
}

// stdlib §9.4: TryParse("false") → ok=true, result=false.
BEZEL_TEST(codegen_654_bool_tryparse, tryparse_false_lowercase_ok) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool; "
        "var ok := bool.TryParse(\"false\", out result); "
        "return (if ok then 1 else 0); ")), 1);
}

// stdlib §9.4: TryParse("false") sets result to false.
BEZEL_TEST(codegen_654_bool_tryparse, tryparse_false_value) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool; "
        "var ok := bool.TryParse(\"false\", out result); "
        "if ok then return (if result then 0 else 1); end; "
        "return -1; ")), 1);
}

// stdlib §9.4: TryParse("True") case-insensitive → ok=true.
BEZEL_TEST(codegen_654_bool_tryparse, tryparse_true_uppercase) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool; "
        "var ok := bool.TryParse(\"True\", out result); "
        "return (if ok then 1 else 0); ")), 1);
}

// stdlib §9.4: TryParse("FALSE") case-insensitive → ok=true, result=false.
BEZEL_TEST(codegen_654_bool_tryparse, tryparse_false_uppercase) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool; "
        "var ok := bool.TryParse(\"FALSE\", out result); "
        "if ok then return (if result then 0 else 1); end; "
        "return -1; ")), 1);
}

// stdlib §9.4: TryParse("yes") → ok=false (garbage input).
BEZEL_TEST(codegen_654_bool_tryparse, tryparse_garbage_returns_false) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool; "
        "var ok := bool.TryParse(\"yes\", out result); "
        "return (if ok then 0 else 1); ")), 1);
}

// stdlib §9.4: TryParse("1") → ok=false (only "true"/"false", not "1").
// Per §9.4: only the two canonical spellings. "1" is rejected by
// Convert.ToBoolean too — it raises FormatException (stdlib §2.3, v0.315),
// which TryParse reports as ok=false rather than throwing.
BEZEL_TEST(codegen_654_bool_tryparse, tryparse_one_is_not_accepted) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool; "
        "var ok := bool.TryParse(\"1\", out result); "
        "return (if ok then 0 else 1); ")), 1);
}

// stdlib §9.4: Boolean.TryParse (boxed-type name spelling) also works.
BEZEL_TEST(codegen_654_bool_tryparse, boolean_tryparse_name_spelling) {
    BEZEL_EXPECT_EQ(run(prog(
        "var result: bool; "
        "var ok := Boolean.TryParse(\"true\", out result); "
        "return (if ok then 1 else 0); ")), 1);
}

// ===========================================================================
// bool.Parse(s): bool  (stdlib §9.4) — issue #1035
//
// `bool.Parse` is a real stdlib §9.4 static method that sema accepts (typed
// bool), but it had NO codegen lowering: the call fell through to the generic
// primitive `Parse` handler's `return arg`, handing the string pointer back as
// an i1 and emitting malformed IR (`br ptr` — verifier failure / would crash).
// Per the §9.x strict-fault Parse contract ("TryParse returns false in every
// case where Parse would raise"), valid "true"/"false" (case-insensitive) yield
// the bool; anything else raises FormatException.
// ===========================================================================

// stdlib §9.4: bool.Parse("true") → true.
BEZEL_TEST(codegen_1035_bool_parse, parse_true) {
    BEZEL_EXPECT_EQ(run(prog(
        "var b := bool.Parse(\"true\"); return (if b then 7 else 9); ")), 7);
}

// stdlib §9.4: bool.Parse("false") → false.
BEZEL_TEST(codegen_1035_bool_parse, parse_false) {
    BEZEL_EXPECT_EQ(run(prog(
        "var b := bool.Parse(\"false\"); return (if b then 1 else 4); ")), 4);
}

// stdlib §9.4: recognition is case-insensitive ("TRUE" → true).
BEZEL_TEST(codegen_1035_bool_parse, parse_case_insensitive) {
    BEZEL_EXPECT_EQ(run(prog(
        "var b := bool.Parse(\"TRUE\"); return (if b then 42 else 0); ")), 42);
}

// stdlib §9.4 / §9.x strict-fault: an unrecognised word raises FormatException.
BEZEL_TEST(codegen_1035_bool_parse, parse_invalid_raises_format_exception) {
    BEZEL_EXPECT_EQ(run(prog(
        "try "
        "  var b := bool.Parse(\"yes\"); "
        "  return 0; "
        "catch FormatException => return 1; "
        "end; "
        "return 0; ")), 1);
}

// stdlib §9.4: Boolean.Parse (boxed-type name spelling) also lowers.
BEZEL_TEST(codegen_1035_bool_parse, boolean_parse_name_spelling) {
    BEZEL_EXPECT_EQ(run(prog(
        "var b := Boolean.Parse(\"false\"); return (if b then 1 else 4); ")), 4);
}

// ===========================================================================
// Win 2 — BitConverter  (stdlib §2.12)
// ===========================================================================
// All four methods are pure bit-reinterpretation (no arithmetic).

// stdlib §2.12: FloatToInt32Bits(0.0) == 0.
BEZEL_TEST(codegen_654_bitconverter, float_to_int32bits_zero) {
    BEZEL_EXPECT_EQ(run(prog(
        "var bits := BitConverter.FloatToInt32Bits(0.0); "
        "return (if bits = 0 then 1 else 0); ")), 1);
}

// stdlib §2.12: FloatToInt32Bits(1.0) == 0x3F800000 (1065353216).
BEZEL_TEST(codegen_654_bitconverter, float_to_int32bits_one) {
    BEZEL_EXPECT_EQ(run(prog(
        "var bits := BitConverter.FloatToInt32Bits(1.0); "
        "return (if bits = 1065353216 then 1 else 0); ")), 1);
}

// stdlib §2.12: Int32BitsToFloat(0) == 0.0.
BEZEL_TEST(codegen_654_bitconverter, int32bits_to_float_zero) {
    BEZEL_EXPECT_EQ(run(prog(
        "var f := BitConverter.Int32BitsToFloat(0); "
        "return (if f = 0.0 then 1 else 0); ")), 1);
}

// stdlib §2.12: Int32BitsToFloat(1065353216) == 1.0.
BEZEL_TEST(codegen_654_bitconverter, int32bits_to_float_one) {
    BEZEL_EXPECT_EQ(run(prog(
        "var f := BitConverter.Int32BitsToFloat(1065353216); "
        "return (if f = 1.0 then 1 else 0); ")), 1);
}

// stdlib §2.12: FloatToInt32Bits round-trips: FloatToInt32Bits(Int32BitsToFloat(n)) = n.
// Use known int-bit pattern 0x3F800000 (=1065353216) which represents 1.0f.
BEZEL_TEST(codegen_654_bitconverter, float_roundtrip) {
    BEZEL_EXPECT_EQ(run(prog(
        "var f := BitConverter.Int32BitsToFloat(1065353216); "
        "var bits := BitConverter.FloatToInt32Bits(f); "
        "return (if bits = 1065353216 then 1 else 0); ")), 1);
}

// stdlib §2.12: DoubleToInt64Bits(0.0) == 0.
BEZEL_TEST(codegen_654_bitconverter, double_to_int64bits_zero) {
    BEZEL_EXPECT_EQ(run(prog(
        "var bits := BitConverter.DoubleToInt64Bits(0.0); "
        "return (if bits = 0 then 1 else 0); ")), 1);
}

// stdlib §2.12: DoubleToInt64Bits(1.0) == 0x3FF0000000000000 (4607182418800017408L).
BEZEL_TEST(codegen_654_bitconverter, double_to_int64bits_one) {
    BEZEL_EXPECT_EQ(run(prog(
        "var bits := BitConverter.DoubleToInt64Bits(1.0); "
        "return (if bits = 4607182418800017408L then 1 else 0); ")), 1);
}

// stdlib §2.12: Int64BitsToDouble(0) == 0.0.
BEZEL_TEST(codegen_654_bitconverter, int64bits_to_double_zero) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := BitConverter.Int64BitsToDouble(0); "
        "return (if d = 0.0 then 1 else 0); ")), 1);
}

// stdlib §2.12: Double round-trips: DoubleToInt64Bits(Int64BitsToDouble(n)) = n.
// Use known bit pattern 0x4000000000000000L (= 2.0 as double).
BEZEL_TEST(codegen_654_bitconverter, double_roundtrip) {
    BEZEL_EXPECT_EQ(run(prog(
        "var d := BitConverter.Int64BitsToDouble(4611686018427387904L); "
        "var bits := BitConverter.DoubleToInt64Bits(d); "
        "return (if bits = 4611686018427387904L then 1 else 0); ")), 1);
}

// #1038 / §3.4: BitConverter (§2.12) is a pure-static utility class with
// exactly five methods; an unknown static was previously left untyped and
// silently miscompiled to a no-op (the builtin-class path has no fallback
// reject). It must now be a compile error, like Math/Convert (#1037).
BEZEL_TEST(codegen_654_bitconverter, unknown_static_rejected_1038) {
    BEZEL_EXPECT_TRUE(hasSemaError(prog(
        "var x := BitConverter.Bogus(1); return 0; ")));
}

// Positive control for the #1038 reject — a real method must STILL type/lower.
BEZEL_TEST(codegen_654_bitconverter, reinterpret_still_works_1038) {
    BEZEL_EXPECT_EQ(run(prog(
        "var x: int := BitConverter.Reinterpret<int>(1.0f); "
        "return (if x = 1065353216 then 1 else 0); ")), 1);
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
        "return (if c = 'a' then 1 else 0); ")), 1);
}

// stdlib §9.6: ToCharArray()[1] is the second code point.
BEZEL_TEST(codegen_654_tochararray, second_char_correct) {
    BEZEL_EXPECT_EQ(run(prog(
        "var s := \"abc\"; "
        "using var a := s.ToCharArray(); "
        "var c := a.Get(1); "
        "return (if c = 'b' then 1 else 0); ")), 1);
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

// fix #747 R43 / §9.6: string.From(buffer: Array<char>, start, count): string —
// the inverse of ToCharArray. The 3-arg overload had no codegen handler, so the
// call produced a garbage string pointer that SIGSEGV'd on first use (equality,
// indexing, Length-traversal). Now builds the string from the array's code
// points at [start, start+count). All exceptions are caught.

// Empty array + (0, 0) → empty string (the minimal repro that SIGSEGV'd).
BEZEL_TEST(codegen_string_from_747, empty_array_zero_count_equals_empty) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var chars := new Array<char>(); "
        "  var s := string.From(chars, 0, 0); "
        "  return (if s = \"\" then 0 else 1); "  // 0
        "end Main; end C;"), 0);
}

// Content is populated correctly: From(['H','i','!'], 0, 2) == "Hi", Length 2.
BEZEL_TEST(codegen_string_from_747, builds_substring_content) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var chars := new Array<char>(); "
        "  chars.Add('H'); chars.Add('i'); chars.Add('!'); "
        "  var s := string.From(chars, 0, 2); "
        "  return (if s = \"Hi\" then s.Length else 99); "  // 2
        "end Main; end C;"), 2);
}

// Round-trips with ToCharArray: "World".ToCharArray() then From(arr, 1, 3) == "orl".
BEZEL_TEST(codegen_string_from_747, roundtrips_with_to_char_array) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var arr := \"World\".ToCharArray(); "
        "  var s := string.From(arr, 1, 3); "
        "  return (if s = \"orl\" then 0 else s.Length); "  // 0
        "end Main; end C;"), 0);
}

// Non-ASCII code points round-trip (adaptive width selected): "café".
BEZEL_TEST(codegen_string_from_747, non_ascii_roundtrip_width) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var arr := \"caf\xC3\xA9\".ToCharArray(); "
        "  var s := string.From(arr, 0, 4); "
        "  return (if s = \"caf\xC3\xA9\" then s.Length else 99); "  // 4
        "end Main; end C;"), 4);
}

// Out-of-range count raises IndexOutOfRangeException (§4.6 bounds convention).
BEZEL_TEST(codegen_string_from_747, out_of_range_count_throws) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var chars := new Array<char>(); "
        "  chars.Add('A'); chars.Add('B'); "
        "  try var s := string.From(chars, 0, 10); return 1; "
        "  catch IndexOutOfRangeException => return 0; end; "  // 0
        "end Main; end C;"), 0);
}

// ── #1145 / §9.6: a 1-arg `String.From(Array<char>)` has NO matching overload ─
// The single-argument forms take `char` / `CString` / `WString`; the char-array
// form is the THREE-arg `From(buffer, start, count)`. Bezel omits implicit
// conversions (rationale §5.1), so a 1-arg array call must be a clean no-overload
// compile error. Before the fix it mis-resolved to `From(value: char)` and
// codegen sign-extended the array's heap pointer to i32 → malformed IR
// (`sext ptr to i32`), aborting compilation instead of diagnosing.
BEZEL_TEST(codegen_string_from_747, one_arg_char_array_no_overload_rejected_1145) {
    BEZEL_EXPECT_TRUE(hasSemaError(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<char>(); "
        "  arr.Add('h'); arr.Add('i'); "
        "  var t := String.From(arr); "                 // §9.6 — no 1-arg array overload
        "  return t.Length; "
        "end Main; end C;"));
}

// #1145 CONTROL: the valid 1-arg `String.From(char)` form is unaffected.
BEZEL_TEST(codegen_string_from_747, one_arg_char_still_builds_1145) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var t := String.From('A'); "
        "  return t.Length; "                           // 1
        "end Main; end C;"), 1);
}

// #1145 CONTROL: the valid 3-arg array form is unaffected (still builds "hi").
BEZEL_TEST(codegen_string_from_747, three_arg_array_still_builds_1145) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Collections.Generic; "
        "class C public static method Main(): int "
        "  using var arr := new Array<char>(); "
        "  arr.Add('h'); arr.Add('i'); "
        "  var t := String.From(arr, 0, arr.Count); "
        "  return t.Length; "                           // 2
        "end Main; end C;"), 2);
}
