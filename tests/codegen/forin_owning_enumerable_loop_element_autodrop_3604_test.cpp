// Regression tests for #3604 (spec §29.7 / §29.2 / §28.3, v0.286): the
// per-iteration auto-drop of an `IOwningEnumerable<T>` `for var x in seq`
// loop variable, on the §29.7 ABANDONMENT edge.
//
// §29.2 says the iteration variable `x` is a plain owned local (§28.3) and the
// §28.3 owned-local discharge rule applies on EVERY body-exit edge (fall-through
// to the next iteration, `break`, `continue`, `return`, `throw`): on each normal-
// exit edge `x` is either discharged by the program or auto-dropped with the
// must-use warning. §29.7 says an abandoned iterator's Dispose runs the pending
// cleanup actions active at the point of suspension "in reverse order of
// establishment, exactly as if the body had exited at the suspension point".
// That covers the auto-drop on the §29.7 edge.
//
// `lowerIEnumerableBuiltinFor` registers the auto-drop as a null-guarded
// `unwindOnly` disposal — and the §29.7 yield snapshot has skipped every
// `unwindOnly` entry by design (its exemption predates #3604). On every other
// path (endBB / break / throw / return) the disposal fires correctly: each of
// those paths is an `isUnwind=true` `emitDisposals`, which is what an
// `unwindOnly` branch handles. The §29.7 edge is the one path where the
// snapshot's exemption is wrong for this specific disposal.
//
// Two changes are load-bearing:
//
//   (a) the §29.7 snapshot admits the entry via a new `forInAutoDrop` flag
//       (every other consumer reads zero of the flag — the snapshot is the
//       only one — so endBB / break / throw / return are byte-for-byte
//       unchanged);
//
//   (b) the loop variable's slot is wired into the §29.7 frame field keyed on
//       the ForStmt node (`smAllocaToField_[varSlot] = smLocalFields_[&fs]`),
//       the same wiring the drain-for-in arm (~line 7323) and the await-for
//       arm (~line 9006) already make for their loops. WITHOUT (b), the
//       frame field is reserved but never WRITTEN, so the iterator's Dispose
//       loads garbage into its restored `locals_[&fs]` slot and the
//       disposal's null-guard skips — exactly the 6400 B / 400 leak #3604
//       measures.
//
// ORACLE. The leaked object is a compiler-synthesised heap wrapper around
// `Item`, so no user destructor counts it: LeakSanitizer is the oracle
// (`runAotChecked`), and each program AMPLIFIES the abandonment to 400 rounds
// (the loop-amplified case in the issue body) so the leak cannot hide as a
// still-stack-reachable single allocation. Every case also asserts the
// computed value on stdout, so a program that stopped producing the right
// answer is not silently accepted as "clean".
//
// Spec authority: §29.7 *Disposal of partially-consumed iterators*, §29.2
// *Desugaring of `for`*, §28.3 *Local variables, ownership, and borrows*,
// v0.286. Related: #3468 (enumerator/source frames), #944, #895.

#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

// The exact program the issue pins: an `IOwningEnumerable<T>` for-in whose
// body does NOT explicitly discharge the loop variable (the §29.2/§28.3
// auto-drop warning fires), inside a yielding iterator. The consumer's outer
// `for` takes one element and `break`s, abandoning the iterator INSIDE the
// inner for-in's body (a `yield return x.V`) — the §29.7 abandonment edge
// the snapshot must replay the auto-drop on.
//
// `Run` is repeated 400 times so an abandonment leak accumulates well past the
// "still reachable from the stack" one-shot false negative — exactly the
// amplification the issue documents as 6400 B / 400 alloc.
std::string abandonProgram() {
    return
        "module M;\n"
        "class Item\n"
        "  public V: int;\n"
        "  public constructor(v: int) V := v; end constructor;\n"
        "end Item;\n"
        "class Src\n"
        "  private owned _a: OwningArray<Item>;\n"
        "  public constructor()\n"
        "    _a := new OwningArray<Item>();\n"
        "    _a.Add(new Item(3));\n"
        "    _a.Add(new Item(4));\n"
        "  end constructor;\n"
        "  public method Walk(): IEnumerable<int>\n"
        "    var seq: IOwningEnumerable<Item> := _a;\n"
        "    for var x in seq do\n"
        "      yield return x.V;\n"
        "    end for;\n"
        "  end Walk;\n"
        "end Src;\n"
        "class Main\n"
        "  public static method Run(): int\n"
        "    using var s := new Src();\n"
        "    var t := 0;\n"
        "    for var v in s.Walk() do\n"
        "      t := t * 10 + v;\n"
        "      break;\n"
        "    end for;\n"
        "    return t;\n"
        "  end Run;\n"
        "  public static method Main(): int\n"
        "    var t := 0;\n"
        "    var i := 0;\n"
        "    while i < 400 do\n"
        "      t := t + Run();\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(\"t=\" + t.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Main;\n";
}

