// R41 CLUSTER 3 — the stdlib §5.1 AGGREGATION GATES decide conformance BY
// CONFORMANCE, not BY SHAPE (stdlib §5.1 / §2.19, spec §14.2 *Interface
// conformance* rules 1, 2 and 6, spec §3.6 rule 2, v0.355).
//
// THE ROOT CAUSE. `ExprTC::typeCall`'s element-conformance tests classified the
// RECEIVER'S ELEMENT TYPE by its AST SHAPE — "is it a primitive / a class /
// an interface / an unresolved placeholder" — instead of asking whether the
// element type actually CONFORMS to `INumber<Self>`. Three shapes were
// answered by their shape, and each one is a row below:
//
//   * a USER RECORD conformer (`record Money: INumber<Money>`) was REFUSED —
//     a record is NEITHER a class NOR an interface, so it fell to the refusal
//     even though spec §14.2 rule 2 admits it ("records ... implementing it").
//     Issue #8223.
//
//   * a CLASS element was ADMITTED on the ground that "the constraint is
//     checked at instantiation (§14.2)". A CONCRETE receiver has NO
//     instantiation, so the check never happened: codegen lowered the call
//     through the PRIMITIVE accumulator path and emitted malformed IR
//     ("Both operands to a binary operator are not of the same type! %22 =
//     add i32 %21, ptr %20") and `bezelc` aborted with an IR-verification
//     failure. Issue #8339.
//
//   * a TYPE PARAMETER element was ADMITTED UNCONDITIONALLY (the "unresolved
//     placeholder" arm), so a generic body whose `T` guarantees nothing was
//     accepted and lowered per instantiation — at `T = string` it emitted
//     "add i32 %17, ptr %16" and aborted. Issue #8285.
//
// THE FIX. The gate asks the CONFORMANCE question, with the compiler's own
// answers:
//
//   * the USER-RECORD-CONFORMER arm: a Named element whose declaration is a
//     RECORD and whose base closure reaches `INumber<elem>` AT THIS
//     INSTANTIATION (§14.2 rule 2, §12.9's invariant `INumber`) is ADMITTED;
//     the result type is the ELEMENT type (`Sum(): owned T`), so
//     `a.Sum().Cents` resolves.
//
//   * the TYPE-PARAMETER arm: an in-scope type parameter is decided by the
//     parameter's OWN `where` clause (§14.2 rule 6) — the same walk
//     (`linqElemTypeParamClauseUnsatisfied` /
//     `typeParamGuaranteesSelfInterface`) the §26.5.1 applicability filter
//     and the `Math.Min/Max/Clamp` type-argument gate already read, so the
//     filter and this gate cannot disagree. An UNCONSTRAINED parameter, and
//     one whose clauses do not reach `INumber<P>`, guarantee NOTHING and are
//     REFUSED.
//
//   * the CLASS / INTERFACE arm: the shape no longer decides; the
//     CONFORMANCE does. A class that does not conform is REFUSED with the
//     §5.1 / #4730 diagnostic (a located diagnostic instead of an
//     IR-verification abort), and a class that DOES conform is refused with
//     its own message, because codegen's §5.1 fold has no arm for a class
//     element (it lowers the ELEVEN §2.19 primitive conformers and a
//     VALUE-TYPE record conformer) — admitting it would emit the malformed
//     IR the refusal replaces.
//
// The oracle is the STRICT AOT path (`runAot`): the defects were COMPILE-TIME
// IR aborts (the compiler's own `verifyModule` rejected the emitted module),
// so a row must drive the AOT pipeline and observe the program's stdout — a
// clean compile would not be a measurement.

#include <string>

#include "support/aot_result_asserts.hpp"
#include "support/aot_support.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

using bezel::test::JitResult;
using bezel::test::runAot;

