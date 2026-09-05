#pragma once

// §15.1 / §22.12: compile-time integer constant folding shared by the passes
// that need to evaluate an integer-valued constant expression — inline-array
// sizes (`[N]T`, type resolution) being the first consumer. Kept as a free
// function taking an identifier-resolution callback so each caller supplies its
// own name lookup (member consts on the enclosing type, locals in the lexical
// scope, …) while the structural arithmetic logic lives in exactly one place.

#include <functional>
#include <optional>

#include "ast/Node.hpp"
#include "sema/TypeRef.hpp"

namespace bezel::sema {

// Fold `e` to a compile-time `long long`. Admits integer literals, unary
// `+ - ~`, binary arithmetic/bitwise (`+ - * / % << >> & | ^`), and bare
// identifiers — the latter resolved through `resolveIdent(name, depth)`, which
// returns the identifier's integer value or nullopt when the name is not an
// integer-valued compile-time constant. `resolveIdent` should thread `depth`
// back into a nested foldIntConst call when it recurses into a const's
// initializer, so the shared depth guard bounds cyclic const references.
// Returns nullopt for any expression that is not a foldable integer constant.
std::optional<long long> foldIntConst(
    const ast::Expr& e,
    const std::function<std::optional<long long>(std::string_view, int)>&
        resolveIdent,
    int depth = 0);

// §17.14 (v0.184, #1688): does the compile-time-constant value `v` fit the
// representable range of integer-family primitive type `p`? Shared by every
// const evaluation-fault check (class-level and local `const` alike) so a
// folded value that overflows its declared/inferred type — `const B :=
// 2147483647 + 1;` folds to 2147483648 in `foldIntConst`'s unchecked 64-bit
// arithmetic, which does not fit `int` — is reported the same way everywhere.
// `p` must be one of the eight integer-family PrimitiveTypes; Long/ULong
// admit any 64-bit pattern (the literal/fold path already bounds them to 64
// bits), so they always fit.
bool fitsConstIntRange(long long v, PrimitiveType p);

// §17.14 (v0.184, #1688): does `e` contain a `/` or `mod` node whose RHS
// folds (via `resolveIdent`) to a compile-time zero? Called when
// `foldIntConst` on the whole expression returns nullopt, to distinguish
// "division/modulo by zero" (a specific evaluation fault, §17.14 — a compile
// error at the constant expression, never deferred to runtime) from "not
// structurally a constant expression" (a different diagnostic, e.g.
// unresolved names). `resolveIdent` should be the SAME resolver passed to
// the sibling `foldIntConst` call so both walks see consistent const values.
bool containsConstDivByZero(
    const ast::Expr& e,
    const std::function<std::optional<long long>(std::string_view, int)>&
        resolveIdent,
    int depth = 0);

// per §6.5 (v0.242, #3045): the shift count's DOMAIN is `0 .. width−1`, where
// width is the bit width of the **left (shifted) operand's type** — 8 for
// `byte`/`sbyte`, 16 for `short`/`ushort`, 32 for `int`/`uint`, 64 for
// `long`/`ulong`. The left operand's SOURCE type is the authority: §6.5 keeps
// the count out of §6.2's promotion ("a narrower left operand is *not* widened
// to match a wider count (`byte << int` yields `byte`, not `int`)"), so
// `byte << k` has domain `0..7` no matter how wide `k` is.
// Returns 0 for a non-integer primitive (`bool`, `char`, the floats,
// `decimal`, `string`, `Object`) — §6.5 rejects those operands on their own
// kind, so there is no domain to check and callers skip.
int shiftCountDomainWidth(PrimitiveType p);

// per §6.5 / §17.14 (v0.242, #3045): a constant shift count outside the §6.5
// domain is an evaluation FAULT — "a compile error at the constant
// expression", exactly like a constant `5 / 0` — so `foldIntConst` declines to
// fold such a node (it has no value). This is the companion probe, the sibling
// of `containsConstDivByZero`: called when `foldIntConst` returns nullopt, it
// finds the offending shift so the caller can report the specific cause
// instead of silently treating the expression as "not constant".
// `resolveIdent` should be the SAME resolver passed to the sibling
// `foldIntConst` call so both walks see consistent const values.
struct ConstShiftDomainFault {
    bool found = false;
    bool leftShift = true;   // `<<` when true, `>>` when false
    long long count = 0;     // the folded, out-of-domain count
    int width = 0;           // the left operand's bit width (the domain bound)
};
ConstShiftDomainFault findConstShiftDomainFault(
    const ast::Expr& e,
    const std::function<std::optional<long long>(std::string_view, int)>&
        resolveIdent,
    int depth = 0);

// fix #3879 (§22.12, v0.294): is `rt` an admissible declared type for a
// class-level (or enum-body) `const` (grammar A.4 `ConstDecl`)? §22.12's
// `const` paragraph after rule 2's tables requires "a primitive value type or
// `string`", and rule 2's own verdict table is stricter still on the
// primitive-or-enum row: "non-nullable primitive: ok; enum and both nullable
// forms: —" (the `string`/`string?` row states the identical non-
// nullable-ok/nullable-reject split for `string`). So the admissible set is
// exactly: `kind == Primitive`, excluding `Object` (§8's Object class is not
// itself a primitive value type; the primitive-value-type enumeration that
// excludes it is §3.1), excluding EITHER nullable form.
//
// Shared by checkTypeConsts's explicit-type-annotation check
// (TypeResolution_Checks.cpp) and ExprTypecheck's inferred-type mirror (fix
// #3856, which deliberately copied checkTypeConsts's two-clause test verbatim
// so the two admission points could not drift) — before this fix each site
// independently spelled `kind == Primitive && primitive != Object` and BOTH
// omitted the `nullable` clause the table states, so `int? / string? / <a
// nullable enum, already excluded by kind alone>` were silently accepted at
// both the explicit (`const A: int? := 1;`) and inferred (`const B := (if …
// then 1 else null);`) spellings. Routing both sites through this one
// function keeps that "one rule, two admission points" property intact going
// forward: a future change to the rule only needs to change here.
bool isAdmissibleClassConstType(const TypeRef& rt);

}  // namespace bezel::sema
