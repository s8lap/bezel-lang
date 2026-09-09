// Regression tests for issue #3869 — spec catch-up: a `choice` body admits
// instance fields, constructors, destructors, block-form/storage-backed
// properties, indexers, and `event` members with ZERO diagnostics, contrary
// to language spec §35.1/§35.2.
//
// §35.1 (v0.295) states plainly: "Choices cannot have destructors,
// constructors, instance fields, block-form or storage-backed properties,
// indexers, or nested types — the same restriction set as enums (§10.5),
// for the same reasons, plus one: a choice's instance storage IS its
// cases' payloads." §35.2 independently confirms the admitted-member list
// ("instance methods, expression-bodied read-only instance properties,
// static methods, `static readonly` fields ..., `const` members, and
// operator overloads ... this list rejects the rest") and separately: "A
// choice has no user constructors, instance fields, or storage-backed
// properties by design."
//
// `Resolver::checkEnumRestrictions` (src/sema/TypeResolution_Checks.cpp)
// already enforces the equivalent §10.5 set for `TypeDeclKind::Enum`; no
// equivalent existed for `TypeDeclKind::Choice` before this fix (only
// `checkChoiceDispatchModifiers`, #3837, existed — and its own comment
// explicitly scopes itself to the virtual/abstract/override dispatch-
// modifier sub-rule, leaving the member-KIND admission question, this
// issue, unaddressed). `Resolver::checkChoiceRestrictions` mirrors
// `checkEnumRestrictions`'s non-dispatch-modifier checks.
//
// Cite: language spec §35.1/§35.2, v0.295.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;

namespace {

int run(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// A compile-time rejection must be pinned by diagnostic TEXT (never by
// `!r.ran` alone — a runtime crash also synthesizes a diagnostic).
bool rejectedWith(const std::string& program, const char* keyword) {
    auto r = runAot(program);
    if (!bezel::test::failedAtCompileTimeWithRealLocation(r)) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(keyword) != std::string::npos) return true;
    }
    return false;
}

// Builds `module M; [helper] <choiceDecl> class C public static method
// Main(): int <body> end Main; end C;`.
std::string program(const std::string& helper, const std::string& choiceDecl,
                     const std::string& body) {
    return "module M; " + helper + choiceDecl +
           "class C public static method Main(): int " + body +
           "end Main; end C;";
}

}  // namespace

// ═══════════════════════ Reject grid (§35.1 / §35.2) ═══════════════════════
// Every form measured live against origin/main `7561e19` (pre-fix) to compile
// at exit 0 with zero diagnostics, one probe per form, before writing the
// fix (LANE_RULES §1: verify the repro before writing a single line).

