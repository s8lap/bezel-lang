// Codegen / disposal — an explicit `GetOwningEnumerator()` on a FRESHLY
// PRODUCED (temporary) IOwningEnumerable<T> source must not leak the source.
// Issue #1641. Spec: §29.2 (desugaring of `for`), §29.1 (owning enumerator /
// TakeCurrent), §28.3 (owned-local discharge).
//
// THE BUG (#1641). The explicit `src.GetOwningEnumerator()` codegen path
// (LlvmBackend_Calls2.cpp) ALWAYS dispatched vtable slot 4 to produce a fresh
// owning-enumerator CLONE, for every receiver. For a NAMED source that is
// correct: the clone BORROWS the source's owned captures (cloneDisposeFn skips
// them) and the named source disposes them once at its own scope end. But for a
// TEMPORARY source (`Gen().GetOwningEnumerator()`) there is no binding that owns
// the source, so the source frame (`bz$C$Gen`'s calloc'd struct, plus its owned
// captures) was NEVER disposed and LEAKED — the clone's cloneDisposeFn frees only
// the clone frame, never the source. (The parallel explicit-GetEnumerator path
// got exactly this named/temporary split in #1265; GetOwningEnumerator was
// missed — filed as #1641.)
//
// THE FIX. Split on whether the receiver is a NAMED value or a fresh producer
// (CallExpr/NewExpr). For a TEMPORARY, return the source ITSELF (do not clone):
// a temporary is single-use and fresh (state 0), so driving it is
// observationally identical to driving a clone, and the source struct's own
// vtable slot 1 is the FULL Dispose (disposeFn, disposeOwned=true), so the
// caller's `using var e` disposes the source — captures AND frame — exactly once
// (iterEnumDispose, registerUsingDisposal) AFTER driving. This mirrors §29.2's
// for-loop desugaring, which binds `using var __source := source;` for a fresh
// iterable source and thereby "closes the leak that would arise from
// `for var x in GetThings() do ... end`". It mirrors the #1265 explicit-
// GetEnumerator temporary handling exactly.
//
// SOUNDNESS (never trade a leak for a UAF). Cloning-then-disposing the source
// would be a USE-AFTER-FREE: the slot-4 clone is a shallow memcpy that SHARES
// the source's owned captures (GetEnumerator, LlvmBackend_Iterators.cpp), so
// disposing the source before the clone is driven frees captures the clone still
// reads. Returning the source itself avoids both — there is no separate clone.
//
// OBSERVABILITY. runAotChecked instruments the generated code with ASan/LSan, so
// the leaked source frame + captured Cell flip `r.ran` to false WITHOUT the fix,
// and a UAF (had we naively disposed a still-borrowed clone's captures) would do
// the same. As a positive observable, the temporary source's owned Cell(1)
// capture has a destructor that prints "CELL-DISPOSED-1": with the fix it appears
// exactly once (the source's full Dispose cascades into the capture); without the
// fix it never appears (the source leaked). The named-source control confirms the
// clone path is un-regressed (borrowed capture disposed once by the source, never
// double-disposed).

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAot / runAotChecked — AOT + ASan/LSan
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;
using bezel::test::runAotChecked;

