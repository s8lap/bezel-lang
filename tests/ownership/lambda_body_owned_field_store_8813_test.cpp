// Ownership — issue #8813: inside a LAMBDA body, an owned local stored
// UNMARKED into an `owned` field is double-freed, and an `owned` lambda
// PARAMETER stored into an `owned` field is lost (the closure call that
// carries the value never runs).
//
// SPEC (v0.359).
//   §28.8.1 *The replacing `move`*, verbatim: "The replacement `e` is an owned
//     value for the location's declared type: a fresh producer, a `move`
//     result, a named owned binding — the position is marker-free, as every
//     store into an owned location is (K3.1: the binding is left moved-from,
//     and the freeze applies)".
//     So a store into an `owned` location is a MARKER-FREE position, and the
//     named owned binding it reads is LEFT MOVED-FROM (K3.1's consume-table
//     row for "store into an `owned` field").
//   §28.3 K3.1 *Consume table*: the post-state for "store into an `owned`
//     field" is **moved-from** — "a later read is an error until reassigned".
//   §28.3 K7.4 / *Auto-drop (affine)*: "a transferred value [is left] in
//     moved-from state so the scope-end disposal skips it" — the scope-end
//     disposal is PER-PATH (K1.4): the transferred path skips it, the
//     not-transferred path still performs it, exactly once.
//   §28.5 / §28.8: an `owned` field is an owned storage location; it may only
//     receive an OWNED value, and the field's owner disposes it.
//   §28.3 axiom A5: a `using var` is scope-bound and "cannot be moved out
//     of" — so it may not be the source of such a store (the method-body
//     twin says so verbatim: "cannot move out of `using var` `ys`; `using
//     var` is scope-bound (§28.3)").
//   §15.4: a collection literal has no standalone type — it is CONTEXT-TYPED
//     by the position it is written in, so a call argument carrying one must
//     be materialized at the FORMAL's type.
//
// THE HOLE — two independent defects, one issue.
//
//   (1) THE OWNED-FIELD STORE CONSUME never reached a lambda body. The
//       ownership pass's dataflow never enters a lambda body (a lambda body
//       lives inside a `LambdaExpr`, an expression), so the method-level
//       `handleAssignment` arm `targetIsOwnedField` →
//       `handleOwnedFieldAssignment` → `markMovedByName` never ran for a
//       store written inside a closure: the local stayed Owned, the §28.3
//       affine auto-drop kept flagging it for the scope-end disposal, and
//       the lambda frame's scope-end free ran ALONGSIDE the field's own
//       disposal.
//
//       MEASURED ON THE BASE (r30-c1 @ 02d621783, `--sanitize=address`):
//       "ERROR: AddressSanitizer: attempting double-free", the first free
//       the lambda frame's scope-end `bz_array_free`, the second
//       `C.DisposeCore` — the field's owner disposing the same array. The
//       byte-identical METHOD-body twin is clean 5/5.
//
//   (2) THE CLOSURE CALL never materialized a COLLECTION-LITERAL argument.
//       `lowerExpr` returns null for a `CollectionLiteralExpr` (§15.4: it
//       names no target type of its own), and the function-VALUE call path
//       (`Emitter::emitClosureCall`) builds its own argument list instead of
//       routing through `buildOrderedArgs` — so it never reached the
//       materializer. The null value fell through the unit-rescue and the
//       `if (!v) return nullptr;` guard, and the WHOLE CALL was abandoned:
//       the callee never ran.
//
//       MEASURED ON THE BASE: `f([1, 2])` — "Unhandled exception:
//       System.NullReferenceException" (exit 70) for the
//       `Console.WriteLine(h.saved!.Count)` twin, and a printed `0` instead
//       of `3` with "internal: return-value expression failed to lower to a
//       value … #2491" for the `return f([1, 2, 3]);` twin. The
//       byte-identical METHOD-call twin (`C.Take([1, 2, 3])`) works.
//
// THE ORACLE. Every row below is RED on the base and GREEN here, for one of
// the two reasons above: an ASan report, a wrong VALUE, or a missing
// REJECTION that the method-body twin already produced.
//
// per §28.8.1, §28.3 K3.1 / K7.4 / A5, §28.5, §28.8, §15.4, v0.359, #8813

#include <string>

#include "support/aot_support.hpp"      // runAot — the AOT oracle
#include "support/ownership_support.hpp"  // parseAndCheckOwnership
#include "support/run_under_asan.hpp"   // runUnderAsan — the ASan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::OwnershipResult;
using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;

