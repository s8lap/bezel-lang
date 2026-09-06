// Regression tests for #3929 (§25/§25.1/§25.2/§11.8, v0.294): a relational
// operator (`<`, `>`, `<=`, `>=`) on an interface- or choice-typed operand
// with no declared comparison overload reached codegen and crashed LLVM's
// IR verifier, instead of getting a clean sema rejection. The sweep for this
// fix also found the same "no applicable overload" gap on the RIGHT operand
// of an arithmetic/bitwise operator.
//
// Follow-up (#3952, caught in review): the FIRST cut of this fix reused the
// record/class/choice diagnostic wording ("declare a matching `operator X`")
// for an interface operand too. That advice is correct for a choice (it CAN
// declare an operator; `choice_with_declared_comparison_group_still_works`
// below is the control proving so) but is actively WRONG for an interface —
// per §11.8, an interface's static operator (however it is declared) is
// reachable only through a constrained generic type parameter, never
// through a plain interface-typed value, so "declare one" sends the user
// down a path that cannot fix their code. This file's interface rows now
// assert the DEDICATED interface diagnostic instead, including the row the
// reviewer built by hand: an interface that DOES declare a matching static
// operator, with a conforming class, still rejected — with the message
// explaining why, not telling the user to declare one.
//
// Authority:
//   §25 *Operator Overloading* — "Records, enums, choices ... and classes
//   may overload operators." Interfaces are not on this list.
//   §25.2 *Restrictions and resolution* — "Comparison operators must be
//   overloaded as a complete group: `<`, `>`, `<=`, `>=` together." The
//   Resolution paragraph draws the candidate set from "the static types of
//   both operands."
//   §11.8 *Static interface members*, "Static-member interfaces as ordinary
//   types" — verbatim: "through an interface-typed value only instance
//   members are reachable, and calling a static member on the interface
//   name (`INumber<int>.Zero()`) is a compile error — the interface
//   declares no bodies to run. `T.Member(...)` dispatch requires a generic
//   type parameter constrained by the interface: a value's static type
//   names no single conforming type, so "static dispatch through a value"
//   would be hidden dynamic dispatch, which Bezel declines."
//
// THE DEFECT — measured on `5d43c87` (the #3903 fix, immediately before this
// one), `bezelc -o /tmp/x prog.bzl`:
//
//     module M;
//     public interface IFoo end IFoo;
//     class Box: IFoo end Box;
//     class C public static method Main(): int
//       var b: IFoo := new Box();
//       var d := b < 1;
//       return 0;
//     end Main; end C;
//
//   produced:
//     Both operands to ICmp instruction are not of the same type!
//       %7 = icmp slt ptr %6, i32 1
//     bezelc: codegen error: IR verification failed (malformed IR emitted...)
//
//   and the analogous choice program (`var s: Shape := Shape.Circle(1); var
//   d := s < 1;`) produced the same class of crash (`icmp slt %choice.Shape
//   %2, i32 1`).
//
// Root cause: `typeBinary`'s §25.2 relational guard
// (`src/sema/ExprTypecheck_Exprs.cpp`) filtered `memberTargetType`'s result
// to `ts->kind == Record || ts->kind == Class` only — omitting Interface and
// Choice — so those operands fell through this record/class-only gate and
// reached codegen typed `bool` by the generic relational fallback further
// down, which has NO Named-type backstop at all (unlike the arithmetic path,
// which at least checks the LEFT operand). The fix widens the SAME
// memberTargetType-based, both-operands-loop check (and its §25.1 arithmetic/
// bitwise sibling a few lines below) to also accept Choice, and adds a
// SEPARATE, dedicated check (same shape, own message) for Interface — three
// distinct "no applicable overload" reasons (Object can never declare one;
// record/class/choice can declare, didn't; interface can be declared on but
// never reached through a value) get three messages, not a merge.
//
// The sweep for this issue also found a second, related gap: for arithmetic
// (`+ - * / mod`) and bitwise (`& | ^ << >>`) operators, an interface/choice
// operand as the LEFT operand was already rejected by a later generic
// "Named-type" backstop — but that backstop only inspects the LEFT operand,
// so the SWAPPED form (`1 + b`, `1 << b`) fell through it too: `+ - * / mod`
// died in the IR verifier the same way as the relational case, and
// `& | ^ << >>` didn't even crash — they hit the unrelated #2434
// lower-to-zero backstop and silently compiled to a WRONG value (a
// zero-initialized slot, exit 0, only an "internal:" stderr note) — the same
// three-symptom pattern #3903 found for `Object`. Because the §25.1 gates
// this fix widens/adds already loop over BOTH operands, the same widening
// closes this gap too, in one place, for both operand orders.