namespace {

// No toolchain-SKIP guard here: a box without the C toolchain must FAIL
// these rows loudly, not pass them vacuously (fix #3733's toolchain-SKIP
// burn-down — the checker's remedy is to delete the guard, not to return
// on it).

// Count non-overlapping occurrences of `needle` in `hay`.
int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// Shared decls: a disposal-marking owned capture `Cell`, a value-carrying `Box`,
// and an owning iterator that CAPTURES an owned Cell and yields Boxes derived
// from it. The capture makes the source-frame leak observable positively (the
// captured Cell's destructor) as well as via LSan.
const std::string kDecls =
    "class Cell : IDisposable "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor Console.WriteLine(\"CELL-DISPOSED-\" + Id.ToString()); "
    "  end destructor; "
    "end Cell; "
    "class Box "
    "  public v: int := 0; "
    "  public constructor(n: int) v := n; end constructor; "
    "end Box; "
    "class C "
    "  public static method Gen(owned cell: Cell): IOwningEnumerable<Box> "
    "    yield return new Box(cell.Id); "
    "    yield return new Box(cell.Id + 1); "
    "  end Gen; "
    "end C; ";

// TEMPORARY source: `Gen(new Cell(1)).GetOwningEnumerator()` — the receiver is a
// fresh producer with no persistent owner. WITHOUT the fix the source frame + its
// captured Cell(1) leak. Sums the two yielded Box values: 1 + 2 = 3.
const std::string kTemporarySource =
    "module M; " + kDecls +
    "class Main2 "
    "  public static method Main(): int "
    "    using var e := C.Gen(new Cell(1)).GetOwningEnumerator(); "
    "    var s := 0; "
    "    while e.MoveNext() do "
    "      using var x := e.TakeCurrent(); "
    "      s := s + x.v; "
    "    end; "
    "    return s; "
    "  end Main; "
    "end Main2;";

// NAMED source control: `using var g := Gen(...); g.GetOwningEnumerator()`. The
// enumerator clone BORROWS g's captured Cell; g disposes it once at its own scope
// end. Must NOT double-dispose (this is the clone path the fix preserves).
const std::string kNamedSource =
    "module M; " + kDecls +
    "class Main2 "
    "  public static method Main(): int "
    "    using var g := C.Gen(new Cell(1)); "
    "    using var e := g.GetOwningEnumerator(); "
    "    var s := 0; "
    "    while e.MoveNext() do "
    "      using var x := e.TakeCurrent(); "
    "      s := s + x.v; "
    "    end; "
    "    return s; "
    "  end Main; "
    "end Main2;";

// A captureless temporary source pins the exact frame leak from the issue (the
// bare `bz$C$Gen` state-struct calloc). WITHOUT the fix LSan reports the frame.
const std::string kTemporaryCaptureless =
    "module M; "
    "class Box public v: int := 0; "
    "  public constructor(n: int) v := n; end constructor; end Box; "
    "class C public static method Gen(): IOwningEnumerable<Box> "
    "  yield return new Box(1); yield return new Box(2); yield return new Box(3); "
    "end Gen; end C; "
    "class Main2 public static method Main(): int "
    "  using var e := C.Gen().GetOwningEnumerator(); "
    "  var s := 0; "
    "  while e.MoveNext() do "
    "    using var x := e.TakeCurrent(); "
    "    s := s + x.v; "
    "  end; "
    "  return s; "     // 1 + 2 + 3 = 6
    "end Main; end Main2;";

}  // namespace

// =====================================================================
// #1641 — explicit GetOwningEnumerator() on a TEMPORARY source disposes the
// source exactly once (no leak), and its owned capture with it (§29.2, §28.3).
// =====================================================================

// KEY regression: the temporary source + its owned Cell capture must be disposed
// exactly once. WITHOUT the fix the source frame + Cell(1) leak → r.ran false and
// "CELL-DISPOSED-1" never appears. WITH the fix the run is ASan/LSan-clean and
// the capture is disposed exactly once (also excludes the cloning-UAF direction:
// a use-after-free on the shared capture would flip r.ran too).
BEZEL_TEST(owning_enumerator_temp_source_1641,
           temporary_source_no_leak_disposes_capture_spec_29_2) {
    JitResult r = runAotChecked(kTemporarySource);
    BEZEL_EXPECT_TRUE(r.ran);            // LSan-clean: source frame + capture freed
    BEZEL_EXPECT_EQ(r.exitCode, 3);      // 1 + 2
    // Load-bearing: the temporary source's captured Cell(1) WAS disposed, exactly
    // once, by disposing the returned owning enumerator. Absent the fix: 0.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "CELL-DISPOSED-1"), 1);
}

// The bare frame leak from the issue (captureless temporary): LSan reports the
// leaked bz$C$Gen state struct WITHOUT the fix (r.ran false); clean WITH it.
BEZEL_TEST(owning_enumerator_temp_source_1641,
           temporary_source_captureless_no_frame_leak_spec_29_2) {
    JitResult r = runAotChecked(kTemporaryCaptureless);
    BEZEL_EXPECT_TRUE(r.ran);            // no leaked source frame
    BEZEL_EXPECT_EQ(r.exitCode, 6);      // 1 + 2 + 3
}

// Value correctness of the temporary path (non-ASan): the enumerator drives the
// (returned-as-itself) source and yields the full sequence.
BEZEL_TEST(owning_enumerator_temp_source_1641,
           temporary_source_values_correct_spec_29_1) {
    auto r = runAot(kTemporaryCaptureless);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 6);
}

// =====================================================================
// Control — a NAMED source stays on the clone (borrow) path and is disposed
// exactly once at its own scope end; the enumerator borrows and never double-
// disposes the capture. §29.2 / §29.1.
// =====================================================================

BEZEL_TEST(owning_enumerator_temp_source_1641,
           named_source_control_not_double_disposed_spec_29_2) {
    JitResult r = runAotChecked(kNamedSource);
    BEZEL_EXPECT_TRUE(r.ran);            // no leak AND no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 3);      // 1 + 2
    // The named source `g` disposes its captured Cell(1) exactly once (the
    // enumerator clone borrows it via cloneDisposeFn and must not dispose it).
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "CELL-DISPOSED-1"), 1);
}
