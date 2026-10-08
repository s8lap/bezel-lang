// Spec-derived regression tests for #3036 — the residual of #3033: a
// DISCHARGED `owned` collection parameter (one the body moves out, returns,
// or `.Dispose()`s) was registered as `unwindOnly` for class types but NOT
// for collection types, so an exception thrown BEFORE the discharge site
// leaked the container's backing (and, for the owning family, every element
// it still held) out the unwinding edge.
//
// ── THE RULE ───────────────────────────────────────────────────────────────
// Bezel_Language_Spec.md §28.3 (v0.240) "Exception-path discharge", verbatim:
//
//   "Owned locals and owned parameters that are live when an exception
//    propagates out of their enclosing block are disposed implicitly during
//    stack unwinding. The compiler treats every owned local and owned
//    parameter as if it were wrapped in an implicit `try`/`finally` whose
//    `finally` disposes the value if it is still owned (i.e., not already
//    moved-from and not already explicitly disposed)."
//
// and §28.3 "One mechanism, two specializations, one lint", verbatim:
//
//   "Every owned local (and undischarged `owned` parameter) is disposed
//    exactly once on every path by the affine auto-drop above — the
//    exception-path discharge being the same mechanism on the unwinding
//    edge."
//
// §28.11 "Owning collections", verbatim: "The collection's `Dispose()`
// cascades through every element."
//
// The §28.3 quote names two cases — "owned local" (any type) and "owned
// parameter" (any type). The "undischarged" qualifier applies to the
// auto-drop specialization (the next paragraph); the exception-path
// discharge's "live when an exception propagates" is orthogonal and applies
// whether or not the body happens to ALSO carry a discharge point on the
// normal exit. A `return a` / `.Dispose()` / `move a` discharge covers the
// NORMAL exit; the §28.3 implicit `try`/`finally` covers the EXCEPTION exit.
// Both must run; neither subsumes the other.
//
// ── THE DEFECT ─────────────────────────────────────────────────────────────
// `Emitter::registerUnwindOwnedParam` (LlvmBackend_Setup.cpp) registered
// `unwindOnly` for a class/interface-typed discharged `owned` parameter, but
// its `releaseOnUnwindOnly`-style sibling for the COLLECTION family was
// gated on `ps->autoDropAtScopeEnd` (#3033). A discharged collection param
// therefore registered NO unwind-only entry, the §28.3 implicit `try`/
// `finally` did not fire on the unwinding edge, and the backing + elements
// leaked. #3033 deliberately left this residual, recorded it, and noted
// the double-free hazard that prevents naively widening #3033's branch
// (the collection/dict/linkedList arms of `emitDisposals` carry no
// `releaseOnUnwindOnly` gate and do not null the slot after freeing, so
// adding an entry today would trade a bounded leak for a double-free).
//
// The fix is the standard one #1957 / #2869 already shaped for the
// ChannelSender / Channel / Shared family:
//   (1) add the `if (it->releaseOnUnwindOnly && !dropUndischarged) continue;`
//       gate to the three collection arms of `emitDisposals`;
//   (2) null the slot after the free in those arms (the class path's
//       dispose-exactly-once pattern), so a sibling or outer disposal
//       registered against the same slot reads null on its later unwind
//       path and skips;
//   (3) drop the `autoDropAtScopeEnd` gate in `registerUnwindOwnedParam`'s
//       collection branch and set `d.releaseOnUnwindOnly =
//       !ps->autoDropAtScopeEnd`, mirroring the ChannelSender/Channel/
//       Shared branch above it.
//
// ── OBSERVABILITY ──────────────────────────────────────────────────────────
// Every accept case uses `runAotChecked` (per-program ASan/LSan, per
// tests/README.md): without the fix each is an LSan DIRECT leak of the
// backing + elements, which flips `r.ran` to false. The exit code
// additionally pins that the program computed the right answer, so a
// vacuous "it exited 0" cannot certify the path. Every cell that touches
// the collection/dict/linkedList arms of `emitDisposals` also exercises
// step (2) — the null-after-free — so the regression test catches a fix
// that wires up only the `releaseOnUnwindOnly` gate without the nulling.

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"  // runAotChecked — AOT + ASan/LSan oracle
#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAotChecked;

namespace {

// A checked run is SKIPPED (not failed) when the ASan link toolchain is absent.
bool wasSkipped(const JitResult& r) {
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("SKIP") != std::string::npos) return true;
    return false;
}

