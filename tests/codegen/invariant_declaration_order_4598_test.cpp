// Spec-derived regression tests for §22.16 (v0.306), issue #4598:
// `Emitter::emitInvariantCheck` evaluated a class's OWN invariant members in
// REVERSE declaration order.
//
// §22.16 "Class invariants" (v0.306):
//   "The class invariant of C is the conjunction of every invariant member
//    of C and of every base class, evaluated base-first, declaration order
//    within each class."
//
// This is TWO independent axes: (1) base-first across the hierarchy, and
// (2) declaration order within each class's own members. The pre-fix code
// built the chain most-derived-first (each class's members pushed in
// declaration order), then ran a SINGLE `std::reverse` over the whole flat
// vector to fix axis (1). For a vector built as blockD ++ blockB (blocks in
// most-derived-first order, each block's own contents already in
// declaration order), `reverse(blockD ++ blockB) == reverse(blockB) ++
// reverse(blockD)`: the whole-vector reverse flips the BLOCK order to
// base-first (axis 1, correct) but ALSO reverses each block's own contents
// (axis 2, WRONG) — a class with two invariants evaluated them in the
// wrong order.
//
// §22.16 clause evaluation stops at the first clause producing `false`
// (short-circuit), so which invariant is blamed FIRST is observable at
// runtime whenever the invariant's own evaluation can differ by more than
// "a boolean came out false" — this file uses a **guard-then-divide** pair
// per class: `invariant(g <> 0); invariant(x / g > 0);`. When `g == 0`:
//   * evaluated in declaration order (guard first) — the guard clause is
//     `false`, raising ContractViolationException BEFORE the divide clause
//     is ever reached — no crash.
//   * evaluated in the wrong (reversed) order — the divide clause runs
//     FIRST with `g == 0`, raising DivideByZeroException — a different,
//     specific, and distinguishable exception type. `catch
//     ContractViolationException` does NOT catch it.
// This is exactly the pattern the issue itself names ("invariant(p <> null)
// declared before invariant(p.X > 0)") adapted to avoid §7.4's nullable
// member-access narrowing question — integer division by zero (§6.1)
// raises unconditionally regardless of narrowing, so the observable is not
// entangled with any other spec rule.
//
// Each test exercises the real AOT path (fork-isolated Driver::compileFile
// + clang link + native exec) and asserts the SPECIFIC caught exception's
// discriminator — never `failedAtCompileTimeWithRealLocation` (vacuous when
// the pre-fix behaviour is a crash) and never a bare "did it compile".

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

// §22.16 / --checked-contracts (issue #4386): invariant checks are emitted
// only under checked-contracts mode.
int runChecked(const std::string& program) {
    auto r = bezel::test::runAotCheckedContracts(program);
    return r.ran ? r.exitCode : -999;
}

// §22.16 release default (unchecked mode, production default): invariant
// checks are NOT emitted at all.
int runUnchecked(const std::string& program) {
    auto r = bezel::test::runAot(program, bezel::codegen::CodegenOptions{});
    return r.ran ? r.exitCode : -999;
}

// A base class B (two invariants: a guard, then a divide using the guard)
// and a derived class D (two more invariants of the same shape, using D's
// own fields), covering all four §22.16 positions:
//   B position 1 (base, decl-order 1): invariant(gB <> 0);
//   B position 2 (base, decl-order 2): invariant(xB / gB > 0);
//   D position 1 (derived, decl-order 1): invariant(gD <> 0);
//   D position 2 (derived, decl-order 2): invariant(xD / gD > 0);
// Correct §22.16 order is exactly that list: base-first, declaration order
// within each class.
const char* kHierarchy =
    "module M; "
    "class B "
    "  public gB: int := 1; "
    "  public xB: int := 1; "
    "  invariant(gB <> 0); "
    "  invariant(xB / gB > 0); "
    "  public constructor(gB_: int, xB_: int) "
    "    gB := gB_; xB := xB_; "
    "  end constructor; "
    "end B; "
    "class D : B "
    "  public gD: int := 1; "
    "  public xD: int := 1; "
    "  invariant(gD <> 0); "
    "  invariant(xD / gD > 0); "
    "  public constructor(gB_: int, xB_: int, gD_: int, xD_: int) "
    "    base(gB_, xB_); "
    "    gD := gD_; xD := xD_; "
    "  end constructor; "
    "end D; "
    "class Program "
    "  public static method Run(gB_: int, xB_: int, gD_: int, xD_: int): int "
    "    var r: int := 0; "
    "    try "
    "      using var d := new D(gB_, xB_, gD_, xD_); "
    "      r := 0; "
    "    catch ContractViolationException => r := 1; "
    "    catch DivideByZeroException => r := 2; "
    "    end; "
    "    return r; "
    "  end Run; "
    "end Program;";

std::string mainCalling(const std::string& args) {
    return std::string(kHierarchy) +
        " class MainEntry "
        "  public static method Main(): int "
        "    return Program.Run(" + args + "); "
        "  end Main; "
        " end MainEntry;";
}

}  // namespace

// =========================================================================
// Axis 2 (within-class order), BASE class B: gB == 0 trips B's OWN guard
// clause. §22.16 requires B.pos1 (the guard) to evaluate BEFORE B.pos2 (the
// divide) — so the fault is a clean ContractViolationException (r == 1),
// never reaching the divide. D's fields are set so D's own invariants would
// pass if ever reached (they must not be, since B's construction-exit check
// faults first and short-circuits the whole conjunction).
//
// FAIL-WITHOUT: pre-#4598, `emitInvariantCheck` evaluated B's TWO
// invariants in reverse (the divide first), so `xB / gB` with `gB == 0`
// raised DivideByZeroException (r == 2) instead.
// =========================================================================
BEZEL_TEST(invariant_declaration_order_4598,
           base_class_within_order_guard_before_divide_spec_22_16) {
    BEZEL_EXPECT_EQ(runChecked(mainCalling("0, 1, 1, 1")), 1);
}

