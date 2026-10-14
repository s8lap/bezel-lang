// Spec-derived runtime tests for §22.16's DYNAMIC class invariant (v0.338,
// issue #4645).
//
// §22.16 "Class invariants":
//
//   "The **class invariant** of C is the conjunction of every invariant member
//    of C and of every base class, evaluated base-first, declaration order
//    within each class."
//
// §22.16 "The instrumentation set":
//
//   "- **Entry and normal exit** of every `public` instance **method**. Order
//      at entry: `requires` clauses, then the invariant; at exit: `ensures`
//      clauses, then the invariant.
//    - **Entry and normal exit** of every `public` **setter** — block-form,
//      auto-property, indexer, `set(owned)` alike."
//
// §22.16 "The visibility boundary and the `this`-reentrancy exemption":
//
//   "**Dispatch checks the dynamic class's invariant**: an external call
//    through a base-typed reference evaluates the full dynamic class invariant
//    — base and derived conjunction — at the dynamic target's checked face."
//
// THE DEFECT. Every instrumentation point evaluated the conjunction of the
// member's DECLARING class. A method `D` inherits unchanged from `B` therefore
// checked `B`'s clauses only, and every clause `D` adds was skipped — through
// a `B`-typed reference and through a `D`-typed one alike, because both reach
// the same `B::Bump` body. Measured with `bezelc --checked-contracts` on the
// pre-fix compiler: each ★ row below ran to completion (42) where §22.16
// requires ContractViolationException (99).
//
// ORACLE DISCIPLINE. Every violating row has a SATISFIED twin built from the
// same declarations with a different argument, so a row cannot pass by
// raising unconditionally, and the controls pin the three things a careless
// fix would break: sibling/base objects are not over-checked, the
// `this`-reentrancy exemption survives for a member whose own class declares
// no invariant, and release mode still evaluates nothing. `runChecked`
// collapses "did not terminate normally" to -999, so no row passes by
// crashing.
//
// HELD OUT, each for a defect that is NOT this one and reproduces without any
// inheritance (measured on the pre-fix compiler with a single class):
//   * generic classes — no construction-exit or method-boundary check is
//     emitted for a generic class at all (#6204), so a generic subclass's
//     clause cannot be observed at an inherited method (the two generic rows
//     at the end pin only that such a program still compiles and runs);
//   * a `virtual` block-form property setter — the virtual-dispatch store
//     arm is not instrumented even for the declaring class's own clause
//     (#6205);
//   * a subclass declared in an IMPORTING module (#6206) — the recorded
//     cross-module residual, which needs the artifact to carry the invariant
//     (tests/module/xmod_invariant_dynamic_class_4645_test.cpp pins the
//     boundary rows).
//
// Every row runs the real `bezelc` AOT path (fork-isolated
// Driver::compileFile + clang link + native exec).

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

