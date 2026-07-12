// Codegen — two related generic IR-verification failures (issues #604 and #605).
//
// #604 / §14.1 / §22.4: generic CLASS with 3+ type parameters and nullable fields
// (`TFirst?`, `TSecond?`, `TThird?`) — methods returning each parameter via `_field!`
// (force-unwrap PostfixBangExpr on a nullable bare type-param) emit `ret { i8, i32 }
// %2` against an `i32` (or ptr/bool) function return type → IR-verify failure.
//
// #605 / §14.1 / §8.1: inside a generic method, `var s := val.ToString()` stores the
// result of a universal Object member call on a bare type-param receiver into an
// INFERRED-type local — codegen assigns `i32` (the type-param placeholder) to the
// slot and `return s` emits `ret i32 %3` against a `ptr`-return function → IR-verify.
//
// Root causes (before fix):
//
//   #604: isNullableValueTy(rt) returns false for a bare nullable type-param TypeRef
//   (kind=Named, nullable=true, no declaration), because there is no concrete
//   PrimitiveType/enum/record declaration attached. The PostfixBangExpr handler
//   therefore fell through without extracting the {i8, T} payload field, and returned
//   the whole struct — `ret { i8, i32 }` against the method's `i32` return → IR fail.
//   The fix resolves the bare type-param through genericSubstSemaTy_ to the concrete
//   nullable value type (nullableTypeParamConcrete helper, introduced in fix #568)
//   and handles it identically to a directly-known nullable value type.
//
//   #605: sema resolveMemberAccess left the MemberAccessExpr `val.ToString` untyped
//   for bare type-param receivers (kind=Named, no declaration, where T: value/notnull
//   constraints give only shape, not a named type surface). With no type on the
//   MemberAccess, typeCall's `!calleeSym` / `callee exprType` fallback could not
//   propagate the string type to the enclosing CallExpr. lowerLocalDecl could not
//   infer the var's slot type and fell back to `i32`. `return s` then emitted
//   `ret i32 %3` against a `ptr`-return function → IR verify failure.
//   The fix sets the MemberAccessExpr type for universal Object members
//   (ToString→string, GetHashCode→int, Equals→bool, GetType→Type) on bare
//   type-param receivers in resolveMemberAccess, which propagates through the
//   existing typeCall fallback path.
//
// Each test below must IR-verify-fail (or silently miscompile) before the fix and
// produce the spec-stated value after.

#include "support/aot_support.hpp"   // runAot — AOT test harness (JIT->AOT migration, P1)
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

bezel::test::JitResult jit(const std::string& prog) { return runAot(prog); }

int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// Fixture for #604: a 3-type-parameter class with nullable fields.
// §14.1: each type argument is independently substituted in method return types.
// §7.1: `T?` for a value-type T lowers to the {i8, T} tagged struct; `_field!`
// extracts the value field via the `!` (PostfixBangExpr) operator.
const char* kTripleNullable =
    "module M; "
    "class Triple<TFirst, TSecond, TThird> "
    "  private _first: TFirst?; "
    "  private _second: TSecond?; "
    "  private _third: TThird?; "
    "  public constructor(a: TFirst, b: TSecond, c: TThird) "
    "    _first := a; _second := b; _third := c; "
    "  end constructor; "
    "  public method GetFirst(): TFirst   return _first!;  end GetFirst; "
    "  public method GetSecond(): TSecond return _second!; end GetSecond; "
    "  public method GetThird(): TThird   return _third!;  end GetThird; "
    "end Triple; ";

}  // namespace

// ── #604 / §14.1 / §7.1: nullable field force-unwrap for 3 type parameters ──────

