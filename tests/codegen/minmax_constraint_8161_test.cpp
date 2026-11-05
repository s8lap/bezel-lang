// Regression tests for issue #8161 — three no-selector `Min()`/`Max()`
// defects in the stdlib §5.1 "No-selector aggregation overloads" extension,
// found by the #8140 review:
//
//   1. the CONSTRAINT CHECK over-admits: a `record W<T>(…): IComparable<W<int>>`
//      used at `W<string>` was admitted and then ordered by a `CompareTo`
//      monomorphized at the WRONG layout (silent garbage);
//   2. `MinOrNull()`/`MaxOrNull()` over a GENERIC RECORD element SEGFAULTS
//      `bezelc`;
//   3. `.Where(…).Min()` with no selector is typed `int` over a record
//      element, so `f.v` on the result does not resolve.
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
//     clause and declares matching members" ⇒ the conformance is judged AT
//     THE INSTANTIATION, so `record W<T>: IComparable<W<T>>` conforms at
//     every instantiation, while `record W<T>: IComparable<W<int>>` conforms
//     ONLY at `W<int>`.
//   * spec §14.1 "Generic declarations" *Instantiation and its site*: "An
//     instantiation of a generic declaration is the constructed type or
//     constructed member obtained by substituting type arguments for its
//     type parameters" ⇒ the interface a generic record implements is
//     re-expressed in the INSTANCE's arguments before it is compared.
//
// ORACLE. `runAot` (the AOT path). Every accepting row asserts the EXACT
// STDOUT — a VALUE oracle, never "no diagnostic was produced" — and every
// rejecting row asserts the SPECIFIC diagnostic text, so a program that is
// rejected for an unrelated reason cannot satisfy the row.
//
// ⚠ THE ORDERING IS THE RECORD'S OWN `CompareTo`, AND IT IS REVERSED
// (descending) in these rows, so the minimum is NOT the first element of the
// buffer. A lowering that compared addresses, or that compared the raw
// words, would answer the FIRST element instead — and the reversed order
// makes that visible in the printed value.

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

// REJECTION with the SPECIFIC diagnostic text: a program that was rejected
// for an unrelated reason (or that compiled) must NOT satisfy the row.
bool rejectsWith(const std::string& program, const std::string& needle) {
    auto r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// `decls` are the module-level type declarations; `body` is Main's body.
std::string prog(const std::string& decls, const std::string& body) {
    return "module M; " + decls +
           " public class C public static method Main(): int " + body +
           " return 0; end Main; end C;";
}

// A generic record whose DECLARED interface names a CONCRETE instantiation
// (`IComparable<W<int>>`), so the record conforms at `W<int>` and NOT at
// `W<string>`. The `CompareTo` is REVERSED (descending), so `Min()` selects
// the LARGEST `v` and `Max()` the SMALLEST.
std::string wFixedDecl() {
    return "record W<T>(v: int): IComparable<W<int>> "
           "  public method CompareTo(other: W<int>): int => "
           "    (if v > other.v then -1 else "
           "     (if v < other.v then 1 else 0)); "
           "end W; ";
}

// The CONFORMING generic record (`IComparable<W<T>>`) — the shape #8140
// admitted. Same REVERSED `CompareTo`.
std::string wSelfDecl() {
    return "record W<T>(v: int): IComparable<W<T>> "
           "  public method CompareTo(other: W<T>): int => "
           "    (if v > other.v then -1 else "
           "     (if v < other.v then 1 else 0)); "
           "end W; ";
}

// A generic record with a `T`-TYPED component whose declared interface names
// a CONCRETE instantiation: at `W2<string>` the layout is {ptr, i32} and at
// `W2<int>` it is {i32, i32}, so an element read at the wrong instantiation
// answers a garbage `v`.
std::string w2FixedDecl() {
    return "record W2<T>(tag: T, v: int): IComparable<W2<int>> "
           "  public method CompareTo(other: W2<int>): int => "
           "    (if v > other.v then -1 else "
           "     (if v < other.v then 1 else 0)); "
           "end W2; ";
}

// The CONFORMING generic record with a `T`-TYPED component (the shape whose
// struct layout differs per instantiation). Same REVERSED `CompareTo`.
std::string w2SelfDecl() {
    return "record W2<T>(tag: T, v: int): IComparable<W2<T>> "
           "  public method CompareTo(other: W2<T>): int => "
           "    (if v > other.v then -1 else "
           "     (if v < other.v then 1 else 0)); "
           "end W2; ";
}

// The constraint diagnostic the no-selector `Min()`/`Max()` gate reports
// (stdlib §5.1 / §14.2 / #1198). The element's simple name is `W`.
const char* kConstraintNeedle =
    "W does not implement IComparable<T>";

}  // namespace

