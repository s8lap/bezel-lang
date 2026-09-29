// Spec-derived tests for fix #5162 — §14.1/§14.2/§5.1, v0.310.
//
// Unfiled finding surfaced by the #5127 lane (test commit 536ab4dfd,
// tests/codegen/method_own_type_param_escape_release_5127_test.cpp's header
// comment): a VOID-returning generic method with a bare method-own type-
// parameter formal (`method M<U>(x: U)`), called with an EXPLICIT call-site
// type-argument list (`c.M<() -> int>(f)`), rejected a genuine function-
// typed argument with:
//
//   the argument `x` to call to `M` has a type that is not implicitly
//   convertible to the target type (§5.1)
//
// The byte-identical shape with a non-void return type (`method M<U>(x: U):
// int`) compiled cleanly at the same call site.
//
// ROOT CAUSE (src/sema/ExprTypecheck_Calls.cpp, `ExprTC::typeCall`): the
// explicit-type-argument-list substitution map `explicitSubst` (built from
// the call's own `<...>` list and later consumed by
// `checkCallArity(ce, ms, &explicitSubst)`, which substitutes each generic
// formal's declared type before running the §5.1 argument-convertibility
// check) was populated ONLY inside the `if (md && md->returnType) { ... }`
// arm, which also performs the (unrelated) return-type substitution. The
// void sibling arm, `else if (md && !md->returnType)`, only set the call's
// own type to `Unit` and never touched `explicitSubst` — so for a void
// method the map stayed empty, the formal's declared type stayed the bare
// `U`, and a concrete function-typed argument was compared against `U`
// itself and rejected, purely because the callee happened to be void.
// Neither §14.1 (type-parameter substitution) nor §14.2 (constraint
// checking) conditions generic-parameter substitution on the callee's
// return type.
//
// THE FIX: populate `explicitSubst` from the call's own explicit type-
// argument list in the void arm too (mirroring the non-void arm's explicit-
// list handling, minus the return-type substitution, which does not apply
// to a void call), and re-run `checkMethodGenericConstraints`/
// `checkAsyncInstantiationBorrowBearing` against it for parity with the
// non-void arm.
//
// Test oracle: `runAot` (the real AOT compile+link+exec path) for every
// accept row — proves the call is not just ACCEPTED by sema but actually
// DISPATCHES and invokes the passed-in function value correctly at runtime.
// Reject controls use `failedAtCompileTimeWithRealLocation` plus specific
// diagnostic text (§18 of the lane process).
//
// Not in scope: the argument-INFERRED (no explicit type-argument list) case
// for a function-typed argument against a bare `U` formal fails to compile
// for BOTH void and non-void return types — a pre-existing, unrelated
// generic-argument-inference limitation for function-typed arguments, not a
// void-specific regression. A non-author review found the original two rows
// here mis-pinned that reject as correct behavior: §14.5 phase 1, v0.310
// lists "a function-typed value" as self-determined, so `c.M(f)` with a
// named function-typed local `f` against bare `U` should bind
// `U := () -> int` and be ACCEPTED. Filed as #5178; the rows are removed
// rather than pinned so a fix does not have to invalidate this file.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::runAot;

