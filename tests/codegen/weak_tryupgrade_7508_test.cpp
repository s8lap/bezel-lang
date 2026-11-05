// Codegen regression — issue #7508: `Weak<T>.TryUpgrade()` must be the
// stdlib §2.16 Try shape-(b) NULLABLE RETURN `TryUpgrade(): owned
// Shared<T>?` — not a `bool`.
//
// WHAT THE DOCUMENTS REQUIRE.
//
// stdlib §2.16 `Shared<T>` and `Weak<T>` (docs/Bezel_Standard_Library.md,
// v0.355) — the DECLARING line:
//
//   public class Weak<T>: IDisposable where T: class
//     public method TryUpgrade(): owned Shared<T>?;              // a new strong
//                                                               //   handle if the
//                                                               //   value is still
//                                                               //   alive; null
//                                                               //   otherwise
//                                                               //   (§1 Try shape (b))
//     // Does not keep the underlying value alive. Dispose() releases the observer.
//   end Weak;
//
// stdlib §2.16's usage note (v0.355): "`TryUpgrade` follows Try shape (b) (§1
// — null alone is the signal: `Shared<T>` is a proper class type at every
// instantiation, so the handle itself carries the outcome; the
// `Mutex.TryAcquire` acquisition shape): `using var s := w.TryUpgrade(); if s
// <> null then ... end;` — on non-null the caller owns a fresh handle,
// discharged by the `using var`; a null binding's scope-end disposal is a
// no-op."
//
// spec §28.13 (docs/Bezel_Language_Spec.md, v0.355): "`TryUpgrade(): owned
// Shared<T>?` atomically produces a new strong handle if the value is still
// alive, and `null` otherwise — the strong count reaches zero before the
// value's disposal begins, so a `TryUpgrade()` that runs during that
// disposal, from the value's own destructor or from another disposer of the
// same pass, answers `null` (Try shape (b), stdlib §1 — the handle is a
// proper class type at every instantiation, so null alone carries the
// signal; `using var` + null test at the call site, the `Mutex.TryAcquire`
// idiom)."
//
// stdlib §1's Try-shape table (v0.355) names the member in the shape-(b)
// row: "acquisition-shaped members whose payload is statically non-nullable
// at every instantiation: `Mutex.TryAcquire` (simpler call sites), `Weak<T>
// .TryUpgrade` (a bool would carry no information)".
//
// WHAT WAS WRONG (MEASURED on r30-c1, the base of this lane).
//
// The compiler implemented the shape-(a) spelling — an `out owned` parameter
// plus a `bool` result — so the stdlib's own idiom did not type at all:
//
//   18:7: warning: `using var` `s` is vacuous — the RHS is a value-typed/
//        runtime-managed value with no disposal obligation (§28.9) …
//   20:27: no member `Get` on primitive type `bool` …
//
// (the issue #7508 repro, verbatim — row (A) below).
//
// THE ROWS.
//
//  (A) The issue's own repro, VERBATIM, under the CHECKED oracle
//      (runAotChecked): it must compile with no diagnostic, print "a" then
//      "gone", and never fault.  The plain (unsanitized) arm pins the same
//      output and the same exit code (Sweep returns the live count, 1).
//  (B) The 50-ITERATION refcount-balance loop under the checked oracle: the
//      Shared strong/weak counts must balance — the Cell is disposed EXACTLY
//      ONCE per iteration, the counter returns to 0, and there is no leak and
//      no double release.  (A one-shot repro is not a reliable leak oracle;
//      the loop is load-bearing.)
//  (C) The RETIRED `out` spelling must be REJECTED with a diagnostic that
//      names the shape-(b) signature: `TryUpgrade` takes no parameters.
//
// ORACLE NOTES.  `runAotChecked` drives the real AOT path with
// `--sanitize=address` on the GENERATED code; `checkedToolchainSkipped` is
// asserted false so a toolchain skip cannot turn these rows green (the
// skip-absorbing-guard trap).

#include <string>

#include "support/aot_support.hpp"        // runAot / runAotChecked
#include "support/aot_result_asserts.hpp"  // checkedToolchainSkipped
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// ---------------------------------------------------------------------------
// (A) The issue #7508 repro, VERBATIM.
//
// EXPECT (spec §28.13 / stdlib §2.16): compiles; no report line; prints "a"
// then "gone"; never faults in either build mode.  Sweep returns the number
// of live items — 1 (only "a" is alive) — so the exit code is 1.
// ---------------------------------------------------------------------------
const char* kIssueRepro =
    "module Probe; "
    "public class Item "
    "  private _name: string; "
    "  public constructor(name: string) "
    "    _name := name; "
    "  end constructor; "
    "  public property Name: string => _name; "
    "end Item; "
    "public class Registry "
    "  private owned _items: OwningArray<Weak<Item>> := "
    "    new OwningArray<Weak<Item>>(); "
    "  public constructor() "
    "  end constructor; "
    "  public method Register(owned w: Weak<Item>) => _items.Add(w); "
    "  public method Sweep(): int "
    "    var live := 0; "
    "    for var w in _items do "
    "      using var s := w.TryUpgrade(); "
    "      if s <> null then "
    "        Console.WriteLine(s.Get().Name); "
    "        live := live + 1; "
    "      else "
    "        Console.WriteLine(\"gone\"); "
    "      end; "
    "    end; "
    "    return live; "
    "  end Sweep; "
    "end Registry; "
    "public class Program "
    "  public static method Main(): int "
    "    using var reg := new Registry(); "
    "    using var a := new Shared<Item>(new Item(\"a\")); "
    "    var b := new Shared<Item>(new Item(\"b\")); "
    "    reg.Register(a.Downgrade()); "
    "    reg.Register(b.Downgrade()); "
    "    b.Dispose(); "
    "    return reg.Sweep(); "
    "  end Main; "
    "end Program;";

