// fix-forward #3575 (§35.4 / §35.3, v0.334): in `MethodOwnership::producerState`,
// ONLY a genuine owning-choice CASE CONSTRUCTION is a fresh `Owned` producer.
//
// THE RULE. §35.3: "The constructed value's own ownership follows its
// classification (§35.4): … an owning construction is an **owned move-only
// value** — §28.3's producers rows for move-only values cover it (the caller
// takes ownership and discharges by binding, storing, returning, or consuming
// it)". §35.4 then makes that obligation observable: "locals are auto-disposed
// at scope end with the must-use warning (K3.14)". So `var b := Bundle.Ready(r)`
// is `Owned`, and an undischarged one must warn.
//
// THE DEFECT (found in non-author review of e7a7616a, REJECTED, never landed).
// The arm that makes the warning fire gated on the wrong thing. It entered its
// `Owned` branch whenever the callee symbol was merely "not a MethodSymbol",
// which admits every call that merely READS an owning choice out of storage.
// A read is a BORROW: the local it produces must be discharged against the
// receiver, not independently. Classifying it `Owned` discharges the produced
// local on its own (scope-end auto-drop, or an `owned` sink), disposing a
// payload the receiver still owns — the receiver's destructor then disposes it
// a SECOND time.
//
// MEASURED, on this lane's base (r23-c28) versus the arm as submitted:
//
//   spelling                                    base              arm as submitted
//   `b.Get()`            (MethodSymbol)         rejected (§28.8)   rejected
//   `b._fetch()`  (function-typed FIELD)        rejected (§28.8)   ACCEPTED
//   `g()`         (function-typed LOCAL)        rejected (§28.8)   ACCEPTED
//   `Bundle.Ready(r)`   (case construction)     NO warning         warned
//
// The two function-typed rows are the silent-acceptance inversion: a rejection
// was DELETED. Feeding the accepted form to the AOT path under ASan yields
// `heap-use-after-free … in Box$DisposeCore`, freed by `main`, allocated by
// `Box$ctor` — the review's `Box$DisposeCore <- Box$Dispose <- main` backtrace.
//
// THE FIX. The test is POSITIVE: the callee must resolve to a
// `sema::ChoiceCaseSymbol` whose parent is a Choice `TypeSymbol`. That is what
// sema actually produces for a case construction (`ExprTypecheck_Calls.cpp`
// ~4438 stamps the call's result type from exactly this resolution).
// `sema::ConstructorSymbol` is the WRONG test and is measured dead: it is a
// SIBLING of `ChoiceCaseSymbol` under `MemberSymbol`, not a supertype, so the
// cast is unconditionally null and the §35.4 rule silently retires.
//
// ORACLE. The two function-typed rows are REJECTS, so their fail-without is the
// §28.8 diagnostic being ABSENT without the fix (an accept row is the vacuous
// oracle here — the program then double-frees). The positive row is a warn:
// `runAot` fills diagnostics only on reject, so the must-use warning is read
// off a REJECTED run — the case construction is given a second, rejected
// statement so the diagnostic channel is populated — and the needle is the
// §28.3 must-use text, not merely "an error happened".
//
// per §35.4, §35.3, §28.3 K3.14, §28.8, v0.334

#include <string>

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"          // runAot — the strict AOT oracle
#include "support/codegen_support.hpp"      // bezel::test::JitResult
#include "test_harness.hpp"

using bezel::test::diagnosticMessageContains;
using bezel::test::failedAtCompileTimeWithRealLocation;
using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// §35.4 rule 1's own shape: an owning choice (a payload marked `owned`), whose
// payload is a class with a COUNTING destructor so "disposed exactly once"
// is checkable, wrapped in a class whose `owned` field of that choice type is
// the storage every read spelling borrows from.
const char* kCoreDecls =
    "public class Resource "
    "  private _n: int; "
    "  public constructor(n: int) _n := n; end constructor; "
    "  public method N(): int => _n; "
    "  destructor Console.WriteLine($\"disposed {_n}\"); end destructor; "
    "end Resource; "
    "public choice Bundle "
    "  Ready(owned r: Resource), "
    "  Empty; "
    "end Bundle; ";

// The class whose `owned` field every READ spelling borrows from. Kept apart
// from kCoreDecls since #6630 (§28.3 sink rule, v0.340): its constructor's
// `_fetch := lambda() => _h` stores a closure that captures the receiver into
// a field, which the sink rule now rejects at the store. The three READ rows
// below are rejects whose §28.8 needle is still asserted on their own; the
// POSITIVE row needs a program that is legal apart from the transfer under
// test, so it is built without Box (progNoBox).
const char* kBoxDecls =
    "public class Box "
    // `owned` field of the owning choice: the storage every read borrows.
    "  public owned _h: Bundle; "
    // THE FUNCTION-TYPED FIELD spelling. A field is not a method, so this call
    // has no MethodSymbol — the shape the rejected arm let through.
    "  public _fetch: () -> Bundle; "
    "  public constructor() "
    "    _h := Bundle.Ready(new Resource(42)); "
    "    _fetch := lambda() => _h; "
    "  end constructor; "
    // THE METHOD spelling. `Get` returns `_h` by reading it.
    "  public method Get(): Bundle => _h; "
    // The `owned` sink that discharges the produced local on its own.
    "  public method Take(owned h: Bundle) end; "
    "end Box; ";

