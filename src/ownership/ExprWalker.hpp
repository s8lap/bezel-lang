#pragma once

// ExprWalker — a generic DEEP walk over every expression and statement of a
// statement tree: lambda bodies (expression and block form), `case` arms and
// binding atoms included.
//
// Moved verbatim out of Ownership_FieldWriteUse.cpp (fix #7976's two
// syntactic pre-scans) by the #8767 fix-forward, whose getter-body escape
// screen (`MethodOwnership::getterBorrowMayEscape`, Ownership.cpp) needs the
// same complete expression grammar: a screen that does not descend into a
// lambda body misses a store or a call made there (r42-rev-s67 probes s20,
// s22). One copy of the grammar, so the next expression kind is a one-site
// change, as `forEachNestedStmt` (StmtWalker.hpp) is for statements.
//
// `forEachChild(e, fe, fs)` calls `fe` on each direct sub-expression of `e`
// and `fs` on each direct sub-statement (a block lambda's body, a `case`
// arm's statements, a binding atom's clauses). `walkStmtDeep(s, onExpr,
// onStmt)` / `walkExprDeep(e, onExpr, onStmt)` call `onStmt` on every
// statement and `onExpr` on every expression reachable from the root, in
// pre-order.

#include <memory>
#include <vector>

#include "ast/Node.hpp"

namespace bezel::ownership {

template <class FE, class FS>
void forEachChild(const ast::Expr& e, FE&& fe, FS&& fs) {
    auto args = [&](const std::vector<ast::CallArgument>& as) {
        for (const auto& a : as)
            if (a.value) fe(*a.value);
    };
    auto one = [&](const std::unique_ptr<ast::Expr>& p) {
        if (p) fe(*p);
    };
    if (auto* x = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        one(x->target);
    } else if (auto* x = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        one(x->target);
    } else if (auto* x = dynamic_cast<const ast::CallExpr*>(&e)) {
        one(x->callee);
        args(x->args);
    } else if (auto* x = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
        one(x->callee);
        args(x->args);
    } else if (auto* x = dynamic_cast<const ast::IndexExpr*>(&e)) {
        one(x->target);
        args(x->indices);
    } else if (auto* x = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        one(x->operand);
    } else if (auto* x = dynamic_cast<const ast::NewExpr*>(&e)) {
        args(x->args);
    } else if (auto* x = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        one(x->left);
        one(x->right);
    } else if (auto* x = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        one(x->operand);
        one(x->replacement);
    } else if (auto* x = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
        one(x->operand);
    } else if (auto* x = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        one(x->condition);
        one(x->thenValue);
        one(x->elseValue);
    } else if (auto* x = dynamic_cast<const ast::ThrowExpr*>(&e)) {
        one(x->operand);
    } else if (auto* x = dynamic_cast<const ast::RangeExpr*>(&e)) {
        one(x->lo);
        one(x->hi);
        one(x->step);
    } else if (auto* x = dynamic_cast<const ast::WithExpr*>(&e)) {
        one(x->source);
        for (const auto& a : x->assigns) one(a.value);
    } else if (auto* x = dynamic_cast<const ast::CollectionLiteralExpr*>(&e)) {
        for (const auto& k : x->keys) one(k);
        for (const auto& v : x->elements) one(v);
    } else if (auto* x = dynamic_cast<const ast::InterpolatedStringExpr*>(&e)) {
        for (const auto& p : x->parts) one(p.expr);
    } else if (auto* x = dynamic_cast<const ast::LambdaExpr*>(&e)) {
        one(x->exprBody);
        for (const auto& s : x->stmts)
            if (s) fs(*s);
    } else if (auto* x = dynamic_cast<const ast::CaseExpr*>(&e)) {
        one(x->scrutinee);
        for (const auto& arm : x->arms) {
            one(arm.guard);
            one(arm.exprBody);
            for (const auto& s : arm.stmts)
                if (s) fs(*s);
        }
    } else if (auto* x = dynamic_cast<const ast::BindExpr*>(&e)) {
        for (const auto& c : x->clauses)
            if (c) fs(*c);
        one(x->body);
    }
    // Leaves (identifiers, literals, `nameof`, `typeof`, `default`, the unit
    // expression, a marked transfer) have no sub-expression.
}

template <class FE, class FA>
void walkExprDeep(const ast::Expr& e, FE& onExpr, FA& onStmt);

template <class FE, class FA>
void walkStmtDeep(const ast::Stmt& s, FE& onExpr, FA& onStmt) {
    onStmt(s);
    auto ex = [&](const std::unique_ptr<ast::Expr>& p) {
        if (p) walkExprDeep(*p, onExpr, onStmt);
    };
    auto block = [&](const std::vector<std::unique_ptr<ast::Stmt>>& b) {
        for (const auto& c : b)
            if (c) walkStmtDeep(*c, onExpr, onStmt);
    };
    if (auto* x = dynamic_cast<const ast::LocalDecl*>(&s)) {
        ex(x->initializer);
    } else if (auto* x = dynamic_cast<const ast::LocalConstDecl*>(&s)) {
        ex(x->value);
    } else if (auto* x = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        ex(x->expr);
    } else if (auto* x = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        ex(x->target);
        ex(x->value);
    } else if (auto* x = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        ex(x->value);
    } else if (auto* x = dynamic_cast<const ast::ThrowStmt*>(&s)) {
        ex(x->value);
    } else if (auto* x = dynamic_cast<const ast::YieldStmt*>(&s)) {
        ex(x->value);
    } else if (auto* x = dynamic_cast<const ast::DiscardStmt*>(&s)) {
        ex(x->value);
    } else if (auto* x = dynamic_cast<const ast::IncrementStmt*>(&s)) {
        ex(x->target);
    } else if (auto* x = dynamic_cast<const ast::IfStmt*>(&s)) {
        ex(x->ifBranch.condition);
        block(x->ifBranch.body);
        for (const auto& el : x->elifBranches) {
            ex(el.condition);
            block(el.body);
        }
        block(x->elseBody);
    } else if (auto* x = dynamic_cast<const ast::WhileStmt*>(&s)) {
        ex(x->condition);
        block(x->body);
    } else if (auto* x = dynamic_cast<const ast::ForStmt*>(&s)) {
        ex(x->iterable);
        ex(x->withExpr);
        block(x->body);
    } else if (auto* x = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        block(x->body);
    } else if (auto* x = dynamic_cast<const ast::TryStmt*>(&s)) {
        block(x->body);
        for (const auto& arm : x->catches) {
            ex(arm.guard);
            if (arm.body) walkStmtDeep(*arm.body, onExpr, onStmt);
        }
        block(x->finallyBody);
    } else if (auto* x = dynamic_cast<const ast::CaseStmt*>(&s)) {
        ex(x->scrutinee);
        for (const auto& arm : x->arms) {
            ex(arm.guard);
            ex(arm.exprBody);
            block(arm.stmts);
        }
    }
}

template <class FE, class FA>
void walkExprDeep(const ast::Expr& e, FE& onExpr, FA& onStmt) {
    onExpr(e);
    forEachChild(
        e, [&](const ast::Expr& c) { walkExprDeep(c, onExpr, onStmt); },
        [&](const ast::Stmt& s) { walkStmtDeep(s, onExpr, onStmt); });
}

} // namespace bezel::ownership