// =====================================================================
// ROW 1 — THE OVER-ADMISSION, `Min()`.
//
// `record W<T>(v: int): IComparable<W<int>>` used at `W<string>`:
// `W<string>` does NOT implement `IComparable<W<string>>` (it implements
// `IComparable<W<int>>`), so stdlib §5.1's `where T: IComparable<T>` is
// NOT satisfied and the call must be REJECTED at compile time.
//
// Pre-fix: the call was ADMITTED and the program compiled — printing the
// element read through the WRONG layout (garbage). The row is RED on the
// base because the base COMPILES (so `rejectsWith` is false).
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           min_over_a_generic_record_declaring_a_concrete_instantiation) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog(wFixedDecl(),
             "var a: Array<W<string>> := "
             "[new W<string>(3), new W<string>(1)]; "
             "Console.WriteLine(a.Min().v); "),
        kConstraintNeedle));
}

// =====================================================================
// ROW 2 — THE OVER-ADMISSION, `Max()`: the same shape through the sibling
// member, so the gate cannot be fixed for `Min()` alone.
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           max_over_a_generic_record_declaring_a_concrete_instantiation) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog(wFixedDecl(),
             "var a: Array<W<string>> := "
             "[new W<string>(3), new W<string>(1)]; "
             "Console.WriteLine(a.Max().v); "),
        kConstraintNeedle));
}

// =====================================================================
// ROW 3 — THE OVER-ADMISSION AT A DIFFERENT LAYOUT: `W2<string>`'s struct
// is {ptr, i32} and `W2<int>`'s is {i32, i32}, so the pre-fix program
// printed a garbage `v` — the symptom the #8140 review measured ("3 2").
// The row must be RED on the base (the base compiles and prints garbage).
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           min_over_a_generic_record_with_a_t_typed_component) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog(w2FixedDecl(),
             "var a: Array<W2<string>> := "
             "[new W2<string>(\"x\", 3), new W2<string>(\"y\", 1), "
             "new W2<string>(\"z\", 2)]; "
             "Console.WriteLine(a.Min().v); "
             "Console.WriteLine(a.Max().v); "),
        "W2 does not implement IComparable<T>"));
}

// =====================================================================
// ROW 4 — CONTROL: THE SAME RECORD AT THE INSTANTIATION IT CONFORMS AT.
// `W<int>` DOES implement `IComparable<W<int>>`, so the call is admitted and
// the REVERSED `CompareTo` orders the elements descending: `Min()` is the
// LARGEST `v` (3) and `Max()` the SMALLEST (1).
//
// Green on the base AND on the fix — the positive control that shows the
// shape is genuinely admitted rather than rejected for an unrelated reason.
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           control_the_same_record_at_the_instantiation_it_conforms_at) {
    BEZEL_EXPECT_EQ(
        out(prog(wFixedDecl(),
                 "var a: Array<W<int>> := [new W<int>(3), new W<int>(1)]; "
                 "Console.WriteLine(a.Min().v); "
                 "Console.WriteLine(a.Max().v); ")),
        "3\n"
        "1\n");
}

// =====================================================================
// ROW 5 — CONTROL: THE CONFORMING GENERIC RECORD (`IComparable<W<T>>`) at
// `W<string>` — the shape #8140 admitted. It must STAY admitted: the
// conformance is re-expressed in the instance's arguments, and here the
// declared argument IS the instance's argument.
//
// The REVERSED `CompareTo` makes the ordering visible: `Min()` is 3 and
// `Max()` is 1.
//
// Green on the base AND on the fix.
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           control_a_self_conforming_generic_record_stays_admitted) {
    BEZEL_EXPECT_EQ(
        out(prog(wSelfDecl(),
                 "var a: Array<W<string>> := "
                 "[new W<string>(3), new W<string>(1)]; "
                 "Console.WriteLine(a.Min().v); "
                 "Console.WriteLine(a.Max().v); ")),
        "3\n"
        "1\n");
}

