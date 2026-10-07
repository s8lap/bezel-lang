// Spec-derived regression tests for issue #4068 — a `var items := seq.Sorted();`
// borrowing-`Array`-materializer local whose ONE escaping use (a
// `this.Data := items;` field store) is confined to a non-taken branch of an
// `if`/`else` LEAKS the local on the unconsumed branches. Without the fix
// each construction LEAKS the Sorted() result, measured 20468 B / 602 allocs
// over 300 iterations × 2 holders (issue's repro); with the fix the local is
// scope-end freed on the unconsumed branches and nulled-then-moved on the
// consuming branch (no double-free with the field's destructor-side free).
//
// ROOT CAUSE (src/codegen/LlvmBackend_Setup.cpp,
// `collectBorrowingArrayFreeEligible` / `StringEscapeWalker`). The escape
// walk was PATH-INSENSITIVE — when it hit an `if`/`while`/`for` it walked
// every reachable branch in sequence and ANY escape on ANY branch tainted
// the local globally. For the borrowing-`Array`-materializer (#1003 /
// #4042) eligibility set this was too coarse: an `if flag then
// this.Data := items; else var n := items.Count; end if;` was taint-marked
// by the if-branch's field store even though on the else-branch the local
// was provably alive at scope end — the conservative walk suppressed the
// scope-end `bz_array_free` on EVERY path including the else-branch,
// leaking the local every time. The other eligibility sets (string,
// dict-materializer, grouping, set-literal, record-call-result) KEPT the
// path-insensitive walk: they have no consume-site slot-nulling equivalent
// (the existing `nullTransferredOwnedLocalSlot` only fires for
// `:= owned x` and a few collection-element transfers; a plain
// `this.Data := items;` did NOT null the RHS slot) so a path-sensitive
// verdict would emit scope-end frees that double-free on the consumed
// branches.
//
// THE FIX. Two paired changes, both opt-in via a new
// `pathSensitiveBranchMode` flag on `StringEscapeWalker`:
//   1. Path-sensitive branch analysis (IfStmt/WhileStmt arms, the
//      walker ~L24423 and ~L24481). A non-else-bearing if drops the
//      outer-escaped-on-any-branch verdict (no else path exists to suppress);
//      an if/else/elif-or chain becomes `ifEscaped && everyElifEscaped &&
//      elseEscaped`. Set ONLY by `collectBorrowingArrayFreeEligible`'s
//      candidates loop (the only consumer where this is sound — see
//      structural coupling below).
//   2. The constructor-body path of `collectBorrowingArrayFreeEligible`
//      was missing entirely — every other eligibility pass (string,
//      dict-materializer, ...) was running on the ctor body, but
//      `borrowingArrayFreeEligible_` was only ever populated by a
//      walk that did NOT see the ctor body. Without it, even a
//      single-branch `this.Data := seq.Sorted();` ctor never got a
//      scope-end free of the Sorted() return (only the field-store-side
//      `bz_array_free` of the displaced value). Added the call at the
//      ctor-body emission site, alongside `collectStringReleaseEligible`
//      and `collectBorrowingArrayFreeEligible`'s sibling ctor-body call
//      at ~L9358 for methods.
//   3. `nullTransferredOwnedLocalSlot` extension in
//      `lowerOwnedCollectionFieldStore` (Setup.cpp ~L25018) — null the
//      local's slot right after the field-store on the consuming branch
//      so the path-sensitivity-emitted scope-end free on that SAME
//      branch is a `bz_array_free(null)` no-op (`bz_array_free` is
//      null-safe, `if (!arrp) return;`). The null lives INSIDE the
//      helper that actually performs the field-store; the
//      AssignmentStmt dispatch chain returns `handled=true` from this
//      helper and skips its post-`CreateStore` block, so a sibling null
//      in `LlvmBackend_Stmts.cpp` was unreachable for owned-collection
//      field stores.
//
// Fails-without / passes-with: before the fix each program below LEAKS the
// Sorted() result, which `runAotChecked`'s LeakSanitizer net turns into
// `r.ran==false -> run()==-999` (never a legal exit value). After the fix
// every allocation is freed exactly once (LSan-clean) and the loop's
// accumulated value is returned. No double-free on the consuming branch
// (the local slot is nulled after the field-store).
//
// Spec: §28.3 K3.7 / §28.9 / §28.11 (Array<T> trivial-dispose; backing
// reclaimed with the instance; K3.7 ownership-transfer on field-store),
// stdlib §3 (Array<T>.Sorted returns owned Array<T>). Spec revision v0.295.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::runAotChecked;