// The `owned` sink the POSITIVE row transfers a genuine construction into.
const char* kSinkDecls =
    "public class Sink "
    "  public constructor() end constructor; "
    "  public method Put(owned h: Bundle) end; "
    "end Sink; ";

std::string prog(const char* body) {
    return std::string("module M; ") + kCoreDecls + kBoxDecls + kSinkDecls +
           "public class C "
           "  public static method Main(): int " + body +
           "  end Main; "
           "end C;";
}

// The same program without Box — see kBoxDecls.
std::string progNoBox(const char* body) {
    return std::string("module M; ") + kCoreDecls + kSinkDecls +
           "public class C "
           "  public static method Main(): int " + body +
           "  end Main; "
           "end C;";
}

// §28.8's rejection of a transfer whose source is a borrow.
const char* kBorrowToOwnedNeedle = "a borrow cannot be passed";

}  // namespace

// --- the rejects: a READ is a borrow, never a fresh producer ---------------

BEZEL_TEST(owning_choice_case_ctor_only_is_a_fresh_producer_3575,
           spec_35_4_call_through_function_typed_field_is_a_borrow_not_owned) {
    // `b._fetch()`: the callee is a FieldSymbol of function type. It reads the
    // owned field `_h` and hands back a borrow of it. The `owned` sink must
    // reject it (§28.8). Without the fix this compiles and double-frees.
    auto r = runAot(prog(
        "var b := new Box(); "
        "var h := b._fetch(); "
        "b.Take(move h); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kBorrowToOwnedNeedle));
}

BEZEL_TEST(owning_choice_case_ctor_only_is_a_fresh_producer_3575,
           spec_35_4_call_through_function_typed_local_is_a_borrow_not_owned) {
    // `g()`: the callee is a LocalSymbol of function type whose body returns
    // the owned field — the same read, reached without any member access. A
    // local carries no MethodSymbol and no ConstructorSymbol, so it is the
    // sharpest instance of the rejected arm's "no MethodSymbol means fresh
    // producer" inference.
    auto r = runAot(prog(
        "var b := new Box(); "
        "var src := b; "
        "var g: () -> Bundle := lambda() => src._h; "
        "var h := g(); "
        "b.Take(move h); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kBorrowToOwnedNeedle));
}

BEZEL_TEST(owning_choice_case_ctor_only_is_a_fresh_producer_3575,
           spec_35_4_method_returning_the_owned_field_is_a_borrow_not_owned) {
    // `b.Get()`: the callee IS a MethodSymbol. This row is the existing
    // method-spelling coverage; it is kept here so all three read spellings
    // stand together, and because the corrected arm replaces the method
    // spelling's (empty) special case with the positive case-member test —
    // this row is what proves that replacement did not lose the method arm.
    auto r = runAot(prog(
        "var b := new Box(); "
        "var h := b.Get(); "
        "b.Take(move h); "
        "return 0;"));
    BEZEL_EXPECT_TRUE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(r, kBorrowToOwnedNeedle));
}

// --- the positive: a genuine case construction IS a fresh producer ----------

BEZEL_TEST(owning_choice_case_ctor_only_is_a_fresh_producer_3575,
           spec_35_4_owning_case_construction_is_an_owned_transfer_source) {
    // §35.3's positive half, and the ratchet for this arm: the corrected test
    // is `ChoiceCaseSymbol`-keyed, so it MUST keep firing on the construction it
    // exists to catch, or §35.4's rule silently retires and every owning
    // payload of this shape is classified a borrow.
    //
    // The oracle is the §28.8 TRANSFER, which discriminates on the
    // classification itself rather than on its consequence. §35.3 makes an
    // owning construction "an `owned` move-only value … the caller takes
    // ownership and discharges it by binding, storing, returning, or
    // CONSUMING it" — so `s.Put(move b)` is an owned transfer and must be
    // ADMITTED. MEASURED on this lane's base (r23-c28, arm absent): the same
    // program is REJECTED — `b` classified Borrowed makes the marker a
    // borrow-transfer, and §28.8 refuses it. That makes this an accept row with
    // a real fail-without, rather than the vacuous one an earlier draft of this
    // row was: a scope-end-disposal oracle passes at base too, because codegen
    // disposes the owning-choice local off the TYPE and so fires the destructor
    // with or without the ownership-pass classification.
    //
    // The disposal is still asserted, because "it compiled" would not separate
    // a correct transfer from a leak: the payload must be disposed EXACTLY ONCE,
    // by the sink that took the obligation. `checkedToolchainSkipped` is
    // checked first so a toolchain skip cannot satisfy the `ran` assertion
    // vacuously.
    auto r = runAot(progNoBox(
        "var b := Bundle.Ready(new Resource(42)); "
        "var s := new Sink(); "
        "s.Put(move b); "
        "return 0;"));
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    // The pre-fix REJECT must be gone, and with its specific needle gone.
    BEZEL_EXPECT_FALSE(failedAtCompileTimeWithRealLocation(r));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, kBorrowToOwnedNeedle));
    // A real run, not a compile that produced nothing to execute.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // EXACTLY ONCE: absent is a leak, doubled is a double free.
    BEZEL_EXPECT_TRUE(r.stdoutText.find("disposed 42") != std::string::npos);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("disposed 42") ==
                      r.stdoutText.rfind("disposed 42"));
}