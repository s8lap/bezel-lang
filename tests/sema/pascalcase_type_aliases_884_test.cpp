// Spec-derived tests for §3.4, issue #884 — PascalCase System type-name
// aliases resolve to the same primitive type as their lowercase keyword.
//
// §3.4: "Each primitive keyword aliases a standard library type. The two forms
// are interchangeable: `bool`/`Boolean`, `char`/`Char`, `string`/`String`,
// `byte`/`Byte`, `sbyte`/`SByte`, `short`/`Int16`, `ushort`/`UInt16`,
// `int`/`Int32`, `uint`/`UInt32`, `long`/`Int64`, `ulong`/`UInt64`,
// `float`/`Float`, `double`/`Double`, `decimal`/`Decimal`."
//
// Bug: before the fix, every PascalCase name errored with
// "unresolved type name `String`" etc. — primitiveFromName did not handle
// the PascalCase spellings (TypeResolution.cpp).
//
// After the fix: each PascalCase name resolves to the same TypeRef::Kind::Primitive
// as its lowercase keyword, so it is interchangeable in type positions and in
// static-member dispatch (String.From, Int32.MaxValue, etc.).
//
// fix #3833 (ruled #3812, v0.293): `Single` was never in §3.4's table (only
// `Float` is `float`'s PascalCase alias) — the resolver's undocumented
// `Single` -> float arm is deleted, so `Single` is now an ordinary,
// unreserved identifier: `var c: Single := …;` is "unresolved type name",
// and `record Single(...)` is accepted (below, was rejected under #1081).

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::LocalSymbol;
using bezel::sema::PrimitiveType;
using bezel::sema::TypeRef;

namespace {

bezel::test::SemaResult parseSema(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

bool anyMessageMentions(const bezel::test::SemaResult& r,
                        const std::string& needle) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(needle) != std::string::npos) return true;
    return false;
}

