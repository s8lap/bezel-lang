// Spec-derived tests for issue #4418 -- §28.9 (v0.357): the trivial-dispose
// ceremony exemption covers EVERY holder form the §28.3 must-use / auto-drop
// warning attaches to, not only a `new`-bound plain-`var` local.
//
// THE RULE (§28.9, v0.357): "No must-use / auto-drop diagnostic attaches to a
// trivial-dispose value. The §28.3 must-use warning is not emitted at the
// implicit-drop site of a plain-`var` owned local, of an undischarged `owned`
// parameter -- a method's, an iterator's (§29.8), or an async method's (§30.1)
// -- or of an owning iteration variable (§29.2) whose type is trivial-dispose".
// The classification key is "read from the produced type where the drop
// site's value has a statically-known producer (a `new` expression, an
// `owned`-returning call), and from the declared type otherwise".
//
// Each positive row is one holder form that drew the warning before the fix:
//   * a plain-`var` local bound to an `owned`-returning CALL (the produced
//     type is the call's, the study item B2 shape);
//   * an owning iteration variable over a trivial-dispose element type;
//   * an undischarged `owned` parameter of an ITERATOR method (§29.8);
//   * an undischarged `owned` parameter of an ASYNC method (§30.1).
// The exemption is over the diagnostic only: the scope-end / frame free is
// unchanged (the flags are set above each gate). Controls that keep the
// warning (a destructor-bearing class; a trivial `Base` declared over a
// destructor-bearing producer) are corpus candidates; the two kept here pin
// the value-keyed direction.

#include "support/ownership_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAndCheckOwnership;

namespace {

// Any §28.3 auto-drop must-use warning naming `name`: the local ("end of its
// scope"), loop-variable ("end of each loop iteration") and the frame
// parameter ("by the frame at teardown") spellings alike.
bool hasMustUseWarning(const bezel::test::OwnershipResult& r,
                       const std::string& name) {
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Warning) continue;
        if (d.message.find("must-use / auto-drop") == std::string::npos)
            continue;
        if (d.message.find("`" + name + "`") != std::string::npos) return true;
    }
    return false;
}

// A trivial-dispose class (§28.9: no destructor, no owned fields, no base).
const char* kTrivial =
    "class K "
    "  public constructor(x: int) _x := x; end constructor; "
    "  private _x: int; "
    "  public method X(): int => _x; "
    "end K; ";

// A trivial-dispose `Base` with a destructor-bearing `Derived` (NOT trivial).
const char* kBaseDerived =
    "class Base "
    "  public constructor() end constructor; "
    "  public method X(): int => 1; "
    "end Base; "
    "class Derived: Base "
    "  public constructor() base(); end constructor; "
    "  destructor end destructor; "
    "end Derived; ";

// The destructor-bearing twin: NOT trivial-dispose.
const char* kNonTrivial =
    "class D "
    "  public constructor(x: int) _x := x; end constructor; "
    "  destructor end destructor; "
    "  private _x: int; "
    "  public method X(): int => _x; "
    "end D; ";

}  // namespace

// §28.9 v0.357 / study item B2: a plain `var` bound to a call of an
// `owned Array<string>`-returning method (a trivial-dispose borrowing
// collection) draws no must-use warning.
BEZEL_TEST(trivial_dispose_every_holder_form_4418,
           call_bound_local_of_trivial_collection_is_silent_spec_28_9) {
    auto r = parseAndCheckOwnership(
        "module M; class C "
        "  private static method Mk(): owned Array<string> "
        "    var a := new Array<string>(); a.Add(\"x\"); return a; "
        "  end Mk; "
        "  public static method Main(): int "
        "    var f := Mk(); return f.Count; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMustUseWarning(r, "f"));
}

// The same holder form at a user trivial-dispose class: the produced type is
// the call's `owned K`.
BEZEL_TEST(trivial_dispose_every_holder_form_4418,
           call_bound_local_of_trivial_user_class_is_silent_spec_28_9) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kTrivial +
        "class C "
        "  private static method Mk(): owned K "
        "    return new K(1); "
        "  end Mk; "
        "  public static method Main(): int "
        "    var k := Mk(); return k.X(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMustUseWarning(r, "k"));
}