// ---------------------------------------------------------------------------
// (B) The 50-iteration refcount-balance loop.
//
// Every iteration mints a Shared<Cell>, downgrades it, and upgrades the weak
// handle back.  The upgrade's handle is a NEW strong handle: if it is
// released exactly once, the strong count returns to 1 and the LAST release
// (the original handle) disposes the Cell exactly once — so `alive` returns
// to 0 after 50 iterations.  A double release would drive the count negative
// and free the control block twice (ASan: heap-use-after-free / double-free);
// a missing release would leave `alive` at 50 (a leak).
//
// The loop is load-bearing: a one-shot repro is not a reliable leak oracle.
//
// The row READS THE VALUE THROUGH THE UPGRADED HANDLE (`up.Get().V`), not
// only the handle's null-ness: on the pre-fix compiler `w.TryUpgrade()` was
// typed `bool`, so a row that only counted non-null handles would compile and
// print the same numbers on the BASE tree (MEASURED: the pre-fix program
// degraded to a bool and still printed "50\n0\n" — a row that cannot fail).
// The member read is what makes the row a shape-(b) program, and it also
// proves the upgraded handle really points at the value.
// ---------------------------------------------------------------------------
const char* kRefcountLoop50 =
    "module M; "
    "class Cell "
    "  public static alive: int := 0; "
    "  public V: int := 7; "
    "  public constructor() Cell.alive := Cell.alive + 1; end constructor; "
    "  destructor Cell.alive := Cell.alive - 1; end destructor; "
    "end Cell; "
    "class C "
    "  public static method Main(): int "
    "    var n := 0; "
    "    var sum := 0; "
    "    for var i in 0..49 do "
    "      do using var sh := new Shared<Cell>(new Cell()); "
    "        using var w := sh.Downgrade(); "
    "        using var up := w.TryUpgrade(); "
    "        if up <> null then n := n + 1; sum := sum + up.Get().V; end; "
    "      end; "
    "    end; "
    "    Console.WriteLine(n); "           // 50 upgrades succeeded
    "    Console.WriteLine(sum); "        // 350 — the value read through the handle
    "    Console.WriteLine(Cell.alive); " // 0 — every Cell disposed exactly once
    "    return 0; "
    "  end Main; "
    "end C;";

// ---------------------------------------------------------------------------
// (C) The retired `out` spelling.
// ---------------------------------------------------------------------------
const char* kRetiredOutSpelling =
    "module M; "
    "class Obj public Val: int := 0; end Obj; "
    "class C "
    "  public static method Main(): int "
    "    var obj := new Obj(); "
    "    using var sh := new Shared<Obj>(move obj); "
    "    using var w := sh.Downgrade(); "
    "    using var up: Shared<Obj>? := null; "
    "    var ok := w.TryUpgrade(out up); "
    "    return 0; "
    "  end Main; "
    "end C;";

}  // namespace

// ---------------------------------------------------------------------------
// (A) the issue's own repro — the CHECKED (ASan/LSan) arm.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_weak_tryupgrade_7508,
           issue_repro_checked_28_13_stdlib_2_16_v0355) {
    JitResult r = runAotChecked(kIssueRepro);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);          // Sweep's live count
    BEZEL_EXPECT_EQ(r.stdoutText, "a\ngone\n");
}

// ---------------------------------------------------------------------------
// (A) the issue's own repro — the PLAIN arm (same output, same exit code).
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_weak_tryupgrade_7508,
           issue_repro_plain_28_13_stdlib_2_16_v0355) {
    JitResult r = runAot(kIssueRepro);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
    BEZEL_EXPECT_EQ(r.stdoutText, "a\ngone\n");
}

// ---------------------------------------------------------------------------
// (B) the 50-iteration refcount-balance loop — the CHECKED arm.  This is the
// row that catches a leak or a double release: `alive` must be 0 and the
// sanitizer must be silent.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_weak_tryupgrade_7508,
           refcount_balances_over_50_iterations_checked_28_13_v0355) {
    JitResult r = runAotChecked(kRefcountLoop50);
    BEZEL_EXPECT_FALSE(bezel::test::checkedToolchainSkipped(r));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "50\n350\n0\n");
}

// ---------------------------------------------------------------------------
// (B) the same loop — the PLAIN arm.  Pins the VALUES (50 upgrades, 0 live
// cells) so a sanitizer-only oracle cannot hide a wrong count.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_weak_tryupgrade_7508,
           refcount_balances_over_50_iterations_plain_28_13_v0355) {
    JitResult r = runAot(kRefcountLoop50);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "50\n350\n0\n");
}

// ---------------------------------------------------------------------------
// (C) the retired `out` spelling is rejected, and the diagnostic names the
// shape-(b) signature so a user can find the right spelling.
// ---------------------------------------------------------------------------
BEZEL_TEST(codegen_weak_tryupgrade_7508,
           retired_out_spelling_is_rejected_28_13_stdlib_2_16_v0355) {
    JitResult r = runAot(kRetiredOutSpelling);
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool cited = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("TryUpgrade") != std::string::npos &&
            d.message.find("owned Shared<T>?") != std::string::npos)
            cited = true;
    BEZEL_EXPECT_TRUE(cited);
}
