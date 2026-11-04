// Regression tests for issue #8140: a no-selector `Min()`/`Max()` over an
// element whose type is a GENERIC RECORD (`record W<T>: IComparable<W<T>>`)
// aborted the compile with "IR verification failed (malformed IR emitted)" —
// the lowering compared the record aggregate against an `i32`.
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
//   * spec §14.2 "Constraints" *Interface conformance* rule 2: "T is a
//     record that lists I (or any interface extending I) in its inheritance
//     clause and declares matching members" ⇒ a GENERIC record
//     `W<T>: IComparable<W<T>>` conforms at every instantiation, so
//     `W<string>` satisfies the no-selector `Min()` constraint of stdlib
//     §5.1, `where T: IComparable<T>` (the interface is stdlib §2.8), and
//     `Array<W<string>>.Min()` MUST be admitted.
//   * spec §14.1 "Generic declarations" *Instantiation and its site*: "An
//     instantiation of a generic declaration is the constructed type or
//     constructed member obtained by substituting type arguments for its type
//     parameters" ⇒ the `CompareTo` that orders `W<string>` is the one
//     MONOMORPHIZED at `W<string>`, reading the element at `W<string>`'s own
//     layout.
//
// THE DEFECT (MEASURED on the base, r30-c1 @ 3b393693, the issue's repro
// verbatim):
//
//     Both operands to ICmp instruction are not of the same type!
//       %25 = icmp slt %"record.W$spec0" %22, i32 %24
//     bezelc: codegen error: IR verification failed (malformed IR emitted)
//
// The no-selector `Min()`/`Max()` lowering reaches a record element through
// `buildRecordCompareToThunk`, and that builder DECLINES a generic record
// (its `CompareTo` would need monomorphization) — so the call fell through
// to the numeric path, which compares the element (a struct) against an
// `i32` accumulator.
//
// ⇒ THE FIX monomorphizes the generic record's `CompareTo` at the
// SUBSTITUTED instantiation and orders through it, the discipline the
// non-generic record arm already follows.
//
// ORACLE. `runAot` (the AOT path). Every row asserts the EXACT STDOUT — a
// VALUE oracle, never "no diagnostic was produced" — so a legal
// `Min`/`Max` over a generic record must compile and print the value
// stdlib §5.1's ordering selects. A program that is REJECTED yields the
// harness's "<did-not-run>" sentinel, which no expected string matches — so
// a compile ABORT (the base's behaviour) and a wrong value are both observed
// as a FAILURE of the row.
//
// ⚠ THE ROWS ARE NOT ALL THE SAME SHAPE: the element's LAYOUT changes with
// the instantiation (`W<string>`'s struct and `W<int>`'s struct differ once
// a `T`-typed component is declared), and the ORDERING is the record's own
// `CompareTo`, not the array's address order — so a fix that only worked for
// one instantiation, or that ordered by address, would pass a single-layout
// test.

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

// The issue's own generic record: a type parameter that appears in the
// `IComparable<W<T>>` clause and in the `CompareTo` parameter, but NOT in
// the storage. Its struct layout is the same at every instantiation, so it
// isolates the ORDERING (and the monomorphization) from the layout.
std::string wDecl() {
    return "record W<T>(v: int): IComparable<W<T>> "
           "  public method CompareTo(other: W<T>): int => "
           "    (if v > other.v then 1 else "
           "     (if v < other.v then -1 else 0)); "
           "end W; ";
}

// A generic record whose type parameter IS a storage component: `v` then
// sits at a different offset in `W2<string>` ({ptr,i32}) than in `W2<int>`
// ({i32,i32}), so an element loaded at the WRONG layout answers a garbage
// `v` instead of the right one.
std::string w2Decl() {
    return "record W2<T>(tag: T, v: int): IComparable<W2<T>> "
           "  public method CompareTo(other: W2<T>): int => "
           "    (if v > other.v then 1 else "
           "     (if v < other.v then -1 else 0)); "
           "end W2; ";
}

}  // namespace

// =====================================================================
// ROW 1 — THE ISSUE'S OWN REPRO (its program, verbatim but for the
// printed values): a GENERIC record `W<T>` at `W<string>`.
//
// The elements are 3 and 1, so the MINIMUM is the SECOND element — an
// address ordering (the bug a naive widening of the pointer arm would
// introduce) would answer the FIRST element of the buffer instead.
//
// Base: the compile ABORTS ("IR verification failed (malformed IR emitted)").
// Fixed: "1" / "3".
// =====================================================================
BEZEL_TEST(minmax_generic_record_8140,
           the_issue_repro_a_generic_record_over_w_string) {
    BEZEL_EXPECT_EQ(
        out(prog(
            wDecl(),
            "var a: Array<W<string>> := [new W<string>(3), new W<string>(1)]; "
            "var m := a.Min(); "
            "var x := a.Max(); "
            "Console.WriteLine(m.v); "
            "Console.WriteLine(x.v); ")),
        "1\n"
        "3\n");
}

// =====================================================================
// ROW 2 — THE SAME RECORD AT `W<int>`. A second instantiation proves the
// monomorphization is PER INSTANTIATION and not a one-off that happens to
// work for the `string` argument.
//
// Base: the compile ABORTS. Fixed: "1" / "3".
// =====================================================================
BEZEL_TEST(minmax_generic_record_8140,
           the_same_generic_record_over_w_int) {
    BEZEL_EXPECT_EQ(
        out(prog(
            wDecl(),
            "var a: Array<W<int>> := [new W<int>(3), new W<int>(1)]; "
            "var m := a.Min(); "
            "var x := a.Max(); "
            "Console.WriteLine(m.v); "
            "Console.WriteLine(x.v); ")),
        "1\n"
        "3\n");
}