// =====================================================================
// ROW 6 — CONTROL: THE CONFORMANCE THROUGH AN INTERFACE THAT EXTENDS
// `IComparable<T>` (spec §11 rule 2, v0.355: "lists I (or any interface
// extending I)"). `record W<T>: IOrd<W<T>>` with
// `interface IOrd<T>: IComparable<T>` conforms at EVERY instantiation.
//
// Green on the base AND on the fix.
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           control_conformance_through_an_extending_interface) {
    BEZEL_EXPECT_EQ(
        out(prog("interface IOrd<T>: IComparable<T> end IOrd; "
                 "record W<T>(v: int): IOrd<W<T>> "
                 "  public method CompareTo(other: W<T>): int => "
                 "    (if v > other.v then -1 else "
                 "     (if v < other.v then 1 else 0)); "
                 "end W; ",
                 "var a: Array<W<string>> := "
                 "[new W<string>(3), new W<string>(1)]; "
                 "Console.WriteLine(a.Min().v); "
                 "Console.WriteLine(a.Max().v); ")),
        "3\n"
        "1\n");
}

// =====================================================================
// ROW 7 — `MinOrNull()` OVER A GENERIC RECORD ELEMENT (part 2).
//
// `record W<T>(v: int): IComparable<W<T>>` at `W<string>` — the shape that
// SEGFAULTED `bezelc` (the `!` unwrap read field 1 of a ONE-FIELD struct,
// because the `MinOrNull()` refusal handed back the BARE element where the
// arm's result type is `T?`).
//
// The REVERSED `CompareTo` makes the ordering visible: `MinOrNull()` is the
// LARGEST `v` (3) and `MaxOrNull()` the SMALLEST (1).
//
// Pre-fix: `bezelc` dies with SIGSEGV. Fixed: "3" / "1".
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           min_or_null_over_a_generic_record_element) {
    BEZEL_EXPECT_EQ(
        out(prog(wSelfDecl(),
                 "var a: Array<W<string>> := "
                 "[new W<string>(3), new W<string>(1)]; "
                 "var m := a.MinOrNull(); "
                 "if m <> null then Console.WriteLine($\"{m!.v}\"); end if; "
                 "var x := a.MaxOrNull(); "
                 "if x <> null then Console.WriteLine($\"{x!.v}\"); end if; ")),
        "3\n"
        "1\n");
}

// =====================================================================
// ROW 8 — THE EMPTY SOURCE: stdlib §5.1's `*OrNull` contract is "null on
// an empty source" (the no-selector `Min()`/`Max()` throw instead). The
// generic-record arm keeps that contract.
//
// Pre-fix: `bezelc` dies with SIGSEGV. Fixed: "null".
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           min_or_null_over_an_empty_generic_record_source_is_null) {
    BEZEL_EXPECT_EQ(
        out(prog(wSelfDecl(),
                 "var e: Array<W<string>> := []; "
                 "var m := e.MinOrNull(); "
                 "if m <> null then Console.WriteLine(\"present\"); "
                 "else Console.WriteLine(\"null\"); end if; ")),
        "null\n");
}

// =====================================================================
// ROW 9 — A GENERIC RECORD WITH A `T`-TYPED COMPONENT: `W2<string>`'s
// struct is {ptr, i32} and `W2<int>`'s is {i32, i32}, so the element must
// be read at the INSTANTIATION's own layout.
//
// Pre-fix: `bezelc` dies with SIGSEGV. Fixed: "3" / "1".
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           min_or_null_over_a_generic_record_with_a_t_typed_component) {
    BEZEL_EXPECT_EQ(
        out(prog(w2SelfDecl(),
                 "var a: Array<W2<string>> := "
                 "[new W2<string>(\"x\", 3), new W2<string>(\"y\", 1)]; "
                 "var m := a.MinOrNull(); "
                 "if m <> null then Console.WriteLine($\"{m!.v}\"); end if; "
                 "var x := a.MaxOrNull(); "
                 "if x <> null then Console.WriteLine($\"{x!.v}\"); end if; ")),
        "3\n"
        "1\n");
}

// =====================================================================
// ROW 10 — TWO INSTANTIATIONS IN ONE PROGRAM: the comparator thunk is
// cached per INSTANTIATION, so a fix that minted ONE thunk per record
// DECLARATION would order the second array by the first's `CompareTo`.
//
// Pre-fix: `bezelc` dies with SIGSEGV. Fixed: "3" / "7".
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           min_or_null_over_two_instantiations_in_one_program) {
    BEZEL_EXPECT_EQ(
        out(prog(wSelfDecl(),
                 "var a: Array<W<string>> := "
                 "[new W<string>(3), new W<string>(1)]; "
                 "var b: Array<W<int>> := [new W<int>(7), new W<int>(5)]; "
                 "var m := a.MinOrNull(); "
                 "if m <> null then Console.WriteLine($\"{m!.v}\"); end if; "
                 "var n := b.MinOrNull(); "
                 "if n <> null then Console.WriteLine($\"{n!.v}\"); end if; ")),
        "3\n"
        "7\n");
}

