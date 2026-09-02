// Regression tests for #3603 / #3539 (spec §29.7 / §27.5 / §28.9, v0.286): a
// disposal whose slot is a §27.5 LAMBDA-CAPTURED local must be reached through
// `boxedStorage`, not by loading the slot raw.
//
// WHAT THE BUG ACTUALLY WAS (it is not what #3603 says). §27.5 (#808) rebinds a
// lambda-captured local's storage to a `ptr`-alloca holding a shared heap CELL;
// the value lives INSIDE the cell. `Emitter::boxedStorage`
// (LlvmBackend_Layout.cpp) is the ONE authority that answers "given a slot,
// where does the value actually live" — it unboxes a `cellVars_` slot and
// returns the slot itself, emitting no instruction, for every other slot.
// Every arm of `emitDisposals` asks it… except one: the §29.7 `iterEnumDispose`
// arm (LlvmBackend_Disposal.cpp) loaded `it->slot` RAW. That is a second,
// drifting spelling of a question an existing authority already answers, and it
// is wrong exactly when the slot is a capture cell: the arm then took the CELL
// POINTER for the object and dispatched vtable slot 1 out of the cell header —
// a wild jump.
//
// MEASURED on r12-cycle-1 (`af4f73b`), with `--probe --asan`, for
// `var seq: IEnumerable<Item> := _a;` captured by `lambda() => seq.Count()`:
//
//     AddressSanitizer: SEGV on unknown address 0x…  (pc == the address)
//     Hint: PC is at a non-executable region. Maybe a wild jump?
//     SUMMARY: AddressSanitizer: SEGV (<unknown module>)
//
// on ALL FOUR of {`var seq`, `using var seq`} × {abandoned, fully drained} —
// i.e. the crash is NOT specific to abandonment at all. It fired in
// `bz$iter$M.Src$Walk$MoveNext`, on MoveNext's own scope exit, with #3468's
// abandonment-path exclusion fully in force. #3603 describes the symptom as a
// 38400 B leak; that measurement is from `907ce0f` and no longer reproduces —
// on this tree the same programs die before they can leak.
//
// THE FIX, at two sites, both of them "ask the existing authority":
//   1. LlvmBackend_Disposal.cpp — the `iterEnumDispose` arm now calls
//      `boxedStorage(it->slot, ety)` like its siblings, and stores the
//      post-disposal null through that same address. For a non-captured slot
//      `boxedStorage` emits nothing, so that IR is byte-for-byte unchanged
//      (verified by a whole-module IR diff of the repro: only the functions
//      holding a captured iterator-format local differ).
//   2. LlvmBackend_Iterators.cpp — the Dispose/CloneDispose "restore
//      suspend-locals from the frame" loop registers a captured local's
//      restored holder in `cellVars_`, so the SAME authority unboxes it. The
//      holder is first made non-null (a `select` onto a copy of the frame's
//      §27.5 pre-box field, `frame.localPreBoxFields`, reserved by
//      `buildStateMachineFrame` for exactly this node) so the "capture site
//      never ran" state #3484 exists for cannot become a null dereference.
//      That loop existed TWICE, byte-identical — once for the sync iterator
//      and once for the async one (LlvmBackend_Async.cpp) — so it is now ONE
//      `Emitter::restoreFrameLocalIntoSlot` with two call sites. Both crashed
//      identically before the unification (the async row below SEGVs with only
//      the sync half repaired), which is why the async rows are here.
// With the slot-1 dispatch sound, #3468's narrow exclusion in
// LlvmBackend_Stmts.cpp (which kept the abandonment disposal deliberately
// disabled rather than arm a wild jump) is retired.
//
// #3539 is the same defect reached from the ordinary `using var` path (its
// reported symptom is a `heap-buffer-overflow in bz$iter$Gen$CloneDispose`),
// so the last two rows pin a captured `using var` over a plain class.
//
// ORACLE. Every row runs under `runAotChecked` — generated-code ASan plus
// LeakSanitizer — because the two halves of this defect need different sides
// of that oracle:
//   * The FULL-DRAIN rows must be completely clean. Pre-fix they CRASH (the
//     drain edge is MoveNext's own scope exit, where the wild jump fires), so
//     they are the SEGV half's fail-without.
//   * The ABANDONED rows cannot be clean while #3541's filter stands: an
//     abandoned enumeration still leaks the §27.5 closure (40 B), its env
//     (16 B) and the cell (8 B). Asserting "clean" would be a lie; asserting
//     nothing would be vacuous, and MEASURED so — an earlier revision of this
//     file ran them under `runAotCheckedLenient` (LSan off) and three of them
//     PASSED on the pre-fix compiler, because the pre-fix behaviour of an
//     abandoned enumeration is a SILENT LEAK (#3468's exclusion dropped the
//     disposal entirely) and LeakSanitizer is the only oracle that can see it.
//     So each abandoned row pins the residue EXACTLY: `in 600 allocation(s)`,
//     three per round. Pre-fix it is FOUR per round — the iterator-format
//     wrapper the §29.7 disposal owes is in the set — so each row is a real
//     fail-without for the LEAK half too.
//     ⚠ If #3541 lands, these programs go LSan-CLEAN and every abandoned row
//     fails on `EXPECT_FALSE(r.ran)`. That is intended: retire the residue
//     assertion and assert cleanliness instead.
//   * WHAT EACH ORACLE ACTUALLY COVERS — stated precisely, because it is easy
//     to overclaim here. The printed sum is computed inside MoveNext, BEFORE
//     any Dispose runs, so it cannot witness a wrong Dispose-side unbox. What
//     it does witness is the MoveNext-side half: the body yields `x.V + f()`
//     where `f` reads the captured `seq` THROUGH the §27.5 cell, so a wrong
//     read there changes the number rather than merely crashing. The
//     Dispose-side half is covered by the other three oracles instead —
//     generated-code ASan (the wild jump / heap-buffer-overflow faults at the
//     access), the EXACT residue count (`in 600 allocation(s)`: a dropped or
//     mis-aimed §29.7 disposal moves it, in either direction), and, in
//     #3539's own repro, `Cell.n` — bumped by the constructor and dropped by
//     the destructor, so its printed `0` is a dispose-exactly-once statement
//     about the abandonment Dispose itself over all 200 rounds.
//     Each row asserts its FULL stdout, not a substring, so a value cannot
//     pass by being a prefix of a wrong one.
//   * Every program amplifies to 200 rounds so no leak can hide as a
//     still-stack-reachable single allocation.
//
// Spec authority: §29.7 *Disposal of partially-consumed iterators*, §27.5
// *Closures*, §28.9 *Runtime-managed types* (v0.286).
// Related: #3539, #3541, #3468, #3500, #3484, #808, #1462.

