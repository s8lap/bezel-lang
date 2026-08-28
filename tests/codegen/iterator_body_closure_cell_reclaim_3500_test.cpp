// Codegen — issue #3500 part (a) (§27.5 Closures, §28.9 Runtime-managed types,
// §29.3 Iterator methods, spec v0.283): a SYNC ITERATOR body ran with NONE of
// the §27.5 closure-and-capture reclaim analyses established for it, so a
// `var f := lambda …` in an iterator body leaked its closure cell, its env
// block, and every capture cell, once per activation of the iterator.
//
//     public static method Gen(v: int): IEnumerable<int>
//       var f := lambda(): int => v;
//       yield return f();
//       yield return v;
//       v := v + 5;
//       yield return f();
//     end Gen;
//
//     $ bezelc --sanitize=address -o p p.bzl && ./p
//     14
//     SUMMARY: AddressSanitizer: 52 byte(s) leaked in 3 allocation(s)
//
// 52 / 3 is exactly the closure cell (40) + its env block (8) + the `int`
// capture cell (4) for ONE `Gen` activation — one of each of the three things
// these analyses reclaim.
//
// ── THE MECHANISM, and why it is a family rather than a one-off. §29.3 says
// "The compiler translates the body into the equivalent state-machine
// implementation of `IEnumerator<T>`", so an iterator body does NOT lower into
// the function `emitFunctionBody` was called with — it lowers into the
// generated `MoveNext`. Every scope-end reclaim family in this backend is keyed
// to the function its pre-scan analyzed (`currentFn_ == <family>Fn_`), and
// `emitFunctionBody` routes a `yield`-bearing body to `emitIteratorMethod`
// BEFORE it runs its own pre-scan block — so inside `MoveNext` those tokens
// still named whatever function was compiled last, and every gate was false.
//
// That gap has been closed one family at a time: #1897 for the unwind
// disposals, #2148 / #2142 / #2143 for the borrowing-collection literals and
// materializers, #2817 for the `var` string locals. §27.5's closure release and
// the capture-cell frees were the ones still missing, and #3500 is their turn.
// The fix is the same four lines each of the others used — run the pre-scan
// over the iterator body and pin the token to `moveNextFn` — and it is the
// sync-iterator twin of what #1416 / #1418 established for an ASYNC method's
// resume fn. No new predicate: the authorities (`collectClosureReleaseEligible`,
// `collectBoxCaptureLocals`, `collectDefinitelyEscapingLambdas`,
// `collectCellFreeEligible`) already compute the facts; they simply were not
// being asked about this body.
//
// ── WHY IT IS SOUND HERE. The activation model is #2817's, unchanged: a
// MoveNext resume runs once per advance, so a scope's disposals run on the
// advance that leaves the scope — one bind, one release.
//
// ⚠ ABANDONMENT IS **NOT** COVERED, AND THAT IS DELIBERATE — it is the half of
// this commit that took longest to get right, and it is worth stating because
// the obvious version of this fix is wrong in the worst direction. The yield
// handler snapshots every live Disposal that is neither `caughtException` nor
// `unwindOnly` and whose slot is in `locals_` into `smIterCleanupAtState_`, and
// `Dispose` runs that stack in reverse. Both newly-registrable kinds qualify on
// their face — a `closureRelease` is keyed to the closure local's own slot, and
// a `boxedCellFree` to the cell HOLDER, which `lowerLambda` rebinds into
// `locals_[decl]` — and an earlier revision of this commit let them in. The
// snapshot's exclusivity argument ("`Dispose` stamps the DISPOSED sentinel while
// an exhausted iterator parks at -1") covers abandonment against exhaustion but
// says NOTHING about a body that THROWS after a yield, where that stack runs IN
// ADDITION to what the throw edge already did. With the two frees enrolled, the
// throwing row below went from a leak to a
// `heap-use-after-free … READ of size 8 … in bz$lam$0$envdtor` — A LEAK
// CONVERTED INTO A USE-AFTER-FREE, measured, on a shape no test in the tree
// covered.
//
// That double-run is PRE-EXISTING and is not this issue's to fix: on `ef339fa`,
// with none of this commit present, the same iterator shape over a plain
// `using var` local is a heap-use-after-free and over a `string` local is a
// SEGV. It is filed separately. So the reclaim arms only where the dispose stack
// is run exactly once — MoveNext's own scope exit, which is the run-to-
// exhaustion path this issue's repro takes and, as it turns out, the throw edge
// too — and `LlvmBackend_Stmts.cpp`'s snapshot skips the two new kinds. An
// ABANDONED enumeration therefore keeps the pre-existing leak, unchanged; the
// parity row below pins exactly that, and the throwing row pins the other side.
//
// ── WHAT THIS DOES NOT FIX. #3500 part (b) — a closure produced by an
// if-EXPRESSION arm rather than bound directly by a `var` — is a DIFFERENT
// mechanism (`collectClosureReleaseEligible`'s candidate scan admits only a
// LocalDecl whose initializer IS the `+1` producer, and a ConditionalExpr is
// not) and is filed separately, as issue 3518. It reproduces identically before
// and after this commit — 11600 B / 700 allocations over 200 calls, both sides.
//
// FAIL-WITHOUT / PASS-WITH, measured program-by-program with
// `bezelc --sanitize=address` (LeakSanitizer on), one build of each compiler:
//
//   program                                        pre-fix (ef339fa)  post-fix
//   #3500(a) exact repro, ONE enumeration          52 B / 3           clean
//   the same, x300 enumerations                    15600 B / 900      clean
//   `string` closure + capture in an iterator x200 11200 B / 600      clean
//   `using var` + capture in an iterator, x200     13600 B / 800      clean
//   nested iterators, each with a closure, x200    20800 B / 1200     clean
//   an instance iterator storing the closure in a
//     field of its own receiver, x200                800 B / 200      clean
//   a body that THROWS after a yield, x200         10400 B / 600      clean
//   a captured move-only [2 owned] array in a
//     body that THROWS after a yield, x50         4800 B / 250       clean
//   PARITY: abandoned enumeration (break), x200    10400 B / 600      10400 B / 600
//
// ⚠ THESE LEAK ROWS NEED THE ASan/LSan ORACLE SPECIFICALLY (`runAotChecked`, not
// `runAot`): the effect under test is memory nobody frees, which a plain build
// reports as a perfectly correct answer. Do not "simplify" a checkedOut row here
// to plainOut — that silently disarms it. Every leak row is AMPLIFIED except the
// issue's own one-shot repro, which is kept because 52 B / 3 was the filed
// measurement and it is a real, non-vacuous LSan report at x1; the x300 twin
// beside it is what makes the row immune to a one-shot reachability accident.
// per §27.5, §28.9, §29.3 (issue #3500)

