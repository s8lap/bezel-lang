// Spec-derived tests for §17.6 (v0.300, issue #4107): `++`/`--` are
// admitted on exactly the operand types §6.6's compound form admits for
// `+= 1` — an integer type, a floating-point type, `decimal`, or `char` —
// plus §7.4-narrowed nullable forms of those. An `enum`-typed operand is
// none of these (it is its own distinct type, §10.3), so `x++` on a bare
// enum with no declared overload is a compile error per §17.6.
//
// Pre-fix (#4107): the IncrementStmt handler's
// `isRefType`/`isValueTypeRecord` classification never matched
// TypeDeclKind::Enum, so bare `enum++` was silently accepted and the value
// was actually mutated at runtime (the LLVM representation of an enum is its
// underlying integer type, so the "built-in numeric increment/decrement"
// codegen branch produced correct IR).
//
// The fix lives in ExprTypecheck.cpp's IncrementStmt handler: an `isEnumType`
// flag (parallel to `isValueTypeRecord`) is added and included in the
// unconditional-reject branch that already handles reference types and
// records. An enum-typed `x++` is now rejected with a diagnostic naming
// §17.6 and §25.1.
//
// Tests:
//   REJECT shapes (§17.6 violation — was silently accepted + mutated pre-fix):
//    1. `c++` on an enum local
//    2. `c--` on an enum local
//    3. `c.P++` on an enum-typed computed property
//   ACCEPT shapes (no false reject — a regression here would break enums):
//    4. `i++` on an int local that holds an enum's underlying value

#include "support/sema_support.hpp"
#include "test_harness.hpp"

#include "sema/ExprTypecheck.hpp"
#include "sema/TypeResolution.hpp"

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// True iff some reported diagnostic's message contains `needle`.
bool hasMsg(const bezel::test::SemaResult& r, const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

} // namespace

// =========================================================================
// §17.6 / #4107 — REJECTION SHAPES (pre-fix: silent accept + runtime mutation)
// =========================================================================

// §17.6 / #4107 primary repro: `c++` on an enum local. Pre-fix: silently
// accepted, and the value was actually mutated at runtime (Color.Red ->
// Color.Green). Post-fix: clean sema REJECT citing §17.6 and §25.1.
BEZEL_TEST(bare_enum_increment_rejected_4107,
           enum_local_increment_rejected_spec_17_6_issue_4107) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color "
        "  Red, Green, Blue; "
        "end Color; "
        "class C "
        "  public static method Main(): int "
        "    var c := Color.Red; "
        "    c++; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "`++` is not defined on enum type `Color`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§17.6"));
}

// §17.6 / #4107: `c--` on an enum local — the `--` twin.
BEZEL_TEST(bare_enum_increment_rejected_4107,
           enum_local_decrement_rejected_spec_17_6_issue_4107) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color "
        "  Red, Green, Blue; "
        "end Color; "
        "class C "
        "  public static method Main(): int "
        "    var c := Color.Green; "
        "    c--; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "`--` is not defined on enum type `Color`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§17.6"));
}

// §17.6 / #4107: `c.P++` on an enum-typed computed property.
BEZEL_TEST(bare_enum_increment_rejected_4107,
           enum_property_increment_rejected_spec_17_6_issue_4107) {
    auto r = parseSemaAll(
        "module M; "
        "enum Color "
        "  Red, Green, Blue; "
        "end Color; "
        "class C "
        "  public _c: Color := Color.Red; "
        "  public property P: Color "
        "    get => _c; "
        "    set => do _c := value; end; "
        "  end; "
        "  public static method Main(): int "
        "    var c := new C(); "
        "    c.P++; "
        "    return 0; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(hasMsg(r, "`++` is not defined on enum type `Color`"));
    BEZEL_EXPECT_TRUE(hasMsg(r, "§17.6"));
}

// =========================================================================
// §17.6 / §6.1 — ACCEPT SHAPES (no false reject)
// =========================================================================

// §17.6 / §6.1: `i++` on an int local — an enum's underlying value is a
// plain integer and MUST still admit `++`. This is NOT the same as incrementing
// the enum itself.
BEZEL_TEST(bare_enum_increment_rejected_4107,
           int_underlying_value_increment_accepted_spec_17_6) {
    auto r = parseSemaAll(
        "module M; class C "
        "  public static method Main(): int "
        "    var i := 5; "
        "    i++; "
        "    return i; "
        "  end Main; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}