#include <iostream>
#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// Print the harness diagnostics (compile reject, ASan/LSan report, …) so a
// failing row shows WHY rather than only "assertion failed: r.ran".
void dumpDiagnostics(const JitResult& r) {
    for (const auto& d : r.diagnostics.diagnostics())
        std::cout << "    [diag] " << d.message << "\n";
}

// ── the #3603 shape: an ITERATOR-FORMAT local captured by a lambda ───────────
// `seq` is an `IEnumerable<Item>` bound to the owning array — an
// iterator-format value whose scope-end disposal is an `iterEnumDispose`
// (vtable slot 1), NOT a class-table dispatch. `f` captures it, which is what
// makes §27.5 rebind `seq`'s storage onto a heap cell and widen its frame field
// to pointer width. `f()` is CALLED in the yielded expression, so the value on
// stdout depends on reading the captured `seq` correctly.
//
// `useUsingSeq` switches `var` ⇄ `using var`: the snapshot entry becomes an
// ordinary `Disposal` instead of an `iterEnumDispose`, reaching the identical
// mis-typed slot by the other path.
//
// `abandon` puts the `break` in the CONSUMER's loop, so `Walk`'s MoveNext never
// reaches its own scope exit and §29.7's abandonment Dispose is the only thing
// that can run the pending cleanup. (The knob is compile-time per row: the
// pre-fix revision of this file had it as dead code and only ever exercised
// one side.)
std::string program(bool useUsingSeq, bool abandon) {
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
        + std::string(useUsingSeq
            ? "    using var seq: IEnumerable<Item> := _a;\n"
            : "    var seq: IEnumerable<Item> := _a;\n") +
        "    var f: () -> int := lambda() => seq.Count();\n"
        "    for var x in _a do\n"
        "      yield return x.V + f();\n"
        "    end for;\n"
        "  end Walk;\n"
        "end Src;\n"
        "class Main\n"
        "  public static method Run(): int\n"
        "    using var s := new Src();\n"
        "    var t := 0;\n"
        "    for var v in s.Walk() do\n"
        "      t := t * 100 + v;\n"
        + std::string(abandon ? "      break;\n" : std::string()) +
        "    end for;\n"
        "    return t;\n"
        "  end Run;\n"
        "  public static method Main(): int\n"
        "    var t := 0;\n"
        "    var i := 0;\n"
        "    while i < 200 do\n"
        "      t := t + Run();\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(\"t=\" + t.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Main;\n";
}

