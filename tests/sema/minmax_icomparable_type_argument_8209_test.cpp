// Regression tests for issue #8209 — the no-selector `Min()` / `Max()` /
// `MinOrNull()` / `MaxOrNull()` `IComparable<T>` gate matched the interface
// NAME through the base closure and never checked its TYPE ARGUMENT.
//
// WHAT THE DOCUMENTS REQUIRE (each §-number opened at v0.355 in this tree).
//
//   * stdlib §5.1 "No-selector aggregation overloads" declares
//         public extension IEnumerable<T> where T: IComparable<T>
//           public method Min(): T;              // InvalidOperationException if empty
//           public method Max(): T;              // InvalidOperationException if empty
//           public method MinOrNull(): T?;       // null if empty
//           public method MaxOrNull(): T?;       // null if empty
//     so the constraint is EXACTLY `T: IComparable<T>` — the type argument of
//     the constraint's interface must be the element type ITSELF.
//   * stdlib §2.8 "`IComparable<T>`" declares the interface's authority:
//         public interface IComparable<T>
//           method CompareTo(other: T): int;
//         end IComparable;
//     — so a class implementing `IComparable<int>` declares a
//     `CompareTo(other: int)`, which is NOT the member `IComparable<K>`
//     requires.
//   * spec §14.2 "Constraints" *Interface conformance* rule 1 (v0.355): "T is
//     a class that lists I (or any interface extending I) in its inheritance
//     clause, that inherits from a class which conforms to I" — the
//     conformance is to `I` ITSELF, i.e. `IComparable<K>` for an element of
//     type `K`, not to any instantiation of the name `IComparable`.
//   * spec §14.1 "Generic declarations" *Instantiation and its site* (v0.355):
//     "An instantiation of a generic declaration is the constructed type or
//     constructed member obtained by substituting type arguments for its type
//     parameters" — so the interface a generic class implements is
//     re-expressed in the INSTANCE's arguments before it is compared.
//
// THE DEFECT (MEASURED on r30-c1, the base). The gate read the interface NAME
// through the base closure (`inheritsBuiltinInterface`) and never looked at
// the type argument, so a class implementing `IComparable<int>` satisfied
// `where T: IComparable<T>` at an element of type `K`. Sema ADMITTED the
// call and codegen then dispatched the comparison through `CompareTo(other:
// int)` with an `other: K` operand:
//     Call parameter type does not match function signature!
//       %mm.acc = load ptr, ptr %3, align 8
//      i32  %57 = call i32 @"bz$M.K$CompareTo"(ptr %mm.el, ptr %mm.acc)
//     bezelc: codegen error: IR verification failed (malformed IR emitted)
// — a compiler ABORT, not a diagnostic. Three spellings reach it (the issue's
// own three): the DIRECT base, an EXTENDING INTERFACE, and a GENERIC BASE.
//
// ORACLE. `runAot` (the AOT path). Every rejecting row asserts the SPECIFIC
// diagnostic text, so a program rejected for an unrelated reason (or that
// compiled) cannot satisfy the row. Every accepting row asserts the EXACT
// STDOUT — a VALUE oracle, never "no diagnostic was produced".
//
// ⚠ THE CONTROLS ARE THE POINT. The fix must reject the WRONG type argument
// and admit the RIGHT one, in every spelling: `IComparable<K>` (direct), an
// extending interface carrying `IComparable<K>`, and a generic base carrying
// `IComparable<T>` reached at `B<K>`. The controls are green on the BASE and
// on the fix — the positive control that shows the shape is genuinely
// admitted rather than rejected for an unrelated reason.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// The program's stdout, verbatim. "<did-not-run>" when the program was
// rejected, crashed or timed out.
std::string out(const std::string& program) {
    JitResult r = runAot(program);
    if (!r.ran) return std::string("<did-not-run>");
    return r.stdoutText;
}

