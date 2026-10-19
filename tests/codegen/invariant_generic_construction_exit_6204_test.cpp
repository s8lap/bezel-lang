// Spec-derived runtime tests for §22.16's CONSTRUCTION-EXIT row on a
// CONSTRUCTED GENERIC class (v0.338, issue #6204).
//
// §22.16 "The instrumentation set" (v0.338):
//
//   "- **Construction exit**: the normal completion of the **most-derived
//      constructor** of a construction, of any visibility, after its body and
//      its `ensures` clauses."
//
// THE DEFECT. Construction exit is wired at the `new` SITE, and that site has
// two arms: an explicit constructor is either CALLED (`builder_.CreateCall`)
// or INLINED into the allocating frame (`inlineCtorBody`). The call arm emits
// `emitInvariantCheck`; the inline arm did not. The arm is chosen by
// `needsInlineForLayout = !genericSubst_.empty()` — so EVERY constructed
// generic took the inline arm and §22.16's construction-exit row was silently
// absent for every generic construction, whatever its arity and however
// public its constructor.
//
// MEASURED, `bezelc --checked-contracts`, on the pre-fix compiler
// (r30-c1 @ f468f8a9): `class D<T>` carrying `invariant(_x >= 0);` with a
// one-argument constructor, `new D<int>(0 - 1)` inside a
// `try`/`catch ContractViolationException`, completed normally and returned
// **42** where §22.16 requires **99**. Deleting `<T>` from that one
// declaration — nothing else changes — makes the same program raise (99).
//
// WHAT THIS FILE DOES NOT PIN. The OTHER half of #6204 — §22.16's public
// METHOD boundary on a generic class — is NOT fixed by this commit and is not
// asserted here. A generic class's live body is the per-instantiation
// specialization `buildSpecializedMethodFn` (LlvmBackend_Calls2.cpp), which
// emits the body directly and never reaches `emitFunctionBody`'s §22.16
// prologue/epilogue; instrumenting it needs an unchecked FACE of the
// specialization (`uncheckedFaceFor` clones `methodFns_[ms]`, the dead erased
// template, which a specialization never calls), or §22.16's `this`-
// reentrancy exemption is lost and a generic public method that legitimately
// drives its own instance through a broken intermediate state over-checks.
// That half is filed as its own issue; this file's `method_boundary_*` rows
// pin the CURRENT behaviour so the deferral is visible rather than silent.
//
// ORACLE DISCIPLINE. Every violating row has a SATISFIED twin built from the
// same declarations with a different argument, so a row cannot pass by raising
// unconditionally. `runChecked` collapses "did not terminate normally" to
// -999, so no row passes by crashing. Every row runs on
// `runAotCheckedContracts`, whose `detectLeaks=true` (aot_support.hpp) arms
// LSan: the construction-exit throw edge is the §22.9 unwind shape (Dispose +
// free of the partially-constructed instance), so a missing or doubled
// disposal there is exactly what LSan is there to catch, and the looped row
// runs the edge 200 times because a single-shot leak repro hides under the
// sanitizer's noise.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

// §22.16 checked-contracts MODE — exactly `bezelc --checked-contracts`, and
// the only oracle that also arms LSan.
int runChecked(const std::string& program) {
    auto r = bezel::test::runAotCheckedContracts(program);
    return r.ran ? r.exitCode : -999;
}

// §22.16's unchecked (release) mode — the production default (#4386).
int runRelease(const std::string& program) {
    auto r = bezel::test::runAot(program, bezel::codegen::CodegenOptions{});
    return r.ran ? r.exitCode : -999;
}

// A `Prog` that constructs `new D<int>(arg)` inside a try/catch and returns
// 42 (the construction completed) or 99 (ContractViolationException).
// `ctor` names the `new` expression's type arguments so the same helper
// serves the non-generic control (`""`).
std::string mainFor(const std::string& decls, const std::string& arg,
                    const std::string& targs = "<int>") {
    return "module M; " + decls +
           " class Prog "
           "  public static method Main(): int "
           "    var r: int := 0; "
           "    try "
           "      using var d := new D" + targs + "(" + arg + "); "
           "      r := 42; "
           "    catch ContractViolationException => r := 99; "
           "    end; "
           "    return r; "
           "  end Main; "
           "end Prog;";
}

