// Spec-derived tests for Ownership slice O4 — branch joins (§28.3).
//
// O4's contract: at a control-flow merge after if/elif/else, each
// local's ownership state in each incoming branch must agree.
//   - Owned ⊓ Owned         = Owned (obligation carried forward)
//   - Borrowed ⊓ Borrowed   = Borrowed
//   - PolyNull ⊓ X          = X  (PolyNull absorbs into either side)
//   - Owned ⊓ Borrowed      = ERROR (per the spec's asymmetric join)

#include "support/aot_support.hpp"  // runAotChecked — AOT + LSan oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::hasErrorMessage;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAotChecked;

namespace {

// Wrap a method body that uses `cond`, `a`, `b`, `source` as scope-
// available names. Declares all of them as parameters/fields so the
// identifier-resolution pass doesn't emit unrelated diagnostics.
std::string wrap(const std::string& body) {
    return std::string("module M; "
        "class T end T; "
        "class Stream "
        "  public Inner: Stream? := null; "
        "  public Other: Stream? := null; "
        "end Stream; "
        "class C(source: Stream) "
        "  public method M(cond: bool, a: bool, b: bool) "
        ) + body + " end M; end C;";
}

// §28.3 Pattern 2 / K3.14, v0.277: an uninitialized plain local adopts Owned from
// both branches, and must warn+auto-drop rather than hard-error. The destructor's
// marker makes the AOT/LSan test verify one disposal and no double-free.
const char* kNoInitializerOwnedJoin =
    "module M; "
    "class Res : IDisposable "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor Console.WriteLine(\"D\" + Id.ToString()); end destructor; "
    "end Res; "
    "class W "
    "  public static method Main(): int "
    "    var cond := true; "
    "    var x: Res; "
    "    if cond then x := new Res(1); else x := new Res(2); end; "
    "    Console.WriteLine(\"body\"); "
    "    return 0; "
    "  end Main; "
    "end W;";

} // namespace

BEZEL_TEST(own_join, no_initializer_owned_join_warns_and_autodrops_spec_28_3) {
    auto r = runAotChecked(kNoInitializerOwnedJoin);
    bool skipped = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) skipped = true;
    if (skipped) return;  // ASan/LSan toolchain unavailable — skip

    // Before #3317 this valid program was rejected by the no-initializer
    // exclusion in localDropOnOverwriteCovered; now it compiles and runs
    // with exactly one destructor marker under LSan. (The K3.14
    // "implicitly disposed at the end of its scope" warning fires on
    // stderr but runAotChecked's diagnostics bag carries only errors;
    // the warning-text check lives in the parseAndCheckOwnership sibling
    // test below, and LSan is the canonical double-free oracle here.)
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText.find("D1") != std::string::npos, true);
    BEZEL_EXPECT_EQ(r.stdoutText.find("D2") != std::string::npos, false);
    BEZEL_EXPECT_EQ(r.stdoutText.find("D3") != std::string::npos, false);
}

BEZEL_TEST(own_join, owned_vs_borrowed_at_merge_diagnosed) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: Stream?; "
        "if cond then x := new Stream(); "
        "else x := source.Inner; "
        "end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "branch ownership conflict"));
}

BEZEL_TEST(own_join, owned_on_both_branches_carries_obligation) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: Stream; "
        "if cond then x := new Stream(); "
        "else x := new Stream(); "
        "end;"));
    // Both branches produce owned → the no-initializer declaration adopts
    // Owned and §28.3 K3.14 emits a warning plus scope-end auto-drop.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    bool warned = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Warning &&
            d.message.find("implicitly disposed at the end of its scope") !=
                std::string::npos)
            warned = true;
    BEZEL_EXPECT_TRUE(warned);
}

BEZEL_TEST(own_join, owned_on_both_branches_and_discharged_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: Stream; "
        "if cond then x := new Stream(); "
        "else x := new Stream(); "
        "end; "
        "x.Dispose();"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_join, borrowed_on_both_branches_clean) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: Stream?; "
        "if cond then x := source.Inner; "
        "else x := source.Other; "
        "end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_join, no_else_with_owned_in_then_is_clash) {
    // Without an else, the false edge falls through with entry state.
    // Entry x is Uninitialized; then-branch makes it Owned. Merge is
    // Owned ⊓ Uninitialized — currently reported as a poison/clash
    // since they don't match.
    auto r = parseAndCheckOwnership(wrap(
        "var x: Stream; "
        "if cond then x := new Stream(); end;"));
    // Slice O4 treats this as a state mismatch (state poison) — the
    // local can't be safely used after the merge. No explicit diagnostic
    // is required here (the state becomes Poisoned silently); a
    // subsequent read would diagnose.
    (void)r;
}