// ---------------------------------------------------------------------------
// ACCEPT (THE BUG): the issue's own repro, verbatim shape — void-returning
// generic method, bare `U` formal, explicit `<() -> int>` type argument, a
// real function-typed caller argument. FAILS WITHOUT THE FIX: rejected with
// the §5.1 "not implicitly convertible" diagnostic.
// ---------------------------------------------------------------------------
BEZEL_TEST(void_generic_method_bare_typeparam_function_arg_5162,
           void_method_explicit_type_arg_accepts_function_value) {
    auto r = runAot(
        "module M; "
        "class Sink5162a "
        "  public method Apply<U>(x: U) "
        "    Console.WriteLine(\"applied\"); "
        "  end Apply; "
        "end Sink5162a; "
        "public static class Program5162a "
        "  public static method Main(): int "
        "    var s := new Sink5162a(); "
        "    var f: () -> int := lambda () => 42; "
        "    s.Apply<() -> int>(f); "
        "    Console.WriteLine(f().ToString()); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "applied\n42\n");
}

// ---------------------------------------------------------------------------
// ACCEPT CONTROL: the non-void sibling shape — same formal, same call site,
// only the return type differs (`int` instead of void). Must stay accepted
// before and after the fix (proves the fix does not merely relax the void
// arm in a way that would also require re-checking the already-working
// non-void arm — it is untouched).
// ---------------------------------------------------------------------------
BEZEL_TEST(void_generic_method_bare_typeparam_function_arg_5162,
           non_void_sibling_still_accepts_function_value_control) {
    auto r = runAot(
        "module M; "
        "class Sink5162b "
        "  public method Apply<U>(x: U): int "
        "    return 7; "
        "  end Apply; "
        "end Sink5162b; "
        "public static class Program5162b "
        "  public static method Main(): int "
        "    var s := new Sink5162b(); "
        "    var f: () -> int := lambda () => 7; "
        "    return s.Apply<() -> int>(f) - 7; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ---------------------------------------------------------------------------
// ACCEPT: the fix's `checkMethodGenericConstraints` re-run must not merely
// skip constraint checking — a `where U: IComparable<U>` bound satisfied by
// the explicit type argument (`int`) must still compile for a void method.
// ---------------------------------------------------------------------------
BEZEL_TEST(void_generic_method_bare_typeparam_function_arg_5162,
           void_method_explicit_type_arg_satisfied_constraint_accepted) {
    auto r = runAot(
        "module M; "
        "class Sink5162c "
        "  public method Apply<U>(x: U) where U: IComparable<U> "
        "    Console.WriteLine(x.CompareTo(x).ToString()); "
        "  end Apply; "
        "end Sink5162c; "
        "public static class Program5162c "
        "  public static method Main(): int "
        "    var s := new Sink5162c(); "
        "    s.Apply<int>(5); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "0\n");
}

// ---------------------------------------------------------------------------
// REJECT CONTROL: a void method's explicit type argument that violates its
// own `where` constraint must still be rejected — proves the fix's
// constraint re-check in the void arm is a REAL check, not a blanket accept
// that merely stopped mis-typing the formal.
// ---------------------------------------------------------------------------
BEZEL_TEST(void_generic_method_bare_typeparam_function_arg_5162,
           void_method_explicit_type_arg_violated_constraint_still_rejected) {
    auto r = runAot(
        "module M; "
        "class NotComparable5162 "
        "end NotComparable5162; "
        "class Sink5162d "
        "  public method Apply<U>(x: U) where U: IComparable<U> "
        "  end Apply; "
        "end Sink5162d; "
        "public static class Program5162d "
        "  public static method Main(): int "
        "    var s := new Sink5162d(); "
        "    var v := new NotComparable5162(); "
        "    s.Apply<NotComparable5162>(v); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, "NotComparable5162"));
}

// ---------------------------------------------------------------------------
// REJECT CONTROL: an explicit type argument that genuinely mismatches the
// argument's own type must still be rejected for a void method — proves the
// fix's substitution makes the §5.1 check REAL again, not vacuously true.
// ---------------------------------------------------------------------------
BEZEL_TEST(void_generic_method_bare_typeparam_function_arg_5162,
           void_method_explicit_type_arg_mismatched_argument_still_rejected) {
    auto r = runAot(
        "module M; "
        "class Sink5162e "
        "  public method Apply<U>(x: U) "
        "  end Apply; "
        "end Sink5162e; "
        "public static class Program5162e "
        "  public static method Main(): int "
        "    var s := new Sink5162e(); "
        "    var b := true; "
        "    s.Apply<int>(b); "
        "    return 0; "
        "  end Main; "
        "end;");
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "not implicitly convertible to the target type"));
}

// The argument-INFERRED (no explicit type-argument list) case for a
// function-typed VALUE argument against a bare `U` formal — for both void
// and non-void return types — is tracked as a separate bug: #5178
// (§14.5 phase 1, v0.310: a function-typed value is self-determined and
// should bind `U` by unification, not fail to compile).