// =====================================================================
// ROW 3 — A NESTED INSTANTIATION, `W<W<int>>`: the type argument is itself
// a constructed generic record, so the substitution composes (§14.1's
// "substituting type arguments for its type parameters").
//
// Base: the compile ABORTS. Fixed: "1" / "3".
// =====================================================================
BEZEL_TEST(minmax_generic_record_8140,
           a_nested_instantiation_w_of_w_int) {
    BEZEL_EXPECT_EQ(
        out(prog(
            wDecl(),
            "var a: Array<W<W<int>>> := "
            "[new W<W<int>>(3), new W<W<int>>(1)]; "
            "var m := a.Min(); "
            "var x := a.Max(); "
            "Console.WriteLine(m.v); "
            "Console.WriteLine(x.v); ")),
        "1\n"
        "3\n");
}

// =====================================================================
// ROW 4 — A GENERIC RECORD WITH A `T`-TYPED COMPONENT, at `W2<string>`.
// `v` sits at offset 8 in that struct ({ptr, i32}) and at offset 4 in
// `W2<int>`'s ({i32, i32}), so this row fails if the element is loaded at
// the WRONG instantiation's layout.
//
// Base: the compile ABORTS. Fixed: "1" / "3".
// =====================================================================
BEZEL_TEST(minmax_generic_record_8140,
           a_generic_record_with_a_t_typed_component_over_w2_string) {
    BEZEL_EXPECT_EQ(
        out(prog(
            w2Decl(),
            "var a: Array<W2<string>> := "
            "[new W2<string>(\"x\", 3), new W2<string>(\"y\", 1)]; "
            "var m := a.Min(); "
            "var x := a.Max(); "
            "Console.WriteLine(m.v); "
            "Console.WriteLine(x.v); ")),
        "1\n"
        "3\n");
}

// =====================================================================
// ROW 5 — THE SAME GENERIC RECORD WITH A `T`-TYPED COMPONENT, at
// `W2<int>`: the OTHER layout. Together with row 4 this pins the element
// load to the instantiation's own layout.
//
// Base: the compile ABORTS. Fixed: "1" / "3".
// =====================================================================
BEZEL_TEST(minmax_generic_record_8140,
           a_generic_record_with_a_t_typed_component_over_w2_int) {
    BEZEL_EXPECT_EQ(
        out(prog(
            w2Decl(),
            "var a: Array<W2<int>> := "
            "[new W2<int>(7, 3), new W2<int>(8, 1)]; "
            "var m := a.Min(); "
            "var x := a.Max(); "
            "Console.WriteLine(m.v); "
            "Console.WriteLine(x.v); ")),
        "1\n"
        "3\n");
}

// =====================================================================
// ROW 6 — CONTROL: a NON-GENERIC record element (`IComparable<Meters>`) is
// unchanged — the path the generic arm sits beside. It already worked, so
// it must stay working.
//
// Base and fixed: "1" / "3".
// =====================================================================
BEZEL_TEST(minmax_generic_record_8140,
           control_a_non_generic_record_element_is_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog(
            "record Meters(cents: int): IComparable<Meters> "
            "  public method CompareTo(other: Meters): int => "
            "    (if cents > other.cents then 1 else "
            "     (if cents < other.cents then -1 else 0)); "
            "end Meters; ",
            "var a: Array<Meters> := [new Meters(3), new Meters(1)]; "
            "var m := a.Min(); "
            "var x := a.Max(); "
            "Console.WriteLine(m.cents); "
            "Console.WriteLine(x.cents); ")),
        "1\n"
        "3\n");
}

// =====================================================================
// ROW 7 — CONTROL: an `int` element is unchanged (the numeric no-selector
// path).
//
// Base and fixed: "1" / "3".
// =====================================================================
BEZEL_TEST(minmax_generic_record_8140,
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
// ROW 8 — TWO INSTANTIATIONS IN ONE PROGRAM: `Array<W<string>>` and
// `Array<W<int>>` in the same module. The comparator thunk is cached per
// INSTANTIATION, so a fix that minted ONE thunk per record DECLARATION
// would order the second array by the first's `CompareTo`.
//
// Base: the compile ABORTS. Fixed: "1" / "3" / "5" / "7".
// =====================================================================
BEZEL_TEST(minmax_generic_record_8140,
           two_instantiations_in_one_program) {
    BEZEL_EXPECT_EQ(
        out(prog(
            wDecl(),
            "var a: Array<W<string>> := [new W<string>(3), new W<string>(1)]; "
            "var b: Array<W<int>> := [new W<int>(7), new W<int>(5)]; "
            "Console.WriteLine(a.Min().v); "
            "Console.WriteLine(a.Max().v); "
            "Console.WriteLine(b.Min().v); "
            "Console.WriteLine(b.Max().v); ")),
        "1\n"
        "3\n"
        "5\n"
        "7\n");
}

// =====================================================================
// ROW 9 — THE EMPTY SOURCE (stdlib §5's empty-source rule: "Min/Max
// — selector and no-selector forms alike — throw InvalidOperationException
// on an empty source"). The generic-record arm keeps that guard, so an
// EMPTY array still throws rather than reading element 0.
//
// Base: the compile ABORTS (the `a.Min()` inside the `try` is the aborting
// call). Fixed: "True".
// =====================================================================
BEZEL_TEST(minmax_generic_record_8140,
           control_an_empty_source_throws_invalid_operation_exception) {
    BEZEL_EXPECT_EQ(
        out(prog(
            wDecl(),
            "var a: Array<W<string>> := []; "
            "var caught := false; "
            "try "
            "  Console.WriteLine(a.Min().v); "
            "catch InvalidOperationException => caught := true; end; "
            "Console.WriteLine(caught); ")),
        "True\n");
}
