// Codegen / ownership — reassigning (`this.seq := Gen2()`) an `owned
// IEnumerable<T>` / `IOwningEnumerable<T>` CLASS FIELD must dispose the OLD
// iterator FRAME before overwriting the pointer (spec §28.7 drop-on-overwrite,
// v0.196; §29.7 iterator-vtable disposal). Issue #1644.
//
// THE BUG (#1644). The drop-on-overwrite path for owned reference fields
// (`isOwnedClassFieldLvalue` / `lowerOwnedClassFieldStore`, LlvmBackend_Setup.cpp)
// gated on `isOwnedReferenceField`, which — like the scope-end path #1643 fixed —
// REJECTS builtin iterator-interface fields (IEnumerable<T> / IOwningEnumerable<T>
// carry no user `declaration`). So a field reassignment did NOT recognise the
// field, fell through to a plain pointer store, and the OLD iterator frame (the
// heap object `bz$C$Gen` calloc's, plus its owned captures) leaked. Same
// root-cause class as #1643, which fixed the SCOPE-END/class-disposal path; this
// is the DROP-ON-OVERWRITE (reassignment) path.
//
// THE FIX. `isOwnedClassFieldLvalue` now also accepts
// `isOwnedIteratorInterfaceField`, so `lowerOwnedClassFieldStore` handles the
// reassignment; its dispose branch dispatches the ITERATOR-vtable slot-1 Dispose
// (vtable[1](self), which runs pending finally blocks and FREES THE FRAME ITSELF,
// §29.7) for an iterator field — NOT the class-vtable slot-2 emitDisposeAndReclaim
// (which would invoke MoveNext and never free). Mirrors emitOwnedFieldDisposals'
// iterField branch exactly (the scope-end analog). The null+identity guards above
// the branch are unchanged, so a first assignment (null old) and a self-assign
// still skip disposal.
//
// OBSERVABILITY. The displaced frame is a DIRECT (unreachable) leak once the
// pointer is overwritten, so runAotChecked's LeakSanitizer flags it: WITHOUT the
// fix `r.ran` is false. As a second, positive observable the generator captures an
// owned Cell whose destructor prints a marker — with the fix the reassignment
// disposes the old frame, which disposes Cell(1) exactly once BEFORE the next
// statement; without the fix that marker never appears (and Cell(1) leaks too).
// runAotChecked additionally guards the with-fix path against a double-free the
// new disposal could introduce (dispose-exactly-once rides on r.ran).

#include <cstddef>
#include <string>

#include "support/aot_support.hpp"   // runAotChecked — AOT + ASan/LSan oracle
#include "test_harness.hpp"

using bezel::test::JitResult;
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

// #1644 canonical repro (from the issue): an `owned IEnumerable<int>` field is
// reassigned via a method (`seq := s`). The displaced first frame must be freed
// by drop-on-overwrite (§28.7); before the fix it leaked (32 bytes from bz$C$Gen).
const char* kIEnumerableFieldOverwriteFramFreed =
    "module M; "
    "class C public static method Gen(): IEnumerable<int> "
    "  yield return 10; yield return 20; "
    "end Gen; end C; "
    "class Holder "
    "  private owned seq: IEnumerable<int>; "
    "  public constructor(owned s: IEnumerable<int>) seq := s; "
    "  end constructor; "
    "  public method Reset(owned s: IEnumerable<int>) seq := s; end Reset; "
    "end Holder; "
    "class M2 public static method Main(): int "
    "  using var h := new Holder(C.Gen()); "
    "  h.Reset(C.Gen()); "   // overwrite: OLD frame must be disposed here
    "  return 0; "
    "end Main; end M2;";

