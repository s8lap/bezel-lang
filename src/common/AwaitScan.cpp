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

// The ONE walk (see AwaitScan.hpp): `onNode` is called once for every
// expression node reachable from `e` along a synchronous sub-expression edge,
// `e` itself first (pre-order), a nested LambdaExpr's body excluded. The
// public entry points below are filters over it.
static void walkPattern(const ast::Pattern* p, SubExprFn onNode);
static void walkCaseArm(const ast::CaseArm& arm, SubExprFn onNode);

static void walkExpr(const ast::Expr* e, SubExprFn onNode) {
    if (!e) return;
    onNode(*e);

    // --- Kinds with Expr children: recurse, mirroring the four original
    // copies' shape exactly (order preserved for a mechanical diff). ---
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(e)) {
        walkExpr(ue->operand.get(), onNode);
        return;
    }
    if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(e)) {
        walkExpr(bin->left.get(), onNode);
        walkExpr(bin->right.get(), onNode);
        return;
    }
    if (auto* me = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
        walkExpr(me->target.get(), onNode);
        return;
    }
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
        walkExpr(nm->target.get(), onNode);
        return;
    }
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(e)) {
        walkExpr(ce->callee.get(), onNode);
        for (const auto& a : ce->args) walkExpr(a.value.get(), onNode);
        return;
    }
    if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
        walkExpr(nc->callee.get(), onNode);
        for (const auto& a : nc->args) walkExpr(a.value.get(), onNode);
        return;
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
        walkExpr(ix->target.get(), onNode);
        for (const auto& a : ix->indices) walkExpr(a.value.get(), onNode);
        return;
    }
    if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
        walkExpr(bg->operand.get(), onNode);
        return;
    }
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(e)) {
        for (const auto& a : ne->args) walkExpr(a.value.get(), onNode);
        return;
    }
    if (auto* cd = dynamic_cast<const ast::ConditionalExpr*>(e)) {
        walkExpr(cd->condition.get(), onNode);
        walkExpr(cd->thenValue.get(), onNode);
        walkExpr(cd->elseValue.get(), onNode);
        return;
    }
    if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(e)) {
        walkExpr(te->operand.get(), onNode);
        return;
    }
    if (auto* th = dynamic_cast<const ast::ThrowExpr*>(e)) {
        walkExpr(th->operand.get(), onNode);
        return;
    }
    if (auto* we = dynamic_cast<const ast::WithExpr*>(e)) {
        walkExpr(we->source.get(), onNode);
        for (const auto& ia : we->assigns) walkExpr(ia.value.get(), onNode);
        return;
    }
    if (auto* re = dynamic_cast<const ast::RangeExpr*>(e)) {
        walkExpr(re->lo.get(), onNode);
        walkExpr(re->hi.get(), onNode);
        walkExpr(re->step.get(), onNode);
        return;
    }
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
        for (const auto& k : cl->keys) walkExpr(k.get(), onNode);
        for (const auto& v : cl->elements) walkExpr(v.get(), onNode);
        return;
    }
    if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
        for (const auto& p : is->parts) walkExpr(p.expr.get(), onNode);
        return;
    }

    // --- fix #3587 / #2251: the CaseExpr gap. Unreached by ALL FOUR
    // original copies (none of them even looked at `scrutinee` or `arms`) —
    // MEASURED LIVE, not latent (see the header). ---
    if (auto* cse = dynamic_cast<const ast::CaseExpr*>(e)) {
        walkExpr(cse->scrutinee.get(), onNode);
        for (const auto& arm : cse->arms)
            walkCaseArm(arm, onNode);
        return;
    }

    // fix #3587: the BindExpr gap (latent — see the header; only CFG.cpp had
    // this arm before). Clauses are plain-`var` LocalDecl nodes
    // (§17.15/A.7); their `initializer` is the clause's Expr.
    if (auto* bx = dynamic_cast<const ast::BindExpr*>(e)) {
        for (const auto& cl : bx->clauses)
            if (cl) walkExpr(cl->initializer.get(), onNode);
        walkExpr(bx->body.get(), onNode);
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

static void walkPattern(const ast::Pattern* p, SubExprFn onNode) {
    if (!p) return;
    if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(p)) {
        for (const auto& c : cp->constants) walkExpr(c.get(), onNode);
        return;
    }
    if (auto* rp = dynamic_cast<const ast::RangePattern*>(p)) {
        walkExpr(rp->rangeExpr.get(), onNode);
        return;
    }
    if (auto* rop = dynamic_cast<const ast::RelOpPattern*>(p)) {
        walkExpr(rop->expr.get(), onNode);
        return;
    }
    if (auto* ap = dynamic_cast<const ast::AltPattern*>(p)) {
        for (const auto& m : ap->members)
            walkPattern(m.get(), onNode);
        return;
    }
    if (auto* dp = dynamic_cast<const ast::DestructuringPattern*>(p)) {
        for (const auto& sub : dp->subPatterns)
            walkPattern(sub.pattern.get(), onNode);
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

static void walkCaseArm(const ast::CaseArm& arm, SubExprFn onNode) {
    walkPattern(arm.pattern.get(), onNode);
    walkExpr(arm.guard.get(), onNode);
    if (arm.bodyKind == ast::CaseArmBodyKind::Expression)
        walkExpr(arm.exprBody.get(), onNode);
    // Block-form body: `arm.stmts` are Stmt-level, out of this Expr-level
    // walk's scope (see the header).
}

// Adapts an await callback to the node walk: an explicit `await` is a
// UnaryExpr whose `op` is "await". Pre-order, so an outer `await` is reported
// before the awaits inside its operand, as before.
static std::function<void(const ast::Expr&)> awaitFilter(AwaitSiteFn onAwait) {
    return [&onAwait](const ast::Expr& n) {
        if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&n))
            if (ue->op == "await") onAwait(ue);
    };
}

void forEachAwaitSite(const ast::Expr* e, AwaitSiteFn onAwait) {
    walkExpr(e, awaitFilter(onAwait));
}

void forEachAwaitSiteInPattern(const ast::Pattern* p, AwaitSiteFn onAwait) {
    walkPattern(p, awaitFilter(onAwait));
}

void forEachAwaitSiteInCaseArm(const ast::CaseArm& arm, AwaitSiteFn onAwait) {
    walkCaseArm(arm, awaitFilter(onAwait));
}

void forEachSyncSubExpr(const ast::Expr* e, SubExprFn onNode) {
    walkExpr(e, onNode);
}

}  // namespace bezel::common
