// Spec-derived regression test for issue #2604.
//
// §7 (Nullability) / stdlib §6.6 (`TaskCompletionSource<T>` and
// `OwningTaskCompletionSource<T>`), v0.236: stdlib §6.6 declares
//
//     public method SetResult(result: T);
//     public method TrySetResult(result: T): bool;
//
// on `TaskCompletionSource<T>` with a NON-nullable `result: T`. The builtin has
// no MethodSymbol — the CallExpr handler types the family synthetically at
// ExprTypecheck_Calls.cpp:~7288, setting only the RESULT type (bool for
// TrySet*, unit for Set*) and running no per-argument check — so the ordinary
// checkCallArity -> checkArgsAgainstParams -> checkAssignable path never fired
// and an un-narrowed `T?` argument was SILENTLY ACCEPTED. This is the same §7
// hole as #2582 (heap collections), #2600 (span buffers) and #2602 (the atomic
// family), on a family none of those gates' head-name lists covered.
//
// Unlike its siblings this one does NOT ICE codegen — the SetResult lowering
// tolerates the nullable `{ i8, i32 }` representation — so the only symptom is
// the silent §7 soundness accept: an un-narrowed nullable stored as the task's
// non-nullable `T` result. A user method `UserSetResult(result: int)` with the
// identical signature correctly rejected the same argument, so the two paths
// disagreed.
//
// Fix: extend #2582's `checkBuiltinCollectionCallArgs` with an `isTaskSource`
// branch, mirroring how #2600 added `isSpanBuffer` and #2602 added `isAtomic`.
// `OwningTaskCompletionSource<T>` is deliberately EXCLUDED — stdlib §6.6
// declares its twin `SetResult(owned result: T)` / `TrySetResult(owned result:
// T)`, and a §22.3 `owned` transfer is not a §7 nullable->non-nullable
// conversion (the same exclusion the owning collections' `Add(owned item: T)`
// carries). `SetException`/`TrySetException` take `owned exception: Exception`
// on every form; `SetCancelled`/`TrySetCancelled`/`Dispose` take no arguments.
//
// This is a REJECTION fix: each negative case asserts on diagnostic TEXT (not
// the coarse rejects() helper — runAot's WIFSIGNALED path synthesizes an Error
// diagnostic for a runtime SIGSEGV too, so it cannot distinguish a genuine
// rejection from a crash). The positive cases guard against false rejection of
// working code.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include <initializer_list>
#include <string>

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic contains `sub`.
bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// The §12/§7 "nullable -> non-nullable" assignability diagnostic fragment
// shared by the user-method path and (after #2315/#2582/#2600/#2602/#2604) the
// builtin paths.
constexpr const char* kNullableDiag =
    "is nullable but the target type is non-nullable";

}  // namespace

// #2604 / §7 / stdlib §6.6: the issue's exact repro — `tcs.SetResult(N())`
// with an un-narrowed `int?` on a `TaskCompletionSource<int>`. Without the fix
// sema emitted NO diagnostic at all (the silent accept).
BEZEL_TEST(sema_tcs_nullable_arg_2604,
           tcs_setresult_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(tcs: TaskCompletionSource<int>): int "
        "    tcs.SetResult(N()); "  // N(): int? — un-narrowed
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "TaskCompletionSource<T>.SetResult"));
    BEZEL_EXPECT_TRUE(hasError(r, "(§7)"));
}

// #2604 / stdlib §6.6: the `TrySetResult(result: T): bool` twin declares the
// same non-nullable `result: T` and must reject identically.
BEZEL_TEST(sema_tcs_nullable_arg_2604,
           tcs_trysetresult_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(tcs: TaskCompletionSource<int>): int "
        "    var ok := tcs.TrySetResult(N()); "  // un-narrowed int?
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "TaskCompletionSource<T>.TrySetResult"));
}

// #2604 / stdlib §6.6: a non-int result type also rejects — confirms the check
// keys on the instantiated `recvTy->genericArgs[0]`, not a hardcoded `int`.
// §6.6 admits `TaskCompletionSource<string>` (string is a non-class T:
// "for non-class T (copyable value types, string, runtime-managed reference
// types)").
BEZEL_TEST(sema_tcs_nullable_arg_2604,
           tcs_string_setresult_rejects_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method NS(): string? => null; "
        "  public static method T(tcs: TaskCompletionSource<string>): int "
        "    tcs.SetResult(NS()); "  // un-narrowed string?
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "TaskCompletionSource<T>.SetResult"));
}

