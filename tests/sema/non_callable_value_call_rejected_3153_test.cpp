// fix #3153 (§27.7, v0.255): a call on a non-callable value (an
// int/string/class-typed local, parameter, or static field) was silently
// accepted and DROPPED at codegen. The CallExpr carried no sema type
// (typeCall's function-typed-callee branch only sets exprType when the
// callee's declType is Function), so `var y := x()` degraded to a zero-
// initialized slot (#2434's "internal: lowerLocalDecl's initializer
// expression failed to lower to a value"). A statement-only `x();`
// vanished entirely.
//
// §27.7 is the "calling function values" surface: a call's callee is one
// of
//   (a) a named METHOD reference (§27.5),
//   (b) a CONSTRUCTOR reference (§9.5), or
//   (c) a function-TYPED value (a local/parameter/field/element declared
//       or inferred to a function type, §27.7).
// A bare value of any other type is NOT a callable and must be rejected
// — not silently dropped. The fix adds the missing reject in
// ExprTypecheck_Calls.cpp's typeCall (LocalSymbol/ParameterSymbol/
// FieldSymbol callee block), at the existing function-typed-admission
// site so a positive (Function) case is unchanged.

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some diagnostic names the §27.7 "only function-typed values are
// callable" rule (the message fix #3153 emits).
bool hasNonCallableError(const bezel::test::SemaResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("only function-typed values are callable") !=
            std::string::npos)
            return true;
    return false;
}

}  // namespace

// ===========================================================================
// NEGATIVE — a call on a non-callable value must be REJECTED at sema,
// not silently dropped at codegen.
// ===========================================================================

// §27.7: `let x := 42; x();` — `x` is an int local, NOT a function value.
BEZEL_TEST(sema_non_callable_value_call_rejected_3153,
           int_local_call_rejected_spec_27_7) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method M(): int "
        "    var x: int := 42; "
        "    x(); "
        "    return 0; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonCallableError(r));
}

// §27.7: a string-typed local is also not a function value.
BEZEL_TEST(sema_non_callable_value_call_rejected_3153,
           string_local_call_rejected_spec_27_7) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method M(): int "
        "    var s: string := \"hi\"; "
        "    s(); "
        "    return 0; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonCallableError(r));
}

// §27.7: a class-typed local is not a function value either — Bezel has no
// implicit "call on a class" operator.
BEZEL_TEST(sema_non_callable_value_call_rejected_3153,
           class_local_call_rejected_spec_27_7) {
    auto r = parseSemaAll(
        "module M; "
        "class D end D; "
        "class C "
        "  public static method M(): int "
        "    var d: D := new D(); "
        "    d(); "
        "    return 0; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonCallableError(r));
}

// §27.7: an interface-typed local is not a function value (a `IFoo?`
// instance carries an IID, not an invocation slot).
BEZEL_TEST(sema_non_callable_value_call_rejected_3153,
           interface_local_call_rejected_spec_27_7) {
    auto r = parseSemaAll(
        "module M; "
        "interface IFoo end IFoo; "
        "class C "
        "  public static method M(): int "
        "    var f: IFoo? := null; "
        "    f(); "
        "    return 0; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonCallableError(r));
}

// §27.7: a parameter of non-function type is not a function value either.
BEZEL_TEST(sema_non_callable_value_call_rejected_3153,
           int_parameter_call_rejected_spec_27_7) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method M(x: int): int "
        "    x(); "
        "    return 0; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonCallableError(r));
}

// §27.7: a static field of non-function type is not a function value.
BEZEL_TEST(sema_non_callable_value_call_rejected_3153,
           static_field_call_rejected_spec_27_7) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static readonly F: int := 42; "
        "  public static method M(): int "
        "    F(); "
        "    return 0; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonCallableError(r));
}

// §27.7: the diagnostic also fires on the assignment form
// `var y := x()` — the silent-drop case that produced #2434's
// "internal: lowerLocalDecl's initializer expression failed to lower to
// a value".
BEZEL_TEST(sema_non_callable_value_call_rejected_3153,
           int_local_call_in_rhs_rejected_spec_27_7) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method M(): int "
        "    var x: int := 42; "
        "    var y: int := x(); "
        "    return y; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasNonCallableError(r));
}

// ===========================================================================
// POSITIVE — function-typed callees and named-method callees must remain
// accepted. The fix is purely an added reject; the function-typed and
// named-method paths above the fix site are untouched.
// ===========================================================================

// §27.7: a function-typed local callee — the positive case the existing
// LocalSymbol branch in typeCall handles — must still be accepted.
BEZEL_TEST(sema_non_callable_value_call_rejected_3153,
           function_typed_local_call_accepted_spec_27_7) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method M(): int "
        "    var f: (int) -> int := lambda(n: int): int => n + 1; "
        "    var y: int := f(41); "
        "    return y; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.5: a named-method callee (the §27.5 call form) must still be
// accepted — the fix is only for the value-side callee path.
BEZEL_TEST(sema_non_callable_value_call_rejected_3153,
           named_method_call_accepted_spec_27_5) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method Foo(x: int): int return x * 2; end Foo; "
        "  public static method M(): int "
        "    return Foo(21); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §27.7: a function-typed parameter callee — the positive case the
// existing ParameterSymbol branch handles — must still be accepted.
BEZEL_TEST(sema_non_callable_value_call_rejected_3153,
           function_typed_parameter_call_accepted_spec_27_7) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method M(f: (int) -> int): int "
        "    return f(40); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// fix #3153 amend (§27.7 + §27.2): a VOID-returning function-typed LOCAL
// (the §27.2 expression-body lambda whose body is a void intrinsic call,
// `var log := lambda(msg: string) => Console.WriteLine(msg); log("x");`)
// must still be accepted. The original #3153 patch's `else if
// (loc->declType)` arm at the LocalSymbol branch was over-broad — for a
// void-returning lambda the declType has `kind == Function` but a null
// `funcReturn`, so the original `&& loc->declType->funcReturn` guard let
// it fall through, and the new reject arm then incorrectly emitted
// "cannot call a value of type `(fn ...)`". The amend splits the three
// cases: kind==Function with non-null funcReturn -> set exprType; kind==
// Function with null funcReturn -> accept (void lambda); anything else
// -> §27.7 reject.
BEZEL_TEST(sema_non_callable_value_call_rejected_3153,
           void_returning_lambda_call_accepted_spec_27_7_27_2) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method M(): int "
        "    var log := lambda(msg: string) => Console.WriteLine(msg); "
        "    log(\"hello\"); "
        "    return 0; "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}