// PRIMARY REPRO (exact issue repro): Triple<int,string,bool>.GetFirst().
// Before fix: PostfixBangExpr returned the whole {i8,i32} struct → IR-verify fail.
BEZEL_TEST(generic_nullable_typeparam_604, get_first_int_14_1) {
    auto r = jit(std::string(kTripleNullable) +
        "class C "
        "  public static method Main(): int "
        "    using var t := new Triple<int, string, bool>(42, \"hello\", true); "
        "    return t.GetFirst(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 42);
}

// GetSecond() returns the SECOND parameter (string). Length == 5.
BEZEL_TEST(generic_nullable_typeparam_604, get_second_string_length_14_1) {
    BEZEL_EXPECT_EQ(run(std::string(kTripleNullable) +
        "class C "
        "  public static method Main(): int "
        "    using var t := new Triple<int, string, bool>(1, \"world\", true); "
        "    return t.GetSecond().Length; "
        "  end Main; "
        "end C;"), 5);
}

// GetThird() returns the THIRD parameter (bool). true → branch return 1.
BEZEL_TEST(generic_nullable_typeparam_604, get_third_bool_14_1) {
    BEZEL_EXPECT_EQ(run(std::string(kTripleNullable) +
        "class C "
        "  public static method Main(): int "
        "    using var t := new Triple<int, string, bool>(9, \"x\", true); "
        "    var b := t.GetThird(); "
        "    if b then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// All three getters in one program — the combined exit code verifies each.
BEZEL_TEST(generic_nullable_typeparam_604, all_three_params_combined_14_1) {
    auto r = jit(std::string(kTripleNullable) +
        "class C "
        "  public static method Main(): int "
        "    using var t := new Triple<int, string, bool>(7, \"abc\", true); "
        "    var a := t.GetFirst(); "      // 7
        "    var b := t.GetSecond(); "     // "abc" → Length 3
        "    var ok := t.GetThird(); "     // true
        "    if ok then return a + b.Length; end; "   // 7 + 3 = 10
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// 4 type parameters — extends the pattern one more step beyond the 3-param repro.
BEZEL_TEST(generic_nullable_typeparam_604, four_params_nullable_fields_14_1) {
    const char* kQuad =
        "module M; "
        "class Quad<A,B,C,D> "
        "  private _a: A?; "
        "  private _b: B?; "
        "  private _c: C?; "
        "  private _d: D?; "
        "  public constructor(a: A, b: B, c: C, d: D) "
        "    _a := a; _b := b; _c := c; _d := d; end constructor; "
        "  public method GetA(): A return _a!; end GetA; "
        "  public method GetD(): D return _d!; end GetD; "
        "end Quad; ";
    BEZEL_EXPECT_EQ(run(std::string(kQuad) +
        "class C "
        "  public static method Main(): int "
        "    using var q := new Quad<int, string, bool, int>(3, \"hi\", false, 11); "
        "    return q.GetA() + q.GetD(); "   // 3 + 11 = 14
        "  end Main; "
        "end C;"), 14);
}

// Regression: 2-param class with nullable fields (the pre-existing coverage domain)
// must still work after the #604 fix.
BEZEL_TEST(generic_nullable_typeparam_604, two_params_regression_14_1) {
    const char* kPair =
        "module M; "
        "class Pair<A,B> "
        "  private _a: A?; "
        "  private _b: B?; "
        "  public constructor(a: A, b: B) _a := a; _b := b; end constructor; "
        "  public method GetA(): A return _a!; end GetA; "
        "  public method GetB(): B return _b!; end GetB; "
        "end Pair; ";
    BEZEL_EXPECT_EQ(run(std::string(kPair) +
        "class C "
        "  public static method Main(): int "
        "    using var p := new Pair<int, string>(20, \"hi\"); "
        "    return p.GetA() + p.GetB().Length; "   // 20 + 2 = 22
        "  end Main; "
        "end C;"), 22);
}

// ── #605 / §14.1 / §8.1: inferred-type local from T.ToString() in generic method ──

// PRIMARY REPRO (exact issue repro): `var s := val.ToString()` in a generic method.
// Before fix: s gets an i32 slot → `ret i32 %3` in a ptr function → IR-verify fail.
BEZEL_TEST(generic_nullable_typeparam_605, inferred_tostring_var_14_1) {
    auto r = jit(
        "module M; "
        "static class Utils "
        "  public static method ToStr<T>(val: T): string "
        "    where T: value "
        "    var s := val.ToString(); "
        "    return s; "
        "  end ToStr; "
        "end Utils; "
        "class C "
        "  public static method Main(): int "
        "    var result := Utils.ToStr<int>(42); "
        "    return result.Length; "   // "42" → Length 2
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 2);
}

// The explicit-annotation workaround must keep working — `var s: string := ...`.
// (This worked before #605; used as a regression guard.)
BEZEL_TEST(generic_nullable_typeparam_605, explicit_annotation_tostring_still_works_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "static class Utils "
        "  public static method ToStr<T>(val: T): string "
        "    where T: value "
        "    var s: string := val.ToString(); "
        "    return s; "
        "  end ToStr; "
        "end Utils; "
        "class C "
        "  public static method Main(): int "
        "    return Utils.ToStr<int>(99).Length; "  // "99" → 2
        "  end Main; "
        "end C;"), 2);
}

// Direct return also worked pre-fix; ensure the fix does not perturb it.
BEZEL_TEST(generic_nullable_typeparam_605, direct_return_tostring_still_works_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "static class Utils "
        "  public static method ToStr<T>(val: T): string "
        "    where T: value "
        "    return val.ToString(); "
        "  end ToStr; "
        "end Utils; "
        "class C "
        "  public static method Main(): int "
        "    return Utils.ToStr<int>(999).Length; "  // "999" → 3
        "  end Main; "
        "end C;"), 3);
}

// Inferred local from ToString() used via .Length — exercises that the inferred
// string type propagates to member-access on the local (not just bare return).
BEZEL_TEST(generic_nullable_typeparam_605, inferred_tostring_length_usage_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "static class Utils "
        "  public static method ToStrLen<T>(val: T): int "
        "    where T: value "
        "    var s := val.ToString(); "
        "    return s.Length; "
        "  end ToStrLen; "
        "end Utils; "
        "class C "
        "  public static method Main(): int "
        "    return Utils.ToStrLen<int>(12345); "   // "12345" → 5
        "  end Main; "
        "end C;"), 5);
}

// Multiple inferred-type locals from T-dependent calls in the same method body.
BEZEL_TEST(generic_nullable_typeparam_605, multiple_inferred_locals_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "static class Utils "
        "  public static method TwoStrs<T>(a: T, b: T): int "
        "    where T: value "
        "    var sa := a.ToString(); "
        "    var sb := b.ToString(); "
        "    return sa.Length + sb.Length; "
        "  end TwoStrs; "
        "end Utils; "
        "class C "
        "  public static method Main(): int "
        "    return Utils.TwoStrs<int>(42, 100); "  // "42".Length(2) + "100".Length(3) = 5
        "  end Main; "
        "end C;"), 5);
}

// Non-int type arg: T=bool. ToString() on bool → "True" (Length 4).
BEZEL_TEST(generic_nullable_typeparam_605, inferred_tostring_bool_arg_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "static class Utils "
        "  public static method ToStr<T>(val: T): string "
        "    where T: value "
        "    var s := val.ToString(); "
        "    return s; "
        "  end ToStr; "
        "end Utils; "
        "class C "
        "  public static method Main(): int "
        "    var r := Utils.ToStr<bool>(true); "
        "    return r.Length; "    // "True" → 4
        "  end Main; "
        "end C;"), 4);
}
