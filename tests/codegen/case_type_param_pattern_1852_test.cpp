// fix #1852 / §19.3 (v0.182): a `case`/`when` TYPE-TEST pattern whose target is
// a bare TYPE PARAMETER of the enclosing generic body — `case x of T` and the
// typed-binding form `var x: T` — was not lowered by codegen. §19.3 (v0.182):
// "in a generic body, a type parameter may be used as a pattern type — `var x:
// T` and bare `T` test against the SUBSTITUTED type argument, per instantiation,
// the same rule as catch arms." Sema accepts the forms (spec-valid), but the
// backend resolved `T` to none of {enum-const, named-const, class/interface
// descriptor, record/enum type}:
//   * bare `case x of T` fell to the #1847 fallback and emitted a clean-but-
//     wrong "(§19.3) not supported by the backend" diagnostic (a malformed-IR
//     ICE before #1847's guard landed) — the pattern never worked;
//   * `var x: T` matched none of the concrete BindingPattern branches and
//     returned nullptr → `case.end` left unterminated → malformed IR (a hard
//     codegen abort).
//
// Fix (src/codegen): patternMatches resolves a bare type-parameter pattern
// through the active monomorphization substitution (`genericSubstSemaTy_`) and
// emits the concrete type-test via the shared `emitTypeParamPatternTest` helper.
// Two blockers the FIRST attempt hit, fixed here:
//   BLOCKER 1 (crash): a value-builtin substitution (DateTime/Guid/…) entered
//     the class/interface is-a branch, missed `descArray_`, returned null and
//     fell through to a `return nullptr` → an unterminated `case.end` (malformed
//     IR). Now value-builtins are excluded from the is-a branch and emit a
//     well-formed never-match (their box-descriptor is-a is unavailable — see
//     #1945; identical to the CONCRETE `case o of DateTime` behavior), and EVERY
//     resolved type-param target that the helper cannot lower emits a TERMINATED
//     never-match in the caller — no resolved type-param can produce malformed IR.
//   BLOCKER 2 (over-match): the class/interface is-a used the ERASED per-
//     declaration descriptor, so `T=Wrap<int>` matched a `Wrap<string>`
//     scrutinee. Now it routes through #1843's `perInstDescriptorForTarget`, the
//     per-monomorphization descriptor, so distinct instantiations do not match.
//
// Without the fix EVERY test below fails: the bare-`T` cases emit a backend Error
// (runAot rejects → ran==false → -999) and the `var x: T` cases abort codegen on
// malformed IR (ran==false → -999). So each test pins the defect — it fails
// without the fix and returns the spec-stated value with it. Observables are the
// process exit code (values < 256; a two-arm case encodes match/non-match as
// `match*10 + nonmatch`, e.g. 29 = matched then rejected).
//
// NOT covered here (pre-existing limitations, orthogonal to this case-pattern
// lowering, filed separately): built-in-exception type ARGUMENTS collapse to one
// monomorphization so `Box<IOException>` vs `Box<ArgumentException>` cannot be
// distinguished (#1944); a value-builtin has no box descriptor so a boxed
// value-builtin never-matches, and DateTime→Object boxing is itself malformed in
// some call shapes (#1945). The helper's built-in-exception branch is retained
// (correct once #1944 is fixed) but no test asserts per-instantiation
// distinctness for it.

#include "support/aot_support.hpp"   // runAot — AOT test harness
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::runAot;

namespace {
int run(const std::string& prog) {
    auto r = runAot(prog);
    return r.ran ? r.exitCode : -999;
}

// A generic class whose method type-tests its Object argument against the bare
// type parameter T (`case o of T`). Returns 2 on a match, 9 otherwise.
const char* kBoxBare =
    "module M; "
    "class Animal public method K(): int return 1; end K; end Animal; "
    "class Dog    public method K(): int return 2; end K; end Dog; "
    "class Box<T> "
    "  public method Kind(o: Object): int "
    "    return (case o of T => 2, _ => 9); "
    "  end Kind; "
    "end Box; ";

// The typed-binding sibling form (`case o of var x: T`).
const char* kBoxVar =
    "module M; "
    "class Animal public method K(): int return 1; end K; end Animal; "
    "class Dog    public method K(): int return 2; end K; end Dog; "
    "class Box<T> "
    "  public method Kind(o: Object): int "
    "    return (case o of var x: T => 2, _ => 9); "
    "  end Kind; "
    "end Box; ";
}  // namespace

// -- Class instantiation ------------------------------------------------------