#include "support/aot_result_asserts.hpp"  // diagnosticMessageContains
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

using bezel::test::runAot;
using bezel::test::diagnosticMessageContains;

namespace {

// Mirrors binary_op_object_operand_rejected_3903_test.cpp's helper: true iff
// the diagnostics list carries a message containing every needle.
bool diagnosticsMentionAll(const bezel::DiagnosticEngine& de,
                            std::initializer_list<const char*> needles) {
    for (const auto& d : de.diagnostics()) {
        bool allFound = true;
        for (const char* n : needles) {
            if (d.message.find(n) == std::string::npos) {
                allFound = false;
                break;
            }
        }
        if (allFound) return true;
    }
    return false;
}

bool noDiagnosticMentions(const bezel::DiagnosticEngine& de, const char* sub) {
    for (const auto& d : de.diagnostics())
        if (d.message.find(sub) != std::string::npos) return false;
    return true;
}

const char* kIfaceDecls =
    "public interface IFoo end IFoo; class Box: IFoo end Box; ";
const char* kChoiceDecls =
    "public choice Shape Circle(radius: int), Square(side: int); end Shape; ";

// A conforming pair using §11.8's recurrence pattern (mirrors the spec's own
// `INumber<T>` example): the interface DOES declare `operator +`/`<` etc.,
// and `Meters` supplies a matching concrete witness — so the reviewer's
// scenario ("give IFoo a matching static operator and a conforming Box")
// is exercised against a program that is otherwise fully well-formed.
const char* kRecurrenceDecls =
    "public interface INumM<T> where T: INumM<T> "
    "  static operator +(a: T, b: T): owned T; "
    "  static operator <(a: T, b: T): bool; "
    "  static operator >(a: T, b: T): bool; "
    "  static operator <=(a: T, b: T): bool; "
    "  static operator >=(a: T, b: T): bool; "
    "end INumM; "
    "public class Meters: INumM<Meters> "
    "  public n: int; "
    "  public constructor(x: int) n := x; end constructor; "
    "  public static method operator +(a: Meters, b: Meters): owned Meters "
    "    => new Meters(a.n + b.n); "
    "  public static method operator <(a: Meters, b: Meters): bool "
    "    => a.n < b.n; "
    "  public static method operator >(a: Meters, b: Meters): bool "
    "    => a.n > b.n; "
    "  public static method operator <=(a: Meters, b: Meters): bool "
    "    => a.n <= b.n; "
    "  public static method operator >=(a: Meters, b: Meters): bool "
    "    => a.n >= b.n; "
    "end Meters; ";

// Compiles `body` inside a standard `Main(): int` wrapper and asserts a
// clean, real-location §25.2 rejection naming `op` and `typeName` with the
// record/class/choice wording ("declare a matching operator ..."), and that
// it never reaches LLVM's IR verifier (the crash this fix replaces). Used
// for CHOICE only — see `expectInterfaceOperatorUnreachable` for interface.
void expectRelationalRejected(const char* declSnippet, const std::string& body,
                               const char* op, const char* typeName) {
    auto r = runAot(std::string("module M; ") + declSnippet +
                     "class C public static method Main(): int " + body +
                     " end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticsMentionAll(
        r.diagnostics,
        {(std::string("operator `") + op + "` is not defined for type `" +
          typeName + "`")
             .c_str(),
         "comparison operators must be explicitly overloaded",
         "\xC2\xA7"
         "25.2"}));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "codegen error"));
}

