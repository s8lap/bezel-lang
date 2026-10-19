// fix #6462 / §28.3 v0.338 *Returning a deferred-activation frame* +
// *Owned-local discharge requirement* (affine auto-drop); §22.4 (parameter
// modifiers); stdlib §4.5 v0.195 (Drain mints a fresh drain-view frame over
// the array).
//
// THE GAP. `Ownership.cpp`'s `checkReturnDrainFromScopeBoundReceiver` is the
// return-site twin of #2175(a). Its receiver test asked two questions of a
// tracked binding — is it a `using var` (`implicitDischarge`), is it a plain
// owned local (`everOwnedPlainLocals_`) — and both answer about a LOCAL. A
// `ParameterSymbol` is never a `LocalDecl`, so it can never be in
// `everOwnedPlainLocals_`, and `implicitDischarge` is the `using var` flag.
// The whole parameter class therefore fell through the check's "admitted"
// tail, and the two controls that covered it — `return_parameter_drain_
// admitted_control_spec_28_3` and `return_this_field_drain_admitted_control_
// spec_28_3` in drain_view_residual_surfaces_2175_test.cpp — cover a PLAIN
// parameter and `this`, neither of which is in the owned class. There was no
// row for `owned items: OwningArray<R>` at all.
//
// THE SHAPE. `public static method View(owned items: OwningArray<R>):
// IOwningEnumerable<R> return items.Drain(); end View;` compiled CLEAN
// (warnings only, rc=0) and reached codegen. #6452 narrowed its codegen arm
// to decline to MINT a frame over an `owned` parameter, so on this branch the
// program takes the `#2491` degraded path and SEGVs at a null page; with that
// narrow absent the same program SEGVs inside `bz_array_drain_next`, a
// use-after-free. Both are the same defect seen through two lowerings.
//
// WHY THE VERDICT IS REJECT, AND NOT AN INFERENCE FROM THE CONTROLS. The
// controls being admitted is irrelevant to this class — (a4) is a PLAIN
// parameter, a different class. The verdict comes from §28.3's own words:
//
//   * *Borrow lifetime rules*, the local-scope check's clause 2, states the
//     classification outright: "an `owned` parameter is scope-bound: the
//     method disposes its undischarged value at its own end (*Auto-drop
//     (affine)* below), so a borrow of it is rejected here exactly as a
//     borrow of a scope-bound local is."
//   * *Owned-local discharge requirement*, *Auto-drop (affine)*, puts that
//     discharge at "the end of the method" for an `owned` parameter — so the
//     parameter does NOT outlive the call, which is the premise the (a4)
//     control rests on and the premise *Returning a deferred-activation
//     frame*'s admit-list rests on.
//   * *Returning a deferred-activation frame* then rejects at the return site
//     "a borrow of anything else — a scope-bound local, a `using var`, a
//     temporary ... since the frame would outlive the source". Its reject list
//     is keyed on SCOPE-BOUND, and an `owned` parameter is scope-bound by the
//     first bullet. The frame does outlive the source here: the view is handed
//     to the caller and the parameter's array is released on the same edge.
//   * §22.4 writes the classification on the formal — `owned` and `out owned`
//     are the two spellings discharged at scope end — which is the predicate
//     this fix reads, and the same predicate codegen's
//     `drainReceiverSourceDiesAtScopeEnd` reads.
//
// THE SITE. The discharge is at function exit, but the RULE is still at the
// return site: §28.3 says "rejected at the return site" in three places
// (*Returning a deferred-activation frame*, *Borrow lifetime rules* item 4,
// and the bare-type-parameter callee side's item 4). The discharge's TIMING is
// what makes the parameter scope-bound; it is not an alternative site. The
// check therefore joins the three existing reject classes at the return, and
// codegen's #6452 narrow is left exactly as it is — it is not widened, and it
// is not narrowed further.
//
// ROWS.
//   (1) THE DEFECT, direct spelling — the issue's repro verbatim. Rejected.
//   (2) THE SAME HAZARD, chain root — `owned p: Bag`, `return p.items.Drain();`
//       An ordinary field is the DIRECT receiver, so asking only about the
//       direct receiver leaves this one admitted. Both arms ask about the root.
//   (3) CONTROL, PLAIN parameter — `View(items: OwningArray<R>)`. Still
//       admitted: a plain parameter outlives the call, and nothing here
//       narrows that class.
//   (4) CONTROL, `this` field — §28.3's receiver-sourced case. Still admitted.
//   (5) RUNTIME for the admitted class — (3) compiled, linked and run under
//       runAotChecked (AOT + ASan/LSan): three elements reach the caller and
//       `R.live` returns to 0, so each is discharged exactly once. This is
//       the leak half of the oracle; the crash half is that the rejected
//       shapes produce no binary at all.
//
// NOT ESTABLISHED HERE, and recorded rather than taken: the LAUNDERED
// spelling `var v := items.Drain(); return v;` reaches this check through the
// returned `IdentifierExpr`, which it does not match — the same residual the
// local-decl shapes have had since #2175. Closing it needs the binding peel
// `checkGetterFreshOwnedReturn` does for §28.6, which is a separate change.
// MEASURED post-fix, and it is why #6452's codegen narrow is left exactly
// where it is: that spelling still compiles clean and still emits NO `#2491`
// note, which is the narrow declining the mint. So the narrow remains
// load-bearing for a shape the ownership pass does not yet reach, and neither
// widening nor narrowing it further is available to this fix.

#include <string>

#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "support/aot_support.hpp"         // runAotChecked — AOT + ASan/LSan
#include "support/ownership_support.hpp"   // parseAndCheckOwnership — diag pins
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::checkedToolchainSkipped;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// A file-local substring test, matching the spelling #3892 records as
// already-per-file in this suite.
bool has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

