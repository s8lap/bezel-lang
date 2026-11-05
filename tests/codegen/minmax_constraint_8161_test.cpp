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
