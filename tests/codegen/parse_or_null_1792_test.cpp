// Spec-derived regression tests for fix #1792 — `char.ParseOrNull`,
// `bool.ParseOrNull`, and `char.TryParse` direct static calls.
//
// WHAT THE DOCUMENTS REQUIRE.  stdlib §9.5 declares on Char:
//     public static pure method Parse(s: string): char;            // exactly one code point
//     public static method TryParse(s: string, out result: char): bool;
//     public static pure method ParseOrNull(s: string): char?;     // v0.231
//
// stdlib §9.4 declares on Boolean:
//     public static pure method Parse(s: string): bool;            // "true"/"false", case-insensitive
//     public static method TryParse(s: string, out result: bool): bool;
//     public static pure method ParseOrNull(s: string): bool?;     // v0.231
//
// `char.TryParse` is mentioned as the IParsable<char> witness at §2.19
// ("char → ISteppable/IParsable only", v0.222); the v0.231 ruling (#1792)
// added the `*OrNull` convenience form alongside `Parse`/`TryParse` per the
// §1 `*OrNull` tier ("null exactly where Parse(s) raises").
//
// THE DEFECT — a SEMANTIC reject in the closed-set static-member handler
// at `ExprTypecheck_Calls.cpp::resolveMemberAccess`.  The `bool` arm
// listed `TryParse` and `Parse` but not `ParseOrNull`; there was no `char`
// arm at all (every `char.TryParse(...)` and `char.ParseOrNull(...)` call
// fell through to the §3.4 "no static method" diagnostic).  For
// `char.TryParse` the codegen side was already lowering it
// (LlvmBackend_Calls2.cpp, ~L417); for `char.ParseOrNull` and
// `bool.ParseOrNull` codegen had no handler at all.
//
// THE FIX:
//   * sema: admit `bool.ParseOrNull` → `bool?`, `char.TryParse` → `bool`,
//     `char.ParseOrNull` → `char?` in `resolveMemberAccess`.
//   * codegen: lower `char.ParseOrNull(s)` (length-check, nullable struct
//     wrap, null on len ≠ 1) and `bool.ParseOrNull(s)` (strcasecmp against
//     "true"/"false", nullable struct wrap, null when neither matches) in
//     `tryLowerPrimitiveStaticCall`.
//
// Without the fix, the direct calls below are rejected at compile time:
// "no static method or property" / "no member" — the test program does
// not reach the codegen pass and exits -999 (no run).  With the fix, each
// test's packed return value verifies the lowered semantics.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

bool hasSemaError(const std::string& src) {
    auto r = runAot(src);
    return !r.ran && r.diagnostics.hasErrors();
}

}  // namespace

// ===========================================================================
// char.ParseOrNull  (stdlib §9.5, v0.231, fix #1792)
// ===========================================================================

// §9.5: a one-code-point string yields Some(char), the code point value.
// Packed as int(cp) = 122 ('z').
BEZEL_TEST(parse_or_null_1792, char_parse_or_null_one_code_point_returns_some) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var c := char.ParseOrNull(\"z\"); "
        "  if c = null then return -1; end; "
        "  return int(c!); "
        "end Main; end C;"), 122);
}

// §9.5: empty string yields null (Parse("") raises FormatException).
BEZEL_TEST(parse_or_null_1792, char_parse_or_null_empty_string_returns_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var c := char.ParseOrNull(\"\"); "
        "  if c = null then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §9.5: multi-code-point string yields null (s.Length must equal 1).
BEZEL_TEST(parse_or_null_1792, char_parse_or_null_multi_code_point_returns_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var c := char.ParseOrNull(\"ab\"); "
        "  if c = null then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §9.5: `??` composition — the standard *OrNull call site. `??` picks the
// fallback when ParseOrNull returned null. Packed return = fallback value.
BEZEL_TEST(parse_or_null_1792, char_parse_or_null_coalesce_with_fallback) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a := char.ParseOrNull(\"a\") ?? '?'; "
        "  var b := char.ParseOrNull(\"abc\") ?? '?'; "
        "  return int(a) + int(b); "      // 97 + 63 = 160
        "end Main; end C;"), 160);
}

// ===========================================================================
// char.TryParse  (stdlib §9.5, #1792 / §2.19 IParsable<char> witness)
// ===========================================================================

// §9.5 / §2.19: char.TryParse("z", out var c) → ok=true, c='z'.
// Without the fix, sema rejects char.TryParse as "no static method"; exit -999.
BEZEL_TEST(parse_or_null_1792, char_tryparse_one_code_point_ok) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var c: char; "
        "  var ok := char.TryParse(\"z\", out c); "
        "  if ok then return int(c); end; "
        "  return 0; "
        "end Main; end C;"), 122);
}

