// Spec-derived tests for Sema slice S32 — enum member restrictions
// (§10.5): no instance fields, constructors, nested types, indexers;
// no virtual/abstract/override methods. Static members allowed (§10.4).

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/TypeResolution.hpp"
#include "sema/ExprTypecheck.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
    }
    return r;
}

} // namespace

BEZEL_TEST(sema_enum, plain_enum_ok) {
    auto r = parseSemaAll(
        "module M; enum Color Red, Green, Blue; end Color;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_enum, enum_static_method_ok) {
    auto r = parseSemaAll(
        "module M; enum E A, B; "
        "  public static method Count(): int => 2; "
        "end E;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_enum, instance_field_diagnoses) {
    auto r = parseSemaAll(
        "module M; enum E A, B; public _x: int := 0; end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_enum, static_field_ok) {
    auto r = parseSemaAll(
        "module M; enum E A, B; public static _x: int := 0; end E;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_enum, constructor_diagnoses) {
    auto r = parseSemaAll(
        "module M; enum E A, B; public constructor() end constructor; end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_enum, virtual_method_diagnoses) {
    auto r = parseSemaAll(
        "module M; enum E A, B; "
        "  public virtual method M() _ := 1; end M; "
        "end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_enum, instance_method_ok) {
    auto r = parseSemaAll(
        "module M; enum E A, B; "
        "  public method Describe(): int => 1; "
        "end E;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §10.3 (#740): a member value that exceeds the declared underlying type's
// range is a compile error (it previously wrapped silently at codegen).
BEZEL_TEST(sema_enum, byte_member_over_255_rejected) {
    auto r = parseSemaAll("module M; enum E: byte A := 256; end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §10.3: an unsigned underlying type rejects a negative value.
BEZEL_TEST(sema_enum, byte_member_negative_rejected) {
    auto r = parseSemaAll("module M; enum E: byte A := -1; end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §10.3: a value that overflows a `short` (but fits `int`) is rejected.
BEZEL_TEST(sema_enum, short_member_over_range_rejected) {
    auto r = parseSemaAll("module M; enum E: short A := 40000; end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// §10.3 regression guard: in-range values, an unsigned boundary, a signed
// minimum, and AUTO-INCREMENT continuation are all accepted.
BEZEL_TEST(sema_enum, in_range_and_auto_increment_ok) {
    auto r = parseSemaAll(
        "module M; "
        "enum B: byte Lo := 0, Mid := 200, Hi := 255; end B; "
        "enum S: short A := -32768, Z := 32767; end S; "
        "enum Inc: byte X := 253, Y, Z; end Inc;");  // 253, 254, 255 — all fit
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// §10.3: auto-increment that overflows the underlying type is rejected
// (`byte X := 255, Y;` → Y is 256, out of range).
BEZEL_TEST(sema_enum, auto_increment_overflow_rejected) {
    auto r = parseSemaAll(
        "module M; enum E: byte X := 255, Y; end E;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ── §10.4 / Appendix A.4 (v0.181, issue #1564): enum static owned surface ──

// §10.4: "A static readonly field of class type may be marked `owned` …
// enrolling it in §22.13's program-exit disposal." Before v0.181 the enum
// grammar's StaticReadonlyDecl production had no `owned` slot; A.4 now
// carries it. The parser already accepted `owned` generically (parseMember's
// modifier loop is shared by class and enum bodies) and checkEnumRestrictions
// never special-cased `owned`, so this was already accepted on HEAD — this
// test locks the acceptance in as a spec-derived regression guard.
BEZEL_TEST(sema_enum, static_owned_readonly_field_ok_1564) {
    auto r = parseSemaAll(
        "module M; "
        "class Held end Held; "
        "enum Kind A, B; "
        "  public static owned readonly Cache: Held := new Held(); "
        "end Kind;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ── §10.4 / §22.12 (issue #1564): enum `const` follows class-level rules ──
//
// §10.4: "A `const` member (grammar A.4 `StaticConstDecl`) follows §22.12's
// class-level `const` rules: implicitly static, compile-time-constant
// initializer, primitive or `string` type." checkConstTypes (Slice S30)
// iterates every TypeSymbol in the module uninformed by TypeDeclKind, so it
// already applies uniformly to enums — these tests are the conformance
// check §1564 asked for; they confirm the existing behavior rather than
// changing it.

// A primitive-typed enum const is accepted, exactly as on a class.
BEZEL_TEST(sema_enum, const_member_primitive_ok_1564_22_12) {
    auto r = parseSemaAll(
        "module M; enum Kind A, B; "
        "  public const Limit: int := 42; "
        "end Kind;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A `string`-typed enum const is accepted.
BEZEL_TEST(sema_enum, const_member_string_ok_1564_22_12) {
    auto r = parseSemaAll(
        "module M; enum Kind A, B; "
        "  public const Label: string := \"k\"; "
        "end Kind;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A class-typed enum const is rejected — §22.12's "primitive or `string`"
// restriction, same as on a class.
BEZEL_TEST(sema_enum, const_member_non_primitive_rejected_1564_22_12) {
    auto r = parseSemaAll(
        "module M; "
        "class Widget end Widget; "
        "enum Kind A, B; "
        "  public const Bad: Widget := new Widget(); "
        "end Kind;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// ── #1137 / §8.4 / §10: enum comparison operand type-checking ─────────────────
// An enum may be compared (=/<>/</<=/>/>=) only with a value of the SAME enum;
// the operands are reduced to their underlying integers, so without an operand
// type-check `enum = int` / `enumA = enumB` silently compiled as an int compare.

namespace {
// Full sema pipeline (resolveTypes + expression typechecking) — the enum
// comparison check lives in the expression-typecheck pass, which parseSemaAll's
// resolve-only path does not run.
bezel::test::SemaResult parseSemaFull(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}
}  // namespace

// enum compared with an int literal is rejected (#1137).
BEZEL_TEST(sema_enum, enum_vs_int_comparison_rejected_1137) {
    auto r = parseSemaFull(
        "module M; enum Color Red, Green, Blue; end Color; "
        "class C public static method Main(): int "
        "  if Color.Green = 1 then return 1; end; return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// enum compared with a DIFFERENT enum is rejected (#1137).
BEZEL_TEST(sema_enum, enum_vs_different_enum_comparison_rejected_1137) {
    auto r = parseSemaFull(
        "module M; enum Color Red, Green; end Color; enum Size Small, Big; end Size; "
        "class C public static method Main(): int "
        "  if Color.Red = Size.Small then return 1; end; return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Relational enum-vs-int is also rejected (#1137).
BEZEL_TEST(sema_enum, enum_vs_int_relational_rejected_1137) {
    auto r = parseSemaFull(
        "module M; enum Color Red, Green; end Color; "
        "class C public static method Main(): int "
        "  if Color.Red < 1 then return 1; end; return 0; end Main; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
}

// Same-enum comparison stays accepted (#1137 must not over-reject).
BEZEL_TEST(sema_enum, same_enum_comparison_accepted_1137) {
    auto r = parseSemaFull(
        "module M; enum Color Red, Green; end Color; "
        "class C public static method Main(): int "
        "  var c: Color := Color.Green; "
        "  if c = Color.Green then return 1; end; return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// `this = E.X` inside an enum method stays accepted (`this` is the same enum).
BEZEL_TEST(sema_enum, enum_method_this_comparison_accepted_1137) {
    auto r = parseSemaFull(
        "module M; enum Status Ok, Error; "
        "  public method IsOk(): bool => this = Status.Ok; end Status; "
        "class C public static method Main(): int return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// A nullable enum compared with `null` stays accepted (§8.4 null-safe equality).
BEZEL_TEST(sema_enum, nullable_enum_vs_null_accepted_1137) {
    auto r = parseSemaFull(
        "module M; enum Color Red, Green; end Color; "
        "class C public static method Main(): int "
        "  var c: Color? := null; "
        "  if c = null then return 1; end; return 0; end Main; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
