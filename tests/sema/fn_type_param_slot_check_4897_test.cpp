// fix #4897 (§27.1.1 v0.307 / §27.6 / §27.4; stdlib §6.5): checkAssignable's
// Function-to-Function check (ExprTypecheck.cpp) carves out BOTH a fresh
// lambda literal (`freshLambda`) and a fresh method/property reference
// (`freshReferenceFormation`) from its parameter/return SHAPE check
// (`skipShapeCheck`) — correctly, per §27.3/§27.4 ("the lambda's parameters
// take the target's parameter types") and §27.6 ("forming a reference from a
// named method ... may bridge any §5.1 implicit conversion"). But two gaps
// let a real parameter-type MISMATCH through silently:
//
//   1. An EXPLICITLY ANNOTATED lambda parameter is not itself target-typed —
//      §27.4's inference story is for an OMITTED annotation ("parameter type
//      annotations may be omitted"); a written annotation instead behaves as
//      a declared parameter type, exactly as the return-type ascription
//      already does (§27.3 "Admission at a target-typed non-call position":
//      "a function-typed value at the same position ... takes §27.1.1's
//      narrower rule unchanged"). Pre-fix, `skipShapeCheck` skipped the
//      parameter check unconditionally for ANY freshLambda, annotated or not.
//
//   2. A BARE (unqualified) method-group identifier — `OnS` naming an
//      enclosing method, as opposed to the qualified `C.OnS`/`obj.OnS` form —
//      never receives a Function TypeRef at all: ExprTypecheck_Exprs.cpp's
//      IdentifierExpr name-lookup cascade ("Slice 3b") has arms for
//      LocalSymbol/ParameterSymbol/FieldSymbol/PropertySymbol/
//      ConstMemberSymbol but none for MethodSymbol (unlike resolveMemberAccess,
//      which already types the QUALIFIED spelling correctly). So
//      checkAssignable's `from` arrives null, the entire Function-to-Function
//      block is skipped, and any parameter-type mismatch is silently
//      admitted. `checkEventHandlerArgument` (fix #4869/#4968)'s own early
//      `if (!h ...) return;` has the identical gap for the `+=`/`-=` forms.
//
// The fix adds a NEW `functionTypeFromBareMethodSymbol` helper (mirroring the
// shape resolveMemberAccess already builds for the qualified spelling) that
// synthesizes the missing Function TypeRef for a bare MethodSymbol reference,
// called from both checkAssignable and checkEventHandlerArgument; and adds a
// per-parameter annotation check inside the `freshLambda` branch, run only
// for parameters that actually carry an explicit annotation (an unannotated
// parameter's synthesized type IS the target's, by construction, so the
// check is a no-op there — proven by the accept rows below).
//
// The REJECT rows FAIL-WITHOUT the fix (checked out `git checkout 6bf1ed38 --
// src/sema/ExprTypecheck.cpp src/sema/ExprTypecheck_impl.hpp`, rebuilt,
// re-ran): all five compiled clean and ran (silent type confusion) pre-fix.
// The ACCEPT rows RUN (runAot) and assert the printed value, proving the new
// checks are not spuriously over-rejecting the inference / matching /
// contravariant-admission shapes.

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot — AOT + native exec oracle
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

const bezel::Diagnostic* findDiag(const bezel::test::JitResult& r,
                                  const char* needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return &d;
    return nullptr;
}

constexpr const char* kLambdaParamNeedle =
    "type annotation does not agree with the target function type's "
    "parameter slot";
constexpr const char* kNotConvertibleNeedle =
    "has a type that is not implicitly convertible to the target type "
    "(\xc2\xa7""5.1)";

}  // namespace