// §19.3 (#1852): bare `T` at a CLASS instantiation — an Animal value tested
// against T=Animal matches (Box<Animal>.Kind(animal) → 2).
BEZEL_TEST(case_type_param_pattern_1852, bare_class_instantiation_matches) {
    BEZEL_EXPECT_EQ(run(std::string(kBoxBare) +
        "class Program public static method Main(): int "
        "  using var b := new Box<Animal>(); "
        "  using var a := new Animal(); "
        "  return b.Kind(a); "
        "end Main; end Program;"), 2);
}

// §19.3 (#1852): bare `T` must NOT over-match an UNRELATED class —
// Box<Animal>.Kind(dog) → 9 (Dog is not an Animal).
BEZEL_TEST(case_type_param_pattern_1852, bare_class_rejects_other_class) {
    BEZEL_EXPECT_EQ(run(std::string(kBoxBare) +
        "class Program public static method Main(): int "
        "  using var b := new Box<Animal>(); "
        "  using var d := new Dog(); "
        "  return b.Kind(d); "
        "end Main; end Program;"), 9);
}

// §19.3 (#1852): a class-instantiation type-test over a scrutinee that holds a
// STRING must not match — and must not fault (a string in an Object is a
// vtable-less bz_string*; the helper guards emitIsA with bz_obj_is_string).
// Box<Animal>.Kind("hi") → 9.
BEZEL_TEST(case_type_param_pattern_1852, bare_class_rejects_string_no_fault) {
    BEZEL_EXPECT_EQ(run(std::string(kBoxBare) +
        "class Program public static method Main(): int "
        "  using var b := new Box<Animal>(); "
        "  return b.Kind(\"hi\"); "
        "end Main; end Program;"), 9);
}

// §19.3 (#1852): the TYPED-BINDING form `var x: T` at a class instantiation —
// Box<Animal>.Kind(animal) → 2. (Previously malformed IR — a hard abort.)
BEZEL_TEST(case_type_param_pattern_1852, typed_binding_class_matches) {
    BEZEL_EXPECT_EQ(run(std::string(kBoxVar) +
        "class Program public static method Main(): int "
        "  using var b := new Box<Animal>(); "
        "  using var a := new Animal(); "
        "  return b.Kind(a); "
        "end Main; end Program;"), 2);
}

// -- String instantiation -----------------------------------------------------

// §19.3 (#1852): bare `T` at a STRING instantiation — a string value matches
// T=string (Box<string>.Kind("hi") → 2). The issue's own repro shape.
BEZEL_TEST(case_type_param_pattern_1852, bare_string_instantiation_matches) {
    BEZEL_EXPECT_EQ(run(std::string(kBoxBare) +
        "class Program public static method Main(): int "
        "  using var b := new Box<string>(); "
        "  return b.Kind(\"hi\"); "
        "end Main; end Program;"), 2);
}

// §19.3 (#1852): a string instantiation must reject a non-string scrutinee —
// Box<string>.Kind(animal) → 9.
BEZEL_TEST(case_type_param_pattern_1852, bare_string_rejects_class) {
    BEZEL_EXPECT_EQ(run(std::string(kBoxBare) +
        "class Program public static method Main(): int "
        "  using var b := new Box<string>(); "
        "  using var a := new Animal(); "
        "  return b.Kind(a); "
        "end Main; end Program;"), 9);
}

// -- Value primitive instantiation --------------------------------------------

// §19.3 (#1852): bare `T` at a VALUE (int) instantiation — a boxed int matches
// T=int (Box<int>.Kind(boxedInt) → 2).
BEZEL_TEST(case_type_param_pattern_1852, bare_value_instantiation_matches_boxed) {
    BEZEL_EXPECT_EQ(run(std::string(kBoxBare) +
        "class Program public static method Main(): int "
        "  using var b := new Box<int>(); "
        "  var oi: Object := 5; "
        "  return b.Kind(oi); "
        "end Main; end Program;"), 2);
}

// §19.3 (#1852): the int instantiation rejects a string — Box<int>.Kind("hi")
// → 9 (a string is not a boxed int; no fault).
BEZEL_TEST(case_type_param_pattern_1852, bare_value_rejects_string) {
    BEZEL_EXPECT_EQ(run(std::string(kBoxBare) +
        "class Program public static method Main(): int "
        "  using var b := new Box<int>(); "
        "  return b.Kind(\"hi\"); "
        "end Main; end Program;"), 9);
}

// -- Interface instantiation --------------------------------------------------