// =========================================================================
// Axis 2 (within-class order), DERIVED class D: B's own invariants are set
// to PASS (gB == 1, xB / gB == 1 > 0), so the conjunction reaches D's block.
// gD == 0 trips D's OWN guard clause; §22.16 requires D.pos1 (the guard) to
// evaluate BEFORE D.pos2 (the divide) — clean ContractViolationException
// (r == 1).
//
// FAIL-WITHOUT: pre-#4598, D's two invariants were also evaluated in
// reverse, so `xD / gD` with `gD == 0` raised DivideByZeroException
// (r == 2) instead. (B's own reversed pair happens to still pass here,
// since both of B's clauses are individually true regardless of order —
// this row isolates D's axis from B's.)
// =========================================================================
BEZEL_TEST(invariant_declaration_order_4598,
           derived_class_within_order_guard_before_divide_spec_22_16) {
    BEZEL_EXPECT_EQ(runChecked(mainCalling("1, 1, 0, 1")), 1);
}

// =========================================================================
// Axis 1 (base-first across classes), confirmed on the SAME hierarchy: both
// gB == 0 AND gD == 0 (both classes' guards would fail). §22.16's base-
// first rule means B's block is evaluated in full before D's block is ever
// reached, so B's own guard (position 1 overall) is the one that fires —
// a clean ContractViolationException short-circuits before D's block is
// touched at all. (This row does not by itself distinguish base-first from
// derived-first when the classes have no crash-observable cross-class
// dependency; it is combined with the cross-class row below, which does.)
// =========================================================================
BEZEL_TEST(invariant_declaration_order_4598,
           base_and_derived_both_violated_blames_base_first_spec_22_16) {
    BEZEL_EXPECT_EQ(runChecked(mainCalling("0, 1, 0, 1")), 1);
}

// =========================================================================
// Axis 1 (base-first across classes), ISOLATED from axis 2: a two-class
// hierarchy where each class declares exactly ONE invariant (so within-
// class order cannot matter), and the DERIVED class's clause divides by a
// field declared and guarded ONLY in the BASE class. §22.16's base-first
// rule requires the base's guard to evaluate before the derived class's
// clause is ever reached.
//
// gShared == 0: correct (base-first) order evaluates GB's guard first —
// clean ContractViolationException (r == 1). If base-first were violated
// (derived evaluated before base), GD's clause `xD / gShared > 0` would run
// first with `gShared == 0` and raise DivideByZeroException (r == 2).
//
// NOTE ON FAIL-WITHOUT: a single flat `std::reverse` over a chain built as
// concatenation of per-class blocks (most-derived-first, each block already
// in declaration order) reverses BOTH the block order and each block's
// contents — which happens to leave the BLOCK order (base-first) correct
// for any hierarchy depth, exactly as issue #4598 itself observes ("gets
// the base-first half right"). So this row is expected to PASS on both the
// pre-fix and post-fix compiler; it is a regression GUARD for axis 1 (in
// case a future fix to axis 2 — e.g. per-class grouping done a different
// way — accidentally reorders the GROUPS too), not a fail-without pin for
// #4598's own defect.
// =========================================================================
BEZEL_TEST(invariant_declaration_order_4598,
           base_before_derived_group_order_spec_22_16) {
    const std::string program =
        "module M; "
        "class GB "
        "  public gShared: int := 1; "
        "  invariant(gShared <> 0); "
        "  public constructor(g: int) gShared := g; end constructor; "
        "end GB; "
        "class GD : GB "
        "  public xD: int := 1; "
        "  invariant(xD / gShared > 0); "
        "  public constructor(g: int, x: int) "
        "    base(g); "
        "    xD := x; "
        "  end constructor; "
        "end GD; "
        "class Program "
        "  public static method Main(): int "
        "    var r: int := 0; "
        "    try "
        "      using var d := new GD(0, 1); "
        "      r := 0; "
        "    catch ContractViolationException => r := 1; "
        "    catch DivideByZeroException => r := 2; "
        "    end; "
        "    return r; "
        "  end Main; "
        "end Program;";
    BEZEL_EXPECT_EQ(runChecked(program), 1);
}

// =========================================================================
// A satisfied conjunction (every clause on every class holds) still
// completes normally under the FIXED order — this pins that the fix does
// not spuriously fault a program whose invariants all genuinely hold,
// regardless of which order they run in.
// =========================================================================
BEZEL_TEST(invariant_declaration_order_4598,
           all_satisfied_no_fault_spec_22_16) {
    BEZEL_EXPECT_EQ(runChecked(mainCalling("1, 1, 1, 1")), 0);
}

// =========================================================================
// Both build modes (non-negotiable coverage): the §22.16 release default
// (unchecked mode, no --checked-contracts) emits NO invariant
// instrumentation at all, so the same violating construction that raises
// under checked mode instead runs to completion uninstrumented — neither
// ContractViolationException nor DivideByZeroException is observed, and
// `d.gB` reads back the constructed (invariant-violating) value. This pins
// that the within-class-order fix does not accidentally emit checks
// unconditionally.
// =========================================================================
BEZEL_TEST(invariant_declaration_order_4598,
           unchecked_mode_no_instrumentation_spec_22_16) {
    const std::string program =
        std::string(kHierarchy) +
        " class MainEntry "
        "  public static method Main(): int "
        "    using var d := new D(0, 1, 1, 1); "
        "    return d.gB; "
        "  end Main; "
        " end MainEntry;";
    BEZEL_EXPECT_EQ(runUnchecked(program), 0);
}