// ---------------------------------------------------------------------------
// REJECT 1/5 — an explicitly-annotated lambda's mismatching parameter type,
// passed as a CALL ARGUMENT against `Take(f: (int) -> ())`.
// ---------------------------------------------------------------------------
BEZEL_TEST(fn_type_param_slot_check_4897,
           lambda_call_arg_annotation_mismatch_rejected_spec_27_1_1) {
    auto r = runAot(
        "module M; import System; "
        "class C "
        "  public static method Take(f: (int) -> ()) f(1); end Take; "
        "  public static method Main(): int "
        "    Take(lambda(n: string) => Console.WriteLine(n)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    const auto* d = findDiag(r, kLambdaParamNeedle);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (!d) return;
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// REJECT 2/5 — the same annotation mismatch at a TYPED-LOCAL INITIALIZER.
// ---------------------------------------------------------------------------
BEZEL_TEST(fn_type_param_slot_check_4897,
           lambda_typed_local_annotation_mismatch_rejected_spec_27_1_1) {
    auto r = runAot(
        "module M; import System; "
        "class C "
        "  public static method Main(): int "
        "    var f: (int) -> () := lambda(n: string) => Console.WriteLine(n); "
        "    return 0; "
        "  end Main; "
        "end C;");
    const auto* d = findDiag(r, kLambdaParamNeedle);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (!d) return;
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// REJECT 3/5 — a bare STATIC method-group reference whose parameter type
// mismatches the target: `var f: (int) -> () := OnS;` with `OnS(s: string)`.
// ---------------------------------------------------------------------------
BEZEL_TEST(fn_type_param_slot_check_4897,
           method_group_static_param_mismatch_rejected_spec_27_1_1) {
    auto r = runAot(
        "module M; import System; "
        "class C "
        "  public static method OnS(s: string) Console.WriteLine(s); "
        "  end OnS; "
        "  public static method Main(): int "
        "    var f: (int) -> () := OnS; "
        "    return 0; "
        "  end Main; "
        "end C;");
    const auto* d = findDiag(r, kNotConvertibleNeedle);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (!d) return;
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// REJECT 4/5 — the same mismatch for a bare INSTANCE method-group reference,
// read from inside another instance method of the same class (implicit
// `this.OnS`).
// ---------------------------------------------------------------------------
BEZEL_TEST(fn_type_param_slot_check_4897,
           method_group_instance_param_mismatch_rejected_spec_27_1_1) {
    auto r = runAot(
        "module M; import System; "
        "class C "
        "  public method OnS(s: string) Console.WriteLine(s); end OnS; "
        "  public method Main2() "
        "    var f: (int) -> () := OnS; "
        "  end Main2; "
        "  public static method Main(): int "
        "    using var c := new C(); "
        "    c.Main2(); "
        "    return 0; "
        "  end Main; "
        "end C;");
    const auto* d = findDiag(r, kNotConvertibleNeedle);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (!d) return;
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// REJECT 5/5 — a bare method-group EVENT HANDLER (`+=`) whose parameter type
// mismatches the EventSource<T> payload: stdlib §6.5's `Add(handler: (T) ->
// ())` desugaring (§17.2), T = int, handler OnS(s: string).
// ---------------------------------------------------------------------------
BEZEL_TEST(fn_type_param_slot_check_4897,
           method_group_event_plus_eq_param_mismatch_rejected_spec_17_2) {
    auto r = runAot(
        "module M; import System; "
        "class Beeper "
        "  private owned _beep: EventSource<int>; "
        "  public constructor() _beep := new EventSource<int>(); "
        "  end constructor; "
        "  public method Hook() _beep += OnS; end Hook; "
        "  public static method OnS(s: string) Console.WriteLine(s); "
        "  end OnS; "
        "  public method Fire(n: int) _beep.Raise(n); end Fire; "
        "end Beeper; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Beeper(); "
        "    b.Hook(); "
        "    b.Fire(0); "
        "    return 0; "
        "  end Main; "
        "end C;");
    const auto* d = findDiag(r, kNotConvertibleNeedle);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (!d) return;
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}

// ---------------------------------------------------------------------------
// ACCEPT 1/5 — an UNANNOTATED lambda parameter still infers from the target
// (§27.4): the carve-out this fix must NOT remove.
// ---------------------------------------------------------------------------
BEZEL_TEST(fn_type_param_slot_check_4897,
           lambda_unannotated_infers_from_target_accepted_spec_27_4) {
    auto r = runAot(
        "module M; import System; "
        "class C "
        "  public static method Take(f: (int) -> ()) f(41); end Take; "
        "  public static method Main(): int "
        "    Take(lambda(n) => Console.WriteLine(n + 1)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
}

// ---------------------------------------------------------------------------
// ACCEPT 2/5 — a MATCHING explicit annotation is admitted.
// ---------------------------------------------------------------------------
BEZEL_TEST(fn_type_param_slot_check_4897,
           lambda_matching_annotation_accepted_spec_27_1_1) {
    auto r = runAot(
        "module M; import System; "
        "class C "
        "  public static method Take(f: (int) -> ()) f(41); end Take; "
        "  public static method Main(): int "
        "    Take(lambda(n: int) => Console.WriteLine(n + 1)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
}

// ---------------------------------------------------------------------------
// ACCEPT 3/5 — §27.1.1 first bullet: a class/interface parameter slot is
// CONTRAVARIANT — a lambda annotated with a BASE class of the target's
// declared parameter type is admitted (`(Animal) -> ()` usable where
// `(Dog) -> ()` is expected).
// ---------------------------------------------------------------------------
BEZEL_TEST(fn_type_param_slot_check_4897,
           lambda_class_param_contravariance_base_class_admitted_spec_27_1_1) {
    auto r = runAot(
        "module M; import System; "
        "class Animal public V: int := 5; end Animal; "
        "class Dog: Animal end Dog; "
        "class C "
        "  public static method Take(f: (Dog) -> ()) "
        "    using var d := new Dog(); "
        "    f(d); "
        "  end Take; "
        "  public static method Main(): int "
        "    Take(lambda(a: Animal) => Console.WriteLine(a.V + 1)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("6\n"));
}

// ---------------------------------------------------------------------------
// ACCEPT 4/5 (bonus — exercises the new synthesis helper on the accept
// side) — a bare STATIC method-group reference whose parameter type MATCHES
// the target is still admitted and runs.
// ---------------------------------------------------------------------------
BEZEL_TEST(fn_type_param_slot_check_4897,
           method_group_static_matching_param_accepted_spec_27_1_1) {
    auto r = runAot(
        "module M; import System; "
        "class C "
        "  public static method OnI(n: int) Console.WriteLine(n + 1); "
        "  end OnI; "
        "  public static method Main(): int "
        "    var f: (int) -> () := OnI; "
        "    f(41); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
}

// ---------------------------------------------------------------------------
// ACCEPT 5/5 (bonus) — a bare method-group EVENT HANDLER whose parameter
// type MATCHES the payload is still admitted and runs.
// ---------------------------------------------------------------------------
BEZEL_TEST(fn_type_param_slot_check_4897,
           method_group_event_plus_eq_matching_param_accepted_spec_17_2) {
    auto r = runAot(
        "module M; import System; "
        "class Beeper "
        "  private owned _beep: EventSource<int>; "
        "  public constructor() _beep := new EventSource<int>(); "
        "  end constructor; "
        "  public method Hook() _beep += OnI; end Hook; "
        "  public static method OnI(n: int) Console.WriteLine(n + 1); "
        "  end OnI; "
        "  public method Fire(n: int) _beep.Raise(n); end Fire; "
        "end Beeper; "
        "class C "
        "  public static method Main(): int "
        "    using var b := new Beeper(); "
        "    b.Hook(); "
        "    b.Fire(41); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("42\n"));
}

// ---------------------------------------------------------------------------
// fix-forward (non-author review of #4897, §27.1.1 v0.307 / §14.5): the
// per-parameter loop above skipped a target slot using
// `refMentionsTypeVariable`, which keys on the CALLER's own enclosing
// type/method generic-parameter names (`currentScopeGenericParamNames()`),
// not the CALLEE's still-unsubstituted method type parameter. A non-generic
// caller — or a generic caller whose own parameter name merely differs from
// the callee's — has an empty/mismatched caller-scope set, so the callee's
// free `T` was never recognised as "still a type variable" and a genuinely
// well-typed annotated-lambda call argument was wrongly rejected; it was
// accepted only when the caller's own generic parameter happened to share
// the callee's spelling by coincidence. Fixed by keying the skip on
// `freeTypeParamNames` (the callee's own free parameters for THIS call,
// threaded in by the call-argument check site), the same authority the
// `toIsCalleeFreeParam` arm elsewhere in `checkAssignable` already uses for
// the identical class of name-collision defect (#4915).
//
// ACCEPT 1/4 — a NON-GENERIC caller: `Util.Apply<T>(f: (T) -> T, x: T)`
// called from `Main()` (no enclosing generic scope at all) with an
// EXPLICITLY ANNOTATED lambda. Pre-fix: rejected (`currentScopeGenericParamNames()`
// is empty, so "T" was never recognised as a type variable and the
// annotation `int` was compared against the bare unsubstituted `T`).
// ---------------------------------------------------------------------------
BEZEL_TEST(fn_type_param_slot_check_4897,
           lambda_annotated_arg_generic_callee_nongeneric_caller_accepted_spec_14_5) {
    auto r = runAot(
        "module M; import System; "
        "static class Util "
        "  public static method Apply<T>(f: (T) -> T, x: T): T "
        "    return f(x); "
        "  end Apply; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(Util.Apply(lambda(n: int) => n + 1, 3)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n"));
}

// ---------------------------------------------------------------------------
// ACCEPT 2/4 — a GENERIC caller whose OWN type parameter is spelled
// DIFFERENTLY from the callee's (`Go<Q>` calling `Apply<T>`). Pre-fix:
// rejected — the caller scope is `{"Q"}`, and "T" ∉ {"Q"}, so the skip never
// fired; accepted before this test existed only by the accidental
// `Go<T>`/`Apply<T>` name coincidence
// (`callee_type_param_name_collision_4915_test.cpp`'s
// `literal_beside_lambda_to_same_named_callee_param_accepted_spec_14_5`).
// ---------------------------------------------------------------------------
BEZEL_TEST(fn_type_param_slot_check_4897,
           lambda_annotated_arg_generic_callee_differently_named_caller_accepted_spec_14_5) {
    auto r = runAot(
        "module M; import System; "
        "static class Util "
        "  public static method Apply<T>(f: (T) -> T, x: T): T "
        "    return f(x); "
        "  end Apply; "
        "end Util; "
        "class C "
        "  public static method Go<Q>(v: Q): int "
        "    Console.WriteLine(Util.Apply(lambda(n: int) => n + 1, 3)); "
        "    return 0; "
        "  end Go; "
        "  public static method Main(): int "
        "    return Go<int>(0); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n"));
}

// ---------------------------------------------------------------------------
// ACCEPT 3/4 — TWO callee type parameters (`Map<T, U>(f: (T) -> U, x: T): U`)
// where the annotated lambda's parameter fixes `T` and its (unannotated)
// return type fixes `U` to a DIFFERENT concrete type (`bool`) than `T`
// (`int`) — proving the structural free-type-param walk
// (`refOutParamTypeMentionsTypeParam`) is not accidentally keying on a
// single name or position.
// ---------------------------------------------------------------------------
BEZEL_TEST(fn_type_param_slot_check_4897,
           lambda_annotated_arg_two_callee_type_params_accepted_spec_14_5) {
    auto r = runAot(
        "module M; import System; "
        "static class Util "
        "  public static method Map<T, U>(f: (T) -> U, x: T): U "
        "    return f(x); "
        "  end Map; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(Util.Map(lambda(n: int) => n > 0, 3)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("True\n"));
}

// ---------------------------------------------------------------------------
// REJECT — the skip must NOT swallow a genuine mismatch once the callee's
// type parameter IS substituted: an EXPLICIT type argument
// (`Util.Apply<long>(...)`) binds `T = long` before this call's argument
// check runs (`explicitSubst`, ExprTypecheck_Calls.cpp), so `f`'s target
// slot is the CONCRETE `(long) -> long`, not a free variable, and the
// lambda's `int` annotation is a genuine §27.1.1 parameter-slot mismatch.
// ---------------------------------------------------------------------------
BEZEL_TEST(fn_type_param_slot_check_4897,
           lambda_annotated_arg_explicit_type_arg_mismatch_rejected_spec_27_1_1) {
    auto r = runAot(
        "module M; import System; "
        "static class Util "
        "  public static method Apply<T>(f: (T) -> T, x: T): T "
        "    return f(x); "
        "  end Apply; "
        "end Util; "
        "class C "
        "  public static method Main(): int "
        "    Console.WriteLine(Util.Apply<long>(lambda(n: int) => n, 3L)); "
        "    return 0; "
        "  end Main; "
        "end C;");
    const auto* d = findDiag(r, kLambdaParamNeedle);
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (!d) return;
    BEZEL_EXPECT_TRUE(bezel::test::failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(r.ran);
}
