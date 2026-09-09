// Spec-derived tests for Sema slice 3e — parameter typing on
// identifier use. An IdentifierExpr resolving to a ParameterSymbol
// should carry the parameter's declared type via exprType. Combined
// with slice 3d, this lets `p.X` resolve member `X` on the parameter's
// type.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::FieldSymbol;
using bezel::sema::ParameterSymbol;
using bezel::sema::PrimitiveType;
using bezel::sema::TypeRef;

namespace {

bezel::test::SemaResult parseSemaAll(std::string_view src) {
    auto r = bezel::test::parseAndCollect(src);
    if (r.sourceFile && r.ctx) {
        bezel::sema::resolveTypes(*r.sourceFile, *r.ctx);
        bezel::sema::typecheckExpressions(*r.sourceFile, *r.ctx);
    }
    return r;
}

// #1166: does any reported diagnostic contain `sub`?
bool hasError(const bezel::test::SemaResult& r, const char* sub) {
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find(sub) != std::string::npos) return true;
    return false;
}

// Find the first method body's first IdentifierExpr discard
// (`_ := <expr>;` form). Returns null if not present.
const bezel::ast::IdentifierExpr* firstDiscardIdentExpr(
    const bezel::test::SemaResult& r) {
    if (!r.sourceFile) return nullptr;
    for (const auto& topD : r.sourceFile->topDecls) {
        const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(topD.get());
        if (!td) continue;
        for (const auto& m : td->members) {
            const auto* md =
                dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
            if (!md) continue;
            if (md->bodyKind == bezel::ast::MethodBodyKind::Expression &&
                md->exprBody) {
                if (auto* id =
                        dynamic_cast<const bezel::ast::IdentifierExpr*>(
                            md->exprBody.get())) return id;
            }
            if (md->bodyKind == bezel::ast::MethodBodyKind::Block) {
                for (const auto& s : md->stmts) {
                    if (!s) continue;
                    if (auto* ds =
                            dynamic_cast<const bezel::ast::DiscardStmt*>(
                                s.get())) {
                        if (auto* id =
                                dynamic_cast<const bezel::ast::IdentifierExpr*>(
                                    ds->value.get())) return id;
                    }
                }
            }
        }
    }
    return nullptr;
}

const bezel::ast::MemberAccessExpr* firstDiscardMemberAccess(
    const bezel::test::SemaResult& r) {
    if (!r.sourceFile) return nullptr;
    for (const auto& topD : r.sourceFile->topDecls) {
        const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(topD.get());
        if (!td) continue;
        for (const auto& m : td->members) {
            const auto* md =
                dynamic_cast<const bezel::ast::MethodDecl*>(m.get());
            if (!md || md->bodyKind != bezel::ast::MethodBodyKind::Block)
                continue;
            for (const auto& s : md->stmts) {
                if (!s) continue;
                if (auto* ds =
                        dynamic_cast<const bezel::ast::DiscardStmt*>(s.get())) {
                    if (auto* ma =
                            dynamic_cast<const bezel::ast::MemberAccessExpr*>(
                                ds->value.get())) return ma;
                }
            }
        }
    }
    return nullptr;
}

} // namespace

BEZEL_TEST(sema_param, primitive_param_identifier_typed) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public method M(p: int) _ := p; end M; "
        "end C;");
    auto* id = firstDiscardIdentExpr(r);
    BEZEL_EXPECT_TRUE(id != nullptr);
    if (id) {
        auto* sym = r.ctx->exprSymbol(*id);
        BEZEL_EXPECT_TRUE(dynamic_cast<ParameterSymbol*>(sym) != nullptr);
        auto* t = r.ctx->exprType(*id);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

BEZEL_TEST(sema_param, named_param_identifier_typed) {
    auto r = parseSemaAll(
        "module M; "
        "class D end D; "
        "class C "
        "  public method M(d: D) _ := d; end M; "
        "end C;");
    auto* id = firstDiscardIdentExpr(r);
    BEZEL_EXPECT_TRUE(id != nullptr);
    if (id) {
        auto* t = r.ctx->exprType(*id);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named);
    }
}

BEZEL_TEST(sema_param, param_field_member_access_resolves) {
    auto r = parseSemaAll(
        "module M; "
        "class D public _y: int; end D; "
        "class C "
        "  public method M(d: D) _ := d._y; end M; "
        "end C;");
    auto* ma = firstDiscardMemberAccess(r);
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<FieldSymbol*>(sym) != nullptr);
        auto* t = r.ctx->exprType(*ma);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