// The issue's class, generic and non-generic. `clause` sits on the class; the
// constructor takes the value that violates or satisfies it.
std::string genericDecl(const std::string& clause) {
    return "class D<T> "
           "  public _x: int := 0; " + clause +
           "  public constructor(v: int) _x := v; end constructor; "
           "end D; ";
}
std::string nonGenericDecl(const std::string& clause) {
    return "class D "
           "  public _x: int := 0; " + clause +
           "  public constructor(v: int) _x := v; end constructor; "
           "end D; ";
}

const char* kBad = "0 - 1";  // drives `_x` below zero
const char* kOk = "1";       // keeps the clause true

}  // namespace

// ── ★ The issue's shape ────────────────────────────────────────────────────

BEZEL_TEST(invariant_generic_construction_exit_6204,
           generic_construction_exit_checks_clause_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(mainFor(genericDecl("invariant(_x >= 0);"),
                                           kBad)));
    BEZEL_EXPECT_EQ(42, runChecked(mainFor(genericDecl("invariant(_x >= 0);"),
                                           kOk)));
}

// The control that identifies the defect: the SAME declarations without
// `<T>`. This program was already 99 on the pre-fix compiler — it is what
// makes "the `T` is the difference" a measurement rather than an assertion.
BEZEL_TEST(invariant_generic_construction_exit_6204,
           non_generic_construction_exit_checks_clause_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(mainFor(nonGenericDecl("invariant(_x >= 0);"),
                                           kBad, "")));
    BEZEL_EXPECT_EQ(42, runChecked(mainFor(nonGenericDecl("invariant(_x >= 0);"),
                                           kOk, "")));
}

// §22.16 "the conjunction of every invariant member of C and of every base
// class" — a constructed generic whose SUBCLASS adds the clause the
// construction breaks. Both halves must run, and the subclass half is the one
// this path had never reached.
BEZEL_TEST(invariant_generic_construction_exit_6204,
           generic_subclass_clause_checked_at_construction_exit_spec_22_16) {
    const std::string decls =
        "class B<T> "
        "  public _x: int := 0; "
        "  invariant(_x <= 100); "
        "  public constructor() _x := 0; end constructor; "
        "end B; "
        "class D<T>: B<T> "
        "  public constructor(n: int) _x := n; end constructor; "
        "  invariant(_x >= 0); "
        "end D; ";
    // -101 breaks the BASE clause, which the subclass clause also breaks; the
    // satisfied twin keeps both.
    auto prog = [&](const char* arg) {
        return "module M; " + decls +
               " class Prog "
               "  public static method Main(): int "
               "    var r: int := 0; "
               "    try "
               "      using var d := new D<int>(" + arg + "); "
               "      r := 42; "
               "    catch ContractViolationException => r := 99; "
               "    end; "
               "    return r; "
               "  end Main; "
               "end Prog;";
    };
    BEZEL_EXPECT_EQ(99, runChecked(prog(kBad)));
    BEZEL_EXPECT_EQ(42, runChecked(prog("5")));
}

// The clause reads a field whose type is the CLASS'S OWN TYPE PARAMETER, so
// its verdict depends on the substitution, not on a constant. This is the
// shape the #4645 regression test records as unlowerable inside a base
// member's body; at the `new` site the substitution is active, so it lowers
// and runs. Two instantiations, each with a satisfied and a violating value.
BEZEL_TEST(invariant_generic_construction_exit_6204,
           generic_clause_reading_a_t_typed_field_spec_22_16) {
    const std::string decls =
        "class D<T> "
        "  public _v: T; "
        "  public _x: int := 0; "
        "  invariant(_v.ToString().Length >= 0 and _x >= 0); "
        "  public constructor(v: T, n: int) _v := v; _x := n; "
        "  end constructor; "
        "end D; ";
    auto prog = [&](const char* v, const char* n) {
        return "module M; " + decls +
               " class Prog "
               "  public static method Main(): int "
               "    var r: int := 0; "
               "    try "
               "      using var d := new D<int>(" + v + ", " + n + "); "
               "      r := 42; "
               "    catch ContractViolationException => r := 99; "
               "    end; "
               "    return r; "
               "  end Main; "
               "end Prog;";
    };
    // The satisfied row reads the substituted `_v`; the violating row breaks
    // the `int` half, so a fix that ignored the substitution would still see
    // the second one and the pair would not distinguish it.
    BEZEL_EXPECT_EQ(42, runChecked(prog("7", "3")));
    BEZEL_EXPECT_EQ(99, runChecked(prog("7", kBad)));
}