// Same shape, for the §25.1 arithmetic/bitwise family's diagnostic wording.
// Used for CHOICE only.
void expectArithmeticRejected(const char* declSnippet, const std::string& body,
                               const char* op, const char* typeName) {
    auto r = runAot(std::string("module M; ") + declSnippet +
                     "class C public static method Main(): int " + body +
                     " end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticsMentionAll(
        r.diagnostics,
        {(std::string("operator `") + op + "` is not defined for type `" +
          typeName + "`")
             .c_str(),
         "declare a matching `operator", "\xC2\xA7""25"}));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "codegen error"));
    // Not the #2434 lower-to-zero silent-degrade path either.
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "degraded to a "
                                                      "zero-initialized"));
}

// The INTERFACE-specific diagnostic (#3952): asserts the dedicated §11.8
// wording — and, critically, asserts the record/class/choice-style
// "declare a matching operator" wording is ABSENT, so a future drift back
// toward the merged wording fails this test.
void expectInterfaceOperatorUnreachable(const char* declSnippet,
                                         const std::string& body,
                                         const char* op,
                                         const char* typeName) {
    auto r = runAot(std::string("module M; ") + declSnippet +
                     "class C public static method Main(): int " + body +
                     " end Main; end C;");
    BEZEL_EXPECT_FALSE(r.ran);
    BEZEL_EXPECT_TRUE(diagnosticsMentionAll(
        r.diagnostics,
        {(std::string("operator `") + op + "` is not defined for type `" +
          typeName + "`")
             .c_str(),
         "reachable only through a generic type parameter",
         "\xC2\xA7""11.8",
         "Narrow to the concrete conforming type first"}));
    BEZEL_EXPECT_TRUE(noDiagnosticMentions(r.diagnostics,
                                            "declare a matching `operator"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "IR verification failed"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "codegen error"));
    BEZEL_EXPECT_FALSE(diagnosticMessageContains(r, "degraded to a "
                                                      "zero-initialized"));
}

}  // namespace

// ── Relational — interface operand, no declared overload ─────────────────
// (interface-specific diagnostic, #3952)

BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929, iface_lt_int) {
    expectInterfaceOperatorUnreachable(
        kIfaceDecls, "var b: IFoo := new Box(); var d := b < 1; return 0;",
        "<", "IFoo");
}

BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929, iface_gt_int) {
    expectInterfaceOperatorUnreachable(
        kIfaceDecls, "var b: IFoo := new Box(); var d := b > 1; return 0;",
        ">", "IFoo");
}

BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929, iface_le_int) {
    expectInterfaceOperatorUnreachable(
        kIfaceDecls, "var b: IFoo := new Box(); var d := b <= 1; return 0;",
        "<=", "IFoo");
}

BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929, iface_ge_int) {
    expectInterfaceOperatorUnreachable(
        kIfaceDecls, "var b: IFoo := new Box(); var d := b >= 1; return 0;",
        ">=", "IFoo");
}

// Swapped operand order — the memberTargetType loop walks both operands, so
// the interface on the RIGHT is caught the same way.
BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929, int_lt_iface_swap) {
    expectInterfaceOperatorUnreachable(
        kIfaceDecls, "var b: IFoo := new Box(); var d := 1 < b; return 0;",
        "<", "IFoo");
}

// The reviewer's scenario (#3952): IFoo/Meters use §11.8's recurrence
// pattern, so `INumM` genuinely DOES declare `operator <`, and `Meters`
// supplies a matching concrete witness — yet `b < t` (`b: INumM<Meters>`,
// the INTERFACE type, not the concrete `Meters`) is STILL rejected, because
// the operator is reachable only through a constrained type parameter,
// never through a plain interface-typed value. This is the row that rules
// out "declare a matching operator" as correct advice: one already exists,
// and it doesn't help.
BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929,
           iface_with_declared_matching_operator_still_rejected) {
    expectInterfaceOperatorUnreachable(
        kRecurrenceDecls,
        "var b: INumM<Meters> := new Meters(3); "
        "var t: INumM<Meters> := new Meters(4); "
        "var d := b < t; return 0;",
        "<", "INumM");
}

BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929,
           iface_with_declared_matching_plus_operator_still_rejected) {
    expectInterfaceOperatorUnreachable(
        kRecurrenceDecls,
        "var b: INumM<Meters> := new Meters(3); "
        "var t: INumM<Meters> := new Meters(4); "
        "var d := b + t; return 0;",
        "+", "INumM");
}

// ── MUST-PASS: the diagnostic's remedies actually work ────────────────────
// (proves the advice is actionable, not just non-misleading)

// Remedy 1: narrow to the concrete conforming type first (`is`/`as`) — its
// OWN declared operator is then reachable (an ordinary Meters-typed
// operand, handled by the plain findBinaryOperator path).
BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929,
           iface_remedy_narrow_to_concrete_type_works) {
    auto r = runAot(
        std::string("module M; ") + kRecurrenceDecls +
        "class C public static method Main(): int "
        "  var b: INumM<Meters> := new Meters(3); "
        "  if b is Meters then "
        "    var bm := (b as Meters)!; "
        "    var d := bm + new Meters(4); "
        "    return d.n; "
        "  end if; "
        "  return -1; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// Remedy 2: constrain a generic type parameter by the interface (§11.8's
// own `Sum<T>`-shaped pattern) — the operator resolves through the
// constraint at instantiation.
BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929,
           iface_remedy_generic_constraint_works) {
    auto r = runAot(
        std::string("module M; ") + kRecurrenceDecls +
        "class C "
        "  public static method AddThem<T>(a: T, b: T): owned T "
        "    where T: INumM<T> => a + b; "
        "  public static method Main(): int "
        "    var m1 := new Meters(3); "
        "    var m2 := new Meters(4); "
        "    using var r := AddThem(m1, m2); "
        "    return r.n; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}

// ── Relational — choice operand, no declared overload ─────────────────────
// (record/class/choice diagnostic wording — choice CAN declare an operator)

BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929, choice_lt_int) {
    expectRelationalRejected(
        kChoiceDecls,
        "var s: Shape := Shape.Circle(1); var d := s < 1; return 0;", "<",
        "Shape");
}

BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929, choice_gt_int) {
    expectRelationalRejected(
        kChoiceDecls,
        "var s: Shape := Shape.Circle(1); var d := s > 1; return 0;", ">",
        "Shape");
}

BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929, choice_le_int) {
    expectRelationalRejected(
        kChoiceDecls,
        "var s: Shape := Shape.Circle(1); var d := s <= 1; return 0;", "<=",
        "Shape");
}

BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929, choice_ge_int) {
    expectRelationalRejected(
        kChoiceDecls,
        "var s: Shape := Shape.Circle(1); var d := s >= 1; return 0;", ">=",
        "Shape");
}

BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929, int_gt_choice_swap) {
    expectRelationalRejected(
        kChoiceDecls,
        "var s: Shape := Shape.Circle(1); var d := 1 > s; return 0;", ">",
        "Shape");
}

// ── Arithmetic / bitwise — the RIGHT-operand gap the sweep also found ─────
// (the LEFT-operand form was already rejected pre-fix by a later generic
// backstop; these swapped forms crashed / silently degraded pre-fix.)

BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929, int_plus_iface_swap) {
    expectInterfaceOperatorUnreachable(
        kIfaceDecls, "var b: IFoo := new Box(); var d := 1 + b; return 0;",
        "+", "IFoo");
}

BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929, int_minus_iface_swap) {
    expectInterfaceOperatorUnreachable(
        kIfaceDecls, "var b: IFoo := new Box(); var d := 1 - b; return 0;",
        "-", "IFoo");
}

BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929, int_plus_choice_swap) {
    expectArithmeticRejected(
        kChoiceDecls,
        "var s: Shape := Shape.Circle(1); var d := 1 + s; return 0;", "+",
        "Shape");
}