// REJECTION with the SPECIFIC diagnostic text: a program that was rejected
// for an unrelated reason (or that compiled) must NOT satisfy the row.
bool rejectsWith(const std::string& program, const std::string& needle) {
    JitResult r = runAot(program);
    if (r.ran || !r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

// The §5.1 constraint diagnostic the no-selector `Min()` / `Max()` gate
// reports (stdlib §5.1 / §14.2 / #1198).
const char* kConstraintNeedle = "does not implement IComparable<T>";

// The `*OrNull` sibling's wording (stdlib §5.1 / §14.2 / #1198).
const char* kOrNullConstraintNeedle =
    "requires the element type to implement IComparable<T>";

std::string prog(const std::string& decls, const std::string& body) {
    return "module M;\n" + decls +
           "public class C\n"
           "  public static method Main(): int\n" + body +
           "    return 0;\n"
           "  end Main;\n"
           "end C;\n";
}

}  // namespace

// =====================================================================
// ROW 1 — THE DIRECT SPELLING. `class K: IComparable<int>` does NOT
// implement `IComparable<K>`, so stdlib §5.1's `where T: IComparable<T>` is
// not satisfied at an element of type `K` and the call must be REJECTED at
// compile time.
//
// Pre-fix: the call was ADMITTED and the compile ABORTED in codegen ("IR
// verification failed"). The row is RED on the base because the base does not
// report this diagnostic.
// =====================================================================
BEZEL_TEST(minmax_icomparable_type_argument_8209,
           class_with_icomparable_of_another_type_is_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("public class K: IComparable<int>\n"
             "  public X: int;\n"
             "  public constructor(x: int)\n"
             "    X := x;\n"
             "  end;\n"
             "  public method CompareTo(other: int): int => X - other;\n"
             "end K;\n",
             "    var a: Array<K> := [new K(2), new K(1)];\n"
             "    var m := a.Min();\n"
             "    Console.WriteLine(m.X);\n"),
        kConstraintNeedle));
}

// =====================================================================
// ROW 2 — THE EXTENDING-INTERFACE SPELLING (the issue's own repro).
// `interface IW: IComparable<int>` + `class K: IW`: rule 1 admits a class
// that lists an interface EXTENDING `IComparable` — but the interface it
// extends is `IComparable<int>`, not `IComparable<K>`.
// =====================================================================
BEZEL_TEST(minmax_icomparable_type_argument_8209,
           class_through_an_extending_interface_of_another_type_is_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("public interface IW: IComparable<int> end IW;\n"
             "public class K: IW\n"
             "  public X: int;\n"
             "  public constructor(x: int)\n"
             "    X := x;\n"
             "  end;\n"
             "  public method CompareTo(other: int): int => X - other;\n"
             "end K;\n",
             "    var a: Array<K> := [new K(2), new K(1)];\n"
             "    var m := a.Min();\n"
             "    Console.WriteLine(m.X);\n"),
        kConstraintNeedle));
}

// =====================================================================
// ROW 3 — THE GENERIC-BASE SPELLING. `class B<T>: IComparable<int>` +
// `class K: B<int>`: rule 1 admits a class that "inherits from a class which
// conforms to I" — but `B<int>` conforms to `IComparable<int>`, not to
// `IComparable<K>`.
// =====================================================================
BEZEL_TEST(minmax_icomparable_type_argument_8209,
           class_through_a_generic_base_of_another_type_is_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("public class B<T>: IComparable<int>\n"
             "  public X: int;\n"
             "  public constructor() X := 0; end;\n"
             "  public method CompareTo(other: int): int => X - other;\n"
             "end B;\n"
             "public class K: B<int>\n"
             "  public constructor() base(); end;\n"
             "end K;\n",
             "    var a: Array<K> := [new K(), new K()];\n"
             "    var m := a.Min();\n"
             "    Console.WriteLine(m.X);\n"),
        kConstraintNeedle));
}

