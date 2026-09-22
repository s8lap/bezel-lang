// Regression test for #1678 (T-10 / v0.184 / spec §31.1, §28.3 cross-reference):
// a thrown exception instance with a NON-OWNED (borrowed) class-typed field
// whose source is a frame-local binding of the throwing frame must fault
// deterministically as `DisposedAccessException` in checked disposal mode
// when the catch arm accesses that field. Per §31.1:
//
//   "A borrowing (non-owned) class-typed field inside a thrown instance follows
//    §28.3's field-held residual-trust regime unchanged: the throw extends the
//    instance's lifetime to the matching handler, so a field borrow whose
//    source is a binding of the throwing frame — or of any frame the exception
//    unwinds past — violates the source-outlives-the-object obligation the
//    moment the unwind disposes the source; the catch-arm access then faults
//    deterministically as DisposedAccessException in checked disposal mode
//    (§28.2; undefined behavior unchecked), because the unwind itself set the
//    disposed flag."
//
// This is test-only: the runtime machinery (the disposed-flag guard raised by
// any member access through a logically-disposed instance) already exists for
// §28.2, and #1678 simply pins the §31.1 cross-reference by exercising it on a
// borrow-field path. The test pins the extra-programs-01 program.
//
// runAot observes Main()'s return via the process exit code (8-bit wrapped,
// aot_support.hpp); every value used here is ≤ 100, so it round-trips exactly.

#include "support/aot_support.hpp"   // runAotCheckedDisposal — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAotCheckedDisposal;

namespace {

// Convenience wrapper: run in checked-disposal MODE (§28.2 — DisposedAccess-
// Exception on use-after-dispose), via the AOT harness; returns exit code, or
// -999 if the program failed to compile/run.
//
// There is deliberately no default-mode (`runAot`) wrapper here: the only cell
// that wanted one asserted that §31.1 undefined behaviour runs to completion,
// which is incoherent under the gate's generated-code ASan (see the note where
// that cell used to be, below).
int runChecked(const std::string& program) {
    auto r = runAotCheckedDisposal(program);
    return r.ran ? r.exitCode : -999;
}

} // namespace


// ── §31.1 / §28.3 v0.184: catch-arm access to a thrown instance's borrowed
// frame-local field faults as DisposedAccessException in checked mode.

// The throwing frame establishes `src := new Source(42)` (a `using var`, so
// it will be disposed when the throw unwinds out of the try). It wraps `src`
// in a `MyException` whose field `Ref: Source` is a NON-OWNED borrow of that
// local. The throw unwinds out of the try, `src`'s scope-end disposal runs
// (`using var`), the borrowed field now points at a disposed instance, and the
// catch arm's `e.Ref.Val` read (which is a method/field access on a logically-
// disposed instance) raises `DisposedAccessException` per §28.2/§31.4.
const char* kBorrowFieldInThrown =
    "module M; "
    "class Source "
    "  public Val: int := 0; "
    "  public constructor(v: int) Val := v; end constructor; "
    "end Source; "
    "class MyException : Exception "
    "  public Ref: Source; "
    "  public constructor(s: Source, msg: string) base(msg); Ref := s; end constructor; "
    "end MyException; "
    "class C "
    "  public static method Main(): int "
    "    try "
    "      try "
    "        using var src := new Source(42); "
    "        var ex := new MyException(src, \"boom\"); "
    "        throw ex; "
    "      catch var e: MyException => "
    "        return e.Ref.Val; "
    "      end; "
    "    catch DisposedAccessException => return 1; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

BEZEL_TEST(throw_borrow_field_catch_dispose_1678, spec_31_1_borrow_field_disposed_access_raises) {
    // §31.1: in checked disposal mode the catch-arm access to the thrown
    // instance's borrowed field whose source was disposed by the throw's
    // unwind raises DisposedAccessException, which the outer arm catches
    // and turns into exit code 1.
    BEZEL_EXPECT_EQ(runChecked(kBorrowFieldInThrown), 1);
}


// ── §31.1 in DEFAULT (unchecked) mode: DELIBERATELY NOT TESTED HERE.
//
// A `spec_31_1_unchecked_runs` cell stood here. It ran kBorrowFieldInThrown
// through runAot and asserted only that the program ran (exit != -999),
// to "document the asymmetry" with the checked-mode cell above.
//
// That assertion cannot hold under the gate's `--full` pass, and removing it
// is not a workaround — the cell was asking for something incoherent. §31.1
// says the catch-arm access to a borrow whose source the unwind already
// disposed is UNDEFINED in unchecked mode. Generated-code AddressSanitizer,
// which `--full` forces on every test program, exists precisely to catch that
// read: it reports a use-after-free, the harness sets ran=false, and the cell
// fails. Asserting that undefined behaviour "runs to completion" under a
// sanitizer whose whole job is to detect it is a contradiction, and the ASan
// report is the CORRECT outcome, not a false positive.
//
// Nothing is lost. The cell's own comment said what it actually pinned was
// COMPILE acceptance — and that is already pinned by
// `spec_31_1_checked_faults` above, which compiles the very same
// kBorrowFieldInThrown program (a compile error there would fail that cell
// first). The runtime half is unpinnable by construction: the spec defines no
// observable to assert.
//
// If the accept-surface ever needs its own dedicated cell, the lit track is
// its home — it drives bezelc per-process and never runs the program.


// ── §31.1 / §28.3: a same-frame handler whose sources' scopes enclose the
// try is sound — the borrow's source is NOT disposed, so the catch-arm access
// succeeds normally. Pins the spec's "A same-frame handler whose sources'
// scopes enclose the try remains sound" clause.

// `outerSrc` is declared BEFORE the try and outlives it; the thrown
// exception's `Ref: Source` field borrows `outerSrc`. The throw unwinds,
// but `outerSrc` (an outer `using var`) is NOT disposed by the unwind
// (its scope encloses the try), so the catch arm's `e.Ref.Val` read is a
// legitimate access on a live instance.
const char* kBorrowFieldOuterScope =
    "module M; "
    "class Source "
    "  public Val: int := 0; "
    "  public constructor(v: int) Val := v; end constructor; "
    "end Source; "
    "class MyException : Exception "
    "  public Ref: Source; "
    "  public constructor(s: Source, msg: string) base(msg); Ref := s; end constructor; "
    "end MyException; "
    "class C "
    "  public static method Main(): int "
    "    using var outerSrc := new Source(99); "
    "    try "
    "      var ex := new MyException(outerSrc, \"boom\"); "
    "      throw ex; "
    "    catch var e: MyException => "
    "      return e.Ref.Val; "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end C;";

BEZEL_TEST(throw_borrow_field_catch_dispose_1678, spec_31_1_outer_scope_borrow_sound) {
    // §31.1: a borrow field whose source's scope encloses the try remains
    // sound across the unwind — the source is NOT disposed, so the
    // catch-arm access succeeds and returns 99 (the Val we stored).
    // This is checked-mode too (the test runs in checked disposal mode),
    // and the live access must NOT raise.
    BEZEL_EXPECT_EQ(runChecked(kBorrowFieldOuterScope), 99);
}