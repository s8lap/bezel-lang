// Regression tests for issue #5633 — test gap for fix #5610.
//
// b530bf05 narrowed `isObjectVirtualReplacementMember`'s `Equals` arm in
// `src/sema/TypeResolution_Checks.cpp` from bare arity-1 to a parameter-type
// test, so an `override` member on an enum or choice now requires the
// parameter to be the enum/choice's own type or nullable `Object`.
//
// Spec §8.1 declares `Object`'s own member as:
//   public virtual method Equals(other: Object?): bool;
// The non-nullable spelling `Equals(Object)` is NOT the Object slot — it is an
// overload with different semantics — and is not admitted as an override target
// on value kinds (§10.5 / §35.2).
//
// The three shapes, measured pre-fix → post-fix:
//
//   shape                              | pre-fix | post-fix
//   -----------------------------------|---------|---------
//   override Equals(other: Object)      | accept  | REJECT
//   override Equals(other: Object?)    | accept  | REJECT (fix #5924 item 1, v0.338)
//   override Equals(other: Color/E)    | accept  | accept
//
// The own-typed row is the control proving the fix is narrow rather than
// "reject everything arity-1". All rows exist for both enum (§10.5) and
// choice (§35.2). fix #5924 item 1 (§9.2 / §10.5 / §35.2, v0.338) makes an
// Object-spelled `Equals` on every value kind a declaration-time reject, the
// diagnostic naming the own-typed spelling; the rows at the end of this file
// pin the plain spelling and the record kind.
//
// GetHashCode is co-declared in every row because spec §8.3 requires it
// alongside any override Equals.

#include "support/aot_result_asserts.hpp"  // failedAtCompileTimeWithRealLocation
#include "support/aot_support.hpp"         // runAot
#include "test_harness.hpp"

#include <string>

using bezel::test::runAot;
using bezel::test::runAotOut;

namespace {

// Compile-time rejection pinned by specific diagnostic text (never by `!r.ran`
// alone — a runtime crash also synthesizes a diagnostic).
bool rejectedWith(const std::string& program, const char* needle) {
    auto r = runAot(program);
    if (!bezel::test::failedAtCompileTimeWithRealLocation(r)) return false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.severity != bezel::Severity::Error) continue;
        if (d.message.find(needle) != std::string::npos) return true;
    }
    return false;
}

int runOk(const std::string& program) {
    auto r = runAot(program);
    return r.ran ? r.exitCode : -999;
}

// Builds a minimal module wrapping an enum or choice declaration followed by
// a class with a Main that runs the given body.
std::string program(const std::string& typeDecl, const std::string& body) {
    return "module M; " + typeDecl +
           "class C public static method Main(): int " + body +
           "end Main; end C;";
}

}  // namespace

// ═══════════════════════ Enum (§10.5) ═══════════════════════════════════════