LocalSymbol* findLocal(const bezel::test::SemaResult& r,
                       std::string_view name) {
    for (const auto& s : r.ctx->symbols()) {
        if (auto* ls = dynamic_cast<LocalSymbol*>(s.get()))
            if (ls->name == name) return ls;
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Core: String resolves without error and produces a Primitive/String TypeRef.
// §3.4, #884 — was: "unresolved type name `String`".
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_pascalcase_aliases_884, string_type_in_local_no_error_sec3_4) {
    auto r = parseSema(
        "module M; class C public static method M(): int "
        "  var s: String := \"hello\"; return 0; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(anyMessageMentions(r, "unresolved type name"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// String local's declType is Primitive/String (same as `string`).
BEZEL_TEST(sema_pascalcase_aliases_884,
           string_local_has_primitive_string_type_sec3_4) {
    auto r = parseSema(
        "module M; class C public method M() "
        "  var s: String := \"hello\"; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* s = findLocal(r, "s");
    BEZEL_EXPECT_TRUE(s != nullptr);
    if (s && s->declType) {
        BEZEL_EXPECT_TRUE(s->declType->kind == TypeRef::Kind::Primitive);
        BEZEL_EXPECT_TRUE(s->declType->primitive == PrimitiveType::String);
    }
}

// ---------------------------------------------------------------------------
// Int32 resolves to Primitive/Int — same as `int`.
// §3.4, #884.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_pascalcase_aliases_884, int32_type_no_error_sec3_4) {
    auto r = parseSema(
        "module M; class C public method M() "
        "  var n: Int32 := 5; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(anyMessageMentions(r, "unresolved type name"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* n = findLocal(r, "n");
    BEZEL_EXPECT_TRUE(n != nullptr);
    if (n && n->declType) {
        BEZEL_EXPECT_TRUE(n->declType->kind == TypeRef::Kind::Primitive);
        BEZEL_EXPECT_TRUE(n->declType->primitive == PrimitiveType::Int);
    }
}

// ---------------------------------------------------------------------------
// Int64 → Primitive/Long.
// §3.4, #884.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_pascalcase_aliases_884, int64_type_no_error_sec3_4) {
    auto r = parseSema(
        "module M; class C public method M() "
        "  var L: Int64 := 9; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* L = findLocal(r, "L");
    BEZEL_EXPECT_TRUE(L != nullptr);
    if (L && L->declType) {
        BEZEL_EXPECT_TRUE(L->declType->kind == TypeRef::Kind::Primitive);
        BEZEL_EXPECT_TRUE(L->declType->primitive == PrimitiveType::Long);
    }
}

// ---------------------------------------------------------------------------
// Double → Primitive/Double.
// §3.4, #884.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_pascalcase_aliases_884, double_type_no_error_sec3_4) {
    auto r = parseSema(
        "module M; class C public method M() "
        "  var d: Double := 1.5; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* d = findLocal(r, "d");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d && d->declType) {
        BEZEL_EXPECT_TRUE(d->declType->kind == TypeRef::Kind::Primitive);
        BEZEL_EXPECT_TRUE(d->declType->primitive == PrimitiveType::Double);
    }
}

// ---------------------------------------------------------------------------
// Boolean → Primitive/Bool.
// §3.4, #884.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_pascalcase_aliases_884, boolean_type_no_error_sec3_4) {
    auto r = parseSema(
        "module M; class C public method M() "
        "  var b: Boolean := true; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* b = findLocal(r, "b");
    BEZEL_EXPECT_TRUE(b != nullptr);
    if (b && b->declType) {
        BEZEL_EXPECT_TRUE(b->declType->kind == TypeRef::Kind::Primitive);
        BEZEL_EXPECT_TRUE(b->declType->primitive == PrimitiveType::Bool);
    }
}

// ---------------------------------------------------------------------------
// Char → Primitive/Char.
// §3.4, #884.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_pascalcase_aliases_884, char_type_no_error_sec3_4) {
    auto r = parseSema(
        "module M; class C public method M() "
        "  var c: Char := 'z'; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* c = findLocal(r, "c");
    BEZEL_EXPECT_TRUE(c != nullptr);
    if (c && c->declType) {
        BEZEL_EXPECT_TRUE(c->declType->kind == TypeRef::Kind::Primitive);
        BEZEL_EXPECT_TRUE(c->declType->primitive == PrimitiveType::Char);
    }
}

// ---------------------------------------------------------------------------
// Remaining PascalCase aliases: Byte, SByte, Int16, UInt16, UInt32, UInt64,
// Decimal — each must not produce "unresolved type name".
// §3.4, #884.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_pascalcase_aliases_884, byte_sbyte_int16_uint16_no_error_sec3_4) {
    auto r = parseSema(
        "module M; class C public method M() "
        "  var a: Byte := 10; "
        "  var b: SByte := -5; "
        "  var c: Int16 := 100; "
        "  var d: UInt16 := 200; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(anyMessageMentions(r, "unresolved type name"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

BEZEL_TEST(sema_pascalcase_aliases_884, uint32_uint64_decimal_no_error_sec3_4) {
    auto r = parseSema(
        "module M; class C public method M() "
        "  var a: UInt32 := 1000; "
        "  var b: UInt64 := 9000000000; "
        "  var c: Decimal := decimal(2); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(anyMessageMentions(r, "unresolved type name"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// fix #3833 (ruled #3812, v0.293): `Single` is NOT a §3.4 alias — only
// `Float` pairs with `float`. `var c: Single := float(1.5);` must now be
// rejected as "unresolved type name `Single`", with a real (non-0:0) source
// location at the type annotation (the sema DiagnosticEngine here is
// populated directly by TypeResolution_Names.cpp's `t.span.begin`, not via
// the AOT harness's location-discarding wrapper).
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_pascalcase_aliases_884, single_no_longer_an_alias_rejected_3833) {
    auto r = parseSema(
        "module M; class C public method M() "
        "  var c: Single := float(1.5); "
        "end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "unresolved type name `Single`"));
    bool sawRealLocation = false;
    for (const auto& d : r.diagnostics.diagnostics()) {
        if (d.message.find("unresolved type name `Single`") != std::string::npos) {
            if (d.location.line != 0 || d.location.column != 0)
                sawRealLocation = true;
        }
    }
    BEZEL_EXPECT_TRUE(sawRealLocation);
}

// ---------------------------------------------------------------------------
// PascalCase in a method signature: `method F(x: Int32): String` compiles.
// §3.4, #884.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_pascalcase_aliases_884,
           method_signature_pascalcase_no_error_sec3_4) {
    auto r = parseSema(
        "module M; class C "
        "  public static method F(x: Int32): String => x.ToString(); "
        "end C;");
    BEZEL_EXPECT_FALSE(anyMessageMentions(r, "unresolved type name"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Static member dispatch: String.From is resolved through the alias.
// §3.4, #884.  String → Primitive/String → tryPrimitiveStaticMember path.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_pascalcase_aliases_884,
           string_from_static_member_no_error_sec3_4) {
    auto r = parseSema(
        "module M; class C public method M() "
        "  var s: String := String.From('X'); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(anyMessageMentions(r, "unresolved"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Int32.MaxValue — static property on the PascalCase alias.
// §3.4, #884.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_pascalcase_aliases_884,
           int32_maxvalue_static_property_no_error_sec3_4) {
    auto r = parseSema(
        "module M; class C public method M() "
        "  var n: Int32 := Int32.MaxValue; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(anyMessageMentions(r, "unresolved"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// Regression: lowercase keywords still resolve correctly.
// §3.4 — lowercase is the primary form; PascalCase is the alias.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_pascalcase_aliases_884,
           lowercase_keywords_still_resolve_regression_sec3_4) {
    auto r = parseSema(
        "module M; class C public method M() "
        "  var n: int := 5; "
        "  var s: string := \"hi\"; "
        "  var b: bool := true; "
        "  var d: double := 1.0; "
        "  var c: char := 'z'; "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// String and string are interchangeable: String can be assigned to string and
// vice versa (same underlying Primitive/String type).
// §3.4, #884.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_pascalcase_aliases_884,
           string_and_String_are_interchangeable_sec3_4) {
    auto r = parseSema(
        "module M; class C "
        "  public static method Accept(s: string): int => 1; "
        "  public static method M(): int "
        "    var s: String := \"hello\"; "
        "    return Accept(s); "
        "  end M; "
        "end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
}

// ---------------------------------------------------------------------------
// fix #3833 (ruled #3812, v0.293): `record Single` is now ACCEPTED — flips the
// pre-fix expectation pinned by #1081. Before this fix `Single` was reserved
// riding on TypeResolution.cpp's undocumented `Single -> float` resolver arm
// (the #1081 wrong-value bug: `record Single` silently shadowed `float`, so
// `new Single(42).v` read 0); with that arm deleted, §3.4's reserved-name list
// never named `Single` (only `Float` pairs with `float`), so nothing reserves
// it any more and the declaration is a clean, ordinary user record. Asserts
// both that no reservation/unresolved error fires AND that `s`'s declared
// type resolves to the user record (Kind::Named), not to Primitive/Float —
// the actual #1081 shadowing hazard this test previously existed to catch.
// ---------------------------------------------------------------------------
BEZEL_TEST(sema_pascalcase_aliases_884, user_record_single_now_accepted_3833) {
    auto r = parseSema(
        "module M; record Single(v: int); "
        "class C public method M() "
        "  var s: Single := new Single(42); "
        "end M; end C;");
    BEZEL_EXPECT_FALSE(anyMessageMentions(r, "reserved primitive-alias type name"));
    BEZEL_EXPECT_FALSE(anyMessageMentions(r, "unresolved type name"));
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* s = findLocal(r, "s");
    BEZEL_EXPECT_TRUE(s != nullptr);
    if (s && s->declType) {
        BEZEL_EXPECT_TRUE(s->declType->kind == TypeRef::Kind::Named);
    }
}

BEZEL_TEST(sema_pascalcase_aliases_884, user_class_double_reserved_rejected_1081) {
    auto r = parseSema(
        "module M; class Double end Double; "
        "class C public static method Main(): int => 0; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "reserved primitive-alias type name"));
}

BEZEL_TEST(sema_pascalcase_aliases_884, user_record_int32_reserved_rejected_1081) {
    auto r = parseSema(
        "module M; record Int32(x: int); "
        "class C public static method Main(): int => 0; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "reserved primitive-alias type name"));
}

BEZEL_TEST(sema_pascalcase_aliases_884, user_interface_string_reserved_rejected_1081) {
    auto r = parseSema(
        "module M; interface String end String; "
        "class C public static method Main(): int => 0; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    BEZEL_EXPECT_TRUE(anyMessageMentions(r, "reserved primitive-alias type name"));
}

// §3.4: `Object` is NOT reserved (it has no keyword alias) — and an ordinary
// non-alias user type name is accepted. Neither triggers the #1081 reservation.
BEZEL_TEST(sema_pascalcase_aliases_884, user_record_non_reserved_name_accepted_1081) {
    auto r = parseSema(
        "module M; record MyValue(v: int); "
        "class C public static method Main(): int "
        "  var a := new MyValue(5); return a.v; end Main; end C;");
    BEZEL_EXPECT_FALSE(anyMessageMentions(r, "reserved primitive-alias type name"));
}