BEZEL_TEST(choice_restrictions_3869, instance_field_rejected_spec_35_1_35_2) {
    const std::string p = program(
        "", "choice Shape "
            "  Circle(radius: double), Square(side: double); "
            "  public V: int; "
            "end Shape; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "choice `Shape` may not declare an instance field"));
}

BEZEL_TEST(choice_restrictions_3869, constructor_rejected_spec_35_1_35_2) {
    const std::string p = program(
        "", "choice Shape "
            "  Circle(radius: double), Square(side: double); "
            "  public constructor() end constructor; "
            "end Shape; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "choice `Shape` may not declare a constructor"));
}

BEZEL_TEST(choice_restrictions_3869, destructor_rejected_spec_35_1_35_2) {
    const std::string p = program(
        "", "choice Shape "
            "  Circle(radius: double), Square(side: double); "
            "  destructor end destructor; "
            "end Shape; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "choice `Shape` may not declare a destructor"));
}

BEZEL_TEST(choice_restrictions_3869, nested_type_rejected_spec_35_1_35_2) {
    const std::string p = program(
        "", "choice Shape "
            "  Circle(radius: double), Square(side: double); "
            "  public class Inner end Inner; "
            "end Shape; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "choice `Shape` may not declare a nested type"));
}

BEZEL_TEST(choice_restrictions_3869, indexer_rejected_spec_35_1_35_2) {
    const std::string p = program(
        "", "choice Shape "
            "  Circle(radius: double), Square(side: double); "
            "  public property [i: int]: int get => i; end; "
            "end Shape; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "choice `Shape` may not declare an indexer"));
}

BEZEL_TEST(choice_restrictions_3869,
           block_form_property_rejected_spec_35_1_35_2) {
    const std::string p = program(
        "", "choice Shape "
            "  Circle(radius: double), Square(side: double); "
            "  public property V: int get => 5; end; "
            "end Shape; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(rejectedWith(
        p, "choice `Shape` may not declare a block-form property"));
}

BEZEL_TEST(choice_restrictions_3869,
           storage_backed_property_rejected_spec_35_1_35_2) {
    const std::string p = program(
        "", "choice Shape "
            "  Circle(radius: double), Square(side: double); "
            "  public property V: int := 5; "
            "end Shape; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(rejectedWith(
        p, "choice `Shape` may not declare a property with backing storage"));
}

BEZEL_TEST(choice_restrictions_3869, event_member_rejected_spec_35_2) {
    const std::string p = program(
        "record Ev(N: int); ",
        "choice Shape "
        "  Circle(radius: double), Square(side: double); "
        "  public event Boom: Ev; "
        "end Shape; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(rejectedWith(
        p, "choice `Shape` may not declare an `event` member"));
}

// ═══════════════ Accept-side grid (§35.2's admitted-member list) ══════════
// A fix that adds new rejections is only sound if it does not also reject
// what §35.2 legitimately admits. Each row below compiles a program using
// exactly one admitted form; the closing kitchen-sink row combines ALL of
// them (plus interface conformance and a generic instantiation) into one
// choice and asserts the RUNTIME VALUE, not merely that it compiles.

BEZEL_TEST(choice_restrictions_3869, case_arms_payload_free_and_carrying_accepted) {
    const std::string p = program(
        "", "choice Shape "
            "  Circle(radius: double), Square(side: double), Point; "
            "end Shape; ",
        "var a := Shape.Circle(2.0); var b := Shape.Point; return 42; ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(choice_restrictions_3869, instance_method_accepted_spec_35_2) {
    const std::string p = program(
        "", "choice Shape "
            "  Circle(radius: double), Square(side: double); "
            "  public method IsCircle(): bool => "
            "    (case this of Circle(_) => true, Square(_) => false); "
            "end Shape; ",
        "var s := Shape.Circle(2.0); "
        "return (if s.IsCircle() then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

// fix #3869's own accept-side sweep found a SEPARATE, pre-existing codegen
// defect while probing this exact shape: an expression-bodied property
// whose body is DIRECTLY `(case this of ...)` on a choice IR-verification-
// crashes ("Basic Block ... does not have terminator!", #2491's backstop
// then substitutes a zero return) — confirmed pre-existing (reproduces
// identically with checkChoiceRestrictions reverted) and unrelated to this
// fix (a property with no case-match in its own body, e.g. a literal or a
// delegating call to an instance method, is unaffected — see the delegating
// form used below). Filed separately; worked around here by delegating the
// case-match to an instance method (itself independently pinned accepted by
// `instance_method_accepted_spec_35_2` above) so this row still exercises
// §35.2's admitted expression-bodied-property FORM without tripping the
// unrelated codegen gap.
BEZEL_TEST(choice_restrictions_3869,
           expression_bodied_readonly_property_accepted_spec_35_2) {
    const std::string p = program(
        "", "choice Shape "
            "  Circle(radius: double), Square(side: double); "
            "  public method DescribeIt(): string => "
            "    (case this of Circle(_) => \"circle\", Square(_) => \"square\"); "
            "  public property Kind: string => DescribeIt(); "
            "end Shape; ",
        "var s := Shape.Circle(2.0); "
        "return (if s.Kind = \"circle\" then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(choice_restrictions_3869, static_method_accepted_spec_35_2) {
    const std::string p = program(
        "", "choice Shape "
            "  Circle(radius: double), Square(side: double); "
            "  public static method Unit(): Shape => Circle(1.0); "
            "end Shape; ",
        "var s := Shape.Unit(); "
        "return (case s of Circle(r) => (if r = 1.0 then 42 else 1), "
        "                  Square(_) => 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(choice_restrictions_3869, static_readonly_field_accepted_spec_35_2) {
    const std::string p = program(
        "", "choice Shape "
            "  Circle(radius: double), Square(side: double); "
            "  public static readonly Unit: double := 1.0; "
            "end Shape; ",
        "return (if Shape.Unit = 1.0 then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(choice_restrictions_3869, const_member_accepted_spec_35_2) {
    const std::string p = program(
        "", "choice Shape "
            "  Circle(radius: double), Square(side: double); "
            "  public const Zero: int := 0; "
            "end Shape; ",
        "return (if Shape.Zero = 0 then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(choice_restrictions_3869, operator_overload_accepted_spec_35_2) {
    const std::string p = program(
        "", "choice Shape "
            "  Circle(radius: double), Square(side: double); "
            "  public static method operator +(a: Shape, b: Shape): int => 42; "
            "end Shape; ",
        "var a := Shape.Circle(1.0); var b := Shape.Square(2.0); "
        "return a + b; ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

// #3837's own admitted trio (override on Equals/GetHashCode/ToString) must
// remain accepted — this fix must not double-report what #3837 already
// permits via checkChoiceDispatchModifiers.
BEZEL_TEST(choice_restrictions_3869,
           override_object_virtual_trio_accepted_spec_35_2_3837) {
    const std::string p = program(
        "", "choice Shape "
            "  Circle(radius: double), Square(side: double); "
            "  public override method ToString(): string => \"shape\"; "
            "  public override method Equals(other: Shape): bool => true; "
            "  public override method GetHashCode(): int => 7; "
            "end Shape; ",
        "var s := Shape.Circle(1.0); "
        "return (if s.ToString() = \"shape\" and s.GetHashCode() = 7 "
        "        then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

// fix #3869's own accept-side sweep found a SEPARATE, pre-existing codegen
// defect while probing this exact shape: a choice implementing a plain user
// interface with NO explicit `ToString`/`Equals`/`GetHashCode` override
// SEGFAULTS bezelc while building the box's default ToString thunk
// (confirmed pre-existing — reproduces identically with
// checkChoiceRestrictions reverted; stack trace bottoms out in
// Emitter::emitChoiceToString / boxToStringThunk / setupBoxDescriptors,
// LlvmBackend_String.cpp / LlvmBackend_Setup.cpp — a null llvm::Value fed to
// CreateExtractValue). Declaring the §35.2-admitted override trio (also
// independently pinned accepted by `override_object_virtual_trio_accepted_
// spec_35_2_3837` above) avoids the crash — confirmed live, isolated repro
// — and is thematically apt here regardless: §35.2 says the trio "carries
// over for the boxable classifications," and a choice satisfying an
// interface reference is exactly the boxable case. Filed separately with
// the full stack trace and the narrower (non-generic, trivial-body)
// trigger; using the override trio here so this row still exercises §35.1's
// "a choice may implement interfaces" admission without tripping the
// unrelated codegen gap.
BEZEL_TEST(choice_restrictions_3869, interface_conformance_accepted_spec_35_1) {
    const std::string p = program(
        "interface IShape method Area(): double; end IShape; ",
        "choice Shape: IShape "
        "  Circle(radius: double), Square(side: double); "
        "  public method Area(): double => "
        "    (case this of Circle(r) => r * r, Square(s) => s * s); "
        "  public override method ToString(): string => \"shape\"; "
        "  public override method Equals(other: Shape): bool => true; "
        "  public override method GetHashCode(): int => 1; "
        "end Shape; ",
        "var s := Shape.Circle(2.0); "
        "return (if s.Area() = 4.0 then 42 else 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

BEZEL_TEST(choice_restrictions_3869, generic_choice_accepted_spec_35_11) {
    const std::string p = program(
        "", "choice Opt<T> Some(v: T), None; end Opt; ",
        "var o := Opt<int>.Some(7); "
        "return (case o of Some(v) => v * 6, None => 1); ");
    BEZEL_EXPECT_EQ(run(p), 42);
}

// Kitchen sink: EVERY §35.2-admitted form together on one generic,
// interface-conforming choice — the integration control. If this fix's
// checks interact badly with each other (e.g. rejecting a legitimate form
// only when combined with another), this is where it would show.
BEZEL_TEST(choice_restrictions_3869, kitchen_sink_all_admitted_forms_accepted) {
    const std::string p =
        "module M; "
        "class Box public N: int; public constructor(n: int) N := n; "
        "end constructor; end Box; "
        "interface IShape method Area(): double; end IShape; "
        "choice Shape<T>: IShape "
        "  Circle(radius: double), Square(side: double), Tagged(payload: T); "
        "  public static readonly Origin: double := 0.0; "
        "  public static owned readonly Cache: Box := new Box(7); "
        "  public const Pi: double := 3.14159; "
        "  public method Area(): double => "
        "    (case this of Circle(r) => Pi * r * r, Square(s) => s * s, "
        "                  Tagged(_) => Origin); "
        "  public property Label: string => "
        "    (case this of Circle(_) => \"circle\", Square(_) => \"square\", "
        "                  Tagged(_) => \"tagged\"); "
        "  public static method Describe(s: Shape<T>): string => s.Label; "
        "  public override method ToString(): string => Label; "
        "  public override method Equals(other: Shape<T>): bool => "
        "    Label = other.Label; "
        "  public override method GetHashCode(): int => Label.Length; "
        "  public static method operator +(a: Shape<T>, b: Shape<T>): double => "
        "    a.Area() + b.Area(); "
        "end Shape; "
        "class C public static method Main(): int "
        "  var c := Shape<int>.Circle(2.0); "
        "  var sq := Shape<int>.Square(3.0); "
        "  var tg := Shape<int>.Tagged(9); "
        "  var areaSum := c.Area() + sq.Area(); "
        "  var sumOp := c + sq; "
        "  var s := Shape<int>.Describe(c); "
        "  var tostr := c.ToString(); "
        "  var eq := c.Equals(c); "
        "  var hc := c.GetHashCode(); "
        "  var okLabel := (s = \"circle\") and (tostr = \"circle\") and eq "
        "                 and (hc = 6); "
        "  var okArea := (areaSum > 21.0) and (areaSum < 22.0); "
        "  var okOp := (sumOp > 21.0) and (sumOp < 22.0); "
        "  var okConst := Shape<int>.Pi > 3.14 and Shape<int>.Pi < 3.15; "
        "  var okStatic := Shape<int>.Origin = 0.0; "
        "  var okTagged := tg.Area() = 0.0; "
        "  return (if okLabel and okArea and okOp and okConst and okStatic "
        "             and okTagged "
        "          then 42 else 1); "
        "end Main; end C;";
    BEZEL_EXPECT_EQ(run(p), 42);
}