// `seq.Count()` is 2, so the two yields are 3+2=5 and 4+2=6.
//   abandoned  : Run() == 5              -> 200 * 5   = 1000
//   drained    : Run() == 5*100 + 6 = 506 -> 200 * 506 = 101200
const char* kAbandonedOut = "t=1000\n";
const char* kDrainedOut = "t=101200\n";

// ── the #3539 shape: a plain `using var` CLASS local captured by a lambda ────
// No iterator-format value anywhere; the captured local's disposal is the
// ordinary class dispatch. It reaches the same restore loop and the same
// cell-typed slot, which is why #3539's reported symptom is a
// `heap-buffer-overflow in bz$iter$Gen$CloneDispose`.
std::string usingVarCaptureProgram(bool abandon) {
    return
        "module M;\n"
        "class R\n"
        "  public V: int;\n"
        "  public constructor(v: int) V := v; end constructor;\n"
        "end R;\n"
        "class Src\n"
        "  public method Gen(): IEnumerable<int>\n"
        "    using var r := new R(7);\n"
        "    var f: () -> int := lambda() => r.V;\n"
        "    yield return f();\n"
        "    yield return 2;\n"
        "  end Gen;\n"
        "end Src;\n"
        "class Main\n"
        "  public static method Run(): int\n"
        "    using var s := new Src();\n"
        "    var t := 0;\n"
        "    for var v in s.Gen() do\n"
        "      t := t + v;\n"
        + std::string(abandon ? "      break;\n" : std::string()) +
        "    end for;\n"
        "    return t;\n"
        "  end Run;\n"
        "  public static method Main(): int\n"
        "    var t := 0;\n"
        "    var i := 0;\n"
        "    while i < 200 do\n"
        "      t := t + Run();\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(\"t=\" + t.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Main;\n";
}

// abandoned: Run() == 7        -> 200 * 7 = 1400
// drained  : Run() == 7 + 2    -> 200 * 9 = 1800
const char* kCaptureAbandonedOut = "t=1400\n";
const char* kCaptureDrainedOut = "t=1800\n";


// ── the ASYNC mirror of the #3603 shape ─────────────────────────────────────
// `emitAsyncIteratorMethod`'s disposeFn carried a byte-identical copy of the
// sync restore loop. Measured: with ONLY the sync half repaired this program
// is still `AddressSanitizer: SEGV (<unknown module>)`; with both call sites
// on the shared `restoreFrameLocalIntoSlot` it is correct. The FULL-DRAIN
// async row was already clean at that midpoint, because the drain path goes
// through the `boxedStorage` repair in `emitDisposals` rather than through the
// Dispose-side restore — so this pair is what makes the async half
// non-vacuous.
std::string asyncProgram(bool abandon) {
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
        "  public async method Walk(): IAsyncEnumerable<int>\n"
        "    var seq: IEnumerable<Item> := _a;\n"
        "    var f: () -> int := lambda() => seq.Count();\n"
        "    for var x in _a do\n"
        "      yield return x.V + f();\n"
        "    end for;\n"
        "  end Walk;\n"
        "end Src;\n"
        "class Main\n"
        "  public static async method Main(): Task<int>\n"
        "    var t := 0;\n"
        "    var i := 0;\n"
        "    while i < 200 do\n"
        "      using var s := new Src();\n"
        "      var r := 0;\n"
        "      await for var v in s.Walk() do\n"
        "        r := r * 100 + v;\n"
        + std::string(abandon ? "        break;\n" : std::string()) +
        "      end for;\n"
        "      t := t + r;\n"
        "      i := i + 1;\n"
        "    end while;\n"
        "    Console.WriteLine(\"t=\" + t.ToString());\n"
        "    return 0;\n"
        "  end Main;\n"
        "end Main;\n";
}