// §28.9 v0.357, the produced-type arm (control 1 of 2): a trivial-dispose
// DECLARED `Base` over a call returning a destructor-bearing `owned Derived`
// is not exempt -- "Where a producer is present, its produced type decides in
// both directions whatever the declared type is". Green on r30-c1 too; it is
// RED under the plausible wrong fix that keys the call arm on the local's
// declared type (`t = ls->declType` for a CallExpr initializer).
BEZEL_TEST(trivial_dispose_every_holder_form_4418,
           call_bound_local_nontrivial_producer_keeps_warning_spec_28_9) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kBaseDerived +
        "class C "
        "  private static method MkD(): owned Derived "
        "    return new Derived(); "
        "  end MkD; "
        "  public static method Main(): int "
        "    var b: Base := MkD(); return b.X(); "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMustUseWarning(r, "b"));
}

// §28.9 v0.357 / §29.2: an owning iteration variable of trivial-dispose
// element type draws no must-use warning (the issue's own repro).
BEZEL_TEST(trivial_dispose_every_holder_form_4418,
           owning_iteration_variable_of_trivial_type_is_silent_spec_29_2) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kTrivial +
        "class C "
        "  public static method Main(): int "
        "    using var arr := new OwningArray<K>(); "
        "    arr.Add(new K(1)); "
        "    for var k in arr.Drain() do "
        "      var z := 1; "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMustUseWarning(r, "k"));
}

// §28.9 v0.357 / §29.8: an undischarged `owned` parameter of an ITERATOR
// method, of trivial-dispose type, draws no must-use warning.
BEZEL_TEST(trivial_dispose_every_holder_form_4418,
           iterator_owned_param_of_trivial_type_is_silent_spec_29_8) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kTrivial +
        "class C "
        "  public static method Gen(owned k: K): owned IEnumerable<int> "
        "    yield return 1; "
        "  end Gen; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMustUseWarning(r, "k"));
}

// §28.9 v0.357 / §30.1: the same at an ASYNC method.
BEZEL_TEST(trivial_dispose_every_holder_form_4418,
           async_owned_param_of_trivial_type_is_silent_spec_30_1) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kTrivial +
        "class C "
        "  public static async method Run(owned k: K): Task<int> "
        "    return 1; "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMustUseWarning(r, "k"));
}

// Control 2 of 2, for the frame-parameter arms: a destructor-bearing `owned`
// parameter of an iterator keeps its warning (the exemption is value-keyed,
// not a blanket drop of the frame lint). Green on r30-c1 too; it is RED under
// the plausible wrong fix that silences the iterator frame's must-use warning
// unconditionally instead of through `ownedParamIsTrivialDispose`.
BEZEL_TEST(trivial_dispose_every_holder_form_4418,
           iterator_owned_param_of_nontrivial_type_keeps_warning_spec_29_8) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kNonTrivial +
        "class C "
        "  public static method Gen(owned d: D): owned IEnumerable<int> "
        "    yield return 1; "
        "  end Gen; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasMustUseWarning(r, "d"));
}

