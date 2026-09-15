// Spec-derived tests for Sema slice 3g — local type inference.
//
// Per §17.1 / §A.6: a `var x := EXPR` declaration without an explicit
// `: Type` ascription takes its type from the initializer. After this
// slice, a LocalSymbol carries either the resolved ascription or the
// inferred initializer-type as `declType`.
//
// The inference covers any expression with an attached exprType in
// the current snapshot: primitive literals, `new T()`, `default(T)`,
// and the operator-typed forms from slice 3c. Initializers that are
// not yet typed (e.g. method-call returns) leave declType null —
// those refine when later slices type them.

#include "sema/ExprTypecheck.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeResolution.hpp"
#include "support/sema_support.hpp"
#include "test_harness.hpp"

using bezel::sema::FieldSymbol;
using bezel::sema::LocalSymbol;
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

// Find a LocalSymbol by name across all symbols (returns the first
// match — sufficient for tests that declare a single local).
LocalSymbol* findLocal(const bezel::test::SemaResult& r,
                      std::string_view name) {
    for (const auto& s : r.ctx->symbols()) {
        if (auto* ls = dynamic_cast<LocalSymbol*>(s.get())) {
            if (ls->name == name) return ls;
        }
    }
    return nullptr;
}

} // namespace

BEZEL_TEST(sema_local_infer, int_initializer) {
    auto r = parseSemaAll(
        "module M; "
        "class C public method M() var x := 42; end M; end C;");
    auto* x = findLocal(r, "x");
    BEZEL_EXPECT_TRUE(x != nullptr);
    if (x) {
        BEZEL_EXPECT_TRUE(x->declType != nullptr);
        if (x->declType)
            BEZEL_EXPECT_TRUE(x->declType->primitive == PrimitiveType::Int);
    }
}

BEZEL_TEST(sema_local_infer, new_T_initializer) {
    auto r = parseSemaAll(
        "module M; "
        "class T end T; "
        "class C public method M() var t := new T(); end M; end C;");
    auto* t = findLocal(r, "t");
    BEZEL_EXPECT_TRUE(t != nullptr);
    if (t) {
        BEZEL_EXPECT_TRUE(t->declType != nullptr);
        if (t->declType)
            BEZEL_EXPECT_TRUE(t->declType->kind == TypeRef::Kind::Named);
    }
}

BEZEL_TEST(sema_local_infer, explicit_ascription_wins) {
    // When both an ascription and an initializer are present, the
    // ascription wins (subtyping check between them is a later slice).
    auto r = parseSemaAll(
        "module M; "
        "class D end D; "
        "class C public method M() var d: D := new D(); end M; end C;");
    auto* d = findLocal(r, "d");
    BEZEL_EXPECT_TRUE(d != nullptr);
    if (d) {
        BEZEL_EXPECT_TRUE(d->declType != nullptr);
        if (d->declType)
            BEZEL_EXPECT_TRUE(d->declType->kind == TypeRef::Kind::Named);
    }
}

BEZEL_TEST(sema_local_infer, inferred_local_supports_member_access) {
    // The whole reason this slice exists: `var d := new D(); d._y;`
    // should now resolve `_y` on the inferred D type.
    auto r = parseSemaAll(
        "module M; "
        "class D public _y: int; end D; "
        "class C "
        "  public method M() "
        "    var d := new D(); "
        "    _ := d._y; "
        "  end M; "
        "end C;");
    // Walk to the DiscardStmt's MemberAccessExpr.
    const auto* td = dynamic_cast<const bezel::ast::TypeDecl*>(
        r.sourceFile->topDecls[1].get());
    const auto* md =
        dynamic_cast<const bezel::ast::MethodDecl*>(td->members[0].get());
    const auto* ds =
        dynamic_cast<const bezel::ast::DiscardStmt*>(md->stmts[1].get());
    const auto* ma =
        dynamic_cast<const bezel::ast::MemberAccessExpr*>(ds->value.get());
    BEZEL_EXPECT_TRUE(ma != nullptr);
    if (ma) {
        auto* sym = r.ctx->exprSymbol(*ma);
        BEZEL_EXPECT_TRUE(dynamic_cast<FieldSymbol*>(sym) != nullptr);
    }
}

BEZEL_TEST(sema_local_infer, no_initializer_no_ascription_no_type) {
    // `var x;` (no type, no init) leaves declType null. Later DA
    // assignments would refine, but slice 3g doesn't track those.
    auto r = parseSemaAll(
        "module M; "
        "class C public method M() var x: int; end M; end C;");
    // Use a typed example to validate parser acceptance; the inference
    // case of "no init no ascription" doesn't parse cleanly (the
    // grammar requires `:= Expr` when there's no ascription), so we
    // simply verify ascription-only locals also receive declType.
    auto* x = findLocal(r, "x");
    BEZEL_EXPECT_TRUE(x != nullptr);
    if (x) {
        BEZEL_EXPECT_TRUE(x->declType != nullptr);
        if (x->declType)
            BEZEL_EXPECT_TRUE(x->declType->primitive == PrimitiveType::Int);
    }
}

// #1084 (§7.1): a bare `var x := null` carries no inferable type — a null
// literal has no type of its own. Before the fix it was silently accepted with
// an unresolved null type, and a later use (`x ?? 99`) crashed codegen with an
// LLVM PHI assertion. Reject at the declaration, directing the user to a
// nullable annotation.
BEZEL_TEST(sema_local_infer, bare_null_initializer_rejected_1084) {
    auto r = parseSemaAll(
        "module M; "
        "class C public method M() var x := null; end M; end C;");
    BEZEL_EXPECT_TRUE(r.diagnostics.hasErrors());
    bool mentionsInfer = false;
    for (const auto& d : r.diagnostics.diagnostics())
        if (d.message.find("cannot infer a type") != std::string::npos)
            mentionsInfer = true;
    BEZEL_EXPECT_TRUE(mentionsInfer);
}

// #1084 regression: a nullable-ANNOTATED null local is well-typed (the §7.1
// reject is only for the typeless bare form).
BEZEL_TEST(sema_local_infer, annotated_null_initializer_accepted_1084) {
    auto r = parseSemaAll(
        "module M; "
        "class C public method M() var x: int? := null; end M; end C;");
    BEZEL_EXPECT_FALSE(r.diagnostics.hasErrors());
    auto* x = findLocal(r, "x");
    BEZEL_EXPECT_TRUE(x != nullptr);
}