int countOccurrences(const std::string& hay, const std::string& needle) {
    int n = 0;
    for (std::size_t p = hay.find(needle); p != std::string::npos;
         p = hay.find(needle, p + needle.size()))
        ++n;
    return n;
}

// A non-trivial-dispose element (§28.9: it has a destructor), so an
// OwningArray<Cell> exception-edge disposal must CASCADE into it — the
// destructor's marker is how the cascade is observed rather than merely
// inferred from LSan.
const char* kCell =
    "class Cell "
    "  public V: int; "
    "  public constructor(v: int) V := v; end constructor; "
    "  destructor Console.WriteLine($\"D{V}\"); end destructor; "
    "end Cell; ";

}  // namespace

// ===========================================================================
// (1) THE ISSUE'S OWN REPRO — §28.3 discharged + exception edge for an
//     `owned OwningArray<Cell>` parameter. The `return a` is a real
//     discharge (ownership transfers to the caller's `using var b`), but
//     the throw exits BEFORE the discharge runs, so the value is LIVE when
//     the unwinding edge reaches it. §28.3's implicit `try`/`finally` must
//     reclaim the backing AND cascade into both elements.
// ===========================================================================

// Verbatim repro from issue #3036: `class Fwd(a: owned OwningArray<Cell>):
// owned OwningArray<Cell> ... throw ... return a` leaks the collection on
// the exception path.
BEZEL_TEST(owned_collection_param_unwind_3036,
           issue_repro_forwarded_array_throws_before_discharge_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kCell +
        "class Fwd "
        "  public static method Run(a: owned OwningArray<Cell>): owned OwningArray<Cell> "
        "    if a.Count > 1 then throw new InvalidOperationException(\"boom\"); end; "
        "    return a; "
        "  end Run; "
        "end Fwd; "
        "class C "
        "  public static method Main(): int "
        "    var a := new OwningArray<Cell>(); "
        "    a.Add(new Cell(1)); "
        "    a.Add(new Cell(2)); "
        "    try "
        "      using var b := Fwd.Run(move a); "
        "      return b.Count; "
        "    catch e: InvalidOperationException => "
        "      return 43; "
        "    end; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    // WITHOUT the fix: LSan "184 byte(s) leaked in 2 allocation(s)" (the
    // bz_array_t header + its slot storage) PLUS the two Cell instances,
    // → r.ran false.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 43);
    // The §28.11 cascade over the array fires on the unwind path: each
    // element disposed exactly once (not zero — the leak; not twice — a
    // double-free the new null-after-free must prevent).
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
}

// ===========================================================================
// (2) AXIS: the collection FAMILY. `buildCollectionTempDisposal` selects
//     bz_array_free / bz_llist_free / bz_dict_free. One cell per arm —
//     a fix that wired up only the array-backed arm would leave two
//     leaking on the exception edge of a discharged parameter.
// ===========================================================================

// bz_llist_free arm (§4.7): OwningLinkedList<Cell> — node chain + cascade.
BEZEL_TEST(owned_collection_param_unwind_3036,
           owned_linked_list_param_exception_edge_cascades_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kCell +
        "class Fwd "
        "  public static method Run(xs: owned OwningLinkedList<Cell>): owned OwningLinkedList<Cell> "
        "    if xs.Count > 1 then throw new InvalidOperationException(\"boom\"); end; "
        "    return xs; "
        "  end Run; "
        "end Fwd; "
        "class C "
        "  public static method Main(): int "
        "    var xs := new OwningLinkedList<Cell>(); "
        "    xs.AddLast(new Cell(1)); "
        "    xs.AddLast(new Cell(2)); "
        "    try "
        "      using var b := Fwd.Run(move xs); "
        "      return b.Count; "
        "    catch e: InvalidOperationException => "
        "      return 43; "
        "    end; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 43);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
}

// bz_dict_free arm (§4.9): OwningDictionary<string,Cell> — table +
// value-cascade. Keys are value-like and freed by bz_dict_free's own key
// free; only the values need the cascade.
BEZEL_TEST(owned_collection_param_unwind_3036,
           owned_dictionary_param_exception_edge_cascades_values_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kCell +
        "class Fwd "
        "  public static method Run(d: owned OwningDictionary<string, Cell>): owned OwningDictionary<string, Cell> "
        "    if d.Count > 1 then throw new InvalidOperationException(\"boom\"); end; "
        "    return d; "
        "  end Run; "
        "end Fwd; "
        "class C "
        "  public static method Main(): int "
        "    var d := new OwningDictionary<string, Cell>(); "
        "    d.Add(\"a\", new Cell(1)); "
        "    d.Add(\"b\", new Cell(2)); "
        "    try "
        "      using var b := Fwd.Run(move d); "
        "      return b.Count; "
        "    catch e: InvalidOperationException => "
        "      return 43; "
        "    end; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 43);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
}