// §19.3 (#1852): bare `T` at an INTERFACE instantiation — "Subtypes match": a
// Square (which implements IShape) matches T=IShape, a Plain does not.
// Box<IShape>.Kind(square)*10 + Kind(plain) = 2*10 + 9 = 29.
BEZEL_TEST(case_type_param_pattern_1852, bare_interface_matches_and_rejects) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "class Square: IShape public method Area(): int return 4; end Area; end Square; "
        "class Plain public method P(): int return 0; end P; end Plain; "
        "class Box<T> public method Kind(o: Object): int "
        "  return (case o of T => 2, _ => 9); end Kind; end Box; "
        "class Program public static method Main(): int "
        "  using var b := new Box<IShape>(); "
        "  using var s := new Square(); "
        "  using var p := new Plain(); "
        "  return b.Kind(s) * 10 + b.Kind(p); "
        "end Main; end Program;"), 29);
}

// §19.3 (#1852): the TYPED-BINDING form binds the value, not just selects the
// arm. `var x: T` over T=Square binds x to the Square; x.Area() (available
// through the `where T: IShape` constraint) reads it → 4. A Plain takes the `_`
// arm → 9. Asserts the BOUND VALUE: 4 (match, value used) and 9 (reject).
BEZEL_TEST(case_type_param_pattern_1852, typed_binding_bound_value) {
    const char* prog =
        "module M; "
        "interface IShape method Area(): int; end IShape; "
        "class Square: IShape public method Area(): int return 4; end Area; end Square; "
        "class Plain public method P(): int return 0; end P; end Plain; "
        "class ShapeBox<T> where T: IShape "
        "  public method Bind(o: Object): int "
        "    return (case o of var x: T => x.Area(), _ => 9); "
        "  end Bind; "
        "end ShapeBox; ";
    // Match: the bound x is the Square, x.Area() == 4.
    BEZEL_EXPECT_EQ(run(std::string(prog) +
        "class Program public static method Main(): int "
        "  using var b := new ShapeBox<Square>(); "
        "  using var s := new Square(); "
        "  return b.Bind(s); "
        "end Main; end Program;"), 4);
    // Reject: a Plain is not a Square, so the `_` arm runs → 9.
    BEZEL_EXPECT_EQ(run(std::string(prog) +
        "class Program public static method Main(): int "
        "  using var b := new ShapeBox<Square>(); "
        "  using var p := new Plain(); "
        "  return b.Bind(p); "
        "end Main; end Program;"), 9);
}

// -- Record / enum instantiation (value types, statically decidable) ----------

// §19.3 / §19.5 (#1852): bare `T` at a RECORD instantiation over a scrutinee
// whose static type IS the record (an unboxed value) — a sealed value type has
// no subtyping, so the test is statically decidable. Chk<Pt,Pt>.M(pt) matches
// (2); Chk<Pt,Other>.M(other) — scrutinee is Other, pattern-type is Pt — does
// not (9). 2*10 + 9 = 29.
BEZEL_TEST(case_type_param_pattern_1852, bare_record_static_match_and_reject) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "record Pt(x: int, y: int); "
        "record Other(a: int); "
        "class Chk<T, U> public method M(a: U): int "
        "  return (case a of T => 2, _ => 9); end M; end Chk; "
        "class Program public static method Main(): int "
        "  using var c1 := new Chk<Pt, Pt>(); "
        "  using var c2 := new Chk<Pt, Other>(); "
        "  var p := new Pt(1, 2); "
        "  var o := new Other(3); "
        "  return c1.M(p) * 10 + c2.M(o); "
        "end Main; end Program;"), 29);
}

// §19.3 / §19.5 (#1852): bare `T` at an ENUM instantiation over a scrutinee of
// that enum type — statically decidable match. Same<Color>.Match(color) → 2.
BEZEL_TEST(case_type_param_pattern_1852, bare_enum_static_match) {
    BEZEL_EXPECT_EQ(run(
        "module M; "
        "enum Color Red, Green, Blue; end Color; "
        "class Same<T> public method Match(a: T): int "
        "  return (case a of T => 2, _ => 9); end Match; end Same; "
        "class Program public static method Main(): int "
        "  using var s := new Same<Color>(); "
        "  var c := Color.Green; "
        "  return s.Match(c); "
        "end Main; end Program;"), 2);
}

// -- Value-builtin instantiation (blocker 1: well-formed, no malformed IR) -----

// §19.3 (#1852) blocker 1: a VALUE-BUILTIN substitution (DateTime) must COMPILE
// (no unterminated `case.end`) for BOTH the bare and typed-binding forms. A
// value-builtin has no reference/box descriptor (#1945), so the test is a
// well-formed never-match — identical to the concrete `case o of DateTime`.
// A class instance is never a DateTime → 9 for both forms. (Passing a class
// avoids the orthogonal DateTime→Object boxing gap, #1945.)
// Without the fix: bare `T` → backend Error (-999); `var x: T` → malformed IR
// (-999). With the fix both compile and return 9.
BEZEL_TEST(case_type_param_pattern_1852, value_builtin_bare_wellformed_never_match) {
    BEZEL_EXPECT_EQ(run(std::string(kBoxBare) +
        "class Program public static method Main(): int "
        "  using var b := new Box<DateTime>(); "
        "  using var a := new Animal(); "
        "  return b.Kind(a); "        // never-match → 9 (well-formed)
        "end Main; end Program;"), 9);
}

