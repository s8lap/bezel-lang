// fix #1955 + #1956 — §28.3 "Exception-path discharge", spec v0.197. Two
// UnwindIneligibleWalker over-approximations that leak a plain `owned` local
// live at a throw. Same family as #1918 (which closed only the user-method
// half); both fixed here in the one walker + its paired arg-nulling sites.
// Codegen-observable, LeakSanitizer/AddressSanitizer-checked (runAotChecked).
//
// The defects (both a FLOW/PATH-insensitive taint that dropped the unwindOnly
// disposal for a throw edge that PRECEDES a discharge, leaking on that edge):
//
//   #1955 — an owned local transferred via `owned x` to a NON-user-method
//     owned-consuming callee: a builtin owning-collection `Add`/`Insert`/`Push`
//     (lowers to bz_array_add / bz_array_insert_at), or a CONSTRUCTOR. The
//     walker only left a transferred owned local eligible for `calleeIsUserMethod`
//     (#1918); a constructor / owning-collection Add was left INELIGIBLE, so a
//     throw BEFORE the transfer leaked the still-live local.
//
//   #1956 — an owned local REASSIGNED later in the body: the AssignmentStmt
//     handler tainted the bare-identifier target FLOW-INSENSITIVELY, so a throw
//     BEFORE the reassignment (where the ORIGINAL value is still live) had no
//     unwind disposal and leaked.
//
// The fixes (all keep the exactly-once §28.3 invariant, ASan-guarded here):
//   * UnwindIneligibleWalker (LlvmBackend_Setup.cpp):
//       - calleeIsOwningCollectionTransfer: an `OwningArray.Add(owned x)` / Push
//         / Enqueue / Insert leaves its transferred bare owned local eligible;
//       - the NewExpr handler leaves an explicit `owned x` constructor argument
//         eligible;
//       - the AssignmentStmt handler no longer taints a bare-identifier
//         reassignment target.
//   * The paired arg-nulling that keeps the null-guarded unwindOnly disposal
//     exactly-once across the transfer:
//       - lowerNew (LlvmBackend_Exprs2.cpp) now passes buildOrderedArgs
//         `nullTransferredOwned=true` — nulls the caller's slot for a ctor
//         `owned x` transfer;
//       - the owning-collection Add/Insert lowering (LlvmBackend_Calls.cpp) nulls
//         the caller's slot after the element is moved into the collection
//         (nullTransferredOwnedLocalSlot);
//       - the reassignment lowering already disposes the displaced OLD value and
//         stores the new (disposeDisplacedOwnedClassLocal), so the slot always
//         holds exactly the currently-live value at any throw edge.
//
// Observation model (as in unwind_branch_owned_arg_transfer_1918_test): class
// Resource bumps a static net-live counter (ctor +1, dtor -1). Disposed exactly
// once => net 0; a leak => +1; a double-free => -1 AND an ASan trip under the
// runAotChecked oracle. Every leg rides runAotChecked (AOT + ASan/LSan), so
// r.ran==false flags any leak/double-free/UAF and the destructor-count exit code
// is asserted together.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// Resource: static net-live counter. Disposed exactly once => net 0; leaked =>
// +1; double-disposed => -1 (and the second reclaim trips ASan under the check).
const char* kResource =
    "class Resource "
    "  public static _live: int := 0; "
    "  public constructor() Resource._live := Resource._live + 1; end constructor; "
    "  destructor Resource._live := Resource._live - 1; end destructor; "
    "end Resource; ";

// Box owns a Resource via an `owned` field (§28.4 auto-disposes the field on the
// box's disposal) — the constructor-transfer recipient for #1955.
const char* kBox =
    "class Box "
    "  public owned _r: Resource; "
    "  public constructor(owned r: Resource) _r := r; end constructor; "
    "  destructor end destructor; "
    "end Box; ";

}  // namespace