#include "support/aot_support.hpp"   // runAotChecked — ASan/LSan AOT harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {
// runAotChecked runs the program under generated-code ASan with LeakSanitizer
// on, so a non-empty leak report fails the row before the value is compared.
std::string checkedOut(const std::string& prog) {
    auto r = runAotChecked(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
// Plain runAot for the parity row, whose shape still leaks by design.
std::string plainOut(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.stdoutText : std::string("<did-not-run>");
}
}  // namespace

// ── The issue's exact repro. One enumeration of a 3-yield generator whose body
// binds one lambda over the method's parameter. The value 14 is the §27.5
// sharing across the yields (3 + 3 + 8) and must stay right; pre-fix the program
// printed 14 and leaked 52 B / 3 allocations.
// per §27.5, §28.9, §29.3 (issue #3500)
BEZEL_TEST(iterator_body_closure_cell_reclaim_3500,
           sync_iterator_closure_and_capture_cell_reclaimed_spec_27_5_29_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class C "
        "  public static method Gen(v: int): IEnumerable<int> "
        "    var f := lambda(): int => v; "
        "    yield return f(); "
        "    yield return v; "
        "    v := v + 5; "
        "    yield return f(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var x in C.Gen(3) do acc := acc + x; end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "14\n");
}

// ── LOOP-AMPLIFIED twin of the row above: 300 full enumerations, so the pre-fix
// report is 15600 B / 900 allocations rather than 52 B / 3. This is the row that
// no one-shot reachability accident can explain. Printed total: 300 x 14 = 4200.
// per §27.5, §28.9, §29.3 (issue #3500)
BEZEL_TEST(iterator_body_closure_cell_reclaim_3500,
           sync_iterator_reclaim_amplified_300_spec_27_5_29_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class C "
        "  public static method Gen(v: int): IEnumerable<int> "
        "    var f := lambda(): int => v; "
        "    yield return f(); "
        "    yield return v; "
        "    v := v + 5; "
        "    yield return f(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<300 do "
        "      for var x in C.Gen(3) do acc := acc + x; end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "4200\n");
}

// ── A `string` capture rather than an `int` one. The envdtor of a captured
// string ALSO releases the cell's CONTENTS, so this row is what distinguishes a
// fix that armed the reclaim correctly from one that armed it without keeping
// the content release ordered before the cell free — the latter is an
// over-release (a heap-use-after-free on the string), which ASan reports as an
// ERROR rather than as a leak. Printed total: 200 x (4 + 2 + 4) = 2000.
// per §27.5, §28.9, §29.3 (issue #3500)
BEZEL_TEST(iterator_body_closure_cell_reclaim_3500,
           sync_iterator_string_closure_reclaimed_amplified_200_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class C "
        "  public static method Gen(p: string): IEnumerable<string> "
        "    var f := lambda(): string => p + \"-x\"; "
        "    yield return f(); "
        "    yield return p; "
        "    yield return f(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var n := 0; "
        "    for var j in 0..<200 do "
        "      for var s in C.Gen(\"ab\") do n := n + s.Length; end; "
        "    end; "
        "    Console.WriteLine(n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "2000\n");
}

// ── A `using var` OWNED local in the iterator body, captured by the lambda.
// This is the shape `consumeLastLambdaCapturedCells`'s #966 / #1017 guard exists
// for: the local's own disposal READS THROUGH the cell, so the cell free must be
// DEFERRED behind it rather than run in LIFO order. Newly reachable in an
// iterator body only because this commit arms the free at all, so the row is
// here to prove the deferral holds in `MoveNext` too — an un-deferred free is a
// use-after-free at the `Dispose`, not a leak. Printed total:
// 200 x ((7 + 3) + 3 + (7 + 3)) = 4600.
// per §27.5, §28.9, §29.3 (issue #3500)
BEZEL_TEST(iterator_body_closure_cell_reclaim_3500,
           sync_iterator_using_var_capture_deferred_free_amplified_200_spec_27_5_28_9) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class R "
        "  public V: int := 7; "
        "end R; "
        "class C "
        "  public static method Gen(v: int): IEnumerable<int> "
        "    using var r := new R(); "
        "    var f := lambda(): int => r.V + v; "
        "    yield return f(); "
        "    yield return v; "
        "    yield return f(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      for var x in C.Gen(3) do acc := acc + x; end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "4600\n");
}