BEZEL_TEST(case_type_param_pattern_1852, value_builtin_typed_binding_wellformed) {
    // T=Guid, the typed-binding form (the exact blocker-1 crash shape).
    BEZEL_EXPECT_EQ(run(std::string(kBoxVar) +
        "class Program public static method Main(): int "
        "  using var b := new Box<Guid>(); "
        "  using var a := new Animal(); "
        "  return b.Kind(a); "        // never-match → 9 (well-formed, was ICE)
        "end Main; end Program;"), 9);
}

// -- Per-instantiation rule (two instantiations of one template) --------------

// §19.3 (#1852): bare `T` obeys the per-instantiation rule — Box<string>.Kind
// ("hi") matches (2) while Box<Animal>.Kind("hi") does not (9), at two
// instantiations of the SAME template. 2*10 + 9 = 29.
BEZEL_TEST(case_type_param_pattern_1852, bare_per_instantiation) {
    BEZEL_EXPECT_EQ(run(std::string(kBoxBare) +
        "class Program public static method Main(): int "
        "  using var bs := new Box<string>(); "
        "  using var ba := new Box<Animal>(); "
        "  return bs.Kind(\"hi\") * 10 + ba.Kind(\"hi\"); "
        "end Main; end Program;"), 29);
}

// §19.3 (#1852): the typed-binding form obeys the same per-instantiation rule.
BEZEL_TEST(case_type_param_pattern_1852, typed_binding_per_instantiation) {
    BEZEL_EXPECT_EQ(run(std::string(kBoxVar) +
        "class Program public static method Main(): int "
        "  using var bs := new Box<string>(); "
        "  using var ba := new Box<Animal>(); "
        "  return bs.Kind(\"hi\") * 10 + ba.Kind(\"hi\"); "
        "end Main; end Program;"), 29);
}

// -- BLOCKER 2: constructed-generic over-match (the core soundness fix) --------

// A generic `Wrap<U>` plus an outer `Container<T>` whose method type-tests its
// Object argument against T. Container<Wrap<int>> must match a Wrap<int> and NOT
// a Wrap<string> — the per-monomorphization descriptor (#1843), not the shared
// erased one (which would over-match every instantiation). Bare and var forms.
namespace {
const char* kWrapContainer =
    "module M; "
    "class Wrap<U> public method Tag(): int return 7; end Tag; end Wrap; "
    "class Container<T> "
    "  public method Kind(o: Object): int "
    "    return (case o of T => 2, _ => 9); "
    "  end Kind; "
    "  public method KindVar(o: Object): int "
    "    return (case o of var x: T => 2, _ => 9); "
    "  end KindVar; "
    "end Container; ";
}  // namespace

// §19.3 / §12.9 (#1852 blocker 2): bare `T=Wrap<int>` matches a Wrap<int> (2)
// and must NOT over-match a Wrap<string> (9). 2*10 + 9 = 29 (over-match = 22).
BEZEL_TEST(case_type_param_pattern_1852, overmatch_bare_rejects_sibling_instantiation) {
    BEZEL_EXPECT_EQ(run(std::string(kWrapContainer) +
        "class Program public static method Main(): int "
        "  using var c := new Container<Wrap<int>>(); "
        "  using var wi := new Wrap<int>(); "
        "  using var ws := new Wrap<string>(); "
        "  return c.Kind(wi) * 10 + c.Kind(ws); "
        "end Main; end Program;"), 29);
}

// §19.3 / §12.9 (#1852 blocker 2): the typed-binding form `var x: T` obeys the
// same per-monomorphization discrimination — Wrap<int> yes, Wrap<string> no.
BEZEL_TEST(case_type_param_pattern_1852, overmatch_typed_binding_rejects_sibling) {
    BEZEL_EXPECT_EQ(run(std::string(kWrapContainer) +
        "class Program public static method Main(): int "
        "  using var c := new Container<Wrap<int>>(); "
        "  using var wi := new Wrap<int>(); "
        "  using var ws := new Wrap<string>(); "
        "  return c.KindVar(wi) * 10 + c.KindVar(ws); "
        "end Main; end Program;"), 29);
}