// The diagnostic's load-bearing halves: it names the receiver, and it says
// WHY in the words of §28.3. The "an `owned` parameter" half is the one that
// distinguishes this row from (a1)/(a3) of the #2175 file — a rejection that
// said only "scope-bound" would be satisfied by the pre-fix behaviour of a
// different class, and the pin would stop discriminating.
bool isOwnedParameterDrainError(const std::string& msg) {
    return msg.find("`.Drain()`") != std::string::npos &&
           msg.find("drain view aliases") != std::string::npos &&
           msg.find("an `owned` parameter disposed at the end of this method") !=
               std::string::npos;
}

// ctor +1 / dtor -1 net-zero dispose-once oracle. The static is a `string`-
// typed class INSTANCE (not an int): the §22.12 rule-2 form-(iii) mutable
// primitive-static declaration warning is noise this file has no reason to
// absorb, and `Console.WriteLine` on it keeps the balance observable through
// stdout, which the gtest AOT harness captures.
const char* kR =
    "module M; "
    "class R "
    "  public static live: int := 0; "
    "  public static method Live(): int return R.live; end Live; "
    "  public constructor() R.live := R.live + 1; end constructor; "
    "  destructor R.live := R.live - 1; end destructor; "
    "end R; ";

}  // namespace

// ===========================================================================
// (1) THE DEFECT — the issue's repro, direct spelling. An `owned` parameter
// is SCOPE-BOUND (§28.3), so the drain view aliases storage the callee
// discharges on the same edge it hands the view over.
// ===========================================================================
BEZEL_TEST(drain_view_owned_parameter_6462,
           owned_parameter_drain_return_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kR) +
        "class Bad "
        "  public static method View(owned items: OwningArray<R>): "
        "IOwningEnumerable<R> "
        "    return items.Drain(); "
        "  end View; "
        "end Bad;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (isOwnedParameterDrainError(d.message)) {
            found = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(found);
}

// ===========================================================================
// (2) THE SAME HAZARD AT A CHAIN ROOT — `owned p: Bag`, `p.items.Drain()`.
// The DIRECT receiver here is an ordinary owned field, so a narrow that
// asked only about it would leave this spelling admitted. Both arms of the
// check walk to the root, and this row is what holds that.
// ===========================================================================
BEZEL_TEST(drain_view_owned_parameter_6462,
           owned_parameter_chain_root_drain_return_rejected_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kR) +
        "class Bag "
        "  public owned items: OwningArray<R>; "
        "  public constructor() items := new OwningArray<R>(); end constructor; "
        "end Bag; "
        "class Bad "
        "  public static method View(owned p: Bag): IOwningEnumerable<R> "
        "    return p.items.Drain(); "
        "  end View; "
        "end Bad;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool found = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (isOwnedParameterDrainError(d.message)) {
            found = true;
            break;
        }
    }
    BEZEL_EXPECT_TRUE(found);
}

// ===========================================================================
// (3) CONTROL — the PLAIN parameter is still ADMITTED. This is the pairing
// control for (1): the two programs differ by one keyword, so a fix that
// rejected both would pass (1) alone and fail here. A plain parameter
// outlives the call, which is the whole of its admission.
// ===========================================================================
BEZEL_TEST(drain_view_owned_parameter_6462,
           plain_parameter_drain_return_still_admitted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kR) +
        "class Good "
        "  public static method View(items: OwningArray<R>): "
        "IOwningEnumerable<R> "
        "    return items.Drain(); "
        "  end View; "
        "end Good;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// (4) CONTROL — `this.f.Drain()` is still ADMITTED: §28.3's receiver-sourced
// case, which reaches through the receiver's fields. `this` outlives the
// call and is not scope-bound by any reading.
// ===========================================================================
BEZEL_TEST(drain_view_owned_parameter_6462,
           this_field_drain_return_still_admitted_spec_28_3) {
    auto r = parseAndCheckOwnership(
        std::string(kR) +
        "class Owner "
        "  public owned Items: OwningArray<R>; "
        "  public constructor() Items := new OwningArray<R>(); end constructor; "
        "  public method View(): IOwningEnumerable<R> "
        "    return Items.Drain(); "
        "  end View; "
        "end Owner;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ===========================================================================
// (5) RUNTIME for the class that stays admitted — (3) compiled, linked and
// run. Two lines, both load-bearing: the count is 3, so the frame really
// carried its elements (a null or empty frame yields 0), and the balance is 0,
// so each was discharged exactly once — the leak half of the oracle. A
// use-after-free or a double-free additionally fails `r.ran` under ASan.
// ===========================================================================
BEZEL_TEST(drain_view_owned_parameter_6462,
           plain_parameter_drain_returned_to_caller_iterates_spec_28_3) {
    JitResult r = runAotChecked(
        std::string(kR) +
        "class Good "
        "  public static method View(items: OwningArray<R>): "
        "IOwningEnumerable<R> "
        "    return items.Drain(); "
        "  end View; "
        "end Good; "
        "class P "
        "  public static method Main(): int "
        "    var a := new OwningArray<R>(); "
        "    a.Add(new R()); "
        "    a.Add(new R()); "
        "    a.Add(new R()); "
        "    using var view: IOwningEnumerable<R> := Good.View(a); "
        "    var n := 0; "
        "    for var r in view do "
        "      n := n + 1; "
        "      using var _ := move r; "
        "    end for; "
        "    Console.WriteLine($\"{n}\"); "
        "    a.Dispose(); "
        "    Console.WriteLine($\"{R.Live()}\"); "
        "    return 0; "
        "  end Main; "
        "end P;");
    BEZEL_EXPECT_FALSE(checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "3"));
    BEZEL_EXPECT_TRUE(has(r.stdoutText, "0"));
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}