BEZEL_TEST(sema_param, owned_param_modifier_carried) {
    // Slice 3e: the modifier doesn't change the type-carrying behavior;
    // an `owned T` param still has type T on identifier use.
    auto r = parseSemaAll(
        "module M; "
        "class D end D; "
        "class C "
        "  public method M(owned d: D) _ := d; end M; "
        "end C;");
    auto* id = firstDiscardIdentExpr(r);
    BEZEL_EXPECT_TRUE(id != nullptr);
    if (id) {
        auto* sym = r.ctx->exprSymbol(*id);
        auto* par = dynamic_cast<ParameterSymbol*>(sym);
        BEZEL_EXPECT_TRUE(par != nullptr);
        if (par) BEZEL_EXPECT_TRUE(par->modifier == "owned");
        auto* t = r.ctx->exprType(*id);
        BEZEL_EXPECT_TRUE(t && t->kind == TypeRef::Kind::Named);
    }
}

BEZEL_TEST(sema_param, constructor_param_identifier_typed) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public constructor(p: int) _ := p; end constructor; "
        "end C;");
    // First ctor's first stmt.
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[0].get());
    const auto* cd = dynamic_cast<const bezel::ast::ConstructorDecl*>(
        td->members[0].get());
    const auto* ds = dynamic_cast<const bezel::ast::DiscardStmt*>(
        cd->body[0].get());
    const auto* id =
        dynamic_cast<const bezel::ast::IdentifierExpr*>(ds->value.get());
    BEZEL_EXPECT_TRUE(id != nullptr);
    if (id) {
        auto* t = r.ctx->exprType(*id);
        BEZEL_EXPECT_TRUE(t && t->primitive == PrimitiveType::Int);
    }
}

// ---------------------------------------------------------------------------
// §17.11 (#1166): the `default(T)` well-formedness check must run on a
// parameter's default-value expression. An ill-formed `default(R)` for a
// non-nullable reference type R has no zero value and must be rejected exactly
// as in any other position; previously the param default's inner type node was
// never resolved by TypeResolution, so the check never fired and the ill-formed
// default slipped through silently.
// ---------------------------------------------------------------------------

// A non-nullable class-typed parameter default `default(T)` is rejected (§17.11).
BEZEL_TEST(sema_param, default_nonnullable_ref_param_default_rejected_1166) {
    auto r = parseSemaAll(
        "module M; "
        "class T end T; "
        "class C "
        "  public static method M(w: T := default(T)): int => 0; "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "default(T)") && hasError(r, "17.11"));
}

// `default(string)` (non-nullable reference) as a param default is rejected.
BEZEL_TEST(sema_param, default_string_param_default_rejected_1166) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method M(s: string := default(string)): int => 0; "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "default(T)") && hasError(r, "17.11"));
}

// A constructor parameter default `default(T)` (non-nullable ref) is rejected
// — the check runs on constructor params too, not only methods.
BEZEL_TEST(sema_param, default_nonnullable_ref_ctor_param_default_rejected_1166) {
    auto r = parseSemaAll(
        "module M; "
        "class T end T; "
        "class C "
        "  public constructor(w: T := default(T)) end constructor; "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "default(T)") && hasError(r, "17.11"));
}

// A record whose declared field is a non-nullable reference type has no
// well-defined default; `default(R)` as a param default is rejected (§17.11).
BEZEL_TEST(sema_param, default_record_nonnullable_ref_field_param_default_rejected_1166) {
    auto r = parseSemaAll(
        "module M; "
        "class T end T; "
        "record R(Inner: T); "
        "class C "
        "  public static method M(r: R := default(R)): int => 0; "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "17.11"));
}

// Control: a value-type parameter default `default(int)` is well-formed (§17.11).
BEZEL_TEST(sema_param, default_value_type_param_default_ok_1166) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method M(x: int := default(int)): int => x; "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, "17.11"));
}

// Control: a nullable reference parameter default `default(T?)` is well-formed —
// it produces null, the legitimate nullable counterpart (§17.11).
BEZEL_TEST(sema_param, default_nullable_ref_param_default_ok_1166) {
    auto r = parseSemaAll(
        "module M; "
        "class T end T; "
        "class C "
        "  public static method M(w: T? := default(T?)): int => 0; "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, "17.11"));
}

// Control: a record all of whose fields are value types has a well-defined
// default; `default(R)` as a param default is accepted (§17.11).
BEZEL_TEST(sema_param, default_value_record_param_default_ok_1166) {
    auto r = parseSemaAll(
        "module M; "
        "record P(X: int, Y: int); "
        "class C "
        "  public static method M(p: P := default(P)): int => p.X; "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, "17.11"));
}

// §22.5 "Defaults and modifiers" (v0.184, issue #1684): a default-value
// expression that produces an owned value of NON-trivial-dispose class type
// ([§28.9](Bezel_Language_Spec.md#sec-trivial-dispose-classes)) is a compile
// error on a plain (non-`owned`) parameter — the fresh instance has no owner
// to discharge it, a leak. `T` here declares a destructor, so `new T()` is a
// non-trivial-dispose producer, and `w` carries no modifier.
BEZEL_TEST(sema_param, default_owned_new_on_plain_param_rejected_1684) {
    auto r = parseSemaAll(
        "module M; "
        "class T "
        "  destructor end destructor; "
        "end T; "
        "class C "
        "  public static method M(w: T := new T()): int => 0; "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "non-trivial-dispose") && hasError(r, "22.5"));
}