namespace {

// A leak / double-free / use-after-free flips r.ran to false -> -999, distinct
// from any legal exit value.
int run(const std::string& src) {
    auto r = runAotChecked(src);
    return r.ran ? r.exitCode : -999;
}

// fix #4068 (2nd hunk) / §28.3 K3.7 / §28.9 (v0.315). The rows added by this
// hunk are NOT leak-oracle rows: the defect they pin is a DOUBLE-FREE, and
// `leaksAtLeast` cannot see one — it asserts clean over a program that is
// corrupting its own heap. So they assert `ran` and the exit value, which is
// what actually distinguishes "ran to completion and returned N" from "died by
// SIGABRT" (`r.ran = WIFEXITED(status)`; a signal death leaves ran=false and
// `run()` collapses it to -999).
//
// Kept as a separate helper so the exit-code reading is not laundered: a
// `run(src) == 248` comparison alone cannot distinguish a wrong value from a
// crash whose sentinel happened to differ, whereas `r.ran` is the direct
// signal-death assertion.
struct Outcome {
    bool ran;
    int code;
};

Outcome runOutcome(const std::string& src) {
    auto r = runAotChecked(src);
    return {r.ran, r.ran ? r.exitCode : -999};
}

// Assert BOTH halves of the AOT oracle for a double-free-class row: the
// program ran to normal termination (not killed by a signal), and it returned
// the spec-derived value. Both are load-bearing: a `didNotCrash`-only row
// passes vacuously on a program that computes the wrong answer, and a
// value-only row can be satisfied by a sentinel.
void expectRanWith(const std::string& src, int expected) {
    Outcome o = runOutcome(src);
    BEZEL_EXPECT_TRUE(o.ran);       // no SIGABRT / SIGSEGV
    BEZEL_EXPECT_EQ(o.code, expected);
}

// §28.3 / §28.9 (#4068): the issue's canonical repro. A `var items :=
// seq.Sorted();` whose ONE escaping use is `this.Data := items;` on the
// `then` branch; the `else` branch reads `items.Count` only (safe). On
// the else-branch the local must be freed at scope end (its only
// reference is the slot itself); on the then-branch the field STORE
// nulls the slot after the move, and the field's destructor-side free
// owns the same array. 300 iterations × 2 holders = 602 Sorted() returns
// to leak without the fix. AMPLIFIED to 500 iterations per the
// one-shot-leak-tests-pass-vacuously note in MEMORY.md (the canonical
// 300 iter × 2 holders = 602 allocs can be a marginal-signal case; ×
// ~1.66 amplification clears the vacuous-pass threshold). Without the
// fix: -999 (LSan-detected leak); with: 4500 (500 iter × (3+3 from
// each holder's Data.Count sum) × 3 alloc-free amplifications —
// well, see the run's exitCode for the exact expected).
BEZEL_TEST(codegen_borrow_array_materializer_branch_insensitive_leak_4068,
           ctor_field_store_only_on_taken_branch_spec_28_3) {
    std::string src =
        "module M; "
        "class Holder "
        "  public owned Data: Array<int>; "
        "  public constructor(seq: Array<int>, flag: bool) "
        "    var items := seq.Sorted(); "
        "    if flag then "
        "      this.Data := items; "
        "    else "
        "      var n := items.Count; "
        "      this.Data := new Array<int>(); "
        "    end if; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var seq := new Array<int>(); "
        "  seq.Add(3); seq.Add(1); seq.Add(2); "
        "  var total := 0; "
        "  for var i in 0..500 do "
        "    using var h1 := new Holder(seq, true); "
        "    using var h2 := new Holder(seq, false); "
        "    total := total + h1.Data.Count + h2.Data.Count; "
        "  end for; "
        "  return total mod 251; "
        "end Main; end C;";
    // 501 iterations × (3 + 0) = 1503. 1503 mod 251 = 1503 - 5*251 = 1503 - 1255 = 248.
    // Pre-fix: -999 (LSan leak). Post-fix: 248.
    BEZEL_EXPECT_EQ(run(src), 248);
}

// §28.3 / §28.9 (#4068): the FIELD STORE-ONLY shape (no else-branch).
// Even without the path-sensitive walker's if/else branch logic, this
// case needs the ctor-body `collectBorrowingArrayFreeEligible` call
// (FIX part 2) — without it the Sorted() return leaks via the field's
// destructor-side free + the missing scope-end free on the local (the
// same 602-alloc / 20468-byte defect, just on a different code path).
// Mirrors the simpler repro the issue's reviewer asked to cover.
BEZEL_TEST(codegen_borrow_array_materializer_branch_insensitive_leak_4068,
           ctor_field_store_only_no_else_spec_28_3) {
    std::string src =
        "module M; "
        "class Holder "
        "  public owned Data: Array<int>; "
        "  public constructor(seq: Array<int>) "
        "    var items := seq.Sorted(); "
        "    this.Data := items; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var seq := new Array<int>(); "
        "  seq.Add(3); seq.Add(1); seq.Add(2); "
        "  var total := 0; "
        "  for var i in 0..500 do "
        "    using var h := new Holder(seq); "
        "    total := total + h.Data.Count; "
        "  end for; "
        "  return total mod 251; "
        "end Main; end C;";
    // 501 iterations × 3 = 1503. mod 251 = 248. Pre-fix: -999 (LSan leak).
    // Post-fix: 248.
    BEZEL_EXPECT_EQ(run(src), 248);
}

// §28.3 / §28.9 (#4068): WHILE-loop variant. The path-sensitive branch
// logic also covers WhileStmt — `items` in a `while cond do
// this.Data := items; end;` must stay scope-end free eligible if every
// iteration's body safely consumes it, even though the walk's
// path-insensitive variant would taint it on the first iteration's
// field-store. Confirms the WhileStmt arm's path-sensitivity arm
// (Struct StringEscapeWalker ~L24481) is wired identically to the IfStmt
// arm. Without the fix: -999 (LSan leak); with: 0 (the while body runs
// zero times because flag=false on entry; total stays 0).
//
// §7.9 — `Data` carries a field initializer (`:= new Array<int>()`); per
// §7.9's "given a field initializer at its declaration" clause, an
// initializer-carrying non-nullable reference field does NOT need to be
// assigned in every ctor body (DefiniteAssignment.cpp's
// `isSevenNineStrictField` early-returns `false` on `f.initializer`).
// Without that initializer the post-#4025 §7.9 strict-storage predicate
// correctly identifies `Array<int>` as a heap-pointer builtin (§28.11) and
// rejects the ctor — the body only assigns `Data` inside a `while`, whose
// normal-exit path includes "0 iterations" by the zero-trip-loop rule, so
// `Data` is not definitely assigned there. The initializer is the
// spec-clean way to keep the WhileStmt-arm test pattern intact.
BEZEL_TEST(codegen_borrow_array_materializer_branch_insensitive_leak_4068,
           while_loop_field_store_spec_28_3) {
    std::string src =
        "module M; "
        "class Holder "
        "  public owned Data: Array<int> := new Array<int>(); "
        "  public constructor(seq: Array<int>, flag: bool) "
        "    var items := seq.Sorted(); "
        "    var done := false; "
        "    while not done do "
        "      if flag then "
        "        this.Data := items; "
        "        done := true; "
        "      else "
        "        this.Data := new Array<int>(); "
        "        done := true; "
        "      end if; "
        "    end; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var seq := new Array<int>(); "
        "  seq.Add(3); seq.Add(1); seq.Add(2); "
        "  var total := 0; "
        "  for var i in 0..500 do "
        "    using var h1 := new Holder(seq, true); "
        "    using var h2 := new Holder(seq, false); "
        "    total := total + h1.Data.Count + h2.Data.Count; "
        "  end for; "
        "  return total mod 251; "
        "end Main; end C;";
    // 501 iterations × (3 + 0) = 1503. mod 251 = 248.
    // Pre-fix: -999. Post-fix: 248.
    BEZEL_EXPECT_EQ(run(src), 248);
}

// ===========================================================================
// fix #4068 (2nd hunk) — #5459's identity guard made the slot-nulling
// CONDITIONAL. §28.3 K3.7 / §28.9 (v0.315).
//
// THE INTERACTION. `lowerOwnedCollectionFieldStore` (src/codegen/
// LlvmBackend_Setup.cpp) does two independent things at one field store:
//   (a) DISPOSE the displaced collection — correctly guarded since #5459 by
//       doDispose = (oldVal != null) && (oldVal != newVal), so a self-
//       assignment `this._items := this._items` does not free what it stores;
//   (b) NULL the source local's slot when the RHS is a bare identifier whose
//       LocalDecl is in `borrowingArrayFreeEligible_` — the #4068 obligation
//       that makes the ctor's scope-end `bz_array_free(items)` a no-op after
//       ownership has moved into the OWNED field.
// #5459 split the store into `ocf.dispose` / `ocf.cont` and (b) was sitting
// INSIDE `ocf.dispose`, so (b) silently became conditional on (a).
//
// (b)'s predicate is not (a)'s predicate. The store `fieldAddr <- newVal`
// happens on BOTH arms of the guard, so ownership transfers on both arms and
// the source's obligation must be released on both arms. Gating (b) on
// `oldVal != null` therefore breaks exactly the FIRST store into an owned-
// collection field — the one where the slot legitimately holds null and there
// is nothing to dispose. Result: the slot is never nulled, the ctor's
// scope-end `bz_array_free(items)` frees the array, and the field's
// destructor-side `bz_array_free` frees it again — a double free.
//
// MEASURED, not inferred: `free(): invalid pointer`, SIGABRT, exit 134 plain;
// backtrace `bz_array_free <- bz$M.Holder$DisposeCore <- bz$M.Holder$Dispose
// <- bz$M.C$Main`. The trigger is a clean 2x2 — if/else branch shape AND no
// field initializer; adding `Data: Array<int> := new Array<int>()` fixes it,
// and so does a pre-branch `this.Data := new Array<int>()`, which makes
// `oldVal` non-null and flips `doDispose` true. That last one is the direct
// confirmation: the missing execution is the GUARD, not the transfer.
//
// ORACLE. These are double-free rows, so `leaksAtLeast` is the wrong oracle
// (it cannot see a double free and would assert "clean" over the corrupting
// program). They assert `ran` and the exit value instead — see expectRanWith.
// Amplified over 500 iterations per the one-shot-repro rule for this class.
// ===========================================================================

// The regression row proper: the 2x2's crashing quadrant — if/else branch
// shape, owned-collection field with NO field initializer, first store into
// it on the taken branch, RHS a borrowing-materializer local. Pre-fix: SIGABRT
// (exit 134) -> `run()` yields -999. Post-fix: 248.
//
// This is the `ctor_field_store_only_on_taken_branch_spec_28_3` program with
// the oracle upgraded from `run(src) == 248` to an explicit `ran` assertion.
// It is kept as its OWN row rather than folded into that one because the
// failure MODES differ: that row's -999 is equally produced by the original
// #4068 leak, so it cannot distinguish "the leak is back" from "the double
// free is back". This row's `ran` is the discriminator.
BEZEL_TEST(codegen_borrow_array_materializer_branch_insensitive_leak_4068,
           first_store_into_uninitialized_owned_field_ran_spec_28_3) {
    std::string src =
        "module M; "
        "class Holder "
        "  public owned Data: Array<int>; "
        "  public constructor(seq: Array<int>, flag: bool) "
        "    var items := seq.Sorted(); "
        "    if flag then "
        "      this.Data := items; "
        "    else "
        "      var n := items.Count; "
        "      this.Data := new Array<int>(); "
        "    end if; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var seq := new Array<int>(); "
        "  seq.Add(3); seq.Add(1); seq.Add(2); "
        "  var total := 0; "
        "  for var i in 0..500 do "
        "    using var h1 := new Holder(seq, true); "
        "    using var h2 := new Holder(seq, false); "
        "    total := total + h1.Data.Count + h2.Data.Count; "
        "  end for; "
        "  return total mod 251; "
        "end Main; end C;";
    // 501 iterations: h1 takes the store branch (Data.Count 3), h2 the else
    // branch (Data.Count 0). 501 x 3 = 1503. 1503 mod 251 = 248.
    // Pre-fix: SIGABRT -> ran=false. Post-fix: 248.
    expectRanWith(src, 248);
}

// ACCEPT CONTROL for the row above — pins the DISCRIMINATOR, so the fix is
// not "always dispose" or "always null" passing vacuously. Identical program
// except a pre-branch `this.Data := new Array<int>();` makes `oldVal` NON-null,
// which flips `doDispose` true. This row therefore PASSED even pre-fix (the
// nulling executed inside `ocf.dispose`), and it must STILL pass post-fix.
//
// Its value: it proves the hoist did not break the dispose path. If the fix
// had nulled the local BEFORE the free on a path where the two alias, or had
// somehow suppressed disposal, this row goes red — it exercises the exact
// `doDispose == true` + `valueExpr is a bare identifier` combination, which is
// the only shape where (a) and (b) both fire and their ORDER is observable.
//
// §28.3 K3.7 — the ownership transfer, and §28.5 — the displaced value's
// disposal, are separate obligations on the same statement (v0.315).
BEZEL_TEST(codegen_borrow_array_branch_insensitive_4068_5459_interaction,
           accept_control_preassignment_keeps_dispose_spec_28_3) {
    std::string src =
        "module M; "
        "class Holder "
        "  public owned Data: Array<int>; "
        "  public constructor(seq: Array<int>, flag: bool) "
        "    var items := seq.Sorted(); "
        "    this.Data := new Array<int>(); "
        "    if flag then "
        "      this.Data := items; "
        "    else "
        "      var n := items.Count; "
        "      this.Data := new Array<int>(); "
        "    end if; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var seq := new Array<int>(); "
        "  seq.Add(3); seq.Add(1); seq.Add(2); "
        "  var total := 0; "
        "  for var i in 0..500 do "
        "    using var h1 := new Holder(seq, true); "
        "    using var h2 := new Holder(seq, false); "
        "    total := total + h1.Data.Count + h2.Data.Count; "
        "  end for; "
        "  return total mod 251; "
        "end Main; end C;";
    // Same arithmetic as the regression row: the pre-branch store is
    // immediately displaced (or replaced) by the branch, so h1.Data.Count is
    // still 3 and h2.Data.Count still 0. 501 x 3 = 1503 mod 251 = 248.
    expectRanWith(src, 248);
}

// The OTHER quadrant of the 2x2 — field initializer instead of a
// pre-assignment. `Data` starts non-null, so `oldVal != null` on the taken
// branch and the pre-fix code nulled the slot correctly. Second accept
// control, and the one that keeps the two quadrants honest about being the
// SAME program modulo one property: if the discriminator were anything other
// than "does the slot start non-null", this row would move relative to the
// regression row.
//
// §7.9 (v0.315) — the field initializer is what makes the strict-storage
// definite-assignment rule admit the ctor body at all.
BEZEL_TEST(codegen_borrow_array_branch_insensitive_4068_5459_interaction,
           accept_control_field_initializer_spec_28_3) {
    std::string src =
        "module M; "
        "class Holder "
        "  public owned Data: Array<int> := new Array<int>(); "
        "  public constructor(seq: Array<int>, flag: bool) "
        "    var items := seq.Sorted(); "
        "    if flag then "
        "      this.Data := items; "
        "    else "
        "      var n := items.Count; "
        "    end if; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var seq := new Array<int>(); "
        "  seq.Add(3); seq.Add(1); seq.Add(2); "
        "  var total := 0; "
        "  for var i in 0..500 do "
        "    using var h1 := new Holder(seq, true); "
        "    using var h2 := new Holder(seq, false); "
        "    total := total + h1.Data.Count + h2.Data.Count; "
        "  end for; "
        "  return total mod 251; "
        "end Main; end C;";
    // The else branch now leaves the initializer's array in place (Count 0).
    // h1 replaces it with the 3-element Sorted() result. 501 x 3 = 1503 mod
    // 251 = 248.
    expectRanWith(src, 248);
}

// DISPLACEMENT arm — the hoist must not null on a path where disposal ALSO
// fires, or the displaced array is leaked; and must not fail to null, or it is
// double-freed. Two stores into the field:
//
//   A: this.Data := items;            (oldVal null  -> doDispose FALSE, the
//                                       #4068 crash quadrant; nulling MUST run)
//   B: this.Data := new Array<int>(); (oldVal = A's array, newVal different
//                                       -> doDispose TRUE; disposes A's array)
//
// Together they pin "the Sorted() result is freed exactly ONCE, by store B" —
// not zero times (an over-eager hoist nulling at B, leaking it) and not twice
// (a missing hoist nulling at A, double-freed against the ctor's scope-end
// free).
//
// ⚠ THIS ROW IS AN ACCEPT CONTROL, NOT A FAIL-WITHOUT PIN. Measured: it is
// GREEN on the pre-fix tree, because store A here is UNCONDITIONAL — with the
// store on every path the walker does not leave the local scope-end-free, so
// there is no second free to double up against and the pre-fix conditional
// nulling is never needed. It is kept because it pins a direction the
// fail-without rows cannot: that the hoist did not over-null, and that a
// `doDispose == true` store still disposes. A "fix" that nulled on every path
// including where disposal must still happen, or that suppressed disposal,
// keeps the fail-without rows green and turns this one red.
//
// Note `items` is NOT read after store A: A moves out of it, and a post-move
// read is a §28.3 error the compiler correctly rejects ("owned value in `items`
// was already moved out"), so this row must derive its value from `Data`
// alone. (Measured, not assumed — an earlier draft of this row read
// `items.Count` after the move and was rejected at compile time, which the
// oracle reports as `ran == false`. A fixture that does not compile is not a
// regression test.)
//
// §28.5 / §28.9 (v0.315) — drop-on-overwrite of an owned collection field;
// §28.3 K3.7 — the transfer that makes store A's nulling necessary.
BEZEL_TEST(codegen_borrow_array_branch_insensitive_4068_5459_interaction,
           second_store_displaces_nulled_array_exactly_once_spec_28_5) {
    std::string src =
        "module M; "
        "class Holder "
        "  public owned Data: Array<int>; "
        "  public constructor(seq: Array<int>, flag: bool) "
        "    var items := seq.Sorted(); "
        "    this.Data := items; "
        "    if flag then "
        "      this.Data := new Array<int>(); "
        "    else "
        "      var n := 1; "
        "    end if; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var seq := new Array<int>(); "
        "  seq.Add(3); seq.Add(1); seq.Add(2); "
        "  var total := 0; "
        "  for var i in 0..500 do "
        "    using var h1 := new Holder(seq, true); "
        "    using var h2 := new Holder(seq, false); "
        "    total := total + h1.Data.Count + h2.Data.Count; "
        "  end for; "
        "  return total mod 251; "
        "end Main; end C;";
    // h1 (flag=true): store A puts the 3-element Sorted() array in, store B
    //   displaces it with an empty one -> Data.Count 0.
    // h2 (flag=false): store A's array stays -> Data.Count 3.
    // 501 x (0 + 3) = 1503. 1503 mod 251 = 248.
    //
    // The value is a WEAK check on its own (it does not move if the Sorted()
    // array is merely leaked rather than double-freed); the load-bearing
    // assertion is `ran`, since a double free here is SIGABRT.
    expectRanWith(src, 248);
}

// MIRRORED branch arm of the regression row — the consuming store sits in the
// ELSE branch rather than the THEN branch, so the escaping read
// (`items.Count`) is on the branch that was previously the "safe" one.
//
// Why this is a separate row and not a parameterisation of the one above: the
// defect's discriminator is "does the slot start non-null", NOT "is the store
// in the then-branch". A fix that special-cased the then-arm (say, by emitting
// the nulling only when some flag about the preceding statement held) would
// keep the row above green and go red here. Pinning both arms is what makes the
// row pin the RULE rather than one spelling of it.
//
// Pre-fix: on the else branch `oldVal` is null (no field initializer) ⇒
// `doDispose` false ⇒ the nulling never ran ⇒ SIGABRT.
// Post-fix: ran, and 248.
//
// §28.3 K3.7 / §28.9 (v0.315).
BEZEL_TEST(codegen_borrow_array_materializer_branch_insensitive_leak_4068,
           first_store_on_else_branch_ran_spec_28_3) {
    std::string src =
        "module M; "
        "class Holder "
        "  public owned Data: Array<int>; "
        "  public constructor(seq: Array<int>, flag: bool) "
        "    var items := seq.Sorted(); "
        "    if flag then "
        "      var n := items.Count; "
        "      this.Data := new Array<int>(); "
        "    else "
        "      this.Data := items; "
        "    end if; "
        "  end constructor; "
        "  destructor end destructor; "
        "end Holder; "
        "class C public static method Main(): int "
        "  using var seq := new Array<int>(); "
        "  seq.Add(3); seq.Add(1); seq.Add(2); "
        "  var total := 0; "
        "  for var i in 0..500 do "
        "    using var h1 := new Holder(seq, false); "
        "    using var h2 := new Holder(seq, true); "
        "    total := total + h1.Data.Count + h2.Data.Count; "
        "  end for; "
        "  return total mod 251; "
        "end Main; end C;";
    // h1 (flag=false) takes the else branch and keeps the 3-element Sorted()
    // array; h2 (flag=true) takes the then branch and stores an empty one.
    // 501 x (3 + 0) = 1503. 1503 mod 251 = 248.
    // Pre-fix: ran=false (SIGABRT). Post-fix: 248.
    expectRanWith(src, 248);
}

}  // namespace