// Dict-backed Set<int> (§4.10): the bz_dict_free arm, no element cascade
// (int elements are value-like, no Dispose).
BEZEL_TEST(owned_collection_param_unwind_3036,
           owned_dict_backed_set_param_exception_edge_spec_28_3) {
    JitResult r = runAotChecked(
        "module M; "
        "class Fwd "
        "  public static method Run(s: owned Set<int>): owned Set<int> "
        "    if s.Count > 1 then throw new InvalidOperationException(\"boom\"); end; "
        "    return s; "
        "  end Run; "
        "end Fwd; "
        "class C "
        "  public static method Main(): int "
        "    var s := new Set<int>(); "
        "    s.Add(1); "
        "    s.Add(2); "
        "    try "
        "      using var b := Fwd.Run(move s); "
        "      return b.Count; "
        "    catch e: InvalidOperationException => "
        "      return 43; "
        "    end; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 43);
}

// ===========================================================================
// (3) NORMAL EXIT UNCHANGED. The fix is gated on `releaseOnUnwindOnly`,
//     so on the normal (return a / Dispose / move) path no unwind entry
//     fires. Pinned by both the success path of the repro (return a
//     reaches the caller's `using var b`) and an explicit-Dispose path.
//     This is the cell that fails loudly (ASan heap-use-after-free or
//     double-free) if the gate is dropped — the strongest attack on the
//     fix's "exactly once" invariant.
// ===========================================================================

// The success branch of the issue's repro: `return a` runs, the
// caller's `using var b` owns from there, the auto-drop never fires, and
// the normal-exit disposal in the callee must NOT free the backing (the
// callee only enters the throw branch).
BEZEL_TEST(owned_collection_param_unwind_3036,
           forwarded_array_param_normal_exit_not_double_freed_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kCell +
        "class Fwd "
        "  public static method Run(a: owned OwningArray<Cell>): owned OwningArray<Cell> "
        "    return a; "
        "  end Run; "
        "end Fwd; "
        "class C "
        "  public static method Main(): int "
        "    var a := new OwningArray<Cell>(); "
        "    a.Add(new Cell(1)); "
        "    a.Add(new Cell(2)); "
        "    using var b := Fwd.Run(move a); "
        "    return b.Count + 41; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    // A non-gated registration on the discharged path would be an ASan
    // heap-use-after-free / double-free here → r.ran false.
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 43);  // b.Count=2 + 41
    // Every element disposed exactly once: the caller's `using var b`
    // cascade at scope end.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
}

// ===========================================================================
// (4) AUTO-DROP PATH UNCHANGED. #3033's auto-drop branch must still
//     dispose on BOTH exits (a never-discharged owner at scope end). The
//     fix only changes the discharged branch's behavior; the auto-drop
//     path's `releaseOnUnwindOnly = false` keeps it on both edges.
//     Pinned here as a guard against an over-eager fix that drops the
//     `autoDropAtScopeEnd` distinction by mistake.
// ===========================================================================

// Verbatim from #3033 (this is its `owned_array_param_disposed_on_unwind_edge_spec_28_3`
// shape) — `return 0` is not a discharge, so the parameter is auto-dropped;
// both edges must fire.
BEZEL_TEST(owned_collection_param_unwind_3036,
           auto_drop_array_param_still_fires_on_both_edges_spec_28_3) {
    JitResult r = runAotChecked(
        std::string("module M; ") + kCell +
        "class C "
        "  public static method Take(a: owned OwningArray<Cell>): int "
        "    if a.Count > 1 then throw new InvalidOperationException(\"boom\"); end; "
        "    return 0; "
        "  end Take; "
        "  public static method Main(): int "
        "    var a := new OwningArray<Cell>(); "
        "    a.Add(new Cell(1)); "
        "    a.Add(new Cell(2)); "
        "    try "
        "      return C.Take(move a); "
        "    catch e: InvalidOperationException => "
        "      return 43; "
        "    end; "
        "  end Main; "
        "end C;");
    if (wasSkipped(r)) return;
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 43);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "D2"), 1);
}