// Control (§22.5): the identical default is admitted when the parameter is
// declared `owned` — the owned-allocation default form the section already
// blesses; the callee receives and discharges the fresh instance.
BEZEL_TEST(sema_param, default_owned_new_on_owned_param_accepted_1684) {
    auto r = parseSemaAll(
        "module M; "
        "class T "
        "  destructor end destructor; "
        "end T; "
        "class C "
        "  public static method M(owned w: T := new T()): int => 0; "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, "non-trivial-dispose"));
}

// Control (§22.5 / §28.9): a default whose class is trivial-dispose (no
// destructor, no owned field) stays admissible on a plain parameter — it
// carries no disposal obligation to leak.
BEZEL_TEST(sema_param,
           default_owned_new_trivial_dispose_on_plain_param_accepted_1684) {
    auto r = parseSemaAll(
        "module M; "
        "class S end S; "
        "class C "
        "  public static method M(w: S := new S()): int => 0; "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, "non-trivial-dispose"));
}

// §22.5: the factory-call form of the same rule — a static method declared
// `owned`-returning is an owned producer exactly like `new T()`. Mirrors the
// issue's own rejected example: `M(log: Logger := Logger.Create())`.
BEZEL_TEST(sema_param, default_owned_factory_call_on_plain_param_rejected_1684) {
    auto r = parseSemaAll(
        "module M; "
        "class Logger "
        "  destructor end destructor; "
        "  public static method Create(): owned Logger => new Logger(); "
        "end Logger; "
        "class Program "
        "  public static method M(log: Logger := Logger.Create()): int => 0; "
        "  public static method Main(): int => 0; "
        "end Program;");
    BEZEL_EXPECT_TRUE(hasError(r, "non-trivial-dispose") && hasError(r, "22.5"));
}

// Control, matching the issue's accepted example: `M(owned log: Logger :=
// Logger.Create())` — the `owned` parameter receives and discharges the
// factory's fresh instance.
BEZEL_TEST(sema_param,
           default_owned_factory_call_on_owned_param_accepted_1684) {
    auto r = parseSemaAll(
        "module M; "
        "class Logger "
        "  destructor end destructor; "
        "  public static method Create(): owned Logger => new Logger(); "
        "end Logger; "
        "class Program "
        "  public static method M(owned log: Logger := Logger.Create()): "
        "int => 0; "
        "  public static method Main(): int => 0; "
        "end Program;");
    BEZEL_EXPECT_TRUE(!hasError(r, "non-trivial-dispose"));
}

// Control, matching the issue's other accepted example: a trivial-dispose
// stdlib factory default (`new StringBuilder()`, stdlib §2.9) stays
// admissible on a plain parameter.
BEZEL_TEST(sema_param,
           default_stringbuilder_trivial_dispose_on_plain_param_accepted_1684) {
    auto r = parseSemaAll(
        "module M; "
        "class C "
        "  public static method M(sb: StringBuilder := new StringBuilder()): "
        "int => 0; "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, "non-trivial-dispose"));
}

// §22.5 / §28.9: the owned-FIELD path. §28.9 (v0.286) evaluates the
// trivial-dispose property RECURSIVELY: "an `owned` field whose declared type
// is itself trivial-dispose does not disqualify the holder". `T` here has no
// destructor and no owned fields, so `Holder { owned _t: T }` is
// trivial-dispose and the default `new Holder()` on a plain parameter is
// ADMITTED. (Before fix #3632 / #3647 the predicate short-circuited on any
// owned field and this row pinned that conservative verdict; the R12 gate
// caught the fixture when the widening landed.)
BEZEL_TEST(sema_param,
           default_owned_new_trivial_owned_field_on_plain_param_admitted_1684) {
    auto r = parseSemaAll(
        "module M; "
        "class T end T; "
        "class Holder "
        "  private owned _t: T; "
        "  public constructor() _t := new T(); end constructor; "
        "end Holder; "
        "class C "
        "  public static method M(h: Holder := new Holder()): int => 0; "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(!hasError(r, "non-trivial-dispose"));
}

// §22.5 / §28.9: the same owned-FIELD path with a NON-trivial-dispose field
// type — `T` declares a destructor, so the holder is non-trivial-dispose
// through the recursive clause and the default `new` on a plain parameter is
// still rejected. This is the row the widening must not admit.
BEZEL_TEST(sema_param,
           default_owned_new_nontrivial_owned_field_on_plain_param_rejected_1684) {
    auto r = parseSemaAll(
        "module M; "
        "class T "
        "  destructor end destructor; "
        "end T; "
        "class Holder "
        "  private owned _t: T; "
        "  public constructor() _t := new T(); end constructor; "
        "end Holder; "
        "class C "
        "  public static method M(h: Holder := new Holder()): int => 0; "
        "  public static method Main(): int => 0; "
        "end C;");
    BEZEL_EXPECT_TRUE(hasError(r, "non-trivial-dispose") && hasError(r, "22.5"));
}
