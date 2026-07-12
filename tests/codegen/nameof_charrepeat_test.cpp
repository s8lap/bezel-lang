// Codegen tests for nameof (§17.9) and string.Repeat (stdlib §9.6).
//
// BUG A — nameof(…) was untyped in sema and unhandled in codegen, causing a
//          crash / garbage result. §17.9: nameof yields the rightmost identifier
//          of its operand as a compile-time string constant.
//
// BUG B — char.Repeat is NOT a member of the Char type (stdlib §9.5 lists no
//          Repeat method). The correct API is string.Repeat(c: char, count: int)
//          defined at stdlib §9.6. Tests here use the authoritative stdlib API.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& src) {
    auto r = runAot(src);
    return r.ran ? r.exitCode : -999;
}

} // namespace

// ── §17.9 nameof ─────────────────────────────────────────────────────────────

// §17.9 example: nameof(BankAccount) → "BankAccount" — a simple type name.
BEZEL_TEST(codegen_nameof_spec17_9, nameof_type_name) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := nameof(C); "
        "  return (1 if n = \"C\" else 0); "
        "end Main; end C;"), 1);
}

// §17.9 example: nameof(amount) — a local variable name.
BEZEL_TEST(codegen_nameof_spec17_9, nameof_local_var) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var amount := 42; "
        "  var n := nameof(amount); "
        "  return (1 if n = \"amount\" else 0); "
        "end Main; end C;"), 1);
}

// §17.9: result is a string (test via Length property).
BEZEL_TEST(codegen_nameof_spec17_9, nameof_result_is_string_with_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := nameof(C); "
        "  return n.Length; "
        "end Main; end C;"), 1);
}

// §17.9 example: nameof(BankAccount.Deposit) → "Deposit" — rightmost segment.
BEZEL_TEST(codegen_nameof_spec17_9, nameof_qualified_name_rightmost) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := nameof(C.Main); "
        "  return (1 if n = \"Main\" else 0); "
        "end Main; end C;"), 1);
}

// §17.9 example: nameof(System.Console) → "Console" — rightmost of 2-segment
// qualified name.
BEZEL_TEST(codegen_nameof_spec17_9, nameof_two_segment_qualified_rightmost) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := nameof(M.C); "
        "  return (1 if n = \"C\" else 0); "
        "end Main; end C;"), 1);
}

// §17.9: nameof result can be used in a string concatenation expression.
BEZEL_TEST(codegen_nameof_spec17_9, nameof_used_in_concat) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var n := nameof(C); "
        "  var full := \"class:\" + n; "
        "  return (1 if full = \"class:C\" else 0); "
        "end Main; end C;"), 1);
}

// §17.9: nameof of an underscore-prefixed identifier preserves the underscore.
BEZEL_TEST(codegen_nameof_spec17_9, nameof_underscore_prefix) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var _balance := 0; "
        "  var n := nameof(_balance); "
        "  return (1 if n = \"_balance\" else 0); "
        "end Main; end C;"), 1);
}

// ── stdlib §9.6 string.Repeat ────────────────────────────────────────────────
// NOTE: char.Repeat is NOT in the Char stdlib type (§9.5 lists no Repeat).
// The authoritative API is string.Repeat(c: char, count: int): string (§9.6).
// Tests below use that correct API.

// §9.6: string.Repeat('x', 3) produces "xxx".
BEZEL_TEST(codegen_string_repeat_spec9_6, repeat_x_three_times) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var rep: string := string.Repeat('x', 3); "
        "  return (1 if rep = \"xxx\" else 0); "
        "end Main; end C;"), 1);
}

// §9.6: string.Repeat('a', 0) produces the empty string.
BEZEL_TEST(codegen_string_repeat_spec9_6, repeat_zero_times_is_empty) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var rep: string := string.Repeat('a', 0); "
        "  return (1 if rep = \"\" else 0); "
        "end Main; end C;"), 1);
}

// §9.6: Length of string.Repeat('x', 3) is 3.
BEZEL_TEST(codegen_string_repeat_spec9_6, repeat_result_length) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var rep: string := string.Repeat('z', 5); "
        "  return rep.Length; "
        "end Main; end C;"), 5);
}

// §9.6: string.Repeat result can be used in string concatenation.
BEZEL_TEST(codegen_string_repeat_spec9_6, repeat_used_in_concat) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var rep: string := string.Repeat('a', 2); "
        "  var s := \"x\" + rep + \"x\"; "
        "  return (1 if s = \"xaax\" else 0); "
        "end Main; end C;"), 1);
}
