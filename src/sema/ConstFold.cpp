#include "sema/ConstFold.hpp"

#include <stdexcept>
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
        const std::string text(il->text);
        try {
            return std::stoll(text);
        } catch (const std::out_of_range&) {
            // §10 (#749): a `ulong` constant in [2^63, 2^64) overflows signed
            // `long long` but fits `unsigned long long`. Preserve its bit
            // pattern as the (negative) signed value — the i64 materialization
            // (LlvmBackend::enumConstant casts back through uint64_t) then reads
            // the correct unsigned value instead of falling back to 0. A leaf
            // literal is always non-negative (a leading `-` is a UnaryExpr),
            // so `stoull` never sees a sign and never silently wraps.
            try {
                return static_cast<long long>(std::stoull(text));
            } catch (...) {
                return std::nullopt;  // genuinely beyond the 64-bit range
            }
        } catch (...) {
            return std::nullopt;  // not a valid integer literal
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
