// Spec-derived regression tests for issue #3779 — a constructor argument's
// owned-sink classification was keyed on its SOURCE position rather than on the
// §22.5 parameter it BINDS to, at six of the eight consumers of a constructor's
// resolved formal list.
//
// ── THE RULE ───────────────────────────────────────────────────────────────
//
// §22.5 "Optional parameters and named arguments": "Named arguments may appear
// in any order after positional arguments" and "naming an argument changes
// which parameter it binds to, never when it runs". `boundParamIndex`
// (Ownership_Checks.cpp, #1973) is the one function that performs that binding,
// and its own comment states what skipping it costs, verbatim: mapping by
// source position "MISSES a consume (an owned binding at a borrow slot's source
// index never marked moved-from → the callee disposes it while the caller still
// believes it owns it → double-free / UAF)".
//
// ── WHAT WAS MEASURED (89eb914 the parent, 5fbb8ab the #3769 fix) ──────────
//
//     class Two
//       private owned _a: Res;
//       public constructor(owned a: Res, b: Res, c: int := 1)
//         _a := move a; ...
//
//   `new Two(b := y, a := x);` as a BARE STATEMENT, `x` a plain owned local and
//   `y` a `using var`:
//
//     89eb914  compiles clean          -> `x` consumed, never marked moved-from
//     5fbb8ab  REJECTED, naming `y`    -> `y` binds to the BORROW formal `b`;
//                                         no correct spelling exists, because
//                                         `owned y` in a named argument is an
//                                         A.7 MarkedTransfer grammar error
//     this     REJECTED, naming `x`    -> the formal `x` actually binds to,
//                                         with `a := move x` as the fix
//
//   The EXACT-ARITY twin (`c := 3` supplied) was wrong on BOTH earlier
//   compilers — #3769 did not introduce the defect, it extended its reach to
//   defaulted-arity calls by resolving a formal list where none resolved before.
//   B2 / B4 pin the exact-arity half; B1 / B3 pin the defaulted-arity half.
//
// ── WHY SIX SITES AND NOT ONE ──────────────────────────────────────────────
//
// `resolveCtorParams`'s result is read by eight consumers. Two bound correctly
// (`recordMovesIn`, `scanCapturedOwnedConsumeExpr`); six read the source index:
// `handleDischarge` (both its §22.3 marker half AND its §28.8 transfer half),
// `borrowSourceForExpr`, `catchBorrowAmongNonOwnedCtorArgs`,
// `collectOwnedConsumingInExpr`, and the constructor arms of `scopedFormalAt`
// and `verifiedCallBoundedFormalAt`. Fixing a subset is the #1462 half-fix
// shape — the §22.3 diagnostic and the §28.8 moved-from marking would disagree
// about the same argument — so they move together, through three accessors
// (`boundFormalIndex` / `boundModifier` / `boundFormalName`) that make the raw
// index unspellable: getting a modifier now REQUIRES passing the argument list
// the binding is computed from.
//
// The METHOD arms of the two §28.14 gates (#3785) were the same shape — they
// received only a callee and a SOURCE index — and were fixed alongside #3779:
// the same signature change (add `args` parameter), the same `boundFormalIndex`
// access. B8 now asserts the §22.5-equivalence between the positional and
// reordered-named spellings of one call.

#include <string>

#include "support/aot_support.hpp"
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::OwnershipResult;
using bezel::test::parseAndCheckOwnership;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// A destructor-less `Res`: the ×200 rows below use a BARE-STATEMENT `new`,
// which §21.9 rejects outright for a non-trivial-dispose class — so the
// exactly-once-disposal oracle here is ASan/LSan over 200 iterations
// (`runAotChecked`), not a destructor counter, plus the `_sum` static that
// proves WHICH formal each argument reached.
const char* kRes =
    "class Res "
    "  private _id: int; "
    "  public constructor(id: int) _id := id; end constructor; "
    "  public method Id(): int => _id; "
    "end Res; ";

// `owned a` FIRST, borrow `b` SECOND. A reordered named call therefore puts the
// borrow at the owned formal's source index and vice versa — the whole point.
// `_sum` accumulates `_a.Id() + _bid + _c`, so a mis-binding changes the value,
// not just the diagnostics.
const char* kTwo =
    "class Two "
    "  public static _sum: int := 0; "
    "  private owned _a: Res; "
    "  private _bid: int; "
    "  private _c: int; "
    "  public constructor(owned a: Res, b: Res, c: int := 1) "
    "    _a := move a; "
    "    _bid := b.Id(); "
    "    _c := c; "
    "    _sum := _sum + _a.Id() + _bid + _c; "
    "  end constructor; "
    "end Two; ";