// =====================================================================
// ROW 11 — CONTROL: a NON-GENERIC record element is unchanged — the path
// the generic arm sits beside. It already worked, so it must stay working.
//
// Base and fixed: "3" / "1".
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           control_min_or_null_over_a_non_generic_record_is_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog("record R(v: int): IComparable<R> "
                 "  public method CompareTo(other: R): int => "
                 "    (if v > other.v then -1 else "
                 "     (if v < other.v then 1 else 0)); "
                 "end R; ",
                 "var a: Array<R> := [new R(3), new R(1)]; "
                 "var m := a.MinOrNull(); "
                 "if m <> null then Console.WriteLine($\"{m!.v}\"); end if; "
                 "var x := a.MaxOrNull(); "
                 "if x <> null then Console.WriteLine($\"{x!.v}\"); end if; ")),
        "3\n"
        "1\n");
}

// =====================================================================
// ROW 12 — CONTROL: an `int` element is unchanged (the numeric no-selector
// path). The order is NUMERIC here, so `MinOrNull()` is 1 and
// `MaxOrNull()` is 3.
//
// Base and fixed: "1" / "3".
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           control_min_or_null_over_an_int_element_is_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog("",
                 "var a: Array<int> := [3, 1]; "
                 "var m := a.MinOrNull(); "
                 "if m <> null then Console.WriteLine($\"{m!}\"); end if; "
                 "var x := a.MaxOrNull(); "
                 "if x <> null then Console.WriteLine($\"{x!}\"); end if; ")),
        "1\n"
        "3\n");
}

// =====================================================================
// ROW 13 — CONTROL: the REFUSAL path still reports CLEANLY. A `decimal`
// element is not supported by the no-selector `*OrNull` overload, so the
// call is REFUSED with the diagnostic — never a crash.
//
// Base and fixed: the same diagnostic.
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           control_min_or_null_over_an_unsupported_element_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("",
             "var a: Array<decimal> := [3m, 1m]; "
             "var m := a.MinOrNull(); "
             "if m <> null then Console.WriteLine($\"{m!}\"); end if; "),
        "is not yet implemented for this element type"));
}

// =====================================================================
// ROW 14 — `.Where(…).Min()` WITH NO SELECTOR IS TYPED AS THE ELEMENT.
//
// stdlib §5.1 (v0.355) declares the no-selector form as
//     public extension IEnumerable<T> where T: IComparable<T>
//       public method Min(): T;
// so the result of a LAZY `Where(…).Min()` over a record element is the
// RECORD — `f.v` on the result must resolve.
//
// Pre-fix: the result was typed `int`, so `f.v` was rejected with "no
// member `v` on primitive type `int`".
//
// The REVERSED `CompareTo` makes the ordering visible: `Min()` is the
// LARGEST `v` (3) and `Max()` the SMALLEST (1).
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           where_then_min_over_a_record_element_is_typed_as_the_element) {
    BEZEL_EXPECT_EQ(
        out(prog("record R(v: int): IComparable<R> "
                 "  public method CompareTo(other: R): int => "
                 "    (if v > other.v then -1 else "
                 "     (if v < other.v then 1 else 0)); "
                 "end R; ",
                 "var a: Array<R> := [new R(3), new R(1), new R(2)]; "
                 "var f := a.Where(lambda(x: R) => x.v <> 2).Min(); "
                 "Console.WriteLine($\"{f.v}\"); "
                 "var g := a.Where(lambda(x: R) => x.v <> 2).Max(); "
                 "Console.WriteLine($\"{g.v}\"); ")),
        "3\n"
        "1\n");
}

// =====================================================================
// ROW 15 — THE SAME OVER A GENERIC RECORD ELEMENT: the lazy receiver's
// element type is the record's own INSTANTIATION, so the result is
// `W<string>` — and the element must be read at `W<string>`'s layout.
//
// Pre-fix: "no member `v` on primitive type `int`".
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           where_then_min_over_a_generic_record_element) {
    BEZEL_EXPECT_EQ(
        out(prog(wSelfDecl(),
                 "var a: Array<W<string>> := "
                 "[new W<string>(3), new W<string>(1), new W<string>(2)]; "
                 "var f := a.Where(lambda(x: W<string>) => x.v <> 2).Min(); "
                 "Console.WriteLine($\"{f.v}\"); "
                 "var g := a.Where(lambda(x: W<string>) => x.v <> 2).Max(); "
                 "Console.WriteLine($\"{g.v}\"); ")),
        "3\n"
        "1\n");
}

