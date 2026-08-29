#include "common/AwaitScan.hpp"

#include <cstdio>
#include <cstdlib>
#include <typeinfo>

#include "ast/Node.hpp"

// See AwaitScan.hpp for the full rationale (#3587 / #2251). Every dynamic_cast
// arm below preserves the recursion shape the four original copies used
// (verified by the #3587 fix commit's exclusion diff), so a diff against the
// old per-copy behaviour on the previously-handled kinds is mechanical.

namespace bezel::common {

namespace {

// Loud, UNCONDITIONAL failure (never compiled out by NDEBUG — see the header
// for why a plain assert() is not enough) for an ast::Expr kind this file
// does not recognise.
[[noreturn]] void unhandledAwaitScanExprKind(const ast::Expr& e) {
    std::fprintf(stderr,
        "internal compiler error: src/common/AwaitScan.cpp's "
        "forEachAwaitSite does not recognise this ast::Expr kind "
        "(typeid=%s) at %u:%u -- a new *Expr struct was added to "
        "src/ast/Node.hpp without a matching arm here (see AwaitScan.hpp)\n",
        typeid(e).name(), e.span.begin.line, e.span.begin.column);
    std::abort();
}

// Same, for an ast::Pattern kind.
[[noreturn]] void unhandledAwaitScanPatternKind(const ast::Pattern& p) {
    std::fprintf(stderr,
        "internal compiler error: src/common/AwaitScan.cpp's "
        "forEachAwaitSiteInPattern does not recognise this ast::Pattern "
        "kind (typeid=%s) -- a new *Pattern struct was added to "
        "src/ast/Node.hpp without a matching arm here (see AwaitScan.hpp)\n",
        typeid(p).name());
    std::abort();
}

}  // namespace

void forEachAwaitSite(const ast::Expr* e, AwaitSiteFn onAwait) {
    if (!e) return;

    // --- Kinds with Expr children: recurse, mirroring the four original
    // copies' shape exactly (order preserved for a mechanical diff). ---
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(e)) {
        if (ue->op == "await") onAwait(ue);
        forEachAwaitSite(ue->operand.get(), onAwait);
        return;
    }
    if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(e)) {
        forEachAwaitSite(bin->left.get(), onAwait);
        forEachAwaitSite(bin->right.get(), onAwait);
        return;
    }
    if (auto* me = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
        forEachAwaitSite(me->target.get(), onAwait);
        return;
    }
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
        forEachAwaitSite(nm->target.get(), onAwait);
        return;
    }
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(e)) {
        forEachAwaitSite(ce->callee.get(), onAwait);
        for (const auto& a : ce->args) forEachAwaitSite(a.value.get(), onAwait);
        return;
    }
    if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
        forEachAwaitSite(nc->callee.get(), onAwait);
        for (const auto& a : nc->args) forEachAwaitSite(a.value.get(), onAwait);
        return;
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
        forEachAwaitSite(ix->target.get(), onAwait);
        for (const auto& a : ix->indices) forEachAwaitSite(a.value.get(), onAwait);
        return;
    }
    if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
        forEachAwaitSite(bg->operand.get(), onAwait);
        return;
    }
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(e)) {
        for (const auto& a : ne->args) forEachAwaitSite(a.value.get(), onAwait);
        return;
    }
    if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(e)) {
        forEachAwaitSite(cd->condition.get(), onAwait);
        forEachAwaitSite(cd->thenValue.get(), onAwait);
        forEachAwaitSite(cd->elseValue.get(), onAwait);
        return;
    }
    if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(e)) {
        forEachAwaitSite(te->operand.get(), onAwait);
        return;
    }
    if (auto* th = dynamic_cast<const ast::ThrowExpr*>(e)) {
        forEachAwaitSite(th->operand.get(), onAwait);
        return;
    }
    if (auto* we = dynamic_cast<const ast::WithExpr*>(e)) {
        forEachAwaitSite(we->source.get(), onAwait);
        for (const auto& ia : we->assigns) forEachAwaitSite(ia.value.get(), onAwait);
        return;
    }
    if (auto* re = dynamic_cast<const ast::RangeExpr*>(e)) {
        forEachAwaitSite(re->lo.get(), onAwait);
        forEachAwaitSite(re->hi.get(), onAwait);
        forEachAwaitSite(re->step.get(), onAwait);
        return;
    }
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
        for (const auto& k : cl->keys) forEachAwaitSite(k.get(), onAwait);
        for (const auto& v : cl->elements) forEachAwaitSite(v.get(), onAwait);
        return;
    }
    if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
        for (const auto& p : is->parts) forEachAwaitSite(p.expr.get(), onAwait);
        return;
    }

    // --- fix #3587 / #2251: the CaseExpr gap. Unreached by ALL FOUR
    // original copies (none of them even looked at `scrutinee` or `arms`) —
    // MEASURED LIVE, not latent (see the header). ---
    if (auto* cse = dynamic_cast<const ast::CaseExpr*>(e)) {
        forEachAwaitSite(cse->scrutinee.get(), onAwait);
        for (const auto& arm : cse->arms)
            forEachAwaitSiteInCaseArm(arm, onAwait);
        return;
    }

    // fix #3587: the BindExpr gap (latent — see the header; only CFG.cpp had
    // this arm before). Clauses are plain-`var` LocalDecl nodes
    // (§17.15/A.7); their `initializer` is the clause's Expr.
    if (auto* bx = dynamic_cast<const ast::BindExpr*>(e)) {
        for (const auto& cl : bx->clauses)
            if (cl) forEachAwaitSite(cl->initializer.get(), onAwait);
        forEachAwaitSite(bx->body.get(), onAwait);
        return;
    }

    // --- Leaves: no Expr-typed child, so nothing to recurse into. Listed
    // explicitly (rather than a wildcard) so the exhaustiveness contract
    // holds — a kind not in this list falls to the loud failure below. ---
    if (dynamic_cast<const ast::IntLiteralExpr*>(e)) return;
    if (dynamic_cast<const ast::FloatLiteralExpr*>(e)) return;
    if (dynamic_cast<const ast::CharLiteralExpr*>(e)) return;
    if (dynamic_cast<const ast::StringLiteralExpr*>(e)) return;
    if (dynamic_cast<const ast::VerbatimStringExpr*>(e)) return;
    if (dynamic_cast<const ast::BoolLiteralExpr*>(e)) return;
    if (dynamic_cast<const ast::NullLiteralExpr*>(e)) return;
    if (dynamic_cast<const ast::IdentifierExpr*>(e)) return;
    // TypeOfExpr/NameOfExpr/DefaultExpr hold a Type/QualifiedName, never an
    // Expr — a `typeof`/`nameof`/`default` operand cannot syntactically
    // embed an `await`.
    if (dynamic_cast<const ast::TypeOfExpr*>(e)) return;
    if (dynamic_cast<const ast::NameOfExpr*>(e)) return;
    if (dynamic_cast<const ast::DefaultExpr*>(e)) return;
    if (dynamic_cast<const ast::UnitExpr*>(e)) return;
    // MarkedTransferExpr (`owned Identifier`) holds only a name — the
    // identifier it marks, never a sub-expression.
    if (dynamic_cast<const ast::MarkedTransferExpr*>(e)) return;
    // LambdaExpr: DELIBERATELY not descended — a separate async scope
    // (§30.6). Matches all four original copies' documented behaviour.
    if (dynamic_cast<const ast::LambdaExpr*>(e)) return;

    unhandledAwaitScanExprKind(*e);
}