// The §22.3 (v0.169) missing-owned-marker diagnostic, scoped to a NAME. The
// name is the entire assertion in this file: the defect was never "no
// diagnostic" at the exact-arity sites, it was "the diagnostic names the wrong
// argument", which `hasErrors()` cannot tell apart.
bool hasMissingOwnedMarker(const OwnershipResult& r, const std::string& name) {
    const std::string needle = "this call consumes `" + name + "`";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos &&
            d.message.find("§22.3") != std::string::npos)
            return true;
    return false;
}

// Any error mentioning `name` — the wider net for B1/B2's negative half. On
// 5fbb8ab the wrong argument drew BOTH the §22.3 marker demand and a §28.3
// "cannot move out of `using var`" follow-on, so asserting the absence of only
// the first would have passed over the second.
bool hasErrorNaming(const OwnershipResult& r, const std::string& name) {
    const std::string needle = "`" + name + "`";
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.severity == bezel::Severity::Error &&
            d.message.find(needle) != std::string::npos)
            return true;
    return false;
}

// §28.14's unverifiable-captures diagnostic — B8's known-gap marker.
bool hasUnverifiableCaptures(const OwnershipResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("captures of this function-typed expression cannot "
                           "be verified") != std::string::npos)
            return true;
    return false;
}

// 200 bare-statement constructions. `y` is a borrow throughout; each `x` is a
// fresh owned local transferred by the named `a := move x`.
std::string loopBare(const std::string& newStmt) {
    return "class C "
           "  public static method Main(): int "
           "    using var y := new Res(3); "
           "    var i: int := 0; "
           "    while i < 200 do "
           "      var x := new Res(i); "
           "      " + newStmt +
           "      i := i + 1; "
           "    end while; "
           "    Console.WriteLine($\"sum={Two._sum}\"); "
           "    return 0; "
           "  end Main; "
           "end C; ";
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// B1 — REJECT, and REJECT THE RIGHT ARGUMENT. The review's repro: a
// defaulted-arity bare-statement `new` with reordered named arguments.
// FAIL-WITHOUT (89eb914): no diagnostic at all — `x` silently consumed.
// FAIL-WITHOUT (5fbb8ab): rejected naming `y`, the BORROW formal's argument.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 (named-argument binding) / §22.3 (call-site marker) / §28.8,
// issue #3779
BEZEL_TEST(bound_ctor_arg_named_owned_sink_3779,
           bare_statement_reordered_named_args_name_the_bound_formal_defaulted_arity) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kRes + kTwo +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(6); "
        "    using var y := new Res(7); "
        "    new Two(b := y, a := x); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(hasMissingOwnedMarker(r, "x"));
    BEZEL_EXPECT_FALSE(hasErrorNaming(r, "y"));
}

// ═══════════════════════════════════════════════════════════════════════════
// B2 — REJECT, right argument, EXACT arity. This half was wrong on the parent
// AND on 5fbb8ab: #3769 did not introduce the defect, it widened its reach.
// FAIL-WITHOUT (both): rejected naming `y`.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 / §22.3, issue #3779
BEZEL_TEST(bound_ctor_arg_named_owned_sink_3779,
           bare_statement_reordered_named_args_name_the_bound_formal_exact_arity) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kRes + kTwo +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(6); "
        "    using var y := new Res(7); "
        "    new Two(b := y, a := x, c := 3); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(hasMissingOwnedMarker(r, "x"));
    BEZEL_EXPECT_FALSE(hasErrorNaming(r, "y"));
}

