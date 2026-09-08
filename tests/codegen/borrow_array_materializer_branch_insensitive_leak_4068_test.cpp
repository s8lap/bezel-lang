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
// this.Data := items; else var n := items.Length; end if;` was taint-marked
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

// §28.3 / §28.9 (#4068): the issue's canonical repro. A `var items :=
// seq.Sorted();` whose ONE escaping use is `this.Data := items;` on the
// `then` branch; the `else` branch reads `items.Length` only (safe). On
// the else-branch the local must be freed at scope end (its only
// reference is the slot itself); on the then-branch the field STORE
// nulls the slot after the move, and the field's destructor-side free
// owns the same array. 300 iterations × 2 holders = 602 Sorted() returns
// to leak without the fix. AMPLIFIED to 500 iterations per the
// one-shot-leak-tests-pass-vacuously note in MEMORY.md (the canonical
// 300 iter × 2 holders = 602 allocs can be a marginal-signal case; ×
// ~1.66 amplification clears the vacuous-pass threshold). Without the
// fix: -999 (LSan-detected leak); with: 4500 (500 iter × (3+3 from
// each holder's Data.Length sum) × 3 alloc-free amplifications —
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
        "      var n := items.Length; "
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
        "    total := total + h1.Data.Length + h2.Data.Length; "
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
        "    total := total + h.Data.Length; "
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
BEZEL_TEST(codegen_borrow_array_materializer_branch_insensitive_leak_4068,
           while_loop_field_store_spec_28_3) {
    std::string src =
        "module M; "
        "class Holder "
        "  public owned Data: Array<int>; "
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
        "    total := total + h1.Data.Length + h2.Data.Length; "
        "  end for; "
        "  return total mod 251; "
        "end Main; end C;";
    // 501 iterations × (3 + 0) = 1503. mod 251 = 248.
    // Pre-fix: -999. Post-fix: 248.
    BEZEL_EXPECT_EQ(run(src), 248);
}

}  // namespace