// §28.9 v0.357: the NESTED-SCOPE fallback arm (a plain-`var` owned local
// declared in an inner scope, in a method whose only exit is a throw, so it
// never reaches the function-exit endState loop) carries the same exemption
// -- for the `new`-bound AND the call-bound shapes.
BEZEL_TEST(trivial_dispose_every_holder_form_4418,
           nested_scope_throw_only_exit_locals_of_trivial_type_are_silent_spec_28_9) {
    auto r = parseAndCheckOwnership(
        std::string("module M; ") + kTrivial +
        "class C "
        "  public static method Mk(): owned K return new K(2); end Mk; "
        "  public static method Run(c: bool): int "
        "    if c then "
        "      var v := new K(1); "
        "      var w := Mk(); "
        "    end; "
        "    throw new Exception(\"x\"); "
        "  end Run; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_FALSE(hasMustUseWarning(r, "v"));
    BEZEL_EXPECT_FALSE(hasMustUseWarning(r, "w"));
}

// ---------------------------------------------------------------------------
// fix-forward #4418 (review r42-rev-p7-4418, REJECT 1): a WRAPPED producer.
// §28.3.4's producers table classifies every owned initializer, and §28.9
// keys the exemption on the produced type "where the drop site's value has a
// statically-known producer": "Where a producer is present, its produced type
// decides in both directions whatever the declared type is: `var b: Base :=
// new Derived();` above is not exempt". The rejected first cut recognised only
// a BARE `new` / call initializer and keyed every other form on the local's
// declared (or inferred) type, so each row below -- a destructor-bearing
// `Derived` production under a trivial `Base` -- lost its warning. Each row is
// RED on a869c719d (the rejected first cut) and green on r30-c1 and here.
// ---------------------------------------------------------------------------

namespace {
std::string baseDerivedProg(const std::string& members) {
    return std::string("module M; ") + kBaseDerived + "class C " + members +
           "end C;";
}
}  // namespace

// §28.3.4 row `x!`: "a fresh producer's `!` is owned" -- the producer is the
// call, whose produced type is the destructor-bearing `Derived`.
BEZEL_TEST(trivial_dispose_every_holder_form_4418,
           bang_over_call_producing_derived_under_trivial_base_keeps_warning_spec_28_9) {
    auto r = parseAndCheckOwnership(baseDerivedProg(
        "  private static method MkDN(): owned Derived? => new Derived(); "
        "  public static method Main(): int "
        "    var b: Base := MkDN()!; return b.X(); "
        "  end Main; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMustUseWarning(r, "b"));
}

// §28.3.4 row `(if c then v1 else v2)`: the meet of the arms -- one arm is a
// destructor-bearing production, so the declared `Base` does not exempt it.
BEZEL_TEST(trivial_dispose_every_holder_form_4418,
           if_join_with_derived_arm_under_declared_trivial_base_keeps_warning_spec_28_9) {
    auto r = parseAndCheckOwnership(baseDerivedProg(
        "  public static method Main(c: bool): int "
        "    var b: Base := (if c then new Derived() else new Base()); "
        "    return b.X(); "
        "  end Main; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMustUseWarning(r, "b"));
}

// The same join with NO annotation: the INFERRED join type is the trivial
// `Base`, and it still does not decide over the `new Derived()` arm.
BEZEL_TEST(trivial_dispose_every_holder_form_4418,
           if_join_with_derived_arm_inferred_trivial_base_keeps_warning_spec_28_9) {
    auto r = parseAndCheckOwnership(baseDerivedProg(
        "  public static method Main(c: bool): int "
        "    var b := (if c then new Derived() else new Base()); "
        "    return b.X(); "
        "  end Main; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMustUseWarning(r, "b"));
}

// §28.3.4 row `a ?? b`: the meet of both operands' productions.
BEZEL_TEST(trivial_dispose_every_holder_form_4418,
           coalesce_of_derived_productions_under_trivial_base_keeps_warning_spec_28_9) {
    auto r = parseAndCheckOwnership(baseDerivedProg(
        "  private static method MkDN(): owned Derived? => new Derived(); "
        "  public static method Main(): int "
        "    var b: Base := MkDN() ?? new Derived(); return b.X(); "
        "  end Main; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMustUseWarning(r, "b"));
}

// §28.3.4 row expression-`case`: "the meet ranges over every arm's result
// expression" -- one destructor-bearing arm keeps the warning.
BEZEL_TEST(trivial_dispose_every_holder_form_4418,
           case_join_with_derived_arm_under_trivial_base_keeps_warning_spec_28_9) {
    auto r = parseAndCheckOwnership(baseDerivedProg(
        "  public static method Main(n: int): int "
        "    var b: Base := (case n of 0 => new Base(), _ => new Derived()); "
        "    return b.X(); "
        "  end Main; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMustUseWarning(r, "b"));
}

// `move d` over `d := new Derived()`: the transferred value's key is d's own
// producer (the `new Derived()`), not the receiving binding's declared `Base`.
// (The reviewer recorded the declared-vs-value-keyed reading of a `move`
// initializer as a spec tension on #4418 for the docs session; this keeps the
// r30-c1 behaviour, and 3709's `var y := move v` over `new StringBuilder()`
// stays silent because v's producer is trivial.)
BEZEL_TEST(trivial_dispose_every_holder_form_4418,
           move_of_derived_local_into_trivial_base_keeps_warning_spec_28_9) {
    auto r = parseAndCheckOwnership(baseDerivedProg(
        "  public static method Main(): int "
        "    var d := new Derived(); var b: Base := move d; return b.X(); "
        "  end Main; "));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMustUseWarning(r, "b"));
}