// REJECT — pre-fix: accepted (exit 0); post-fix: rejected.
// `override Equals(Object)` (non-nullable Object parameter) is not the
// Object slot; spec §8.1 names only `Equals(Object?)`. fix #5924 item 1
// (§10.5, v0.338): an Object-spelled Equals is rejected on an enum at the
// declaration, naming the own-typed spelling.
BEZEL_TEST(override_equals_parameter_type_5610,
           enum_override_equals_non_nullable_object_rejected_spec_10_5) {
    const std::string p = program(
        "enum Color Red, Green, Blue; "
        "  public override method Equals(other: Object): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end Color; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "enum method `Equals` may not take `Object`"));
}

// REJECT — fix #5924 item 1 (§10.5, v0.338): `override Equals(Object?)` is not
// the Object slot on a value kind; the enum's own-typed `Equals(other: Color)`
// fills it, so the nullable-Object spelling is rejected at the declaration,
// naming the own-typed spelling.
BEZEL_TEST(override_equals_parameter_type_5610,
           enum_override_equals_nullable_object_rejected_spec_10_5) {
    const std::string p = program(
        "enum Color Red, Green, Blue; "
        "  public override method Equals(other: Object?): bool => other <> null; "
        "  public override method GetHashCode(): int => 0; "
        "end Color; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "enum method `Equals` may not take `Object?`"));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "Equals(other: Color)"));
}

// ACCEPT — `override Equals(Color)` (own-type parameter) is §10.5's admitted
// `Equals(E)` replacement.
BEZEL_TEST(override_equals_parameter_type_5610,
           enum_override_equals_own_type_accepted_spec_10_5) {
    const std::string p = program(
        "enum Color Red, Green, Blue; "
        "  public override method Equals(other: Color): bool => this = other; "
        "  public override method GetHashCode(): int => 0; "
        "end Color; ",
        "var c := Color.Red; "
        "return (if c.Equals(Color.Red) then 42 else 1); ");
    BEZEL_EXPECT_EQ(runOk(p), 42);
}

// ═══════════════════════ Choice (§35.2) ════════════════════════════════════

// REJECT — pre-fix: accepted (exit 0); post-fix: rejected.
// Same logic as the enum case: non-nullable Object is not the Object slot.
BEZEL_TEST(override_equals_parameter_type_5610,
           choice_override_equals_non_nullable_object_rejected_spec_35_2) {
    const std::string p = program(
        "choice Color Red(payload: int), Green, Blue(msg: string); "
        "  public override method Equals(other: Object): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end Color; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "choice method `Equals` may not take `Object`"));
}

// REJECT — fix #5924 item 1 (§35.2, v0.338): `override Equals(Object?)` is
// rejected on a choice at the declaration, as on an enum; the own-typed
// `Equals(other: Color)` fills the `Object` slot.
BEZEL_TEST(override_equals_parameter_type_5610,
           choice_override_equals_nullable_object_rejected_spec_35_2) {
    const std::string p = program(
        "choice Color Red(payload: int), Green, Blue(msg: string); "
        "  public override method Equals(other: Object?): bool => other <> null; "
        "  public override method GetHashCode(): int => 0; "
        "end Color; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "choice method `Equals` may not take `Object?`"));
    BEZEL_EXPECT_TRUE(rejectedWith(p, "Equals(other: Color)"));
}

// ACCEPT — `override Equals(Color)` (own-type parameter) is §35.2's admitted
// `Equals(E)` replacement, the same rule §35.2 inherits from §10.5.
BEZEL_TEST(override_equals_parameter_type_5610,
           choice_override_equals_own_type_accepted_spec_35_2) {
    const std::string p = program(
        "choice Color Red(payload: int), Green, Blue(msg: string); "
        "  public override method Equals(other: Color): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end Color; ",
        // Just verify it compiles and runs; the override is accepted.
        "return 42; ");
    BEZEL_EXPECT_EQ(runOk(p), 42);
}

// ═══════════ Object-spelled Equals on every value kind (#5924 item 1) ═══════
//
// fix #5924 item 1 (§9.2, §10.5, §35.2, v0.338): "An `Equals` taking `Object?`
// or `Object` is rejected on every value kind at the declaration, `override` or
// plain, the diagnostic naming the own-typed spelling." The own-typed
// `Equals(other: T)` fills the kind's `Object` slot ([§8.3], [§8.6]).

BEZEL_TEST(override_equals_parameter_type_5610,
           enum_equals_object_nullable_plain_rejected_spec_10_5) {
    const std::string p = program(
        "enum Color Red, Green, Blue; "
        "  public method Equals(other: Object?): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end Color; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "enum method `Equals` may not take `Object?`"));
}

BEZEL_TEST(override_equals_parameter_type_5610,
           enum_equals_object_plain_rejected_spec_10_5) {
    const std::string p = program(
        "enum Color Red, Green, Blue; "
        "  public method Equals(other: Object): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end Color; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "enum method `Equals` may not take `Object`"));
}

BEZEL_TEST(override_equals_parameter_type_5610,
           choice_equals_object_nullable_plain_rejected_spec_35_2) {
    const std::string p = program(
        "choice Color Red(payload: int), Green, Blue(msg: string); "
        "  public method Equals(other: Object?): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end Color; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "choice method `Equals` may not take `Object?`"));
}

BEZEL_TEST(override_equals_parameter_type_5610,
           choice_equals_object_plain_rejected_spec_35_2) {
    const std::string p = program(
        "choice Color Red(payload: int), Green, Blue(msg: string); "
        "  public method Equals(other: Object): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end Color; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "choice method `Equals` may not take `Object`"));
}

BEZEL_TEST(override_equals_parameter_type_5610,
           record_equals_object_nullable_override_rejected_spec_9_2) {
    const std::string p = program(
        "record R(X: int) "
        "  public override method Equals(other: Object?): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end R; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "record member `Equals` may not take `Object?`"));
}

BEZEL_TEST(override_equals_parameter_type_5610,
           record_equals_object_nullable_plain_rejected_spec_9_2) {
    const std::string p = program(
        "record R(X: int) "
        "  public method Equals(other: Object?): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end R; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "record member `Equals` may not take `Object?`"));
}

BEZEL_TEST(override_equals_parameter_type_5610,
           record_equals_object_plain_rejected_spec_9_2) {
    const std::string p = program(
        "record R(X: int) "
        "  public method Equals(other: Object): bool => true; "
        "  public override method GetHashCode(): int => 0; "
        "end R; ",
        "return 0; ");
    BEZEL_EXPECT_TRUE(
        rejectedWith(p, "record member `Equals` may not take `Object`"));
}

// ACCEPT control: the own-typed plain `Equals` with `GetHashCode` on an enum
// is the Object slot's member and runs (§10.5 / §8.3); stdout is the observable.
BEZEL_TEST(override_equals_parameter_type_5610,
           enum_equals_own_type_plain_runs_spec_10_5) {
    const std::string src =
        "module M; enum Color Red, Green; "
        "  public method Equals(other: Color): bool => this = other; "
        "  public override method GetHashCode(): int => 1; "
        "end Color; "
        "class C public static method Main(): int "
        "  Console.WriteLine(Color.Red.Equals(Color.Red)); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(runAotOut(src), "True");
}

// ACCEPT control: the choice's own-typed plain `Equals` runs the same way.
BEZEL_TEST(override_equals_parameter_type_5610,
           choice_equals_own_type_plain_runs_spec_35_2) {
    const std::string src =
        "module M; choice Shape Circle(r: int), Square(s: int); "
        "  public method Equals(other: Shape): bool => true; "
        "  public override method GetHashCode(): int => 1; "
        "end Shape; "
        "class C public static method Main(): int "
        "  var s := Shape.Circle(3); "
        "  Console.WriteLine(s.Equals(Shape.Square(4))); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(runAotOut(src), "True");
}

// ACCEPT control: a CLASS keeps its `Object?` slot spelling (§8.3); the fix is
// value-kind only and does not widen to classes.
BEZEL_TEST(override_equals_parameter_type_5610,
           class_equals_object_nullable_override_accepted_spec_8_3) {
    const std::string src =
        "module M; class P public override method Equals(other: Object?): bool "
        "  => other <> null; "
        "  public override method GetHashCode(): int => 1; "
        "end P; "
        "class C public static method Main(): int "
        "  Console.WriteLine((new P()).Equals(new P())); "
        "  return 0; "
        "end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(runAotOut(src), "True");
}

// §9.2 (v0.338) L1228 speaks of "the member that fills the kind's Object
// slot", an instance member; whether a `static` Equals(Object?/Object) on a
// value kind is rejected is open (#6191). Until it is ruled, the static spelling
// keeps its pre-#5924 behaviour: accepted, and it runs. These rows pin that
// (the reject arm of #5924 is instance-only).
BEZEL_TEST(override_equals_parameter_type_5610,
           record_static_equals_object_nullable_not_rejected_pending_6191) {
    const std::string src =
        "module M; "
        "record R public property V: int; public constructor(v: int) V := v; end constructor; "
        "  public static method Equals(o: Object?): bool => false; "
        "end R; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"ok\"); return 0; end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(runAotOut(src), "ok");
}

BEZEL_TEST(override_equals_parameter_type_5610,
           enum_static_equals_object_not_rejected_pending_6191) {
    const std::string src =
        "module M; "
        "enum E A, B; "
        "  public static method Equals(o: Object): bool => false; "
        "end E; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"ok\"); return 0; end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(runAotOut(src), "ok");
}

BEZEL_TEST(override_equals_parameter_type_5610,
           choice_static_equals_object_nullable_not_rejected_pending_6191) {
    const std::string src =
        "module M; "
        "choice K One, Two; "
        "  public static method Equals(o: Object?): bool => false; "
        "end K; "
        "class C public static method Main(): int "
        "  Console.WriteLine(\"ok\"); return 0; end Main; end C;";
    auto r = runAot(src);
    BEZEL_EXPECT_TRUE(r.ran);
    BEZEL_EXPECT_EQ(runAotOut(src), "ok");
}
