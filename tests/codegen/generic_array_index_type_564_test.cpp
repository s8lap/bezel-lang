// Codegen — generic method returning owned Array<T> indexed in an inferred var
// (issue #564).
//
// §14.1 / §22.1: when a generic method returns `owned Array<T>` and the call
// has explicit type args (`MakeTwo<int>(7, 14)`), the call expression must be
// typed as the concrete `Array<int>`, NOT the unsubstituted `Array<T>`.  An
// inferred `using var arr := MakeTwo<int>(...)` must therefore record
// `Array<int>` as `arr`'s type; `arr[i]` (IndexExpr §22.1) yields element
// type `int`, not the bare unresolved `T` (which prints as `?` in a diagnostic
// and causes an ICmpInst::AssertOK SIGABRT when compared to a string literal).
//
// Root cause (pre-fix): `typeCall` in ExprTypecheck_Calls.cpp only substituted
// the return-type's method-level generic param when the return type was a BARE
// parameter reference (`rt` = Named{"T", no-decl, parts.size()==1}).  For a
// compound return type like `Array<T>` (a null-declaration stdlib Named whose
// single generic arg is the bare `T`), neither the explicit-typeArgs block nor
// the argument-inferred block triggered substitution, so `arr` was typed as
// `Array<T>` (unsubstituted).  Additionally, `substituteReturnType` only
// recursed into genericArgs when `declaration != nullptr`, missing the
// null-declaration stdlib case.
//
// The fix:
//   1. `substituteReturnType`: allow substitution into genericArgs for ANY
//      Named TypeRef with non-empty genericArgs (not just those with a
//      non-null declaration).  This covers `Array<T>`, `OwnedArray<T>`, etc.
//   2. `typeCall` explicit-typeArgs block (issue #509 / §14.1): always apply
//      `substituteReturnType` with the built explicit-subst map, so compound
//      return types like `Array<T>` get their param substituted.
//   3. `typeCall` argument-inferred block (issue #504 / §14.1): same — build a
//      subst map from the matched argument type and call `substituteReturnType`
//      so compound return types are handled in addition to bare params.
//
// Spec refs:
//   §14.1 — generic-method monomorphization; call-site type args substitute
//            into the declared return type.
//   §22.1 — index expression `a[i]` on `Array<T>` has element type `T`.

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runJit;

namespace {

// Shared helper: a static class with a generic method that builds and returns
// an `owned Array<T>` populated with two values.
const char* kMakeTwo =
    "module M; "
    "class C "
    "  public static method MakeTwo<T>(a: T, b: T): owned Array<T> "
    "      where T: notnull "
    "    using var arr := new Array<T>(); "
    "    arr.Add(a); "
    "    arr.Add(b); "
    "    return arr; "
    "  end MakeTwo; ";

int run(const std::string& prog) {
    auto r = runJit(prog);
    return r.ran ? r.exitCode : -999;
}

bezel::test::JitResult jit(const std::string& prog) { return runJit(prog); }

}  // namespace

// ── §14.1 / §22.1: int element — inferred var + arithmetic ───────────────────

// PRIMARY REPRO (MED diagnostic): `using var arr := MakeTwo<int>(7, 14);`
// followed by `var a := arr[0]; var b := arr[1]; return a + b;`.
// Before fix: arr typed as Array<T> (unsubstituted) → arr[0] typed as T →
// `a` and `b` typed as `?` → operator '+' not defined for type `?` diagnostic
// (compile-time error; never ran).
// After fix: arr typed as Array<int> → arr[0] typed as int → a+b = 21.
BEZEL_TEST(generic_array_index_564, int_inferred_var_index_plus_14_1_22_1) {
    std::string prog =
        std::string(kMakeTwo) +
        "  public static method Main(): int "
        "    using var arr := MakeTwo<int>(7, 14); "
        "    var a := arr[0]; "
        "    var b := arr[1]; "
        "    return a + b; "
        "  end Main; "
        "end C;";
    auto r = jit(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 21);
}

// §14.1 / §22.1: two-element array, direct element access.
BEZEL_TEST(generic_array_index_564, int_element_first_14_1_22_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kMakeTwo) +
        "  public static method Main(): int "
        "    using var arr := MakeTwo<int>(3, 9); "
        "    var x := arr[0]; "
        "    return x; "
        "  end Main; "
        "end C;"), 3);
}

BEZEL_TEST(generic_array_index_564, int_element_second_14_1_22_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kMakeTwo) +
        "  public static method Main(): int "
        "    using var arr := MakeTwo<int>(3, 9); "
        "    var x := arr[1]; "
        "    return x; "
        "  end Main; "
        "end C;"), 9);
}

