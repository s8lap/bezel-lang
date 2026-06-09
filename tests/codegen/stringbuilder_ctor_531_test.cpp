// Fix #531: StringBuilder(initial: string) constructor ignores its argument.
//
// Root cause: the `new StringBuilder(...)` codegen branch in LlvmBackend_Exprs.cpp
// always emitted bz_array_alloc(SB_ESZ, 64) regardless of arguments, so
// `new StringBuilder("hello")` produced an empty builder (Length = 0).
//
// Fix: when one string arg is present (named or positional), allocate the empty
// builder then call bz_array_add to append the initial string part — reusing
// the same append path as Append(string). The no-arg and capacity-int forms are
// unchanged.
//
// Bezel named-arg call syntax: Identifier ":=" Value (e.g., initial := "hello").
// The parameter declaration uses ":" (e.g., constructor(initial: string)).
//
// Tests derive from stdlib §3.1 (StringBuilder constructors). Each test cites
// the spec and verifies a distinct behavioral property.
//
// StringBuilder is an owned type — programs bind it with `using var` (§28.3).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

// -------------------------------------------------------------------------
// §3.1: new StringBuilder(initial := "hello") — Length equals the code-point
// count of the initial string.  Before the fix this returned 0 (bug #531).
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_ctor_531, initial_named_arg_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(initial := \"hello\"); "
        "  return sb.Length; "
        "end Main; end C;"), 5);
}

// -------------------------------------------------------------------------
// §3.1: Positional form new StringBuilder("hello") — also ignored before fix.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_ctor_531, initial_positional_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(\"hello\"); "
        "  return sb.Length; "
        "end Main; end C;"), 5);
}

// -------------------------------------------------------------------------
// §3.1: Empty string initial → Length 0 (edge case: no parts stored).
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_ctor_531, initial_empty_string_length_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(initial := \"\"); "
        "  return sb.Length; "
        "end Main; end C;"), 0);
}

// -------------------------------------------------------------------------
// §3.1 / §4.6: Non-ASCII initial — Length counts code points (§4.6 defines
// string.Length as code-point count, not byte count). "café" = 4 code points.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_ctor_531, initial_non_ascii_length_codepoints) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(initial := \"caf\xC3\xA9\"); "
        "  return sb.Length; "
        "end Main; end C;"), 4);
}

// -------------------------------------------------------------------------
// §3.1: ToString after initial string is set — content must match.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_ctor_531, initial_string_tostring) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(initial := \"hello\"); "
        "  return (1 if sb.ToString() = \"hello\" else 0); "
        "end Main; end C;"), 1);
}

// -------------------------------------------------------------------------
// §3.1: Append after initial string — Length and ToString are both correct.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_ctor_531, initial_then_append_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(initial := \"hello\"); "
        "  sb.Append(\" world\"); "
        "  return sb.Length; "
        "end Main; end C;"), 11);
}

BEZEL_TEST(codegen_sb_ctor_531, initial_then_append_tostring) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(initial := \"hello\"); "
        "  sb.Append(\" world\"); "
        "  return (1 if sb.ToString() = \"hello world\" else 0); "
        "end Main; end C;"), 1);
}

// -------------------------------------------------------------------------
// §3.1 CONTROL: new StringBuilder() (no-arg) + Append still works correctly.
// Ensures the fix for the initial-string path does not regress the no-arg ctor.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_ctor_531, no_arg_ctor_append_control) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(); "
        "  sb.Append(\"hello\"); "
        "  return sb.Length; "
        "end Main; end C;"), 5);
}

// -------------------------------------------------------------------------
// §3.1: new StringBuilder(capacity := 128) — produces an empty builder.
// Capacity is a pre-allocation hint; it does not populate content.
// -------------------------------------------------------------------------

BEZEL_TEST(codegen_sb_ctor_531, capacity_ctor_length_zero) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(capacity := 128); "
        "  return sb.Length; "
        "end Main; end C;"), 0);
}

BEZEL_TEST(codegen_sb_ctor_531, capacity_ctor_then_append) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  using var sb := new StringBuilder(capacity := 128); "
        "  sb.Append(\"hi\"); "
        "  return sb.Length; "
        "end Main; end C;"), 2);
}

}  // namespace