namespace {

// §22.16 checked-contracts MODE — exactly `bezelc --checked-contracts`.
int runChecked(const std::string& program) {
    auto r = bezel::test::runAotCheckedContracts(program);
    return r.ran ? r.exitCode : -999;
}

// §22.16's unchecked (release) mode — the production default (#4386).
int runRelease(const std::string& program) {
    auto r = bezel::test::runAot(program, bezel::codegen::CodegenOptions{});
    return r.ran ? r.exitCode : -999;
}

// `Main`: construct `ctor`, alias it as a `baseTy`-typed `b`, run `stmt`
// (which may use `b` or `d`). 99 = ContractViolationException, 42 = `stmt`
// completed normally.
std::string withMain(const std::string& decls, const std::string& ctor,
                     const std::string& baseTy, const std::string& stmt) {
    return "module M; " + decls +
           " class Prog "
           "  public static method Main(): int "
           "    var r: int := 0; "
           "    using var d := " + ctor + "; "
           "    var b: " + baseTy + " := d; "
           "    try "
           "      " + stmt + " "
           "      r := 42; "
           "    catch ContractViolationException => r := 99; "
           "    end; "
           "    return r; "
           "  end Main; "
           "end Prog;";
}

// The issue's hierarchy: `B` declares a clause the mutation keeps true, `D`
// adds the clause the mutation breaks. `Bump` is inherited, never overridden.
const char* kBaseAndDerivedClause =
    "class B "
    "  public _x: int := 0; "
    "  invariant(_x <= 100); "
    "  public method Bump(v: int): int _x := _x + v; return _x; end Bump; "
    "end B; "
    "class D: B "
    "  invariant(_x >= 0); "
    "end D;";

// `B` declares NO invariant at all — the whole conjunction is the subclass's.
const char* kOnlyDerivedClause =
    "class B "
    "  public _x: int := 0; "
    "  public method Bump(v: int): int _x := _x + v; return _x; end Bump; "
    "  public pure method Ok(): bool return _x >= 0; end Ok; "
    "end B; "
    "class D: B "
    "  invariant(_x >= 0); "
    "end D;";

// The subclass's clause reads a field only the subclass has.
const char* kClauseReadsSubclassField =
    "class B "
    "  public _x: int := 0; "
    "  public method Bump(v: int): int _x := _x + v; return _x; end Bump; "
    "end B; "
    "class D: B "
    "  public _lim: int := 0 - 2; "
    "  invariant(_x >= _lim); "
    "end D;";

// Two levels; the clause sits on the most-derived class.
const char* kTwoLevelLeafClause =
    "class B "
    "  public _x: int := 0; "
    "  invariant(_x <= 100); "
    "  public method Bump(v: int): int _x := _x + v; return _x; end Bump; "
    "end B; "
    "class M: B end M; "
    "class D: M "
    "  invariant(_x >= 0); "
    "end D;";

// Two levels; the clause sits on the MIDDLE class and the object is a leaf
// that declares none of its own.
const char* kTwoLevelMiddleClause =
    "class B "
    "  public _x: int := 0; "
    "  public method Bump(v: int): int _x := _x + v; return _x; end Bump; "
    "end B; "
    "class M: B "
    "  invariant(_x >= 0); "
    "end M; "
    "class D: M end D;";

// Base-first ACROSS the dynamic part of the chain: `M`'s clause guards the
// division in `D`'s. Evaluated derived-first, `_x == -5` divides by zero and
// raises DivideByZeroException, which the `catch` arm does not catch.
const char* kTwoLevelOrder =
    "class B "
    "  public _x: int := 0; "
    "  public method Bump(v: int): int _x := _x + v; return _x; end Bump; "
    "end B; "
    "class M: B "
    "  invariant(_x <> 0 - 5); "
    "end M; "
    "class D: M "
    "  invariant(100 / (_x + 5) >= 0); "
    "end D;";

// A sibling branch carries the clause; the object is of the OTHER branch.
const char* kSiblingClause =
    "class B "
    "  public _x: int := 0; "
    "  public method Bump(v: int): int _x := _x + v; return _x; end Bump; "
    "end B; "
    "class E: B "
    "  invariant(_x >= 0); "
    "end E; "
    "class D: B end D;";

// The SETTER rows of the instrumentation set, each declared by the base.
const char* kBlockSetter =
    "class B "
    "  public _x: int := 0; "
    "  public property Q: int "
    "    get => _x; "
    "    set => do _x := value; end; "
    "  end Q; "
    "end B; "
    "class D: B "
    "  invariant(_x >= 0); "
    "end D;";
const char* kAutoProperty =
    "class B "
    "  public property P: int := 1; "
    "end B; "
    "class D: B "
    "  invariant(P >= 0); "
    "end D;";

// The receiver is an INTERFACE reference; the dynamic target is `B::Bump`.
std::string ifaceProgram(const std::string& arg) {
    return "module M; "
           "interface IBump method Bump(v: int): int; end IBump; "
           "class B: IBump "
           "  public _x: int := 0; "
           "  public method Bump(v: int): int _x := _x + v; return _x; end Bump; "
           "end B; "
           "class D: B "
           "  invariant(_x >= 0); "
           "end D; "
           "class Prog "
           "  public static method Main(): int "
           "    var r: int := 0; "
           "    using var d := new D(); "
           "    var i: IBump := d; "
           "    try "
           "      i.Bump(" + arg + "); "
           "      r := 42; "
           "    catch ContractViolationException => r := 99; "
           "    end; "
           "    return r; "
           "  end Main; "
           "end Prog;";
}

// The ENTRY boundary, isolated: the instance is broken through a non-public
// path (never an instrumentation point) and the inherited public method
// RESTORES the subclass's clause before it returns, so only an entry check
// can see the violation.
const char* kEntryBoundary =
    "class B "
    "  public _x: int := 0; "
    "  public method Reset(): int _x := 1; return _x; end Reset; "
    "end B; "
    "class D: B "
    "  invariant(_x >= 0); "
    "  internal method BreakIt() _x := 0 - 9; end BreakIt; "
    "end D;";

// §22.16's `this`-reentrancy exemption for a member whose OWN class declares
// no invariant: `Work` drives the instance through a state `D`'s clause
// rejects, calls the inherited `Bump` on `this` while it is there, and
// restores the clause before returning.
std::string thisReentry(const std::string& receiver) {
    return std::string(
               "class B "
               "  public _x: int := 0; "
               "  public method Bump(v: int): int _x := _x + v; return _x; "
               "  end Bump; "
               "end B; "
               "class D: B "
               "  invariant(_x >= 0); "
               "  public method Work(): int "
               "    _x := 0 - 7; "
               "    var t := ") +
           receiver +
           "Bump(1); "
           "    _x := 3; "
           "    return t; "
           "  end Work; "
           "end D;";
}

// `D`'s clause calls a `pure` public method it inherits from an
// invariant-less `B`. The clause runs inside `B::Bump`'s checked face; were
// `Ok` to have one face only, evaluating the clause would re-enter it.
const char* kClauseCallsInheritedPure =
    "class B "
    "  public _x: int := 0; "
    "  public method Bump(v: int): int _x := _x + v; return _x; end Bump; "
    "  public pure method Ok(): bool return _x >= 0; end Ok; "
    "end B; "
    "class D: B "
    "  invariant(Ok()); "
    "end D;";

// §22.16 "Calls on *other* instances of the same class check normally": the
// receiver is a parameter, not `this`, although the call is written inside
// the subclass.
const char* kOtherInstance =
    "module M; "
    "class B "
    "  public _x: int := 0; "
    "  public method Bump(v: int): int _x := _x + v; return _x; end Bump; "
    "end B; "
    "class D: B "
    "  invariant(_x >= 0); "
    "  public method Poke(o: B, v: int): int return o.Bump(v); end Poke; "
    "end D; "
    "class Prog "
    "  public static method Main(): int "
    "    var r: int := 0; "
    "    using var d := new D(); "
    "    using var e := new D(); "
    "    try "
    "      d.Poke(e, 0 - 5); "
    "      r := 42; "
    "    catch ContractViolationException => r := 99; "
    "    end; "
    "    return r; "
    "  end Main; "
    "end Prog;";

// The violation message names the class that DECLARES the failed clause.
const char* kMessageNamesDeclaringClass =
    "module M; import System; "
    "class B "
    "  public _x: int := 0; "
    "  invariant(_x <= 100); "
    "  public method Bump(v: int): int _x := _x + v; return _x; end Bump; "
    "end B; "
    "class D: B "
    "  invariant(_x >= 0); "
    "end D; "
    "class Prog "
    "  public static method Main(): int "
    "    using var d := new D(); "
    "    var b: B := d; "
    "    try "
    "      b.Bump(0 - 5); "
    "      Console.WriteLine(\"none\"); "
    "    catch var ex: ContractViolationException => "
    "      Console.WriteLine(ex.Message); "
    "    end; "
    "    return 0; "
    "  end Main; "
    "end Prog;";

const char* kBad = "0 - 5";  // drives `_x` below zero
const char* kOk = "5";       // keeps every clause true

}  // namespace

