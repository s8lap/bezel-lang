// Codegen — null-safe equality/inequality for nullable strings (`string?`).
//
// §4.6 / §9.1: a `string?` variable is a reference type — it lowers to a plain
// pointer (`ptr` to bz_string) where null is a real null pointer.  The `=` and
// `<>` operators on `string?` must therefore be null-safe: dereferencing either
// operand without first checking for null causes a SIGSEGV.
//
// Bug: when BOTH operands are `string?` variables (i.e. neither side is a null
// literal in the AST) the code path sets `useStrEq=true` and calls
// `emitStringEq(l, r)` directly.  `emitStringEq` had no null guard — it would
// call `loadStringLen(l)` / `loadStringLen(r)` without first checking that the
// pointers are non-null, crashing when one variable holds null at runtime.
//
// Fix (inside `emitStringEq`): if either pointer is null, the result is pure
// pointer equality — null==null → true, null vs. non-null → false — with NO
// dereference.  Only when both are non-null does the existing length+content
// comparison run.  This makes ALL callers of `emitStringEq` null-safe
// automatically (record string-field equality, string.Equals, etc.).
//
// Spec citations: §4.6 (string value semantics and equality), §9.1 (string
// equality is content equality), §7.2/§8.4 (null-safe equality: null = null →
// true; null = non-null → false).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

int run(const std::string& src) {
    auto r = runJit(src);
    return r.ran ? r.exitCode : -999;
}

}  // namespace

// ---- §7.2/§8.4/§4.6: null = null → true ----------------------------------------

// §8.4 / §7.2: null = null is true (both sides are null pointers — pointer
// equality holds).  This must not crash (was: crash when both vars hold null).
BEZEL_TEST(codegen_nullable_string_cmp, null_eq_null_spec_7_2_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: string? := null; var b: string? := null; "
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 1);
}

// §8.4: null <> null is false.
BEZEL_TEST(codegen_nullable_string_cmp, null_neq_null_spec_8_4) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: string? := null; var b: string? := null; "
        "  return (1 if a <> b else 0); "
        "end Main; end C;"), 0);
}

// ---- §8.4/§4.6: null = non-null → false ----------------------------------------

// §8.4: null = "hello" is false.  Left side is null — must not SIGSEGV.
// This is the exact crash case from the bug report.
BEZEL_TEST(codegen_nullable_string_cmp, null_eq_nonnull_spec_8_4_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: string? := null; var b: string? := \"hello\"; "
        "  if a = b then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §8.4: "hello" = null is false.  Right side is null.
BEZEL_TEST(codegen_nullable_string_cmp, nonnull_eq_null_spec_8_4_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: string? := \"hello\"; var b: string? := null; "
        "  if a = b then return 1; end; "
        "  return 0; "
        "end Main; end C;"), 0);
}

// §4.6: null <> "hello" is true.
BEZEL_TEST(codegen_nullable_string_cmp, null_neq_nonnull_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: string? := null; var b: string? := \"x\"; "
        "  return (1 if a <> b else 0); "
        "end Main; end C;"), 1);
}

// §4.6: "hello" <> null is true.
BEZEL_TEST(codegen_nullable_string_cmp, nonnull_neq_null_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: string? := \"hello\"; var b: string? := null; "
        "  return (1 if a <> b else 0); "
        "end Main; end C;"), 1);
}

// ---- §9.1/§4.6: two non-null strings — content equality -------------------------

// §9.1: two non-null `string?` variables with equal content compare equal.
BEZEL_TEST(codegen_nullable_string_cmp, nonnull_equal_content_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: string? := \"hello\"; var b: string? := \"hello\"; "
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 1);
}

// §9.1: two non-null `string?` variables with different content compare unequal.
BEZEL_TEST(codegen_nullable_string_cmp, nonnull_different_content_spec_9_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: string? := \"hello\"; var b: string? := \"world\"; "
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 0);
}

// §4.6 / §9.1: `<>` negates `=` — "a" <> "a" is false.
BEZEL_TEST(codegen_nullable_string_cmp, nonnull_neq_equal_content_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: string? := \"a\"; var b: string? := \"a\"; "
        "  return (1 if a <> b else 0); "
        "end Main; end C;"), 0);
}

// §4.6 / §9.1: `<>` — "a" <> "b" is true.
BEZEL_TEST(codegen_nullable_string_cmp, nonnull_neq_different_content_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: string? := \"a\"; var b: string? := \"b\"; "
        "  return (1 if a <> b else 0); "
        "end Main; end C;"), 1);
}

// ---- §4.6: non-null string? compared to a plain (non-nullable) string ----------

// §4.6: a non-nullable `string` and a `string?` that holds a value — equal
// content means they compare equal via the same content path.
BEZEL_TEST(codegen_nullable_string_cmp, nonnull_nullable_vs_plain_equal_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: string? := \"hi\"; var b: string := \"hi\"; "
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 1);
}

// §4.6: non-nullable `string` compared to a `string?` holding a different value.
BEZEL_TEST(codegen_nullable_string_cmp, nonnull_nullable_vs_plain_diff_spec_4_6) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public static method Main(): int "
        "  var a: string? := \"hi\"; var b: string := \"bye\"; "
        "  return (1 if a = b else 0); "
        "end Main; end C;"), 0);
}