// =====================================================================
// ROW 4 — THE SIBLING MEMBER. `Max()` shares the gate with `Min()`, so the
// fix cannot be for `Min()` alone.
// =====================================================================
BEZEL_TEST(minmax_icomparable_type_argument_8209,
           max_over_a_class_with_icomparable_of_another_type_is_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("public class K: IComparable<int>\n"
             "  public X: int;\n"
             "  public constructor(x: int)\n"
             "    X := x;\n"
             "  end;\n"
             "  public method CompareTo(other: int): int => X - other;\n"
             "end K;\n",
             "    var a: Array<K> := [new K(2), new K(1)];\n"
             "    var m := a.Max();\n"
             "    Console.WriteLine(m.X);\n"),
        kConstraintNeedle));
}

// =====================================================================
// ROW 5 — THE `*OrNull` SIBLINGS. stdlib §5.1 declares
// `MinOrNull(): T?` / `MaxOrNull(): T?` under the SAME `where T:
// IComparable<T>`, and the two `*OrNull` gates are separate sites (the
// concrete one and the LAZY one) — so both must reject the wrong type
// argument.
// =====================================================================
BEZEL_TEST(minmax_icomparable_type_argument_8209,
           min_or_null_over_a_class_with_icomparable_of_another_type_is_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("public class K: IComparable<int>\n"
             "  public X: int;\n"
             "  public constructor(x: int)\n"
             "    X := x;\n"
             "  end;\n"
             "  public method CompareTo(other: int): int => X - other;\n"
             "end K;\n",
             "    var a: Array<K> := [new K(2), new K(1)];\n"
             "    var m := a.MinOrNull();\n"
             "    Console.WriteLine(m.X);\n"),
        kOrNullConstraintNeedle));
}

BEZEL_TEST(minmax_icomparable_type_argument_8209,
           lazy_min_or_null_over_a_class_with_icomparable_of_another_type_is_rejected) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("public class K: IComparable<int>\n"
             "  public X: int;\n"
             "  public constructor(x: int)\n"
             "    X := x;\n"
             "  end;\n"
             "  public method CompareTo(other: int): int => X - other;\n"
             "end K;\n",
             "    var a: Array<K> := [new K(2), new K(1)];\n"
             "    var m := a.Where(lambda(x: K) => x.X > 0).MinOrNull();\n"
             "    Console.WriteLine(m.X);\n"),
        kOrNullConstraintNeedle));
}

// =====================================================================
// CONTROL 1 — THE DIRECT SELF ARGUMENT. `class K: IComparable<K>` DOES
// implement `IComparable<K>`, so the call is admitted and the element's own
// `CompareTo` orders the elements. GREEN on the base AND on the fix.
//
// The elements are 3 and 1 and the `CompareTo` is the natural one, so the
// minimum is the element with X = 1.
// =====================================================================
BEZEL_TEST(minmax_icomparable_type_argument_8209,
           control_class_with_icomparable_of_its_own_type_is_still_ordered) {
    BEZEL_EXPECT_EQ(
        out(prog("public class K: IComparable<K>\n"
                 "  public X: int;\n"
                 "  public constructor(x: int)\n"
                 "    X := x;\n"
                 "  end;\n"
                 "  public method CompareTo(other: K): int => X - other.X;\n"
                 "end K;\n",
                 "    var a: Array<K> := [new K(3), new K(1)];\n"
                 "    var m := a.Min();\n"
                 "    Console.WriteLine(m.X);\n")),
        "1\n");
}

// =====================================================================
// CONTROL 2 — THE EXTENDING-INTERFACE SPELLING WITH THE SELF ARGUMENT:
// `interface IOrd<T>: IComparable<T>` + `class K: IOrd<K>`. The closure
// reaches `IComparable<K>` after §14.1's substitution, so the call is
// admitted. GREEN on the base AND on the fix.
// =====================================================================
BEZEL_TEST(minmax_icomparable_type_argument_8209,
           control_class_through_an_extending_interface_of_its_own_type_is_still_ordered) {
    BEZEL_EXPECT_EQ(
        out(prog("public interface IOrd<T>: IComparable<T> end IOrd;\n"
                 "public class K: IOrd<K>\n"
                 "  public X: int;\n"
                 "  public constructor(x: int)\n"
                 "    X := x;\n"
                 "  end;\n"
                 "  public method CompareTo(other: K): int => X - other.X;\n"
                 "end K;\n",
                 "    var a: Array<K> := [new K(3), new K(1)];\n"
                 "    var m := a.Min();\n"
                 "    Console.WriteLine(m.X);\n")),
        "1\n");
}

