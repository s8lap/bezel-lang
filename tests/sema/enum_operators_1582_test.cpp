// Spec-derived sema tests for issue #1582 — enum operator overloads
// (§10.4, §25/§25.2, grammar Appendix A.3/A.4; v0.183, #1381).
//
// §10.4: "An enum may declare ... operator overloads (§25; grammar A.3 —
// v0.183, #1381)." Appendix A.3's `EnumMember ::= MethodDecl |
// ExpressionPropertyDecl | StaticReadonlyDecl | StaticConstDecl |
// OperatorDecl` (v0.183) admits `OperatorDecl` as an enum member, and
// `OperatorDecl` is `public static` only, exactly as on records/classes.
//
// §25.2 restrictions apply uniformly across record/enum/class declaring
// types: at least one operand must be the declaring type; comparison
// operators (`<`,`>`,`<=`,`>=`) must be a complete group; `=`/`<>` may be
// overloaded on records and enums (not classes). This file checks that an
// enum's `operator` member is admitted (not rejected by the §10.5 member
// filter) and that the §25.2 restrictions reach enum declarations exactly as
// they reach record declarations. Runtime dispatch/codegen correctness is
// covered separately in tests/codegen/enum_operators_1582_test.cpp.

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

namespace {

bezel::test::SemaResult parseSema(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool anyMessageMentions(const bezel::test::SemaResult& r,
                        const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

// §10.4/A.3 (v0.183): a well-formed binary `operator +` inside an enum body
// is admitted — no "may not declare" (§10.5) diagnostic. Before #1582 this
// is the case the issue calls out as the sema member filter's gap.
BEZEL_TEST(sema_enum_operators_1582, binary_operator_admitted_no_error_spec_10_4) {
    auto r = parseSema(
        "module M; "
        "enum Priority Low, Medium, High; "
        "  public static method operator +(a: Priority, b: int): Priority "
        "    => Priority(int(a) + b); "
        "end Priority; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §10.4/A.3: a unary `operator -` inside an enum body is admitted.
BEZEL_TEST(sema_enum_operators_1582, unary_operator_admitted_no_error_spec_10_4) {
    auto r = parseSema(
        "module M; "
        "enum Sign: int Neg := -1, Zero := 0, Pos := 1; "
        "  public static method operator -(a: Sign): Sign => Sign(-int(a)); "
        "end Sign; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §10.4/A.3: a complete comparison group inside an enum body is admitted.
BEZEL_TEST(sema_enum_operators_1582, complete_comparison_group_admitted_spec_10_4) {
    auto r = parseSema(
        "module M; "
        "enum Priority Low, Medium, High; "
        "  public static method operator <(a: Priority, b: Priority): bool => int(a) < int(b); "
        "  public static method operator >(a: Priority, b: Priority): bool => int(a) > int(b); "
        "  public static method operator <=(a: Priority, b: Priority): bool => int(a) <= int(b); "
        "  public static method operator >=(a: Priority, b: Priority): bool => int(a) >= int(b); "
        "end Priority; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §25.2: "At least one parameter of an operator overload must be the
// declaring type." This restriction is stated type-kind-agnostically and
// must reach an enum's own declarations exactly as it reaches a record's.
BEZEL_TEST(sema_enum_operators_1582,
           declaring_type_operand_required_on_enum_spec_25_2) {
    auto r = parseSema(
        "module M; "
        "enum Priority Low, Medium, High; "
        "  public static method operator +(a: int, b: int): int => a + b; "
        "end Priority; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "must be the declaring type"));
}

// §25.2: "Comparison operators must be overloaded as a complete group."
// declaring only `<` on an enum must be rejected exactly as on a record.
BEZEL_TEST(sema_enum_operators_1582,
           incomplete_comparison_group_rejected_on_enum_spec_25_2) {
    auto r = parseSema(
        "module M; "
        "enum Priority Low, Medium, High; "
        "  public static method operator <(a: Priority, b: Priority): bool => int(a) < int(b); "
        "end Priority; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "complete group"));
}

// §25.2: `=`/`<>` ARE overloadable on enums (unlike classes) — a homogeneous
// pair with the required co-declaration (§25.2 v0.158/v0.183) is accepted.
BEZEL_TEST(sema_enum_operators_1582, equality_pair_admitted_on_enum_spec_25_2) {
    auto r = parseSema(
        "module M; "
        "enum Priority Low, Medium, High; "
        "  public static method operator =(a: Priority, b: Priority): bool => int(a) = int(b); "
        "  public static method operator <>(a: Priority, b: Priority): bool => int(a) <> int(b); "
        "  public method Equals(other: Priority): bool => int(this) = int(other); "
        "  public method GetHashCode(): int => int(this); "
        "end Priority; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §10.5 (v0.183): `override` on the co-declared `GetHashCode` is still
// rejected on an enum — enums dispatch statically and never carry an
// override chain. This confirms the §10.5 restriction and the §25.2
// co-declaration rule compose correctly: the required member is the PLAIN
// spelling, and the `override` spelling remains illegal.
BEZEL_TEST(sema_enum_operators_1582,
           override_gethashcode_still_rejected_on_enum_spec_10_5) {
    auto r = parseSema(
        "module M; "
        "enum Priority Low, Medium, High; "
        "  public static method operator =(a: Priority, b: Priority): bool => int(a) = int(b); "
        "  public static method operator <>(a: Priority, b: Priority): bool => int(a) <> int(b); "
        "  public method Equals(other: Priority): bool => int(this) = int(other); "
        "  public override method GetHashCode(): int => int(this); "
        "end Priority; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(
        r, "may not be virtual, abstract, or override"));
}
