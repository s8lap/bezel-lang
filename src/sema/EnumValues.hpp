#pragma once

// §10.2 / §5.4 (v0.242): the declared underlying values of an enum type.
//
// Several rules need the enum's *declared value set* rather than just its
// constant names:
//
//   * §5.4's checked cast — `E(n)` succeeds exactly when `n` equals the
//     underlying value of some declared constant of `E`;
//   * §17.11's `default(E)` rule — an enum with no declared constant of
//     underlying value 0 has no well-defined zero;
//   * §19.6/§10.2's exhaustiveness and duplicate-arm analysis, which counts
//     coverage by *value* (two same-valued constants denote one value).
//
// The values are re-derived here rather than read off EnumConstantSymbol::value
// because that field is populated only for an IMPORTED enum (it is the
// cross-module carrier, Symbol.hpp); a LOCAL enum's values live in its AST and
// follow §10.2's explicit-value + auto-increment-continuation rule.

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "ast/Node.hpp"
#include "sema/ConstFold.hpp"
#include "sema/Symbol.hpp"

namespace bezel::sema {

// §10.2: one enum's declared constants, in declaration order, with their
// underlying values.
struct EnumDeclaredValues {
    std::vector<std::pair<std::string_view, long long>> constants;
    // False when at least one constant carried an explicit initializer that
    // did NOT fold to a compile-time integer — the value set is then only
    // partially known and no rule may reject on the strength of it (the
    // non-constant initializer has its own diagnostic). Callers that decide
    // *rejection* must bail out when this is false.
    bool complete = true;

    bool contains(long long v) const {
        for (const auto& c : constants)
            if (c.second == v) return true;
        return false;
    }
    // §17.11: does the enum declare a constant of underlying value 0?
    bool hasZero() const { return contains(0); }
    bool empty() const { return constants.empty(); }
};

// §10.2: the declared constants of `e` with their underlying values. A LOCAL
// enum is read from its AST (explicit `:= N` initializers, folded through
// `foldIntConst` with sibling-constant resolution, and auto-increment
// continuation from the previous value); an IMPORTED enum has no AST TypeDecl,
// so its reconstructed EnumConstantSymbol::value carries the answer.
inline EnumDeclaredValues enumDeclaredValues(const TypeSymbol& e) {
    EnumDeclaredValues out;
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(e.decl)) {
        long long running = 0;
        for (const auto& ec : td->enumConstants) {
            long long val = running;
            if (ec.value) {
                auto resolve = [&](std::string_view name, int)
                        -> std::optional<long long> {
                    for (const auto& c : out.constants)
                        if (c.first == name) return c.second;
                    return std::nullopt;
                };
                if (auto folded = foldIntConst(*ec.value, resolve))
                    val = *folded;
                else
                    out.complete = false;  // non-constant initializer
            }
            out.constants.emplace_back(ec.name, val);
            running = val + 1;
        }
        return out;
    }
    for (const Symbol* m : e.members)
        if (auto* ec = dynamic_cast<const EnumConstantSymbol*>(m))
            out.constants.emplace_back(ec->name, ec->value);
    return out;
}

// §5.4 (v0.242): is `v` the underlying value of some declared constant of the
// enum `e`? Returns nullopt when the answer cannot be decided — the enum's
// value set is incomplete (a non-constant initializer, already diagnosed) or it
// declares no constants at all — so a caller never rejects on a guess.
inline std::optional<bool> enumDeclaresValue(const TypeSymbol& e, long long v) {
    EnumDeclaredValues vals = enumDeclaredValues(e);
    if (!vals.complete || vals.empty()) return std::nullopt;
    return vals.contains(v);
}

// §17.11 (v0.242): does `e` declare a constant of underlying value 0 — i.e. is
// `default(E)` well-formed for it? Undecidable (nullopt) under the same two
// conditions as `enumDeclaresValue`.
inline std::optional<bool> enumHasZeroConstant(const TypeSymbol& e) {
    return enumDeclaresValue(e, 0);
}

}  // namespace bezel::sema