// ── ★ The issue's shape: an inherited method, base and derived reference ────

BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           inherited_method_through_base_ref_checks_derived_clause_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(withMain(kBaseAndDerivedClause, "new D()",
                                            "B", "b.Bump(0 - 5);")));
    BEZEL_EXPECT_EQ(42, runChecked(withMain(kBaseAndDerivedClause, "new D()",
                                            "B", "b.Bump(5);")));
}

BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           inherited_method_through_derived_ref_checks_derived_clause_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(withMain(kBaseAndDerivedClause, "new D()",
                                            "B", "d.Bump(0 - 5);")));
    BEZEL_EXPECT_EQ(42, runChecked(withMain(kBaseAndDerivedClause, "new D()",
                                            "B", "d.Bump(5);")));
}

BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           base_without_invariant_still_checks_derived_clause_spec_22_16) {
    for (const char* recv : {"b", "d"}) {
        const std::string call = std::string(recv) + ".Bump(";
        BEZEL_EXPECT_EQ(99, runChecked(withMain(kOnlyDerivedClause, "new D()",
                                                "B", call + kBad + ");")));
        BEZEL_EXPECT_EQ(42, runChecked(withMain(kOnlyDerivedClause, "new D()",
                                                "B", call + kOk + ");")));
    }
}

BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           derived_clause_reading_subclass_field_spec_22_16) {
    // `_lim` is -2: -1 keeps `_x >= _lim` true, -5 breaks it — the clause's
    // verdict depends on the subclass field, not on a constant.
    BEZEL_EXPECT_EQ(42, runChecked(withMain(kClauseReadsSubclassField,
                                            "new D()", "B", "b.Bump(0 - 1);")));
    BEZEL_EXPECT_EQ(99, runChecked(withMain(kClauseReadsSubclassField,
                                            "new D()", "B", "b.Bump(0 - 5);")));
}