// ── §14.1 / §22.1: string element — inferred var + equality (was ICE) ────────

// CRITICAL REPRO (ICE): `var s := arr[0]; if s = "hello" then return 1; end;`
// Before fix: arr typed as Array<T>, arr[0] typed as T (`?`), s typed as T.
// Comparing s (typed as unresolved T → i32 placeholder) against "hello" (ptr)
// caused ICmpInst::AssertOK assertion failure ("operands not same type") → SIGABRT.
// After fix: arr typed as Array<string>, arr[0] typed as string → s is string,
// string equality check compiles and returns 1.
BEZEL_TEST(generic_array_index_564, string_element_equality_no_ice_14_1_22_1) {
    std::string prog =
        std::string(kMakeTwo) +
        "  public static method Main(): int "
        "    using var arr := MakeTwo<string>(\"hello\", \"world\"); "
        "    var s := arr[0]; "
        "    if s = \"hello\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;";
    auto r = jit(prog);
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// §14.1 / §22.1: second string element.
BEZEL_TEST(generic_array_index_564, string_element_second_no_ice_14_1_22_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kMakeTwo) +
        "  public static method Main(): int "
        "    using var arr := MakeTwo<string>(\"hello\", \"world\"); "
        "    var s := arr[1]; "
        "    if s = \"world\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// ── §14.1 / §22.1: argument-inferred type arg + compound return ──────────────

// Argument-inferred T (no explicit <int>) — the return type is still
// `owned Array<T>` but T is bound by the arguments.  Before fix the
// argument-inferred block also only substituted bare-param return types.
// After fix it also substitutes compound return types (Array<T> → Array<int>).
BEZEL_TEST(generic_array_index_564, arg_inferred_int_element_14_1_22_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kMakeTwo) +
        "  public static method Main(): int "
        "    using var arr := MakeTwo(5, 10); "
        "    var x := arr[0]; "
        "    return x; "
        "  end Main; "
        "end C;"), 5);
}

BEZEL_TEST(generic_array_index_564, arg_inferred_string_element_14_1_22_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kMakeTwo) +
        "  public static method Main(): int "
        "    using var arr := MakeTwo(\"foo\", \"bar\"); "
        "    var s := arr[0]; "
        "    if s = \"foo\" then return 1; end; "
        "    return 0; "
        "  end Main; "
        "end C;"), 1);
}

// ── Regression guards ─────────────────────────────────────────────────────────

// Explicit annotation `var a: int := arr[0]` already worked pre-fix (typed
// local, assignment checks int); must remain working after fix.
BEZEL_TEST(generic_array_index_564, explicit_annotation_still_works_14_1_22_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kMakeTwo) +
        "  public static method Main(): int "
        "    using var arr := MakeTwo<int>(7, 14); "
        "    var a: int := arr[0]; "
        "    var b: int := arr[1]; "
        "    return a + b; "
        "  end Main; "
        "end C;"), 21);
}

// Direct `return arr[0]` without an inferred intermediate var already worked;
// must remain working.
BEZEL_TEST(generic_array_index_564, direct_return_still_works_14_1_22_1) {
    BEZEL_EXPECT_EQ(run(
        std::string(kMakeTwo) +
        "  public static method Main(): int "
        "    using var arr := MakeTwo<int>(7, 14); "
        "    return arr[0]; "
        "  end Main; "
        "end C;"), 7);
}

// Non-generic `owned Array<int>` return (no generic method involved) must
// remain working — the fix must not break the non-generic index path.
BEZEL_TEST(generic_array_index_564, non_generic_array_return_still_works_22_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method MakePair(): owned Array<int> "
        "    using var arr := new Array<int>(); "
        "    arr.Add(4); arr.Add(8); "
        "    return arr; "
        "  end MakePair; "
        "  public static method Main(): int "
        "    using var arr := MakePair(); "
        "    var x := arr[0]; "
        "    return x; "
        "  end Main; "
        "end C;"), 4);
}

// Bare type param return (e.g. `Identity<T>(v: T): T`) must still work —
// the existing bare-param substitution must not be broken by the fix.
BEZEL_TEST(generic_array_index_564, bare_param_return_still_works_14_1) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "class C "
        "  public static method Id<T>(x: T): T where T: notnull => x; "
        "  public static method Main(): int "
        "    using var arr := new Array<int>(); "
        "    arr.Add(Id<int>(42)); "
        "    return arr[0]; "
        "  end Main; "
        "end C;"), 42);
}