namespace {

// A RECORD conformer of `INumber<Money>` (stdlib §2.19's static-interface
// abstraction: `Zero`, `One` and the five arithmetic operators plus `<`).
// §25.2's "a relational operator brings its complete group with it" means the
// conformer declares `>`, `<=` and `>=` as well.
std::string moneyConformer(const char* iface) {
    return std::string("public record Money(Cents: int): ") + iface +
           "<Money>\n"
           "  public static method Zero(): Money => new Money(0);\n"
           "  public static method One(): Money => new Money(1);\n"
           "  public static method operator +(a: Money, b: Money): Money =>"
           " new Money(a.Cents + b.Cents);\n"
           "  public static method operator -(a: Money, b: Money): Money =>"
           " new Money(a.Cents - b.Cents);\n"
           "  public static method operator *(a: Money, b: Money): Money =>"
           " new Money(a.Cents * b.Cents);\n"
           "  public static method operator /(a: Money, b: Money): Money =>"
           " new Money(a.Cents / b.Cents);\n"
           "  public static method operator <(a: Money, b: Money): bool =>"
           " a.Cents < b.Cents;\n"
           "  public static method operator >(a: Money, b: Money): bool =>"
           " a.Cents > b.Cents;\n"
           "  public static method operator <=(a: Money, b: Money): bool =>"
           " a.Cents <= b.Cents;\n"
           "  public static method operator >=(a: Money, b: Money): bool =>"
           " a.Cents >= b.Cents;\n"
           "end Money;\n";
}

}  // namespace

// ===========================================================================
// #8223 — the CONCRETE call over a USER `INumber<T>` RECORD conformer.
//
// PARENT: "`Money` does not conform to `INumber<Money>`" (stdlib §5.1 / spec
// §2.19 / #4730) — a record is neither a class nor an interface, so the gate
// answered by the element's SHAPE. The GENERIC half of the same issue
// (the fold's IR) was closed earlier in the chain; this file owns the
// CONCRETE half, the refusal.
// ===========================================================================

// The issue's own repro, verbatim: `a.Sum()` over `Array<Money>`.
BEZEL_TEST(sema_linq_aggregation_conformance_gates_cluster3,
           concrete_sum_over_user_inumber_record_conformer_is_accepted) {
    JitResult r = runAot("module M;\n" + moneyConformer("INumber") +
        "public class Program\n"
        "  public static method Main()\n"
        "    var a := new Array<Money>();\n"
        "    a.Add(new Money(5)); a.Add(new Money(6));\n"
        "    var s := a.Sum();\n"
        "    Console.WriteLine(s.Cents);\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\n");
}

// The conformance may come through an interface that EXTENDS `INumber<T>`
// (spec §11 rule 2, v0.355: "lists I (or any interface extending I)") — the
// #8198 shape. The gate must read the EXTENSION CHAIN, not the direct base.
BEZEL_TEST(sema_linq_aggregation_conformance_gates_cluster3,
           concrete_sum_over_interface_extending_inumber_conformer_is_accepted) {
    JitResult r = runAot(
        "module M;\n"
        "public interface IMyNum<T>: INumber<T> where T: IMyNum<T>\n"
        "end IMyNum;\n" +
        moneyConformer("IMyNum") +
        "public class Program\n"
        "  public static method Main()\n"
        "    var a := new Array<Money>();\n"
        "    a.Add(new Money(5)); a.Add(new Money(6));\n"
        "    Console.WriteLine(a.Sum().Cents);\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\n");
}

// The ELEVEN §2.19 primitive conformers keep the primitive fold (M5 — the
// MUST-NOT-CHANGE control).
BEZEL_TEST(sema_linq_aggregation_conformance_gates_cluster3,
           control_primitive_conformer_sum_is_unchanged) {
    JitResult r = runAot(
        "module M;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var a := new Array<int>();\n"
        "    a.Add(5); a.Add(6);\n"
        "    Console.WriteLine(a.Sum());\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\n");
}

// ===========================================================================
// #8339 — a CONCRETE `Array<Class>.Sum()` is REFUSED IN SEMA.
//
// PARENT: the class element was ADMITTED by its shape ("the constraint is
// checked at instantiation (§14.2)" — but a concrete receiver has no
// instantiation), so codegen lowered the call through the PRIMITIVE
// accumulator path and the COMPILE aborted with "Both operands to a binary
// operator are not of the same type! %22 = add i32 %21, ptr %20" / "IR
// verification failed (malformed IR emitted)".
// ===========================================================================