// ===========================================================================
// #1955 CORE — collection transfer (§28.3). `b` is transferred via `owned b`
// into an OwningArray (a builtin `Add`, bz_array_add — not a user method) AFTER
// a throw that leaves `b` live. It must be unwind-disposed exactly once.
// FAIL-WITHOUT: b leaks in bz$Program$Work (r.ran==false / value 11).
// PASS-WITH: exit == 10 (caught == 1 AND Resource._live == 0).
// ===========================================================================
BEZEL_TEST(unwind_owned_transfer_1955_1956, collection_add_owned_transfer_after_throw_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    using var log := new OwningArray<Resource>(); "
        "    var b := new Resource(); "        // plain owned local
        "    Boom(); "                          // throws BEFORE transfer — b live (§28.3)
        "    log.Add(move b); "                // normal path: b moved into log
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);          // ASan/LSan-clean (no leak, no double-free)
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1955 DOUBLE-FREE GUARD — collection transfer WINDOW. `b` is moved into `log`
// and THEN a throw occurs. On the unwind edge `log` (a `using var`) disposes its
// element `b`; the caller's own unwindOnly disposal for `b` must SKIP it (the
// slot was nulled at the transfer) — otherwise a double-free.
// FAIL-WITHOUT the nulling: ASan trips (r.ran==false) or value 9.
// PASS-WITH: exit == 10 (caught == 1 AND Resource._live == 0).
// ===========================================================================
BEZEL_TEST(unwind_owned_transfer_1955_1956, collection_add_transfer_then_throw_no_double_free_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    using var log := new OwningArray<Resource>(); "
        "    var b := new Resource(); "
        "    log.Add(move b); "                // b moved into log; slot nulled
        "    Boom(); "                          // throw AFTER transfer — log owns b
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);          // no double-free on the transfer-window edge
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1955 CONTROL — collection transfer NORMAL path (no throw). `b` is moved into
// `log`, `log` disposes it once at scope end. The new unwind-eligibility must
// NOT cause a second disposal (unwindOnly is isUnwind-gated + slot nulled).
// PASS-WITH and WITHOUT: exit == 0. Guards a normal-path double-free regression.
// ===========================================================================
BEZEL_TEST(unwind_owned_transfer_1955_1956, collection_add_normal_path_not_double_freed_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Work() "
        "    using var log := new OwningArray<Resource>(); "
        "    var b := new Resource(); "
        "    log.Add(move b); "
        "  end Work; "
        "  public static method Main(): int "
        "    Work(); "
        "    return Resource._live; "           // 0 = disposed exactly once
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

// ===========================================================================
// #1955 CORE — constructor transfer (§28.3). `b` is transferred via `owned b`
// into a CONSTRUCTOR (`new Box(owned b)`) AFTER a throw that leaves `b` live.
// FAIL-WITHOUT: b leaks in bz$Program$Work (r.ran==false / value 11).
// PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(unwind_owned_transfer_1955_1956, constructor_owned_transfer_after_throw_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource + kBox +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    var b := new Resource(); "
        "    Boom(); "                          // throws BEFORE transfer — b live
        "    using var box := new Box(move b); "  // ctor transfer: b moved into box
        "    _ := box; "
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1955 DOUBLE-FREE GUARD — constructor throws AFTER storing the owned field.
// The ctor stores `_r := r` then throws; its cleanup (§28.4) disposes `_r`, and
// the caller's slot for `b` was nulled at the transfer, so the caller's
// unwindOnly disposal must SKIP `b` (no double-free).
// FAIL-WITHOUT the ctor nulling: ASan trips (r.ran==false) or value 9.
// PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(unwind_owned_transfer_1955_1956, constructor_throws_after_field_store_no_double_free_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Box "
        "  public owned _r: Resource; "
        "  public constructor(owned r: Resource) "
        "    _r := r; "
        "    throw new InvalidOperationException(\"in ctor\"); "  // ctor owns r here
        "  end constructor; "
        "  destructor end destructor; "
        "end Box; "
        "class Program "
        "  public static method Work() "
        "    var b := new Resource(); "
        "    using var box := new Box(move b); "  // ctor takes ownership then throws
        "    _ := box; "
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);          // no double-free (caller nulled + ctor cleanup)
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1956 CORE — reassignment (§28.3). `x` owns R#1, a throw occurs while R#1 is
// live, and `x` is REASSIGNED later in the body. The throw edge PRECEDES the
// reassignment, so R#1 must be unwind-disposed exactly once.
// FAIL-WITHOUT: R#1 leaks in bz$Program$Work (r.ran==false / value 11).
// PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(unwind_owned_transfer_1955_1956, reassigned_local_live_at_throw_before_reassign_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    var x := new Resource(); "         // x owns R#1
        "    Boom(); "                          // THROWS — R#1 live, must unwind-dispose
        "    x := new Resource(); "             // reassignment (never reached on throw)
        "    _ := x; "
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1956 DOUBLE-FREE GUARD — throw AFTER the reassignment. R#1 is disposed at the
// reassignment (disposeDisplacedOwnedClassLocal); R#2 is live at the throw and
// must be unwind-disposed. Neither may be double-freed.
// FAIL-WITHOUT: R#2 leaks (value 11). A regression that disposed R#1 twice would
// trip ASan (r.ran==false). PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(unwind_owned_transfer_1955_1956, reassigned_local_throw_after_reassign_no_double_free_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    var x := new Resource(); "         // R#1
        "    x := new Resource(); "             // dispose R#1, store R#2
        "    Boom(); "                          // THROWS — R#2 live
        "    _ := x; "
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1956 EDGE — the reassignment's RHS itself throws. The RHS is evaluated BEFORE
// the displaced-value disposal, so R#1 is still live in the slot on the RHS-throw
// edge and must be unwind-disposed exactly once (the reassignment never
// completed). FAIL-WITHOUT: R#1 leaks (value 11). PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(unwind_owned_transfer_1955_1956, reassignment_rhs_throws_original_disposed_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method MakeOrThrow(): owned Resource "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end MakeOrThrow; "
        "  public static method Work() "
        "    var x := new Resource(); "         // R#1
        "    x := MakeOrThrow(); "              // RHS throws — R#1 still live
        "    _ := x; "
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1956 — an OWNED PARAMETER reassigned. The same shape on the ParameterSymbol
// arm (registerUnwindOwnedParam): a throw before the reassignment leaves the
// original owned-param value live and it must be unwind-disposed exactly once.
// FAIL-WITHOUT: leaks (value 11). PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(unwind_owned_transfer_1955_1956, reassigned_owned_param_live_at_throw_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work(owned p: Resource) "
        "    Boom(); "                          // THROWS — the owned param is live
        "    p := new Resource(); "             // reassignment (never reached)
        "    _ := p; "
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(new Resource()); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1955/#1956 CONTROL — the user-method transfer the #1918 fix already made
// correct must still work (guards against a regression in the shared walker).
// `b` is discharged via `Sink(owned b)` after a throw that leaves it live.
// PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(unwind_owned_transfer_1955_1956, user_method_transfer_still_correct_control_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Sink(owned r: Resource) _ := r; end Sink; "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    var b := new Resource(); "
        "    Boom(); "                          // throws BEFORE transfer — b live
        "    Sink(move b); "
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1955 SOUNDNESS GUARD — a COMPILER-KNOWN BUILTIN owned-consuming constructor
// (Shared<T>, stdlib §2.16 / §28.13) lowers MANUALLY (bz_shared_new), NOT
// through buildOrderedArgs, so it does NOT null the caller's slot. The walker
// must therefore leave `owned x` INTO such a builtin ctor TAINTED — otherwise a
// throw AFTER the transfer double-frees: the Shared's bz_shared_release frees R1
// on unwind, then the caller's unwind disposal reads the freed vtable and frees
// it AGAIN (ASan heap-use-after-free). This is a regression guard for the
// initial #1955 fix, which untainted ALL non-imported ctors and reintroduced the
// double-free; the corrected walker restricts the untaint to USER class/record
// ctors (whose lowerNew path nulls). The transfer-then-throw leg exercises the
// double-free window; the Shared owns R1 and disposes it exactly once.
// FAIL-WITHOUT (the broad-untaint regression): ASan trips (r.ran==false).
// PASS-WITH: exit == 10 (caught == 1 AND Resource._live == 0).
// (A throw BEFORE the transfer leaks R1 — the deliberate "safe residual leak,
// never a double-free" for excluded builtin ctors, a tracked #1955 follow-up —
// so it is intentionally not asserted clean here.)
// ===========================================================================
BEZEL_TEST(unwind_owned_transfer_1955_1956, builtin_shared_ctor_transfer_then_throw_no_double_free_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    var x := new Resource(); "
        "    using var sh := new Shared<Resource>(move x); "  // manual-lowered builtin ctor
        "    Boom(); "                          // throw AFTER transfer — Shared owns x
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);          // no double-free (Shared stays tainted)
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1955 SOUNDNESS GUARD — a USER class with a captured `owned` PRIMARY-ctor
// parameter (`class Box(owned r: Resource)`, `r` referenced in a method → a
// disposed owned FIELD, #1543/#1594) has NO ConstructorSymbol: sema stores the
// primary-ctor params on the TypeDecl, so ctorForArgs returns null and NO
// buildOrderedArgs runs — the owned arg is stored via the positional-field path,
// which does NOT null the caller's slot. The walker must therefore leave `owned
// x` into such a ctor TAINTED — otherwise a throw AFTER the transfer double-
// frees: bz$Box$DisposeCore frees the Resource (its captured owned field) on
// unwind, then the caller's unwind disposal reads the freed vtable and frees it
// AGAIN (ASan heap-use-after-free). Regression guard for the intermediate fix,
// which untainted on TypeDeclKind::Class and reintroduced the double-free; the
// corrected walker (newExprRoutesThroughNullingCtor) untaints ONLY an EXPLICIT-
// ctor class/record whose lowerNew path nulls. The Box owns R and disposes it
// exactly once on the throw-after edge.
// FAIL-WITHOUT (the TypeDeclKind::Class regression): ASan trips (r.ran==false).
// PASS-WITH: exit == 10 (caught == 1 AND Resource._live == 0).
// (A throw BEFORE the transfer leaks R — the deliberate "safe residual leak,
// never a double-free" for excluded ctor shapes, tracked #1970 — not asserted
// clean here.)
// ===========================================================================
BEZEL_TEST(unwind_owned_transfer_1955_1956, primary_ctor_class_transfer_then_throw_no_double_free_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") +
        "class Resource "
        "  public static _live: int := 0; "
        "  public value: int := 0; "
        "  public constructor() Resource._live := Resource._live + 1; end constructor; "
        "  destructor Resource._live := Resource._live - 1; end destructor; "
        "end Resource; "
        "class Box(owned r: Resource) "               // primary-ctor: no ConstructorSymbol
        "  public method Use(): int return r.value; end Use; "  // r → captured owned field
        "end Box; "
        "class Program "
        "  public static method Boom() "
        "    throw new InvalidOperationException(\"boom\"); "
        "  end Boom; "
        "  public static method Work() "
        "    var b := new Resource(); "
        "    using var box := new Box(move b); "      // manual positional-field store (no null)
        "    Boom(); "                          // throw AFTER transfer — Box owns b
        "  end Work; "
        "  public static method Main(): int "
        "    var caught := 0; "
        "    try Work(); "
        "    catch var e: InvalidOperationException => caught := 1; end; "
        "    return caught * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);          // no double-free (primary-ctor class stays tainted)
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// ===========================================================================
// #1955 — OwningStack.Push(owned x), the array-backed owning-collection transfer
// (bz_array_add). A throw BEFORE the Push leaves `b` live → unwind-disposed
// exactly once (FAIL-WITHOUT: leak, value 11). PASS-WITH: exit == 10.
// ===========================================================================
BEZEL_TEST(unwind_owned_transfer_1955_1956, owning_stack_push_before_throw_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() throw new InvalidOperationException(\"boom\"); end Boom; "
        "  public static method Work() "
        "    using var st := new OwningStack<Resource>(); "
        "    var b := new Resource(); "
        "    Boom(); "                          // throw BEFORE transfer — b live
        "    st.Push(move b); "
        "  end Work; "
        "  public static method Main(): int "
        "    var c := 0; "
        "    try Work(); catch var e: InvalidOperationException => c := 1; end; "
        "    return c * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// #1955 — OwningStack.Push double-free guard: throw AFTER the transfer. The stack
// (a `using var`) disposes `b` on unwind; the caller's nulled slot must skip it.
BEZEL_TEST(unwind_owned_transfer_1955_1956, owning_stack_push_transfer_then_throw_no_double_free_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() throw new InvalidOperationException(\"boom\"); end Boom; "
        "  public static method Work() "
        "    using var st := new OwningStack<Resource>(); "
        "    var b := new Resource(); "
        "    st.Push(move b); "                // b moved in; slot nulled
        "    Boom(); "                          // throw AFTER transfer
        "  end Work; "
        "  public static method Main(): int "
        "    var c := 0; "
        "    try Work(); catch var e: InvalidOperationException => c := 1; end; "
        "    return c * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// #1955 — OwningQueue.Enqueue(owned x) double-free guard: throw AFTER the
// transfer. Same array-backed path; no double-free.
BEZEL_TEST(unwind_owned_transfer_1955_1956, owning_queue_enqueue_transfer_then_throw_no_double_free_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() throw new InvalidOperationException(\"boom\"); end Boom; "
        "  public static method Work() "
        "    using var q := new OwningQueue<Resource>(); "
        "    var b := new Resource(); "
        "    q.Enqueue(move b); "
        "    Boom(); "
        "  end Work; "
        "  public static method Main(): int "
        "    var c := 0; "
        "    try Work(); catch var e: InvalidOperationException => c := 1; end; "
        "    return c * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// #1955 — OwningArray.Insert(index, owned x): the two-arg transfer method
// (bz_array_insert_at, arg 1 is the owned element). A throw BEFORE the Insert
// leaves `b` live → unwind-disposed exactly once.
BEZEL_TEST(unwind_owned_transfer_1955_1956, owning_array_insert_before_throw_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() throw new InvalidOperationException(\"boom\"); end Boom; "
        "  public static method Work() "
        "    using var log := new OwningArray<Resource>(); "
        "    log.Add(new Resource()); "
        "    var b := new Resource(); "
        "    Boom(); "                          // throw BEFORE transfer — b live
        "    log.Insert(0, move b); "
        "  end Work; "
        "  public static method Main(): int "
        "    var c := 0; "
        "    try Work(); catch var e: InvalidOperationException => c := 1; end; "
        "    return c * 10 + Resource._live; "   // 10 = both disposed; b unwind-disposed once
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// #1955 — OwningArray.Insert double-free guard: throw AFTER the transfer. The
// array disposes both elements on unwind; the caller's nulled slot skips `b`.
BEZEL_TEST(unwind_owned_transfer_1955_1956, owning_array_insert_transfer_then_throw_no_double_free_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Boom() throw new InvalidOperationException(\"boom\"); end Boom; "
        "  public static method Work() "
        "    using var log := new OwningArray<Resource>(); "
        "    log.Add(new Resource()); "
        "    var b := new Resource(); "
        "    log.Insert(0, move b); "
        "    Boom(); "                          // throw AFTER transfer
        "  end Work; "
        "  public static method Main(): int "
        "    var c := 0; "
        "    try Work(); catch var e: InvalidOperationException => c := 1; end; "
        "    return c * 10 + Resource._live; "
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// #1955 — Push/Enqueue NORMAL-path control (no throw): the transferred elements
// are disposed exactly once by the collection at scope end; the new unwind-
// eligibility must not double-dispose. PASS-WITH and WITHOUT: exit == 0.
BEZEL_TEST(unwind_owned_transfer_1955_1956, owning_collection_transfer_normal_path_not_double_freed_28_3) {
    auto r = runAotChecked(
        std::string("module M; ") + kResource +
        "class Program "
        "  public static method Work() "
        "    using var st := new OwningStack<Resource>(); "
        "    var a := new Resource(); "
        "    st.Push(move a); "
        "    using var q := new OwningQueue<Resource>(); "
        "    var b := new Resource(); "
        "    q.Enqueue(move b); "
        "  end Work; "
        "  public static method Main(): int "
        "    Work(); "
        "    return Resource._live; "           // 0 = each disposed exactly once
        "  end Main; "
        "end Program;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}
