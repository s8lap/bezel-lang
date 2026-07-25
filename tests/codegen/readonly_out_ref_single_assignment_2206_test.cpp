// §22.8 (v0.226) single-assignment of readonly fields (#2206, route A).
//
// RESIDUAL after #2187 (class direct-assignment family) and #2207 (record
// ctor): the once-check (checkReadonlyOnceness in DefiniteAssignment.cpp)
// fired only on AssignmentStmt/IncrementStmt, not on `out`/`ref` argument
// writes. So a readonly field that received its single `:=` could be written a
// SECOND time through an `out`/`ref` argument without diagnosis — the same
// §22.8 finality break the §7.4/#2116 narrowing anchor is sound only against
// (an observed non-null readonly value must be final; an `out` write after the
// fact rode a closure would null it). The assignability gate
// (checkRefOutFieldOperand in ExprTypecheck.cpp) legitimately admits a single
// `ref`/`out` to a readonly field while `inConstructor_`; the missing piece was
// the ONCE-NESS, which is the DA pass's job.
//
// The fix feeds every `ref`/`out` operand through checkReadonlyOnceness inside
// visitCallArg: a first, non-loop write is admitted (the legitimate single
// assignment, including via `out`/`ref`); a SECOND one, or any such write
// inside a loop, is rejected. This file is the runtime-value companion to the
// lit/FileCheck track (tests/lit/sema/readonly_*_single_assignment_2206__*):
// the ACCEPT controls exercise the legitimate single-`out`/`ref` shapes
// (including the narrowing read the fix protects) and confirm they COMPILE and
// produce the correct value; the REJECT cells confirm the double-write family
// via `out`/`ref` is now a §22.8 compile error.

#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// True iff sema rejected `src` with a diagnostic containing `needle`.
bool rejectedWith(const std::string& src, const std::string& needle) {
    auto r = runAot(src);
    if (r.ran) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

}  // namespace

// =========================================================================
// ACCEPT controls — a single `out`/`ref` to a readonly field is the legitimate
// single assignment. (The narrowing reads exercise the #2116 §7.4 feature the
// fix protects — the once-ness is what makes the observed value final.)
// =========================================================================

// §22.8 (#2206): a readonly field initialized EXACTLY ONCE via an `out`
// argument (the ctor never `:=`s it; `Init(out _name)` is the single write).
// Compiles, runs, and the §7.4 narrowing reads `_name.Length` — the soundness
// property the once-check exists to protect. Non-regression: the fix must NOT
// reject the sound single-`out` case. "hey".Length == 3.
BEZEL_TEST(readonly_out_ref_single_assignment_2206, single_out_init_narrows) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public readonly _name: string?; "
        "  public constructor() Init(out _name); end constructor; "
        "  destructor end destructor; "
        "  public method Init(out x: string?) x := \"hey\"; end Init; "
        "  public method Use(): int "
        "    if _name <> null then return _name.Length; end; return 0; "
        "  end Use; "
        "  public static method Main(): int "
        "    using var c := new C(); return c.Use(); "
        "  end Main; end C;"), 3);
}

// §22.8 (#2206): a readonly field initialized EXACTLY ONCE via a `ref`
// argument (the ctor never `:=`s it; `Probe(ref _name)` is the single write —
// the callee stores through the alias). Admitted as the single assignment,
// runs, and the §7.4 narrowing reads `_name.Length`. NB: a `ref` AFTER a `:=`
// of the same field is a second write and is rejected (see
// ref_second_write_rejected below). "hey".Length == 3.
BEZEL_TEST(readonly_out_ref_single_assignment_2206, single_ref_init_narrows) {
    BEZEL_EXPECT_EQ(run(
        "module M; class C public readonly _name: string?; "
        "  public constructor() Probe(ref _name); end constructor; "
        "  destructor end destructor; "
        "  public method Probe(ref x: string?) x := \"hey\"; end Probe; "
        "  public method Use(): int "
        "    if _name <> null then return _name.Length; end; return 0; "
        "  end Use; "
        "  public static method Main(): int "
        "    using var c := new C(); return c.Use(); "
        "  end Main; end C;"), 3);
}

// =========================================================================
// REJECT cells — a second write of a readonly field via `out`/`ref` is a §22.8
// error (the residual route #2206 names after #2187/#2207).
// =========================================================================

// §22.8 (#2206): the exact repro from the issue — `_f := "a"; SetIt(out _f);`
// is two assignments of the readonly `_f` (the `out` write is the second).
BEZEL_TEST(readonly_out_ref_single_assignment_2206, out_second_write_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C public readonly _f: string?; "
        "  public constructor() _f := \"a\"; SetIt(out _f); end constructor; "
        "  destructor end destructor; "
        "  public method SetIt(out x: string?) x := null; end SetIt; "
        "  public static method Main(): int return 0; end Main; end C;",
        "cannot assign to readonly field `_f` more than once"));
}

// §22.8 (#2206): a `ref` second write — `_f := "a"; Mutate(ref _f);` — is the
// same finality break as the `out` route (the callee may store through `ref`).
BEZEL_TEST(readonly_out_ref_single_assignment_2206, ref_second_write_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C public readonly _f: string?; "
        "  public constructor() _f := \"a\"; Mutate(ref _f); end constructor; "
        "  destructor end destructor; "
        "  public method Mutate(ref x: string?) x := null; end Mutate; "
        "  public static method Main(): int return 0; end Main; end C;",
        "cannot assign to readonly field `_f` more than once"));
}

// §22.8 (#2206): order-independence — the `out` write seeds the once-set on the
// FIRST write, so a later `:=` of the same readonly field is the second
// assignment and is rejected regardless of which route wrote first.
BEZEL_TEST(readonly_out_ref_single_assignment_2206, out_then_assign_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C public readonly _f: string?; "
        "  public constructor() SetIt(out _f); _f := \"b\"; end constructor; "
        "  destructor end destructor; "
        "  public method SetIt(out x: string?) x := \"a\"; end SetIt; "
        "  public static method Main(): int return 0; end Main; end C;",
        "cannot assign to readonly field `_f` more than once"));
}

// §22.8 (#2206): a readonly field written through an `out` argument inside a
// loop could be written on more than one iteration — rejected, same shape as
// #2187's direct-assign-in-loop check.
BEZEL_TEST(readonly_out_ref_single_assignment_2206, out_in_loop_rejected) {
    BEZEL_EXPECT_TRUE(rejectedWith(
        "module M; class C public readonly _f: int; "
        "  public constructor() for var i in 0..3 do SetIt(out _f); end; end constructor; "
        "  destructor end destructor; "
        "  public method SetIt(out x: int) x := 0; end SetIt; "
        "  public static method Main(): int return 0; end Main; end C;",
        "cannot assign to readonly field `_f` inside a loop"));
}