// #1644 with an OWNED-CAPTURE generator: the iterator frame captures an owned
// Cell. Reassigning the field must dispose the OLD frame, which disposes its
// captured Cell(1) exactly once (marker "CELL-DISPOSED-1") BEFORE "after-reset".
// The new frame's Cell(2) is disposed at the holder's scope-end. Positively
// pins that the reassignment ran the iterator-vtable Dispose (not a no-op, and
// not the class-vtable slot-2 path — which would MoveNext and never free).
const char* kIEnumerableFieldOverwriteDisposesCapture =
    "module M; "
    "class Cell : IDisposable "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor Console.WriteLine(\"CELL-DISPOSED-\" + Id.ToString()); "
    "  end destructor; "
    "end Cell; "
    "class C public static method Gen(owned cell: Cell): IEnumerable<int> "
    "  yield return cell.Id; "
    "end Gen; end C; "
    "class Holder "
    "  private owned seq: IEnumerable<int>; "
    "  public constructor(owned s: IEnumerable<int>) seq := s; "
    "  end constructor; "
    "  public method Reset(owned s: IEnumerable<int>) seq := s; end Reset; "
    "end Holder; "
    "class M2 public static method Main(): int "
    "  using var h := new Holder(C.Gen(new Cell(1))); "
    "  Console.WriteLine(\"before-reset\"); "
    "  h.Reset(C.Gen(new Cell(2))); "
    "  Console.WriteLine(\"after-reset\"); "
    "  return 0; "
    "end Main; end M2;";

// Regression guard: a PLAIN owned class-typed field (`owned c: Cell`) overwrite
// must still dispose the displaced old value via the class-vtable path
// (emitDisposeAndReclaim), exactly once — the iterator branch must not perturb
// it. Cell(1) disposed on reassignment (before "after-reset"), Cell(2) at scope
// end; a wrong branch here would leak or double-free (r.ran false).
const char* kPlainClassFieldOverwriteStillDisposes =
    "module M; "
    "class Cell : IDisposable "
    "  public Id: int; "
    "  public constructor(id: int) Id := id; end constructor; "
    "  destructor Console.WriteLine(\"CELL-DISPOSED-\" + Id.ToString()); "
    "  end destructor; "
    "end Cell; "
    "class Holder "
    "  private owned c: Cell; "
    "  public constructor(owned x: Cell) c := x; end constructor; "
    "  public method Reset(owned x: Cell) c := x; end Reset; "
    "end Holder; "
    "class M2 public static method Main(): int "
    "  using var h := new Holder(new Cell(1)); "
    "  Console.WriteLine(\"before-reset\"); "
    "  h.Reset(new Cell(2)); "
    "  Console.WriteLine(\"after-reset\"); "
    "  return 0; "
    "end Main; end M2;";

}  // namespace

// =====================================================================
// #1644 — reassigning an owned iterator-interface field frees the displaced
// frame (§28.7 drop-on-overwrite, v0.196; §29.7 iterator-vtable Dispose).
// =====================================================================

BEZEL_TEST(codegen_owned_iter_field_overwrite_1644,
           ienumerable_field_overwrite_frees_old_frame_spec_28_7) {
    // spec §28.7, v0.196: reassigning an owned field disposes the displaced
    // value before storing the new owner. For an iterator-interface field the
    // displaced value is an iterator frame freed via the slot-1 Dispose.
    // WITHOUT the fix the first frame leaks (LSan: 32 bytes from bz$C$Gen) →
    // r.ran flips false. WITH the fix the run is LSan-clean.
    JitResult r = runAotChecked(kIEnumerableFieldOverwriteFramFreed);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
}

BEZEL_TEST(codegen_owned_iter_field_overwrite_1644,
           ienumerable_field_overwrite_disposes_captured_owned_spec_28_7) {
    // §28.7 / §29.7: the reassignment disposes the OLD frame via the iterator
    // slot-1 Dispose, which frees the frame AND cascades into its owned capture
    // Cell(1). WITHOUT the fix the old frame + its Cell(1) leak (r.ran false) and
    // "CELL-DISPOSED-1" never appears; WITH the fix it appears exactly once and
    // before "after-reset". Cell(2) (the new frame) is disposed once at scope end.
    JitResult r = runAotChecked(kIEnumerableFieldOverwriteDisposesCapture);
    BEZEL_EXPECT_TRUE(r.ran);            // LSan-clean: no leaked frame / capture
    BEZEL_EXPECT_EQ(r.exitCode, 0);

    // Load-bearing #1644 assertion: the displaced frame's captured Cell(1) WAS
    // disposed, exactly once, by the reassignment (§28.7). Absent the fix: 0.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "CELL-DISPOSED-1"), 1);
    // The new frame's Cell(2) is disposed exactly once at the holder's scope end.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "CELL-DISPOSED-2"), 1);

    // Cell(1)'s disposal happens AT the reassignment — before the next statement.
    const std::size_t disp1 = r.stdoutText.find("CELL-DISPOSED-1");
    const std::size_t after = r.stdoutText.find("after-reset");
    BEZEL_EXPECT_TRUE(disp1 != std::string::npos);
    BEZEL_EXPECT_TRUE(after != std::string::npos);
    BEZEL_EXPECT_TRUE(disp1 < after);
}

// =====================================================================
// Regression — a plain owned class-typed field overwrite still disposes the
// displaced value via the class-vtable path (the iterator branch must not
// perturb it). §28.7.
// =====================================================================

BEZEL_TEST(codegen_owned_iter_field_overwrite_1644,
           plain_owned_class_field_overwrite_still_disposes_spec_28_7) {
    JitResult r = runAotChecked(kPlainClassFieldOverwriteStillDisposes);
    BEZEL_EXPECT_TRUE(r.ran);            // no leak / no double-free
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    // Cell(1) disposed on reassignment, Cell(2) at scope end — each exactly once.
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "CELL-DISPOSED-1"), 1);
    BEZEL_EXPECT_EQ(countOccurrences(r.stdoutText, "CELL-DISPOSED-2"), 1);
    const std::size_t disp1 = r.stdoutText.find("CELL-DISPOSED-1");
    const std::size_t after = r.stdoutText.find("after-reset");
    BEZEL_EXPECT_TRUE(disp1 != std::string::npos && after != std::string::npos);
    BEZEL_EXPECT_TRUE(disp1 < after);
}
