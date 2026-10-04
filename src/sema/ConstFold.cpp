#include "sema/ConstFold.hpp"

#include <stdexcept>
#include <string>

#include "common/IntLiteral.hpp"

namespace bezel::sema {

// per §6.5 (#1210; domain rule since v0.242, #3045; result truncation since
// v0.247, #3078): the shift count's domain is bounded by the LEFT operand's
// bit width. This helper infers that width purely from the AST shape, without
// a type-checker context:
//   • an integer literal is `int` (32-bit) by default per §2, and `long`
//     (64-bit) when suffixed with `L` or `l` (with or without a preceding `u`/
//     `U`);
//   • unary +/-/~ on a known-width sub-expression inherits that width;
//   • a binary arithmetic/bitwise expression whose BOTH sides have the same
//     known width inherits that width (Bezel has no implicit integer widening);
//   • any other form (identifier naming a const, cast, …) returns 0, meaning
//     "unknown" — the caller cannot decide the domain for those here, and the
//     typed check in ExprTypecheck (which does know the operand's type) covers
//     them.
// Only 32/64 are returned for literal-arithmetic; 8/16 require type
// information that is not available at this level.
static int inferIntWidthBitsForShift(const ast::Expr& e) {
    if (auto* il = dynamic_cast<const ast::IntLiteralExpr*>(&e)) {
        // fix #3078 (§6.5 / §17.14, v0.247): an `L`/`l` literal suffix widens
        // the literal to `long` (64-bit), so `1L << 63` has domain `0..63`
        // (the long literal's source width), not `0..31`. Without this the
        // count was checked against 32 and the in-domain `1L << 63` folded
        // to nullopt, indistinguishable from the genuine fault `1L << 64`.
        std::string_view text = il->text;
        if (!text.empty() && (text.back() == 'L' || text.back() == 'l'))
            return 64;
        return 32;
    }
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

// Sibling of inferIntWidthBitsForShift for the LEFT operand's SIGNEDNESS —
// drives the post-fold result truncation in fix #3078. Returns true for a
// signed integer literal (the default per §2: `int`/`long`), false for an
// unsigned literal (any `u`/`U` suffix). Same AST-shape-only discipline as
// the width helper: any non-literal form returns true (the common case;
// the typed check uses the actual type).
static bool inferIntSignednessForShift(const ast::Expr& e) {
    if (auto* il = dynamic_cast<const ast::IntLiteralExpr*>(&e)) {
        std::string_view text = il->text;
        // Strip the optional L/l suffix first, so `1uL` reads as `1u`.
        if (!text.empty() && (text.back() == 'L' || text.back() == 'l'))
            text.remove_suffix(1);
        // A trailing `u`/`U` marks the literal as unsigned.
        if (!text.empty() && (text.back() == 'u' || text.back() == 'U'))
            return false;
        return true;
    }
    if (auto* u = dynamic_cast<const ast::UnaryExpr*>(&e))
        return u->operand ? inferIntSignednessForShift(*u->operand) : true;
    return true;  // unknown — default signed (matches the `int` literal rule)
}

// fix #3078 (§6.5 / §17.14, v0.247): a shift's RESULT is truncated to the
// LEFT operand's bit width, with sign extension for signed types — the
// per-§6.5 "bits shifted beyond the left operand's width are discarded
// silently" rule, applied to the `long long` fold so the destination-type
// range check sees a value that IS representable in the destination (e.g.
// `1 << 31` → -2147483648 on `int`, not the unrepresentable `2147483648`
// from the unchecked 64-bit shift). `bits` is the LEFT operand's width
// (8/16/32/64); `signedLeft` is the LEFT operand's signedness. Returns
// `raw` unchanged when `bits >= 64` (the natural `long long` width) or
// `bits <= 0` (unknown).
static long long truncateShiftFoldToLeftWidth(
    long long raw, int bits, bool signedLeft) {
    if (bits <= 0 || bits >= 64) return raw;
    long long mask = (1LL << bits) - 1;
    long long truncated = raw & mask;
    if (signedLeft && (truncated & (1LL << (bits - 1)))) {
        // Re-extend the high bit as a sign, so the value fits `long long`
        // the same way it would fit the destination signed type.
        truncated -= (1LL << bits);
    }
    return truncated;
}

std::optional<long long> foldIntConst(
    const ast::Expr& e,
    const std::function<std::optional<long long>(std::string_view, int)>&
        resolveIdent,
    int depth,
    std::function<std::optional<long long>(
        const ast::Expr& base, std::string_view member, bool& errorSeen)>
        resolveMemberAccess) {
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
        auto v = foldIntConst(*u->operand, resolveIdent, depth + 1,
                              resolveMemberAccess);
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
        auto l = foldIntConst(*b->left, resolveIdent, depth + 1,
                              resolveMemberAccess);
        auto r = foldIntConst(*b->right, resolveIdent, depth + 1,
                              resolveMemberAccess);
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
        // §6.5 / §17.14 (v0.242, #3045): a shift count outside the left
        // operand's `0 .. width−1` domain is an evaluation FAULT — §17.14:
        // "An evaluation fault — integer or `decimal` overflow, division or
        // `mod` by zero, a shift count outside the §6.5 domain — is a compile
        // error at the constant expression, never a deferred runtime fault:
        // … and `const F := 1 << 64;` are rejected where they are written."
        // A faulting expression has NO value, so decline to fold; the caller
        // reports the specific cause via findConstShiftDomainFault (below).
        //
        // Before v0.242 this MASKED the count (`*r & (leftBits - 1)`), because
        // §6.5 then read "the shift count is reduced modulo the operand's bit
        // width" — the silent-masking rule proposal 0079 retired. `1 << 40`
        // folded to `1 << 8` = 256; it is now a compile error.
        //
        // Width inference returns 0 when the left expr's type is unavailable
        // (e.g. a named-const identifier); the §6.5 domain cannot be decided
        // here for those, and the typed check in ExprTypecheck covers them.
        // The C++-UB guard stays either way: a shift by >= 64 (or negative) on
        // a `long long` is UB in the host language regardless of the spec.
        if (b->op == "<<" || b->op == ">>") {
            int leftBits = inferIntWidthBitsForShift(*b->left);
            if (leftBits > 0 && (*r < 0 || *r >= leftBits)) return std::nullopt;
            if (*r < 0 || *r >= 64) return std::nullopt;
            // fix #3078 (§6.5 / §17.14, v0.247): the fold computes in `long
            // long`, but §6.5 says a shift's RESULT is truncated to the LEFT
            // operand's width. Without this re-truncation `1 << 31` folds to
            // 2147483648 (which exceeds `Int`'s max), so the destination
            // range check at the const declaration reports the representable
            // value -2147483648 as a non-fit. Truncate here so the fold's
            // result is what the destination type will actually hold.
            long long raw = (b->op == "<<") ? (*l << *r) : (*l >> *r);
            return truncateShiftFoldToLeftWidth(
                raw, leftBits, inferIntSignednessForShift(*b->left));
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

    // fix #5402 (§10.2, v0.315): a qualified enum-constant reference `E.A`
    // in an enum value initializer. The caller-supplied `resolveMemberAccess`
    // callback resolves the base expression to the enum type and looks up the
    // member; if the member is an earlier sibling, the callback returns its
    // underlying value. On self/forward reference it returns nullopt and sets
    // `errorSeen = true` so the caller can report the error.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (resolveMemberAccess) {
            bool errorSeen = false;
            if (auto v = resolveMemberAccess(*ma->target, ma->member, errorSeen)) {
                return v;
            }
            if (errorSeen) return std::nullopt;  // error already reported
        }
        // Not a qualifier reference (or no callback): not foldable.
        return std::nullopt;
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
            // §17.14 (v0.184, #1688): a division/modulo-by-zero in a const
            // initializer is a compile error. When `foldIntConst` returns a
            // value, check if it is zero. When it returns nullopt (the
            // divisor's value cannot be folded — e.g. a forward reference, an
            // imported const, or a non-integer const), we cannot prove the
            // divisor is non-zero at compile time, so we conservatively report
            // division-by-zero rather than silently accepting the const.
            if (!r || *r == 0) return true;
        }
        return containsConstDivByZero(*b->left, resolveIdent, depth + 1) ||
               containsConstDivByZero(*b->right, resolveIdent, depth + 1);
    }
    if (auto* u = dynamic_cast<const ast::UnaryExpr*>(&e))
        return u->operand &&
               containsConstDivByZero(*u->operand, resolveIdent, depth + 1);
    return false;
}