// ── #3539's own repro, verbatim from the issue ──────────────────────────────
// Its oracle is stronger than a value: `Cell.n` is incremented by the
// constructor and decremented by the destructor, so the second printed line is
// 0 IFF the captured `using var` was disposed exactly once on every one of the
// 200 abandoned rounds. Pre-fix this program is
// `heap-buffer-overflow ... in bz$iter$Gen$CloneDispose`.
const char* kIssue3539Repro =
    "module M; import System; import System.Collections.Generic;\n"
    "class Cell\n"
    "  public static n: int := 0;\n"
    "  public V: int := 0;\n"
    "  public constructor(v: int) Cell.n := Cell.n + 1; V := v; end constructor;\n"
    "  destructor Cell.n := Cell.n - 1; end destructor;\n"
    "end Cell;\n"
    "class C\n"
    "  public static method Gen(v: int): IEnumerable<int>\n"
    "    using var c := new Cell(7);\n"
    "    var f := lambda(): int => c.V;\n"
    "    yield return f();\n"
    "    yield return f();\n"
    "  end Gen;\n"
    "  public static method Main(): int\n"
    "    var acc := 0;\n"
    "    for var j in 0..<200 do\n"
    "      for var x in C.Gen(3) do acc := acc + x; break; end;\n"
    "    end;\n"
    "    Console.WriteLine(acc.ToString());\n"
    "    Console.WriteLine(Cell.n.ToString());\n"
    "    return 0;\n"
    "  end Main;\n"
    "end C;\n";

// `expectStdout` is the program's COMPLETE stdout, compared for equality. A
// `find()` here would be a weaker pin than it looks: every expected value is a
// prefix of a wrong one an off-by-a-round or an extra-iteration bug produces
// (`t=1000` inside `t=10000`, `t=1400` inside `t=14000`).
void expectStdoutExactly(const JitResult& r, const char* expectStdout) {
    if (r.stdoutText != expectStdout)
        std::cout << "    [stdout] " << r.stdoutText;
    BEZEL_EXPECT_TRUE(r.stdoutText == expectStdout);
}

void expectRanCleanly(const JitResult& r, const char* expectOut) {
    if (!r.ran) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    expectStdoutExactly(r, expectOut);
}

// The abandoned-path oracle: the program computes the right answer, and what
// LeakSanitizer reports is EXACTLY #3541's residue and nothing of this issue's
// — see the ORACLE note in the file header for why this is an assertion and
// not a `Lenient` shrug. A pre-fix CRASH (the wild jump, or #3539's
// heap-buffer-overflow) also lands in this shape, with a report that names
// neither the value nor an allocation count, so it fails all three ways.
void expectValueAndOnlyThe3541Residue(const JitResult& r,
                                      const char* expectOut,
                                      const char* expectAllocs) {
    expectStdoutExactly(r, expectOut);
    BEZEL_EXPECT_FALSE(r.ran);   // LeakSanitizer fired on the #3541 residue
    std::string report;
    for (const auto& d : r.diagnostics.diagnostics()) report += d.message;
    const bool onlyResidue = report.find(expectAllocs) != std::string::npos;
    if (!onlyResidue) dumpDiagnostics(r);
    BEZEL_EXPECT_TRUE(onlyResidue);
}

// Three §27.5 allocations per abandoned round over 200 rounds.
const char* kResidue3541 = "in 600 allocation(s)";

}  // namespace

// ── #3603: `var seq: IEnumerable<T>` captured by a lambda ────────────────────
// Pre-fix (r12-cycle-1): ASan SEGV, wild jump through the cell header, on BOTH
// rows. Post-fix: the slot-1 dispatch lands on the iterator-format object and
// the printed sum — which reads the captured `seq` through `f()` — is right.
BEZEL_TEST(iter_lambda_captured_iter_format_local_unbox_3603,
           var_seq_captured_by_lambda_abandoned_29_7) {
    JitResult r = runAotChecked(program(/*useUsingSeq=*/false,
                                        /*abandon=*/true));
    expectValueAndOnlyThe3541Residue(r, kAbandonedOut, kResidue3541);
}

BEZEL_TEST(iter_lambda_captured_iter_format_local_unbox_3603,
           var_seq_captured_by_lambda_fully_drained_29_7) {
    JitResult r = runAotChecked(program(/*useUsingSeq=*/false,
                                        /*abandon=*/false));
    expectRanCleanly(r, kDrainedOut);
}