// ═══════════════════════════════════════════════════════════════════════════
// B3 — ACCEPT and RUN, ASan/LSan, 200 iterations, defaulted arity. The correct
// spelling of B1: §22.5 admits `a := move x` because "a transfer-marked
// argument carries no modifier — the marker is part of the argument
// expression". `sum` proves the binding, LSan proves exactly-once disposal of
// 200 unbound owned temporaries.
// FAIL-WITHOUT (5fbb8ab): rejected — "cannot move out of `using var` `y`".
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 / §28.8.1 (the `move` expression) / §28.3 (unbound owned
// temporaries), issue #3779
BEZEL_TEST(bound_ctor_arg_named_owned_sink_3779,
           bare_statement_reordered_named_move_transfers_asan_x200_defaulted_arity) {
    const std::string src = std::string("module M3779c; ") + kRes + kTwo +
                            loopBare("new Two(b := y, a := move x); ");
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // sum = sum over i in 0..199 of (i + y.Id() + default c) = 19900 + 200*(3+1).
    BEZEL_EXPECT_TRUE(r.stdoutText.find("sum=20700") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// B4 — ACCEPT and RUN, ASan/LSan, 200 iterations, EXACT arity. The correct
// spelling of B2, and the row that proves #3779's pre-existing half is closed.
// FAIL-WITHOUT (89eb914 AND 5fbb8ab): rejected — "cannot move out of
// `using var` `y`".
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 / §28.8.1, issue #3779
BEZEL_TEST(bound_ctor_arg_named_owned_sink_3779,
           bare_statement_reordered_named_move_transfers_asan_x200_exact_arity) {
    const std::string src = std::string("module M3779d; ") + kRes + kTwo +
                            loopBare("new Two(b := y, a := move x, c := 3); ");
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // sum = 19900 + 200*(3+3).
    BEZEL_EXPECT_TRUE(r.stdoutText.find("sum=21100") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// B5 — ANTI-OVER-REJECT (passes on all three compilers, by design). The
// ALL-POSITIONAL spelling of B3, where the source index and the bound index
// coincide. §22.5 makes the two spellings equivalent, so their observable
// result must be identical — `sum=20700` here and in B3.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 (naming an argument changes which parameter it binds to, never
// when it runs), issue #3779
BEZEL_TEST(bound_ctor_arg_named_owned_sink_3779,
           bare_statement_positional_control_is_unchanged_asan_x200) {
    const std::string src = std::string("module M3779e; ") + kRes + kTwo +
                            loopBare("new Two(move x, y); ");
    auto r = runAotChecked(src);
    BEZEL_EXPECT_TRUE(r.ran);
    if (!r.ran) return;
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("sum=20700") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// B6 — REJECT. Issue #3779's own filed repro, at exact arity: a bare-statement
// `new Sink(tag := 7, r := x)` against `constructor(owned r: Res, tag: int)`.
// FAIL-WITHOUT (89eb914 AND 5fbb8ab): compiles with NO diagnostic and is a
// measured `AddressSanitizer: SEGV` reading `x` after the constructor disposed
// it — while the identical call bound to a local was correctly rejected.
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 / §22.3 / §28.8, issue #3779
BEZEL_TEST(bound_ctor_arg_named_owned_sink_3779,
           filed_repro_bare_statement_named_owned_arg_is_rejected) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kRes +
        "class Sink "
        "  private owned _s: Res; "
        "  private _t: int; "
        "  public constructor(owned r: Res, tag: int) "
        "    _s := move r; "
        "    _t := tag; "
        "  end constructor; "
        "end Sink; "
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(6); "
        "    new Sink(tag := 7, r := x); "
        "    return x.Id(); "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(hasMissingOwnedMarker(r, "x"));
}

// ═══════════════════════════════════════════════════════════════════════════
// B7 — ANTI-REGRESSION for the path that was ALREADY bound-correct. The same
// call bound to a local reaches `recordMovesIn`, which has applied #1973's
// binding since it was written. It must keep naming `x`, and this fix must not
// disturb it.
// (Fails on 89eb914 only because the defaulted arity resolved no formals there
// at all — that is #3769's half, pinned in its own file.)
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 / §22.3, issues #3779 and #3769
BEZEL_TEST(bound_ctor_arg_named_owned_sink_3779,
           local_decl_form_still_names_the_bound_formal) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kRes + kTwo +
        "class C "
        "  public static method Main(): int "
        "    var x := new Res(6); "
        "    using var y := new Res(7); "
        "    using var t := new Two(b := y, a := x); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_TRUE(hasMissingOwnedMarker(r, "x"));
    BEZEL_EXPECT_FALSE(hasErrorNaming(r, "y"));
}

// ═══════════════════════════════════════════════════════════════════════════
// B8 — METHOD-arms pin for issue #3785 (FIXED). The METHOD arms of
// `scopedFormalAt` / `verifiedCallBoundedFormalAt` now read the §22.5-BOUND
// formal (look up the argument's named-binding target in the callee type's
// parameter list), so the positional and reordered-named spellings of the
// same call draw the same §28.14 verdicts. The pre-fix behaviour was a
// false-positive §28.14 unverifiable-captures diagnostic on the
// reordered-named form alone (the §22.5-equivalent spelling produced none).
// ═══════════════════════════════════════════════════════════════════════════
// per §22.5 / §28.14, issue #3785
BEZEL_TEST(bound_ctor_arg_named_owned_sink_3779,
           method_arm_reads_the_bound_formal_not_source_index_3785) {
    const char* holder =
        "class Holder "
        "  private scoped _h: () -> int; "
        "  public constructor() "
        "    _h := lambda() => 0; "
        "  end constructor; "
        "  public method Keep(scoped cb: () -> int, n: int) "
        "    _h := cb; "
        "  end Keep; "
        "end Holder; ";
    auto positional = parseAndCheckOwnership(
        std::string("module M; ") + holder +
        "class C "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    h.Keep(lambda() => 5, 1); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    auto named = parseAndCheckOwnership(
        std::string("module M; ") + holder +
        "class C "
        "  public static method Main(): int "
        "    using var h := new Holder(); "
        "    h.Keep(n := 1, cb := lambda() => 5); "
        "    return 0; "
        "  end Main; "
        "end C; ");
    BEZEL_EXPECT_FALSE(hasUnverifiableCaptures(positional));
    BEZEL_EXPECT_FALSE(hasUnverifiableCaptures(named));
}