// ---- Positive controls (guard against false rejection of working code) ----

// #2604 / stdlib §6.6: a plain non-nullable argument to BOTH methods compiles
// unchanged. (`tcs_974`'s codegen suite proves the same shapes still RUN —
// `TrySetResult(99)` + `await tcs.Task` returns 99.)
BEZEL_TEST(sema_tcs_nullable_arg_2604,
           tcs_accepts_plain_nonnullable_result) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(tcs: TaskCompletionSource<int>): int "
        "    tcs.SetResult(5); "
        "    var ok := tcs.TrySetResult(7); "
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2604 / §7.4: a `T?` PROVED non-null by a preceding null-check IS assignable
// to the non-nullable `result: T` and must NOT be false-rejected. The §7.4
// narrowing exception lives inside checkAssignable and is reused unchanged.
// (SEMA-level only, deliberately: the narrowed form compiles through codegen
// but a separate, PRE-EXISTING codegen defect delivers the tagged-optional's
// tag byte instead of the value to the awaiter — the #2605 shape on this
// family — filed independently. That defect is orthogonal to this §7 fix,
// which correctly ACCEPTS the narrowed case per §7.4; asserting a runtime
// value here would pin the miscompile rather than the rule.)
BEZEL_TEST(sema_tcs_nullable_arg_2604,
           tcs_accepts_narrowed_nullable_result) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(tcs: TaskCompletionSource<int>): int "
        "    var n: int? := 55; "
        "    if n = null then return 1; end; "  // narrows n non-null below
        "    var ok := tcs.TrySetResult(n); "
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2604 / §7: the parameter's OWN nullability is the INSTANTIATED type's —
// `TaskCompletionSource<int?>` has `SetResult(result: int?)`, so an un-narrowed
// `int?` is ADMISSIBLE there and must not be rejected. This is the case a
// hardcoded "reject any nullable argument" implementation would break.
BEZEL_TEST(sema_tcs_nullable_arg_2604,
           tcs_of_nullable_t_accepts_unnarrowed_nullable) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(tcs: TaskCompletionSource<int?>): int "
        "    tcs.SetResult(N()); "  // param IS int? — admissible
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2604 / stdlib §6.6 / §22.3: `OwningTaskCompletionSource<T>` is EXCLUDED —
// its `SetResult(owned result: T)` transfers under §22.3, not §7. The owning
// form must keep compiling exactly as before (the gate is head-name keyed, so
// this also proves the new branch cannot leak onto the owning twin).
BEZEL_TEST(sema_tcs_nullable_arg_2604,
           owning_tcs_owned_result_not_gated) {
    auto r = parseSemaAll(
        "module M; "
        "class Res "
        "  public v: int := 0; "
        "  public constructor(x: int) v := x; end constructor; "
        "end Res; "
        "class C "
        "  public static method T(tcs: OwningTaskCompletionSource<Res>): int "
        "    var ok := tcs.TrySetResult(new Res(7)); "
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2604 / stdlib §6.6: the non-argument and `owned exception` members of the
// family are untouched — `SetException(owned exception: Exception)`,
// `SetCancelled()`, `TrySetCancelled()`. The non-generic
// `TaskCompletionSource` (void completion) has no genericArgs and is filtered
// by the gate's emptiness guard; its `TrySetResult()` takes no argument.
BEZEL_TEST(sema_tcs_nullable_arg_2604,
           tcs_non_result_members_and_nongeneric_unaffected) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method T(tcs: TaskCompletionSource<int>, "
        "                         plain: TaskCompletionSource): int "
        "    tcs.SetException(new Exception(\"x\")); "
        "    var c1 := tcs.TrySetCancelled(); "
        "    var c2 := plain.TrySetResult(); "
        "    plain.SetCancelled(); "
        "    return 0; "
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, kNullableDiag));
}

// #2604 cross-domain guard: the new isTaskSource branch is head-name keyed, so
// it cannot perturb the isElementCollection / isKeyedCollection / isSpanBuffer
// / isAtomic branches sharing this helper. Re-run a representative #2602 case
// (the atomic family) to confirm it still rejects after the branch was added.
BEZEL_TEST(sema_tcs_nullable_arg_2604,
           atomic_branch_unaffected_by_tasksource_branch) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  private static method N(): int? => null; "
        "  public static method T(a: Atomic<int>): int "
        "    return a.Exchange(N()); "  // #2602 case — must still reject
        "  end T; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, kNullableDiag));
    BEZEL_EXPECT_TRUE(hasError(r, "Atomic<T>.Exchange"));
}