// ── NESTED iterators, each with its own closure local. Two MoveNext bodies are
// live at once and each pins the four tokens to ITS OWN `moveNextFn` while it is
// being EMITTED — the pinning is a compile-time property, not a runtime one, so
// the row's real job is to prove that emitting one iterator inside the emission
// of another does not leave the outer one's sets pointing at the inner one's
// function. Printed total: 200 x ((6 + 4) + (3 + 4)) = 3400.
// per §27.5, §28.9, §29.3 (issue #3500)
BEZEL_TEST(iterator_body_closure_cell_reclaim_3500,
           nested_sync_iterators_each_reclaimed_amplified_200_spec_27_5_29_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class C "
        "  public static method Inner(v: int): IEnumerable<int> "
        "    var f := lambda(): int => v * 2; "
        "    yield return f(); "
        "    yield return v; "
        "  end Inner; "
        "  public static method Outer(n: int): IEnumerable<int> "
        "    var g := lambda(): int => n + 1; "
        "    for var y in C.Inner(n) do "
        "      yield return y + g(); "
        "    end; "
        "  end Outer; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      for var x in C.Outer(3) do acc := acc + x; end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "3400\n");
}

// ── PARITY row: an ABANDONED enumeration (`break` after the first element), so
// the state machine exits through `Dispose` rather than through exhaustion. It
// is a plain VALUE row on purpose: this shape deliberately keeps the
// PRE-EXISTING leak (10400 B / 600 allocations, IDENTICAL before and after this
// commit), because arming a free on the Dispose path is exactly what turned the
// throwing row below into a use-after-free — see the ⚠ block in the header. An
// LSan row here would enshrine a residue this commit chose not to touch.
//
// What it DOES assert is that abandonment still WORKS: `Dispose` runs the
// snapshotted cleanup stack, which this commit filters, and a filter that
// removed too much (an owner disposal, an enumerator frame, a `finally`) would
// show up here as a wrong number or an ASan error rather than as silence.
// Printed total: 200 x 3 (first element only).
// per §27.5, §28.9, §29.3 (issue #3500)
BEZEL_TEST(iterator_body_closure_cell_reclaim_3500,
           parity_abandoned_enumeration_disposes_cleanly_spec_27_5_29_3) {
    BEZEL_EXPECT_EQ(plainOut(
        "module M; import System; import System.Collections.Generic; "
        "class C "
        "  public static method Gen(v: int): IEnumerable<int> "
        "    var f := lambda(): int => v; "
        "    yield return f(); "
        "    yield return v; "
        "    v := v + 5; "
        "    yield return f(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      for var x in C.Gen(3) do "
        "        acc := acc + x; "
        "        if x = 3 then break; end if; "
        "      end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "600\n");
}

// ── THE ROW THAT PINS THE NARROWING. A body that THROWS after a yield. With the
// closure release and the cell free enrolled in the abandonment snapshot (the
// obvious version of this fix), this program was a
// `heap-use-after-free ... READ of size 8 ... in bz$lam$0$envdtor` — freed once
// on the throw edge, read again by the iterator's CloneDispose. With them
// excluded it is CLEAN, and better than `ef339fa`, which leaked 10400 B / 600
// allocations here: the throw edge already runs MoveNext's own scope disposals
// and was only ever missing the eligibility this commit establishes.
//
// ⚠ THIS ROW NEEDS THE ASan ORACLE AND MUST NOT BE WEAKENED TO A VALUE CHECK.
// The double-free version printed the CORRECT total (800) on a plain build; only
// ASan saw it. That is #2633's two-oracle asymmetry in its most expensive form —
// the failure this row exists to catch is invisible to `runAot`.
// Printed total: 200 x (3 for the first element + 1 for the catch) = 800.
// per §27.5, §28.9, §29.3 (issue #3500)
BEZEL_TEST(iterator_body_closure_cell_reclaim_3500,
           throwing_iterator_body_does_not_double_free_the_closure_spec_27_5_29_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class C "
        "  public static method Gen(v: int): IEnumerable<int> "
        "    var f := lambda(): int => v; "
        "    yield return f(); "
        "    if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "    yield return f(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      try "
        "        for var x in C.Gen(3) do acc := acc + x; end; "
        "      catch e: InvalidOperationException => "
        "        acc := acc + 1; "
        "      end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "800\n");
}

// ── An INSTANCE iterator whose body stores the lambda in a field of its own
// receiver, so the closure genuinely OUTLIVES the enumeration and is dropped by
// the receiver's disposal instead. `collectDefinitelyEscapingLambdas` is one of
// the four walkers this commit newly runs over an iterator body, and this is the
// shape that exercises it: the row reads the capture back through the cell AFTER
// the for-in has exhausted the iterator (`h.F()` on the line after), which an
// over-eager envdtor free would turn into an ASan error or a wrong number.
// Pre-fix 800 B / 200 allocations; clean after.
// Printed total: 200 x (3 + 3 + 4 + 4) = 2800.
// per §27.5, §28.9, §29.3 (issue #3500)
BEZEL_TEST(iterator_body_closure_cell_reclaim_3500,
           iterator_field_store_escape_readable_after_exhaustion_spec_27_5_29_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class H "
        "  public F: () -> int := lambda(): int => 0; "
        "  public method Gen(v: int): IEnumerable<int> "
        "    var t := 0; "
        "    if v > 0 then t := v; end if; "
        "    this.F := lambda(): int => t; "
        "    yield return t; "
        "    yield return this.F(); "
        "    t := t + 1; "
        "    yield return this.F(); "
        "  end Gen; "
        "end H; "
        "class C "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<200 do "
        "      using var h := new H(); "
        "      for var x in h.Gen(3) do acc := acc + x; end; "
        "      acc := acc + h.F(); "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "2800\n");
}

// ── THE SAME NARROWING, FOR THE FOURTH DISPOSAL KIND. `consumeLastLambdaCaptured
// Cells` registers FOUR kinds, not three: beside the `boxedCellFree` /
// `boxedCellFreeDeferred` it pushes an `inlineOwnedArrayCell` whenever the
// captured local is a move-only OWNING inline array (#782), and that entry
// disposes the owned ELEMENTS **through the very cell** the `boxedCellFree`
// beside it frees. The first narrowing of this commit filtered three kinds and
// left this one in the abandonment snapshot, which is the identical
// use-after-free one kind over: on a body that throws after a yield, MoveNext's
// throw edge disposes the elements and frees the cell, and `CloneDispose` then
// replays the snapshot against that freed cell —
// `heap-use-after-free ... in bz$iter$Gen$CloneDispose`.
//
// This row is the pin for the completeness of that filter list. `Cell.n` is a
// static instance counter, so the SECOND printed line is a destructor-balance
// oracle independent of the leak check: `0` means every one of the 100 `Cell`s
// this program constructs was disposed exactly once.
//
//   compiler                             result
//   ef339fa base                         100 / 100, leak 4800 B / 250
//   this commit's three-kind narrowing   heap-use-after-free
//   this commit as shipped               100 / 0, clean
//
// The base prints `100` for `Cell.n`, i.e. it disposed NONE of them — so the
// four-kind filter is also strictly better than the baseline on this shape, not
// merely non-crashing.
//
// ⚠ ASan ORACLE REQUIRED. The three-kind version printed the correct `100` on a
// plain build; only ASan saw the double-dispose. Do not weaken this row.
// per §27.5, §28.9, §29.3, §15.3 (issue #3500)
BEZEL_TEST(iterator_body_closure_cell_reclaim_3500,
           throwing_iterator_inline_owned_array_capture_not_double_disposed_spec_27_5_15_3) {
    BEZEL_EXPECT_EQ(checkedOut(
        "module M; import System; import System.Collections.Generic; "
        "class Cell "
        "  public static n: int := 0; "
        "  public V: int := 0; "
        "  public constructor(v: int) Cell.n := Cell.n + 1; V := v; end constructor; "
        "  destructor Cell.n := Cell.n - 1; end destructor; "
        "end Cell; "
        "class C "
        "  public static method Gen(v: int): IEnumerable<int> "
        "    var a: [2 owned]Cell := [new Cell(1), new Cell(2)]; "
        "    var f := lambda(): int => a[0].V; "
        "    yield return f(); "
        "    if v > 2 then throw new InvalidOperationException(\"boom\"); end if; "
        "    yield return f(); "
        "  end Gen; "
        "  public static method Main(): int "
        "    var acc := 0; "
        "    for var j in 0..<50 do "
        "      try "
        "        for var x in C.Gen(3) do acc := acc + x; end; "
        "      catch e: InvalidOperationException => "
        "        acc := acc + 1; "
        "      end; "
        "    end; "
        "    Console.WriteLine(acc.ToString()); "
        "    Console.WriteLine(Cell.n.ToString()); "
        "    return 0; "
        "  end Main; "
        "end C;"),
        "100\n0\n");
}
