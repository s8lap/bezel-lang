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

#include "support/aot_support.hpp"   // runAot, runAotCheckedDisposal — AOT test harness
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::runAotCheckedDisposal;

namespace {

// Convenience wrapper: run in checked-disposal MODE (§28.2 — DisposedAccess-
// Exception on use-after-dispose), via the AOT harness; returns exit code, or
// -999 if the program failed to compile/run.
int runChecked(const std::string& program) {
    auto r = runAotCheckedDisposal(program);
    return r.ran ? r.exitCode : -999;
}

// Convenience wrapper: run in default (unchecked) mode.
int runDefault(const std::string& program) {
    auto r = runAot(program);
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
    "        throw move ex; "
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


// ── §31.1: same program in DEFAULT (unchecked) mode — the behavior is
// undefined per the spec, so we don't pin a specific value; we only verify
// the program RUNS (no compile error, no crash-from-the-test-harness). The
// borrowed field read in the catch arm touches disposed memory and may
// observe anything; what we DO pin is the COMPILE acceptance — the spec
// admits the borrow-in-field program, and the check-vs-unchecked difference
// is purely at runtime.
//
// We use runDefault and assert != -999 (program ran). This test exists to
// document the asymmetry; it is not the load-bearing assertion.

BEZEL_TEST(throw_borrow_field_catch_dispose_1678, spec_31_1_unchecked_runs) {
    // §31.1: in unchecked disposal mode the catch-arm access is undefined
    // behavior (it may read garbage, fault, or happen to return a value);
    // what the spec REQUIRES is that the program compiles and runs to
    // completion under the language-level rules — the check is a runtime
    // disposal-mode-only backstop. Assert the program ran (exit != -999).
    int rc = runDefault(kBorrowFieldInThrown);
    BEZEL_EXPECT_TRUE(rc != -999);
}


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
    "      throw move ex; "
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