// The bitwise family swapped form: pre-fix this did NOT crash at all — it
// hit the #2434 lower-to-zero backstop and compiled to a zero-initialized
// value with exit 0, no diagnostic. This is the row a bare `!ran`/exit-code
// check would have missed entirely.
BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929,
           int_shl_iface_swap_was_silent_wrong_value) {
    expectInterfaceOperatorUnreachable(
        kIfaceDecls, "var b: IFoo := new Box(); var d := 1 << b; return 0;",
        "<<", "IFoo");
}

BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929,
           int_band_choice_swap_was_silent_wrong_value) {
    expectArithmeticRejected(
        kChoiceDecls,
        "var s: Shape := Shape.Circle(1); var d := 1 & s; return 0;", "&",
        "Shape");
}

// ── MUST-PASS controls ─────────────────────────────────────────────────────

// `=`/`<>` on an interface operand are UNAFFECTED — §8.4 gives every type
// `=`/`<>` via `Equals`, and this fix's gate is scoped to relational and
// arithmetic/bitwise operators only.
BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929,
           iface_equals_still_works) {
    auto r = runAot(std::string("module M; ") + kIfaceDecls +
                     "class C public static method Main(): int "
                     "var b: IFoo := new Box(); "
                     "var b2: IFoo := b; "
                     "var d := b = b2; "
                     "return (if d then 1 else 0); "
                     "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// A value-like choice's auto-generated `=` is UNAFFECTED.
BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929,
           choice_equals_still_works) {
    auto r = runAot(std::string("module M; ") + kChoiceDecls +
                     "class C public static method Main(): int "
                     "var s: Shape := Shape.Circle(1); "
                     "var t: Shape := Shape.Circle(1); "
                     "var d := s = t; "
                     "return (if d then 1 else 0); "
                     "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// An enum's `<` (ordering over the underlying integer, §10.3) is UNAFFECTED
// — enums are deliberately excluded from this widening (they take a
// separate, correct code path further down `typeBinary`).
BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929, enum_lt_still_works) {
    auto r = runAot(
        "module M; public enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "var a := Color.Red; "
        "var b := Color.Blue; "
        "var d := a < b; "
        "return (if d then 1 else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// A choice that DOES declare a complete comparison group is UNAFFECTED —
// `findBinaryOperator` finds the user overload and this fix's gate is never
// reached (it only fires when no matching declaration exists).
BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929,
           choice_with_declared_comparison_group_still_works) {
    auto r = runAot(
        "module M; "
        "public choice Shape Circle(radius: int), Square(side: int); "
        "  public static method operator <(a: Shape, b: Shape): bool => "
        "true; "
        "  public static method operator >(a: Shape, b: Shape): bool => "
        "false; "
        "  public static method operator <=(a: Shape, b: Shape): bool => "
        "true; "
        "  public static method operator >=(a: Shape, b: Shape): bool => "
        "false; "
        "end Shape; "
        "class C public static method Main(): int "
        "var s := Shape.Circle(1); "
        "var t := Shape.Square(2); "
        "var d := s < t; "
        "return (if d then 1 else 0); "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 1);
}

// A record with a declared `+` overload is UNAFFECTED — the widening only
// touches the "no applicable overload was found" fallback.
BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929,
           record_with_declared_plus_still_works) {
    auto r = runAot(
        "module M; "
        "public record Vec(X: int, Y: int) "
        "  public static method operator +(a: Vec, b: Vec): Vec => "
        "new Vec(a.X + b.X, a.Y + b.Y); "
        "end Vec; "
        "class C public static method Main(): int "
        "var v := new Vec(1, 2) + new Vec(3, 4); "
        "return v.X + v.Y; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 10);
}

// Ordinary numeric arithmetic is unaffected.
BEZEL_TEST(binary_op_iface_choice_operand_rejected_3929,
           int_plus_int_still_works) {
    auto r = runAot(
        "module M; class C public static method Main(): int "
        "var a := 3; "
        "var b := 4; "
        "return a + b; "
        "end Main; end C;");
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(r.exitCode, 7);
}