// =====================================================================
// CONTROL 3 — THE GENERIC-BASE SPELLING WITH THE SELF ARGUMENT:
// `class B<T>: IComparable<T>` + `class D: B<D>`. §14.1's substitution
// re-expresses the base's `IComparable<T>` as `IComparable<D>` at `B<D>`,
// which IS the element type — so the call is admitted. GREEN on the base AND
// on the fix.
// =====================================================================
BEZEL_TEST(minmax_icomparable_type_argument_8209,
           control_class_through_a_generic_base_of_its_own_type_is_still_ordered) {
    BEZEL_EXPECT_EQ(
        out(prog("public class B<T>: IComparable<T>\n"
                 "  public X: int;\n"
                 "  public constructor(x: int)\n"
                 "    X := x;\n"
                 "  end;\n"
                 "  public virtual method CompareTo(other: T): int => 0;\n"
                 "end B;\n"
                 "public class D: B<D>\n"
                 "  public constructor(x: int)\n"
                 "    base(x);\n"
                 "  end;\n"
                 "  public override method CompareTo(other: D): int =>\n"
                 "    X - other.X;\n"
                 "end D;\n",
                 "    var a: Array<D> := [new D(3), new D(1)];\n"
                 "    var m := a.Min();\n"
                 "    Console.WriteLine(m.X);\n")),
        "1\n");
}

// =====================================================================
// CONTROL 4 — THE RECORD FAMILY IS UNCHANGED. The record gate already
// followed the type argument AT THE INSTANTIATION (#8161 / #8139), so a
// record implementing `IComparable<Other>` was already refused, and a record
// implementing `IComparable<Self>` was already admitted. Both verdicts must
// survive the class/interface fix unchanged. GREEN on the base AND on the
// fix.
// =====================================================================
BEZEL_TEST(minmax_icomparable_type_argument_8209,
           control_record_family_verdicts_are_unchanged) {
    // The record with a WRONG type argument stays refused.
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("public record Other(y: int);\n"
             "public interface IOrd<T>: IComparable<T> end IOrd;\n"
             "record Pt(x: int): IOrd<Other>\n"
             "  public method CompareTo(other: Other): int => x - other.y;\n"
             "end Pt;\n",
             "    var a: Array<Pt> := [new Pt(3), new Pt(1)];\n"
             "    var m := a.Min();\n"
             "    Console.WriteLine(m.x);\n"),
        "Pt does not implement IComparable<T>"));
    // The record with the SELF argument stays admitted and ordered.
    BEZEL_EXPECT_EQ(
        out(prog("record Pt(x: int): IComparable<Pt>\n"
                 "  public method CompareTo(other: Pt): int => x - other.x;\n"
                 "end Pt;\n",
                 "    var a: Array<Pt> := [new Pt(3), new Pt(1)];\n"
                 "    var m := a.Min();\n"
                 "    Console.WriteLine(m.x);\n")),
        "1\n");
}

// =====================================================================
// CONTROL 5 — A CLASS THAT REACHES NO `IComparable` AT ALL stays refused by
// the gate's own diagnostic (the #8185 control). GREEN on the base AND on
// the fix.
// =====================================================================
BEZEL_TEST(minmax_icomparable_type_argument_8209,
           control_class_reaching_no_icomparable_stays_refused) {
    BEZEL_EXPECT_TRUE(rejectsWith(
        prog("public interface IOrd2 end IOrd2;\n"
             "public class K: IOrd2\n"
             "  public X: int;\n"
             "  public constructor(x: int)\n"
             "    X := x;\n"
             "  end;\n"
             "end K;\n",
             "    var a: Array<K> := [new K(3), new K(1)];\n"
             "    var m := a.Min();\n"
             "    Console.WriteLine(m.X);\n"),
        "K does not implement IComparable<T>"));
}