BEZEL_TEST(sema_linq_aggregation_conformance_gates_cluster3,
           concrete_sum_over_a_non_conforming_class_element_is_refused) {
    JitResult r = runAot(
        "module M;\n"
        "public class Widget\n"
        "  public N: int;\n"
        "end Widget;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var a := new Array<Widget>();\n"
        "    using var t := new Widget();\n"
        "    a.Add(t);\n"
        "    Console.WriteLine(a.Sum());\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    // The §5.1 / #4730 constraint diagnostic, naming the offending element.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "Sum() requires the element type to satisfy `T: INumber<T>`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Widget` does not conform to `INumber<Widget>`"));
    // AND NOT a codegen abort — the refusal is in SEMA (M1 — the DECIDING
    // ROW: a located diagnostic, not malformed IR).
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "not of the same type"));
}

// A CLASS that DOES conform to `INumber<Self>` (spec §14.2 rule 1) is
// refused with its OWN message: it satisfies the constraint, but codegen's
// §5.1 fold has no arm for a class element (it lowers the ELEVEN §2.19
// primitive conformers and a VALUE-TYPE record conformer), so admitting it
// would emit the malformed IR the refusal replaces.
BEZEL_TEST(sema_linq_aggregation_conformance_gates_cluster3,
           concrete_sum_over_a_conforming_class_element_is_refused_with_its_own_message) {
    JitResult r = runAot(
        "module M;\n"
        "public class Meters: INumber<Meters>\n"
        "  public Cents: int;\n"
        "  public constructor(c: int) Cents := c; end;\n"
        "  public static method Zero(): owned Meters => new Meters(0);\n"
        "  public static method One(): owned Meters => new Meters(1);\n"
        "  public static method operator +(a: Meters, b: Meters): owned Meters =>"
        " new Meters(a.Cents + b.Cents);\n"
        "  public static method operator -(a: Meters, b: Meters): owned Meters =>"
        " new Meters(a.Cents - b.Cents);\n"
        "  public static method operator *(a: Meters, b: Meters): owned Meters =>"
        " new Meters(a.Cents * b.Cents);\n"
        "  public static method operator /(a: Meters, b: Meters): owned Meters =>"
        " new Meters(a.Cents / b.Cents);\n"
        "  public static method operator <(a: Meters, b: Meters): bool =>"
        " a.Cents < b.Cents;\n"
        "  public static method operator >(a: Meters, b: Meters): bool =>"
        " a.Cents > b.Cents;\n"
        "  public static method operator <=(a: Meters, b: Meters): bool =>"
        " a.Cents <= b.Cents;\n"
        "  public static method operator >=(a: Meters, b: Meters): bool =>"
        " a.Cents >= b.Cents;\n"
        "end Meters;\n"
        "public class Program\n"
        "  public static method Main()\n"
        "    var a := new Array<Meters>();\n"
        "    using var t := new Meters(5);\n"
        "    using var u := new Meters(6);\n"
        "    a.Add(t); a.Add(u);\n"
        "    Console.WriteLine(a.Sum().Cents);\n"
        "  end Main;\n"
        "end Program;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`Meters` DOES conform to `INumber<Meters>`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "no lowering for a CLASS or INTERFACE element"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

// ===========================================================================
// #8285 — the no-selector `Sum()` in a generic body whose `T` does not
// guarantee `INumber<T>` is REFUSED AT THE DECLARATION.
//
// PARENT: the type-parameter element was ADMITTED by its shape (the
// "unresolved placeholder" arm), so the body was accepted; at `T = string`
// the instantiation emitted "add i32 %17, ptr %16" and the COMPILE aborted
// with an IR-verification failure. spec §3.6 rule 2 (v0.355): "A generic body
// type-checks under the weakest assumption its constraints establish"; spec
// §14.2 rule 6: a type parameter conforms only when its OWN constraints
// guarantee it.
// ===========================================================================

BEZEL_TEST(sema_linq_aggregation_conformance_gates_cluster3,
           unconstrained_type_parameter_sum_is_refused_at_the_declaration) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method G<T>(a: Array<T>): int => a.Sum();\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(5); a.Add(6);\n"
        "    Console.WriteLine(G(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "Sum() requires the element type to satisfy `T: INumber<T>`"));
    // The element is named by its own spelling (§14.1 — "a type parameter has
    // no qualified spelling"), not by the `<element>` placeholder.
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`T` does not conform to `INumber<T>`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
}

// The SAME row over the element's OWN type (`: T`, the §5.1 signature
// `Sum(): owned T`) — the shape the r41 review of the parallel
// `r41-fix-typeparam-bounds` lane named: "the unconstrained half still
// compiles and prints `11`, and at `T = string` the tip still aborts with
// malformed IR". Without the fix this body COMPILES (the §5.1 candidate is
// admitted by the placeholder arm) and the `T = string` instantiation aborts
// the compile with "add i32 %17, ptr %16"; with the fix the DECLARATION is
// refused, so no instantiation is ever lowered.
BEZEL_TEST(sema_linq_aggregation_conformance_gates_cluster3,
           unconstrained_type_parameter_sum_over_its_own_type_is_refused_at_the_declaration) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method G<T>(a: Array<T>): T => a.Sum();\n"
        "  public static method Main(): int\n"
        "    var a := new Array<string>();\n"
        "    a.Add(\"x\"); a.Add(\"y\");\n"
        "    Console.WriteLine(G(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "Sum() requires the element type to satisfy `T: INumber<T>`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`T` does not conform to `INumber<T>`"));
    // NOT a codegen abort — the refusal is in SEMA, at the declaration.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "not of the same type"));
}

// The `where T: IComparable<T>` twin: the constraint does NOT reach
// `INumber<T>` (§14.2 rule 6 — the bound names `IComparable`, not
// `INumber`), so the §5 `Sum()` is not applicable and the body is refused
// AT THE DECLARATION — never accepted and left to the instantiation to
// miscompile (the parent's "add i32 %17, ptr %16" at `T = string`).
BEZEL_TEST(sema_linq_aggregation_conformance_gates_cluster3,
           icomparable_only_type_parameter_sum_is_refused_not_malformed_ir) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method G<T>(a: Array<T>): T where T: IComparable<T> =>"
        " a.Sum();\n"
        "  public static method Main(): int\n"
        "    var a := new Array<string>();\n"
        "    a.Add(\"x\"); a.Add(\"y\");\n"
        "    Console.WriteLine(G(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "Sum() requires the element type to satisfy `T: INumber<T>`"));
    BEZEL_EXPECT_TRUE(diagnosticMessageContains(
        r, "`T` does not conform to `INumber<T>`"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "not of the same type"));
}

// CONTROL: a type parameter whose OWN `where` clause DOES guarantee
// `INumber<T>` (§14.2 rule 6) is still admitted, and the fold still runs.
BEZEL_TEST(sema_linq_aggregation_conformance_gates_cluster3,
           control_inumber_bounded_type_parameter_sum_is_still_accepted) {
    JitResult r = runAot(
        "module M;\n"
        "class C\n"
        "  public static method G<T>(a: Array<T>): T where T: INumber<T> =>"
        " a.Sum();\n"
        "  public static method Main(): int\n"
        "    var a := new Array<int>();\n"
        "    a.Add(5); a.Add(6);\n"
        "    Console.WriteLine(G(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "11\n");
}

// CONTROL: an unconstrained generic body whose `Sum()` is answered by a
// USER's applicable `extension Object { Sum() }` (§26.5.1 — the §5 block is
// not a candidate there, so the user's member answers) is UNCHANGED. This is
// the #8181 behaviour the type-parameter arm must not disturb.
BEZEL_TEST(sema_linq_aggregation_conformance_gates_cluster3,
           control_user_extension_still_answers_an_unconstrained_generic_body) {
    JitResult r = runAot(
        "module M;\n"
        "extension Object\n"
        "  public method Sum(): int => 4242;\n"
        "end;\n"
        "class C\n"
        "  static method G<T>(a: Array<T>): int => a.Sum();\n"
        "  public static method Main(): int\n"
        "    var a := new Array<string>();\n"
        "    a.Add(\"x\");\n"
        "    Console.WriteLine(G(a));\n"
        "    return 0;\n"
        "  end Main;\n"
        "end C;\n");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 0);
    BEZEL_EXPECT_EQ(r.stdoutText, "4242\n");
}
