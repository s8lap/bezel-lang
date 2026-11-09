// Regression tests for issue #8272: the no-selector `Min()`/`Max()` over an
// element class that reaches `IComparable<E>` through a GENERIC base whose
// `CompareTo(o: T)` is NON-VIRTUAL emitted a MISTYPED call — the erased
// template's `T`-typed parameter lowers to the i32 placeholder, so the
// element POINTER was passed where the callee wanted an `i32`, and the IR
// verifier ABORTED the compiler on a program stdlib §5.1 admits.
//
// WHAT THE DOCUMENTS REQUIRE (each §-number opened at v0.355 in this tree).
//
//   * stdlib §5.1 "No-selector aggregation overloads" declares
//         public extension IEnumerable<T> where T: IComparable<T>
//           public method Min(): T;              // InvalidOperationException if empty
//           public method Max(): T;              // InvalidOperationException if empty
//     ⇒ the no-selector form ORDERS BY THE ELEMENT'S OWN `CompareTo` and
//       returns the element itself (`T`).
//   * stdlib §2.8 "`IComparable<T>`" declares the constraint's authority:
//         public interface IComparable<T>
//           method CompareTo(other: T): int;
//         end IComparable;
//   * spec §14.2 "Constraints" *Interface conformance* rule 1: "T is a class
//     that lists I (or any interface extending I) in its inheritance clause,
//     that inherits from a class which conforms to I" — and the rule admits
//     an INHERITED implementation ("Implementation may be direct ...,
//     inherited (a base class declares them)"). ⇒ an element class that
//     reaches `IComparable<E>` through a GENERIC base `B2<E>` CONFORMS, so
//     `Array<Item2>.Min()` MUST be admitted.
//   * spec §14.1 "Generic declarations" *Instantiation and its site*: "An
//     instantiation of a generic declaration is the constructed type or
//     constructed member obtained by substituting type arguments for its
//     type parameters" ⇒ the `CompareTo` that orders `Item2` is the one
//     MONOMORPHIZED at `B2<Item2>` — `B2<Item2>.CompareTo(other: Item2)` —
//     NOT the erased template `B2<T>.CompareTo(other: T)`.
//
// THE DEFECT (MEASURED on the base, the issue's repro verbatim):
//
//     Call parameter type does not match function signature!
//       %mm.acc = load ptr, ptr %1, align 8
//      i32  %54 = call i32 @"bz$M.B2$CompareTo"(ptr %mm.el, ptr %mm.acc)
//     bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// ⇒ THE FIX monomorphizes the inherited `CompareTo` at the ELEMENT's own
// instantiation (spec §14.1) and orders through that specialization — the
// very function a written-out `el.CompareTo(acc)` calls.
//
// ORACLE. `runAot` (the AOT path). Every row asserts the EXACT STDOUT — a
// VALUE oracle, never "no diagnostic was produced" — so the program must
// BUILD and RUN and print what stdlib §5.1's ordering selects. A program that
// is REJECTED (or whose compile ABORTS, which is the base's behaviour) yields
// the harness's "<did-not-run>" sentinel, which no expected string matches —
// so the base's malformed-IR abort and a wrong value are both observed as a
// FAILURE of the row.
//
// ⚠ THE `*OrNull` SIBLINGS ARE THE SAME DEFECT. `MinOrNull()`/`MaxOrNull()`
// are declared in the SAME stdlib §5.1 `where T: IComparable<T>` block and
// dispatch through the SAME `CompareTo`, so they are rows here too — a fix
// that repaired only `Min()`/`Max()` would leave the malformed IR reachable
// through the sibling.

#include "support/aot_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

