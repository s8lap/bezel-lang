// Codegen — explicit type args on a bare identifier generic-method call (issue #561).
//
// §14.1: generic methods are monomorphized at each call site; the return type is
// the concrete substituted type.  Issue #509 fixed explicit type args on a
// MemberAccessExpr callee (`Wrapper.Identity<int>(42)`).  Issue #561 is the
// SAME problem on a bare IdentifierExpr callee — a call to a generic method in
// the SAME CLASS without a dot prefix:
//
//   var s := Id<string>("hello");   // bare call — no `C.Id<...>` dot form
//
// Root cause (before fix): the parser treated `Id<string>("hello")` as the
// relational chain `(Id < string) > "hello"` — a comparison expression that
// yields a bool (i1).  `var s :=` then inferred s as bool.  Comparing s (i1 or
// i32) against `"hello"` (ptr) in the subsequent `if s = "hello"` produced an
// LLVM ICmpInst::AssertOK SIGABRT (exit 134).  For integer type args the crash
// was hidden by sign-extension handling the i1 vs i32 mismatch silently, but
// the result was still wrong.
//
// After fix (parser + sema + codegen):
//   - The parser's parsePostfix loop now recognises `Identifier < TypeArgList > (`
//     (disambiguated via tokensLookLikeGenericArgs) and stores the type args
//     on the IdentifierExpr, continuing the postfix chain so `(...)` forms a
//     normal CallExpr.
//   - sema's typeCall reads typeArgs from both MemberAccessExpr and IdentifierExpr
//     callees when substituting the return type.
//   - codegen's argument-inference pre-pass and main substitution setup similarly
//     check both callee node types.
//
// Spec references:
//   §14.1 — generic-method monomorphization; explicit type arguments override
//            inference; the return type is the substituted T, not the bare param.
//   §14.2 — constraint-bound substitution; same rules apply.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

bezel::test::JitResult jit(const std::string& prog) { return runAot(prog); }

// Class with a static generic method called via bare name (no dot).
const char* kC =
    "module M; "
    "class C "
    "  public static method Id<T>(x: T): T where T: notnull => x; ";

// Main entry in a separate class that calls C.Id via same-class MemberAccess.
// (The bare-identifier form only arises when the call is INSIDE the same class.)
const char* kCInner =
    "module M; "
    "class C "
    "  public static method Id<T>(x: T): T where T: notnull => x; "
    "  public static method Main(): int ";

}  // namespace

// ── §14.1 / #561: string type arg, untyped var — was SIGABRT before fix ──────

// Primary regression: `var s := Id<string>("hello")` in the same class as Id.
// Before fix: parsed as `(Id < string) > "hello"` (relational bool); SIGABRT
// at ICmpInst::AssertOK when comparing s (i32) against "hello" (ptr).
BEZEL_TEST(codegen_generic_method_bare_typeargs_561,
           bare_call_string_untyped_var_no_crash_14_1) {
    auto r = jit(
        "module M; "
        "class C "
        "  public static method Id<T>(x: T): T where T: notnull => x; "
        "  public static method Main(): int "
        "    var s := Id<string>(\"hello\"); "
        "    if s = \"hello\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ── §14.1 / #561: int type arg, untyped var ───────────────────────────────────

// Before fix: `var n := Id<int>(7)` was parsed as `(Id < int) > 7` — a bool.
// The comparison `n = 7` silently compared i1 with i32 (sign-extended; 0≠7 so
// result was always 0). After fix: n is int, `n = 7` is true → returns 1.
BEZEL_TEST(codegen_generic_method_bare_typeargs_561,
           bare_call_int_untyped_var_correct_value_14_1) {
    auto r = jit(
        "module M; "
        "class C "
        "  public static method Id<T>(x: T): T where T: notnull => x; "
        "  public static method Main(): int "
        "    var n := Id<int>(7); "
        "    if n = 7 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ── §14.1 / #561: double type arg, untyped var ───────────────────────────────

// Same pattern for double: `var d := Id<double>(1.5)` must yield d=1.5.
BEZEL_TEST(codegen_generic_method_bare_typeargs_561,
           bare_call_double_untyped_var_correct_value_14_1) {
    auto r = jit(
        "module M; "
        "class C "
        "  public static method Id<T>(x: T): T where T: notnull => x; "
        "  public static method Main(): int "
        "    var d := Id<double>(1.5); "
        "    if d = 1.5 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ── §14.1 / #561: bool type arg, untyped var ─────────────────────────────────

// `var b := Id<bool>(true)` — bool is a notnull value type.
BEZEL_TEST(codegen_generic_method_bare_typeargs_561,
           bare_call_bool_untyped_var_14_1) {
    auto r = jit(
        "module M; "
        "class C "
        "  public static method Id<T>(x: T): T where T: notnull => x; "
        "  public static method Main(): int "
        "    var b := Id<bool>(true); "
        "    if b then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ── §14.1 / #561: record type arg, untyped var ───────────────────────────────

// `var p := Id<Point>(new Point(3, 4))` — record type arg.
// After fix: p is typed as Point; p.x = 3 is readable.
BEZEL_TEST(codegen_generic_method_bare_typeargs_561,
           bare_call_record_untyped_var_field_access_14_1) {
    auto r = jit(
        "module M; "
        "record Point(x: int, y: int); "
        "class C "
        "  public static method Id<T>(x: T): T where T: notnull => x; "
        "  public static method Main(): int "
        "    var p := Id<Point>(new Point(3, 4)); "
        "    if p.x = 3 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ── §14.1 / #561: return directly from bare generic call ─────────────────────

// `return Id<int>(55)` in a typed method — no local var at all.
BEZEL_TEST(codegen_generic_method_bare_typeargs_561,
           bare_call_int_direct_return_14_1) {
    auto r = jit(
        "module M; "
        "class C "
        "  public static method Id<T>(x: T): T where T: notnull => x; "
        "  public static method Main(): int "
        "    return Id<int>(55); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 55);
}

// ── §14.1 / #561: multi-arg generic method, bare call ────────────────────────

// `First<int>(a, b)` where T appears in the first param.
BEZEL_TEST(codegen_generic_method_bare_typeargs_561,
           bare_call_multi_param_first_14_1) {
    auto r = jit(
        "module M; "
        "class C "
        "  public static method First<T>(a: T, b: int): T where T: notnull "
        "    return a; "
        "  end First; "
        "  public static method Main(): int "
        "    return First<int>(13, 0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 13);
}

// ── §14.1 / #509 regression: dot-form still works after #561 fix ─────────────

// `Wrapper.Identity<string>("hello")` — dot form must not be broken.
BEZEL_TEST(codegen_generic_method_bare_typeargs_561,
           regression_509_dot_form_string_still_works_14_1) {
    auto r = jit(
        "module M; "
        "class Wrapper "
        "  public static method Identity<T>(v: T): T where T: notnull => v; "
        "end Wrapper; "
        "class C "
        "  public static method Main(): int "
        "    var s := Wrapper.Identity<string>(\"hello\"); "
        "    if s = \"hello\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// ── §14.1 / #504 regression: inferred type arg still works ───────────────────

// `Id(7)` without explicit type arg — inferred T=int from argument.
BEZEL_TEST(codegen_generic_method_bare_typeargs_561,
           regression_504_inferred_typearg_still_works_14_1) {
    auto r = jit(
        "module M; "
        "class C "
        "  public static method Id<T>(x: T): T where T: notnull => x; "
        "  public static method Main(): int "
        "    var n := Id(42); "
        "    if n = 42 then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}
