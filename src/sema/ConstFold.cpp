#include "sema/ConstFold.hpp"

#include <string>

namespace bezel::sema {

std::optional<long long> foldIntConst(
    const ast::Expr& e,
    const std::function<std::optional<long long>(std::string_view, int)>&
        resolveIdent,
    int depth) {
    if (depth > 64) return std::nullopt;  // cyclic / runaway const reference

    // Integer literal — the leaf case.
    if (auto* il = dynamic_cast<const ast::IntLiteralExpr*>(&e)) {
        try {
            return std::stoll(std::string(il->text));
        } catch (...) {
            return std::nullopt;  // out of `long long` range
        }
    }

    // Unary `+ - ~`. (Source parentheses are not their own AST node — the tree
    // shape already encodes grouping.)
    if (auto* u = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        auto v = foldIntConst(*u->operand, resolveIdent, depth + 1);
        if (!v) return std::nullopt;
        if (u->op == "-") return -*v;
        if (u->op == "+") return *v;
        if (u->op == "~") return ~*v;
        return std::nullopt;
    }

    // Binary arithmetic / bitwise (`[BufferSize * 2]byte`, §15.1).
    if (auto* b = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        auto l = foldIntConst(*b->left, resolveIdent, depth + 1);
        auto r = foldIntConst(*b->right, resolveIdent, depth + 1);
        if (!l || !r) return std::nullopt;
        if (b->op == "+") return *l + *r;
        if (b->op == "-") return *l - *r;
        if (b->op == "*") return *l * *r;
        if (b->op == "/")
            return *r == 0 ? std::nullopt : std::optional<long long>(*l / *r);
        if (b->op == "%")
            return *r == 0 ? std::nullopt : std::optional<long long>(*l % *r);
        if (b->op == "<<") return *l << *r;
        if (b->op == ">>") return *l >> *r;
        if (b->op == "&") return *l & *r;
        if (b->op == "|") return *l | *r;
        if (b->op == "^") return *l ^ *r;
        return std::nullopt;
    }

    // A bare identifier naming a `const`. The caller's resolver maps it to a
    // value (and threads `depth` back through foldIntConst when the const's
    // initializer is itself an expression).
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        return resolveIdent(id->name, depth);
    }

    return std::nullopt;
}

}  // namespace bezel::sema