// `using var u := owned x;` is the warning-free discharge — and because it is
// itself a §29.7 cleanup action, the element is reclaimed on abandonment
// regardless of #3604. This is the WORKAROUND the issue records as clean
// (6400 B → 0); pinning it here as a control proves the loop-amplified shape
// is not a leak path on its own — only the un-discharged-body shape is.
std::string workaroundProgram() {
    return
        "module M;\n"
        "class Item\n"
        "  public V: int;\n"
        "  public constructor(v: int) V := v; end constructor;\n"
        "end Item;\n"
        "class Src\n"
        "  private owned _a: OwningArray<Item>;\n"
        "  public constructor()\n"
        "    _a := new OwningArray<Item>();\n"
        "    _a.Add(new Item(3));\n"
        "    _a.Add(new Item(4));\n"
        "  end constructor;\n"
        "  public method Walk(): IEnumerable<int>\n"
        "    var seq: IOwningEnumerable<Item> := _a;\n"
        "    for var x in seq do\n"
        "      using var u := owned x;\n"
        "      yield return u.V;\n"
        "    end for;\n"
        "  end Walk;\n"
        "end Src;\n"
        "class Main\n"
        "  public static method Run(): int\n"
        "    using var s := new Src();\n"
        "    var t := 0;\n"
        "    for var v in s.Walk() do\n"
        "      t := t * 10 + v;\n"
        "      break;\n"
        "    end for;\n"
        "    return t;\n"
        "  end Run;\n"
        "  public static method Main(): int\n"
        "    var t := 0;\n"
        "    var i := 0;\n"
        "    while i < 400 do\n"
        "      t := t + Run();\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(\"t=\" + t.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Main;\n";
}

// The same for-in in a NON-iterator body — `for var x in seq do break; end` —
// where the loop variable's auto-drop already fires on the break path via
// endBB + `emitDisposals(isUnwind=true)` (which the existing `unwindOnly` arm
// already handles). Pinned as the regression: this case must stay clean (it
// was clean before #3604 and is clean after — neither change moves the
// break-path disposal).
std::string nonIteratorBreakProgram() {
    return
        "module M;\n"
        "class Item\n"
        "  public V: int;\n"
        "  public constructor(v: int) V := v; end constructor;\n"
        "end Item;\n"
        "class Main\n"
        "  public static method Run(): int\n"
        "    var arr := new OwningArray<Item>();\n"
        "    arr.Add(new Item(1));\n"
        "    arr.Add(new Item(2));\n"
        "    var seq: IOwningEnumerable<Item> := arr;\n"
        "    for var x in seq do\n"
        "      break;\n"
        "    end for;\n"
        "    return 0;\n"
        "  end Run;\n"
        "  public static method Main(): int\n"
        "    var t := 0;\n"
        "    var i := 0;\n"
        "    while i < 400 do\n"
        "      t := t + Run();\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(\"t=\" + t.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Main;\n";
}

// Every round takes only the first element (consumer `break`s), so
// t = 400 * 3 = 1200.
const char* kAbandonedOut = "t=1200";

void expectClean(const JitResult& r, const char* expectOut) {
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_TRUE(r.stdoutText.find(expectOut) != std::string::npos);
}

}  // namespace

// --- the bug: abandonment of an un-discharged `IOwningEnumerable` loop variable -
// Pre-fix: 6400 B leaked in 400 allocations (the issue's exact measurement
// on HEAD `d2957f8`). Post-fix: 0 leaked.
BEZEL_TEST(forin_owning_enumerable_loop_element_autodrop_3604,
           abandoned_undischarged_loop_element_disposed_on_29_7) {
    JitResult r = runAotChecked(abandonProgram());
    if (wasSkipped(r)) return;
    expectClean(r, kAbandonedOut);
}

// --- the workaround: explicit discharge is warning-free AND clean on 29.7 ---
// Pinning this control so a future change that "fixes" the un-discharged shape
// by silently demoting it to the workaround (loud → quiet, #R10 §6c) is
// detected: the warning is gone here, the un-discharged row above MUST still
// warn — both checked.
BEZEL_TEST(forin_owning_enumerable_loop_element_autodrop_3604,
           abandoned_explicit_discharge_clean_29_7) {
    JitResult r = runAotChecked(workaroundProgram());
    if (wasSkipped(r)) return;
    expectClean(r, kAbandonedOut);
    // No warning on `x` (the body discharged it explicitly via `using var u :=
    // owned x;`).
    for (const auto& d : r.diagnostics.diagnostics()) {
        BEZEL_EXPECT_TRUE(d.message.find("implicitly disposed at the end of "
                                         "each loop iteration") ==
                          std::string::npos);
    }
}

// --- control: the non-iterator break path is unchanged by this fix -----------
// The loop variable's auto-drop already fires on break via endBB (the existing
// `unwindOnly` arm handles it). Pinned here as the false-positive guard: if a
// future change accidentally widens the snapshot carve to a shape that
// pre-existing code covers, this test would double-dispose and ASan would
// report it.
BEZEL_TEST(forin_owning_enumerable_loop_element_autodrop_3604,
           non_iterator_break_keeps_autodrop_on_29_2) {
    JitResult r = runAotChecked(nonIteratorBreakProgram());
    if (wasSkipped(r)) return;
    expectClean(r, "t=0");
}