// §9.5: char.TryParse("") → ok=false, c='\0' (default(char), §17.11).
BEZEL_TEST(parse_or_null_1792, char_tryparse_empty_fails_with_null_char) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var c: char; "
        "  var ok := char.TryParse(\"\", out c); "
        "  if ok then return 99; end; "
        "  if c <> '\\0' then return 98; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// §9.5: char.TryParse("abc") → ok=false (length != 1).
BEZEL_TEST(parse_or_null_1792, char_tryparse_multi_code_point_fails) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var c: char; "
        "  var ok := char.TryParse(\"ab\", out c); "
        "  if ok then return 99; end; "
        "  return 1; "
        "end Main; end C;"), 1);
}

// ===========================================================================
// bool.ParseOrNull  (stdlib §9.4, v0.231, fix #1792)
// ===========================================================================

// §9.4: "true" → Some(true). Packed as 1.
BEZEL_TEST(parse_or_null_1792, bool_parse_or_null_true_returns_some_true) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b := bool.ParseOrNull(\"true\"); "
        "  if b = null then return -1; end; "
        "  return (if b! then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.4: "false" → Some(false). Packed as 1 (the discriminator that b is
// present and is false).
BEZEL_TEST(parse_or_null_1792, bool_parse_or_null_false_returns_some_false) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b := bool.ParseOrNull(\"false\"); "
        "  if b = null then return -1; end; "
        "  return (if b! then 0 else 1); "
        "end Main; end C;"), 1);
}

// §9.4: case-insensitive recognition — "TRUE" → Some(true).
BEZEL_TEST(parse_or_null_1792, bool_parse_or_null_true_uppercase_returns_some) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b := bool.ParseOrNull(\"TRUE\"); "
        "  if b = null then return -1; end; "
        "  return (if b! then 1 else 0); "
        "end Main; end C;"), 1);
}

// §9.4: case-insensitive recognition — "FALSE" → Some(false).
BEZEL_TEST(parse_or_null_1792, bool_parse_or_null_false_uppercase_returns_some) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b := bool.ParseOrNull(\"FALSE\"); "
        "  if b = null then return -1; end; "
        "  return (if b! then 0 else 1); "
        "end Main; end C;"), 1);
}

// §9.4: garbage input → null (Parse("yes") raises FormatException; per the
// §9.x strict-fault Parse contract, ParseOrNull returns null instead).
BEZEL_TEST(parse_or_null_1792, bool_parse_or_null_garbage_returns_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b := bool.ParseOrNull(\"yes\"); "
        "  if b = null then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §9.4: "1" is rejected (only "true"/"false" are accepted; Convert.ToBoolean
// differs from Boolean.TryParse / Boolean.ParseOrNull).  Returns null.
BEZEL_TEST(parse_or_null_1792, bool_parse_or_null_one_returns_null) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b := bool.ParseOrNull(\"1\"); "
        "  if b = null then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 1);
}

// §9.4: `??` composition — the standard *OrNull call site. Picks the fallback
// when ParseOrNull returns null. Packed return = fallback value.
BEZEL_TEST(parse_or_null_1792, bool_parse_or_null_coalesce_with_fallback) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var on := bool.ParseOrNull(\"true\") ?? false; "
        "  var off := bool.ParseOrNull(\"junk\") ?? false; "
        "  return (if on then 10 else 0) + (if off then 0 else 1); "
        "end Main; end C;"), 11);
}

// §9.4: Boolean (boxed-type name spelling) also lowers — same membership
// set as bool keyword alias. Without the fix, Boolean.ParseOrNull rejected.
BEZEL_TEST(parse_or_null_1792, boolean_parse_or_null_name_spelling) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var b := Boolean.ParseOrNull(\"False\"); "
        "  if b = null then return -1; end; "
        "  return (if b! then 0 else 1); "
        "end Main; end C;"), 1);
}

// ===========================================================================
// CONTROL — sema rejects calls the static-method surface does not declare.
// char.TryParse expects (s, out c); an arity-1 call has no sema admission
// and must be rejected (it falls through to the §3.4 "no static method"
// diagnostic, the same surface as before #1792).
// ===========================================================================
BEZEL_TEST(parse_or_null_1792, char_tryparse_wrong_arity_rejected) {
    BEZEL_EXPECT_TRUE(hasSemaError(
        "module M; class C public static method Main(): int "
        "  var b := char.TryParse(\"x\"); "
        "  return 0; "
        "end Main; end C;"));
}