// The program's stdout, verbatim. "<did-not-run>" when the program was
// rejected, crashed or timed out.
std::string out(const std::string& program) {
    auto r = runAot(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

// `decls` are the module-level type declarations; `body` is Main's body.
std::string prog(const std::string& decls, const std::string& body) {
    return "module M; " + decls +
           " public class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

// THE ISSUE'S OWN SHAPE, verbatim: a GENERIC base `B2<T>: IComparable<T>`
// whose `CompareTo(other: T)` is NON-VIRTUAL, and an element class that
// reaches `IComparable<Item2>` through `B2<Item2>` (spec §14.2 rule 1's
// inherited implementation).
//
// Its `CompareTo` returns a CONSTANT, so the ordering it induces is "all
// elements are equal" — `Min()`/`Max()` keep the FIRST element. The row is
// about the CALL: on the base the call was MISTYPED and the compile aborted.
std::string issueDecls() {
    return "class B2<T>: IComparable<T> "
           "  public method CompareTo(other: T): int => 0; "
           "end B2; "
           "class Item2: B2<Item2> "
           "  public A: int; "
           "  public constructor(a: int) A := a; end; "
           "end Item2; ";
}

// THE SAME SHAPE, with the base's `CompareTo` PASSING its argument on to a
// virtual helper the derived class implements — so the ordering READS the
// argument's own data (`A - other.A`).
//
// This is the VALUE oracle: the ordering now depends on the ARGUMENT the
// fix passes, so a fix that swapped the two operands (or passed the wrong
// one) would select the other element and print the other number. `A` is an
// `int` field, so the element's LAYOUT is the same at every instantiation
// and the row isolates the ARGUMENT from the layout.
//
// The helper is `virtual` on purpose: the base's `CompareTo` is still the
// NON-VIRTUAL witness the issue is about, and the helper is what lets the
// base's body reach the derived class's data at all — the base's own type
// parameter is unconstrained, so `other.A` is not nameable there.
std::string readingDecls() {
    return "class B2<T>: IComparable<T> "
           "  public virtual method Cmp(other: T): int => 0; "
           "  public method CompareTo(other: T): int => Cmp(other); "
           "end B2; "
           "class Item2: B2<Item2> "
           "  public A: int; "
           "  public constructor(a: int) A := a; end; "
           "  public override method Cmp(other: Item2): int => A - other.A; "
           "end Item2; ";
}

// CONTROL: the INHERITED VIRTUAL witness — a `virtual CompareTo` in the
// generic base and an `override` in the derived class. The issue's own
// control: it compiles and is correct, so it must stay exactly as it is.
std::string virtualDecls() {
    return "class B<T>: IComparable<T> "
           "  public X: int; "
           "  public constructor(x: int) X := x; end; "
           "  public virtual method CompareTo(other: T): int => 0; "
           "end B; "
           "class D: B<D> "
           "  public constructor(x: int) base(x); end; "
           "  public override method CompareTo(other: D): int => X - other.X; "
           "end D; ";
}

// CONTROL: a DIRECT class implementation (`class E: IComparable<E>` with its
// own `CompareTo`) — the shape that never reached the erased template.
std::string directDecls() {
    return "class E: IComparable<E> "
           "  public A: int; "
           "  public constructor(a: int) A := a; end; "
           "  public method CompareTo(other: E): int => A - other.A; "
           "end E; ";
}

// CONTROL: a USER RECORD element (spec §14.2 rule 2's shape).
std::string recordDecls() {
    return "record R(A: int): IComparable<R> "
           "  public method CompareTo(other: R): int => A - other.A; "
           "end R; ";
}

}  // namespace

// =====================================================================
// ROW 1 — THE ISSUE'S OWN REPRO: the inherited NON-VIRTUAL `CompareTo`
// of a generic base, over a TWO-element array.
//
// The elements are 3 and 1, and the base's `CompareTo` returns a constant,
// so the ordering is "all equal" and both `Min()` and `Max()` keep the
// FIRST element (3). The row pins that the program BUILDS and RUNS.
//
// Base: the compile ABORTS ("IR verification failed (malformed IR emitted)").
// Fixed: "3" / "3".
// =====================================================================
BEZEL_TEST(array_minmax_generic_base_compareto_8272,
           the_issue_repro_an_inherited_non_virtual_compare_to) {
    BEZEL_EXPECT_EQ(
        out(prog(
            issueDecls(),
            "var a: Array<Item2> := [new Item2(3), new Item2(1)]; "
            "Console.WriteLine(a.Min().A); "
            "Console.WriteLine(a.Max().A); ")),
        "3\n"
        "3\n");
}

// =====================================================================
// ROW 2 — THE SAME SHAPE with a base `CompareTo` that READS its argument
// (`V - other.V`). The ordering is now the element's own value, so the
// printed numbers depend on the ARGUMENT the ordering call passes: a fix
// that swapped the operands would print the other element.
//
// Base: the compile ABORTS. Fixed: "1" (the minimum) / "3" (the maximum).
// =====================================================================
BEZEL_TEST(array_minmax_generic_base_compareto_8272,
           the_inherited_compare_to_that_reads_its_argument) {
    BEZEL_EXPECT_EQ(
        out(prog(
            readingDecls(),
            "var a: Array<Item2> := [new Item2(3), new Item2(1)]; "
            "Console.WriteLine(a.Min().A); "
            "Console.WriteLine(a.Max().A); ")),
        "1\n"
        "3\n");
}

// =====================================================================
// ROW 3 — THE `*OrNull` SIBLINGS over the same shape: stdlib §5.1
// declares them in the SAME `where T: IComparable<T>` block, and they
// dispatched through the SAME erased template.
//
// Base: the compile ABORTS. Fixed: "1" / "3".
// =====================================================================
BEZEL_TEST(array_minmax_generic_base_compareto_8272,
           the_or_null_siblings_order_the_same_way) {
    BEZEL_EXPECT_EQ(
        out(prog(
            readingDecls(),
            "var a: Array<Item2> := [new Item2(3), new Item2(1)]; "
            "var m := a.MinOrNull(); "
            "if m = null then Console.WriteLine(\"null\"); "
            "else Console.WriteLine(m!.A); end; "
            "var x := a.MaxOrNull(); "
            "if x = null then Console.WriteLine(\"null\"); "
            "else Console.WriteLine(x!.A); end; ")),
        "1\n"
        "3\n");
}

// =====================================================================
// ROW 4 — A ONE-ELEMENT ARRAY over the issue's shape.
//
// ⚠ THIS IS NOT A CONTROL: the base ABORTED here too, because the
// ordering call is EMITTED whether or not the loop ever executes it —
// the malformed IR is a property of the emitted code, not of the run. The
// row pins that the one-element program builds and runs like the two-
// element one.
//
// Base: the compile ABORTS. Fixed: "3".
// =====================================================================
BEZEL_TEST(array_minmax_generic_base_compareto_8272,
           a_one_element_array_over_the_same_shape) {
    BEZEL_EXPECT_EQ(
        out(prog(
            issueDecls(),
            "var a: Array<Item2> := [new Item2(3)]; "
            "Console.WriteLine(a.Min().A); ")),
        "3\n");
}

// =====================================================================
// ROW 5 — CONTROL: the INHERITED VIRTUAL witness (the issue's own
// control): a `virtual CompareTo` in the generic base and an `override` in
// the derived class. The elements are 7 and 2, so the minimum is 2.
//
// Base and fixed: "2" / "7".
// =====================================================================
BEZEL_TEST(array_minmax_generic_base_compareto_8272,
           control_the_inherited_virtual_witness_is_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            virtualDecls(),
            "var a: Array<D> := [new D(7), new D(2)]; "
            "Console.WriteLine(a.Min().X); "
            "Console.WriteLine(a.Max().X); ")),
        "2\n"
        "7\n");
}

