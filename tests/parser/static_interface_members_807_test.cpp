// Spec-derived parser tests for §11.8 (v0.176, #807) — static interface
// members. InterfaceMember gains the abstract-only `static method` and
// `static operator` forms:
//
//   interface INumber<T> where T: INumber<T>
//     static method Zero(): T;
//     static operator +(a: T, b: T): T;
//   end INumber;
//
// Static interface members are ABSTRACT-ONLY (no bodies — the §17.4
// diamond-avoidance posture); static interface properties/constants are
// deferred in v1 (spell a constant as a nullary method). Authority: spec
// §11.8 (v0.176), stdlib §2.19.

#include <string>

#include "support/parse_support.hpp"
#include "test_harness.hpp"

using bezel::test::parseAll;

namespace {

bool succeedsParse(const std::string& source) {
    auto r = parseAll(source);
    return !r.diagnostics.hasErrors();
}

bool failsNaming118(const std::string& source) {
    auto r = parseAll(source);
    if (!r.diagnostics.hasErrors()) return false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("11.8") != std::string::npos) return true;
    return false;
}

// The first top-level TypeDecl of a parsed source, or null.
const bezel::ast::TypeDecl* firstType(const bezel::test::ParseResult& r) {
    if (!r.sourceFile || r.sourceFile->topDecls.empty()) return nullptr;
    return dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
}

}  // namespace

// ── §11.8: the static method member form parses (the spec's own example) ────

BEZEL_TEST(parser_static_iface_807, static_method_member_parses) {
    // per §11.8 example: `static method Zero(): T;`
    auto r = parseAll(
        "module M; "
        "interface INum<T> where T: INum<T> "
        "  static method Zero(): T; "
        "end INum;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* td = firstType(r);
    BEZEL_EXPECT_TRUE(td != nullptr);
    if (!td || td->members.size() != 1) { BEZEL_EXPECT_TRUE(false); return; }
    auto* md = dynamic_cast<const bezel::ast::MethodDecl*>(td->members[0].get());
    BEZEL_EXPECT_TRUE(md != nullptr);
    if (!md) return;
    BEZEL_EXPECT_TRUE(md->name == "Zero");
    BEZEL_EXPECT_TRUE(md->bodyKind == bezel::ast::MethodBodyKind::Abstract);
    bool hasStatic = false;
    for (auto m : md->modifiers)
        if (m == "static") hasStatic = true;
    BEZEL_EXPECT_TRUE(hasStatic);
}

// §11.8: the static operator member form parses to an abstract OperatorDecl.
BEZEL_TEST(parser_static_iface_807, static_operator_member_parses) {
    // per §11.8 example: `static operator +(a: T, b: T): T;`
    auto r = parseAll(
        "module M; "
        "interface INum<T> where T: INum<T> "
        "  static operator +(a: T, b: T): T; "
        "end INum;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* td = firstType(r);
    BEZEL_EXPECT_TRUE(td != nullptr);
    if (!td || td->members.size() != 1) { BEZEL_EXPECT_TRUE(false); return; }
    auto* od =
        dynamic_cast<const bezel::ast::OperatorDecl*>(td->members[0].get());
    BEZEL_EXPECT_TRUE(od != nullptr);
    if (!od) return;
    BEZEL_EXPECT_TRUE(od->opToken == "+");
    BEZEL_EXPECT_TRUE(od->params.size() == 2);
    BEZEL_EXPECT_TRUE(od->bodyKind == bezel::ast::MethodBodyKind::Abstract);
}

// stdlib §2.19: the full INumber<T> member set parses (methods + 5 operators).
BEZEL_TEST(parser_static_iface_807, stdlib_2_19_inumber_shape_parses) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; "
        "public interface INum<T> where T: INum<T> "
        "  static method Zero(): T; "
        "  static method One(): T; "
        "  static operator +(a: T, b: T): T; "
        "  static operator -(a: T, b: T): T; "
        "  static operator *(a: T, b: T): T; "
        "  static operator /(a: T, b: T): T; "
        "  static operator <(a: T, b: T): bool; "
        "end INum;"));
}

// stdlib §2.19: IParsable<T>'s out-param member parses (`out value: T?`).
BEZEL_TEST(parser_static_iface_807, iparsable_out_param_parses) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; "
        "public interface IPar<T> where T: IPar<T> "
        "  static method Parse(s: string): T; "
        "  static method TryParse(s: string, out value: T?): bool; "
        "end IPar;"));
}

// §11.8: an `owned` return modifier on a static interface method parses
// (the §11.2 exact-match rule then applies to implementations).
BEZEL_TEST(parser_static_iface_807, static_method_owned_return_parses) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; "
        "interface IFac<T> where T: IFac<T> "
        "  static method Make(): owned T; "
        "end IFac;"));
}

// ── §11.8: abstract-only — a body is a compile error naming §11.8 ──────────

BEZEL_TEST(parser_static_iface_807, static_method_body_rejected) {
    BEZEL_EXPECT_TRUE(failsNaming118(
        "module M; "
        "interface INum<T> "
        "  static method Zero(): T => 0; "
        "end INum;"));
}

BEZEL_TEST(parser_static_iface_807, static_operator_body_rejected) {
    BEZEL_EXPECT_TRUE(failsNaming118(
        "module M; "
        "interface INum<T> "
        "  static operator +(a: T, b: T): T => a; "
        "end INum;"));
}

// ── §11.8 v1 scope: static property / const / event forms are rejected ─────

BEZEL_TEST(parser_static_iface_807, static_property_rejected) {
    // per §11.8: constants/properties deferred — spell a constant as a
    // nullary method (`Zero()`).
    BEZEL_EXPECT_TRUE(failsNaming118(
        "module M; "
        "interface I "
        "  static property Count: int; "
        "end I;"));
}

BEZEL_TEST(parser_static_iface_807, static_event_rejected) {
    BEZEL_EXPECT_TRUE(failsNaming118(
        "module M; "
        "interface I "
        "  static event Changed: int; "
        "end I;"));
}

// §11.8: an interface operator member REQUIRES `static`.
BEZEL_TEST(parser_static_iface_807, operator_without_static_rejected) {
    BEZEL_EXPECT_TRUE(failsNaming118(
        "module M; "
        "interface INum<T> "
        "  operator +(a: T, b: T): T; "
        "end INum;"));
}

// ── Instance interface members are unaffected by the §11.8 additions ───────

BEZEL_TEST(parser_static_iface_807, instance_members_unaffected) {
    BEZEL_EXPECT_TRUE(succeedsParse(
        "module M; "
        "interface IShape "
        "  method Area(): double; "
        "  readonly property Name: string; "
        "  event Changed: int; "
        "end IShape;"));
}

// Mixed static + instance members in one interface parse side by side.
BEZEL_TEST(parser_static_iface_807, mixed_static_and_instance_members) {
    auto r = parseAll(
        "module M; "
        "interface IBoth<T> where T: IBoth<T> "
        "  method Scale(f: int): T; "
        "  static method Zero(): T; "
        "  static operator +(a: T, b: T): T; "
        "end IBoth;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    const auto* td = firstType(r);
    BEZEL_EXPECT_TRUE(td != nullptr);
    BEZEL_EXPECT_TRUE(td && td->members.size() == 3);
}