// ── ★ Two-level hierarchies ────────────────────────────────────────────────

BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           two_level_leaf_clause_through_root_and_middle_ref_spec_22_16) {
    for (const char* baseTy : {"B", "M"}) {
        BEZEL_EXPECT_EQ(99, runChecked(withMain(kTwoLevelLeafClause, "new D()",
                                                baseTy, "b.Bump(0 - 5);")));
        BEZEL_EXPECT_EQ(42, runChecked(withMain(kTwoLevelLeafClause, "new D()",
                                                baseTy, "b.Bump(5);")));
    }
}

BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           two_level_middle_clause_on_leaf_object_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(withMain(kTwoLevelMiddleClause, "new D()",
                                            "B", "b.Bump(0 - 5);")));
    BEZEL_EXPECT_EQ(42, runChecked(withMain(kTwoLevelMiddleClause, "new D()",
                                            "B", "b.Bump(5);")));
}

BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           dynamic_clauses_evaluate_base_first_spec_22_16) {
    // -5: `M`'s clause is false and must report BEFORE `D`'s divides by zero.
    BEZEL_EXPECT_EQ(99, runChecked(withMain(kTwoLevelOrder, "new D()", "B",
                                            "b.Bump(0 - 5);")));
    // -7: `M`'s clause holds, `D`'s is 100 / -2 = -50, false.
    BEZEL_EXPECT_EQ(99, runChecked(withMain(kTwoLevelOrder, "new D()", "B",
                                            "b.Bump(0 - 7);")));
    BEZEL_EXPECT_EQ(42, runChecked(withMain(kTwoLevelOrder, "new D()", "B",
                                            "b.Bump(5);")));
}

// ── ★ The setter rows of the instrumentation set ───────────────────────────

BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           inherited_block_setter_checks_derived_clause_spec_22_16) {
    for (const char* recv : {"b", "d"}) {
        const std::string store = std::string(recv) + ".Q := ";
        BEZEL_EXPECT_EQ(99, runChecked(withMain(kBlockSetter, "new D()", "B",
                                                store + kBad + ";")));
        BEZEL_EXPECT_EQ(42, runChecked(withMain(kBlockSetter, "new D()", "B",
                                                store + kOk + ";")));
    }
}

BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           inherited_auto_property_store_checks_derived_clause_spec_22_16) {
    for (const char* recv : {"b", "d"}) {
        const std::string store = std::string(recv) + ".P := ";
        BEZEL_EXPECT_EQ(99, runChecked(withMain(kAutoProperty, "new D()", "B",
                                                store + kBad + ";")));
        BEZEL_EXPECT_EQ(42, runChecked(withMain(kAutoProperty, "new D()", "B",
                                                store + kOk + ";")));
    }
}

// ── ★ Interface reference, entry boundary, other instance ──────────────────

BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           call_through_interface_ref_checks_derived_clause_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(ifaceProgram(kBad)));
    BEZEL_EXPECT_EQ(42, runChecked(ifaceProgram(kOk)));
}

BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           entry_boundary_of_inherited_method_checks_derived_clause_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(withMain(kEntryBoundary, "new D()", "B",
                                            "d.BreakIt(); b.Reset();")));
    BEZEL_EXPECT_EQ(42, runChecked(withMain(kEntryBoundary, "new D()", "B",
                                            "b.Reset();")));
}

BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           other_instance_receiver_inside_subclass_checks_normally_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(kOtherInstance));
}

BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           clause_calling_inherited_pure_method_does_not_recurse_spec_22_16) {
    BEZEL_EXPECT_EQ(99, runChecked(withMain(kClauseCallsInheritedPure,
                                            "new D()", "B", "b.Bump(0 - 5);")));
    BEZEL_EXPECT_EQ(42, runChecked(withMain(kClauseCallsInheritedPure,
                                            "new D()", "B", "b.Bump(5);")));
}

BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           violation_message_names_the_declaring_subclass_spec_22_16) {
    auto r = bezel::test::runAotCheckedContracts(kMessageNamesDeclaringClass);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_TRUE(r.stdoutText.find("invariant of D failed") !=
                      std::string::npos);
}

// ── Controls: what the fix must NOT change ─────────────────────────────────