// §22.16 "throw exits check nothing": a constructor that raises must surface
// ITS OWN exception, not the invariant's. 7 is the ctor's own arm; had the
// construction-exit check been emitted on the throw edge it would have fired
// first and the program would have returned 99.
BEZEL_TEST(invariant_generic_construction_exit_6204,
           throwing_constructor_is_not_reached_by_the_exit_check_spec_22_16) {
    BEZEL_EXPECT_EQ(
        7, runChecked("module M; "
                      "class D<T> "
                      "  public _x: int := 0; "
                      "  invariant(_x >= 0); "
                      "  public constructor(v: int) "
                      "    _x := v; "
                      "    _x := 100 / (v + 1); "
                      "  end constructor; "
                      "end D; "
                      "class Prog "
                      "  public static method Main(): int "
                      "    var r: int := 0; "
                      "    try "
                      "      using var d := new D<int>(0 - 1); "
                      "      r := 42; "
                      "    catch ContractViolationException => r := 99; "
                      "    catch DivideByZeroException => r := 7; "
                      "    end; "
                      "    return r; "
                      "  end Main; "
                      "end Prog;"));
}

// The §22.9 unwind shape of the construction-exit throw edge: the partially
// constructed instance is disposed AND freed before the exception propagates,
// and the enclosing `using` binding does not then dispose it a second time.
// Looped 200× because a one-shot leak repro hides under LSan noise; each
// iteration also owns a 32-byte `string`, so a missing free would be tens of
// kilobytes of direct leak, not a rounding error.
BEZEL_TEST(invariant_generic_construction_exit_6204,
           construction_exit_throw_edge_disposes_the_instance_spec_22_16) {
    auto r = bezel::test::runAotCheckedContracts(
        "module M; "
        "class D<T> "
        "  public _s: string := \"abcdefghijklmnopqrstuvwxyz\"; "
        "  public _x: int := 0; "
        "  invariant(_x >= 0); "
        "  public constructor(v: int) _x := v; end constructor; "
        "end D; "
        "class Prog "
        "  public static method Main(): int "
        "    var hits: int := 0; "
        "    var i: int := 0; "
        "    while i < 200 do "
        "      try "
        "        using var d := new D<int>(0 - 1); "
        "      catch ContractViolationException => hits := hits + 1; "
        "      end; "
        "      i := i + 1; "
        "    end; "
        "    return hits; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(200, r.exitCode);
}

// ── Controls: what the fix must NOT change ─────────────────────────────────
// §22.16 "in unchecked (release) mode — the default — unproven `ensures` and
// `invariant` clauses are not evaluated". 42 was the pre-fix answer here too,
// but only because the check was absent everywhere; this row is what keeps a
// later widening of the emission sites from reaching release mode.
BEZEL_TEST(invariant_generic_construction_exit_6204,
           release_mode_evaluates_no_generic_clause_spec_22_16) {
    BEZEL_EXPECT_EQ(42, runRelease(mainFor(genericDecl("invariant(_x >= 0);"),
                                           kBad)));
}

// A class with no clause anywhere in its chain is not on an instrumented
// boundary. 43 = `_x` (-1) + 44, so the constructor really ran and stored.
BEZEL_TEST(invariant_generic_construction_exit_6204,
           clause_less_generic_is_not_checked_spec_22_16) {
    BEZEL_EXPECT_EQ(
        43, runChecked("module M; " + genericDecl("") +
                       " class Prog "
                       "  public static method Main(): int "
                       "    using var d := new D<int>(0 - 1); "
                       "    return d._x + 44; "
                       "  end Main; "
                       "end Prog;"));
}

// ── The DEFERRED half: §22.16's public METHOD boundary on a generic ────────
// These two rows pin the CURRENT behaviour of the boundary this commit does
// NOT restore, so the gap is visible in the suite instead of only in an
// issue. They are NOT claims about what §22.16 requires — §22.16 requires 99
// for both, and the companion row below records the pre-fix compiler's
// agreement.
BEZEL_TEST(invariant_generic_construction_exit_6204,
           generic_method_boundary_is_still_uninstrumented_spec_22_16) {
    BEZEL_EXPECT_EQ(
        42, runChecked("module M; "
                       "class D<T> "
                       "  public _x: int := 0; "
                       "  invariant(_x >= 0); "
                       "  public constructor(v: int) _x := v; "
                       "  end constructor; "
                       "  public method Bump(v: int): int "
                       "    _x := _x + v; return _x; "
                       "  end Bump; "
                       "end D; "
                       "class Prog "
                       "  public static method Main(): int "
                       "    var r: int := 0; "
                       "    using var d := new D<int>(0); "
                       "    try "
                       "      d.Bump(0 - 5); "
                       "      r := 42; "
                       "    catch ContractViolationException => r := 99; "
                       "    end; "
                       "    return r; "
                       "  end Main; "
                       "end Prog;"));
    // The non-generic twin of the SAME declarations raises — the boundary is
    // present there and absent here, which is the whole of the deferral.
    BEZEL_EXPECT_EQ(
        99, runChecked("module M; "
                       "class D "
                       "  public _x: int := 0; "
                       "  invariant(_x >= 0); "
                       "  public constructor(v: int) _x := v; "
                       "  end constructor; "
                       "  public method Bump(v: int): int "
                       "    _x := _x + v; return _x; "
                       "  end Bump; "
                       "end D; "
                       "class Prog "
                       "  public static method Main(): int "
                       "    var r: int := 0; "
                       "    using var d := new D(0); "
                       "    try "
                       "      d.Bump(0 - 5); "
                       "      r := 42; "
                       "    catch ContractViolationException => r := 99; "
                       "    end; "
                       "    return r; "
                       "  end Main; "
                       "end Prog;"));
}

// §22.16's `requires` row is absent on a generic class for the same reason
// (`buildSpecializedMethodFn` emits no prologue), and `requires` is
// ALWAYS-ON (§22.16 "Failure semantics"), so this is not a checked-contracts
// artefact. Recorded here for the same reason as the row above.
BEZEL_TEST(invariant_generic_construction_exit_6204,
           generic_requires_clause_is_still_unemitted_spec_22_16) {
    BEZEL_EXPECT_EQ(
        42, runRelease("module M; "
                       "class D<T> "
                       "  public _x: int := 0; "
                       "  public method Bump(v: int): int "
                       "    requires(v > 0) "
                       "    _x := _x + v; return _x; "
                       "  end Bump; "
                       "end D; "
                       "class Prog "
                       "  public static method Main(): int "
                       "    var r: int := 0; "
                       "    using var d := new D<int>(); "
                       "    try "
                       "      d.Bump(0 - 5); "
                       "      r := 42; "
                       "    catch ContractViolationException => r := 99; "
                       "    end; "
                       "    return r; "
                       "  end Main; "
                       "end Prog;"));
    // Non-generic twin: the SAME program in release mode raises, because
    // `requires` is evaluated in every build mode.
    BEZEL_EXPECT_EQ(
        99, runRelease("module M; "
                       "class D "
                       "  public _x: int := 0; "
                       "  public method Bump(v: int): int "
                       "    requires(v > 0) "
                       "    _x := _x + v; return _x; "
                       "  end Bump; "
                       "end D; "
                       "class Prog "
                       "  public static method Main(): int "
                       "    var r: int := 0; "
                       "    using var d := new D(); "
                       "    try "
                       "      d.Bump(0 - 5); "
                       "      r := 42; "
                       "    catch ContractViolationException => r := 99; "
                       "    end; "
                       "    return r; "
                       "  end Main; "
                       "end Prog;"));
}