// =====================================================================
// ROW 6 — CONTROL: a DIRECT class implementation (`class E: IComparable<E>`
// with its own `CompareTo`) — the shape that never reached an erased
// template.
//
// Base and fixed: "1" / "3".
// =====================================================================
BEZEL_TEST(array_minmax_generic_base_compareto_8272,
           control_a_direct_class_implementation_is_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            directDecls(),
            "var a: Array<E> := [new E(3), new E(1)]; "
            "Console.WriteLine(a.Min().A); "
            "Console.WriteLine(a.Max().A); ")),
        "1\n"
        "3\n");
}

// =====================================================================
// ROW 7 — CONTROL: a USER RECORD element (spec §14.2 rule 2) — the
// sibling arm the fix's arm sits beside.
//
// Base and fixed: "1" / "3".
// =====================================================================
BEZEL_TEST(array_minmax_generic_base_compareto_8272,
           control_a_record_element_is_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            recordDecls(),
            "var a: Array<R> := [new R(3), new R(1)]; "
            "Console.WriteLine(a.Min().A); "
            "Console.WriteLine(a.Max().A); ")),
        "1\n"
        "3\n");
}

// =====================================================================
// ROW 8 — CONTROL: an `int` element — the numeric no-selector path.
//
// Base and fixed: "1" / "3".
// =====================================================================
BEZEL_TEST(array_minmax_generic_base_compareto_8272,
           control_an_int_element_is_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "",
            "var a: Array<int> := [3, 1]; "
            "Console.WriteLine(a.Min()); "
            "Console.WriteLine(a.Max()); ")),
        "1\n"
        "3\n");
}

// =====================================================================
// ROW 9 — TWO ELEMENTS OF DIFFERENT DERIVED CLASSES over the SAME generic
// base: `Item2: B2<Item2>` and `Item3: B2<Item3>`. The ordering function
// is monomorphized PER INSTANTIATION (spec §14.1), so a fix that minted
// ONE function for the base DECLARATION would order the second array by
// the first's instantiation.
//
// Base: the compile ABORTS. Fixed: "1" / "3" / "8" / "9".
// =====================================================================
BEZEL_TEST(array_minmax_generic_base_compareto_8272,
           two_derived_classes_of_the_same_generic_base) {
    BEZEL_EXPECT_EQ(
        out(prog(
            readingDecls() +
                "class Item3: B2<Item3> "
                "  public A: int; "
                "  public constructor(a: int) A := a; end; "
                "  public override method Cmp(other: Item3): int => "
                "    A - other.A; "
                "end Item3; ",
            "var a: Array<Item2> := [new Item2(3), new Item2(1)]; "
            "Console.WriteLine(a.Min().A); "
            "Console.WriteLine(a.Max().A); "
            "var b: Array<Item3> := [new Item3(9), new Item3(8)]; "
            "Console.WriteLine(b.Min().A); "
            "Console.WriteLine(b.Max().A); ")),
        "1\n"
        "3\n"
        "8\n"
        "9\n");
}