void forEachAwaitSiteInPattern(const ast::Pattern* p, AwaitSiteFn onAwait) {
    if (!p) return;
    if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(p)) {
        for (const auto& c : cp->constants) forEachAwaitSite(c.get(), onAwait);
        return;
    }
    if (auto* rp = dynamic_cast<const ast::RangePattern*>(p)) {
        forEachAwaitSite(rp->rangeExpr.get(), onAwait);
        return;
    }
    if (auto* rop = dynamic_cast<const ast::RelOpPattern*>(p)) {
        forEachAwaitSite(rop->expr.get(), onAwait);
        return;
    }
    if (auto* ap = dynamic_cast<const ast::AltPattern*>(p)) {
        for (const auto& m : ap->members)
            forEachAwaitSiteInPattern(m.get(), onAwait);
        return;
    }
    if (auto* dp = dynamic_cast<const ast::DestructuringPattern*>(p)) {
        for (const auto& sub : dp->subPatterns)
            forEachAwaitSiteInPattern(sub.pattern.get(), onAwait);
        return;
    }
    // Leaves: no embedded Expr. BindingPattern/TypePattern's `type` field is
    // a Type, never an Expr; NamedPattern holds only a QualifiedName;
    // Discard/Null bind nothing.
    if (dynamic_cast<const ast::DiscardPattern*>(p)) return;
    if (dynamic_cast<const ast::NullPattern*>(p)) return;
    if (dynamic_cast<const ast::BindingPattern*>(p)) return;
    if (dynamic_cast<const ast::NamedPattern*>(p)) return;
    if (dynamic_cast<const ast::TypePattern*>(p)) return;

    unhandledAwaitScanPatternKind(*p);
}

void forEachAwaitSiteInCaseArm(const ast::CaseArm& arm, AwaitSiteFn onAwait) {
    forEachAwaitSiteInPattern(arm.pattern.get(), onAwait);
    forEachAwaitSite(arm.guard.get(), onAwait);
    if (arm.bodyKind == ast::CaseArmBodyKind::Expression)
        forEachAwaitSite(arm.exprBody.get(), onAwait);
    // Block-form body: `arm.stmts` are Stmt-level, out of this Expr-level
    // walk's scope (see the header).
}

}  // namespace bezel::common