// per §6.5 (v0.242, #3045). The eight integer-family primitives are the only
// ones with a shift domain; every other primitive is rejected by §6.5's own
// operand-kind rule ("not valid on `bool`, `char`, or floating-point types").
int shiftCountDomainWidth(PrimitiveType p) {
    using P = PrimitiveType;
    switch (p) {
        case P::Byte:  case P::SByte:  return 8;
        case P::Short: case P::UShort: return 16;
        case P::Int:   case P::UInt:   return 32;
        case P::Long:  case P::ULong:  return 64;
        default:                       return 0;
    }
}

// per §6.5 / §17.14 (v0.242, #3045): the sibling of containsConstDivByZero for
// the shift-domain fault. Structurally identical walk — only meaningful once
// the caller has confirmed `e` is structurally a constant expression; this is
// a fault probe, not an "is this constant" check.
ConstShiftDomainFault findConstShiftDomainFault(
    const ast::Expr& e,
    const std::function<std::optional<long long>(std::string_view, int)>&
        resolveIdent,
    int depth) {
    ConstShiftDomainFault none;
    if (depth > 64) return none;  // cyclic / runaway const reference
    if (auto* b = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if (!b->left || !b->right) return none;
        if (b->op == "<<" || b->op == ">>") {
            int leftBits = inferIntWidthBitsForShift(*b->left);
            if (leftBits > 0) {
                if (auto r = foldIntConst(*b->right, resolveIdent, depth + 1)) {
                    if (*r < 0 || *r >= leftBits) {
                        ConstShiftDomainFault f;
                        f.found = true;
                        f.leftShift = (b->op == "<<");
                        f.count = *r;
                        f.width = leftBits;
                        return f;
                    }
                }
            }
        }
        auto lf = findConstShiftDomainFault(*b->left, resolveIdent, depth + 1);
        if (lf.found) return lf;
        return findConstShiftDomainFault(*b->right, resolveIdent, depth + 1);
    }
    if (auto* u = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (!u->operand) return none;
        return findConstShiftDomainFault(*u->operand, resolveIdent, depth + 1);
    }
    // A bare identifier naming a `const` whose own initializer carries the
    // fault: `const SZ: int := 1 << 40; var a: [SZ]int;` — the fold recurses
    // into the initializer, so the probe must too. `resolveIdent` returns only
    // a VALUE, so the initializer expression is not reachable from here; the
    // declaration site reports its own fault (every const initializer is
    // checked where it is written, §17.14), which is where the diagnostic
    // belongs.
    return none;
}

// fix #3879 (§22.12, v0.294): see ConstFold.hpp for the rule this states and
// why it is shared between the explicit- and inferred-type admission points.
bool isAdmissibleClassConstType(const TypeRef& rt) {
    return rt.kind == TypeRef::Kind::Primitive &&
           rt.primitive != PrimitiveType::Object && !rt.nullable;
}

}  // namespace bezel::sema