// An object of a class that declares no such clause is not checked against a
// SIBLING branch's clause, and a plain base object is not checked against a
// subclass's. 42 on both sides of the fix.
BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           sibling_and_base_objects_are_not_over_checked_spec_22_16) {
    BEZEL_EXPECT_EQ(42, runChecked(withMain(kSiblingClause, "new D()", "B",
                                            "b.Bump(0 - 5);")));
    BEZEL_EXPECT_EQ(42, runChecked(withMain(kSiblingClause, "new B()", "B",
                                            "b.Bump(0 - 5);")));
    // …and the sibling that DOES declare it is.
    BEZEL_EXPECT_EQ(99, runChecked(withMain(kSiblingClause, "new E()", "B",
                                            "b.Bump(0 - 5);")));
}

// §22.16 "A call whose receiver is `this` — explicit or implicit — from inside
// any instance member of the class or a subclass enters the member's
// unchecked face". 42 on both sides of the fix: pre-fix `B::Bump` checked
// nothing; post-fix it has a checked face, and these calls must not reach it.
BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           this_receiver_call_to_inherited_method_stays_unchecked_spec_22_16) {
    for (const char* receiver : {"", "this.", "base."}) {
        BEZEL_EXPECT_EQ(42, runChecked(withMain(thisReentry(receiver),
                                                "new D()", "B", "d.Work();")));
    }
}

// §22.16 "in unchecked (release) mode — the default — unproven `ensures` and
// `invariant` clauses are not evaluated".
BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           release_mode_evaluates_no_derived_clause_spec_22_16) {
    BEZEL_EXPECT_EQ(42, runRelease(withMain(kBaseAndDerivedClause, "new D()",
                                            "B", "b.Bump(0 - 5);")));
    BEZEL_EXPECT_EQ(42, runRelease(withMain(kBlockSetter, "new D()", "B",
                                            "b.Q := 0 - 5;")));
}

// ── Generic subclasses are not arms (#6204) ─────────────────────────────────
// A generic class carries a per-instantiation descriptor, so it cannot be
// matched as an arm, and lowering its clause inside the base member's body
// left `T` unbound: a clause using a universal member on `T` turned a program
// that compiles in checked mode into a compile error ("member `Equals` on `T`
// is declared via the universal Object contract ..."). Every instance below
// satisfies its clause, so §22.16 requires normal completion whatever #6204's
// eventual fix evaluates; the rows pin that the program compiles and runs.
BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           generic_subclass_clause_with_equals_on_t_compiles_checked_spec_22_16) {
    auto r = bezel::test::runAotCheckedContracts(
        "module M; "
        "class B "
        "  public _x: int := 0; "
        "  public method Bump(v: int): int _x := _x + v; return _x; end Bump; "
        "end B; "
        "class D<T>: B where T: hashable "
        "  public _v: T; "
        "  public _w: T; "
        "  public constructor(v: T, w: T) _v := v; _w := w; end constructor; "
        "  invariant(_v.Equals(_w) or _x >= 0); "
        "end D; "
        "class Prog "
        "  public static method Run(b: B, v: int): int "
        "    var r: int := 0; "
        "    try "
        "      b.Bump(v); "
        "      r := 4; "
        "    catch ContractViolationException => r := 9; "
        "    end; "
        "    return r; "
        "  end Run; "
        "  public static method Main(): int "
        "    using var d := new D<int>(3, 3); "
        "    using var s := new D<string>(\"a\", \"a\"); "
        "    using var t := new D<long>(3L, 3L); "
        "    Console.WriteLine(Prog.Run(d, 0 - 5)); "
        "    Console.WriteLine(Prog.Run(s, 0 - 5)); "
        "    Console.WriteLine(Prog.Run(t, 0 - 5)); "
        "    return 0; "
        "  end Main; "
        "end Prog;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, std::string("4\n4\n4\n"));
}

BEZEL_TEST(invariant_dynamic_class_conjunction_4645,
           generic_subclass_clause_with_tostring_on_t_compiles_checked_spec_22_16) {
    BEZEL_EXPECT_EQ(42, runChecked(
        "module M; "
        "class B "
        "  public _x: int := 0; "
        "  public method Bump(v: int): int _x := _x + v; return _x; end Bump; "
        "end B; "
        "class D<T>: B "
        "  public _v: T; "
        "  public constructor(v: T) _v := v; end constructor; "
        "  invariant(_v.ToString().Length >= 0); "
        "end D; "
        "class Prog "
        "  public static method Main(): int "
        "    using var a := new D<int>(1); "
        "    var b: B := a; "
        "    var r: int := 0; "
        "    try b.Bump(1); r := 42; catch ContractViolationException => r := 99; end; "
        "    return r; "
        "  end Main; "
        "end Prog;"));
}
