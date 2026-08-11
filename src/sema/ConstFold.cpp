#include "sema/ConstFold.hpp"

#include <stdexcept>
#include <string>

#include "common/IntLiteral.hpp"

namespace bezel::sema {

// per §6.5 (#1210): for shift operators the count is masked by the LEFT
// operand's bit width before the shift — matching the runtime behaviour that
// §6.5 mandates and that codegen already enforces (#1094). This helper infers
// the bit width purely from the AST shape, without a type-checker context:
//   • an untyped integer literal is `int` (32-bit) per §2;
//   • unary +/-/~ on a known-width sub-expression inherits that width;
//   • a binary arithmetic/bitwise expression whose BOTH sides have the same
//     known width inherits that width (Bezel has no implicit integer widening);
//   • any other form (identifier naming a const, cast, …) returns 0, meaning
//     "unknown" — the caller leaves those shifts unmasked.
// Only 32 is returned for normal literal-arithmetic; 8/16/64 require type
// information that is not available at this level.
static int inferIntWidthBitsForShift(const ast::Expr& e) {
    if (dynamic_cast<const ast::IntLiteralExpr*>(&e)) return 32;
    if (auto* u = dynamic_cast<const ast::UnaryExpr*>(&e))
        return u->operand ? inferIntWidthBitsForShift(*u->operand) : 0;
    if (auto* b = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        // Both operands must agree on a known width; any unknown → 0.
        int lw = b->left  ? inferIntWidthBitsForShift(*b->left)  : 0;
        int rw = b->right ? inferIntWidthBitsForShift(*b->right) : 0;
        return (lw != 0 && rw != 0) ? lw : 0;
    }
    // IdentifierExpr or any other form: width needs a type-checker pass.
    return 0;
}

std::optional<long long> foldIntConst(
    const ast::Expr& e,
    const std::function<std::optional<long long>(std::string_view, int)>&
        resolveIdent,
    int depth) {
    if (depth > 64) return std::nullopt;  // cyclic / runaway const reference

    // Integer literal — the leaf case.
    // fix #2936 (grammar A.8 / §4.2 / §15.1, v0.240): decode the lexeme with
    // the ONE literal parser (common::parseIntLiteral), which codegen has
    // always used.
    //
    // This arm used to be `std::stoll(text)` — a BASE-10 parse. Grammar A.8
    // admits `0x`/`0b` bases and `_` digit separators in every base, and
    // base-10 `stoll` handles none of them. Worse, it does not FAIL on them: it
    // reads the leading `0` of `0x60` and stops at the `x`, returning 0 with no
    // exception, so neither `catch` below could fire and the caller received a
    // confidently wrong constant. Measured through §15.1 inline-array sizes:
    // `[0x5]int` and `[0b101]int` folded to 0 (a loud false reject — "an
    // inline-array size must be greater than 0"), while `[5_0]int` folded to 5
    // and `[0x3 + 2]int` folded to 2 — SILENTLY wrong array sizes.
    //
    // `parseIntLiteral` also subsumes the #749 `ulong` case the old `catch
    // (out_of_range)` arm existed for: it parses the bare magnitude with
    // `strtoull`, so the whole [0, 2^64) range keeps its bit pattern and there
    // is nothing left to fall back FROM. A leaf literal is always non-negative
    // (a leading `-` is a UnaryExpr), and the out-of-range guard lives in
    // sema's literal type-check (§5), not here.
    if (auto* il = dynamic_cast<const ast::IntLiteralExpr*>(&e))
        return static_cast<long long>(common::parseIntLiteral(il->text));

    // Unary `+ - ~`. (Source parentheses are not their own AST node — the tree
    // shape already encodes grouping.)
    if (auto* u = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        // per §17.14: only a unary op applied to a constant OPERAND is itself
        // a constant expression. A parser error-recovery AST can leave
        // `operand` null (e.g. `const K: int := -;` recovers to
        // `UnaryExpr{op:"-", operand:null}`, #1631) — mirror
        // Resolver::resolveTypesInExpr's `if (ue->operand)` guard so the
        // absent operand declines to fold instead of dereferencing null.
        if (!u->operand) return std::nullopt;
        auto v = foldIntConst(*u->operand, resolveIdent, depth + 1);
        if (!v) return std::nullopt;
        if (u->op == "-") return -*v;
        if (u->op == "+") return *v;
        if (u->op == "~") return ~*v;
        return std::nullopt;
    }

    // Binary arithmetic / bitwise (`[BufferSize * 2]byte`, §15.1).
    if (auto* b = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        // per §17.14: both operands must themselves be constant expressions.
        // A parser error-recovery AST can leave `left` or `right` null (e.g.
        // `const K: int := 1 + ;` recovers to a `BinaryExpr` with a null
        // `right`; `const K: int := * 1;` — `*` is not a valid expression
        // start, so the unary parse returns null without consuming it and
        // the binary loop then reads that unconsumed `*` as the operator,
        // leaving a null `left` — #1631). Mirror
        // Resolver::resolveTypesInExpr's `if (be->left)` / `if (be->right)`
        // guards so a null operand declines to fold instead of dereferencing
        // null.
        if (!b->left || !b->right) return std::nullopt;
        auto l = foldIntConst(*b->left, resolveIdent, depth + 1);
        auto r = foldIntConst(*b->right, resolveIdent, depth + 1);
        if (!l || !r) return std::nullopt;
        if (b->op == "+") return *l + *r;
        if (b->op == "-") return *l - *r;
        if (b->op == "*") return *l * *r;
        if (b->op == "/")
            return *r == 0 ? std::nullopt : std::optional<long long>(*l / *r);
        // §2 / §22.12 (#1211): `mod` is Bezel's modulo keyword; it maps to
        // the same C++ `%` operation with the same zero-divisor guard.
        if (b->op == "%" || b->op == "mod")
            return *r == 0 ? std::nullopt : std::optional<long long>(*l % *r);
        // §6.5 (#1210): mask the shift count by the left operand's bit width
        // so the const-fold matches the runtime value produced by the §6.5
        // rule (and the codegen masking added in #1094). An unmasked
        // `*l << *r` with r >= 64 is UB in C++, and with r >= 32 (for int-
        // typed left operands) diverges from the §6.5 value — most critically
        // producing a giant arraySizeValue for `[1 << 40]int` (SIGSEGV).
        // Width inference returns 0 when the left expr type is unavailable
        // (e.g. a named-const identifier); those shifts remain unmasked here
        // (the same as before this fix) — acceptable for the rare case and
        // noted in the helper's comment above.
        if (b->op == "<<" || b->op == ">>") {
            int leftBits = inferIntWidthBitsForShift(*b->left);
            long long cnt = (leftBits > 0) ? (*r & (leftBits - 1)) : *r;
            // Guard against C++ UB: a shift by >= 64 on a long long is UB
            // regardless of the spec rule; clamp to a no-op shift when the
            // left-operand width is unknown and the count is out of range.
            if (cnt < 0 || cnt >= 64) return std::nullopt;
            if (b->op == "<<") return *l << cnt;
            return *l >> cnt;
        }
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

// per §17.14 (v0.184, #1688): shared range table for every integer-family
// PrimitiveType — moved here from TypeResolution_Checks.cpp's checkTypeConsts
// (originally #958) so class-level AND local `const` overflow checks agree on
// exactly one definition of "fits". Long/ULong admit any 64-bit pattern: the
// literal/fold path already bounds a folded value to 64 bits, so there is no
// narrower range to check.
bool fitsConstIntRange(long long v, PrimitiveType p) {
    using P = PrimitiveType;
    switch (p) {
        case P::Byte:   return v >= 0 && v <= 255;
        case P::SByte:  return v >= -128 && v <= 127;
        case P::Short:  return v >= -32768 && v <= 32767;
        case P::UShort: return v >= 0 && v <= 65535;
        case P::Int:    return v >= -2147483648LL && v <= 2147483647LL;
        case P::UInt:   return v >= 0 && v <= 4294967295LL;
        default:        return true;  // Long / ULong: any 64-bit pattern fits
    }
}

// per §17.14 (v0.184, #1688): moved here from TypeResolution_Checks.cpp's
// file-local containsConstDivByZero (originally #1211) so the same walk backs
// both the class-level and the local `const` div/mod-by-zero diagnostic.
// Only meaningful once the caller has confirmed `e` is structurally a
// constant expression — this walk is purely a division/modulo-by-zero probe,
// not a general "is this constant" check.
bool containsConstDivByZero(
    const ast::Expr& e,
    const std::function<std::optional<long long>(std::string_view, int)>&
        resolveIdent,
    int depth) {
    if (depth > 64) return false;  // cyclic / runaway const reference
    if (auto* b = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if (!b->left || !b->right) return false;
        if (b->op == "/" || b->op == "mod") {
            auto r = foldIntConst(*b->right, resolveIdent, depth + 1);
            if (r && *r == 0) return true;
        }
        return containsConstDivByZero(*b->left, resolveIdent, depth + 1) ||
               containsConstDivByZero(*b->right, resolveIdent, depth + 1);
    }
    if (auto* u = dynamic_cast<const ast::UnaryExpr*>(&e))
        return u->operand &&
               containsConstDivByZero(*u->operand, resolveIdent, depth + 1);
    return false;
}

}  // namespace bezel::sema