namespace {

// The issue's own repro (case 1): an owned local declared in the LAMBDA
// body's own frame, stored UNMARKED into an `owned` field of the enclosing
// class. §28.8.1 makes the store a marker-free position whose source is
// left moved-from; on the base the local stayed Owned and the lambda
// frame's scope-end free ran beside the field's disposal.
constexpr const char* kLambdaOwnedLocalIntoOwnedField =
    "module M; "
    "class C "
    "  private owned saved: Array<int>?; "
    "  public method Go(): int "
    "    var f := lambda(): int => do "
    "      var ys := new Array<int>(); ys.Add(5); "
    "      this.saved := ys; "
    "      return this.saved!.Count; "
    "    end; "
    "    return f(); "
    "  end Go; "
    "  public static method Main(): int "
    "    using var c := new C(); "
    "    return c.Go(); "
    "  end Main; "
    "end C; ";

// The issue's own repro (case 3): an `owned` LAMBDA PARAMETER stored into an
// `owned` field, reached through a CLOSURE CALL whose argument is a
// collection literal. The literal must be materialized at the formal's type
// (§15.4) so the call RUNS at all.
constexpr const char* kClosureCallOwnedParamIntoOwnedField =
    "module M; "
    "class C "
    "  private owned saved: Array<int>?; "
    "  public method Go(): int "
    "    var f := lambda(owned l: Array<int>): int => do "
    "      this.saved := l; return this.saved!.Count; "
    "    end; "
    "    return f([1, 2, 3]); "
    "  end Go; "
    "  public static method Main(): int "
    "    using var c := new C(); "
    "    return c.Go(); "
    "  end Main; "
    "end C; ";

// The issue's own repro (case 2): the same closure call, but the store
// reaches ANOTHER object's `owned` field.
constexpr const char* kClosureCallOwnedParamIntoOtherOwnedField =
    "module M; "
    "class Holder "
    "  public owned saved: Array<int>?; "
    "end Holder; "
    "class C "
    "  public static method Main(): int "
    "    using var h := new Holder(); "
    "    var f := lambda(owned l: Array<int>) => do h.saved := l; end; "
    "    f([1, 2]); "
    "    return h.saved!.Count; "
    "  end Main; "
    "end C; ";

// A `using var` source: §28.3 axiom A5 makes it scope-bound, so it may not
// be moved out into an `owned` field. The METHOD-body twin says so verbatim
// ("cannot move out of `using var` `ys`; `using var` is scope-bound
// (§28.3)"); on the base the lambda-body spelling compiled clean and
// double-freed.
constexpr const char* kLambdaUsingVarIntoOwnedField =
    "module M; "
    "class C "
    "  private owned saved: Array<int>?; "
    "  public method Go(): int "
    "    var f := lambda(): int => do "
    "      using var ys := new Array<int>(); ys.Add(5); "
    "      this.saved := ys; "
    "      return 0; "
    "    end; "
    "    return f(); "
    "  end Go; "
    "  public static method Main(): int "
    "    using var c := new C(); "
    "    Console.WriteLine(c.Go()); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// The SAME local stored into TWO `owned` fields: §28.8.1 / K3.1 leave the
// binding moved-from after the first store, so the second store is the
// method-body twin's "already moved out" error.
constexpr const char* kLambdaDoubleStoreIntoOwnedFields =
    "module M; "
    "class C "
    "  private owned saved: Array<int>?; "
    "  private owned saved2: Array<int>?; "
    "  public method Go(): int "
    "    var f := lambda(): int => do "
    "      var ys := new Array<int>(); ys.Add(5); "
    "      this.saved := ys; "
    "      this.saved2 := ys; "
    "      return 0; "
    "    end; "
    "    return f(); "
    "  end Go; "
    "  public static method Main(): int "
    "    using var c := new C(); "
    "    Console.WriteLine(c.Go()); "
    "    return 0; "
    "  end Main; "
    "end C; ";

// A CONDITIONAL store: §28.3 K7.4 makes the scope-end disposal PER-PATH —
// the stored path skips it (the field owns the value), the not-stored path
// still disposes it, exactly once.
constexpr const char* kLambdaConditionalStoreIntoOwnedField =
    "module M; "
    "class C "
    "  private owned saved: Array<int>?; "
    "  public method Go(cond: bool): int "
    "    var f := lambda(): int => do "
    "      var ys := new Array<int>(); ys.Add(5); "
    "      if cond then this.saved := ys; end; "
    "      return 0; "
    "    end; "
    "    return f(); "
    "  end Go; "
    "  public static method Main(): int "
    "    using var c := new C(); "
    "    return c.Go(true); "
    "  end Main; "
    "end C; ";

}  // namespace

// ===========================================================================
// (1) THE DOUBLE FREE — §28.8.1 / §28.3 K3.1 / K7.4 (v0.359).
// ===========================================================================

// The issue's own repro, under the sanitizer. WITHOUT the fix the lambda
// frame's scope-end `bz_array_free` and the field's `DisposeCore` free the
// same array: "attempting double-free" — the sanitizer's exit code, 99,
// where the program's own value is 1.
BEZEL_TEST(lambda_body_owned_field_store_8813,
            asan_lambda_owned_local_into_owned_field_no_double_free_28_8_1) {
    int rc = bezel::test::runUnderAsanExitCode(
        std::string(kLambdaOwnedLocalIntoOwnedField),
        "bezel_8813_asan_owned_local", /*detectLeaks=*/true);
    if (rc == -1) return;  // sanitizer toolchain unavailable: SKIP
    BEZEL_EXPECT_EQ(rc, 1);
}