// ── #3603, the `using var` spelling of the same iterator-format local ────────
// The snapshot entry is an ordinary `Disposal` rather than an
// `iterEnumDispose`, so #3468's exclusion never applied to it — and it crashed
// identically, which is the evidence that the exclusion was never the fix.
BEZEL_TEST(iter_lambda_captured_iter_format_local_unbox_3603,
           using_var_seq_captured_by_lambda_abandoned_29_7) {
    JitResult r = runAotChecked(program(/*useUsingSeq=*/true,
                                        /*abandon=*/true));
    expectValueAndOnlyThe3541Residue(r, kAbandonedOut, kResidue3541);
}

BEZEL_TEST(iter_lambda_captured_iter_format_local_unbox_3603,
           using_var_seq_captured_by_lambda_fully_drained_29_7) {
    JitResult r = runAotChecked(program(/*useUsingSeq=*/true,
                                        /*abandon=*/false));
    expectRanCleanly(r, kDrainedOut);
}

// ── #3539: a captured `using var` over a plain class, no iterator-format value
BEZEL_TEST(iter_lambda_captured_iter_format_local_unbox_3603,
           using_var_class_local_captured_by_lambda_abandoned_3539) {
    JitResult r = runAotChecked(usingVarCaptureProgram(/*abandon=*/true));
    expectValueAndOnlyThe3541Residue(r, kCaptureAbandonedOut, kResidue3541);
}

BEZEL_TEST(iter_lambda_captured_iter_format_local_unbox_3603,
           using_var_class_local_captured_by_lambda_fully_drained_3539) {
    JitResult r =
        runAotChecked(usingVarCaptureProgram(/*abandon=*/false));
    expectRanCleanly(r, kCaptureDrainedOut);
}


// ── the ASYNC iterator half of the same defect ──────────────────────────────
BEZEL_TEST(iter_lambda_captured_iter_format_local_unbox_3603,
           async_var_seq_captured_by_lambda_abandoned_29_7) {
    JitResult r = runAotChecked(asyncProgram(/*abandon=*/true));
    expectValueAndOnlyThe3541Residue(r, kAbandonedOut, kResidue3541);
}

BEZEL_TEST(iter_lambda_captured_iter_format_local_unbox_3603,
           async_var_seq_captured_by_lambda_fully_drained_29_7) {
    JitResult r = runAotChecked(asyncProgram(/*abandon=*/false));
    expectRanCleanly(r, kDrainedOut);
}

// ── issue 3539's own repro, with its dispose-exactly-once oracle ───────────
// LeakSanitizer is ON here, like every other row: the residue is pinned rather
// than tolerated (11200 B in the same 600 allocations — `Cell` 24 B + closure
// 40 B + env 16 B per round; the `Cell` is in the set here and the
// iterator-format wrapper is not, because this shape has no iterator-format
// local at all). On top of that, `Cell.n == 0` is a dispose-exactly-once
// oracle in its own right: it would print 200 if the disposal were dropped,
// and the program would have crashed if the disposal had run over the cell.
BEZEL_TEST(iter_lambda_captured_iter_format_local_unbox_3603,
           issue_3539_repro_disposes_captured_using_var_exactly_once_27_5) {
    JitResult r = runAotChecked(kIssue3539Repro);
    // "1400" = 200 rounds x the single yielded 7; "0" = every Cell destructed,
    // i.e. the captured `using var` was disposed exactly once on every one of
    // the 200 abandoned rounds. Pre-fix: heap-buffer-overflow in CloneDispose.
    expectValueAndOnlyThe3541Residue(r, "1400\n0\n", kResidue3541);
}

// ── the #3541 residue, stated once for the reader ───────────────────────────
// #3500's filter in LlvmBackend_Stmts.cpp deliberately keeps `closureRelease`,
// `boxedCellFree`, `boxedCellFreeDeferred` and `inlineOwnedArrayCell` OUT of
// the §29.7 abandonment snapshot, so an abandoned iterator holding a lambda
// leaks exactly three allocations per round: the closure (40 B), its env
// (16 B) and the §27.5 cell (8 B). Removing that filter ON TOP OF this fix was
// MEASURED in the lane that wrote this file and is still an ASan SEGV in
// `CloneDispose` (a NULL dereference, a third and distinct defect), so #3541 is
// NOT unblocked by #3539's repair and the filter stays. Every abandoned row
// above pins that residue at `in 600 allocation(s)`; before this fix it was
// 800, because the iterator-format wrapper the §29.7 disposal owes was in the
// set too — 96 B/round, which is #3603's own "38400 B in 1600 allocations over
// 400 abandonments" scaled.