// =====================================================================
// ROW 16 — THE LAZY RECEIVER'S §5.1 CONSTRAINT: a record with NO
// `CompareTo` has NO order, so stdlib §5.1's `where T: IComparable<T>`
// does not hold and the call must be REFUSED.
//
// Pre-fix: the call was ADMITTED (the result was typed `int`), and the
// compile then ABORTED in codegen — "Both operands to ICmp instruction
// are not of the same type!" → "IR verification failed (malformed IR
// emitted)". The row is RED on the base because the base never emits the
// constraint diagnostic.
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           where_then_min_over_a_record_without_compareto_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("record Plain(v: int) end Plain; ",
             "var a: Array<Plain> := [new Plain(3), new Plain(1)]; "
             "var f := a.Where(lambda(x: Plain) => x.v <> 1).Min(); "
             "Console.WriteLine($\"{f.v}\"); "),
        "Plain does not implement IComparable<T>"));
}

// =====================================================================
// ROW 17 — THE LAZY RECEIVER'S §5.1 CONSTRAINT, AT THE WRONG
// INSTANTIATION: `record W<T>(v: int): IComparable<W<int>>` does not
// implement `IComparable<W<string>>`, so the lazy `MinOrNull()` over
// `Array<W<string>>` is not admissible either.
//
// Pre-fix: the call was ADMITTED and compiled (the `CompareTo` was
// monomorphized at the wrong layout). The row is RED on the base because
// the base never emits the constraint diagnostic.
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           where_then_min_or_null_over_a_generic_record_at_the_wrong_instantiation_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog(wFixedDecl(),
             "var a: Array<W<string>> := "
             "[new W<string>(3), new W<string>(1)]; "
             "var m := a.Where(lambda(x: W<string>) => x.v <> 1).MinOrNull(); "
             "if m <> null then Console.WriteLine($\"{m!.v}\"); end if; "),
        "W does not implement IComparable<T>"));
}

// =====================================================================
// ROW 18 — CONTROL: the lazy `MinOrNull()` over a COMPARABLE record still
// works — the new constraint check must not refuse it.
//
// Base and fixed: "3".
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           control_where_then_min_or_null_over_a_comparable_record) {
    BEZEL_EXPECT_EQ(
        out(prog("record R(v: int): IComparable<R> "
                 "  public method CompareTo(other: R): int => "
                 "    (if v > other.v then -1 else "
                 "     (if v < other.v then 1 else 0)); "
                 "end R; ",
                 "var a: Array<R> := [new R(3), new R(1)]; "
                 "var m := a.Where(lambda(x: R) => x.v <> 1).MinOrNull(); "
                 "if m <> null then Console.WriteLine($\"{m!.v}\"); end if; ")),
        "3\n");
}

// =====================================================================
// ROW 19 — CONTROL: the lazy receiver's `int` element is unchanged (the
// numeric no-selector path; the order is NUMERIC, so `Min()` is 1 and
// `Max()` is 3).
//
// Base and fixed: "1" / "3".
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           control_where_then_min_over_an_int_element_is_unchanged) {
    BEZEL_EXPECT_EQ(
        out(prog("",
                 "var a: Array<int> := [3, 1, 2]; "
                 "var f := a.Where(lambda(x: int) => x <> 2).Min(); "
                 "Console.WriteLine($\"{f}\"); "
                 "var g := a.Where(lambda(x: int) => x <> 2).Max(); "
                 "Console.WriteLine($\"{g}\"); ")),
        "1\n"
        "3\n");
}

// =====================================================================
// ROW 20 — THE CONCRETE `MinOrNull()` APPLIES THE SAME §5.1 CONSTRAINT:
// `record W<T>(v: int): IComparable<W<int>>` does not implement
// `IComparable<W<string>>`, so the no-selector `MinOrNull()` over
// `Array<W<string>>` is not admissible either — exactly as the sibling
// `Min()` over the same array (row 1) is not.
//
// Pre-fix: `bezelc` SEGFAULTED (the `!` unwrap read past the end of the
// refusal's bare element). The row is RED on the base because the base
// never emits the constraint diagnostic.
// =====================================================================
BEZEL_TEST(minmax_constraint_8161,
           min_or_null_over_a_generic_record_at_the_wrong_instantiation_is_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog(wFixedDecl(),
             "var a: Array<W<string>> := "
             "[new W<string>(3), new W<string>(1)]; "
             "var m := a.MinOrNull(); "
             "if m <> null then Console.WriteLine($\"{m!.v}\"); end if; "),
        "W does not implement IComparable<T>"));
}