// The same program at the VALUE level: the field must hold the array the
// lambda stored, and the program must run to completion. On the base the
// program's OWN double free aborted it (the allocator's own abort, no
// sanitizer involved), so it never produced its value at all.
BEZEL_TEST(lambda_body_owned_field_store_8813,
            aot_lambda_owned_local_into_owned_field_runs_28_8_1) {
    JitResult jr = runAot(std::string(kLambdaOwnedLocalIntoOwnedField));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 1);
}

// The CONDITIONAL twin — §28.3 K7.4's per-path scope-end disposal. The
// stored path must skip the scope-end free (the field owns the value) and
// the not-stored path must still perform it, exactly once. On the base both
// paths freed the same array.
BEZEL_TEST(lambda_body_owned_field_store_8813,
            asan_lambda_conditional_store_into_owned_field_no_double_free_28_3_k7_4) {
    int rc = bezel::test::runUnderAsanExitCode(
        std::string(kLambdaConditionalStoreIntoOwnedField),
        "bezel_8813_asan_conditional_store", /*detectLeaks=*/true);
    if (rc == -1) return;  // sanitizer toolchain unavailable: SKIP
    BEZEL_EXPECT_EQ(rc, 0);
}

// ===========================================================================
// (2) THE LOST VALUE — §15.4 (v0.359): the closure call must RUN.
// ===========================================================================

// The issue's own case 3: the closure call carries a collection literal, so
// the literal must be materialized at the formal's type (§15.4) and the
// callee must run. On the base the call was abandoned: the program printed
// `0` (with the internal #2491 note) instead of `3`.
BEZEL_TEST(lambda_body_owned_field_store_8813,
            aot_closure_call_owned_param_into_owned_field_returns_3_15_4) {
    JitResult jr =
        runAot(std::string(kClosureCallOwnedParamIntoOwnedField));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 3);
}

// The issue's own case 2: the same call, storing into ANOTHER object's
// `owned` field. On the base the callee never ran, the field stayed null,
// and the program died with "Unhandled exception:
// System.NullReferenceException" (exit 70).
BEZEL_TEST(lambda_body_owned_field_store_8813,
            aot_closure_call_owned_param_into_other_owned_field_returns_2_15_4) {
    JitResult jr =
        runAot(std::string(kClosureCallOwnedParamIntoOtherOwnedField));
    BEZEL_EXPECT_TRUE(jr.ran);
    BEZEL_EXPECT_EQ(jr.exitCode, 2);
}

// The same shape under the sanitizer: the materialized literal is handed to
// an `owned` formal, so the CALLER must not dispose it (the callee's field
// owns it) and the field's owner must dispose it exactly once. The row
// asserts the program's OWN exit value (3) as well as the sanitizer's
// silence: on the base the call was abandoned, the program degraded to a
// printed `0`, and this row was GREEN even though nothing ran -- pinning the
// value makes it RED there.
BEZEL_TEST(lambda_body_owned_field_store_8813,
            asan_closure_call_owned_param_into_owned_field_no_leak_15_4) {
    int rc = bezel::test::runUnderAsanExitCode(
        std::string(kClosureCallOwnedParamIntoOwnedField),
        "bezel_8813_asan_closure_call", /*detectLeaks=*/true);
    if (rc == -1) return;  // sanitizer toolchain unavailable: SKIP
    BEZEL_EXPECT_EQ(rc, 3);
}

// ===========================================================================
// (3) THE BINDING IS LEFT MOVED-FROM — §28.3 K3.1 (v0.359).
// ===========================================================================

// A `using var` source may not be moved out (§28.3 axiom A5): the
// method-body twin says "cannot move out of `using var` `ys`; `using var` is
// scope-bound (§28.3)", and the lambda-body twin must say the same. On the
// base the lambda-body spelling compiled CLEAN and double-freed.
BEZEL_TEST(lambda_body_owned_field_store_8813,
            lambda_using_var_into_owned_field_is_rejected_28_3) {
    OwnershipResult r =
        parseAndCheckOwnership(std::string(kLambdaUsingVarIntoOwnedField));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "cannot move out of `using var` `ys`; `using var` is scope-bound"));
}

// The same local stored into TWO `owned` fields: the first store leaves the
// binding moved-from (§28.3 K3.1), so the second is the method-body twin's
// "already moved out" error. On the base both stores compiled clean and
// two fields claimed the same array.
BEZEL_TEST(lambda_body_owned_field_store_8813,
            lambda_double_store_into_owned_fields_is_rejected_28_3_k3_1) {
    OwnershipResult r =
        parseAndCheckOwnership(std::string(kLambdaDoubleStoreIntoOwnedFields));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(
        r, "owned value in `ys` was already moved out at this point"));
}