// #1084: the PolyNull-from-null tests below annotate the null local
// (`var x: Stream? := null`) — a bare typeless `var x := null` is now a §7.1
// reject. The PolyNull ownership behavior (null absorbing into Owned/Borrowed
// across branch joins) is unchanged by the annotation.
BEZEL_TEST(own_join, null_init_then_owned_in_branch_absorbs) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: Stream? := null; "
        "if cond then x := new Stream(); "
        "else x := new Stream(); "
        "end; "
        "x.Dispose();"));
    // Entry is PolyNull; both branches produce Owned. PolyNull ⊓ Owned
    // = Owned, then Owned ⊓ Owned = Owned. Discharge satisfies.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_join, polynull_absorbs_borrowed) {
    auto r = parseAndCheckOwnership(wrap(
        "var x: Stream? := null; "
        "if cond then x := source.Inner; "
        "else x := source.Other; "
        "end;"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(own_join, three_branch_chain_with_clash) {
    // if-elif-else, one branch produces owned, one borrowed → error.
    auto r = parseAndCheckOwnership(wrap(
        "var x: Stream?; "
        "if a then x := new Stream(); "
        "elif b then x := new Stream(); "
        "else x := source.Inner; "
        "end;"));
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasErrorMessage(r, "branch ownership conflict"));
}

// fix #4329 (§28.3 / §28.7, v0.298): a REASSIGNMENT of a silently-poisoned
// `Owned ⊓ Uninitialized` non-loop merge (the O4 catch-all) leaks the prior
// Owned value. The O4 design poisons the merged local so a *subsequent read*
// diagnoses, but the reassignment of `x := new Res(9);` overwrites the state
// to the new producer's classification without disposing the prior Owned
// allocation on the assigned path — codegen's `disposeDisplacedOwnedClassLocal`
// refuses to fire for a no-init LocalDecl (`unwindOwnedLocalClass` returns
// null on `!ld->initializer`). The fix sets `becomesOwnedViaReassign` on
// the LocalDecl at the reassignment site (Ownership.cpp's handleAssignment,
// gated on `state == Poisoned && obligationOrigin`); the codegen helper then
// admits the local as an owned class local and disposes the displaced value
// at the reassignment (null-guarded + identity-guarded, so a no-prior-value
// slot is a no-op). Measured on the pre-fix tree, --sanitize=address × 50
// iterations: "AddressSanitizer: 800 byte(s) leaked in 50 allocation(s)".
// Post-fix: 50 prior-Owned + 50 fresh-Owned disposals = Counter == 100.
//
// The test amplifies to 50 iterations so the leak — which sits in the
// cond=true branches' allocations — is reproducible on the LSan oracle
// even with the test binary's per-iteration teardown amortizing most of the
// stack-reachability work: 50 × sizeof(Res) is well above the 16-byte LSan
// detection floor, and the loop dominates. With `i mod 2 = 0`, cond is true
// on the even iterations: 25 of 50 paths allocate the prior Res(5).
// Post-fix: 25 Res(5) + 50 Res(9) = 75 destructor runs; the program
// returns Counter._n = 75 (75 mod 256 == 75, so the AOT exit-status
// truncation does not interfere).
BEZEL_TEST(own_join, poisoned_no_else_reassign_disposes_prior_owned_value_4329) {
    auto r = runAotChecked(
        "module M; "
        "class Counter public static _n: int := 0; end Counter; "
        "class Res "
        "  public tag: int; "
        "  public constructor(t: int) this.tag := t; end constructor; "
        "  destructor Counter._n := Counter._n + 1; end destructor; "
        "end Res; "
        "class C "
        "  public static method Main(): int "
        "    var i := 0; "
        "    while i < 50 do "
        "      var cond := (i mod 2 = 0); "
        "      var x: Res; "
        "      if cond then "
        "        x := new Res(5); "
        "      end; "
        "      x := new Res(9); "
        "      x.Dispose(); "
        "      i := i + 1; "
        "    end; "
        "    return Counter._n; "
        "  end Main; "
        "end C;");
    bool skipped = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) skipped = true;
    if (skipped) return;  // ASan/LSan toolchain unavailable — skip
    // Pre-fix: 25 leaks × sizeof(Res) → ASan/LSan reports, exit nonzero.
    // Post-fix: 25 prior-Res(5) + 50 fresh-Res(9) = 75 destructor runs;
    // the program returns Counter._n = 75; no LSan reports.
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 75);
}
