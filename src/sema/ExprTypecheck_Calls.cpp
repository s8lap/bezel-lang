#include "sema/ExprTypecheck_impl.hpp"

#include "sema/TypeResolution.hpp"  // typeRefEqual

namespace bezel::sema {

// This translation unit holds the call-resolution and member-access
// definitions of the ExprTC worker: CallExpr return-type / arity checking
// (typeCall and friends) and the `.` / `?.` member-access resolvers.

// §5.1: implicit-conversion predicate used by §22.6 overload resolution.
// Identity is NOT a §5.1 conversion (it is the "exact" relation), so a
// pair of equal types returns false here. Conservative on everything the
// type lattice can't yet judge, matching checkAssignable's stance.
bool ExprTC::implicitlyConvertible(const TypeRef* from, const TypeRef* to) {
    if (!from || !to) return false;
    // `Object` is the root supertype: every type converts to it (§12), and
    // checkAssignable already treats an Object target as accepting anything.
    // Mirror that here as a (non-exact) conversion so a candidate with an
    // Object parameter is never spuriously made inapplicable. (typeRefEqual
    // has already ruled out the exact Object→Object case before we are
    // called for ranking.)
    if (to->kind == TypeRef::Kind::Primitive &&
        to->primitive == PrimitiveType::Object) {
        bool fromIsObject = from->kind == TypeRef::Kind::Primitive &&
                            from->primitive == PrimitiveType::Object;
        if (!fromIsObject) return true;
    }
    // primitive → primitive: §5.1 numeric/char widening table.
    if (from->kind == TypeRef::Kind::Primitive &&
        to->kind == TypeRef::Kind::Primitive) {
        // A nullable source cannot implicitly convert to a non-nullable
        // target (that would drop the null case) — §7.
        if (from->nullable && !to->nullable) return false;
        if (from->primitive == to->primitive) {
            // identity at the primitive level — only a conversion when the
            // source is non-nullable and the target nullable (T → T?).
            return !from->nullable && to->nullable;
        }
        return primitiveWidens(from->primitive, to->primitive);
    }
    // named → named: §12 subtype → supertype, and §7 reference → nullable.
    if (from->kind == TypeRef::Kind::Named &&
        to->kind == TypeRef::Kind::Named &&
        from->declaration && to->declaration) {
        if (from->nullable && !to->nullable) return false;
        if (from->declaration == to->declaration) {
            // identity declaration — a conversion only across nullability
            // (reference → nullable, §7).
            return !from->nullable && to->nullable;
        }
        // §22.6 ranks subtype → supertype as a conversion (not exact).
        return isSubtype(from->declaration, to->declaration);
    }
    return false;
}

// §22.6 classify how `argTy` matches `paramTy` at one argument position.
// Exact when the types are equal (Resolver::typeRefEqual); Convert when a
// §5.1 implicit conversion applies; NotApplicable otherwise (the candidate
// requiring a non-§5.1 conversion is unreachable through this argument).
ExprTC::ArgMatch ExprTC::classifyArg(const TypeRef* argTy,
                                     const TypeRef* paramTy) {
    if (!argTy || !paramTy) return ArgMatch::Exact;  // can't judge → don't filter
    if (typeRefEqual(argTy, paramTy)) return ArgMatch::Exact;
    if (implicitlyConvertible(argTy, paramTy)) return ArgMatch::Convert;
    return ArgMatch::NotApplicable;
}

// §22.6 overload resolution among same-arity, all-positional candidates.
ExprTC::OverloadResult ExprTC::resolveOverload(
    const std::vector<std::vector<const TypeRef*>>& paramTypes,
    const std::vector<const TypeRef*>& argTypes,
    const std::vector<std::string>& candNames, SourceLocation loc,
    const std::string& calleeDesc) {
    OverloadResult res;
    const std::size_t nCand = paramTypes.size();
    const std::size_t nArg = argTypes.size();

    // Undecidable guard: §22.6 ranking is reliable only for the type pairs
    // checkAssignable judges — primitive↔primitive and non-generic
    // named↔named (plus the Object root). For anything else (unresolved
    // types, generic instantiations, inline-array / function types, error
    // sentinels) we cannot classify exact vs. convert vs. inapplicable
    // without risking a spurious ambiguous/unreachable diagnostic, so we
    // fall back to arity-based selection. (Per the project's
    // conservative-typing stance — cf. checkAssignable.)
    auto judgeable = [](const TypeRef* t) {
        if (!t) return false;
        switch (t->kind) {
            case TypeRef::Kind::Primitive:
            case TypeRef::Kind::Unit:
                return true;
            case TypeRef::Kind::Named:
                // Non-generic, resolved named types only.
                return t->declaration != nullptr && t->genericArgs.empty();
            default:
                return false;  // Error, InlineArray, Function
        }
    };
    for (const TypeRef* a : argTypes)
        if (!judgeable(a)) { res.outcome = OverloadOutcome::Undecidable; return res; }
    for (const auto& pts : paramTypes) {
        if (pts.size() != nArg) {
            // arity already filtered upstream; defensively bail.
            res.outcome = OverloadOutcome::Undecidable;
            return res;
        }
        for (const TypeRef* p : pts)
            if (!judgeable(p)) { res.outcome = OverloadOutcome::Undecidable; return res; }
    }

    // Classify every (candidate, argument) cell. ArgMatch::NotApplicable in
    // any cell removes that candidate from the applicable set (§22.6 step 1).
    std::vector<std::vector<ArgMatch>> grid(nCand);
    std::vector<bool> applicable(nCand, true);
    for (std::size_t c = 0; c < nCand; ++c) {
        grid[c].resize(nArg);
        for (std::size_t i = 0; i < nArg; ++i) {
            ArgMatch m = classifyArg(argTypes[i], paramTypes[c][i]);
            grid[c][i] = m;
            if (m == ArgMatch::NotApplicable) applicable[c] = false;
        }
    }

    std::vector<std::size_t> appl;
    for (std::size_t c = 0; c < nCand; ++c)
        if (applicable[c]) appl.push_back(c);

    // §22.6 step 4: no applicable candidate → unreachable, no candidates
    // cited.
    if (appl.empty()) {
        ctx_.diagnostics().report(
            Severity::Error, loc,
            "no overload of " + calleeDesc +
                " is applicable to these arguments; every candidate would "
                "require a conversion that §5.1 does not admit as implicit "
                "(§22.6)");
        res.outcome = OverloadOutcome::Unreachable;
        return res;
    }

    if (appl.size() == 1) {
        res.outcome = OverloadOutcome::Resolved;
        res.winnerIndex = appl[0];
        return res;
    }

    // §22.6 tie-break: X is "better than" Y at a position iff X is Exact
    // there and Y is Convert. Two Converts (any pair) are equally ranked;
    // two Exacts are equal. X is "unambiguously better" iff better-or-equal
    // on every position and strictly better on at least one. The call
    // resolves to a candidate unambiguously better than every other.
    auto betterOrEqualAt = [](ArgMatch x, ArgMatch y) {
        // x is not worse than y: worse only when x converts where y is exact.
        return !(x == ArgMatch::Convert && y == ArgMatch::Exact);
    };
    auto strictlyBetterAt = [](ArgMatch x, ArgMatch y) {
        return x == ArgMatch::Exact && y == ArgMatch::Convert;
    };
    auto unambiguouslyBetter = [&](std::size_t x, std::size_t y) {
        bool anyStrict = false;
        for (std::size_t i = 0; i < nArg; ++i) {
            if (!betterOrEqualAt(grid[x][i], grid[y][i])) return false;
            if (strictlyBetterAt(grid[x][i], grid[y][i])) anyStrict = true;
        }
        return anyStrict;
    };

    for (std::size_t cx : appl) {
        bool dominatesAll = true;
        for (std::size_t cy : appl) {
            if (cx == cy) continue;
            if (!unambiguouslyBetter(cx, cy)) { dominatesAll = false; break; }
        }
        if (dominatesAll) {
            res.outcome = OverloadOutcome::Resolved;
            res.winnerIndex = cx;
            return res;
        }
    }

    // §22.6 step 3: no candidate is unambiguously better → ambiguous,
    // cite the applicable candidates.
    std::string msg = "ambiguous call to " + calleeDesc +
                      ": no overload is unambiguously better (§22.6); "
                      "candidates:";
    for (std::size_t c : appl) {
        msg += "\n  ";
        msg += (c < candNames.size() ? candNames[c] : std::string("(overload)"));
    }
    msg += "\nadd an explicit cast (§5.2) so exactly one candidate is an "
           "exact match";
    ctx_.diagnostics().report(Severity::Error, loc, msg);
    res.outcome = OverloadOutcome::Ambiguous;
    return res;
}

namespace {
// §14.1 (#785, #850): structurally unify a value parameter's declared type
// (`pt`, AST) against the call argument's resolved type (`argTy`), binding each
// method type parameter named in `paramNames` to the matching component of the
// argument's type. A bare `p: T` binds T; a generic-typed `p: Pair<A, B>`
// recurses pairwise into the generic args (binding A and B from a
// `Pair<int, string>` argument). The first binding for a name wins (a later or
// already-receiver-bound occurrence is left). Mirrors codegen's
// unifyMethodReturnTypeArg.
// §14.1 / #850: when the parameter is `T?` (nullable named type-param) and the
// argument is `int?` (or another nullable type), peel the shared `?` so that T
// is bound to `int` (non-nullable), not `int?`. This makes `where T: value`
// hold (int satisfies value, int? does not) and the return type `T` stays
// non-nullable. A bare `T` parameter (pt->nullable == false) with a nullable
// argument still binds T = int? and is correctly rejected by the constraint
// check. ctx is used to allocate a stable non-nullable TypeRef in the peel
// case. // per §14.1
// §14.1 / #896: `conflicts` (optional) receives the name of any type parameter
// whose second-occurrence inferred binding is DIFFERENT from the first-occurrence
// binding (e.g. T already bound to int, now seeing bool from a second T? arg).
// The caller emits a §14.1 compile error and aborts the call. Without this
// detection the first-binding-wins logic silently drops the conflict, and codegen
// later produces a PHINode mixing int and bool → SIGABRT.
void unifyArgSubst(
    const ast::Type* pt, TypeRef* argTy,
    const std::unordered_set<std::string_view>& paramNames,
    std::unordered_map<std::string_view, TypeRef*>& argSubst,
    SemaContext& ctx,
    std::unordered_set<std::string_view>* conflicts = nullptr) {
    if (!pt || !argTy) return;
    if (pt->kind == ast::TypeKind::Named && pt->name &&
        pt->name->parts.size() == 1 && paramNames.count(pt->name->parts[0])) {
        std::string_view nm = pt->name->parts[0];
        // Compute the would-be-bound TypeRef applying the same §14.1/#850
        // nullable-peeling rule used on the first occurrence.
        TypeRef* bound = argTy;
        if (pt->nullable && argTy->nullable) {
            if (argTy->kind == TypeRef::Kind::Primitive) {
                bound = ctx.primitiveTypeRef(argTy->primitive, /*nullable=*/false);
            } else {
                TypeRef* nn = ctx.makeTypeRef();
                *nn = *argTy;
                nn->nullable = false;
                bound = nn;
            }
        }
        if (!argSubst.count(nm)) {
            // §14.1 / #850: peel the nullable when the parameter declares `T?`
            // and the argument is also nullable — infer the non-nullable T so
            // `where T: value` and the non-nullable return type `T` work
            // correctly. A bare `T` parameter (pt->nullable == false) with a
            // nullable argument is left as-is and rejected downstream.
            argSubst[nm] = bound;
        } else if (conflicts && !typeRefEqual(argSubst[nm], bound)) {
            // §14.1 / #896: existing binding for `nm` disagrees with what this
            // argument would bind — record the conflict so the caller can emit a
            // clean compile error instead of silently accepting the inconsistency
            // and letting codegen crash (PHINode type mismatch).
            conflicts->insert(nm);
        }
        return;
    }
    if (!pt->genericArgs.empty() &&
        argTy->kind == TypeRef::Kind::Named &&
        argTy->genericArgs.size() == pt->genericArgs.size()) {
        for (std::size_t i = 0; i < pt->genericArgs.size(); ++i)
            unifyArgSubst(pt->genericArgs[i].get(), argTy->genericArgs[i],
                          paramNames, argSubst, ctx, conflicts);
    }
}

// A printable signature for a method candidate, e.g. "F(int, long)".
std::string methodSignatureString(std::string_view name,
                                  const std::vector<ast::Param>& params) {
    std::string s(name);
    s += '(';
    bool first = true;
    for (const auto& p : params) {
        if (!first) s += ", ";
        first = false;
        if (!p.modifier.empty()) { s += std::string(p.modifier); s += ' '; }
        // The parameter's source type name, if it's a simple named type.
        if (p.type && p.type->name && !p.type->name->parts.empty())
            s += std::string(p.type->name->parts.back());
        else
            s += "?";
    }
    s += ')';
    return s;
}
}  // namespace

// §22.6/§12.1 (#1090): collect same-name overloads across the class hierarchy.
void ExprTC::collectHierarchyOverloads(TypeSymbol* start, std::string_view name,
                                       std::vector<MethodSymbol*>& out) {
    if (!start) return;
    // Does an already-collected (more-derived) overload `d` shadow the base
    // candidate `b`? An override re-declares the same signature in a derived
    // class, so identical (arity + parameter types) means `b` is the method
    // `d` overrides — keep only the most-derived one to avoid a spurious
    // same-signature ambiguity. (A generic base param compares unequal to its
    // concrete override and is left for §22.6 ranking to disambiguate.)
    auto shadows = [&](MethodSymbol* d, MethodSymbol* b) -> bool {
        if (d->params.size() != b->params.size()) return false;
        for (std::size_t i = 0; i < d->params.size(); ++i) {
            auto* dp = dynamic_cast<ParameterSymbol*>(d->params[i]);
            auto* bp = dynamic_cast<ParameterSymbol*>(b->params[i]);
            if (!dp || !bp) return false;
            // A `ref`/`out` modifier participates in overload identity (§22.6),
            // so two overloads differing only by modifier are NOT the same
            // signature — never dedup them (an override matches modifiers too).
            if (dp->modifier != bp->modifier) return false;
            TypeRef* dt = dp->declType ? ctx_.resolvedType(*dp->declType)
                                       : dp->resolvedDeclType;
            TypeRef* bt = bp->declType ? ctx_.resolvedType(*bp->declType)
                                       : bp->resolvedDeclType;
            if (!typeRefEqual(dt, bt)) return false;
        }
        return true;
    };
    std::vector<TypeSymbol*> frontier{start};
    std::unordered_set<TypeSymbol*> seen;
    std::size_t i = 0;
    while (i < frontier.size()) {
        TypeSymbol* cur = frontier[i++];
        if (!cur || !seen.insert(cur).second) continue;
        for (Symbol* m : cur->members) {
            auto* cand = dynamic_cast<MethodSymbol*>(m);
            if (!cand || cand->name != name) continue;
            bool shadowed = false;
            for (MethodSymbol* prev : out)
                if (shadows(prev, cand)) { shadowed = true; break; }
            if (!shadowed) out.push_back(cand);
        }
        for (TypeSymbol* b : cur->baseTypes)
            if (b && b->kind == ast::TypeDeclKind::Class) frontier.push_back(b);
    }
}

// §22.6 method-overload resolution; see header for contract.
MethodSymbol* ExprTC::resolveMethodOverload(MethodSymbol* initial,
                                            const ast::CallExpr& ce) {
    if (!initial || !initial->parent) return initial;

    // §12.1/§22.6 (#1090): the candidate set is every same-name overload on the
    // receiver's class hierarchy, not just the one class `lookupMember` stopped
    // at — otherwise an overload declared only in a base class is invisible and
    // a valid call to it is mis-rejected. Most-derived first so first-wins
    // arity selection still honours §22.6 closer-wins.
    std::vector<MethodSymbol*> overloads;
    collectHierarchyOverloads(initial->parent, initial->name, overloads);
    if (overloads.empty()) overloads.push_back(initial);

    bool hasNamed = false;
    for (const auto& a : ce.args)
        if (!a.name.empty()) { hasNamed = true; break; }

    // Arity-based fallback used whenever §22.6 type resolution does not
    // apply (modifier args, undecidable types, or a single matching
    // candidate): re-select the first same-name overload the call's
    // arguments can §22.5-bind to, preserving the prior first-wins
    // behavior. For all-positional calls this is the arity match; for
    // named-arg calls it is the first candidate whose binding succeeds.
    auto canBindTo = [&](const ast::MethodDecl* md) -> bool {
        return bindArgsToParams(md->params, ce.args, ce.span.begin,
                                /*calleeDesc=*/std::string(), /*emit=*/false)
            .ok;
    };
    auto arityFallback = [&]() -> MethodSymbol* {
        auto* initMd = dynamic_cast<const ast::MethodDecl*>(initial->decl);
        if (!hasNamed && initial->params.size() == ce.args.size())
            return initial;
        if (hasNamed && initMd && canBindTo(initMd)) return initial;
        for (MethodSymbol* cand : overloads) {
            if (!hasNamed) {
                if (cand->params.size() == ce.args.size()) return cand;
            } else {
                auto* cmd = dynamic_cast<const ast::MethodDecl*>(cand->decl);
                if (cmd && canBindTo(cmd)) return cand;
            }
        }
        return initial;
    };

    // Collect same-name candidates this call's arguments can §22.5-bind to.
    // For all-positional calls binding reduces to an exact arity match
    // (positionals fill parameters in order, all required must be filled);
    // for named-arg calls it is the per-candidate name/order/required check.
    // Each surviving candidate's parameter-ordered argument list is recorded
    // so §22.6 ranking operates on the bound positions (§22.6: tie-breaking
    // applies only after named-argument binding has filtered the candidates).
    std::vector<MethodSymbol*> cands;
    std::vector<const ast::MethodDecl*> decls;
    std::vector<BindResult> binds;
    for (MethodSymbol* cand : overloads) {
        auto* md = dynamic_cast<const ast::MethodDecl*>(cand->decl);
        if (!md) continue;
        BindResult b = bindArgsToParams(md->params, ce.args, ce.span.begin,
                                        /*calleeDesc=*/std::string(),
                                        /*emit=*/false);
        if (!b.ok) continue;
        cands.push_back(cand);
        decls.push_back(md);
        binds.push_back(std::move(b));
    }
    if (cands.empty()) return arityFallback();

    // §22.3 / §22.6 (#952): the call-site `ref`/`out` modifier participates in
    // overload APPLICABILITY and is MANDATORY at the call site. A `ref`
    // parameter is filled only by a `ref` argument; an `out`/`out owned`
    // parameter only by an `out`-family argument; and a plain (or `owned`)
    // parameter only by an argument with NO `ref`/`out` (a superfluous keyword
    // is rejected). Filtering candidates by this rule both SELECTS the right
    // overload (the call-site `ref` is no longer ignored — defects 1/2) and
    // ENFORCES the mandatory keyword even for a single candidate (defects 3/4).
    auto isOutMod = [](std::string_view m) {
        return m == "out" || m == "out var" || m == "out using var" ||
               m == "out owned";
    };
    auto refOutMatch = [&](std::string_view argMod,
                           std::string_view paramMod) -> bool {
        bool argRef = argMod == "ref";
        bool argOut = isOutMod(argMod);
        bool paramRef = paramMod == "ref";
        bool paramOut = paramMod == "out" || paramMod == "out owned";
        if (paramRef) return argRef;   // ref parameter ⟺ ref argument
        if (paramOut) return argOut;   // out parameter ⟺ out argument
        return !argRef && !argOut;     // plain/owned parameter: no ref/out arg
    };
    {
        std::vector<MethodSymbol*> fc;
        std::vector<const ast::MethodDecl*> fd;
        std::vector<BindResult> fb;
        for (std::size_t c = 0; c < cands.size(); ++c) {
            bool ok = true;
            const auto& params = decls[c]->params;
            for (std::size_t j = 0; j < params.size() && ok; ++j) {
                int ai = binds[c].paramToArg[j];
                std::string_view argMod =
                    ai >= 0 ? ce.args[static_cast<std::size_t>(ai)].modifier
                            : std::string_view{};
                if (!refOutMatch(argMod, params[j].modifier)) ok = false;
            }
            if (ok) {
                fc.push_back(cands[c]);
                fd.push_back(decls[c]);
                fb.push_back(std::move(binds[c]));
            }
        }
        if (fc.empty()) {
            // No candidate's parameter modifiers match the call's `ref`/`out`
            // modifiers — a mandatory keyword is missing or superfluous (§22.3).
            // Build a precise message from the first candidate's first mismatch.
            std::string detail;
            const auto& params = decls[0]->params;
            for (std::size_t j = 0; j < params.size(); ++j) {
                int ai = binds[0].paramToArg[j];
                std::string_view argMod =
                    ai >= 0 ? ce.args[static_cast<std::size_t>(ai)].modifier
                            : std::string_view{};
                if (refOutMatch(argMod, params[j].modifier)) continue;
                std::string pname(params[j].name);
                std::string pm(params[j].modifier);
                std::string am(argMod);
                bool paramRefOut = pm == "ref" || pm == "out" || pm == "out owned";
                if (paramRefOut && am.empty())
                    detail = "parameter `" + pname + "` is `" + pm +
                             "` — pass the argument with `" +
                             (pm == "out owned" ? std::string("out")
                                                : pm) +
                             "` at the call site";
                else if (!paramRefOut && !am.empty())
                    detail = "parameter `" + pname +
                             "` is not `ref`/`out` — remove the `" + am +
                             "` at the call site";
                else
                    detail = "the `" + am + "` argument does not match "
                             "parameter `" + pname + "`'s `" + pm + "` modifier";
                break;
            }
            ctx_.diagnostics().report(
                Severity::Error, ce.span.begin,
                "call to `" + std::string(initial->name) +
                "` has a call-site `ref`/`out` mismatch: " + detail +
                " (§22.3 — the caller writes `ref`/`out` at the call site, and "
                "only for a `ref`/`out` parameter)");
            return initial;  // error emitted; return to avoid a cascade
        }
        cands.swap(fc);
        decls.swap(fd);
        binds.swap(fb);
    }
    if (cands.size() == 1) return cands[0];

    // A discard argument (`out _`) is outside the value-conversion ranking of
    // §22.6's worked examples; fall back to arity/first-bind.
    for (const auto& a : ce.args)
        if (a.isDiscard) return arityFallback();

    // §22.6 ranks the bound argument positions. For each candidate, build
    // its parameter-type vector and the matching per-parameter argument-type
    // vector from the binding; a parameter filled by its default contributes
    // no ranking pressure (its slot is dropped from both vectors so it is
    // neither exact nor a conversion). All surviving candidates rank over the
    // common set of bound parameters — which, for the same name and a fixed
    // call, is the same shape across candidates.
    std::vector<std::vector<const TypeRef*>> paramTypes(cands.size());
    std::vector<std::vector<const TypeRef*>> argTypeRows(cands.size());
    std::vector<std::vector<int>> argIdxRows(cands.size());
    std::vector<std::string> candNames(cands.size());
    for (std::size_t c = 0; c < cands.size(); ++c) {
        const auto& params = decls[c]->params;
        for (std::size_t j = 0; j < params.size(); ++j) {
            int ai = binds[c].paramToArg[j];
            if (ai < 0) continue;  // defaulted slot — outside ranking.
            const TypeRef* pt = params[j].type
                                    ? ctx_.resolvedType(*params[j].type)
                                    : nullptr;
            const TypeRef* at = ce.args[static_cast<std::size_t>(ai)].value
                                    ? ctx_.exprType(
                                          *ce.args[static_cast<std::size_t>(ai)]
                                               .value)
                                    : nullptr;
            paramTypes[c].push_back(pt);
            argTypeRows[c].push_back(at);
            argIdxRows[c].push_back(ai);
        }
        candNames[c] = methodSignatureString(initial->name, params);
    }
    // resolveOverload classifies column i of every candidate against a single
    // shared argument-type vector, so each column must correspond to the SAME
    // call argument across all candidates. This holds when the surviving
    // overloads share their parameter ordering for the bound parameters (the
    // common case — overloads of one name differ by type, not by reordering
    // names). If a candidate's bound-parameter shape (length, or which
    // argument fills each column) differs, §22.6 ranking over a non-uniform
    // shape is outside the worked examples; fall back to arity/first-bind.
    const std::vector<const TypeRef*>& argTypes = argTypeRows[0];
    for (std::size_t c = 0; c < cands.size(); ++c) {
        if (argIdxRows[c].size() != argIdxRows[0].size())
            return arityFallback();
        for (std::size_t i = 0; i < argIdxRows[c].size(); ++i)
            if (argIdxRows[c][i] != argIdxRows[0][i]) return arityFallback();
    }

    OverloadResult r = resolveOverload(
        paramTypes, argTypes, candNames, ce.span.begin,
        std::string("`") + std::string(initial->name) + "`");
    switch (r.outcome) {
        case OverloadOutcome::Resolved:
            return cands[r.winnerIndex];
        case OverloadOutcome::Ambiguous:
        case OverloadOutcome::Unreachable:
            // Diagnostic already emitted. Return a candidate so the return
            // type is still inferred (keeps downstream typing best-effort);
            // arity fallback gives a deterministic choice.
            return arityFallback();
        case OverloadOutcome::Undecidable:
            return arityFallback();
    }
    return arityFallback();
}

// Slice 3h: resolve a CallExpr's return type from the callee's
// resolved MethodSymbol.
void ExprTC::typeCall(const ast::CallExpr& ce) {
    if (!ce.callee) return;
    // stdlib §2.12 (#753): BitConverter.Reinterpret<TTo>(value) well-formedness.
    // The target type (explicit type arg) and the source value must be primitive
    // numeric types of EQUAL bit width; a width mismatch — or a non-numeric type
    // on either side — is a compile error. The result type (the target) is set
    // by resolveMemberAccess; this is only the validation, and args are already
    // typed here (post-order), so the source type is available.
    if (auto* rma = dynamic_cast<const ast::MemberAccessExpr*>(ce.callee.get())) {
        if (rma->member == "Reinterpret" && rma->target) {
            if (auto* tid =
                    dynamic_cast<const ast::IdentifierExpr*>(rma->target.get())) {
                if (tid->name == "BitConverter") {
                    auto rw = [](PrimitiveType p) -> int {
                        switch (p) {
                            case PrimitiveType::Byte:
                            case PrimitiveType::SByte:  return 8;
                            case PrimitiveType::Short:
                            case PrimitiveType::UShort: return 16;
                            case PrimitiveType::Int:
                            case PrimitiveType::UInt:
                            case PrimitiveType::Float:  return 32;
                            case PrimitiveType::Long:
                            case PrimitiveType::ULong:
                            case PrimitiveType::Double: return 64;
                            default: return 0;  // not reinterpret-eligible
                        }
                    };
                    PrimitiveType tgt = PrimitiveType::None;
                    if (!rma->typeArgs.empty() && rma->typeArgs[0] &&
                        rma->typeArgs[0]->name &&
                        !rma->typeArgs[0]->name->parts.empty())
                        tgt = primitiveFromName(
                            rma->typeArgs[0]->name->parts.front());
                    PrimitiveType srcP = PrimitiveType::None;
                    if (ce.args.size() == 1 && ce.args[0].value)
                        if (auto* st = ctx_.exprType(*ce.args[0].value))
                            if (st->kind == TypeRef::Kind::Primitive)
                                srcP = st->primitive;
                    int tw = rw(tgt), sw = rw(srcP);
                    if (tw == 0 || sw == 0 || tw != sw)
                        ctx_.diagnostics().report(
                            Severity::Error, ce.span.begin,
                            "BitConverter.Reinterpret requires the target type and "
                            "the source value to be primitive numeric types of equal "
                            "bit width (stdlib §2.12)");
                }
            }
        }
    }
    // #1048: argument-type validation for the char/int-expecting primitive
    // static methods. These set only their RESULT type in resolveMemberAccess
    // and have no MethodSymbol, so they bypass checkArgsAgainstParams — a
    // wrong-typed argument (a `string`/`double`/… where a `char`/`int` is
    // expected) was accepted and crashed codegen, which lowered the argument
    // and then `sext`/`trunc`'d a non-integer value (a string pointer):
    // `string.Repeat("x", 3)`, `char.IsDigit("x")` both IR-failed. Reject any
    // argument whose KNOWN type is not integer-like (char or an integer type)
    // in those positions; leave an untyped/error argument to its own diagnostic.
    if (auto* sma = dynamic_cast<const ast::MemberAccessExpr*>(ce.callee.get())) {
        if (auto* tid =
                dynamic_cast<const ast::IdentifierExpr*>(sma->target.get())) {
            PrimitiveType recvPt = primitiveFromName(tid->name);
            const auto& mm = sma->member;
            auto isIntegerLike = [](PrimitiveType p) {
                switch (p) {
                    case PrimitiveType::Char:
                    case PrimitiveType::Byte:  case PrimitiveType::SByte:
                    case PrimitiveType::Short: case PrimitiveType::UShort:
                    case PrimitiveType::Int:   case PrimitiveType::UInt:
                    case PrimitiveType::Long:  case PrimitiveType::ULong:
                        return true;
                    default: return false;
                }
            };
            auto requireIntLike = [&](size_t i, const char* what) {
                if (i >= ce.args.size() || !ce.args[i].value) return;
                const TypeRef* t = ctx_.exprType(*ce.args[i].value);
                if (!t) return;  // untyped/error arg — its own diagnostic
                if (t->kind == TypeRef::Kind::Primitive &&
                    isIntegerLike(t->primitive))
                    return;
                ctx_.diagnostics().report(
                    Severity::Error, ce.span.begin,
                    "'" + std::string(tid->name) + "." + std::string(mm) +
                    "' requires a `char` " + what + " (stdlib §9.5/§9.6)");
            };
            // §9.6: string.Repeat(c: char, count: int).
            if (recvPt == PrimitiveType::String && mm == "Repeat" &&
                ce.args.size() == 2) {
                requireIntLike(0, "first argument");
                requireIntLike(1, "(int) count");
            }
            // §9.5: char classification / transform statics take a `char`.
            if (recvPt == PrimitiveType::Char &&
                (mm == "IsDigit" || mm == "IsLetter" || mm == "IsLetterOrDigit" ||
                 mm == "IsWhiteSpace" || mm == "IsUpper" || mm == "IsLower" ||
                 mm == "IsControl" || mm == "IsPunctuation" ||
                 mm == "ToUpper" || mm == "ToLower" || mm == "ToString") &&
                ce.args.size() == 1) {
                requireIntLike(0, "argument");
            }
        }
    }
    // §5.2: an explicit conversion `int(x)` — the callee is a primitive
    // type name with no symbol. The call's type is that primitive type.
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(ce.callee.get())) {
        if (!ctx_.exprSymbol(*ce.callee)) {
            PrimitiveType pt = primitiveFromName(id->name);
            if (pt != PrimitiveType::None) {
                ctx_.setExprType(&ce, ctx_.primitiveTypeRef(pt));
                return;
            }
        }
    }
    Symbol* calleeSym = ctx_.exprSymbol(*ce.callee);
    // §5.4: an enum cast `E(i)` — the callee is an enum type name; the
    // call produces a value of that enum type. (Records are NOT
    // call-constructible: per §9.5 a record is built with `new Record(...)`
    // like a class, so a bare `Record(...)` call is not construction and is
    // left untyped here — it surfaces as an error at the use site.)
    // §10 / #482 fix B: also accept a NestedTypeSymbol whose ->type is an enum
    // — calling `Outer.Inner(i)` where `Inner` is a nested enum reaches here as
    // a NestedTypeSymbol; it does NOT inherit TypeSymbol, so the branch below
    // would miss it without this extra check. per §10 the result type is the
    // inner enum's TypeSymbol.
    if (auto* nts = dynamic_cast<NestedTypeSymbol*>(calleeSym)) {
        if (nts->type && nts->type->kind == ast::TypeDeclKind::Enum) {
            TypeRef* et = ctx_.makeTypeRef();
            et->kind = TypeRef::Kind::Named;
            et->declaration = nts->type;
            ctx_.setExprType(&ce, et);
            return;
        }
        // §9.5 (#955): a NESTED record/class is constructed with `new`, not by
        // calling the qualified type name (`Outer.Point(...)`). A
        // NestedTypeSymbol does not inherit TypeSymbol, so the §9.5 reject below
        // (which casts to TypeSymbol) misses it — a qualified `Outer.Point(1,2)`
        // slipped past, left untyped, and miscompiled (the result read as a
        // zero-valued record, or an IR type-mismatch at a typed use site). Mirror
        // the top-level record/class rejection here.
        if (nts->type && (nts->type->kind == ast::TypeDeclKind::Record ||
                          nts->type->kind == ast::TypeDeclKind::Class)) {
            ctx_.diagnostics().report(
                Severity::Error, ce.span.begin,
                "`" + std::string(nts->type->name) +
                    "` is constructed with `new " +
                    std::string(nts->type->name) +
                    "(...)`, not by calling the type name (§9.5)");
            return;
        }
    }
    if (auto* ts = dynamic_cast<TypeSymbol*>(calleeSym)) {
        if (ts->kind == ast::TypeDeclKind::Enum) {
            // §5.4 (#859): `E(n)` with a compile-time-constant `n` outside the
            // enum's underlying type range is a COMPILE ERROR (a runtime cast
            // raises OverflowException instead — handled in codegen). Mirror the
            // §10.3 member-range check on the underlying type (explicit
            // enumUnderlyingType, or the default `int`).
            if (ce.args.size() == 1 && ce.args[0].value) {
                if (auto cv = foldIntConstExprTC(*ce.args[0].value)) {
                    PrimitiveType up = PrimitiveType::Int;
                    if (ts->enumUnderlyingType &&
                        ts->enumUnderlyingType->kind ==
                            TypeRef::Kind::Primitive)
                        up = ts->enumUnderlyingType->primitive;
                    auto fits = [](long long v, PrimitiveType p) -> bool {
                        using P = PrimitiveType;
                        switch (p) {
                            case P::Byte:   return v >= 0 && v <= 255;
                            case P::SByte:  return v >= -128 && v <= 127;
                            case P::Short:  return v >= -32768 && v <= 32767;
                            case P::UShort: return v >= 0 && v <= 65535;
                            case P::Int:    return v >= -2147483648LL &&
                                                   v <= 2147483647LL;
                            case P::UInt:   return v >= 0 && v <= 4294967295LL;
                            default:        return true;  // long/ulong: 64-bit
                        }
                    };
                    if (!fits(*cv, up))
                        ctx_.diagnostics().report(
                            Severity::Error, ce.span.begin,
                            "enum cast `" + std::string(ts->name) + "(" +
                            std::to_string(*cv) +
                            ")` — the constant is outside the enum's underlying "
                            "type range and cannot be represented (§5.4)");
                }
            }
            // per §5.4/§5.1 (#1171): the argument to an enum cast `E(x)` must
            // be an INTEGER value — §5.4 defines `E(n)` only "for an integer
            // n", which is range-checked against the underlying type (at
            // compile time when constant — the fold above — else at runtime
            // via OverflowException, handled in codegen lowerEnumCast). The
            // const-range check above validated the VALUE but never the
            // argument's TYPE, so enum-to-enum casts (`B(a)`) and
            // `double`/`string`/`bool` arguments were silently accepted and
            // miscompiled (the source bits were reinterpreted as the enum).
            // Reject any single argument whose resolved type is not a
            // non-nullable integer primitive; codegen widens/narrows any
            // integer width to the underlying type, so the exact width need
            // not match here. An untyped/error argument keeps its own
            // diagnostic. (Round-tripping an enum needs `int(e)` first.)
            if (ce.args.size() == 1 && ce.args[0].value) {
                const TypeRef* argTy = ctx_.exprType(*ce.args[0].value);
                if (argTy) {
                    auto isIntegerPrim = [](PrimitiveType p) {
                        switch (p) {
                            case PrimitiveType::Byte:
                            case PrimitiveType::SByte:
                            case PrimitiveType::Short:
                            case PrimitiveType::UShort:
                            case PrimitiveType::Int:
                            case PrimitiveType::UInt:
                            case PrimitiveType::Long:
                            case PrimitiveType::ULong:
                                return true;
                            default:
                                return false;
                        }
                    };
                    bool ok = argTy->kind == TypeRef::Kind::Primitive &&
                              !argTy->nullable && isIntegerPrim(argTy->primitive);
                    if (!ok)
                        ctx_.diagnostics().report(
                            Severity::Error, ce.span.begin,
                            "the argument to an enum cast `" +
                            std::string(ts->name) +
                            "(...)` must be an integer of the enum's underlying "
                            "type (use `int(e)` to convert an enum first) "
                            "(§5.4/§5.1)");
                }
            }
            TypeRef* et = ctx_.makeTypeRef();
            et->kind = TypeRef::Kind::Named;
            et->declaration = ts;
            ctx_.setExprType(&ce, et);
            return;
        }
        // §9.5: records (like classes) are constructed with `new`, not by
        // calling the type name. A bare `Record(...)` is not a valid
        // expression — reject it with a fix-it rather than leaving the
        // call untyped (which would silently miscompile downstream).
        if (ts->kind == ast::TypeDeclKind::Record ||
            ts->kind == ast::TypeDeclKind::Class) {
            ctx_.diagnostics().report(
                Severity::Error, ce.span.begin,
                "`" + std::string(ts->name) +
                    "` is constructed with `new " + std::string(ts->name) +
                    "(...)`, not by calling the type name (§9.5)");
            return;
        }
    }
    // §22.6: overload resolution. The callee initially resolved to one
    // method of its name (first-in-source); re-select among the same-named
    // overloads. When the name has multiple same-arity all-positional
    // candidates and the argument/parameter types are decidable, this is
    // the type-based exact-match-then-ambiguity precedence of §22.6;
    // otherwise it falls back to arity-based first-wins selection.
    if (auto* ms0 = dynamic_cast<MethodSymbol*>(calleeSym)) {
        MethodSymbol* chosen = resolveMethodOverload(ms0, ce);
        if (chosen && chosen != ms0) {
            ctx_.setExprSymbol(ce.callee.get(), chosen);
            calleeSym = chosen;
        }
    }
    if (auto* ms = dynamic_cast<MethodSymbol*>(calleeSym)) {
        auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
        // §12.3/§12.5, #874: `base.M()` where M is abstract has no
        // implementation to call — the vtable slot is unfilled.  Diagnose this
        // as a compile error so the caller never reaches garbage at runtime.
        // Only fires for `base.`-qualified calls (not `this.M()` virtual calls,
        // which dispatch to the concrete override).
        if (auto* calleeMA =
                dynamic_cast<const ast::MemberAccessExpr*>(ce.callee.get())) {
            if (auto* baseId = dynamic_cast<const ast::IdentifierExpr*>(
                    calleeMA->target.get())) {
                if (baseId->name == "base") {
                    // Detect abstract: modifier list OR MethodBodyKind::Abstract.
                    bool isMsAbstract = false;
                    for (auto mod : ms->modifiers)
                        if (mod == "abstract") { isMsAbstract = true; break; }
                    if (!isMsAbstract && md)
                        isMsAbstract =
                            (md->bodyKind == ast::MethodBodyKind::Abstract);
                    if (isMsAbstract) {
                        ctx_.diagnostics().report(
                            Severity::Error, ce.span.begin,
                            "cannot call abstract base method `" +
                                std::string(ms->name) +
                                "` via `base` — it has no implementation "
                                "(§12.3/§12.5)");
                        return;
                    }
                }
            }
        }
        if (md && md->returnType) {
            if (auto* rt = ctx_.resolvedType(*md->returnType)) {
                // per §14.1 (generics are monomorphized): a method whose
                // declared return type is a bare type parameter of its parent
                // class — `method GetIt(): T` on `Box<T>` — has the substituted
                // type at a concrete instantiation: `b.GetIt()` on
                // `b: Box<Pair>` is a `Pair`, not the erased `T`. Substitute
                // against the receiver's concrete generic args (exactly as
                // resolveMemberAccess does for a `T`-typed field/property, and
                // as the user-indexer typing does for a `T`-typed indexer
                // result). Without this the call is typed as the bare `T`, so a
                // record substitution's fields can't resolve at the use site
                // and the result is read as garbage (§9: a record is a value
                // type with by-value semantics). The
                // helper is a no-op for a non-type-parameter return type and
                // for a static method (no receiver target).
                if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                        ce.callee.get())) {
                    if (ma->target && ms->parent)
                        rt = substituteMemberType(rt, ma->target.get(),
                                                  ms->parent);
                    // §26.5 (#714): generic extension method — substitute the
                    // return type using the free-var binding inferred at the
                    // member-access site (e.g. T→int for `xs.FirstOr(0)` where
                    // xs: Array<int> and the extension is on IEnumerable<T>).
                    if (!ms->parent && ms->extensionDecl) {
                        if (const auto* bnd = ctx_.exprExtBinding(*ma)) {
                            if (!bnd->empty()) {
                                std::unordered_map<std::string_view,
                                                   TypeRef*> solved;
                                for (const auto& [nm, ty] : *bnd)
                                    solved[nm] = ty;
                                rt = substituteReturnType(rt, solved);
                            }
                        }
                    }
                }
                // per §14.1/§14.2 (issue #509): EXPLICIT method-level type
                // arguments supplied at the call site (e.g. `Identity<int>(v)`
                // or `MakeTwo<int>(a, b)` with compound return `Array<T>`)
                // override the inferred-from-argument path below.  When the
                // callee MemberAccessExpr or IdentifierExpr carries non-empty
                // typeArgs (populated by the parser when it confirms
                // `< TypeArgList > (`), build the generic-param → concrete-TypeRef
                // map from those explicit args in declaration order, then
                // substitute the return type via substituteReturnType so that
                // BOTH bare-param returns (`T`) AND compound returns (`Array<T>`)
                // are substituted.  issue #564 (§14.1 / §22.1): the previous
                // code only handled bare-param return types; compound returns
                // like `owned Array<T>` were left unsubstituted because the
                // guard required `rt` to be the bare param itself.
                //
                // §14.2 (#729): enforce a generic METHOD's `where T: I`
                // constraints at the CALL SITE. A non-conforming type argument
                // was accepted and then SIGSEGV'd via a bad vtable dispatch;
                // reject it with a clean diagnostic. Conservative: only the
                // `type` (interface/class bound) kind, and only when both the
                // bound and the concrete arg resolve to a declaration — defer
                // otherwise (built-in / generic-param / unresolved) to avoid
                // false positives, mirroring Resolver::checkOneConstraint.
                // #746 R41-C: classify a type-argument TypeRef as reference (1) /
                // value (0) / unknown (-1) for the `class`/`value` kind
                // constraints. Mirrors Resolver::categorize (so a class-kind
                // value-builtin is treated as reference, consistent with the
                // type-instantiation check). Function/inline-array/unit are
                // value-ish per §14.2; an unresolved/Error arg is unknown (defer).
                auto refKindOf = [&](const TypeRef* a) -> int {
                    if (!a) return -1;
                    switch (a->kind) {
                        case TypeRef::Kind::Primitive:
                            return (a->primitive == PrimitiveType::String ||
                                    a->primitive == PrimitiveType::Object) ? 1 : 0;
                        case TypeRef::Kind::Function: return 1;
                        case TypeRef::Kind::InlineArray:
                        case TypeRef::Kind::Unit: return 0;
                        case TypeRef::Kind::Named:
                            if (a->declaration) {
                                auto k = a->declaration->kind;
                                if (k == ast::TypeDeclKind::Class ||
                                    k == ast::TypeDeclKind::Interface) return 1;
                                if (k == ast::TypeDeclKind::Record ||
                                    k == ast::TypeDeclKind::Enum) return 0;
                            }
                            return 1;  // built-in named (Array/IEnumerable/…) → ref
                        default: return -1;  // Error/unresolved → defer
                    }
                };
                auto argNameOf = [&](const TypeRef* a) -> std::string {
                    std::string base;
                    if (a && a->declaration) base = std::string(a->declaration->name);
                    else if (a && a->astNode && a->astNode->name &&
                             !a->astNode->name->parts.empty())
                        base = std::string(a->astNode->name->parts.back());
                    else return "the type argument";
                    // §7.1 (#741/f13): a nullable type argument displays with its
                    // `?` so the diagnostic names the actual offending type
                    // (`int?` does not satisfy `value`, not `int`).
                    if (a->nullable) base += "?";
                    return base;
                };
                // §14.2 (#729 / #746 R41-C): enforce a generic METHOD's
                // `where T: …` constraints at the CALL SITE. #729 covered only
                // `type` (interface/class) bounds; the `class`/`value`/`notnull`
                // KIND constraints were skipped (and a primitive arg, having no
                // declaration, was dropped entirely) — `Clone<int>()` with
                // `where T: class` was silently accepted (a soundness hole).
                auto checkMethodGenericConstraints =
                    [&](const std::unordered_map<std::string_view,
                                                 TypeRef*>& subst) {
                        for (const auto& gc : md->genericConstraints) {
                            auto ai = subst.find(gc.typeParam);
                            if (ai == subst.end() || !ai->second) continue;
                            TypeRef* argRef = ai->second;
                            TypeSymbol* argDecl = argRef->declaration;  // may be null
                            auto report = [&](const std::string& reason) {
                                ctx_.diagnostics().report(
                                    Severity::Error, ce.span.begin,
                                    "type argument `" + argNameOf(argRef) +
                                    "` does not satisfy the constraint `where " +
                                    std::string(gc.typeParam) + ": " + reason +
                                    "` (§14.2)");
                            };
                            for (const auto& item : gc.items) {
                                if (item.kind == "type" && item.typeArg) {
                                    if (!argDecl) continue;  // defer: built-in/primitive bound match
                                    TypeRef* bound =
                                        ctx_.resolvedType(*item.typeArg);
                                    if (!bound || !bound->declaration) continue;
                                    if (!isSubtype(argDecl, bound->declaration))
                                        report(std::string(
                                            bound->declaration->name));
                                } else if (item.kind == "class") {
                                    // §14.2: non-nullable reference type only.
                                    int r = refKindOf(argRef);
                                    if (argRef->nullable || r == 0) report("class");
                                } else if (item.kind == "value") {
                                    // §14.2: non-nullable value type only.
                                    int r = refKindOf(argRef);
                                    if (argRef->nullable || r == 1) report("value");
                                } else if (item.kind == "notnull") {
                                    if (argRef->nullable) report("notnull");
                                } else if (item.kind == "new") {
                                    // §14.2 (#864 part 2): `where T: new()`
                                    // requires an accessible parameterless ctor.
                                    // Mirrors Resolver::argSatisfiesNew for the
                                    // method-call-site check.
                                    if (!argDecl) continue;  // built-in/unresolved: defer
                                    bool satisfied = false;
                                    bool decided = false;
                                    if (argDecl->kind == ast::TypeDeclKind::Enum ||
                                        argDecl->kind == ast::TypeDeclKind::Record) {
                                        // Enums and records (positional fields
                                        // default-constructible when no non-nullable
                                        // ref field) satisfy new(); conservatively
                                        // accept — the exhaustive check is in
                                        // Resolver::argSatisfiesNew. Defer (decided=false).
                                        continue;
                                    }
                                    if (argDecl->kind == ast::TypeDeclKind::Interface) {
                                        decided = true;
                                        satisfied = false;  // §11.2: not constructible
                                    } else if (argDecl->kind == ast::TypeDeclKind::Class) {
                                        auto* td = dynamic_cast<const ast::TypeDecl*>(
                                            argDecl->decl);
                                        decided = true;
                                        if (td && td->hasPositionalFields &&
                                            !td->positionalFields.empty()) {
                                            satisfied = false;  // primary ctor has params
                                        } else {
                                            std::vector<const ConstructorSymbol*> ctors;
                                            for (Symbol* mem : argDecl->members) {
                                                if (auto* cs =
                                                        dynamic_cast<const ConstructorSymbol*>(
                                                            mem))
                                                    if (!cs->isStatic)
                                                        ctors.push_back(cs);
                                            }
                                            if (ctors.empty()) {
                                                satisfied = true;  // implicit default ctor
                                            } else {
                                                for (const ConstructorSymbol* cs : ctors) {
                                                    if (cs->params.empty() &&
                                                        cs->visibility !=
                                                            ast::Visibility::Private) {
                                                        satisfied = true;
                                                        break;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    if (decided && !satisfied) report("new()");
                                }
                            }
                        }
                    };
                bool hadExplicitTypeArgs = false;
                if (!md->genericParams.empty()) {
                    // per §14.1 (issue #509/#561): explicit type args may
                    // appear on a MemberAccessExpr callee (`obj.Method<T>(...)`)
                    // OR on a bare IdentifierExpr callee (`Id<T>(...)` — a
                    // same-class or in-scope generic call without a dot).  Both
                    // AST node types carry a `typeArgs` vector populated by the
                    // parser when it confirms `< TypeArgList > (`.
                    const std::vector<std::unique_ptr<ast::Type>>* callTypeArgs =
                        nullptr;
                    if (auto* callMa =
                            dynamic_cast<const ast::MemberAccessExpr*>(
                                ce.callee.get()))
                        callTypeArgs = &callMa->typeArgs;
                    else if (auto* callId =
                                 dynamic_cast<const ast::IdentifierExpr*>(
                                     ce.callee.get()))
                        callTypeArgs = &callId->typeArgs;
                    if (callTypeArgs && !callTypeArgs->empty()) {
                        // Build the explicit substitution map:
                        // md->genericParams[i].name → resolved TypeRef for
                        // (*callTypeArgs)[i].
                        std::unordered_map<std::string_view, TypeRef*>
                            explicitSubst;
                        for (std::size_t gi = 0;
                             gi < md->genericParams.size() &&
                             gi < callTypeArgs->size(); ++gi) {
                            if (!(*callTypeArgs)[gi]) continue;
                            TypeRef* concreteArg =
                                ctx_.resolvedType(*(*callTypeArgs)[gi]);
                            if (concreteArg)
                                explicitSubst[md->genericParams[gi].name] =
                                    concreteArg;
                        }
                        // #729: enforce the method's where-constraints on the
                        // explicit type args before consuming them. This path is
                        // authoritative when explicit args are present, so the
                        // argument-inferred check below is suppressed.
                        hadExplicitTypeArgs = true;
                        checkMethodGenericConstraints(explicitSubst);
                        // Apply substitution: substituteReturnType handles
                        // both bare-param returns (e.g. `T` → concrete T)
                        // and compound returns (e.g. `Array<T>` → `Array<int>`).
                        if (!explicitSubst.empty())
                            rt = substituteReturnType(rt, explicitSubst);
                    }
                }
                // per §14.1/§14.2 (issue #504): a GENERIC METHOD (its own
                // `<T>` params on a non-generic parent, e.g. `static method
                // Identity<T>(v:T):T` on plain `Wrapper`) whose declared
                // return type references a method-level type parameter is
                // typed by the substituted concrete type at the call site.
                // `substituteMemberType` only handles class-level params
                // (via `ms->parent->genericParams`); it is a no-op here
                // because `Wrapper` has no generic params. Infer T from the
                // call's arguments: for each method generic param, find the
                // first value parameter whose declared type is exactly that
                // param name, and read the corresponding call argument's
                // sema exprType. Build a subst map and call substituteReturnType
                // so that BOTH bare-param returns (`T`) AND compound returns
                // (e.g. `Array<T>` — issue #564) are substituted correctly.
                // This lets chained access `Identity(r).x` resolve `.x`
                // against the concrete record type rather than the
                // unresolvable bare `T`.
                // (The #509 explicit-typeArgs path above takes priority; this
                // path only runs when typeArgs is empty — i.e., type inference
                // from arguments is needed.)
                if (!md->genericParams.empty()) {
                    // §14.1 (#785): build the argument-inferred substitution map
                    // by STRUCTURALLY unifying each value parameter's declared
                    // type against the call argument's type — so a generic-typed
                    // parameter `p: Pair<A, B>` binds A and B (not only a bare
                    // `p: T`). Without this the return type `A` was left as the
                    // unresolvable bare param (`q := First(p)` typed `?`).
                    std::unordered_set<std::string_view> paramNames;
                    for (const auto& gp : md->genericParams)
                        paramNames.insert(gp.name);
                    std::unordered_map<std::string_view, TypeRef*> argSubst;
                    // §14.1 / #896: collect type-param names whose inference
                    // produces a conflict (two args would bind the same T to
                    // different types). Passing &inferConflicts to unifyArgSubst
                    // lets it detect the second-binding mismatch instead of
                    // silently dropping it (first-binding-wins would otherwise
                    // let codegen mix int and bool in a PHINode → SIGABRT).
                    std::unordered_set<std::string_view> inferConflicts;
                    for (std::size_t pi = 0;
                         pi < md->params.size() && pi < ce.args.size(); ++pi) {
                        if (!md->params[pi].type || !ce.args[pi].value) continue;
                        // §14.1 / §22.4 / #1164: an inline `out var x`
                        // (and `out using var` / `out owned var`) argument has
                        // no type of its own — its type is INFERRED FROM the
                        // matched `out` parameter (§22.4), so its sema type is
                        // the parameter's still-unbound type parameter itself
                        // (e.g. `T`). Unifying it here would try to bind `T` to
                        // `T` and spuriously conflict with the binding produced
                        // by a real value argument (e.g. `5` binding `T = int`),
                        // yielding a false "inconsistent type-argument
                        // inference" reject. Skip it: the parameter binds the
                        // out-var; the out-var does not constrain the parameter.
                        // A PRE-DECLARED `out x` (plain `out` / `out owned`)
                        // carries a concrete declared type and still
                        // participates in inference, so only the inline
                        // var-declaring forms are skipped.
                        {
                            std::string_view amod = ce.args[pi].modifier;
                            if (amod == "out var" || amod == "out using var" ||
                                amod == "out owned var")
                                continue;
                        }
                        TypeRef* argTy = ctx_.exprType(*ce.args[pi].value);
                        // §14.1 / #885/#886: skip null-literal arguments during
                        // type-parameter inference — a null literal is typed as
                        // nullable Object (the sema sentinel for an uncontexted
                        // null), which carries no concrete type information.
                        // Binding T=Object from null would block the correct
                        // binding from a subsequent concrete argument.
                        // Mirror the same skip that codegen's lowerCall performs.
                        if (argTy && argTy->kind == TypeRef::Kind::Primitive &&
                            argTy->primitive == PrimitiveType::Object &&
                            argTy->nullable)
                            continue;
                        unifyArgSubst(md->params[pi].type.get(), argTy,
                                      paramNames, argSubst, ctx_,
                                      &inferConflicts);
                    }
                    // §14.1 / #896: if any type parameter was bound to two
                    // incompatible types across the arguments, emit a compile
                    // error and abandon codegen for this call. Report all
                    // conflicting params (there is rarely more than one).
                    if (!inferConflicts.empty()) {
                        // Build a readable type-name for a TypeRef (matching the
                        // style used by the §14.2 argNameOf lambda above).
                        // Spell a resolved TypeRef as a source-level name for
                        // the error message. Mirrors the §14.2 argNameOf lambda
                        // above but also handles the primitive case (where
                        // declaration is always null). per §14.1 / #896.
                        auto spellTR = [&](const TypeRef* a) -> std::string {
                            if (!a) return "?";
                            if (a->kind == TypeRef::Kind::Primitive) {
                                using P = PrimitiveType;
                                const char* n = "?";
                                switch (a->primitive) {
                                    case P::Bool:   n = "bool";    break;
                                    case P::Char:   n = "char";    break;
                                    case P::String: n = "string";  break;
                                    case P::Byte:   n = "byte";    break;
                                    case P::SByte:  n = "sbyte";   break;
                                    case P::Short:  n = "short";   break;
                                    case P::UShort: n = "ushort";  break;
                                    case P::Int:    n = "int";     break;
                                    case P::UInt:   n = "uint";    break;
                                    case P::Long:   n = "long";    break;
                                    case P::ULong:  n = "ulong";   break;
                                    case P::Float:  n = "float";   break;
                                    case P::Double: n = "double";  break;
                                    case P::Decimal: n = "decimal"; break;
                                    case P::Object: n = "Object";  break;
                                    default:        n = "?";       break;
                                }
                                std::string s(n);
                                if (a->nullable) s += '?';
                                return s;
                            }
                            if (a->kind == TypeRef::Kind::Named) {
                                std::string s;
                                if (a->declaration)
                                    s = std::string(a->declaration->name);
                                else if (a->astNode && a->astNode->name &&
                                         !a->astNode->name->parts.empty())
                                    s = std::string(a->astNode->name->parts.back());
                                else
                                    s = "?";
                                if (a->nullable) s += '?';
                                return s;
                            }
                            return "?";
                        };
                        for (std::string_view cnm : inferConflicts) {
                            // argSubst holds the FIRST (winning) binding.
                            const TypeRef* first =
                                argSubst.count(cnm) ? argSubst.at(cnm) : nullptr;
                            ctx_.diagnostics().report(
                                Severity::Error, ce.span.begin,
                                "inconsistent type-argument inference: type "
                                "parameter `" + std::string(cnm) +
                                "` cannot be bound to both `" + spellTR(first) +
                                "` and a different type — all arguments for a "
                                "type parameter must agree (§14.1, #896)");
                        }
                        return;  // skip constraint check + codegen for this call
                    }
                    // #729: enforce the method's where-constraints on the
                    // argument-inferred type args (only when no explicit type
                    // args were given — else the explicit check already ran).
                    if (!hadExplicitTypeArgs)
                        checkMethodGenericConstraints(argSubst);
                    // Apply substitution to the (possibly compound) return type.
                    if (!argSubst.empty())
                        rt = substituteReturnType(rt, argSubst);
                }
                ctx_.setExprType(&ce, rt);
            }
        } else if (md && !md->returnType) {
            // No return type ascription → unit.
            auto* unit = ctx_.makeTypeRef();
            unit->kind = TypeRef::Kind::Unit;
            ctx_.setExprType(&ce, unit);
        } else if (!md && ctx_.isImported(ms)) {
            // §2.2 / §2.5 (issue #328 inc 3): an imported method has no
            // ast::MethodDecl; its resolved return type was stashed by the
            // loader keyed on the symbol. Use it so the call expression types
            // correctly against the imported signature.
            if (TypeRef* rt = ctx_.importedReturnType(ms)) {
                ctx_.setExprType(&ce, rt);
                // #783 / §14.1 (decline-with-reasons): an IMPORTED generic class's
                // method whose return type references a class type parameter is NOT
                // monomorphized importer-side — its body was compiled in the
                // defining module against the placeholder (i32) layout, so calling
                // it on a REFERENCE-typed instantiation (the parameter lowers to
                // `ptr`, not i32) returns the field at the wrong offset → a silently
                // wrong value (e.g. `Box<string>.Get()` → 0). Direct field access,
                // ctors, and owned-field disposal ARE specialized importer-side and
                // keep working; an `int` instantiation matches the placeholder and
                // works; a method whose return type does NOT use the parameter is
                // not substituted (sub == rt) and is left alone. Reject ONLY the
                // proven-miscompiling shape with a clean diagnostic (importer-side
                // method monomorphization is the full fix — #783, #428 family) so a
                // clean compile never returns garbage.
                if (ms->parent && ctx_.isImported(ms->parent) &&
                    !ms->parent->genericParams.empty()) {
                    if (auto* gma = dynamic_cast<const ast::MemberAccessExpr*>(
                            ce.callee.get())) {
                        if (gma->target) {
                            TypeRef* sub = substituteMemberType(
                                rt, gma->target.get(), ms->parent);
                            bool refLike = sub && sub != rt &&
                                ((sub->kind == TypeRef::Kind::Primitive &&
                                  sub->primitive == PrimitiveType::String) ||
                                 (sub->kind == TypeRef::Kind::Named &&
                                  sub->declaration &&
                                  (sub->declaration->kind ==
                                       ast::TypeDeclKind::Class ||
                                   sub->declaration->kind ==
                                       ast::TypeDeclKind::Interface)));
                            if (refLike)
                                ctx_.diagnostics().report(
                                    Severity::Error, gma->span.begin,
                                    "calling `" + std::string(ms->name) +
                                    "` on an imported generic class instantiated "
                                    "with a reference type argument is not yet "
                                    "monomorphized (\xC2\xA7""14.1 / #783): the "
                                    "method's body uses the type parameter against "
                                    "the placeholder layout and would return a "
                                    "wrong value. Access the field directly, or use "
                                    "an `int` instantiation, until importer-side "
                                    "method monomorphization lands");
                        }
                    }
                }
            }
        }
    } else if (auto* xd = dynamic_cast<ExternDeclSymbol*>(calleeSym)) {
        // §32: a call to an extern method takes its declared return type.
        if (xd->externDecl && xd->externDecl->returnType) {
            if (auto* rt = ctx_.resolvedType(*xd->externDecl->returnType))
                ctx_.setExprType(&ce, rt);
        } else {
            auto* unit = ctx_.makeTypeRef();
            unit->kind = TypeRef::Kind::Unit;
            ctx_.setExprType(&ce, unit);
        }
    }
    // CG60: call through a function-typed local/parameter — propagate the
    // function's declared return type to the CallExpr so that downstream
    // expressions (e.g. `var x := f(n)`) are typed correctly.
    if (auto* loc = dynamic_cast<LocalSymbol*>(calleeSym)) {
        if (loc->declType && loc->declType->kind == TypeRef::Kind::Function &&
            loc->declType->funcReturn)
            ctx_.setExprType(&ce, loc->declType->funcReturn);
    } else if (auto* par = dynamic_cast<ParameterSymbol*>(calleeSym)) {
        TypeRef* prt = par->declType ? ctx_.resolvedType(*par->declType) : nullptr;
        if (prt && prt->kind == TypeRef::Kind::Function && prt->funcReturn)
            ctx_.setExprType(&ce, prt->funcReturn);
    } else if (dynamic_cast<FieldSymbol*>(calleeSym)) {
        // #1138 / §27.7: a call through a function-typed FIELD (`h._fn()` where
        // `_fn: () -> Pt`). The local/parameter branches above miss a FieldSymbol
        // callee, and the `!calleeSym` fallback below does not fire (the field
        // resolves to a symbol), so the call result stayed untyped — then
        // `var p := h._fn()` could not type `p`, and a by-value (record) result's
        // field reads folded to 0. Propagate the callee's resolved function
        // return type (the field read is already typed as the function type).
        if (TypeRef* ft = ce.callee ? ctx_.exprType(*ce.callee) : nullptr)
            if (ft->kind == TypeRef::Kind::Function && ft->funcReturn)
                ctx_.setExprType(&ce, ft->funcReturn);
    }
    // #1093 / §5.1: a call THROUGH a function-typed value (a delegate local /
    // parameter / field, an indexed function element, or a chained call result)
    // must type-check its arguments against the function type's parameter list —
    // arity and per-argument assignability — exactly as a named-method call
    // does (checkCallArity). Without this the delegate-invocation path skipped
    // argument checking, so a wrong-typed or wrong-arity argument was silently
    // accepted and miscompiled. A method/constructor callee is excluded (it has
    // its own checkCallArity); only a genuine function-typed callee is checked.
    {
        bool isMethodOrCtor = dynamic_cast<MethodSymbol*>(calleeSym) ||
                              dynamic_cast<ConstructorSymbol*>(calleeSym);
        TypeRef* ft = ce.callee ? ctx_.exprType(*ce.callee) : nullptr;
        if ((!ft || ft->kind != TypeRef::Kind::Function) && !isMethodOrCtor) {
            if (auto* loc = dynamic_cast<LocalSymbol*>(calleeSym))
                ft = loc->declType;
            else if (auto* par = dynamic_cast<ParameterSymbol*>(calleeSym))
                ft = par->declType ? ctx_.resolvedType(*par->declType)
                                   : par->resolvedDeclType;
        }
        if (!isMethodOrCtor && ft && ft->kind == TypeRef::Kind::Function) {
            // Defer named / modifier / discard arguments (by-ref delegate
            // binding is not modeled here), mirroring checkImportedCallArity.
            bool simple = true;
            for (const auto& a : ce.args)
                if (!a.name.empty() || !a.modifier.empty() || a.isDiscard) {
                    simple = false;
                    break;
                }
            if (simple) {
                if (ce.args.size() != ft->funcParams.size()) {
                    ctx_.diagnostics().report(
                        Severity::Error, ce.span.begin,
                        "call supplies " + std::to_string(ce.args.size()) +
                        " argument(s) but the function value expects " +
                        std::to_string(ft->funcParams.size()) + " (§5.1)");
                } else {
                    for (std::size_t i = 0; i < ce.args.size(); ++i) {
                        if (!ce.args[i].value || !ft->funcParams[i].type) continue;
                        checkAssignable(
                            ctx_.exprType(*ce.args[i].value),
                            ft->funcParams[i].type,
                            ce.args[i].value->span.begin,
                            "argument " + std::to_string(i + 1) +
                                " of the function call");
                    }
                }
            }
        }
    }
    // CG27c / CG60: if the callee has no resolved symbol but has a type,
    // propagate the appropriate type to the CallExpr:
    // - If the callee has a Function TypeRef (e.g. indexed array element
    //   `handlers[i]`), use funcReturn as the call's return type.
    // - Otherwise (intrinsic member like `n.ToString()`), use the callee
    //   type directly.
    // §3: infer return types for stdlib collection and IO instance methods
    // (no MethodSymbol exists, so the normal calleeSym path doesn't apply).
    if (!calleeSym) {
        // #591: Console.Write / Console.WriteLine have only 0-or-1-argument
        // overloads (stdlib §2.1). A call with >1 arguments has no matching
        // overload — codegen's tryLowerConsoleCall returns false and the call
        // is silently dropped.  Diagnose here in sema so the user sees a clear
        // error instead of missing output at runtime.
        //
        // Similarly, Console.Error.Write / Console.Error.WriteLine (and the
        // .Out. variants) follow the same overload set; reject wrong-arity
        // calls on those as well.
        if (auto* consoleMA =
                dynamic_cast<const ast::MemberAccessExpr*>(ce.callee.get())) {
            const auto& mbr = consoleMA->member;
            bool isWrite     = (mbr == "Write");
            bool isWriteLine = (mbr == "WriteLine");
            if ((isWrite || isWriteLine) && consoleMA->target) {
                // Direct Console.Write/WriteLine.
                bool onConsole = false;
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        consoleMA->target.get())) {
                    onConsole = (id->name == "Console");
                }
                // Console.Error.Write/WriteLine or Console.Out.Write/WriteLine.
                if (!onConsole) {
                    if (auto* innerMA = dynamic_cast<const ast::MemberAccessExpr*>(
                            consoleMA->target.get())) {
                        if (innerMA->target) {
                            if (auto* id2 = dynamic_cast<const ast::IdentifierExpr*>(
                                    innerMA->target.get())) {
                                if (id2->name == "Console" &&
                                    (innerMA->member == "Error" ||
                                     innerMA->member == "Out"))
                                    onConsole = true;
                            }
                        }
                    }
                }
                if (onConsole) {
                    // Write: exactly 1 arg. WriteLine: 0 or 1 arg.
                    const std::size_t nargs = ce.args.size();
                    bool arityOk = isWrite ? (nargs == 1)
                                           : (nargs == 0 || nargs == 1);
                    if (!arityOk) {
                        ctx_.diagnostics().report(
                            Severity::Error, ce.span.begin,
                            std::string("Console.") + std::string(mbr) +
                            " has no overload that accepts " +
                            std::to_string(nargs) +
                            " argument(s): stdlib §2.1 defines only " +
                            (isWrite ? "Write(value)" : "WriteLine() and WriteLine(value)") +
                            " — for composite formatting use string interpolation"
                            " (`$\"{...}\"`) instead");
                    }
                }
            }
        }
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(ce.callee.get())) {
            if (ma->target) {
                auto* recvTy = ctx_.exprType(*ma->target);
                // #466 / §26.5 / §3: reject chaining a member call/access onto the
                // VOID result of an in-place collection method. Per §26.5
                // ("instance members win"), `a.Reverse()` on a concrete Array<T>
                // resolves to the in-place void `Array<T>.Reverse()` (§3), which
                // SHADOWS the LINQ extension `Reverse(): IEnumerable<T>` (§5) —
                // so `a.Reverse().ToArray()` calls `.ToArray()` on `unit`, which
                // is not a member-bearing value. Without this gate codegen lowers
                // the chained call to no value, leaving the `using var` slot
                // uninitialized and then disposing it → free()-invalid-pointer
                // heap corruption (#466). The same applies to Sort/Add/Push/etc.
                // The fix is the spec-faithful one: a clear compile error, not a
                // silently-different lazy Reverse (which would violate §26.5).
                if (auto* tgtCall =
                        dynamic_cast<const ast::CallExpr*>(ma->target.get())) {
                    if (!ctx_.exprType(*tgtCall)) {  // inner call typed unit/void
                        if (auto* innerMa = dynamic_cast<const ast::MemberAccessExpr*>(
                                tgtCall->callee.get())) {
                            if (innerMa->target) {
                                auto* innerRecvTy = ctx_.exprType(*innerMa->target);
                                // Inner receiver must be a concrete Array-family
                                // collection (a lazy/IEnumerable receiver would
                                // bind the §5 Reverse and is fine).
                                bool innerIsColl =
                                    innerRecvTy &&
                                    innerRecvTy->kind == TypeRef::Kind::Named &&
                                    !innerRecvTy->declaration && innerRecvTy->astNode &&
                                    innerRecvTy->astNode->name &&
                                    !innerRecvTy->astNode->name->parts.empty() &&
                                    [&] {
                                        const auto& cn =
                                            innerRecvTy->astNode->name->parts[0];
                                        return cn == "Array" || cn == "OwnedArray" ||
                                               cn == "Stack" || cn == "OwnedStack" ||
                                               cn == "Queue" || cn == "OwnedQueue";
                                    }();
                                const auto& im = innerMa->member;
                                // In-place void collection mutators (§3). These
                                // return unit; chaining on their result is illegal.
                                bool isVoidInPlace =
                                    im == "Reverse" || im == "Sort" || im == "Add" ||
                                    im == "Push" || im == "Enqueue" || im == "Set" ||
                                    im == "Insert" || im == "RemoveAt" || im == "Clear";
                                if (innerIsColl && isVoidInPlace) {
                                    std::string msg =
                                        std::string("`.") + std::string(im) +
                                        "()` on this collection returns nothing "
                                        "(it modifies the collection in place, §3), "
                                        "so `." + std::string(ma->member) +
                                        "` cannot be chained onto it.";
                                    // `Reverse` is the one method here with BOTH an
                                    // in-place form (§3) and a LINQ extension (§5);
                                    // per §26.5 the instance form shadows the
                                    // extension. Give the lazy-pipeline workaround
                                    // only for Reverse — the others (Add/Sort/Clear/
                                    // …) have no lazy counterpart.
                                    if (im == "Reverse")
                                        msg += " Per §26.5 the in-place instance "
                                               "method shadows the LINQ `Reverse` "
                                               "extension (§5); to build a reversed "
                                               "copy use `a.Reversed()` (§3 / #472) "
                                               "or drive the LINQ `Reverse()` from a "
                                               "lazy receiver (`a.Where(..).Reverse()"
                                               ".ToArray()`).";
                                    ctx_.diagnostics().report(
                                        Severity::Error, ma->span.begin, msg);
                                    return;
                                }
                            }
                        }
                    }
                }
                auto colName = [&]() -> std::string_view {
                    if (recvTy && recvTy->kind == TypeRef::Kind::Named &&
                        !recvTy->declaration && recvTy->astNode &&
                        recvTy->astNode->name && !recvTy->astNode->name->parts.empty())
                        return recvTy->astNode->name->parts[0];
                    // Builtin class identifiers (File, Directory, etc.) are given a
                    // symbol but no TypeRef; fall back to reading the identifier name.
                    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get()))
                        return id->name;
                    return {};
                }();
                using P = PrimitiveType;
                const auto& m = ma->member;
                // §8.5 / §8.6 (#417): `o.ToString()` where o: Object or an
                // interface — the universal Object.ToString (no MethodSymbol,
                // dispatched through the vtable ToString slot in codegen). The
                // result is `string` (§8.5). An Object-typed receiver is a
                // Primitive(Object) TypeRef; an interface-typed receiver is a
                // Named TypeRef whose declaration is an interface.
                // #608 / §8.5 / §12: also handle user-defined class receivers
                // (e.g. a user exception class ErrA: Exception) where ToString
                // has no MethodSymbol (TypeSymbol::baseTypes skips builtin bases,
                // so lookupMember returns null). Codegen dispatches through vtable
                // slot 3 for any class receiver, so the return type is always
                // `string`. per §8.5 (v0.134).
                if (m == "ToString" && ce.args.empty() && recvTy) {
                    bool isObjectRecv = recvTy->kind == TypeRef::Kind::Primitive &&
                                        recvTy->primitive == P::Object;
                    bool isIfaceRecv =
                        recvTy->kind == TypeRef::Kind::Named &&
                        recvTy->declaration &&
                        recvTy->declaration->kind == ast::TypeDeclKind::Interface;
                    bool isNamedClassRecv =
                        recvTy->kind == TypeRef::Kind::Named &&
                        recvTy->declaration &&
                        recvTy->declaration->kind == ast::TypeDeclKind::Class;
                    // #608 / §8.5 / §31: built-in exception types (Exception,
                    // ArgumentException, …) are Named TypeRefs with no user
                    // TypeSymbol (declaration == nullptr). `catch var e: Exception`
                    // gives e a builtin-typed receiver; ToString on it must return
                    // string just like any other class.  per §8.5 (v0.134).
                    bool isBuiltinExRecv =
                        recvTy->kind == TypeRef::Kind::Named &&
                        !recvTy->declaration;
                    if (isObjectRecv || isIfaceRecv || isNamedClassRecv ||
                            isBuiltinExRecv) {
                        ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::String));
                        return;
                    }
                }
                // §8.2 (#487): `v.GetType()` → Type.  GetType is a universal
                // Object member (§8.1) present on every type, so it reaches here
                // with no resolved MethodSymbol on any receiver.  Its return type
                // is the `Type` builtin class (stdlib §2.13).  We type it here
                // in typeCall (not in resolveMemberAccess) so the CallExpr itself
                // is typed, which is what downstream type-inference (var locals,
                // return-type checks, assignability) consults.  Typing is
                // unconditional on receiver kind: GetType is valid on classes,
                // records, enums, primitives (boxed per §8.2), and Object.
                //
                // #511: emit the "not yet implemented" diagnostic HERE in sema
                // (not only in codegen) so that chained uses such as
                // `obj.GetType().Name` fire the error unconditionally regardless
                // of context.  The expression is still TYPED as `Type` so that
                // no secondary "untyped expression" cascades are emitted and the
                // future-implementation path (System.Reflection §11) is preserved.
                if (m == "GetType" && ce.args.empty()) {
                    auto* typeTy = ctx_.makeTypeRef();
                    typeTy->kind = TypeRef::Kind::Named;
                    typeTy->declaration = builtinClassSym("Type");
                    ctx_.setExprType(&ce, typeTy);
                    ctx_.diagnostics().report(
                        Severity::Error, ce.span.begin,
                        "reflection is not yet implemented: 'GetType()' "
                        "(§8.2, #487); deferred pending System.Reflection §11");
                    return;
                }
                // stdlib §5: build a lazy IEnumerable<T> result type for a
                // deferred LINQ operator. Represented as a Named TypeRef with
                // no declaration and no astNode (a structural sentinel the
                // codegen recognizes as "lazy enumerable", distinct from the
                // astNode-backed builtin IEnumerable<T> spelled in source).
                auto makeLazyEnumerable = [&](TypeRef* elem) -> TypeRef* {
                    auto* t = ctx_.makeTypeRef();
                    t->kind = TypeRef::Kind::Named;
                    t->declaration = nullptr;
                    t->astNode = nullptr;
                    if (elem) t->genericArgs.push_back(elem);
                    return t;
                };
                // per §5 / fix #459: build a Dictionary<K,V> result TypeRef for
                // ToDictionary so that the inferred-local case (`var d :=
                // seq.ToDictionary(keyFn)`) produces a type that both sema's
                // `colName` check (reads astNode->name->parts[0]) and codegen's
                // `isDictTypeName` recognize as a Dictionary receiver. A static
                // synthetic ast::Type with the "Dictionary" name is used as the
                // astNode; the actual key/value types live in genericArgs.
                // The static nodes are intentionally leaked (program-lifetime),
                // which is the correct treatment for a compiler's synthetic nodes.
                auto makeDictResult = [&](TypeRef* keyElem,
                                          TypeRef* valElem) -> TypeRef* {
                    static const ast::Type* kDictAstNode = []() -> ast::Type* {
                        auto* qn = new ast::QualifiedName;
                        qn->parts.push_back("Dictionary");
                        auto* t = new ast::Type;
                        t->kind = ast::TypeKind::Named;
                        t->name.reset(qn);
                        return t;
                    }();
                    auto* tr = ctx_.makeTypeRef();
                    tr->kind = TypeRef::Kind::Named;
                    tr->declaration = nullptr;
                    tr->astNode = kDictAstNode;
                    if (keyElem) tr->genericArgs.push_back(keyElem);
                    if (valElem) tr->genericArgs.push_back(valElem);
                    return tr;
                };
                // per §5 + §14: build a concrete Array<T> or Set<T> TypeRef
                // for ToArray/ToSet materializers so that un-annotated inferred
                // locals (`var b := seq.ToArray()`) carry the right concrete
                // type for downstream member access (`.Get(i)`, `.Length`, for-
                // in, etc.). Uses the same static-synthetic-astNode pattern as
                // makeDictResult. Without this, sema was returning the lazy
                // sentinel (no astNode), so colName was empty and downstream
                // calls on the inferred local silently fell through → wrong
                // element layout (int→0) or ICmp crash (record field).
                // The static ast nodes are program-lifetime (intentionally
                // leaked), consistent with all other synthetic nodes here.
                auto makeConcreteCollectionResult =
                    [&](std::string_view collName,
                        TypeRef* elemTy) -> TypeRef* {
                    // One static astNode per collection name; indexed below.
                    // kArray / kSet cover the two materializers we expose.
                    static const ast::Type* kArrayAstNode = []() -> ast::Type* {
                        auto* qn = new ast::QualifiedName;
                        qn->parts.push_back("Array");
                        auto* t = new ast::Type;
                        t->kind = ast::TypeKind::Named;
                        t->name.reset(qn);
                        return t;
                    }();
                    static const ast::Type* kSetAstNode = []() -> ast::Type* {
                        auto* qn = new ast::QualifiedName;
                        qn->parts.push_back("Set");
                        auto* t = new ast::Type;
                        t->kind = ast::TypeKind::Named;
                        t->name.reset(qn);
                        return t;
                    }();
                    const ast::Type* node =
                        (collName == "Set") ? kSetAstNode : kArrayAstNode;
                    auto* tr = ctx_.makeTypeRef();
                    tr->kind = TypeRef::Kind::Named;
                    tr->declaration = nullptr;
                    tr->astNode = node;
                    if (elemTy) tr->genericArgs.push_back(elemTy);
                    return tr;
                };
                // stdlib §5: type a deferred LINQ operator `m` whose source
                // element type is `srcElem`. ToArray materializes to Array<T>
                // (typed as a concrete Array<T> so inferred locals resolve
                // downstream member access). Returns true if `m` was a LINQ
                // operator (typed) — caller falls through otherwise.
                auto typeLinqOperator = [&](TypeRef* srcElem) -> bool {
                    if (m == "Where" || m == "Skip" || m == "Take" ||
                        m == "Distinct") {
                        ctx_.setExprType(&ce, makeLazyEnumerable(srcElem));
                        return true;
                    }
                    if (m == "OrderBy" || m == "OrderByDescending") {
                        ctx_.setExprType(&ce, makeLazyEnumerable(srcElem));
                        return true;
                    }
                    if (m == "Select" && !ce.args.empty() && ce.args[0].value) {
                        TypeRef* outElem = srcElem;
                        if (auto* lamTy = ctx_.exprType(*ce.args[0].value))
                            if (lamTy->kind == TypeRef::Kind::Function && lamTy->funcReturn)
                                outElem = lamTy->funcReturn;
                        ctx_.setExprType(&ce, makeLazyEnumerable(outElem));
                        return true;
                    }
                    // stdlib §5 Combination/Slicing: Concat/TakeWhile/SkipWhile
                    // preserve the source element type T (lazy IEnumerable<T>).
                    if (m == "Concat" || m == "TakeWhile" || m == "SkipWhile") {
                        ctx_.setExprType(&ce, makeLazyEnumerable(srcElem));
                        return true;
                    }
                    // stdlib §5 Projection: SelectMany(selector: (T)->IEnum<U>)
                    // flattens to a lazy IEnumerable<U>. The output element type U
                    // is the element type of the selector lambda's return type
                    // (a collection or lazy enumerable carrying U at genericArgs[0]).
                    if (m == "SelectMany" && !ce.args.empty() && ce.args[0].value) {
                        TypeRef* outElem = srcElem;
                        // issue #775 / §16.3: the selector may return a `Range<T>`
                        // (a range as the inner sequence) — its element type is
                        // genericArgs[0], exactly like a collection/lazy inner.
                        if (auto* lamTy = ctx_.exprType(*ce.args[0].value))
                            if (lamTy->kind == TypeRef::Kind::Function &&
                                lamTy->funcReturn &&
                                !lamTy->funcReturn->genericArgs.empty() &&
                                lamTy->funcReturn->genericArgs[0])
                                outElem = lamTy->funcReturn->genericArgs[0];
                        ctx_.setExprType(&ce, makeLazyEnumerable(outElem));
                        return true;
                    }
                    // stdlib §5 Combination: Zip<U,R>(other, combine: (T,U)->R)
                    // yields a lazy IEnumerable<R>. The output element type R is
                    // the combine lambda's return type (the 2nd arg here).
                    if (m == "Zip" && ce.args.size() >= 2 && ce.args[1].value) {
                        TypeRef* outElem = srcElem;
                        if (auto* lamTy = ctx_.exprType(*ce.args[1].value))
                            if (lamTy->kind == TypeRef::Kind::Function && lamTy->funcReturn)
                                outElem = lamTy->funcReturn;
                        ctx_.setExprType(&ce, makeLazyEnumerable(outElem));
                        return true;
                    }
                    // stdlib §5 Set-like lazy: Union/Intersect/Except(other)
                    // preserve the source element type T (lazy IEnumerable<T>).
                    if (m == "Union" || m == "Intersect" || m == "Except") {
                        ctx_.setExprType(&ce, makeLazyEnumerable(srcElem));
                        return true;
                    }
                    // stdlib §5 Ordering: ThenBy/ThenByDescending preserve T,
                    // return IOrderedEnumerable<T> (same lazy sentinel).
                    if (m == "ThenBy" || m == "ThenByDescending") {
                        ctx_.setExprType(&ce, makeLazyEnumerable(srcElem));
                        return true;
                    }
                    // stdlib §5 *OrNull: return nullable T. Use a nullable copy
                    // of srcElem (set nullable flag on a fresh TypeRef copy).
                    if ((m == "FirstOrNull" || m == "LastOrNull" ||
                         m == "SingleOrNull") && ce.args.empty()) {
                        if (srcElem) {
                            auto* nullTy = ctx_.makeTypeRef();
                            *nullTy = *srcElem;
                            nullTy->nullable = true;
                            ctx_.setExprType(&ce, nullTy);
                        }
                        return true;
                    }
                    // stdlib §5 (#650/#661/#789): FirstOrNull(pred) /
                    // LastOrNull(pred) / SingleOrNull(pred) on lazy receiver → T?.
                    if ((m == "FirstOrNull" || m == "LastOrNull" ||
                         m == "SingleOrNull") &&
                        ce.args.size() == 1) {
                        if (srcElem) {
                            auto* nullTy = ctx_.makeTypeRef();
                            *nullTy = *srcElem;
                            nullTy->nullable = true;
                            ctx_.setExprType(&ce, nullTy);
                        }
                        return true;
                    }
                    // stdlib §5 (#651): OfType<U>() on lazy receiver → lazy IEnum<U>.
                    if (m == "OfType" && ce.args.empty()) {
                        TypeRef* outElem = srcElem;
                        if (auto* callMa = dynamic_cast<const ast::MemberAccessExpr*>(
                                ce.callee.get())) {
                            if (!callMa->typeArgs.empty() && callMa->typeArgs[0]) {
                                if (TypeRef* u =
                                        ctx_.resolvedType(*callMa->typeArgs[0]))
                                    outElem = u;
                            }
                        }
                        ctx_.setExprType(&ce, makeLazyEnumerable(outElem));
                        return true;
                    }
                    // stdlib §5 (#651): GroupBy<TKey>(key) on lazy receiver.
                    if (m == "GroupBy" && ce.args.size() == 1) {
                        TypeRef* keyTy = srcElem;
                        if (!ce.args.empty() && ce.args[0].value) {
                            if (auto* lamTy = ctx_.exprType(*ce.args[0].value)) {
                                if (lamTy->kind == TypeRef::Kind::Function &&
                                    lamTy->funcReturn)
                                    keyTy = lamTy->funcReturn;
                            }
                        }
                        static const ast::Type* kIGroupingNode2 =
                            []() -> ast::Type* {
                                auto* qn = new ast::QualifiedName;
                                qn->parts.push_back("IGrouping");
                                auto* t = new ast::Type;
                                t->kind = ast::TypeKind::Named;
                                t->name.reset(qn);
                                return t;
                            }();
                        auto* grpTy = ctx_.makeTypeRef();
                        grpTy->kind = TypeRef::Kind::Named;
                        grpTy->declaration = nullptr;
                        grpTy->astNode = kIGroupingNode2;
                        if (keyTy)  grpTy->genericArgs.push_back(keyTy);
                        if (srcElem) grpTy->genericArgs.push_back(srcElem);
                        ctx_.setExprType(&ce, makeLazyEnumerable(grpTy));
                        return true;
                    }
                    return false;
                };
                // stdlib §5: chained LINQ on a lazy IEnumerable<T> receiver.
                // The receiver carries no declaration/astNode (the lazy
                // sentinel) but does carry the element type as genericArgs[0].
                bool isLazyRecv = recvTy && recvTy->kind == TypeRef::Kind::Named &&
                    !recvTy->declaration && !recvTy->astNode &&
                    !recvTy->genericArgs.empty();
                // issue #775 / §16.3: a `Range<T>` is IEnumerable<T>. It carries
                // the builtin Range class declaration (so it is not the lazy
                // sentinel above), but exposes the same LINQ operators/terminals
                // with element type genericArgs[0]. Type them through the same
                // block so a chained `(1..n).Where(…).Count()` is typed (its
                // result was otherwise left null → the downstream operator saw an
                // untyped receiver). Range's own members (Contains/GetEnumerator)
                // resolve through their dedicated handlers and are unaffected.
                bool isRangeLinqRecv =
                    recvTy && recvTy->kind == TypeRef::Kind::Named &&
                    recvTy->declaration && recvTy->declaration->name == "Range" &&
                    !recvTy->genericArgs.empty();
                if (isLazyRecv || isRangeLinqRecv) {
                    TypeRef* srcElem = recvTy->genericArgs[0];
                    if (m == "Any" || m == "All")
                        ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                    else if (m == "Count")
                        ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                    // fix #666 / stdlib §5/§5.1: Sum/Min/Max/Average on a lazy
                    // IEnumerable<decimal> return decimal, not int/double.
                    else if (m == "Sum" || m == "Min" || m == "Max") {
                        bool elemIsDecimal =
                            srcElem &&
                            srcElem->kind == TypeRef::Kind::Primitive &&
                            srcElem->primitive == P::Decimal;
                        if (!elemIsDecimal && !ce.args.empty() &&
                            ce.args[0].value) {
                            if (auto* lt = ctx_.exprType(*ce.args[0].value))
                                if (lt->kind == TypeRef::Kind::Function &&
                                    lt->funcReturn &&
                                    lt->funcReturn->kind == TypeRef::Kind::Primitive &&
                                    lt->funcReturn->primitive == P::Decimal)
                                    elemIsDecimal = true;
                        }
                        ctx_.setExprType(&ce,
                            elemIsDecimal
                                ? ctx_.primitiveTypeRef(P::Decimal)
                                : ctx_.primitiveTypeRef(P::Int));
                    }
                    // stdlib §5/§5.1: Average → double (int/long/double forms)
                    // or → decimal for IEnumerable<decimal>.
                    else if (m == "Average") {
                        bool elemIsDecimal =
                            srcElem &&
                            srcElem->kind == TypeRef::Kind::Primitive &&
                            srcElem->primitive == P::Decimal;
                        if (!elemIsDecimal && !ce.args.empty() &&
                            ce.args[0].value) {
                            if (auto* lt = ctx_.exprType(*ce.args[0].value))
                                if (lt->kind == TypeRef::Kind::Function &&
                                    lt->funcReturn &&
                                    lt->funcReturn->kind == TypeRef::Kind::Primitive &&
                                    lt->funcReturn->primitive == P::Decimal)
                                    elemIsDecimal = true;
                        }
                        ctx_.setExprType(&ce,
                            elemIsDecimal
                                ? ctx_.primitiveTypeRef(P::Decimal)
                                : ctx_.primitiveTypeRef(P::Double));
                    }
                    // stdlib §5 Aggregation: Aggregate(seed, fold) returns the
                    // accumulator type TAccum — the static type of the seed arg.
                    else if (m == "Aggregate" && !ce.args.empty() && ce.args[0].value) {
                        if (auto* st = ctx_.exprType(*ce.args[0].value))
                            ctx_.setExprType(&ce, st);
                    }
                    else if (m == "First" || m == "Last" || m == "Single" ||
                             m == "ElementAt")
                        ctx_.setExprType(&ce, srcElem);
                    // stdlib §5 *OrNull (lazy receiver): return nullable T.
                    else if ((m == "FirstOrNull" || m == "LastOrNull" ||
                              m == "SingleOrNull") && ce.args.empty()) {
                        if (srcElem) {
                            auto* nullTy = ctx_.makeTypeRef();
                            *nullTy = *srcElem;
                            nullTy->nullable = true;
                            ctx_.setExprType(&ce, nullTy);
                        }
                    }
                    else if (m == "ToArray" || m == "ToSet")
                        // Materializers (stdlib §5 + §14 inference fix).
                        // Return a concrete Array<T>/Set<T> TypeRef so that
                        // an un-annotated inferred local (`var b :=
                        // seq.ToArray()`) carries the right type for downstream
                        // `.Get(i)`, `.Length`, for-in, etc. Previously this
                        // returned the lazy sentinel (no astNode), causing:
                        //   • int element → wrong slot layout → wrong value
                        //   • record element → ICmp type mismatch → SIGABRT
                        // Codegen already knows to materialise via
                        // bz_lazy_to_array (via tryLowerLazyLinqCall); the
                        // concrete TypeRef here is only for sema inference.
                        ctx_.setExprType(&ce, makeConcreteCollectionResult(
                            m == "ToSet" ? "Set" : "Array", srcElem));
                    // stdlib §5 ToDictionary — typed as Dictionary<K,V> sentinel
                    // (fix #459 / §5 / §22): use makeDictResult so that inferred
                    // locals (`var d := seq.ToDictionary(keyFn)`) produce a type
                    // that isDictTypeName and colName recognize as a Dictionary
                    // receiver. K is the key lambda's return type; V is srcElem.
                    else if (m == "ToDictionary" && !ce.args.empty()) {
                        TypeRef* keyTy = nullptr;
                        if (ce.args[0].value) {
                            if (auto* lamTy = ctx_.exprType(*ce.args[0].value))
                                if (lamTy->kind == TypeRef::Kind::Function &&
                                    lamTy->funcReturn)
                                    keyTy = lamTy->funcReturn;
                        }
                        ctx_.setExprType(&ce, makeDictResult(keyTy, srcElem));
                    }
                    // stdlib §5 Ordering: Reverse() on a lazy receiver — preserves T.
                    // NOT added to typeLinqOperator (would clash with Array.Reverse
                    // in-place). Only typed here, where the receiver is already lazy.
                    else if (m == "Reverse" && ce.args.empty())
                        ctx_.setExprType(&ce, makeLazyEnumerable(srcElem));
                    else if (typeLinqOperator(srcElem)) { /* typed */ }
                }
                // §26.4 / fix #699: a method call on an `IEnumerable<T>`
                // receiver that is NOT the lazy LINQ sentinel — e.g.
                // `this.ToArray()` / `this.Count()` inside a generic
                // `extension IEnumerable<T>` body.  Here `this`'s TypeRef is
                // Named with declaration=null but a real astNode whose name is
                // "IEnumerable" and genericArgs[0]=T (the block-level type
                // parameter).  isLazyEnumerableType excludes this shape (astNode
                // != null), and the lazy-receiver block above requires the
                // no-astNode sentinel, so the LINQ materializers/aggregators are
                // otherwise left untyped — `ctx_.exprType(this.ToArray())` is
                // null.  The codegen inferred-local slot-type fix (#490) then
                // cannot pick a slot type, so `var items := this.ToArray()`
                // defaults to an i32 slot and `return items` emits `ret i32`
                // into the function's `ptr` (Array<T>) return type → LLVM
                // IR-verification failure.  Typing the common materializer and
                // aggregator methods on this receiver (exactly as on the lazy
                // sentinel) gives the inferred local its correct slot type.
                //
                // This makes the UNSPECIALIZED template body type-correct (a
                // dead-but-valid stub — §26.5 dispatches every real call to a
                // per-receiver-type instantiation), with NO codegen IR backstop.
                // It is equally correct for any future per-call-site
                // monomorphization, where the receiver carries the same
                // IEnumerable<concreteT> shape.
                bool ienumRecv =
                    recvTy && recvTy->kind == TypeRef::Kind::Named &&
                    !recvTy->genericArgs.empty() && recvTy->astNode &&
                    recvTy->astNode->name &&
                    !recvTy->astNode->name->parts.empty() &&
                    recvTy->astNode->name->parts[0] == "IEnumerable";
                if (ienumRecv) {
                    TypeRef* srcElem2 = recvTy->genericArgs[0];
                    if ((m == "ToArray" || m == "ToSet") && ce.args.empty())
                        ctx_.setExprType(&ce, makeConcreteCollectionResult(
                            m == "ToSet" ? "Set" : "Array", srcElem2));
                    else if ((m == "Any" || m == "All") && ce.args.size() <= 1)
                        ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                    else if (m == "Count")
                        ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                    else if (m == "First" || m == "Last" || m == "Single" ||
                             m == "ElementAt")
                        ctx_.setExprType(&ce, srcElem2);
                    else if (m == "Sum" || m == "Min" || m == "Max")
                        ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                    else if (m == "Average")
                        ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Double));
                    // issue #995 / stdlib §5: a DEFERRED LINQ operator
                    // (Where/Select/Take/Skip/Distinct/OrderBy/Concat/…) on a
                    // vtable-backed `IEnumerable<T>` receiver (interface-typed
                    // local/param, or a yield-method result) returns a lazy
                    // IEnumerable<U> — exactly as on a concrete collection or the
                    // lazy sentinel. Without typing it here the `Where(...)` call
                    // was left untyped, so the result was not the lazy sentinel and
                    // `lowerLazyLinqFor` / the LINQ-operator codegen rejected it
                    // (the #995 `for var x in seq.Where(...)` NYI / Where garbage).
                    // typeLinqOperator types Where/Select/… as makeLazyEnumerable;
                    // codegen's materialize-then-run path (lowerToLazyEnumerator's
                    // vtable bridge) drives the iterator vtable once into an
                    // Array<T> that the lazy operator then enumerates.
                    else if (typeLinqOperator(srcElem2)) { /* typed */ }
                }
                // stdlib §3.2: a method call on an `Encoding` receiver
                // (`Encoding.Utf8.GetBytes(s)` / `.GetString(b[,i,c])` / `.Name`).
                // The receiver's type is the Encoding TypeSymbol (set in
                // resolveMemberAccess), so colName is empty (the lambda only reads
                // astNode-backed builtins). Type the result so an inferred local
                // and chained member access resolve: GetBytes → owned Array<byte>
                // (carried structurally as a lazy-sentinel Named ref with byte at
                // genericArgs[0] — the same representation ToArray uses, which the
                // codegen and `.Length`/`.Get` paths recognize); GetString → string;
                // Name → string. Only Utf8 reaches here (the other singletons are
                // rejected at member access).
                if (recvTy && recvTy->kind == TypeRef::Kind::Named &&
                    recvTy->declaration &&
                    recvTy->declaration->name == "Encoding") {
                    if (m == "GetBytes") {
                        // fix #526 / stdlib §3.2: GetBytes returns owned
                        // Array<byte>. Use makeConcreteCollectionResult (which
                        // carries a synthetic "Array" astNode) rather than
                        // makeLazyEnumerable (which has no astNode). Without the
                        // astNode, isCollectionTypeName returns false and the
                        // .Count / .Length codegen path does not recognise the
                        // result as an array-backed collection, so .Count reads
                        // garbage. The concrete Array<byte> TypeRef is the same
                        // representation ToArray() uses (makeConcreteCollectionResult)
                        // and is recognized correctly by all downstream sema and
                        // codegen paths (isCollectionTypeName, bz_array_len, etc.).
                        ctx_.setExprType(
                            &ce, makeConcreteCollectionResult(
                                "Array", ctx_.primitiveTypeRef(P::Byte)));
                    } else if (m == "GetString" || m == "Name") {
                        ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::String));
                    }
                }
                if (!colName.empty()) {
                    // §4.5 (#1022/#920): an OWNING collection deliberately exposes
                    // NO non-owning value copy-form — `ToArray`/`ToSet`/
                    // `ToDictionary` would each manufacture a second collection over
                    // the same uniquely owned elements (double-owning them) or drain
                    // the source. The borrowing `Sorted`/`Reversed`/`AsSpan` forms
                    // are rejected separately below; this catches the materializer
                    // copy-forms, which were silently accepted and mis-reclaimed by
                    // codegen. Only a DIRECT owning-collection receiver triggers it —
                    // a LINQ query result (`owned.Where(p).ToArray()`) has an
                    // IEnumerable receiver, so colName != Owned* and it stays valid.
                    // `OwnedBuffer<T>` is exempt (§4.14 gives it owning spans) and is
                    // not in this set.
                    if ((colName == "OwnedArray" || colName == "OwnedStack" ||
                         colName == "OwnedQueue" || colName == "OwnedLinkedList" ||
                         colName == "OwnedSet" || colName == "OwnedDictionary") &&
                        (m == "ToArray" || m == "ToSet" || m == "ToDictionary")) {
                        ctx_.diagnostics().report(
                            Severity::Error, ce.span.begin,
                            std::string("`") + std::string(colName) +
                            "<T>` does not expose `" + std::string(m) +
                            "` — an owning collection has no non-owning copy-form "
                            "(copying would double-own its elements, §4.5); drain it "
                            "(`Drain`/`PopAt`/`PopFirst`/`PopLast`) to transfer "
                            "ownership out, or use a borrowing `Array<T>`");
                    }
                    // §22.5 / §4.x (#920 / #1103-B): a method call on an OWNING
                    // CONTAINER collection whose name is not part of that family's
                    // stdlib surface (nor a universal Object member, a borrowing
                    // LINQ operator, or the enumerable protocol) was silently
                    // ACCEPTED and mis-lowered — e.g. `arr.TakeAt(0)` (nonexistent),
                    // `arr.Set(0, v)` (`Set` is no collection's method), an arbitrary
                    // `arr.Frobnicate()`. Reject any name outside a GENEROUS union of
                    // every valid owning-collection method (over-inclusion never
                    // over-rejects — a name valid on a SIBLING owning type is allowed
                    // here too; that finer per-type precision is a follow-up). Scoped
                    // to the six owning CONTAINER collections (not OwnedBuffer / the
                    // owning iterators, whose surfaces differ). Only a DIRECT owning
                    // receiver triggers it; a LINQ query result has an IEnumerable
                    // receiver (colName != Owned*) and is unaffected.
                    bool isOwningContainer =
                        colName == "OwnedArray" || colName == "OwnedStack" ||
                        colName == "OwnedQueue" || colName == "OwnedLinkedList" ||
                        colName == "OwnedSet" || colName == "OwnedDictionary";
                    if (isOwningContainer) {
                        auto allowedOwningMethod = [](std::string_view mm) -> bool {
                            return
                                // §4.x native owning-collection surface (union over
                                // OwnedArray/Stack/Queue/LinkedList/Set/Dictionary).
                                mm == "Add" || mm == "AddRange" || mm == "AddFirst" ||
                                mm == "AddLast" || mm == "AddBefore" ||
                                mm == "AddAfter" || mm == "Insert" ||
                                mm == "Remove" || mm == "RemoveAt" ||
                                mm == "RemoveFirst" || mm == "RemoveLast" ||
                                mm == "RemoveNode" || mm == "Clear" ||
                                mm == "Contains" || mm == "ContainsKey" ||
                                mm == "IndexOf" || mm == "Find" || mm == "Sort" ||
                                mm == "Reverse" || mm == "Push" || mm == "Pop" ||
                                mm == "Peek" || mm == "TryPop" || mm == "TryPeek" ||
                                mm == "Enqueue" || mm == "Dequeue" ||
                                mm == "TryDequeue" || mm == "PopAt" ||
                                mm == "PopFirst" || mm == "PopLast" ||
                                mm == "TryPopFirst" || mm == "TryPopLast" ||
                                mm == "PopValue" || mm == "TryPopValue" ||
                                mm == "TryAdd" || mm == "Take" || mm == "TryTake" ||
                                mm == "Get" || mm == "Set" || mm == "TryGetValue" ||
                                mm == "GetOrAdd" ||
                                mm == "UnionWith" || mm == "IntersectWith" ||
                                mm == "ExceptWith" || mm == "IsSubsetOf" ||
                                mm == "IsSupersetOf" || mm == "Drain" ||
                                mm == "DrainAndProcess" ||
                                // common property/length accessors also usable in
                                // `.Count()` method form.
                                mm == "Count" || mm == "Capacity" ||
                                mm == "Length" || mm == "IsEmpty" ||
                                mm == "IsReadOnly" || mm == "Keys" ||
                                mm == "Values" || mm == "First" || mm == "Last" ||
                                mm == "FirstNode" || mm == "LastNode" ||
                                // universal Object members (§8.1) + IDisposable.
                                mm == "ToString" || mm == "Equals" ||
                                mm == "GetHashCode" || mm == "GetType" ||
                                mm == "Dispose" ||
                                // enumerable protocol (§4.12/§4.13).
                                mm == "GetEnumerator" ||
                                mm == "GetAsyncEnumerator" ||
                                mm == "MoveNext" || mm == "MoveNextAsync" ||
                                // borrowing LINQ operators (§5) — valid on any
                                // IEnumerable, incl. an owning collection. Included
                                // generously; the materializer copy-forms
                                // (ToArray/ToSet/ToDictionary) are rejected above for
                                // owning receivers but kept here so this check does
                                // not double-report them.
                                mm == "Where" || mm == "Select" ||
                                mm == "SelectMany" || mm == "Skip" ||
                                mm == "SkipWhile" || mm == "TakeWhile" ||
                                mm == "Distinct" || mm == "OrderBy" ||
                                mm == "OrderByDescending" || mm == "ThenBy" ||
                                mm == "ThenByDescending" || mm == "GroupBy" ||
                                mm == "Aggregate" || mm == "All" || mm == "Any" ||
                                mm == "Average" || mm == "Sum" || mm == "Min" ||
                                mm == "Max" || mm == "FirstOrNull" ||
                                mm == "LastOrNull" || mm == "Single" ||
                                mm == "SingleOrNull" || mm == "ElementAt" ||
                                mm == "Concat" || mm == "Append" || mm == "Union" ||
                                mm == "Intersect" || mm == "Except" || mm == "Zip" ||
                                mm == "SequenceEqual" || mm == "OfType" ||
                                mm == "Reversed" || mm == "Sorted" || mm == "Join" ||
                                mm == "ForEach" || mm == "AsReadOnly" ||
                                mm == "ToArray" || mm == "ToSet" ||
                                mm == "ToDictionary" || mm == "ToList";
                        };
                        if (!allowedOwningMethod(m)) {
                            ctx_.diagnostics().report(
                                Severity::Error, ce.span.begin,
                                std::string("no member `") + std::string(m) +
                                "` on `" + std::string(colName) +
                                "<T>` (§22.5 / §4.x) — it is not part of the owning "
                                "collection's surface, a universal Object member, or "
                                "a LINQ operator");
                        }
                    }
                    // Array/Stack/Queue/Set/LinkedList methods
                    bool isArrLike = colName == "Array" || colName == "OwnedArray" ||
                        colName == "Stack" || colName == "OwnedStack" ||
                        colName == "Queue" || colName == "OwnedQueue" ||
                        colName == "Set" || colName == "OwnedSet" ||
                        colName == "LinkedList" || colName == "OwnedLinkedList";
                    if (isArrLike) {
                        if (m == "Length" || m == "Count")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                        else if (m == "IsEmpty")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        else if (m == "Get" || m == "Peek" || m == "Pop" || m == "Dequeue" ||
                                 m == "PopLast" || m == "PopFirst") {
                            // Return type = element type (first generic arg)
                            // fix #642 / stdlib §4.5/§4.7: PopLast/PopFirst return owned T.
                            if (recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0])
                                ctx_.setExprType(&ce, recvTy->genericArgs[0]);
                        }
                        // fix #642 / stdlib §4.5: PopAt(index: int): owned T
                        else if (m == "PopAt" && !ce.args.empty()) {
                            if (recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0])
                                ctx_.setExprType(&ce, recvTy->genericArgs[0]);
                        }
                        // per §5 LINQ element access: First/Last/Single/ElementAt
                        // each return the source element type T (stdlib §5,
                        // `public method First(): T`). Typing the result lets an
                        // inferred local (`var p := a.First(pred)`) and member
                        // access on the call result (`a.First(pred).y`) resolve;
                        // without it the result stays untyped and `.y` is dropped.
                        else if (m == "First" || m == "Last" || m == "Single" ||
                                 m == "ElementAt") {
                            if (recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0])
                                ctx_.setExprType(&ce, recvTy->genericArgs[0]);
                        }
                        // per §3: Contains → bool, IndexOf → int
                        // per §4.10 stdlib: TryAdd(item: T): bool
                        // §4.11/§4.12 (#725): Stack.TryPop/TryPeek,
                        // Queue.TryDequeue/TryPeek (out item: T): bool.
                        else if (m == "Contains" || m == "TryAdd" ||
                                 m == "TryPop" || m == "TryDequeue" ||
                                 m == "TryPeek")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        else if (m == "IndexOf")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                        // per §5 LINQ: Any/All → bool, Count(pred) → int
                        else if (m == "Any" || m == "All")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        else if (m == "Count" && !ce.args.empty())
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                        // fix #666 / stdlib §5/§5.1: Sum/Min/Max/Average on
                        // Array<decimal> return decimal, not int/double.
                        // For the selector forms (Sum(fn), Average(fn)) check
                        // whether the selector lambda returns decimal; otherwise
                        // the element type drives the result.
                        else if (m == "Sum" || m == "Min" || m == "Max") {
                            bool elemIsDecimal =
                                recvTy && !recvTy->genericArgs.empty() &&
                                recvTy->genericArgs[0] &&
                                recvTy->genericArgs[0]->kind ==
                                    TypeRef::Kind::Primitive &&
                                recvTy->genericArgs[0]->primitive == P::Decimal;
                            // Check selector return type (if a lambda arg).
                            if (!elemIsDecimal && !ce.args.empty() &&
                                ce.args[0].value) {
                                if (auto* lt = ctx_.exprType(*ce.args[0].value))
                                    if (lt->kind == TypeRef::Kind::Function &&
                                        lt->funcReturn &&
                                        lt->funcReturn->kind ==
                                            TypeRef::Kind::Primitive &&
                                        lt->funcReturn->primitive == P::Decimal)
                                        elemIsDecimal = true;
                            }
                            ctx_.setExprType(&ce,
                                elemIsDecimal
                                    ? ctx_.primitiveTypeRef(P::Decimal)
                                    : ctx_.primitiveTypeRef(P::Int));
                        }
                        // stdlib §5/§5.1: Average → double (int/long/double forms)
                        // or → decimal for Array<decimal>.Average() / selector-decimal.
                        else if (m == "Average") {
                            bool elemIsDecimal =
                                recvTy && !recvTy->genericArgs.empty() &&
                                recvTy->genericArgs[0] &&
                                recvTy->genericArgs[0]->kind ==
                                    TypeRef::Kind::Primitive &&
                                recvTy->genericArgs[0]->primitive == P::Decimal;
                            if (!elemIsDecimal && !ce.args.empty() &&
                                ce.args[0].value) {
                                if (auto* lt = ctx_.exprType(*ce.args[0].value))
                                    if (lt->kind == TypeRef::Kind::Function &&
                                        lt->funcReturn &&
                                        lt->funcReturn->kind ==
                                            TypeRef::Kind::Primitive &&
                                        lt->funcReturn->primitive == P::Decimal)
                                        elemIsDecimal = true;
                            }
                            ctx_.setExprType(&ce,
                                elemIsDecimal
                                    ? ctx_.primitiveTypeRef(P::Decimal)
                                    : ctx_.primitiveTypeRef(P::Double));
                        }
                        // stdlib §5 Aggregation: Aggregate(seed, fold) returns the
                        // accumulator type TAccum = the static type of the seed.
                        else if (m == "Aggregate" && !ce.args.empty() && ce.args[0].value) {
                            if (auto* st = ctx_.exprType(*ce.args[0].value))
                                ctx_.setExprType(&ce, st);
                        }
                        // per §5 LINQ + §14 inference fix: ToArray/ToSet on a
                        // concrete Array/Set receiver. Return a concrete
                        // Array<T>/Set<T> TypeRef so that un-annotated inferred
                        // locals resolve downstream `.Get(i)`, `.Length`, etc.
                        else if ((m == "ToArray" || m == "ToSet") &&
                                 !recvTy->genericArgs.empty())
                            ctx_.setExprType(&ce, makeConcreteCollectionResult(
                                m == "ToSet" ? "Set" : "Array",
                                recvTy->genericArgs[0]));
                        // stdlib §3 / #472: Array<T>.Reversed() and
                        // Array<T>.Sorted() / Array<T>.Sorted(comparer) —
                        // value-returning copy-forms (past-participle convention).
                        // Return type: owned Array<T> (same element type as receiver).
                        // Only on Array<T> (not OwnedArray<T>); OwnedArray has unique
                        // ownership semantics that make copying its elements unsound.
                        // Sorted() without a comparer requires T: IComparable<T> —
                        // we check here that T is int or string (the concrete types
                        // implementing IComparable<T> in this milestone); all other
                        // element types require the explicit-comparer form.
                        else if (colName == "Array" && !recvTy->genericArgs.empty() &&
                                 m == "Reversed" && ce.args.empty()) {
                            ctx_.setExprType(&ce, makeConcreteCollectionResult(
                                "Array", recvTy->genericArgs[0]));
                        } else if (colName == "Array" && !recvTy->genericArgs.empty() &&
                                   m == "Sorted" && ce.args.empty()) {
                            // Constraint check: T must be IComparable<T>.  For
                            // the concrete subset supported (int, string), this is
                            // trivially satisfied.  For any other T, emit a
                            // compile-time diagnostic directing the user to the
                            // explicit-comparer form — per §3 / #472.
                            const TypeRef* elemTy472 = recvTy->genericArgs[0];
                            bool isComparableElem =
                                (elemTy472 && elemTy472->kind == TypeRef::Kind::Primitive &&
                                 (elemTy472->primitive == P::Int ||
                                  elemTy472->primitive == P::String ||
                                  elemTy472->primitive == P::Long ||
                                  elemTy472->primitive == P::Byte ||
                                  elemTy472->primitive == P::Double ||
                                  elemTy472->primitive == P::Float ||
                                  elemTy472->primitive == P::Char));
                            if (!isComparableElem) {
                                ctx_.diagnostics().report(
                                    Severity::Error, ce.span.begin,
                                    "Array<T>.Sorted() requires T to implement "
                                    "IComparable<T> (§3 / #472); T is not a "
                                    "built-in comparable type. "
                                    "Use Sorted(comparer: (T, T) -> int) instead.");
                            } else {
                                ctx_.setExprType(&ce, makeConcreteCollectionResult(
                                    "Array", recvTy->genericArgs[0]));
                            }
                        } else if (colName == "Array" && !recvTy->genericArgs.empty() &&
                                   m == "Sorted" && ce.args.size() == 1) {
                            // #542 / §3 / #472: validate that the comparer argument
                            // is a function — (T, T) -> int.  A non-function arg
                            // (e.g. an int literal) would be treated as a closure
                            // pointer by codegen, producing malformed IR / a crash.
                            if (ce.args[0].value) {
                                const TypeRef* compTy =
                                    ctx_.exprType(*ce.args[0].value);
                                if (compTy &&
                                    compTy->kind != TypeRef::Kind::Function &&
                                    compTy->kind != TypeRef::Kind::Error) {
                                    ctx_.diagnostics().report(
                                        Severity::Error, ce.span.begin,
                                        "Sorted expects a comparer function "
                                        "`(T, T) -> int` as its argument "
                                        "(stdlib §3 / #472 / #542); "
                                        "got a non-function value instead.");
                                    return;
                                }
                            }
                            ctx_.setExprType(&ce, makeConcreteCollectionResult(
                                "Array", recvTy->genericArgs[0]));
                        }
                        // #542 / §3 / #472: Sort(comparer: (T,T)->int) in-place
                        // sort with user-supplied comparer.  The codegen path for
                        // Sort(comparer) calls lowerExpr on the arg and then treats
                        // the result as a closure pointer; a non-function arg (e.g.
                        // an int literal) would produce malformed IR / a crash.
                        // Validate here so the user gets a clear sema error.
                        if ((colName == "Array" || colName == "OwnedArray") &&
                            m == "Sort" && ce.args.size() == 1 && ce.args[0].value) {
                            const TypeRef* cmpTy =
                                ctx_.exprType(*ce.args[0].value);
                            if (cmpTy &&
                                cmpTy->kind != TypeRef::Kind::Function &&
                                cmpTy->kind != TypeRef::Kind::Error) {
                                ctx_.diagnostics().report(
                                    Severity::Error, ce.span.begin,
                                    "Sort expects a comparer function "
                                    "`(T, T) -> int` as its argument "
                                    "(stdlib §3 / #542); "
                                    "got a non-function value instead.");
                                return;
                            }
                        }
                        // stdlib §3 / #472: Reversed() and Sorted() are NOT defined
                        // on OwnedArray<T> — unique ownership makes a shallow copy
                        // unsound (elements are owned; a bitwise copy would double-
                        // free). Emit a clear diagnostic so the user knows to convert
                        // to Array<T> first (e.g. with ToArray()).
                        if (colName == "OwnedArray" &&
                            (m == "Reversed" || m == "Sorted")) {
                            ctx_.diagnostics().report(
                                Severity::Error, ce.span.begin,
                                std::string(m) +
                                "() is not defined on OwnedArray<T> (§3 / #472). "
                                "OwnedArray<T> elements are uniquely owned; a shallow "
                                "copy would produce two owners of the same object, "
                                "violating §28.3 exactly-once disposal. "
                                "Convert to Array<T> first (e.g. with ToArray()) "
                                "then call " + std::string(m) + "().");
                            return;
                        }
                        // stdlib §5 ToDictionary — typed as Dictionary<K,V> sentinel
                        // (fix #459 / §5 / §22): same as the lazy-source path above.
                        else if (m == "ToDictionary" && !ce.args.empty() &&
                                 !recvTy->genericArgs.empty()) {
                            TypeRef* keyTy = nullptr;
                            if (ce.args[0].value) {
                                if (auto* lamTy = ctx_.exprType(*ce.args[0].value))
                                    if (lamTy->kind == TypeRef::Kind::Function &&
                                        lamTy->funcReturn)
                                        keyTy = lamTy->funcReturn;
                            }
                            ctx_.setExprType(&ce,
                                makeDictResult(keyTy, recvTy->genericArgs[0]));
                        }
                        // stdlib §5 *OrNull (Array receiver): return nullable T.
                        else if ((m == "FirstOrNull" || m == "LastOrNull" ||
                                  m == "SingleOrNull") && ce.args.empty() &&
                                 !recvTy->genericArgs.empty() &&
                                 recvTy->genericArgs[0]) {
                            auto* nullTy = ctx_.makeTypeRef();
                            *nullTy = *recvTy->genericArgs[0];
                            nullTy->nullable = true;
                            ctx_.setExprType(&ce, nullTy);
                        }
                        // stdlib §5 (#650/#661/#789): FirstOrNull(predicate) /
                        // LastOrNull(predicate) / SingleOrNull(predicate) — returns
                        // T? for the predicate form (the no-predicate form is above).
                        else if ((m == "FirstOrNull" || m == "LastOrNull" ||
                                  m == "SingleOrNull") &&
                                 ce.args.size() == 1 &&
                                 !recvTy->genericArgs.empty() &&
                                 recvTy->genericArgs[0]) {
                            auto* nullTy = ctx_.makeTypeRef();
                            *nullTy = *recvTy->genericArgs[0];
                            nullTy->nullable = true;
                            ctx_.setExprType(&ce, nullTy);
                        }
                        // stdlib §5 (#651): OfType<U>() — lazy IEnumerable<U>.
                        // U comes from the explicit type arg on the callee
                        // MemberAccessExpr. When U == T (same source element type),
                        // codegen produces a Where-all identity; when U != T for
                        // value types, produces an empty enumerator.
                        else if (m == "OfType" && ce.args.empty()) {
                            TypeRef* outElem = recvTy->genericArgs.empty()
                                ? nullptr : recvTy->genericArgs[0];
                            // Extract explicit type arg <U> from the callee MA.
                            if (auto* callMa = dynamic_cast<const ast::MemberAccessExpr*>(
                                    ce.callee.get())) {
                                if (!callMa->typeArgs.empty() &&
                                    callMa->typeArgs[0]) {
                                    if (TypeRef* u =
                                            ctx_.resolvedType(*callMa->typeArgs[0]))
                                        outElem = u;
                                }
                            }
                            ctx_.setExprType(&ce, makeLazyEnumerable(outElem));
                        }
                        // stdlib §5 (#651): GroupBy<TKey>(key: (T)->TKey) —
                        // returns IEnumerable<IGrouping<TKey, T>>.
                        // Model IGrouping<TKey, T> as a synthetic lazy element
                        // TypeRef. The key type (TKey) comes from the key lambda's
                        // return type. Codegen identifies "IGrouping" by the
                        // synthetic astNode name and materialization method.
                        else if (m == "GroupBy" && ce.args.size() == 1 &&
                                 !recvTy->genericArgs.empty()) {
                            TypeRef* srcElem = recvTy->genericArgs[0];
                            TypeRef* keyTy   = srcElem; // default: identity key
                            if (!ce.args.empty() && ce.args[0].value) {
                                if (auto* lamTy =
                                        ctx_.exprType(*ce.args[0].value)) {
                                    if (lamTy->kind == TypeRef::Kind::Function &&
                                        lamTy->funcReturn)
                                        keyTy = lamTy->funcReturn;
                                }
                            }
                            // Build IGrouping<TKey, T> TypeRef using a synthetic
                            // astNode so codegen can identify it.
                            static const ast::Type* kIGroupingNode =
                                []() -> ast::Type* {
                                    auto* qn = new ast::QualifiedName;
                                    qn->parts.push_back("IGrouping");
                                    auto* t = new ast::Type;
                                    t->kind = ast::TypeKind::Named;
                                    t->name.reset(qn);
                                    return t;
                                }();
                            auto* grpTy = ctx_.makeTypeRef();
                            grpTy->kind = TypeRef::Kind::Named;
                            grpTy->declaration = nullptr;
                            grpTy->astNode = kIGroupingNode;
                            if (keyTy)  grpTy->genericArgs.push_back(keyTy);
                            if (srcElem) grpTy->genericArgs.push_back(srcElem);
                            ctx_.setExprType(&ce, makeLazyEnumerable(grpTy));
                        }
                        // per §5 LINQ: Where/Select/Skip/Take/Distinct/OrderBy
                        // are LAZY — they return a deferred IEnumerable<T>, not
                        // a materialized Array<T> (stdlib §5).
                        else if (!recvTy->genericArgs.empty() &&
                                 typeLinqOperator(recvTy->genericArgs[0])) {
                            /* typed as lazy IEnumerable<T> */
                        }
                        // Add/Push/Enqueue/Set/RemoveAt/Clear → unit (void)
                    }
                    // §4.15 (#549): Array<T>.AsSpan() / AsSpan(start) /
                    // AsSpan(start,length) / AsSpan(range) → ArraySpan<T>.
                    // Array<T>.AsReadOnlySpan() and overloads → ReadOnlyArraySpan<T>.
                    // Only on Array<T> (the borrowing collection); OwnedArray<T>
                    // does not expose AsSpan (its elements are owned — see §4.5).
                    // §4.14 (#779): OwnedBuffer<T>.AsSpan() → the OWNING Span<T>;
                    // OwnedBuffer<T>.AsReadOnlySpan() → ReadOnlySpan<T>. These are
                    // distinct from Array<T>'s borrowing ArraySpan<T> (§4.15): the
                    // owning span's class-typed slots own their elements, so it
                    // exposes Clear/MoveFrom/MoveTo + a disposing indexer-set +
                    // `move sp[i]`, and forbids Fill/CopyFrom/CopyTo on class-T.
                    // The runtime struct layout ({ ptr, i32, i32 }) is identical,
                    // so codegen routes both through tryLowerArraySpanCall and the
                    // shared isSpanLikeTypeName surface; the owning behaviour is
                    // gated on the "Span" name (isOwningSpanTypeName).
                    if ((colName == "Array" || colName == "OwnedBuffer" ||
                         colName == "OwnedArray") &&
                        !recvTy->genericArgs.empty() &&
                        recvTy->genericArgs[0] &&
                        (m == "AsSpan" || m == "AsReadOnlySpan") &&
                        (ce.args.size() <= 2)) {
                        // §4.5 (#1022): OwnedArray<T> does NOT expose the borrowing
                        // span views — its elements are uniquely owned, so a span
                        // (whose `sp[i] := x` would overwrite an owned slot without
                        // disposal, §28.11) is deliberately not provided. The gate
                        // previously excluded OwnedArray, but the call then fell
                        // through and was accepted, and codegen SILENTLY miscompiled
                        // it to a zeroed span (returns 0 for every index). Reject it
                        // here; the span return type is still set below so the rest
                        // of the typecheck is well-formed (the program is rejected
                        // via the diagnostic).
                        if (colName == "OwnedArray") {
                            ctx_.diagnostics().report(
                                Severity::Error, ce.span.begin,
                                std::string("`OwnedArray<T>` does not expose `") +
                                std::string(m) +
                                "` — an owning collection's elements are uniquely "
                                "owned, so a span view (which can overwrite slots) "
                                "is not provided (§4.5); use a borrowing `Array<T>`,"
                                " or `Drain` / `PopAt` to transfer ownership out");
                        }
                        bool owning = (colName == "OwnedBuffer");
                        // Build synthetic span-type TypeRefs. Array → borrowing
                        // ArraySpan/ReadOnlyArraySpan (§4.15); OwnedBuffer → owning
                        // Span/ReadOnlySpan (§4.14).
                        static const ast::Type* kArraySpanNode = []() -> ast::Type* {
                            auto* qn = new ast::QualifiedName;
                            qn->parts.push_back("ArraySpan");
                            auto* t = new ast::Type;
                            t->kind = ast::TypeKind::Named;
                            t->name.reset(qn);
                            return t;
                        }();
                        static const ast::Type* kROArraySpanNode = []() -> ast::Type* {
                            auto* qn = new ast::QualifiedName;
                            qn->parts.push_back("ReadOnlyArraySpan");
                            auto* t = new ast::Type;
                            t->kind = ast::TypeKind::Named;
                            t->name.reset(qn);
                            return t;
                        }();
                        static const ast::Type* kSpanNode = []() -> ast::Type* {
                            auto* qn = new ast::QualifiedName;
                            qn->parts.push_back("Span");
                            auto* t = new ast::Type;
                            t->kind = ast::TypeKind::Named;
                            t->name.reset(qn);
                            return t;
                        }();
                        static const ast::Type* kROSpanNode = []() -> ast::Type* {
                            auto* qn = new ast::QualifiedName;
                            qn->parts.push_back("ReadOnlySpan");
                            auto* t = new ast::Type;
                            t->kind = ast::TypeKind::Named;
                            t->name.reset(qn);
                            return t;
                        }();
                        const ast::Type* node =
                            (m == "AsSpan")
                                ? (owning ? kSpanNode   : kArraySpanNode)
                                : (owning ? kROSpanNode : kROArraySpanNode);
                        auto* tr = ctx_.makeTypeRef();
                        tr->kind = TypeRef::Kind::Named;
                        tr->declaration = nullptr;
                        tr->astNode = node;
                        tr->genericArgs.push_back(recvTy->genericArgs[0]);
                        ctx_.setExprType(&ce, tr);
                    }
                    // per §4.6/§4.7: LinkedList<T> / OwnedLinkedList<T> node-based API.
                    // AddFirst/AddLast/AddBefore/AddAfter return LinkedListNode<T>.
                    // Find returns LinkedListNode<T>? (nullable).
                    // FirstNode/LastNode properties return LinkedListNode<T>?.
                    // First/Last return T? (nullable element).
                    // RemoveFirst/RemoveLast/RemoveNode → unit.
                    if ((colName == "LinkedList" || colName == "OwnedLinkedList") &&
                        recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
                        auto* elemTy = recvTy->genericArgs[0];
                        // Helper: make a LinkedListNode<T> TypeRef (non-null borrow).
                        auto makeNodeTy = [&]() -> TypeRef* {
                            auto* nt = ctx_.makeTypeRef();
                            nt->kind = TypeRef::Kind::Named;
                            nt->nullable = false;
                            // Build an ast::Type for "LinkedListNode<T>" so
                            // codegen can identify the node type by name.
                            // We don't copy the inner element's astNode —
                            // codegen only needs the top-level name.
                            auto* ann = new ast::Type();
                            ann->kind = ast::TypeKind::Named;
                            auto* nm = new ast::QualifiedName();
                            nm->parts.push_back("LinkedListNode");
                            ann->name.reset(nm);
                            nt->astNode = ann;
                            // Forward the element TypeRef as a generic arg.
                            nt->genericArgs.push_back(elemTy);
                            return nt;
                        };
                        auto makeNullableNodeTy = [&]() -> TypeRef* {
                            auto* nt = makeNodeTy();
                            nt->nullable = true;
                            return nt;
                        };
                        if (m == "AddFirst" || m == "AddLast" ||
                            m == "AddBefore" || m == "AddAfter") {
                            ctx_.setExprType(&ce, makeNodeTy());
                        } else if (m == "Find") {
                            ctx_.setExprType(&ce, makeNullableNodeTy());
                        } else if (m == "Remove" && ce.args.size() == 1) {
                            // Remove(item: T): bool — per §4.6
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        }
                        // First/Last value properties (T? — nullable element)
                        // are property accesses, not call expressions — typed in
                        // the member-access typechecker (ExprTypecheck_Calls.cpp
                        // §2194 property section and LlvmBackend_Exprs.cpp).
                    }
                    // per §4.6: LinkedListNode<T> accessor methods / properties.
                    if (colName == "LinkedListNode" &&
                        recvTy && !recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
                        // .Value: T — accessed as a property; typed in member handler.
                        // .Next / .Previous: LinkedListNode<T>? — same.
                        // No call-expression methods on the node type per §4.6.
                    }
                    // issue #1002 / §29.3 / stdlib §4.4/§4.12: a borrowing
                    // collection's `GetEnumerator()` returns `owned
                    // IEnumerator<T>` (declared on IEnumerable<T>, which Array<T>
                    // and OwnedArray<T> implement). Type the result as the
                    // *builtin* IEnumerator<T> sentinel (a Named TypeRef with no
                    // declaration, astNode name = "IEnumerator", genericArgs =
                    // [T]) so the manual-protocol typing for the #494 fix fires:
                    // MoveNext() is then typed bool (not the i32 default → no
                    // `br i32` IR_FAIL) and `e.Current` is typed T. Codegen
                    // lowers the call by wrapping the array as the §29.11 (#943)
                    // iterator-vtable enumerator, whose slots 1/2/3 are
                    // Dispose/MoveNext/Current — exactly what the #494 dispatch
                    // and the slot-1 using-var disposal expect. Iterating
                    // OwnedArray<T> through IEnumerable<T> is borrowing (§4.3:
                    // shared read-only interfaces yield borrows), so Current is
                    // non-destructive here.
                    if ((colName == "Array" || colName == "OwnedArray") &&
                        m == "GetEnumerator" && ce.args.empty() &&
                        !recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
                        static const ast::Type* kIEnumeratorNode =
                            []() -> ast::Type* {
                            auto* qn = new ast::QualifiedName;
                            qn->parts.push_back("IEnumerator");
                            auto* t = new ast::Type;
                            t->kind = ast::TypeKind::Named;
                            t->name.reset(qn);
                            return t;
                        }();
                        auto* tr = ctx_.makeTypeRef();
                        tr->kind = TypeRef::Kind::Named;
                        tr->declaration = nullptr;
                        tr->astNode = kIEnumeratorNode;
                        tr->genericArgs.push_back(recvTy->genericArgs[0]);
                        ctx_.setExprType(&ce, tr);
                    }
                    // #1102C / §29.11: `GetEnumerator()` on a vtable-backed
                    // IEnumerable<T> / OwnedEnumerable<T> (a yield-method result
                    // or an interface-typed value, NOT the array-backed
                    // collections handled above) returns IEnumerator<T>. Type the
                    // result as the builtin IEnumerator<T> sentinel so the
                    // manual-protocol MoveNext()→bool typing (#494, below) fires —
                    // without this `var b := it.MoveNext()` inferred `b` as the
                    // i32 default and `if b` emitted a malformed `br i32`. Codegen
                    // returns the receiver unchanged (the source iterator struct
                    // IS the enumerator; its vtable slot 2 = MoveNext).
                    if ((colName == "IEnumerable" || colName == "OwnedEnumerable") &&
                        m == "GetEnumerator" && ce.args.empty() &&
                        !recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
                        static const ast::Type* kIEnumeratorNode2 =
                            []() -> ast::Type* {
                            auto* qn = new ast::QualifiedName;
                            qn->parts.push_back("IEnumerator");
                            auto* t = new ast::Type;
                            t->kind = ast::TypeKind::Named;
                            t->name.reset(qn);
                            return t;
                        }();
                        auto* tr = ctx_.makeTypeRef();
                        tr->kind = TypeRef::Kind::Named;
                        tr->declaration = nullptr;
                        tr->astNode = kIEnumeratorNode2;
                        tr->genericArgs.push_back(recvTy->genericArgs[0]);
                        ctx_.setExprType(&ce, tr);
                    }
                    // issue #494 / §29.2 / §29.3: builtin IEnumerator<T> /
                    // OwnedEnumerator<T> call-expression methods.
                    //   MoveNext(): bool — advances the iterator (vtable slot 2)
                    // (Current is a readonly property, typed in resolveMemberAccess.)
                    if (colName == "IEnumerator" || colName == "OwnedEnumerator") {
                        if (m == "MoveNext" && ce.args.empty())
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        // Dispose() → unit; no type needed.
                    }
                    // issue #759 / §29.3 / §30.7 / stdlib §4.13: builtin
                    // IAsyncEnumerator<T> / OwnedAsyncEnumerator<T> manual
                    // protocol. MoveNextAsync(): Task<bool> — advances the async
                    // iterator (vtable slot 2). The `await` typer then peels
                    // Task<bool> → bool. (Current is a readonly property typed in
                    // resolveMemberAccess below.) makeTaskTypeRef is defined later
                    // in this function (out of scope here), so build Task<bool>
                    // inline — a Named "Task" ast::Type with genericArgs=[Bool].
                    if (colName == "IAsyncEnumerator" ||
                        colName == "OwnedAsyncEnumerator") {
                        if (m == "MoveNextAsync" && ce.args.empty()) {
                            auto* taskBool = ctx_.makeTypeRef();
                            taskBool->kind = TypeRef::Kind::Named;
                            taskBool->nullable = false;
                            auto* ann = new ast::Type();
                            ann->kind = ast::TypeKind::Named;
                            auto* nm = new ast::QualifiedName();
                            nm->parts.push_back("Task");
                            ann->name.reset(nm);
                            taskBool->astNode = ann;
                            taskBool->genericArgs.push_back(
                                ctx_.primitiveTypeRef(P::Bool));
                            ctx_.setExprType(&ce, taskBool);
                        }
                        // DisposeAsync()/Dispose() → Task/unit; no type needed.
                    }
                    // issue #757 / §30.7: a direct GetAsyncEnumerator(token) on a
                    // builtin IAsyncEnumerable<T> returns IAsyncEnumerator<T> — the
                    // async-iterator frame, the same value `await for` iterates.
                    // Without typing the call the `await using` slot is sized i32
                    // (not ptr) and scope-exit disposal loads a garbage pointer →
                    // SIGSEGV. Build the result type inline (Named "IAsyncEnumerator"
                    // carrying the source's element type T) exactly like MoveNextAsync
                    // builds Task<bool> above.
                    if (colName == "IAsyncEnumerable" ||
                        colName == "OwnedAsyncEnumerable") {
                        if (m == "GetAsyncEnumerator") {
                            auto* enumTy = ctx_.makeTypeRef();
                            enumTy->kind = TypeRef::Kind::Named;
                            enumTy->nullable = false;
                            auto* ann = new ast::Type();
                            ann->kind = ast::TypeKind::Named;
                            auto* nm = new ast::QualifiedName();
                            nm->parts.push_back("IAsyncEnumerator");
                            ann->name.reset(nm);
                            enumTy->astNode = ann;
                            if (recvTy && !recvTy->genericArgs.empty() &&
                                recvTy->genericArgs[0])
                                enumTy->genericArgs.push_back(
                                    recvTy->genericArgs[0]);
                            ctx_.setExprType(&ce, enumTy);
                        }
                    }
                    // Dictionary methods
                    if (colName == "Dictionary" || colName == "OwnedDictionary") {
                        if (m == "Count")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                        else if (m == "ContainsKey" || m == "Remove" || m == "TryGetValue" ||
                                 m == "TryPopValue")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        // fix #643 / stdlib §4.9: PopValue(key): owned V — removes
                        // and returns the owned value; raises if absent.
                        else if (m == "PopValue" && recvTy &&
                                 recvTy->genericArgs.size() > 1 &&
                                 recvTy->genericArgs[1]) {
                            ctx_.setExprType(&ce, recvTy->genericArgs[1]);
                        }
                        // stdlib §4.8 / §4.9: GetOrAdd(key, factory): V — the
                        // result is the value type V (the second generic arg). For
                        // OwnedDictionary the result is a BORROW of the stored value
                        // (§4.9 "returns borrow"); for the borrowing Dictionary it is
                        // the stored value (§4.8). Either way the static type is V,
                        // which the ownership checker treats as Borrowed (no disposal
                        // obligation) since GetOrAdd has no resolved `owned` return.
                        //
                        // #542: validate that the factory argument (args[1]) is a
                        // function/lambda type — (K) -> V.  A non-function second arg
                        // (e.g. an int literal) must be rejected here; without this
                        // check sema silently emits no diagnostic and codegen
                        // treats the integer value as a closure pointer, producing
                        // malformed IR / a crash (stdlib §4.8 / §4.9).
                        else if (m == "GetOrAdd" && recvTy &&
                                 recvTy->genericArgs.size() > 1 &&
                                 recvTy->genericArgs[1]) {
                            // per §4.8 / §4.9: the second argument must be a factory
                            // function `(K) -> V`.  Check it when the arg is present
                            // and its type has been resolved.
                            if (ce.args.size() >= 2 && ce.args[1].value) {
                                const TypeRef* factTy =
                                    ctx_.exprType(*ce.args[1].value);
                                if (factTy &&
                                    factTy->kind != TypeRef::Kind::Function &&
                                    factTy->kind != TypeRef::Kind::Error) {
                                    ctx_.diagnostics().report(
                                        Severity::Error, ce.span.begin,
                                        "GetOrAdd expects a factory function "
                                        "`(K) -> V` as its second argument "
                                        "(stdlib §4.8 / §4.9 / #542); "
                                        "got a non-function value instead.");
                                    return;
                                }
                            }
                            ctx_.setExprType(&ce, recvTy->genericArgs[1]);
                        }
                        // #628 / stdlib §4.9 v0.135: OwnedDictionary<K,V>.Drain()
                        // returns OwnedEnumerable<DrainedEntry<K,V>>.
                        // DrainedEntry<K,V> is a synthetic Named TypeRef with
                        // importedName="DrainedEntry" and genericArgs=[K,V]; it is
                        // wrapped in OwnedEnumerable<> so the for-loop sema sets
                        // the loop variable's type to DrainedEntry<K,V> (via the
                        // first generic arg of OwnedEnumerable<T>).
                        // codegen's lowerDictDrainFor detects this iterable shape.
                        else if (colName == "OwnedDictionary" &&
                                 m == "Drain" && ce.args.empty() &&
                                 recvTy &&
                                 recvTy->genericArgs.size() >= 2 &&
                                 recvTy->genericArgs[0] &&
                                 recvTy->genericArgs[1]) {
                            // Build the DrainedEntry<K,V> TypeRef (importedName tag).
                            static const char kDrainedEntryName[] = "DrainedEntry";
                            auto* entryTy = ctx_.makeTypeRef();
                            entryTy->kind = TypeRef::Kind::Named;
                            entryTy->declaration = nullptr;
                            entryTy->astNode = nullptr;
                            entryTy->importedName = kDrainedEntryName;
                            entryTy->genericArgs.push_back(recvTy->genericArgs[0]); // K
                            entryTy->genericArgs.push_back(recvTy->genericArgs[1]); // V
                            // Wrap in OwnedEnumerable<DrainedEntry<K,V>>.
                            // Use a static astNode so lowerIEnumerableBuiltinFor can
                            // see the name "OwnedEnumerable" and route to the drain
                            // handler (lowerDictDrainFor checks for DrainedEntry in
                            // the first generic arg BEFORE the builtin path fires).
                            static const ast::Type* kOwnedEnumNode = []() -> ast::Type* {
                                auto* qn = new ast::QualifiedName;
                                qn->parts.push_back("OwnedEnumerable");
                                auto* t = new ast::Type;
                                t->kind = ast::TypeKind::Named;
                                t->name.reset(qn);
                                return t;
                            }();
                            auto* oenTy = ctx_.makeTypeRef();
                            oenTy->kind = TypeRef::Kind::Named;
                            oenTy->declaration = nullptr;
                            oenTy->astNode = kOwnedEnumNode;
                            oenTy->genericArgs.push_back(entryTy);
                            ctx_.setExprType(&ce, oenTy);
                        }
                    }
                    // #639 / stdlib §4.5/§4.7/§4.11 v0.135:
                    // OwnedArray<T>.Drain(), OwnedLinkedList<T>.Drain(),
                    // OwnedStack<T>.Drain(), OwnedQueue<T>.Drain() all
                    // return OwnedEnumerable<T>.
                    // T is the first (and only) generic argument of the receiver.
                    // codegen's lowerOwnedSeqDrainFor handles all four;
                    // it detects the collection kind from the receiver type name.
                    if ((colName == "OwnedArray" ||
                         colName == "OwnedLinkedList" ||
                         colName == "OwnedStack" ||
                         colName == "OwnedQueue") &&
                        m == "Drain" && ce.args.empty() &&
                        recvTy &&
                        !recvTy->genericArgs.empty() &&
                        recvTy->genericArgs[0]) {
                        // Shared static astNode for OwnedEnumerable<T>.
                        static const ast::Type* kOwnedEnumSeqNode =
                            []() -> ast::Type* {
                            auto* qn = new ast::QualifiedName;
                            qn->parts.push_back("OwnedEnumerable");
                            auto* t = new ast::Type;
                            t->kind = ast::TypeKind::Named;
                            t->name.reset(qn);
                            return t;
                        }();
                        auto* oenTy = ctx_.makeTypeRef();
                        oenTy->kind = TypeRef::Kind::Named;
                        oenTy->declaration = nullptr;
                        oenTy->astNode = kOwnedEnumSeqNode;
                        oenTy->genericArgs.push_back(recvTy->genericArgs[0]); // T
                        ctx_.setExprType(&ce, oenTy);
                    }
                    // StringBuilder methods
                    if (colName == "StringBuilder") {
                        if (m == "ToString")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::String));
                        else if (m == "Length" || m == "Count")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                        else if (m == "Append" || m == "AppendLine" ||
                                 m == "Clear" || m == "Insert" ||
                                 m == "Remove" || m == "Replace")
                            ctx_.setExprType(&ce, recvTy);  // returns same StringBuilder (chaining, §3.1)
                    }
                    // Random instance methods
                    if (colName == "Random") {
                        if (m == "Next")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                        else if (m == "NextDouble")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Double));
                    }
                    // string static methods (§9.6)
                    if (colName == "string" || colName == "String") {
                        if (m == "Join")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::String));
                        // §9.6 / #1145: the single-argument `String.From`
                        // overloads accept only `char`, `CString`, or `WString`
                        // (the char-array form is the three-arg
                        // `From(buffer, start, count)`). A 1-arg call with any
                        // other argument — e.g. `From(Array<char>)` — has no
                        // matching overload (Bezel omits implicit conversions,
                        // rationale §5.1). Without this guard the call
                        // mis-lowered as `From(char)`, sign-extending the
                        // argument's pointer to i32 → malformed IR. Reject it
                        // here with a clean no-overload diagnostic.
                        if (m == "From" && ce.args.size() == 1 &&
                            ce.args[0].value) {
                            TypeRef* at = ctx_.exprType(*ce.args[0].value);
                            bool ok = false;
                            if (at && at->kind == TypeRef::Kind::Primitive &&
                                at->primitive == P::Char)
                                ok = true;
                            else if (at && at->kind == TypeRef::Kind::Named &&
                                     at->declaration &&
                                     (at->declaration->name == "CString" ||
                                      at->declaration->name == "WString"))
                                ok = true;
                            if (at && !ok)
                                ctx_.diagnostics().report(
                                    Severity::Error, ce.span.begin,
                                    "no matching overload of `String.From` — "
                                    "the single-argument form takes a `char` "
                                    "(or an FFI `CString`/`WString`); to build "
                                    "a string from a char array use "
                                    "`String.From(buffer, start, count)` "
                                    "(§9.6)");
                        }
                    }
                    // File static methods (§7.3)
                    if (colName == "File") {
                        if (m == "ReadAllText")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::String));
                        else if (m == "Exists" || m == "Delete")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        // §7.3: ReadAllBytes → owned Array<byte>.
                        else if (m == "ReadAllBytes")
                            ctx_.setExprType(
                                &ce, makeConcreteCollectionResult(
                                    "Array", ctx_.primitiveTypeRef(P::Byte)));
                        // ReadAllLines → owned Array<string>.
                        else if (m == "ReadAllLines")
                            ctx_.setExprType(
                                &ce, makeConcreteCollectionResult(
                                    "Array", ctx_.primitiveTypeRef(P::String)));
                        // WriteAllBytes/WriteAllLines/Copy/Move/AppendAllText: void.
                        // (no setExprType needed for void-returning calls)
                    }
                    // Path static methods
                    if (colName == "Path") {
                        if (m == "Combine" || m == "GetFileName" ||
                            m == "GetExtension" || m == "GetFileNameWithoutExtension" ||
                            m == "GetDirectoryName" || m == "GetFullPath")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::String));
                    }
                    if (colName == "Directory") {
                        if (m == "Exists")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                    }
                    // §2.5: DateTime/TimeSpan static properties/methods. Scalar
                    // i64 value types; builtinClassSym carries the name so later
                    // member accesses (Year, Month, ...) are typed.
                    auto makeDTType = [&]() -> TypeRef* {
                        auto* t = ctx_.makeTypeRef();
                        t->kind = TypeRef::Kind::Named;
                        t->declaration = builtinClassSym("DateTime");
                        return t;
                    };
                    auto makeTSType = [&]() -> TypeRef* {
                        auto* t = ctx_.makeTypeRef();
                        t->kind = TypeRef::Kind::Named;
                        t->declaration = builtinClassSym("TimeSpan");
                        return t;
                    };
                    if (colName == "DateTime") {
                        if (m == "Now" || m == "UtcNow" || m == "Today" ||
                            m == "MinValue" || m == "MaxValue" || m == "FromTicks" ||
                            m == "Parse")  // per §2.5 / #648: Parse(s) → DateTime
                            ctx_.setExprType(&ce, makeDTType());
                        // per §2.5 / #648: TryParse(s, out result) → bool.
                        if (m == "TryParse")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                    }
                    // §2.5: TimeSpan static factories → TimeSpan type.
                    if (colName == "TimeSpan") {
                        if (m == "FromDays" || m == "FromHours" || m == "FromMinutes" ||
                            m == "FromSeconds" || m == "FromMilliseconds" || m == "Zero")
                            ctx_.setExprType(&ce, makeTSType());
                    }
                    // stdlib §2.6: Guid static method return types.
                    if (colName == "Guid") {
                        if (m == "NewGuid" || m == "Parse") {
                            auto* gTy = ctx_.makeTypeRef();
                            gTy->kind = TypeRef::Kind::Named;
                            gTy->declaration = builtinClassSym("Guid");
                            ctx_.setExprType(&ce, gTy);
                        }
                        if (m == "TryParse") {
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        }
                    }
                    // §8.1 / issue #646: Stopwatch static factory.
                    // StartNew() → owned Stopwatch (same TypeRef shape as
                    // new Stopwatch(), so using-var disposal and builtinTypeName
                    // checks all see an identical type).
                    if (colName == "Stopwatch") {
                        if (m == "StartNew") {
                            auto* swTy = ctx_.makeTypeRef();
                            swTy->kind = TypeRef::Kind::Named;
                            swTy->declaration = builtinClassSym("Stopwatch");
                            ctx_.setExprType(&ce, swTy);
                        }
                        // Instance methods Start/Stop/Reset/Restart return unit.
                        // Debug.Assert/WriteLine return unit. Both are untyped (void).
                    }
                    // §8.2 / issue #646: Debug static methods return unit (void).
                    // No type annotation needed; untyped is correct for void calls.
                    // (Debug.Assert and Debug.WriteLine both return unit.)

                    // stdlib §10.1/§10.2/§10.3 / §32.6 / fix #977: FFI-boundary
                    // class static factory methods. CString/WString/RawBytes each
                    // expose `From` (and CString additionally `FromBytes`) returning
                    // an owned handle lowered by codegen to ptrTy() (char* / wchar_t*
                    // / void*). The TypeRef shape mirrors builtinClassSym("Stopwatch")
                    // so that lowerLocalDecl's `ffiBuffer` disposal path (which calls
                    // free() at scope end) can identify the result via builtinTypeName.
                    if (colName == "CString") {
                        if (m == "From" || m == "FromBytes") {
                            auto* cstrTy = ctx_.makeTypeRef();
                            cstrTy->kind = TypeRef::Kind::Named;
                            cstrTy->declaration = builtinClassSym("CString");
                            ctx_.setExprType(&ce, cstrTy);
                        }
                    }
                    if (colName == "WString") {
                        if (m == "From") {
                            auto* wstrTy = ctx_.makeTypeRef();
                            wstrTy->kind = TypeRef::Kind::Named;
                            wstrTy->declaration = builtinClassSym("WString");
                            ctx_.setExprType(&ce, wstrTy);
                        }
                    }
                    if (colName == "RawBytes") {
                        if (m == "From") {
                            auto* rbTy = ctx_.makeTypeRef();
                            rbTy->kind = TypeRef::Kind::Named;
                            rbTy->declaration = builtinClassSym("RawBytes");
                            ctx_.setExprType(&ce, rbTy);
                        }
                    }

                    // §6.1 / §30.8: CancellationToken instance method return types.
                    // ThrowIfCancellationRequested() → unit (void); Register(cb) →
                    // owned Subscription (leave untyped — Register is deferred).
                    // IsCancellationRequested is a property (not a call), handled
                    // in resolveMemberAccess.
                    if (colName == "CancellationToken") {
                        // ThrowIfCancellationRequested() returns unit (void/untyped).
                        // Register — deferred (needs closure-list infra).
                    }
                    // §6.1: CancellationTokenSource instance and static method
                    // return types. Token is a property; Cancel() and CancelAfter()
                    // → unit; IsCancellationRequested → handled in resolveMemberAccess.
                    // WithCancelAfter is a STATIC factory (stdlib §6.1) returning
                    // owned CancellationTokenSource.
                    if (colName == "CancellationTokenSource") {
                        if (m == "Token") {
                            // Token property returns CancellationToken.  Set the
                            // declaration to builtinClassSym("CancellationToken") so
                            // subsequent member accesses (IsCancellationRequested) and
                            // lowerTypeRef can identify the type by name.
                            auto* ctTy = ctx_.makeTypeRef();
                            ctTy->kind = TypeRef::Kind::Named;
                            ctTy->declaration = builtinClassSym("CancellationToken");
                            ctx_.setExprType(&ce, ctTy);
                        }
                        // §6.1 / #463: WithCancelAfter(delay: TimeSpan) is a static
                        // factory returning owned CancellationTokenSource.  Without this
                        // entry, `var cts := CancellationTokenSource.WithCancelAfter(…)`
                        // had no sema type → codegen erased cts to i32 → cts.Token
                        // lowered as i32 → MALFORMED IR when passed to Task.Delay as
                        // a CancellationToken (ptr-wide, §6.1).  Using builtinClassSym
                        // produces the same TypeRef shape as the `new CancellationTokenSource()`
                        // path so all downstream checks (builtinTypeName, using-var
                        // disposal, lowerTypeRef) see an identical type.
                        if (m == "WithCancelAfter") {
                            auto* ctsTy = ctx_.makeTypeRef();
                            ctsTy->kind = TypeRef::Kind::Named;
                            ctsTy->declaration = builtinClassSym("CancellationTokenSource");
                            ctx_.setExprType(&ce, ctsTy);
                        }
                    }
                    // §30.10 / stdlib §6.7: Task static helpers.
                    // Helper: build a bare `Task` TypeRef (astNode name="Task",
                    // no generic args). Used by Delay, WhenAll, and as the base
                    // for Task<T> (genericArgs set separately).
                    auto makeTaskTypeRef = [&]() -> TypeRef* {
                        auto* tt = ctx_.makeTypeRef();
                        tt->kind = TypeRef::Kind::Named;
                        tt->nullable = false;
                        auto* ann = new ast::Type();
                        ann->kind = ast::TypeKind::Named;
                        auto* nm = new ast::QualifiedName();
                        nm->parts.push_back("Task");
                        ann->name.reset(nm);
                        tt->astNode = ann;
                        return tt;
                    };
                    // Helper: build a bare `OwnedTask` TypeRef (astNode name=
                    // "OwnedTask", no generic args). Mirrors makeTaskTypeRef for
                    // the §30.5 Pattern B owned-result form so the result lowers
                    // to a ptr task handle (LlvmBackend_Layout lowerTypeRef).
                    auto makeOwnedTaskTypeRef = [&]() -> TypeRef* {
                        auto* tt = ctx_.makeTypeRef();
                        tt->kind = TypeRef::Kind::Named;
                        tt->nullable = false;
                        auto* ann = new ast::Type();
                        ann->kind = ast::TypeKind::Named;
                        auto* nm = new ast::QualifiedName();
                        nm->parts.push_back("OwnedTask");
                        ann->name.reset(nm);
                        tt->astNode = ann;
                        return tt;
                    };
                    if (colName == "Task") {
                        // `Task.Delay(...)` returns a (non-generic) `Task` — a
                        // genuinely-incomplete task the awaiter suspends on. The
                        // result TypeRef carries an ast::Type named "Task" (no
                        // generic args) so the `await` typer recognizes it as
                        // `await Task` → unit. The four Delay overloads (int /
                        // TimeSpan, each with an optional CancellationToken) all
                        // yield a bare `Task`.
                        if (m == "Delay") {
                            ctx_.setExprType(&ce, makeTaskTypeRef());
                        }
                        // stdlib §6.7 (line 2855): Task.FromResult<T>(value: T):
                        // Task<T>. Returns an already-completed task carrying the
                        // argument value. The result type is Task<T> where T is
                        // inferred from the argument.
                        if (m == "FromResult" && !ce.args.empty() && ce.args[0].value) {
                            TypeRef* argTy = ctx_.exprType(*ce.args[0].value);
                            // §30.5 (#1107, sweep #9): FromResult is restricted to
                            // non-class T — value types, `string`, runtime-managed
                            // reference types. A CLASS / interface T must use
                            // `Task.FromOwnedResult` (Pattern B: the produced task
                            // owns the result, transferred to the awaiter). Reject
                            // a class-typed T (taken from the explicit type arg
                            // `FromResult<C>(…)`, else inferred from the argument);
                            // value-builtin wrapper classes (Int32, …) are value
                            // types and stay allowed.
                            TypeRef* tArg = argTy;
                            if (!ma->typeArgs.empty() && ma->typeArgs[0])
                                if (TypeRef* et =
                                        ctx_.resolvedType(*ma->typeArgs[0]))
                                    tArg = et;
                            if (tArg && tArg->kind == TypeRef::Kind::Named &&
                                tArg->declaration &&
                                (tArg->declaration->kind ==
                                     ast::TypeDeclKind::Class ||
                                 tArg->declaration->kind ==
                                     ast::TypeDeclKind::Interface) &&
                                !tArg->declaration->isValueBuiltin) {
                                ctx_.diagnostics().report(
                                    Severity::Error, ce.span.begin,
                                    "`Task.FromResult<T>` is restricted to "
                                    "non-class T (value types, `string`, "
                                    "runtime-managed reference types); `" +
                                    std::string(tArg->declaration->name) +
                                    "` is a class type — use "
                                    "`Task.FromOwnedResult<T>` (Pattern B, "
                                    "ownership transfer) instead (\xC2\xA7""30.5)");
                            }
                            auto* tt = makeTaskTypeRef();
                            if (argTy) tt->genericArgs.push_back(argTy);
                            ctx_.setExprType(&ce, tt);
                        }
                        // stdlib §6.7 (line 2993) / §30.5 Pattern B:
                        // Task.FromOwnedResult<T>(owned value: T): OwnedTask<T>
                        // where T: class. Returns an already-completed OWNED task
                        // carrying the owned value; awaiting it (Pattern B)
                        // transfers ownership of the value to the awaiter. The
                        // result element T is the explicit method type arg (the
                        // argument's static type otherwise). Without this arm the
                        // call had NO expr type, so a `using var task :=
                        // Task.FromOwnedResult<Cell>(...)` local fell to the i32
                        // placeholder storage → bz_task_is_done(i32) IR_FAIL (#939)
                        // / runtime crash (#940).
                        if (m == "FromOwnedResult" && !ce.args.empty() &&
                            ce.args[0].value) {
                            auto* tt = makeOwnedTaskTypeRef();
                            TypeRef* T = nullptr;
                            if (!ma->typeArgs.empty() && ma->typeArgs[0])
                                T = ctx_.resolvedType(*ma->typeArgs[0]);
                            if (!T) T = ctx_.exprType(*ce.args[0].value);
                            if (T) tt->genericArgs.push_back(T);
                            ctx_.setExprType(&ce, tt);
                        }
                        // stdlib §6.7 (line 2986-2987): Task.FromException<T>(exc):
                        // Task<T> and Task.FromCancelled<T>(): Task<T>. The result
                        // T comes from the EXPLICIT method type arg (the argument,
                        // if any, is the Exception — not T). A faulted/cancelled
                        // task never produces a value; awaiting it re-raises.
                        if (m == "FromException" || m == "FromCancelled") {
                            auto* tt = makeTaskTypeRef();
                            if (!ma->typeArgs.empty() && ma->typeArgs[0])
                                if (TypeRef* T =
                                        ctx_.resolvedType(*ma->typeArgs[0]))
                                    tt->genericArgs.push_back(T);
                            ctx_.setExprType(&ce, tt);
                        }
                        // stdlib §6.7 (line 2872): Task.WhenAll(tasks: Array<Task>):
                        // Task. Completes when all input tasks complete; no result
                        // value (bare Task).
                        //
                        // stdlib §6.7 (line 2875):
                        // Task.WhenAll<T>(tasks: Array<Task<T>>): Task<Array<T>>.
                        // When the input is Array<Task<T>> (the element Task has a
                        // generic arg T), the result is Task<Array<T>>. Detect this
                        // by inspecting the first argument's element type.
                        if (m == "WhenAll") {
                            // Check if the arg is Array<Task<T>> (generic variant).
                            TypeRef* resultTy = makeTaskTypeRef();
                            if (!ce.args.empty() && ce.args[0].value) {
                                TypeRef* argTy = ctx_.exprType(*ce.args[0].value);
                                // argTy should be Named("Array", [Named("Task", [T])]).
                                if (argTy && argTy->kind == TypeRef::Kind::Named &&
                                    !argTy->genericArgs.empty() &&
                                    argTy->genericArgs[0]) {
                                    TypeRef* elemTy = argTy->genericArgs[0];
                                    // elemTy is Task<T> when it has a generic arg.
                                    // Check: elemTy.name == "Task" and has genericArgs.
                                    bool elemIsTask = false;
                                    if (elemTy->kind == TypeRef::Kind::Named &&
                                        !elemTy->genericArgs.empty() &&
                                        elemTy->genericArgs[0]) {
                                        // Resolve name: may be !declaration astNode or builtin.
                                        std::string_view ename;
                                        if (!elemTy->declaration && elemTy->astNode &&
                                            elemTy->astNode->name &&
                                            !elemTy->astNode->name->parts.empty())
                                            ename = elemTy->astNode->name->parts[0];
                                        else if (elemTy->declaration && !elemTy->declaration->decl)
                                            ename = elemTy->declaration->name;
                                        if (ename == "Task" || ename == "OwnedTask")
                                            elemIsTask = true;
                                    }
                                    if (elemIsTask) {
                                        // T is elemTy->genericArgs[0].
                                        TypeRef* T = elemTy->genericArgs[0];
                                        // Build Array<T> TypeRef.
                                        auto* arrTy = ctx_.makeTypeRef();
                                        arrTy->kind = TypeRef::Kind::Named;
                                        arrTy->nullable = false;
                                        auto* arrAnn = new ast::Type();
                                        arrAnn->kind = ast::TypeKind::Named;
                                        auto* arrNm = new ast::QualifiedName();
                                        arrNm->parts.push_back("Array");
                                        arrAnn->name.reset(arrNm);
                                        arrTy->astNode = arrAnn;
                                        arrTy->genericArgs.push_back(T);
                                        // Build Task<Array<T>> TypeRef.
                                        resultTy = makeTaskTypeRef();
                                        resultTy->genericArgs.push_back(arrTy);
                                    }
                                }
                            }
                            ctx_.setExprType(&ce, resultTy);
                        }
                        // stdlib §6.7 (line 2884): Task.WhenAny(tasks: Array<Task>):
                        // Task<Task>. Completes with the first-done task pointer as
                        // the result. Typed as Task<Task> so the await result is the
                        // winning task pointer (ptr-sized).
                        if (m == "WhenAny") {
                            auto* tt = makeTaskTypeRef();
                            // genericArgs[0] = Task (the inner Task type)
                            tt->genericArgs.push_back(makeTaskTypeRef());
                            ctx_.setExprType(&ce, tt);
                        }
                        // stdlib §6.7 (line 2942): Task.WhenAllOwned<T>(
                        //   owned tasks: OwnedArray<OwnedTask<T>>
                        // ): OwnedTask<OwnedArray<T>> where T: class.
                        //
                        // Pattern B aggregator: takes ownership of an
                        // OwnedArray<OwnedTask<T>> and produces an
                        // OwnedTask<OwnedArray<T>>. T must be a class type.
                        // The result type is built as OwnedTask<OwnedArray<T>>
                        // where T is the innermost generic arg of the input.
                        if (m == "WhenAllOwned") {
                            // Build OwnedTask<OwnedArray<T>> TypeRef.
                            // Detect T from the arg type: OwnedArray<OwnedTask<T>>.
                            TypeRef* T = nullptr;
                            if (!ce.args.empty() && ce.args[0].value) {
                                TypeRef* argTy = ctx_.exprType(*ce.args[0].value);
                                // argTy = OwnedArray<OwnedTask<T>>
                                if (argTy && argTy->kind == TypeRef::Kind::Named &&
                                    !argTy->genericArgs.empty() &&
                                    argTy->genericArgs[0]) {
                                    TypeRef* elemTy = argTy->genericArgs[0]; // OwnedTask<T>
                                    if (elemTy->kind == TypeRef::Kind::Named &&
                                        !elemTy->genericArgs.empty() &&
                                        elemTy->genericArgs[0]) {
                                        T = elemTy->genericArgs[0];
                                    }
                                }
                            }
                            // Build OwnedArray<T> TypeRef.
                            auto* arrTy = ctx_.makeTypeRef();
                            arrTy->kind = TypeRef::Kind::Named;
                            arrTy->nullable = false;
                            auto* arrAnn = new ast::Type();
                            arrAnn->kind = ast::TypeKind::Named;
                            auto* arrNm = new ast::QualifiedName();
                            arrNm->parts.push_back("OwnedArray");
                            arrAnn->name.reset(arrNm);
                            arrTy->astNode = arrAnn;
                            if (T) arrTy->genericArgs.push_back(T);
                            // Build OwnedTask<OwnedArray<T>> TypeRef.
                            auto* ownedTaskTy = ctx_.makeTypeRef();
                            ownedTaskTy->kind = TypeRef::Kind::Named;
                            ownedTaskTy->nullable = false;
                            auto* otAnn = new ast::Type();
                            otAnn->kind = ast::TypeKind::Named;
                            auto* otNm = new ast::QualifiedName();
                            otNm->parts.push_back("OwnedTask");
                            otAnn->name.reset(otNm);
                            ownedTaskTy->astNode = otAnn;
                            ownedTaskTy->genericArgs.push_back(arrTy);
                            ctx_.setExprType(&ce, ownedTaskTy);
                        }
                        // stdlib §6.7 (line 2885): Task.FireAndForget(owned task):
                        // void/unit. Ownership of the task transfers; the caller
                        // does not need the return value. No TypeRef set → unit.
                        // (FireAndForget return type is untyped/unit in sema.)
                    }
                }
                // §2.5: DateTime instance method return types.
                // The receiver's TypeRef uses builtinClassSym (non-null declaration,
                // null decl) so colName is empty; check via declaration->name directly.
                {
                    auto builtinInstName = [](const TypeRef* rt) -> std::string_view {
                        if (!rt || rt->kind != TypeRef::Kind::Named) return {};
                        if (!rt->declaration) {
                            if (rt->astNode && rt->astNode->name &&
                                !rt->astNode->name->parts.empty())
                                return rt->astNode->name->parts[0];
                            return {};
                        }
                        if (!rt->declaration->decl) return rt->declaration->name;
                        return {};
                    };
                    std::string_view rcn = builtinInstName(recvTy);
                    if (rcn == "DateTime") {
                        // Add*/Subtract(TimeSpan) → DateTime; Subtract(DateTime) →
                        // TimeSpan. AddYears/AddMonths → DateTime. ToUtc/ToLocal →
                        // DateTime. Subtract overload dispatch by arg type is not
                        // tracked here — both overloads typed as DateTime; the
                        // Subtract(DateTime) → TimeSpan is handled in codegen.
                        auto* dtTy = ctx_.makeTypeRef();
                        dtTy->kind = TypeRef::Kind::Named;
                        dtTy->declaration = builtinClassSym("DateTime");
                        auto* tsTy = ctx_.makeTypeRef();
                        tsTy->kind = TypeRef::Kind::Named;
                        tsTy->declaration = builtinClassSym("TimeSpan");
                        if (m == "Add" || m == "AddDays" || m == "AddHours" ||
                            m == "AddMinutes" || m == "AddSeconds" ||
                            m == "AddMilliseconds" || m == "AddYears" ||
                            m == "AddMonths" || m == "ToUtc" || m == "ToLocal")
                            ctx_.setExprType(&ce, dtTy);
                        else if (m == "Subtract") {
                            // Overload: Subtract(DateTime) → TimeSpan;
                            //           Subtract(TimeSpan) → DateTime.
                            // Check first arg type to pick the right return.
                            bool argIsTS = false;
                            if (!ce.args.empty() && ce.args[0].value) {
                                auto* argTy = ctx_.exprType(*ce.args[0].value);
                                argIsTS = argTy && builtinInstName(argTy) == "TimeSpan";
                            }
                            ctx_.setExprType(&ce, argIsTS ? dtTy : tsTy);
                        }
                        // per §2.5 / #648: ToString(format: string) → string.
                        else if (m == "ToString" && !ce.args.empty())
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::String));
                    }
                    // TimeSpan arithmetic methods return TimeSpan (fallback if
                    // the operator desugaring lands here rather than BinaryExpr).
                    if (rcn == "TimeSpan") {
                        auto* tsTy = ctx_.makeTypeRef();
                        tsTy->kind = TypeRef::Kind::Named;
                        tsTy->declaration = builtinClassSym("TimeSpan");
                        if (m == "Add" || m == "Subtract" || m == "Negate")
                            ctx_.setExprType(&ce, tsTy);
                    }
                    // stdlib §10.1 / issue #1000: CString.ToBytes(): owned
                    // Array<byte>. Type the result as a concrete Array<byte>
                    // (the same representation Encoding.GetBytes / ToArray use,
                    // makeConcreteCollectionResult) so the inferred local
                    // (`using var back := s.ToBytes()`) is recognized as an
                    // array-backed owned collection: .Count / [i] resolve and
                    // scope-end disposal runs bz_array_free. The codegen lowers
                    // the call to bz_cstring_to_bytes, which returns a FRESH,
                    // independently-owned buffer (a copy of the CString bytes),
                    // so disposing both `back` and the source CString frees each
                    // exactly once (§28.3 — no buffer aliasing / double-free).
                    if (rcn == "CString" && m == "ToBytes" && ce.args.empty()) {
                        ctx_.setExprType(
                            &ce, makeConcreteCollectionResult(
                                "Array", ctx_.primitiveTypeRef(P::Byte)));
                    }
                    // §3.3 (#883): ReadOnlyStringSpan instance methods. The
                    // receiver is a string-backed view (Source: string, Start,
                    // Length); the search methods (IndexOf/Contains/StartsWith/
                    // EndsWith/SequenceEqual) are deferred (substring search over
                    // the adaptive string encoding — see the follow-up issue).
                    if (rcn == "ReadOnlyStringSpan") {
                        if (m == "Length")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                        else if (m == "IsEmpty")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        else if (m == "Slice") {
                            // Slice(start, length) → ReadOnlyStringSpan over the
                            // same source.
                            auto* tr = ctx_.makeTypeRef();
                            tr->kind = TypeRef::Kind::Named;
                            tr->declaration = nullptr;
                            tr->astNode = recvTy->astNode;  // same synthetic node
                            ctx_.setExprType(&ce, tr);
                        } else if (m == "IndexOf" || m == "Contains" ||
                                   m == "StartsWith" || m == "EndsWith" ||
                                   m == "SequenceEqual") {
                            ctx_.diagnostics().report(
                                Severity::Error, ce.span.begin,
                                std::string("`") + std::string(m) +
                                "` on `ReadOnlyStringSpan` is not yet supported "
                                "(substring search over the adaptive string "
                                "encoding is deferred) — see the follow-up issue; "
                                "for now materialize a substring with "
                                "`s.Substring(...)` and use the `string` methods "
                                "(stdlib \xC2\xA7""3.3)");
                        } else {
                            ctx_.diagnostics().report(
                                Severity::Error, ce.span.begin,
                                std::string("no member `") + std::string(m) +
                                "` on `ReadOnlyStringSpan`; documented surface: "
                                "Length, IsEmpty, Slice(), and the indexer `[i]` "
                                "(stdlib \xC2\xA7""3.3)");
                        }
                    }
                    // §4.15 (#549) / §4.14 (#779): span instance-method return
                    // types. The receiver carries an astNode-backed TypeRef (no
                    // declaration) so rcn is one of ArraySpan / ReadOnlyArraySpan
                    // (borrowing, §4.15) or Span / ReadOnlySpan (owning, §4.14).
                    if (rcn == "ArraySpan" || rcn == "ReadOnlyArraySpan" ||
                        rcn == "Span" || rcn == "ReadOnlySpan") {
                        bool isWritable   = (rcn == "ArraySpan" || rcn == "Span");
                        bool isOwningSpan = (rcn == "Span");  // owning + writable
                        // §4.14: an owning Span<classT>'s slots own their elements,
                        // so Clear/MoveFrom/MoveTo apply (and Fill/CopyFrom/CopyTo —
                        // which would alias one borrow into many owning slots — are
                        // forbidden); the value-typed owning Span keeps the
                        // value-copy surface (Fill/CopyFrom/CopyTo), no Clear/Move.
                        const TypeRef* spEl =
                            (!recvTy->genericArgs.empty()) ? recvTy->genericArgs[0]
                                                           : nullptr;
                        bool elemIsClass = false;
                        if (spEl && spEl->kind == TypeRef::Kind::Named) {
                            if (spEl->declaration &&
                                (spEl->declaration->kind ==
                                     ast::TypeDeclKind::Class ||
                                 spEl->declaration->kind ==
                                     ast::TypeDeclKind::Interface))
                                elemIsClass = true;
                            else if (!spEl->declaration && spEl->astNode &&
                                     spEl->astNode->name &&
                                     !spEl->astNode->name->parts.empty()) {
                                auto en = spEl->astNode->name->parts[0];
                                if (en == "Shared" || en == "Weak")
                                    elemIsClass = true;
                            }
                        }
                        bool owningClassSpan = isOwningSpan && elemIsClass;
                        // The OWNING family (Span/ReadOnlySpan over OwnedBuffer)
                        // with a class element: its slots own their elements, so
                        // the value-copy surface (Fill/CopyFrom/CopyTo) is
                        // forbidden on EITHER the writable or the read-only form.
                        bool owningClassFamily =
                            (rcn == "Span" || rcn == "ReadOnlySpan") && elemIsClass;
                        // Length / IsEmpty are readonly properties; for-call form:
                        if (m == "Length")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                        else if (m == "IsEmpty")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        else if (m == "IndexOf")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                        else if (m == "Contains" || m == "SequenceEqual")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        else if ((m == "Slice") &&
                                 !recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
                            // Slice returns same span type (writable → writable,
                            // read-only → read-only) with same element type.
                            auto* tr = ctx_.makeTypeRef();
                            tr->kind = TypeRef::Kind::Named;
                            tr->declaration = nullptr;
                            tr->astNode = recvTy->astNode;  // same synthetic node
                            tr->genericArgs.push_back(recvTy->genericArgs[0]);
                            ctx_.setExprType(&ce, tr);
                        } else if (m == "AsReadOnly" && isWritable &&
                                   !recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
                            // ArraySpan<T>.AsReadOnly() → ReadOnlyArraySpan<T>;
                            // Span<T>.AsReadOnly() → ReadOnlySpan<T>.
                            static const ast::Type* kROArr = []() -> ast::Type* {
                                auto* qn = new ast::QualifiedName;
                                qn->parts.push_back("ReadOnlyArraySpan");
                                auto* t = new ast::Type;
                                t->kind = ast::TypeKind::Named;
                                t->name.reset(qn);
                                return t;
                            }();
                            static const ast::Type* kROSpan = []() -> ast::Type* {
                                auto* qn = new ast::QualifiedName;
                                qn->parts.push_back("ReadOnlySpan");
                                auto* t = new ast::Type;
                                t->kind = ast::TypeKind::Named;
                                t->name.reset(qn);
                                return t;
                            }();
                            auto* tr = ctx_.makeTypeRef();
                            tr->kind = TypeRef::Kind::Named;
                            tr->declaration = nullptr;
                            tr->astNode = isOwningSpan ? kROSpan : kROArr;
                            tr->genericArgs.push_back(recvTy->genericArgs[0]);
                            ctx_.setExprType(&ce, tr);
                        } else if (m == "Reverse" && isWritable) {
                            // In-place permutation — unit; valid on any writable span.
                        } else if (m == "Fill" || m == "CopyFrom") {
                            // §4.15/§4.14: Fill/CopyFrom MUTATE this span, so the
                            // receiver must be WRITABLE (ArraySpan / Span). And an
                            // owning Span<classT> forbids them (#870) — they would
                            // alias one borrowed reference into many owning slots
                            // (double-free). Value-copy surface stays on
                            // ArraySpan<T> and value-typed Span<T>.
                            if (!isWritable) {
                                ctx_.diagnostics().report(
                                    Severity::Error, ce.span.begin,
                                    std::string("`") + std::string(m) +
                                    "` is not admitted on a read-only span `" +
                                    std::string(rcn) +
                                    "<T>` (stdlib \xC2\xA7""4.14/\xC2\xA7""4.15)");
                            } else if (owningClassFamily) {
                                ctx_.diagnostics().report(
                                    Severity::Error, ce.span.begin,
                                    std::string("`") + std::string(m) +
                                    "` is not admitted on an owning `Span<T>` whose "
                                    "element type is a class — it would copy one "
                                    "borrowed reference into multiple owning slots "
                                    "(a double-free). Use `MoveFrom`/`MoveTo` to "
                                    "transfer ownership, or `Clear` to dispose the "
                                    "elements (stdlib \xC2\xA7""4.14)");
                            }
                            // else unit; no return type.
                        } else if (m == "CopyTo") {
                            // §4.15/§4.14: CopyTo's receiver is the SOURCE — valid
                            // on a read-only OR writable value/borrowing span (it
                            // writes into the destination argument). Forbidden on an
                            // owning class span (#870 — copies owned references).
                            if (owningClassFamily) {
                                ctx_.diagnostics().report(
                                    Severity::Error, ce.span.begin,
                                    "`CopyTo` is not admitted on an owning "
                                    "`Span<T>` whose element type is a class — it "
                                    "would copy one owned reference into multiple "
                                    "owning slots (a double-free). Use `MoveTo` to "
                                    "transfer ownership (stdlib \xC2\xA7""4.14)");
                            }
                            // else unit; no return type.
                        } else if ((m == "Clear" || m == "MoveFrom" ||
                                    m == "MoveTo")) {
                            // §4.14: Clear/MoveFrom/MoveTo are admitted only on an
                            // owning Span<classT> (the slots own their elements).
                            if (!owningClassSpan) {
                                ctx_.diagnostics().report(
                                    Severity::Error, ce.span.begin,
                                    std::string("`") + std::string(m) +
                                    "` is admitted only on an owning `Span<T>` "
                                    "whose element type is a class (returned by "
                                    "`OwnedBuffer<classT>.AsSpan()`); `" +
                                    std::string(rcn) + "<T>` has no `" +
                                    std::string(m) + "` (stdlib \xC2\xA7""4.14)");
                            }
                            // else unit; no return type.
                        } else {
                            // Documented surface exhausted — any other member errs.
                            ctx_.diagnostics().report(
                                Severity::Error, ce.span.begin,
                                std::string("no member `") + std::string(m) +
                                "` on `" + std::string(rcn) + "<T>`; "
                                "documented surface: Length, IsEmpty, IndexOf(), "
                                "Contains(), SequenceEqual(), Slice(), "
                                "AsReadOnly()/Reverse()/Fill()/CopyFrom()/CopyTo() "
                                "[writable spans], Clear()/MoveFrom()/MoveTo() "
                                "[owning class Span<T>] (stdlib \xC2\xA7""4.14/\xC2\xA7""4.15)");
                        }
                    }
                    // §28.13 / stdlib §2.16 (#603): Shared<T> instance method
                    // return types.  The receiver TypeRef has no declaration
                    // (builtin) and astNode carries "Shared"; genericArgs[0] is T.
                    if (rcn == "Shared" && recvTy &&
                        !recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
                        const TypeRef* elemTy = recvTy->genericArgs[0];
                        if (m == "Clone") {
                            // Clone(): owned Shared<T> — returns a TypeRef
                            // identical to the receiver (same structure).
                            ctx_.setExprType(&ce, const_cast<TypeRef*>(recvTy));
                        } else if (m == "Get") {
                            // Get(): T (borrow; source = the handle, handled
                            // via Shape-1 in borrowSourceForExpr when T is a
                            // class — the MemberAccessExpr's identifierRoot
                            // resolves to the Shared handle local). per §28.13.
                            ctx_.setExprType(&ce, const_cast<TypeRef*>(elemTy));
                        } else if (m == "Downgrade") {
                            // Downgrade(): owned Weak<T>. Build a Weak<T>
                            // TypeRef with the same element type argument.
                            auto* weakTy = ctx_.makeTypeRef();
                            weakTy->kind = TypeRef::Kind::Named;
                            weakTy->declaration = nullptr;
                            // Re-use the same synthetic astNode shape as
                            // Shared<T> but with name "Weak".
                            static const ast::Type* kWeakNode = []() -> ast::Type* {
                                auto* qn = new ast::QualifiedName;
                                qn->parts.push_back("Weak");
                                auto* t  = new ast::Type;
                                t->kind  = ast::TypeKind::Named;
                                t->name.reset(qn);
                                return t;
                            }();
                            weakTy->astNode = kWeakNode;
                            weakTy->genericArgs.push_back(
                                const_cast<TypeRef*>(elemTy));
                            ctx_.setExprType(&ce, weakTy);
                        } else if (m != "Dispose") {
                            // §2.16 / #674: Shared<T> exposes only Clone(),
                            // Get(), Downgrade(), and Dispose().  Any other
                            // member is an error.  Dispose is compiler-generated
                            // and must pass silently.
                            ctx_.diagnostics().report(
                                Severity::Error, ce.span.begin,
                                std::string("no member `") + std::string(m) +
                                "` on `Shared<T>`; documented surface: "
                                "Clone(), Get(), Downgrade(), Dispose() "
                                "(stdlib \xC2\xA7""2.16)");
                        }
                        // Dispose(): unit; no return type set.
                    }
                    // §28.13 / stdlib §2.16 (#603): Weak<T> instance methods.
                    if (rcn == "Weak") {
                        if (m == "TryUpgrade") {
                            // TryUpgrade(out owned s: Shared<T>?): bool.
                            // spec §22.4 / #653: the parameter is "out owned" —
                            // the only accepted call-site forms are
                            // "out using var x" and "out owned var x".
                            // Plain "out x" (pre-declared variable) is ill-formed
                            // because it doesn't communicate an owned-discharge
                            // obligation to the compiler. Emit a clear diagnostic.
                            if (!ce.args.empty()) {
                                const auto& arg0 = ce.args[0];
                                bool isOutUsingVar =
                                    arg0.modifier == "out using var";
                                bool isOutOwnedVar =
                                    arg0.modifier == "out owned var";
                                bool isDiscard = arg0.isDiscard;
                                if (!arg0.modifier.empty() &&
                                    !isOutUsingVar && !isOutOwnedVar &&
                                    !isDiscard) {
                                    ctx_.diagnostics().report(
                                        Severity::Error, ce.span.begin,
                                        "TryUpgrade parameter is 'out owned': "
                                        "accepted call-site forms are "
                                        "'out using var x' or 'out owned var x' "
                                        "(spec §22.4). "
                                        "Plain 'out x' is not valid for an "
                                        "'out owned' parameter — use "
                                        "'out using var x' instead.");
                                }
                            }
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        } else if (m != "Dispose") {
                            // §2.16 / #674: Weak<T> exposes only TryUpgrade()
                            // and Dispose().  Any other member is an error.
                            // Dispose is compiler-generated and must pass silently.
                            ctx_.diagnostics().report(
                                Severity::Error, ce.span.begin,
                                std::string("no member `") + std::string(m) +
                                "` on `Weak<T>`; documented surface: "
                                "TryUpgrade(out owned s: Shared<T>?), Dispose() "
                                "(stdlib \xC2\xA7""2.16)");
                        }
                        // Dispose(): unit; no return type set.
                    }

                    // §30.15 / stdlib §6.9 (#600): Channel<T> instance methods.
                    // Channel<T> TypeRef: Named, no declaration, astNode name "Channel",
                    // genericArgs[0] = T.
                    if (rcn == "Channel" && recvTy &&
                        !recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
                        const TypeRef* elemTy = recvTy->genericArgs[0];
                        // Helper: build an OwnedTask / OwnedTask<T> TypeRef.
                        auto makeOwnedTask = [&](const TypeRef* inner) -> TypeRef* {
                            auto* ot = ctx_.makeTypeRef();
                            ot->kind = TypeRef::Kind::Named;
                            ot->nullable = false;
                            auto* ann = new ast::Type();
                            ann->kind = ast::TypeKind::Named;
                            auto* nm = new ast::QualifiedName();
                            nm->parts.push_back("OwnedTask");
                            ann->name.reset(nm);
                            ot->astNode = ann;
                            if (inner) ot->genericArgs.push_back(const_cast<TypeRef*>(inner));
                            return ot;
                        };
                        // Helper: build a Task / Task<T> TypeRef.
                        auto makeTask = [&](const TypeRef* inner) -> TypeRef* {
                            auto* tt = ctx_.makeTypeRef();
                            tt->kind = TypeRef::Kind::Named;
                            tt->nullable = false;
                            auto* ann = new ast::Type();
                            ann->kind = ast::TypeKind::Named;
                            auto* nm = new ast::QualifiedName();
                            nm->parts.push_back("Task");
                            ann->name.reset(nm);
                            tt->astNode = ann;
                            if (inner) tt->genericArgs.push_back(const_cast<TypeRef*>(inner));
                            return tt;
                        };
                        // Helper: build ChannelSender<T> TypeRef.
                        auto makeChannelSender = [&](const TypeRef* inner) -> TypeRef* {
                            auto* cs = ctx_.makeTypeRef();
                            cs->kind = TypeRef::Kind::Named;
                            cs->nullable = false;
                            auto* ann = new ast::Type();
                            ann->kind = ast::TypeKind::Named;
                            auto* nm = new ast::QualifiedName();
                            nm->parts.push_back("ChannelSender");
                            ann->name.reset(nm);
                            cs->astNode = ann;
                            if (inner) cs->genericArgs.push_back(const_cast<TypeRef*>(inner));
                            return cs;
                        };

                        if (m == "Receive") {
                            // §30.15: Receive() returns:
                            //   OwnedTask<T> when T is a class type (owned transfer),
                            //   Task<T>      when T is a value type or string.
                            bool elemIsClass = (elemTy->kind == TypeRef::Kind::Named &&
                                               elemTy->declaration &&
                                               elemTy->declaration->kind ==
                                                   ast::TypeDeclKind::Class);
                            if (elemIsClass)
                                ctx_.setExprType(&ce, makeOwnedTask(elemTy));
                            else
                                ctx_.setExprType(&ce, makeTask(elemTy));
                        } else if (m == "TryReceive") {
                            // TryReceive(out owned item: T?) → bool.
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        } else if (m == "NewSender") {
                            // NewSender() → owned ChannelSender<T>.
                            ctx_.setExprType(&ce, makeChannelSender(elemTy));
                        } else if (m == "Count" || m == "IsClosed") {
                            // Count → int; IsClosed → bool.
                            if (m == "Count")
                                ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                            else
                                ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        } else if (m != "Dispose") {
                            // §6.9 / #640: Channel<T> does not expose `Send` or
                            // `TrySend` (those live on ChannelSender<T>); any other
                            // unknown name is equally invalid.  Emit a diagnostic
                            // pointing to the correct API surface.
                            // `Dispose` is compiler-generated and must silently pass.
                            std::string hint;
                            if (m == "Send" || m == "TrySend") {
                                hint = "; `" + std::string(m) + "` is only available "
                                       "on `ChannelSender<T>` — obtain one via "
                                       "`ch.NewSender()` (stdlib \xC2\xA7""6.9, #640)";
                            }
                            ctx_.diagnostics().report(
                                Severity::Error, ce.span.begin,
                                std::string("no member `") + std::string(m) +
                                "` on `Channel<T>`" + hint +
                                " (stdlib \xC2\xA7""6.9)");
                        }
                    }

                    // §30.15 / stdlib §6.9 (#600): ChannelSender<T> instance methods.
                    if (rcn == "ChannelSender" && recvTy &&
                        !recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
                        if (m == "Send") {
                            // Send(owned item: T) → void (no return type set = unit).
                        } else if (m != "Dispose") {
                            // §6.9 / #674: ChannelSender<T> exposes only Send()
                            // and Dispose().  Any other member is an error.
                            // Dispose is compiler-generated and must pass silently.
                            ctx_.diagnostics().report(
                                Severity::Error, ce.span.begin,
                                std::string("no member `") + std::string(m) +
                                "` on `ChannelSender<T>`; documented surface: "
                                "Send(owned item: T), Dispose() "
                                "(stdlib \xC2\xA7""6.9)");
                        }
                        // Dispose(): unit; no return type set.
                    }
                    // §6.2 / issue #647: Mutex instance methods.
                    // Mutex TypeRef: Named, no declaration, astNode name "Mutex".
                    if (rcn == "Mutex" && recvTy) {
                        // Helper: build a Lock TypeRef (nullary, no declaration).
                        static const ast::Type* kLockNode = []() -> ast::Type* {
                            auto* qn = new ast::QualifiedName;
                            qn->parts.push_back("Lock");
                            auto* t = new ast::Type;
                            t->kind = ast::TypeKind::Named;
                            t->name.reset(qn);
                            return t;
                        }();
                        auto makeLockTypeRef = [&]() -> TypeRef* {
                            auto* tr = ctx_.makeTypeRef();
                            tr->kind = TypeRef::Kind::Named;
                            tr->nullable = false;
                            tr->declaration = nullptr;
                            tr->astNode = kLockNode;
                            return tr;
                        };
                        if (m == "Acquire" && ce.args.empty()) {
                            // Acquire(): owned Lock.
                            ctx_.setExprType(&ce, makeLockTypeRef());
                        } else if (m == "TryAcquire" && ce.args.size() == 1) {
                            // TryAcquire(timeout: TimeSpan): owned Lock?
                            // Build a nullable Lock TypeRef.
                            auto* tr = makeLockTypeRef();
                            tr->nullable = true;
                            ctx_.setExprType(&ce, tr);
                        } else if (m == "AcquireAsync" || m == "TryAcquireAsync") {
                            // §6.2 / #647 / v0.139: AcquireAsync / TryAcquireAsync.
                            //
                            // AcquireAsync(token := CancellationToken.None)
                            //   → OwnedTask<Lock>
                            // TryAcquireAsync(timeout: TimeSpan,
                            //                 token := CancellationToken.None)
                            //   → OwnedTask<Lock?>
                            //
                            // Build OwnedTask<Lock> / OwnedTask<Lock?> TypeRef.
                            // Helper: build OwnedTask<T> TypeRef.
                            auto makeOwnedTaskOf = [&](TypeRef* inner) -> TypeRef* {
                                auto* ot = ctx_.makeTypeRef();
                                ot->kind = TypeRef::Kind::Named;
                                ot->nullable = false;
                                auto* ann = new ast::Type();
                                ann->kind = ast::TypeKind::Named;
                                auto* nm = new ast::QualifiedName();
                                nm->parts.push_back("OwnedTask");
                                ann->name.reset(nm);
                                ot->astNode = ann;
                                if (inner) ot->genericArgs.push_back(inner);
                                return ot;
                            };
                            if (m == "AcquireAsync") {
                                // OwnedTask<Lock>
                                ctx_.setExprType(&ce,
                                    makeOwnedTaskOf(makeLockTypeRef()));
                            } else {
                                // TryAcquireAsync → OwnedTask<Lock?>
                                auto* lockNullable = makeLockTypeRef();
                                lockNullable->nullable = true;
                                ctx_.setExprType(&ce,
                                    makeOwnedTaskOf(lockNullable));
                            }
                        } else if (m != "Dispose") {
                            // §6.2 / #674: Mutex exposes only Acquire(),
                            // TryAcquire(), AcquireAsync(), TryAcquireAsync(),
                            // and Dispose().  Any other member is an error.
                            // Dispose is compiler-generated and must pass silently.
                            ctx_.diagnostics().report(
                                Severity::Error, ce.span.begin,
                                std::string("no member `") + std::string(m) +
                                "` on `Mutex`; documented surface: "
                                "Acquire(), TryAcquire(timeout: TimeSpan), "
                                "AcquireAsync(), TryAcquireAsync(), Dispose() "
                                "(stdlib \xC2\xA7""6.2)");
                        }
                        // Dispose(): unit; no return type set.
                    }
                    // §6.3 / issue #647: Semaphore instance methods.
                    // Semaphore TypeRef: Named, no declaration, astNode name "Semaphore".
                    if (rcn == "Semaphore" && recvTy) {
                        // Helper: build a SemaphoreToken TypeRef (nullary, no declaration).
                        static const ast::Type* kSemTokenNode = []() -> ast::Type* {
                            auto* qn = new ast::QualifiedName;
                            qn->parts.push_back("SemaphoreToken");
                            auto* t = new ast::Type;
                            t->kind = ast::TypeKind::Named;
                            t->name.reset(qn);
                            return t;
                        }();
                        auto makeSemTokenTypeRef = [&]() -> TypeRef* {
                            auto* tr = ctx_.makeTypeRef();
                            tr->kind = TypeRef::Kind::Named;
                            tr->nullable = false;
                            tr->declaration = nullptr;
                            tr->astNode = kSemTokenNode;
                            return tr;
                        };
                        if (m == "Acquire" && ce.args.empty()) {
                            // Acquire(): owned SemaphoreToken.
                            ctx_.setExprType(&ce, makeSemTokenTypeRef());
                        } else if (m == "TryAcquire" && ce.args.size() == 1) {
                            // TryAcquire(timeout: TimeSpan): owned SemaphoreToken?
                            auto* tr = makeSemTokenTypeRef();
                            tr->nullable = true;
                            ctx_.setExprType(&ce, tr);
                        } else if (m == "AcquireAsync" || m == "TryAcquireAsync") {
                            // §6.3 / #647 / v0.140:
                            //   AcquireAsync(token := CancellationToken.None)
                            //     → OwnedTask<SemaphoreToken>
                            //   TryAcquireAsync(timeout: TimeSpan,
                            //                   token := CancellationToken.None)
                            //     → OwnedTask<SemaphoreToken?>
                            auto makeOwnedTaskOf = [&](TypeRef* inner) -> TypeRef* {
                                auto* ot = ctx_.makeTypeRef();
                                ot->kind = TypeRef::Kind::Named;
                                ot->nullable = false;
                                auto* ann = new ast::Type();
                                ann->kind = ast::TypeKind::Named;
                                auto* nm = new ast::QualifiedName();
                                nm->parts.push_back("OwnedTask");
                                ann->name.reset(nm);
                                ot->astNode = ann;
                                if (inner) ot->genericArgs.push_back(inner);
                                return ot;
                            };
                            if (m == "AcquireAsync") {
                                // OwnedTask<SemaphoreToken>
                                ctx_.setExprType(&ce,
                                    makeOwnedTaskOf(makeSemTokenTypeRef()));
                            } else {
                                // TryAcquireAsync → OwnedTask<SemaphoreToken?>
                                auto* tokNullable = makeSemTokenTypeRef();
                                tokNullable->nullable = true;
                                ctx_.setExprType(&ce,
                                    makeOwnedTaskOf(tokNullable));
                            }
                        } else if (m != "Dispose") {
                            // §6.3 / #674: Semaphore exposes only Acquire(),
                            // TryAcquire(), AcquireAsync(), TryAcquireAsync(),
                            // and Dispose().  Any other member is an error.
                            ctx_.diagnostics().report(
                                Severity::Error, ce.span.begin,
                                std::string("no member `") + std::string(m) +
                                "` on `Semaphore`; documented surface: "
                                "Acquire(), TryAcquire(timeout: TimeSpan), "
                                "AcquireAsync(), TryAcquireAsync(), Dispose() "
                                "(stdlib \xC2\xA7""6.3)");
                        }
                        // Dispose(): unit; no return type set.
                    }
                    // issue #545: Buffer<T> instance methods.
                    // Receiver: Named, no declaration, name "Buffer", genericArgs[0]=T.
                    // Surface: [i] (indexer), Length (property), Dispose().
                    // Unknown method produces a clean compile error.
                    if (rcn == "Buffer" && recvTy) {
                        if (m == "Dispose") {
                            // Dispose(): unit; no return type set.
                        } else {
                            ctx_.diagnostics().report(
                                Severity::Error, ce.span.begin,
                                std::string("no member `") + std::string(m) +
                                "` on `Buffer<T>`; documented surface: "
                                "[i] (get/set), Length, Dispose() "
                                "(issue #545)");
                        }
                    }
                    // §6.4 / issue #647: Atomic<T> instance methods.
                    // Receiver TypeRef: Named, no declaration, astNode name "Atomic",
                    // genericArgs[0] = T (the element type).
                    // Value is a property — getter handled here as a method-call-like
                    // case so that `v := atom.Value` works; setter handled in sema
                    // as a block-setter-absent path (codegen intercepts via lowerExpr).
                    if ((rcn == "Atomic" || rcn == "AtomicInteger") && recvTy) {
                        // Helper: get the T TypeRef (genericArgs[0] of the receiver).
                        auto getElemTy = [&]() -> TypeRef* {
                            if (!recvTy->genericArgs.empty() && recvTy->genericArgs[0])
                                return const_cast<TypeRef*>(recvTy->genericArgs[0]);
                            // Fallback: int if T unavailable.
                            return ctx_.primitiveTypeRef(P::Int);
                        };
                        // Exchange(value: T): T — returns the old value.
                        if (m == "Exchange" && ce.args.size() == 1) {
                            ctx_.setExprType(&ce, getElemTy());
                        // CompareExchange(expected: T, replacement: T): T
                        // — returns the observed value (== expected on success).
                        } else if (m == "CompareExchange" && ce.args.size() == 2) {
                            ctx_.setExprType(&ce, getElemTy());
                        } else if (rcn == "AtomicInteger") {
                            // AtomicInteger<T>: integer-arithmetic extensions (§6.4).
                            // Increment(): T — post-increment.
                            // Decrement(): T — post-decrement.
                            // Add(delta: T): T — post-add.
                            if ((m == "Increment" || m == "Decrement") &&
                                ce.args.empty()) {
                                ctx_.setExprType(&ce, getElemTy());
                            } else if (m == "Add" && ce.args.size() == 1) {
                                ctx_.setExprType(&ce, getElemTy());
                            } else if (m != "Dispose") {
                                // §6.4 / #674: unknown method on AtomicInteger.
                                ctx_.diagnostics().report(
                                    Severity::Error, ce.span.begin,
                                    std::string("no member `") + std::string(m) +
                                    "` on `AtomicInteger<T>`; documented surface: "
                                    "Value (get/set), Exchange(T), "
                                    "CompareExchange(T, T), Increment(), "
                                    "Decrement(), Add(T), Dispose() "
                                    "(stdlib \xC2\xA7""6.4)");
                            }
                        } else if (m != "Dispose") {
                            // §6.4 / #674: unknown method on Atomic<T>.
                            ctx_.diagnostics().report(
                                Severity::Error, ce.span.begin,
                                std::string("no member `") + std::string(m) +
                                "` on `Atomic<T>`; documented surface: "
                                "Value (get/set), Exchange(T), "
                                "CompareExchange(T, T), Dispose() "
                                "(stdlib \xC2\xA7""6.4)");
                        }
                        // Dispose(): unit; no return type set.
                    }
                    // §9.8 / §2.15 / issue #550: BinarySerializer.ToBytes() →
                    // OwnedBuffer<byte>.  Set on the CallExpr so that inferred-type
                    // locals (`using var bytes := s.ToBytes()`) pick up the element
                    // type and downstream `bytes.AsReadOnlySpan()` can be intercepted
                    // by isSerializationCall / tryLowerSerializationCall.
                    // stdlib §5 (#651): IGrouping<TKey, T> instance methods.
                    // IGrouping<TKey, T>: Key (TKey property), Count() (int),
                    // and enumeration (bz_grouping_elements).
                    // rcn == "IGrouping" for the synthetic TypeRef built by GroupBy.
                    if (rcn == "IGrouping" && recvTy) {
                        if (m == "Count" && ce.args.empty()) {
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                        }
                        // .Key property: TKey is genericArgs[0].
                        // (Property accesses for IGrouping.Key are handled in
                        //  ExprTypecheck_Exprs visitMemberAccess, not here, but
                        //  we guard the method form just in case.)
                    }
                    if (rcn == "BinarySerializer") {
                        if (m == "ToBytes") {
                            static const ast::Type* kOwnedBufNode2 = []() -> ast::Type* {
                                auto* qn = new ast::QualifiedName;
                                qn->parts.push_back("OwnedBuffer");
                                auto* t = new ast::Type;
                                t->kind = ast::TypeKind::Named;
                                t->name.reset(qn);
                                return t;
                            }();
                            auto* byTy2 = ctx_.primitiveTypeRef(PrimitiveType::Byte);
                            auto* obTy2 = ctx_.makeTypeRef();
                            obTy2->kind  = TypeRef::Kind::Named;
                            obTy2->astNode = kOwnedBufNode2;
                            obTy2->genericArgs.push_back(byTy2);
                            ctx_.setExprType(&ce, obTy2);
                        } else if (
                            // ISerializer surface (stdlib §2.14): all return unit.
                            m == "WriteInt"     || m == "WriteUInt"    ||
                            m == "WriteFloat"   || m == "WriteDecimal" ||
                            m == "WriteBool"    || m == "WriteString"  ||
                            m == "WriteChar"    || m == "WriteNull"    ||
                            m == "BeginObject"  || m == "WriteFieldName" ||
                            m == "EndObject"    || m == "BeginArray"   ||
                            m == "EndArray"     || m == "Dispose") {
                            // All ISerializer / IDisposable methods return unit;
                            // no return type needs to be set.
                        } else {
                            // §2.15 / #674: BinarySerializer exposes ToBytes()
                            // plus the full ISerializer surface (stdlib §2.14).
                            // Any other member name is an error.
                            ctx_.diagnostics().report(
                                Severity::Error, ce.span.begin,
                                std::string("no member `") + std::string(m) +
                                "` on `BinarySerializer`; documented surface: "
                                "ToBytes(), WriteInt/UInt/Float/Decimal/Bool/"
                                "String/Char/WriteNull(), BeginObject/EndObject(), "
                                "WriteFieldName(), BeginArray/EndArray(), Dispose() "
                                "(stdlib \xC2\xA7""2.14" "\xE2\x80\x93" "2.15)");
                        }
                    }
                    // §2.15 / issue #550: JsonSerializer.ToBytes() → OwnedBuffer<byte>.
                    // JsonSerializer.ToString() → string.
                    if (rcn == "JsonSerializer" && m == "ToBytes") {
                        static const ast::Type* kJserBufNode = []() -> ast::Type* {
                            auto* qn = new ast::QualifiedName;
                            qn->parts.push_back("OwnedBuffer");
                            auto* t = new ast::Type;
                            t->kind = ast::TypeKind::Named;
                            t->name.reset(qn);
                            return t;
                        }();
                        auto* byTy3 = ctx_.primitiveTypeRef(PrimitiveType::Byte);
                        auto* obTy3 = ctx_.makeTypeRef();
                        obTy3->kind  = TypeRef::Kind::Named;
                        obTy3->astNode = kJserBufNode;
                        obTy3->genericArgs.push_back(byTy3);
                        ctx_.setExprType(&ce, obTy3);
                    }
                    if (rcn == "JsonSerializer" && m == "ToString") {
                        ctx_.setExprType(&ce, ctx_.primitiveTypeRef(PrimitiveType::String));
                    }
                }

                // §30.14 (#600): Domain.Spawn — static class, colName == "Domain".
                // Domain.Spawn(entry, args...) validates:
                //   1. entry must be capture-free (static ref or non-capturing lambda)
                //   2. entry args must be sendable value/string or owned sendable class
                //   3. entry result type (if any) must be sendable
                // Returns: OwnedTask (void entry) or OwnedTask<R> (R = sendable result).
                if (colName == "Domain" && m == "Spawn" && !ce.args.empty()) {
                    // Build an OwnedTask TypeRef helper (static, reused below).
                    auto makeOwnedTask = [&](const TypeRef* inner) -> TypeRef* {
                        auto* ot = ctx_.makeTypeRef();
                        ot->kind = TypeRef::Kind::Named;
                        ot->nullable = false;
                        auto* ann = new ast::Type();
                        ann->kind = ast::TypeKind::Named;
                        auto* nm = new ast::QualifiedName();
                        nm->parts.push_back("OwnedTask");
                        ann->name.reset(nm);
                        ot->astNode = ann;
                        if (inner) ot->genericArgs.push_back(const_cast<TypeRef*>(inner));
                        return ot;
                    };

                    // Arg 0: the entry function reference or lambda.
                    const ast::Expr* entryExpr = ce.args[0].value.get();

                    // Check 1: capture-free entry.
                    bool entryOk = isCaptureFreeEntry(entryExpr);
                    if (!entryOk) {
                        ctx_.diagnostics().report(
                            Severity::Error, ce.span.begin,
                            "Domain.Spawn entry must be capture-free: a non-"
                            "capturing lambda or a static method reference — "
                            "a capturing lambda or an instance method reference "
                            "is rejected because captures and receivers are "
                            "borrows of the origin domain (\xC2\xA7""30.14, #600)");
                    }

                    // Determine the entry function's parameter types and return type
                    // from the resolved entry expression's Function TypeRef.
                    const TypeRef* entryFnTy = entryExpr ? ctx_.exprType(*entryExpr)
                                                          : nullptr;
                    const TypeRef* entryReturnTy = nullptr;
                    std::vector<const TypeRef*> entryParamTypes;

                    if (entryFnTy && entryFnTy->kind == TypeRef::Kind::Function) {
                        entryReturnTy = entryFnTy->funcReturn;
                        // entryFnTy->funcReturnOwned unused in sema phase (c = runtime)
                        for (const auto& fp : entryFnTy->funcParams)
                            entryParamTypes.push_back(fp.type);
                    }

                    // Check 2: per-argument sendability (args 1..N match params).
                    // For a static method reference, the entry's params come from the
                    // method symbol. For a lambda, from the lambda type.
                    // Arguments 1..N of Domain.Spawn correspond to entry params 0..N-1.
                    for (std::size_t i = 1; i < ce.args.size(); ++i) {
                        const ast::Expr* argExpr = ce.args[i].value.get();
                        if (!argExpr) continue;
                        const TypeRef* argTy = ctx_.exprType(*argExpr);
                        if (!argTy) continue;

                        // The arg must be sendable.
                        bool argSendable = isSendable(argTy);

                        // For class-typed args: the entry param must also declare
                        // `owned` (ownership transfer) — check via the entry function
                        // type's param modifier if available.
                        if (!argSendable) {
                            ctx_.diagnostics().report(
                                Severity::Error, argExpr->span.begin,
                                "Domain.Spawn argument is not sendable: argument "
                                "types to a spawned domain must satisfy §30.14 "
                                "sendable-type rules (not borrow-bearing, not "
                                "function-typed, class types must be "
                                "transfer-closed/sealed owned) "
                                "(\xC2\xA7""30.14, #600)");
                        }

                        // For class-typed args: check that the entry param is `owned`.
                        // (A class passed by borrow — `m: Msg?` — cannot cross the
                        // domain boundary; it must be `owned m: Msg`.)
                        std::size_t pi = i - 1;  // param index
                        if (pi < entryParamTypes.size() && entryParamTypes[pi]) {
                            const TypeRef* paramTy = entryParamTypes[pi];
                            bool paramIsClass = paramTy->kind == TypeRef::Kind::Named &&
                                               paramTy->declaration &&
                                               paramTy->declaration->kind ==
                                                   ast::TypeDeclKind::Class;
                            if (paramIsClass) {
                                // Check if the entry function type param has "owned".
                                bool paramOwned = false;
                                if (entryFnTy && pi < entryFnTy->funcParams.size())
                                    paramOwned = (entryFnTy->funcParams[pi].modifier
                                                 == "owned");
                                if (!paramOwned) {
                                    ctx_.diagnostics().report(
                                        Severity::Error, argExpr->span.begin,
                                        "Domain.Spawn argument is a class type but "
                                        "the entry function does not take it as "
                                        "`owned`: class arguments to Domain.Spawn must "
                                        "be transferred (entry parameter must be "
                                        "`owned`) (\xC2\xA7""30.14, #600)");
                                }
                            }
                        }
                    }

                    // Check 3: return-type sendability (if entry returns a value).
                    // entryReturnTy is null / Unit for void entries.
                    if (entryReturnTy && entryReturnTy->kind != TypeRef::Kind::Unit) {
                        if (!isSendable(entryReturnTy)) {
                            ctx_.diagnostics().report(
                                Severity::Error, ce.span.begin,
                                "Domain.Spawn entry result type is not sendable: "
                                "the result must satisfy §30.14 sendable-type "
                                "rules (not borrow-bearing, class types must be "
                                "transfer-closed/sealed owned) "
                                "(\xC2\xA7""30.14, #600)");
                        }
                    }

                    // Type the Spawn call:
                    //   - void entry → OwnedTask (bare, no T)
                    //   - owned R entry → OwnedTask<R>
                    //   - value-type R entry → OwnedTask<R> (value crossed as copy)
                    if (!entryReturnTy || entryReturnTy->kind == TypeRef::Kind::Unit) {
                        ctx_.setExprType(&ce, makeOwnedTask(nullptr));
                    } else {
                        ctx_.setExprType(&ce, makeOwnedTask(entryReturnTy));
                    }
                }
            }
        }
    }
    if (!calleeSym) {
        if (auto* rt = ctx_.exprType(*ce.callee)) {
            if (rt->kind == TypeRef::Kind::Function && rt->funcReturn)
                ctx_.setExprType(&ce, rt->funcReturn);
            else if (rt->kind != TypeRef::Kind::Function)
                ctx_.setExprType(&ce, rt);
            // If Function with null funcReturn: leave untyped (void/unknown)
        }
        // stdlib §2.2: Math.Abs is polymorphic — its return type matches the
        // argument type (Abs(int):int, Abs(long):long, Abs(double):double).
        // Math.Min/Max/Clamp<T> are generic and also return T (same-as-arg).
        // Math.Sign is NOT polymorphic — every overload (Sign(int), Sign(long),
        // Sign(double)) always returns int (stdlib §2.2: "Sign(x: double): int").
        // Including Sign here previously caused Sign(double) to be typed as double,
        // which then crashed codegen with an ICmp type-mismatch (SIGABRT) when the
        // double result was compared against an int literal.
        if (auto* rt = ctx_.exprType(ce)) {
            if (rt->kind == TypeRef::Kind::Primitive &&
                rt->primitive == PrimitiveType::Int &&
                !ce.args.empty() && ce.args[0].value) {
                const ast::Expr& argE = *ce.args[0].value;
                if (auto* argTy = ctx_.exprType(argE)) {
                    if (argTy->kind == TypeRef::Kind::Primitive &&
                        (argTy->primitive == PrimitiveType::Double ||
                         argTy->primitive == PrimitiveType::Float ||
                         argTy->primitive == PrimitiveType::Long)) {
                        // Determine which method this is — only override
                        // for the numeric functions that return same-as-arg.
                        // Sign is intentionally excluded (always returns int).
                        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                                ce.callee.get())) {
                            if (ma->member == "Abs" ||
                                ma->member == "Min" || ma->member == "Max" ||
                                ma->member == "Clamp") {
                                ctx_.setExprType(&ce, argTy);
                            }
                        }
                    }
                }
            }
        }
        // #1141 / stdlib §1: Math.Min<T>/Max<T>/Clamp<T> (declared only as
        // `where T: IComparable<T>`) instantiated with a non-numeric VALUE type
        // — a record/enum that implements IComparable<T> (§14.2). The result
        // type is T (same as the operands). The numeric same-as-arg block above
        // only covers primitive double/float/long, so a record/enum operand left
        // the call mistyped (defaulting to int) and a `.field` access on the
        // result folded to 0. Type the call as the operand's type so member
        // access on the result resolves. (Codegen routes the comparison through
        // the operand type's `CompareTo` — see tryLowerBuiltinClassCall.)
        if (auto* ma =
                dynamic_cast<const ast::MemberAccessExpr*>(ce.callee.get())) {
            if ((ma->member == "Min" || ma->member == "Max" ||
                 ma->member == "Clamp") &&
                ma->target && !ce.args.empty() && ce.args[0].value) {
                if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                        ma->target.get())) {
                    if (tid->name == "Math") {
                        if (auto* argTy = ctx_.exprType(*ce.args[0].value))
                            if (argTy->kind == TypeRef::Kind::Named &&
                                argTy->declaration &&
                                (argTy->declaration->kind ==
                                     ast::TypeDeclKind::Record ||
                                 argTy->declaration->kind ==
                                     ast::TypeDeclKind::Enum))
                                ctx_.setExprType(&ce, argTy);
                    }
                }
            }
        }
    }
    // CallExpr where the callee is a constructor reference (e.g.
    // `T(args)` form — rare in Bezel since `new T(args)` is the
    // canonical form), or the callee is a type symbol, are not yet
    // handled. Likewise calls on lambda values await the
    // function-type slice.

    // Slice T5: argument-count check (§22.5). Only when the callee
    // resolves to a method that is the SOLE overload of its name in
    // its type, and the call uses all-positional arguments. Named
    // arguments and overload sets are skipped (overload resolution
    // is a later slice). Optional parameters (those with a default)
    // set the minimum; the total is the maximum (no variadics in
    // Bezel — §22.5).
    if (auto* ms = dynamic_cast<MethodSymbol*>(calleeSym)) {
        checkCallArity(ce, ms);
    }

    // §7.5 / FINDING [3] ADDENDUM: a null-conditional call `x?.M(args)` is
    // ALWAYS nullable — it returns null when the receiver is null. The callee is
    // a NullCondMemberExpr; lift the (raw) return type recorded above to its
    // nullable form so downstream consumers (`??`, `= null`, `!`, an `int?`
    // slot) see the §7.5 result type. A unit/void result and an already-nullable
    // return are left unchanged.
    if (auto* ncm = dynamic_cast<const ast::NullCondMemberExpr*>(ce.callee.get())) {
        // §8.1/§8.3/§8.5 (#1075): an inherited Object value-semantics method
        // (`ToString`/`GetHashCode`/`Equals`) on a VALUE-type/enum/record
        // receiver has no MethodSymbol, so the member-access value-method typing
        // (keyed on a plain `.`) never ran and the call is left untyped — codegen
        // then zero-inits the `T?` result (= null) or crashes a string compare.
        // Set the raw return type here (ToString→string, GetHashCode→int,
        // Equals→bool) so the lift below makes it nullable, matching the codegen
        // null-conditional value-method lowering.
        if (!ctx_.exprType(ce) && ncm->target) {
            const TypeRef* rt = ctx_.exprType(*ncm->target);
            const TypeSymbol* rd = rt ? rt->declaration : nullptr;
            bool valuePrim = rt && rt->kind == TypeRef::Kind::Primitive &&
                rt->primitive != PrimitiveType::String &&
                rt->primitive != PrimitiveType::Object;
            bool valueNamed = rd && (rd->kind == ast::TypeDeclKind::Record ||
                                     rd->kind == ast::TypeDeclKind::Enum);
            if (valuePrim || valueNamed) {
                if (ncm->member == "ToString" && ce.args.empty())
                    ctx_.setExprType(&ce, ctx_.primitiveTypeRef(PrimitiveType::String));
                else if (ncm->member == "GetHashCode" && ce.args.empty())
                    ctx_.setExprType(&ce, ctx_.primitiveTypeRef(PrimitiveType::Int));
                else if (ncm->member == "Equals" && ce.args.size() == 1)
                    ctx_.setExprType(&ce, ctx_.primitiveTypeRef(PrimitiveType::Bool));
            }
        }
        if (auto* ct = ctx_.exprType(ce)) {
            if (!ct->nullable && ct->kind != TypeRef::Kind::Unit) {
                TypeRef* nn = ctx_.makeTypeRef(*ct);
                nn->nullable = true;
                ctx_.setExprType(&ce, nn);
            }
        }
    }
}

void ExprTC::checkCallArity(const ast::CallExpr& ce, MethodSymbol* ms) {
    auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
    // §2.2 / §2.5 (issue #328 inc 3): an imported method has no ast::MethodDecl;
    // type-check the call against the imported signature (§2.6 step 3) using the
    // reconstructed ParameterSymbol list. Only the single-overload, all-positional
    // case is checked here (cross-module overload selection is deferred); a named
    // or modifier argument is left to the deferred path rather than misreported.
    if (!md && ctx_.isImported(ms)) {
        checkImportedCallArity(ce, ms);
        return;
    }
    if (!md || !ms->parent) return;
    // §22.5 named-argument binding is now performed by checkArgsAgainstParams,
    // so a named-arg call is checked here too (no bail).
    // `ms` is the overload resolveMethodOverload selected for this call. Count
    // the name's overloads across the receiver's whole class hierarchy (#1090):
    // counting only ms->parent's own members missed an overload declared in a
    // base class, so a call to an inherited overload was arity-checked against
    // the wrong (sole-assumed) member and mis-rejected.
    std::vector<MethodSymbol*> overloads;
    collectHierarchyOverloads(ms->parent, ms->name, overloads);
    if (overloads.size() != 1) {
        // Multi-overload set: overload resolution already validated the call
        // against the chosen overload, so re-checking would double-report. The
        // one gap is a call that matches NO overload (resolution fell back to a
        // non-binding member) — surface its arity error rather than letting it
        // slip through unchecked.
        BindResult b = bindArgsToParams(md->params, ce.args, ce.span.begin,
                                        std::string(), /*emit=*/false);
        if (!b.ok)
            checkArgsAgainstParams(md->params, ce.args, ce.span.begin,
                                   std::string("call to `") +
                                   std::string(ms->name) + "`");
        return;
    }
    checkArgsAgainstParams(md->params, ce.args, ce.span.begin,
                           std::string("call to `") +
                           std::string(ms->name) + "`");
}

// §2.2 / §2.5 / §2.6 step 3 (issue #328 inc 3): type-check a call to an imported
// method against its reconstructed signature. The imported method carries no
// ast::Param list; its parameters are ParameterSymbols whose resolved types ride
// `resolvedDeclType`. Checks the all-positional, single-overload case: argument
// count (§22.5) and per-argument assignability (§5.1). A named/modifier argument
// is conservatively skipped (cross-module overload + by-ref binding deferred).
void ExprTC::checkImportedCallArity(const ast::CallExpr& ce, MethodSymbol* ms) {
    for (const auto& a : ce.args)
        if (!a.name.empty() || !a.modifier.empty() || a.isDiscard) return;
    const std::size_t nparams = ms->params.size();
    const std::string desc =
        std::string("call to `") + std::string(ms->name) + "`";
    if (ce.args.size() != nparams) {
        ctx_.diagnostics().report(
            Severity::Error, ce.span.begin,
            desc + " supplies " + std::to_string(ce.args.size()) +
                " argument(s) but the parameter list has " +
                std::to_string(nparams) + " (§22.5)");
        return;
    }
    for (std::size_t j = 0; j < nparams; ++j) {
        auto* p = dynamic_cast<ParameterSymbol*>(ms->params[j]);
        if (!p || !p->resolvedDeclType) continue;
        const ast::CallArgument& a = ce.args[j];
        if (!a.value) continue;
        checkAssignable(ctx_.exprType(*a.value), p->resolvedDeclType,
                        a.value->span.begin,
                        desc + ": argument " + std::to_string(j + 1));
    }
}

// §22.5 argument-to-parameter binding. See header for the contract.
// Positional arguments fill parameters in order; a named argument binds to
// the parameter of that name; "named arguments may appear in any order after
// positional arguments" — so a positional argument after a named one is an
// error. Each parameter may be bound at most once; every required (no
// default) parameter must be bound; an unknown parameter name is an error.
ExprTC::BindResult ExprTC::bindArgsToParams(
    const std::vector<ast::Param>& params,
    const std::vector<ast::CallArgument>& args, SourceLocation loc,
    const std::string& calleeDesc, bool emit) {
    BindResult res;
    res.paramToArg.assign(params.size(), -1);
    auto fail = [&](const std::string& msg) -> BindResult {
        if (emit) ctx_.diagnostics().report(Severity::Error, loc, msg);
        res.ok = false;
        return res;
    };

    bool sawNamed = false;
    std::size_t pos = 0;  // next positional parameter slot
    for (std::size_t ai = 0; ai < args.size(); ++ai) {
        const ast::CallArgument& a = args[ai];
        if (a.name.empty()) {
            // per §22.5: positional arguments precede named arguments.
            if (sawNamed)
                return fail(calleeDesc +
                            ": a positional argument may not follow a named "
                            "argument (§22.5 — named arguments come after all "
                            "positional arguments)");
            if (pos >= params.size())
                return fail(calleeDesc + " supplies " +
                            std::to_string(args.size()) +
                            " argument(s) but the parameter list has " +
                            std::to_string(params.size()) + " (§22.5)");
            res.paramToArg[pos] = static_cast<int>(ai);
            ++pos;
        } else {
            sawNamed = true;
            std::size_t idx = params.size();
            for (std::size_t j = 0; j < params.size(); ++j)
                if (params[j].name == a.name) { idx = j; break; }
            // per §22.5: a named argument names an existing parameter.
            if (idx >= params.size())
                return fail(calleeDesc + " has no parameter named `" +
                            std::string(a.name) + "` (§22.5)");
            // per §22.5: no parameter may be bound twice (positionally then
            // by name, or by two named arguments).
            if (res.paramToArg[idx] != -1)
                return fail(calleeDesc + ": parameter `" +
                            std::string(a.name) +
                            "` is bound more than once (§22.5)");
            res.paramToArg[idx] = static_cast<int>(ai);
        }
    }

    // per §22.5: every required (non-default) parameter must be bound, either
    // positionally or by name; an unbound optional parameter takes its
    // default.
    for (std::size_t j = 0; j < params.size(); ++j) {
        if (res.paramToArg[j] == -1 && !params[j].defaultValue)
            return fail(calleeDesc + ": required parameter `" +
                        std::string(params[j].name) +
                        "` is not supplied (§22.5)");
    }

    res.ok = true;
    return res;
}

// Slices T5/T6/T7: shared check of an argument list against a parameter
// list — §22.5 binding (positional then named, arity, no-duplicate,
// required-supplied), then per-argument assignability over the bound
// mapping. Callers must have already ruled out overload ambiguity.
void ExprTC::checkArgsAgainstParams(const std::vector<ast::Param>& params,
                                    const std::vector<ast::CallArgument>& args,
                                    SourceLocation loc,
                                    const std::string& calleeDesc) {
    // per §22.5: bind arguments to parameters. The binding emits its own
    // arity / named-arg diagnostics; on failure there is no consistent
    // per-parameter mapping to type-check, so stop here.
    BindResult b = bindArgsToParams(params, args, loc, calleeDesc, /*emit=*/true);
    if (!b.ok) return;
    // per §5.1: each bound argument must be assignable to its parameter.
    // Iterate in parameter order so a reordered named-arg call checks each
    // argument against the parameter it actually fills.
    for (std::size_t j = 0; j < params.size(); ++j) {
        int ai = b.paramToArg[j];
        if (ai < 0) continue;  // defaulted parameter — default-value typing
                               // is checked at the declaration, not here.
        const ast::CallArgument& a = args[static_cast<std::size_t>(ai)];
        if (!a.value || !a.modifier.empty() || a.isDiscard) continue;
        const ast::Param& p = params[j];
        if (!p.type) continue;
        checkAssignable(ctx_.exprType(*a.value),
                        ctx_.resolvedType(*p.type),
                        a.value->span.begin,
                        std::string("argument `") + std::string(p.name) +
                        "` to " + calleeDesc);
    }
}

// Slice T7 / §22.6: check `new T(args)` against T's constructors. Zero
// declared constructors → the implicit default ctor takes no arguments.
// One constructor → argument count/type check. Multiple constructors →
// §22.6 type-based overload resolution (exact-match-then-ambiguity).
void ExprTC::checkNewExprArgs(const ast::NewExpr& ne, TypeSymbol* t) {
    // A class with a primary constructor (parenthesized params after
    // the name) carries those params on the TypeDecl rather than as
    // a ConstructorSymbol, and may combine with chained body
    // constructors. That interaction is deferred — skip such types.
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(t->decl)) {
        if (td->hasPositionalFields) {
            // #1105 (§9.1): a RECORD's positional fields ARE its primary
            // constructor — one argument per field, in declaration order, no
            // chaining and no per-field defaults. Validate `new Record(args)`
            // here; record construction otherwise skipped ALL argument checking
            // (the early return), so too-few / too-many / wrong-typed args were
            // silently accepted and miscompiled (`new Pt(1)`→Y=0; `new
            // Pt("a","b")`→garbage; `new Pt(3.14,..)`→int bit-pattern; some
            // wrong-typed forms reach codegen and crash IR verification). A
            // primary-constructor CLASS may combine positional fields with
            // chained body constructors (the deferred interaction above) — so
            // restrict to records. Generic records need per-instantiation type
            // substitution of the field types (the #428 family) — defer those;
            // validate non-generic records, which is where the cluster lives.
            if (t->kind == ast::TypeDeclKind::Record &&
                t->genericParams.empty()) {
                // The positional fields are the constructor's parameter list
                // (one per field, declaration order). Reuse the same arity +
                // named-argument binding + per-argument assignability check the
                // class-constructor / method paths use (checkArgsAgainstParams):
                // it handles reordered named args (`new Pt(Y := 4, X := 3)`).
                const auto& fields = td->positionalFields;
                // §5.1 (#738): a record positional argument initializes a field,
                // so an in-range int literal/constant bound to a narrower-int
                // field (`new Box(30000)` into `short?`) narrows implicitly —
                // mirror the local-decl / assignment retyping (ExprTypecheck.cpp
                // AssignStmt path) before the assignability check. Bind first
                // (no diagnostics) so each argument's target field is known even
                // for reordered named args.
                auto fitsInIntTy = [](long long v, PrimitiveType pt) -> bool {
                    switch (pt) {
                        case PrimitiveType::SByte:  return v >= -128 && v <= 127;
                        case PrimitiveType::Byte:   return v >= 0 && v <= 255;
                        case PrimitiveType::Short:  return v >= -32768 && v <= 32767;
                        case PrimitiveType::UShort: return v >= 0 && v <= 65535;
                        case PrimitiveType::UInt:   return v >= 0 && v <= 4294967295LL;
                        case PrimitiveType::ULong:  return v >= 0;
                        default:                    return true;
                    }
                };
                BindResult pre = bindArgsToParams(
                    fields, ne.args, ne.span.begin,
                    /*calleeDesc=*/std::string(), /*emit=*/false);
                if (pre.ok) {
                    for (std::size_t j = 0; j < fields.size(); ++j) {
                        int ai = pre.paramToArg[j];
                        if (ai < 0 || !fields[j].type) continue;
                        const ast::CallArgument& a =
                            ne.args[static_cast<std::size_t>(ai)];
                        if (!a.value || a.isDiscard || !a.modifier.empty())
                            continue;
                        TypeRef* fromTy = ctx_.exprType(*a.value);
                        TypeRef* toTy = ctx_.resolvedType(*fields[j].type);
                        if (fromTy && toTy &&
                            fromTy->kind == TypeRef::Kind::Primitive &&
                            fromTy->primitive == PrimitiveType::Int &&
                            toTy->kind == TypeRef::Kind::Primitive) {
                            PrimitiveType tgt = toTy->primitive;
                            bool narrower =
                                tgt == PrimitiveType::Byte ||
                                tgt == PrimitiveType::SByte ||
                                tgt == PrimitiveType::Short ||
                                tgt == PrimitiveType::UShort ||
                                tgt == PrimitiveType::UInt ||
                                tgt == PrimitiveType::ULong;
                            if (narrower) {
                                auto cv = foldIntConstExprTC(*a.value);
                                if (cv && fitsInIntTy(*cv, tgt))
                                    ctx_.setExprType(a.value.get(), toTy);
                            }
                        }
                    }
                }
                checkArgsAgainstParams(
                    fields, ne.args, ne.span.begin,
                    std::string("`new ") + std::string(t->name) + "`");
            }
            return;
        }
    }
    std::vector<ConstructorSymbol*> ctors;
    for (Symbol* m : t->members) {
        if (auto* cs = dynamic_cast<ConstructorSymbol*>(m)) {
            if (!cs->isStatic) ctors.push_back(cs);
        }
    }
    // §22.3 / §22.6 (#967): a `ref`/`out` constructor parameter is mandatory at
    // the call site, exactly as for a method (#952). Enforce it for the single
    // bound constructor — a `ref` parameter requires a `ref` argument, an
    // `out`/`out owned` parameter an `out`-family argument, and a plain/`owned`
    // parameter none. (Without this the codegen ref-param fix would let a
    // missing-`ref` `new C(x)` silently mutate the argument.) Returns true and
    // emits a §22.3 diagnostic on a mismatch. Constructor OVERLOAD sets with
    // call-site modifiers remain deferred below (filed follow-up).
    auto refOutMatch = [](std::string_view argMod,
                          std::string_view paramMod) -> bool {
        auto isOut = [](std::string_view m) {
            return m == "out" || m == "out var" || m == "out using var" ||
                   m == "out owned";
        };
        bool argRef = argMod == "ref", argOut = isOut(argMod);
        bool paramRef = paramMod == "ref";
        bool paramOut = paramMod == "out" || paramMod == "out owned";
        if (paramRef) return argRef;
        if (paramOut) return argOut;
        return !argRef && !argOut;
    };
    auto emitModifierMismatch = [&](const std::vector<ast::Param>& params,
                                    const BindResult& b) -> bool {
        for (std::size_t j = 0; j < params.size(); ++j) {
            int ai = b.paramToArg[j];
            std::string_view argMod =
                ai >= 0 ? ne.args[static_cast<std::size_t>(ai)].modifier
                        : std::string_view{};
            if (refOutMatch(argMod, params[j].modifier)) continue;
            std::string pname(params[j].name), pm(params[j].modifier),
                am(argMod);
            bool prefOut = pm == "ref" || pm == "out" || pm == "out owned";
            std::string detail =
                prefOut && am.empty()
                    ? "parameter `" + pname + "` is `" + pm +
                          "` — pass the argument with `" +
                          (pm == "out owned" ? std::string("out") : pm) +
                          "` at the call site"
                : (!prefOut && !am.empty()
                       ? "parameter `" + pname +
                             "` is not `ref`/`out` — remove the `" + am +
                             "` at the call site"
                       : "the `" + am + "` argument does not match parameter `" +
                             pname + "`'s `" + pm + "` modifier");
            ctx_.diagnostics().report(
                Severity::Error, ne.span.begin,
                "call to `new " + std::string(t->name) +
                "` has a call-site `ref`/`out` mismatch: " + detail +
                " (§22.3 — the caller writes `ref`/`out` at the call site, and "
                "only for a `ref`/`out` parameter)");
            return true;
        }
        return false;
    };
    // §2.5 / §22.7 (issue #328 FINDING [7] B): an IMPORTED type's constructors
    // carry no ast::ConstructorDecl — their parameters are ParameterSymbols whose
    // resolved types ride `resolvedDeclType`. Check `new ImportedT(args)` against
    // those reconstructed signatures (the AST-driven paths below would all bail
    // on the null decl). Mirrors checkImportedCallArity for imported method calls.
    if (ctx_.isImported(t)) {
        checkImportedNewExprArgs(ne, t, ctors);
        return;
    }
    if (ctors.size() > 1) {
        // §22.6 applies identically to constructor overload sets (§22.7);
        // §22.5 named-argument binding filters the candidate set first.
        // Collect the candidates this call's arguments can bind to (§22.5)
        // and record each binding so ranking operates on the bound positions.
        std::vector<const ast::ConstructorDecl*> decls;
        std::vector<BindResult> binds;
        for (ConstructorSymbol* cs : ctors) {
            auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
            if (!cd) continue;
            BindResult b = bindArgsToParams(cd->params, ne.args, ne.span.begin,
                                            /*calleeDesc=*/std::string(),
                                            /*emit=*/false);
            if (!b.ok) continue;
            decls.push_back(cd);
            binds.push_back(std::move(b));
        }
        if (decls.empty()) return;  // no bindable ctor — defer (no check)
        if (decls.size() == 1) {
            if (emitModifierMismatch(decls[0]->params, binds[0])) return;
            checkArgsAgainstParams(decls[0]->params, ne.args, ne.span.begin,
                                   std::string("`new ") +
                                       std::string(t->name) + "`");
            return;
        }
        // A call-site modifier on an argument is outside §22.6's ranking;
        // skip resolution in that case.
        for (const auto& a : ne.args)
            if (!a.modifier.empty() || a.isDiscard) return;
        // Build the per-candidate parameter-type and bound argument-type rows
        // (parameter order, defaulted slots dropped — they carry no ranking
        // pressure), exactly as resolveMethodOverload does for §22.5 calls.
        std::vector<std::vector<const TypeRef*>> paramTypes(decls.size());
        std::vector<std::vector<const TypeRef*>> argTypeRows(decls.size());
        std::vector<std::vector<int>> argIdxRows(decls.size());
        std::vector<std::string> candNames(decls.size());
        for (std::size_t c = 0; c < decls.size(); ++c) {
            const auto& params = decls[c]->params;
            for (std::size_t j = 0; j < params.size(); ++j) {
                int ai = binds[c].paramToArg[j];
                if (ai < 0) continue;
                paramTypes[c].push_back(
                    params[j].type ? ctx_.resolvedType(*params[j].type)
                                   : nullptr);
                argTypeRows[c].push_back(
                    ne.args[static_cast<std::size_t>(ai)].value
                        ? ctx_.exprType(
                              *ne.args[static_cast<std::size_t>(ai)].value)
                        : nullptr);
                argIdxRows[c].push_back(ai);
            }
            candNames[c] = std::string("new ") + std::string(t->name) +
                           methodSignatureString("", params);
        }
        // resolveOverload uses one shared argument vector across candidates,
        // so each column must be the same call argument for every candidate;
        // fall back if the bound shapes diverge (uncommon for ctor overloads).
        const std::vector<const TypeRef*>& argTypes = argTypeRows[0];
        for (std::size_t c = 0; c < decls.size(); ++c) {
            if (argIdxRows[c].size() != argIdxRows[0].size()) return;
            for (std::size_t i = 0; i < argIdxRows[c].size(); ++i)
                if (argIdxRows[c][i] != argIdxRows[0][i]) return;
        }
        // resolveOverload emits the ambiguous / unreachable diagnostic; the
        // winner needs no further per-argument check (each arg is already
        // exact or §5.1-convertible). On Undecidable we defer (no check),
        // matching the prior "overloaded → skip" behavior.
        resolveOverload(paramTypes, argTypes, candNames, ne.span.begin,
                        std::string("`new ") + std::string(t->name) + "`");
        return;
    }
    if (ctors.empty()) {
        // Implicit default constructor: zero parameters. (A primary
        // constructor would surface as a ConstructorSymbol, so the
        // empty case really is the no-arg default.) Any argument — named
        // or positional — is too many for a zero-parameter constructor.
        if (!ne.args.empty()) {
            ctx_.diagnostics().report(
                Severity::Error, ne.span.begin,
                std::string("`new ") + std::string(t->name) +
                "` has " + std::to_string(ne.args.size()) +
                " argument(s) but the implicit default constructor "
                "takes 0 (§22.7)");
        }
        return;
    }
    auto* cd = dynamic_cast<const ast::ConstructorDecl*>(ctors[0]->decl);
    if (!cd) return;
    // §22.3 (#967): enforce the mandatory call-site `ref`/`out` keyword.
    BindResult b = bindArgsToParams(cd->params, ne.args, ne.span.begin,
                                    /*calleeDesc=*/std::string(), /*emit=*/false);
    if (b.ok && emitModifierMismatch(cd->params, b)) return;
    checkArgsAgainstParams(cd->params, ne.args, ne.span.begin,
                           std::string("`new ") + std::string(t->name) +
                           "`");
}

// §2.2 / §2.5 / §22.7 (issue #328 FINDING [7] B): type-check `new ImportedT(args)`
// against the imported type's reconstructed constructor signatures. The imported
// ctors carry no ast::ConstructorDecl; their parameters are ParameterSymbols whose
// resolved types ride `resolvedDeclType`. Checks the all-positional case: with no
// explicit ctor the implicit default (0 args) applies (§22.7); with a single
// reconstructed ctor, argument count (§22.5) and per-argument assignability
// (§5.1) are checked. Multiple imported ctors (overloads) and named/modifier
// arguments are conservatively skipped (cross-module ctor overload resolution is
// deferred, as for imported method calls).
void ExprTC::checkImportedNewExprArgs(
    const ast::NewExpr& ne, TypeSymbol* t,
    const std::vector<ConstructorSymbol*>& ctors) {
    // §14.1 / §2.5 / §2.6 (issue #328 FINDING [2]; #428 / #662 foundation A):
    // constructing an IMPORTED GENERIC type requires importer-side
    // per-instantiation infrastructure (vtable + disposer). Foundation A (#662)
    // delivers that machinery for the ZERO-EXPLICIT-CONSTRUCTOR subset:
    //   — no explicit constructor in the class body (implicit default zero-arg
    //     construction; the spec §22.7 implicit constructor)
    //   — no field initializers (calloc zero-initializes fields; the importer
    //     cannot run field initializer expressions from the exporter's AST)
    // For this subset, `lowerNew` calls buildImportedGenericVtable to install the
    // per-instantiation vtable (with specialized Dispose) and returns the calloced
    // object with zero-initialized fields and the correct vtable pointer.  Classes
    // WITH an explicit constructor body remain gated: the ctor body is not carried
    // in the dependency's metadata (Foundation C / Blocker 3).
    if (!t->genericParams.empty()) {
        // §14/#662 gate for imported generic CLASS construction.
        // Foundation A: a class with NO explicit constructor → zero-arg implicit
        //   path, vtable installed by buildImportedGenericVtable.
        // Foundation C: a class WITH an explicit constructor whose body text was
        //   exported and successfully re-parsed → cs->decl is set; the inline-ctor
        //   codegen path (inlineCtorBody) emits the body with genericSubst_ active.
        // Still-gated: explicit ctor present but cs->decl is null (body not loaded).

        // §14 / §12 (#711): INTERIM GATE — imported generic class with a GENERIC
        // BASE CLASS (e.g. `Box<T> : Container<T>` both in an imported module).
        // The codegen path for Foundation C (inlineCtorBody) re-emits the base
        // class template constructor symbol (`bz$Container$ctor`) importer-side,
        // colliding at link with the exporter's copy.  The collision turns the
        // cryptic link error into a clean §14 compile-time diagnostic.
        // The full fix (importer-side per-instantiation base-ctor monomorphization)
        // is tracked under #428/#711 and is not yet implemented.
        for (const TypeSymbol* b : t->baseTypes) {
            if (b->kind == ast::TypeDeclKind::Class &&
                !b->genericParams.empty()) {
                ctx_.diagnostics().report(
                    Severity::Error, ne.span.begin,
                    std::string("constructing an imported generic class (`new ") +
                        std::string(t->name) +
                        "<...>`) whose generic base class (`" +
                        std::string(b->name) +
                        "<...>`) also lives in an imported module is not yet "
                        "supported (\xC2\xA7""14/\xC2\xA7""12, #711): the base "
                        "constructor would be re-emitted importer-side and collide "
                        "at link; importer-side per-instantiation base-ctor "
                        "monomorphization is the full fix (#428/#711)");
                return;
            }
        }

        bool noExplicitCtor = true;
        const ConstructorSymbol* explicitCs = nullptr;
        for (const Symbol* m : t->members) {
            const auto* cs = dynamic_cast<const ConstructorSymbol*>(m);
            if (cs && !cs->isStatic) {
                noExplicitCtor = false;
                if (!explicitCs) explicitCs = cs;
                break;
            }
        }
        if (noExplicitCtor && ne.args.empty()) {
            // Foundation A: no explicit ctor — fall through.
        } else if (explicitCs && explicitCs->decl != nullptr) {
            // Foundation C: ctor body was loaded — fall through to arg-check.
        } else {
            // Still-gated shape: explicit ctor present but body not yet loaded.
            ctx_.diagnostics().report(
                Severity::Error, ne.span.begin,
                std::string("constructing an imported generic type (`new ") +
                    std::string(t->name) +
                    "<...>`) requires the constructor body to have been exported "
                    "in the dependency's metadata (\xC2\xA7""14/#662 foundation C); "
                    "a zero-argument implicit-constructor generic class is accepted "
                    "(#662 foundation A)");
            return;
        }
    }
    if (ctors.empty()) {
        // §22.7: no declared ctor → implicit default takes zero arguments.
        if (!ne.args.empty())
            ctx_.diagnostics().report(
                Severity::Error, ne.span.begin,
                std::string("`new ") + std::string(t->name) + "` has " +
                    std::to_string(ne.args.size()) +
                    " argument(s) but the implicit default constructor "
                    "takes 0 (§22.7)");
        return;
    }
    // A named/modifier argument is outside this conservative check.
    for (const auto& a : ne.args)
        if (!a.name.empty() || !a.modifier.empty() || a.isDiscard) return;
    if (ctors.size() != 1) return;  // imported ctor overloads deferred
    ConstructorSymbol* cs = ctors[0];
    const std::size_t nparams = cs->params.size();
    const std::string desc =
        std::string("`new ") + std::string(t->name) + "`";
    if (ne.args.size() != nparams) {
        ctx_.diagnostics().report(
            Severity::Error, ne.span.begin,
            desc + " has " + std::to_string(ne.args.size()) +
                " argument(s) but the parameter list has " +
                std::to_string(nparams) + " (§22.5)");
        return;
    }
    for (std::size_t j = 0; j < nparams; ++j) {
        auto* p = dynamic_cast<ParameterSymbol*>(cs->params[j]);
        if (!p || !p->resolvedDeclType) continue;
        const ast::CallArgument& a = ne.args[j];
        if (!a.value) continue;
        checkAssignable(ctx_.exprType(*a.value), p->resolvedDeclType,
                        a.value->span.begin,
                        desc + ": argument " + std::to_string(j + 1));
    }
}

// §17.1: postfix `.` member access. The target's type determines
// the lookup scope. For now we handle `this.member` (lookup in the
// current type) and `<expr>.member` where the expression has a
// resolved Named or Primitive-Object type. Generics and inherited
// members from base types are deferred to a later slice.
// The TypeSymbol a member-access target denotes (for `.` and `?.`):
// `this`/`base` keywords, then the target's exprType, then a fallback
// through its exprSymbol (local/parameter declared type, or a type name
// for static access). Returns null when it can't be determined.
TypeSymbol* ExprTC::memberTargetType(const ast::Expr* target) {
    TypeSymbol* targetType = nullptr;
    // `this.X` — currentType_ takes precedence over name lookup so
    // we don't have to teach `this` to be an expression with type
    // currentType_.
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(target)) {
        if (id->name == "this") {
            return currentType_;
        } else if (id->name == "base" && currentType_) {
            // §12.3: `base.X` resolves X against the base class, so an
            // override in the current type is bypassed and the base
            // member is found.
            for (TypeSymbol* b : currentType_->baseTypes) {
                if (b && b->kind == ast::TypeDeclKind::Class) return b;
            }
        }
    }
    if (target) {
        auto* tt = ctx_.exprType(*target);
        if (tt && tt->kind == TypeRef::Kind::Named) {
            targetType = tt->declaration;
        } else if (!tt) {
            auto* sym = ctx_.exprSymbol(*target);
            if (auto* loc = dynamic_cast<LocalSymbol*>(sym)) {
                if (loc->declType &&
                    loc->declType->kind == TypeRef::Kind::Named) {
                    targetType = loc->declType->declaration;
                }
            } else if (auto* par = dynamic_cast<ParameterSymbol*>(sym)) {
                if (par->declType) {
                    if (auto* rt = ctx_.resolvedType(*par->declType)) {
                        if (rt->kind == TypeRef::Kind::Named) {
                            targetType = rt->declaration;
                        }
                    }
                } else if (par->resolvedDeclType &&
                           par->resolvedDeclType->kind == TypeRef::Kind::Named) {
                    // per §24.3: a context-typed lambda param (event handler).
                    targetType = par->resolvedDeclType->declaration;
                }
            } else if (auto* ts = dynamic_cast<TypeSymbol*>(sym)) {
                // Static member access `Type.member`: the target is a
                // type name, so look the member up on that type.
                targetType = ts;
            }
        }
    }
    return targetType;
}

// per §24.3: if `callee` is `<event>.Subscribe` / `.Add` / `.Remove`, return
// the event's payload type (the `: T` annotation, or unit when omitted). The
// callee's MemberAccess target carries the EventSymbol (set when the target was
// visited). Null otherwise.
TypeRef* ExprTC::eventHandlerPayloadType(const ast::Expr* callee) {
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(callee);
    if (!ma || !ma->target) return nullptr;
    if (ma->member != "Subscribe" && ma->member != "Add" &&
        ma->member != "Remove")
        return nullptr;
    auto* es = dynamic_cast<EventSymbol*>(ctx_.exprSymbol(*ma->target));
    if (!es) return nullptr;
    auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl);
    if (!ed || !ed->type) return nullptr;  // no-payload event → unit (no param)
    return ctx_.resolvedType(*ed->type);
}

// §21.9 / §17.13 / §2.11 (#949): is `e` an `<event>.Subscribe(...)` call whose
// owned-`Subscription` result is being dropped/discarded? The Subscribe call
// result is not typed by sema (codegen recognizes it structurally for disposal,
// LlvmBackend_Calls.cpp isSubscribeCall), so the type-based drop/discard checks
// gate on a null exprType and never fire. Recognize the same structural shape —
// a `.Subscribe` member call whose target resolves to an event member — so the
// hard error fires. (A Subscription obtained via other APIs — CancellationToken
// .Register, a stored IEvent value, a method returning `owned Subscription` —
// is a narrower follow-up; this targets the §24.3 direct-event case of #949.)
bool ExprTC::isEventSubscribeDrop(const ast::Expr* e) {
    auto* ce = dynamic_cast<const ast::CallExpr*>(e);
    if (!ce || !ce->callee) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(ce->callee.get());
    if (!ma || ma->member != "Subscribe" || !ma->target) return false;
    return dynamic_cast<EventSymbol*>(ctx_.exprSymbol(*ma->target)) != nullptr;
}

// stdlib §5 (D7): if `callee` is a LINQ operator/aggregate on a collection or
// lazy IEnumerable receiver whose lambda argument is `(T) -> …` over the
// source element type T, return T (the receiver's first generic arg). This lets
// an untyped LINQ lambda parameter (`(p) => p.x = 5`) be typed as the element
// type so its body resolves member access (`p.x`) — exactly as the
// event-handler payload mechanism (§24.3) types `(e) => …` handler params.
// Null when the call is not such a LINQ operator (no seeding applied).
TypeRef* ExprTC::linqLambdaElementType(const ast::Expr* callee) {
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(callee);
    if (!ma || !ma->target) return nullptr;
    const auto& m = ma->member;
    // LINQ operators/aggregates whose (single) lambda is `(T) -> …` over the
    // source element T (stdlib §5). Select's selector and OrderBy's key fn are
    // also `(T) -> …`; Distinct/Take/Skip take no lambda (excluded).
    // TakeWhile/SkipWhile take `(T) -> bool`; SelectMany takes
    // `(T) -> IEnumerable<U>`; Average takes `(T) -> numeric` — all single-param
    // over the source element T, so they seed the same way.
    bool elemLambda =
        m == "Where" || m == "Select" || m == "Count" || m == "Any" ||
        m == "All" || m == "First" || m == "Last" || m == "Single" ||
        m == "OrderBy" || m == "OrderByDescending" || m == "ForEach" ||
        m == "Min" || m == "Max" || m == "Sum" ||
        m == "TakeWhile" || m == "SkipWhile" || m == "SelectMany" ||
        m == "Average" ||
        // stdlib §5 Materialization (#738): ToDictionary's key selector
        // `(T) -> TKey` is over the same source element T — seed it so an
        // un-annotated `(x) => x` types its param as T (else the inferred
        // Dictionary<K,V> result gets a wrong/null key type and an indexed
        // read mismatches).
        m == "ToDictionary" ||
        // stdlib §5 Ordering secondary key (this milestone): ThenBy / ThenByDescending
        // take a `(T) -> TKey` lambda over the same source element T. The receiver is
        // IOrderedEnumerable<T> (the lazy sentinel); genericArgs[0] = T.
        m == "ThenBy" || m == "ThenByDescending";
    if (!elemLambda) return nullptr;
    auto* recvTy = ctx_.exprType(*ma->target);
    if (!recvTy || recvTy->kind != TypeRef::Kind::Named ||
        recvTy->genericArgs.empty() || !recvTy->genericArgs[0])
        return nullptr;
    // A collection (Array/Set/… — Named with astNode, no declaration) or a lazy
    // IEnumerable sentinel (Named with neither). Either carries the element
    // type as genericArgs[0]. A user class/interface (has declaration) is not
    // a LINQ source and is excluded — EXCEPT a `Range<T>` (issue #775), which is
    // IEnumerable<T> but carries the builtin Range class decl; its element is
    // genericArgs[0]. Without this seeding, a Range LINQ lambda's untyped param
    // (`(1L..n).Select((x) => x * 2L)`) was left untyped → codegen compiled the
    // selector with an i32 param while the body produced i64 → an IR mismatch.
    if (recvTy->declaration && recvTy->declaration->name != "Range")
        return nullptr;
    return recvTy->genericArgs[0];
}

// stdlib §5 (#1124): `src.Aggregate(seed, (acc, x) => …)` — the fold lambda is
// `(TAccum, T) -> TAccum`, where TAccum is the seed argument's type and T is the
// source element type. Return {TAccum, T} so an un-annotated `(acc, x)` fold gets
// its parameters typed before its body is visited. (The single-param LINQ seeding
// `linqLambdaElementType` does not apply: the fold lambda has two parameters whose
// types come from different sources — the seed and the element.) Empty otherwise.
std::vector<TypeRef*> ExprTC::aggregateFoldLambdaParamTypes(const ast::CallExpr& ce) {
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(ce.callee.get());
    if (!ma || !ma->target || ma->member != "Aggregate" || ce.args.size() < 2)
        return {};
    if (!ce.args[0].value) return {};
    TypeRef* seedTy = ctx_.exprType(*ce.args[0].value);   // TAccum
    auto* recvTy = ctx_.exprType(*ma->target);
    if (!seedTy || !recvTy || recvTy->kind != TypeRef::Kind::Named ||
        recvTy->genericArgs.empty() || !recvTy->genericArgs[0])
        return {};
    // Same source gate as linqLambdaElementType: a collection / lazy-IEnumerable
    // sentinel carries the element as genericArgs[0]; a user class is excluded
    // (except Range, which is IEnumerable<T> over genericArgs[0]).
    if (recvTy->declaration && recvTy->declaration->name != "Range")
        return {};
    return { seedTy, recvTy->genericArgs[0] };            // {TAccum, T}
}

// per §24.3: enforce the event visibility fence. `op` is the producer-side
// operation being attempted on the event (`Raise`, `RaiseAsync`, `Add`,
// `Remove`, or `+=` / `-=`). These are in scope only when the event's static
// type is `EventSource<T>` / `AsyncEventSource<T>` — i.e. inside the declaring
// class, or inside a derived class for a `protected` event (§24.5). Outside,
// the member's type is `IEvent<T>` / `IAsyncEvent<T>`, which has no such
// member, so the access is a compile error.
void ExprTC::checkEventVisibilityFence(EventSymbol* es, std::string_view op,
                                       SourceLocation loc) {
    if (!es) return;
    TypeSymbol* declaring = es->parent;
    if (!declaring) return;  // defensive: an event always has a parent type
    // Inside the declaring class: the EventSource<T> face is in scope.
    if (currentType_ == declaring) return;
    // §24.5: a derived class may use the producer-side face for a `protected`
    // event declared on a base class. Any other event accessed from a subclass
    // (or from anywhere else) is, outside its declaring class, only IEvent<T>.
    if (es->visibility == ast::Visibility::Protected && currentType_ &&
        isSubtype(currentType_, declaring))
        return;
    bool isOperator = (op == "+=" || op == "-=");
    std::string detail =
        isOperator
            ? std::string("the `") + std::string(op) +
                  "` form (which desugars to " +
                  (op == "+=" ? "`Add`" : "`Remove`") + ")"
            : std::string("`.") + std::string(op) + "`";
    ctx_.diagnostics().report(
        Severity::Error, loc,
        std::string("event `") + std::string(es->name) +
        "` is accessed from outside its declaring class `" +
        std::string(declaring->name) + "`, where its static type is the "
        "subscribe-only interface `IEvent<T>` — " + detail +
        " is not available; only `.Subscribe` may be used here (§24.3)");
}

// §13.1 member-visibility enforcement (issue #432).
// Returns true and emits a diagnostic when the access is rejected;
// returns false when the access is allowed.
//
// Visibility rules (§13.1, §13.4):
//   public    — always allowed.
//   internal  — allowed when the member is NOT imported (i.e. is defined in the
//               same module as the access site — §13.4). Cross-module imports
//               are rejected because `internal` is module-private.
//   protected — allowed when currentType_ is the declaring type itself or is a
//               type that derives (transitively) from the declaring type (§13.1).
//   private   — allowed only when currentType_ == declaring type (§13.1, slice S8).
//
// Overload-suppression: for private and protected, if a non-private-or-accessible
// sibling with the same name exists on the declaring type (i.e. there is a
// public/internal overload of the same name), the error is suppressed — the
// codegen overload-resolution step will pick the accessible variant.
// §11.6 (#954): is `inner` a nested type of `outer` — directly, or transitively
// through a deeper nesting chain? A nested type is recorded as a
// NestedTypeSymbol in its enclosing type's member list (no back-pointer), so
// the relationship is found by walking the enclosing type's members.
static bool isNestedWithin(const TypeSymbol* inner, const TypeSymbol* outer) {
    if (!inner || !outer) return false;
    for (const Symbol* m : outer->members) {
        if (auto* nts = dynamic_cast<const NestedTypeSymbol*>(m)) {
            if (nts->type == inner) return true;
            if (nts->type && isNestedWithin(inner, nts->type)) return true;
        }
    }
    return false;
}

bool ExprTC::checkMemberVisibility(const MemberSymbol* msym,
                                   std::string_view memberName,
                                   SourceLocation loc) {
    if (!msym || !msym->parent) return false;
    TypeSymbol* declaring = msym->parent;
    const ast::Visibility vis = msym->visibility;

    // public — always allowed (§13.1).
    if (vis == ast::Visibility::Public || vis == ast::Visibility::None)
        return false;

    // §11.6 (#954): a nested type and its enclosing type have mutual access to
    // ALL of each other's members, including `private` and `protected` — "A
    // nested class has access to all members (including private) of its
    // enclosing class … The enclosing class similarly has full access to its
    // nested types' members." Allow any access when currentType_ is nested
    // within `declaring`, or `declaring` is nested within currentType_
    // (directly or transitively). Field/method access is a plain offset/call in
    // codegen (visibility is a sema-only concept), so this is purely lifting a
    // false rejection.
    if (currentType_ && currentType_ != declaring &&
        (isNestedWithin(currentType_, declaring) ||
         isNestedWithin(declaring, currentType_)))
        return false;

    // internal — allowed iff the member is in the same module (not imported).
    // §13.4: `internal` = code in the same module. An imported symbol is by
    // definition from a different module, so it is never same-module.
    if (vis == ast::Visibility::Internal) {
        if (!ctx_.isImported(msym)) return false;  // same module — OK
        // Cross-module: internal member is inaccessible.
        ctx_.diagnostics().report(
            Severity::Error, loc,
            std::string("`") + std::string(memberName) +
            "` is internal to module `" +
            std::string(declaring->name) +
            "` and not accessible from outside that module"
            " (§13.1 — `internal` is visible to the same module only)");
        return true;
    }

    // protected — allowed when currentType_ is the declaring type or derives
    // from it (§13.1). Access from within the hierarchy is always valid.
    if (vis == ast::Visibility::Protected) {
        // Same type or subtype — OK.
        if (currentType_ == declaring) return false;
        if (currentType_ && isSubtype(currentType_, declaring)) return false;
        // Outside the hierarchy: check for an accessible overload with the same
        // name before emitting (mirrors the private overload-suppression).
        bool hasAccessibleOverload = false;
        for (Symbol* sib : declaring->members) {
            if (!sib || sib == msym || sib->name != memberName) continue;
            if (auto* sm = dynamic_cast<MemberSymbol*>(sib)) {
                if (sm->visibility != ast::Visibility::Private &&
                    sm->visibility != ast::Visibility::Protected)
                    hasAccessibleOverload = true;
            }
        }
        if (hasAccessibleOverload) return false;
        ctx_.diagnostics().report(
            Severity::Error, loc,
            std::string("`") + std::string(memberName) +
            "` is protected on type `" +
            std::string(declaring->name) +
            "` and not accessible from `" +
            std::string(currentType_ ? currentType_->name
                                     : std::string_view("(global)")) +
            "` (§13.1 — `protected` is visible to the declaring class and "
            "derived classes only)");
        return true;
    }

    // private — allowed only from the declaring type (§13.1, original slice S8).
    if (vis == ast::Visibility::Private) {
        if (msym->parent == currentType_) return false;
        // Check for a non-private overload (the codegen will pick it).
        bool hasPublicOverload = false;
        for (Symbol* sib : declaring->members) {
            if (!sib || sib == msym || sib->name != memberName) continue;
            if (auto* sm = dynamic_cast<MemberSymbol*>(sib)) {
                if (sm->visibility != ast::Visibility::Private)
                    hasPublicOverload = true;
            }
        }
        if (hasPublicOverload) return false;
        ctx_.diagnostics().report(
            Severity::Error, loc,
            std::string("`") + std::string(memberName) +
            "` is private to type `" +
            std::string(declaring->name) +
            "` and not accessible from `" +
            std::string(currentType_ ? currentType_->name
                                     : std::string_view("(global)")) +
            "` (§13.1 — `private` is visible to the declaring "
            "class only)");
        return true;
    }

    return false;
}

// per §14.1/§14.2: substitute a member's declared type against the receiver's
// concrete generic arguments. A field/property declared `v: T` on a generic
// type `Holder<T>`, accessed off `b: Holder<Inner>`, has type `Inner` — not
// the bare type parameter `T`. Substituting here lets chained access like
// `b.v.x` resolve `.x` against the concrete record layout (issue #361 / D13).
TypeRef* ExprTC::substituteMemberType(TypeRef* declared,
                                      const ast::Expr* target,
                                      TypeSymbol* parent) {
    if (!declared || !parent) return declared;
    if (parent->genericParams.empty()) return declared;
    // Substitution applies both to a bare type-parameter reference (`v: T`)
    // and to a constructed type that *embeds* a parent type parameter
    // (`method BoxB(): Box<B>` on `Pair<A,B>`).  issue #597 (§14.1 / §22.4):
    // a method returning `Box<B>` / `Pair<B,A>` — a Named TypeRef with a
    // non-null declaration whose genericArgs reference the parent's params —
    // was returned unchanged by the old `declared->declaration` early-out, so
    // the class-level params inside the constructed type stayed unsubstituted.
    // Codegen then defaulted each unresolved param to the i32 placeholder,
    // which happens to equal the *first* type argument's lowering when it is
    // `int` (so `Box<A>` with A=int worked by luck) but is wrong for any
    // other param — `Box<B>` with B=string laid out as `Box<int>` (zero-init)
    // and `Pair<B,A>` laid out as `Pair<int,int>` (malformed IR on return).
    if (declared->kind != TypeRef::Kind::Named) return declared;
    // Find the receiver's resolved TypeRef (carries the concrete generic args).
    // Prefer the target's exprType; fall back to a local/parameter's declared
    // type when the target itself was not given an exprType.
    // #989: target may be null when this is called for a bare-identifier field
    // reference (implicit `this.field`); in that case skip the target-dependent
    // paths and go straight to the derived/currentType_ fallback below.
    TypeRef* recvTy = target ? ctx_.exprType(*target) : nullptr;
    if (target && (!recvTy || recvTy->declaration != parent ||
        recvTy->genericArgs.empty())) {
        Symbol* sym = ctx_.exprSymbol(*target);
        if (auto* loc = dynamic_cast<LocalSymbol*>(sym)) {
            if (loc->declType && loc->declType->declaration == parent &&
                !loc->declType->genericArgs.empty())
                recvTy = loc->declType;
        } else if (auto* par = dynamic_cast<ParameterSymbol*>(sym)) {
            if (par->declType) {
                if (auto* prt = ctx_.resolvedType(*par->declType))
                    if (prt->declaration == parent && !prt->genericArgs.empty())
                        recvTy = prt;
            }
        }
    }
    // #722: an INHERITED member — the receiver is a DERIVED type that fixes the
    // generic base's type args (e.g. `IntHolder: Base<int>` calling Base's
    // `GetVal(): T`). recvTy->declaration is the derived type (not `parent`), so
    // the direct path above did not match. Walk the derived type's base chain to
    // find `parent` and substitute recvTy with that base instantiation's TypeRef
    // (carrying the bound args, e.g. Base<int>) so the classSubst machinery below
    // resolves `T` to the concrete arg. Mirrors the codegen base-substitution in
    // LlvmBackend_Layout.cpp:738-765. A single inheritance level (the reported
    // case) is handled; deeper args-through-args chains can extend this later.
    // #747 R42: a `this`/`base` keyword receiver carries no exprType, so recvTy
    // is null and the local/parameter fallbacks above did not match. An INHERITED
    // generic-base method called via `this`/`base` (e.g. `this.GetVal()` /
    // `base.GetVal()` on `Derived: Base<int>` where GetVal(): T comes from Base)
    // must still substitute the base's type params. Seed the base-chain walk from
    // the ENCLOSING type (currentType_) in that case; otherwise from the derived
    // receiver's declaration (the #722 path).
    // #989: a null `target` means an implicit `this.field` bare-identifier access
    // inside a method body — use currentType_ exactly as the `this`/`base`
    // keyword paths above do.
    TypeSymbol* derived = (recvTy && recvTy->declaration) ? recvTy->declaration
                                                          : nullptr;
    if (!derived) {
        if (!target) {
            // bare-identifier field — treat as implicit `this`
            if (currentType_) derived = currentType_;
        } else if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(target)) {
            if ((id->name == "this" || id->name == "base") && currentType_)
                derived = currentType_;
        }
    }
    // Build the parent-param-name -> concrete-arg map.
    std::unordered_map<std::string_view, TypeRef*> classSubst;
    if (recvTy && recvTy->declaration == parent && !recvTy->genericArgs.empty()) {
        // Direct: the receiver IS the parent instantiation (`b: Box<int>`).
        // Positional by index (param `B` at genericParams[1] -> genericArgs[1]).
        for (std::size_t gi = 0; gi < parent->genericParams.size() &&
                                 gi < recvTy->genericArgs.size(); ++gi)
            if (parent->genericParams[gi] && recvTy->genericArgs[gi])
                classSubst[parent->genericParams[gi]->name] =
                    recvTy->genericArgs[gi];
    } else if (derived && derived != parent) {
        // Inherited: the receiver is a DERIVED type that fixes `parent`'s
        // generic args somewhere up the base chain. Walk the chain transitively
        // from `derived` up to `parent`, COMPOSING the type-arg bindings at each
        // level: this handles both a single level (`IntHolder: Base<int>`, #722)
        // and a multi-level chain (`Child: Mid: Box<int>`) plus intermediate
        // generic links (`Child: Mid<int>: Box<U>`). #1101-A: the prior one-level
        // walk only matched an immediate base, so the binding was dropped through
        // 2+ levels and the inherited generic-T member typed as `?` (e.g. a
        // binary-operator operand rejected as "operator + not defined for `?`").
        // Mirrors the codegen base-substitution (computeAncestorGenericSubst).
        const TypeSymbol* cur = derived;
        // curArgs: cur's generic-param-name -> concrete TypeRef at this level.
        // Seeded from the receiver's own bound args when the receiver itself is
        // a generic instantiation (`Child<int>`); usually `derived` is the
        // enclosing class with bare params, so curArgs starts empty.
        std::unordered_map<std::string_view, TypeRef*> curArgs;
        if (recvTy && recvTy->declaration == derived &&
            !recvTy->genericArgs.empty() &&
            derived->genericParams.size() == recvTy->genericArgs.size())
            for (std::size_t gi = 0; gi < derived->genericParams.size(); ++gi)
                if (derived->genericParams[gi])
                    curArgs[derived->genericParams[gi]->name] =
                        recvTy->genericArgs[gi];
        while (cur && cur != parent) {
            const TypeSymbol* base = nullptr;
            TypeRef* baseRef = nullptr;
            for (std::size_t bi = 0; bi < cur->baseTypes.size(); ++bi) {
                if (cur->baseTypes[bi] &&
                    cur->baseTypes[bi]->kind == ast::TypeDeclKind::Class) {
                    base = cur->baseTypes[bi];
                    baseRef = (bi < cur->baseTypeRefs.size())
                                  ? cur->baseTypeRefs[bi] : nullptr;
                    break;
                }
            }
            if (!base) break;  // `parent` not on the class chain
            std::unordered_map<std::string_view, TypeRef*> nextArgs;
            if (!base->genericParams.empty()) {
                if (!baseRef ||
                    baseRef->genericArgs.size() != base->genericParams.size())
                    return declared;  // unresolvable generic link
                for (std::size_t gi = 0; gi < base->genericParams.size(); ++gi) {
                    TypeRef* arg = baseRef->genericArgs[gi];
                    // Resolve a bare generic-param ref (`T`, no declaration)
                    // through this level's bindings; a concrete arg passes
                    // through unchanged.
                    if (arg && arg->kind == TypeRef::Kind::Named &&
                        !arg->declaration && arg->astNode && arg->astNode->name &&
                        arg->astNode->name->parts.size() == 1) {
                        auto it = curArgs.find(arg->astNode->name->parts[0]);
                        if (it != curArgs.end()) arg = it->second;
                    }
                    if (base->genericParams[gi])
                        nextArgs[base->genericParams[gi]->name] = arg;
                }
            }
            cur = base;
            curArgs = std::move(nextArgs);
        }
        if (cur != parent) return declared;  // ancestor not reached
        for (const Symbol* gp : parent->genericParams) {
            auto it = curArgs.find(gp->name);
            if (it != curArgs.end() && it->second)
                classSubst[gp->name] = it->second;
        }
    }
    if (classSubst.empty()) return declared;
    // substituteReturnType handles both the bare-param case (with the `T?`
    // nullable-companion preservation) and recursion into the genericArgs of a
    // constructed type — exactly what `Box<B>` / `Pair<B,A>` need.
    return substituteReturnType(declared, classSubst);
}

// issue #391 helper: collect the subset of `paramNames` that the declared
// return TypeRef syntactically references (a bare declaration-less Named whose
// single name part is the parameter name), recursing into generic arguments.
static void collectReturnParamNames(
    const TypeRef* t,
    const std::unordered_set<std::string_view>& paramNames,
    std::unordered_set<std::string_view>& out) {
    if (!t) return;
    if (t->kind == TypeRef::Kind::Named && !t->declaration && t->astNode &&
        t->astNode->name && t->astNode->name->parts.size() == 1 &&
        paramNames.count(t->astNode->name->parts[0]))
        out.insert(t->astNode->name->parts[0]);
    for (auto* ga : t->genericArgs)
        collectReturnParamNames(ga, paramNames, out);
}

// per §14.1/§14.2 + §A.7 (issue #391) — see the header comment on this
// helper. Recursively unify a declared return TypeRef against the concrete
// target, binding type-parameter names into `solved`.
bool ExprTC::unifyReturnAgainstTarget(
    const TypeRef* declared, const TypeRef* target,
    const std::unordered_set<std::string_view>& paramNames,
    std::unordered_map<std::string_view, TypeRef*>& solved) {
    if (!declared || !target) return false;
    // A bare type-parameter reference resolves to a Named TypeRef with no
    // declaration whose single name part is the parameter name (the parameter
    // is an in-scope name with a null symbol; TypeResolution leaves it
    // declaration-less). Binding it solves the parameter.
    if (declared->kind == TypeRef::Kind::Named && !declared->declaration &&
        declared->astNode && declared->astNode->name &&
        declared->astNode->name->parts.size() == 1) {
        std::string_view nm = declared->astNode->name->parts[0];
        if (paramNames.count(nm)) {
            // `: T?` against a target T binds T to the target's non-nullable
            // form, mirroring how `T?` is the nullable companion of T (§14.2).
            TypeRef* bound = const_cast<TypeRef*>(target);
            if (declared->nullable && target->nullable) {
                TypeRef* nn = ctx_.makeTypeRef(*target);
                nn->nullable = false;
                bound = nn;
            }
            auto it = solved.find(nm);
            if (it != solved.end()) {
                // Already bound elsewhere — only consistent if it agrees.
                return it->second && bound &&
                       it->second->declaration == bound->declaration &&
                       it->second->primitive == bound->primitive &&
                       it->second->kind == bound->kind;
            }
            solved[nm] = bound;
            return true;
        }
        // A non-parameter declaration-less Named (e.g. a stdlib built-in) that
        // carries no parameter: treat as opaque, succeed without solving.
        return true;
    }
    // A constructed generic return type (`Box<T>`) unifies structurally
    // against a same-shaped target (`Box<Widget>`): same declaration and arg
    // count, recurse pairwise so the nested T is solved from the target's arg.
    if (declared->kind == TypeRef::Kind::Named && declared->declaration &&
        target->kind == TypeRef::Kind::Named &&
        declared->declaration == target->declaration &&
        declared->genericArgs.size() == target->genericArgs.size()) {
        for (std::size_t i = 0; i < declared->genericArgs.size(); ++i) {
            if (!unifyReturnAgainstTarget(declared->genericArgs[i],
                                          target->genericArgs[i], paramNames,
                                          solved))
                return false;
        }
        return true;
    }
    // No type parameter is involved (concrete vs concrete) — nothing to solve;
    // succeed so a return type that is partly concrete does not block the
    // parameter-bearing parts from solving above.
    return true;
}

// per §14.1 (issue #391) — see the header comment. Rebuild a declared return
// TypeRef with each solved type-parameter name replaced by its concrete
// TypeRef.
TypeRef* ExprTC::substituteReturnType(
    TypeRef* declared,
    const std::unordered_map<std::string_view, TypeRef*>& solved) {
    if (!declared) return declared;
    // Bare parameter → its solved concrete type (carrying a `T?` annotation
    // through to the substituted type's nullable form). A bare parameter is a
    // declaration-less Named TypeRef with a single name part that IS a method
    // type parameter; distinguish it from a null-declaration stdlib built-in
    // (e.g. `Array`) by checking whether the name is in `solved`.
    if (declared->kind == TypeRef::Kind::Named && !declared->declaration &&
        declared->astNode && declared->astNode->name &&
        declared->astNode->name->parts.size() == 1 &&
        declared->genericArgs.empty()) {
        // Only a bare param reference has no genericArgs and no declaration.
        // Check if this name is a solved type parameter.
        auto it = solved.find(declared->astNode->name->parts[0]);
        if (it != solved.end() && it->second) {
            TypeRef* sub = it->second;
            if (declared->nullable && !sub->nullable) {
                TypeRef* nn = ctx_.makeTypeRef(*sub);
                nn->nullable = true;
                return nn;
            }
            return sub;
        }
        return declared;
    }
    // #428 phase B: an imported generic-record field's type parameter has NO
    // astNode (there is no AST for imported types); its name rides importedName
    // instead (set by ModuleLoader::convert for MetaType::Named when
    // declaration==nullptr).  The local-AST branch above misses this case.
    // Look up importedName directly in `solved` — same substitution semantics.
    // per §14.1 / §9 (imported positional-record field type substitution).
    if (declared->kind == TypeRef::Kind::Named && !declared->declaration &&
        !declared->astNode && !declared->importedName.empty() &&
        declared->genericArgs.empty()) {
        auto it = solved.find(declared->importedName);
        if (it != solved.end() && it->second) {
            TypeRef* sub = it->second;
            if (declared->nullable && !sub->nullable) {
                TypeRef* nn = ctx_.makeTypeRef(*sub);
                nn->nullable = true;
                return nn;
            }
            return sub;
        }
        return declared;
    }
    // Constructed generic → substitute each generic argument in place.
    // This covers both user-defined generic types (declaration != nullptr, e.g.
    // `Box<T>`) and null-declaration stdlib built-ins (e.g. `Array<T>`,
    // `OwnedArray<T>`) — both carry the element type parameter in genericArgs.
    // issue #564 (§14.1 / §22.1): the `declaration &&` guard was too strict;
    // null-declaration stdlib collections were left unsubstituted, so
    // `MakeTwo<int>(): owned Array<T>` typed the call as `Array<T>` instead of
    // `Array<int>` and `arr[0]` resolved to unresolvable `?`.
    if (declared->kind == TypeRef::Kind::Named &&
        !declared->genericArgs.empty()) {
        bool changed = false;
        std::vector<TypeRef*> newArgs;
        newArgs.reserve(declared->genericArgs.size());
        for (auto* ga : declared->genericArgs) {
            TypeRef* sub = substituteReturnType(ga, solved);
            if (sub != ga) changed = true;
            newArgs.push_back(sub);
        }
        if (!changed) return declared;
        TypeRef* result = ctx_.makeTypeRef(*declared);
        result->genericArgs = std::move(newArgs);
        return result;
    }
    return declared;
}

// §14.1 (#785): collect the method type-parameter names that a value
// parameter's declared type BINDS — recursively, so a generic-typed parameter
// like `p: Pair<A, B>` discovers both A and B (a bare `p: T` discovers T), not
// only a top-level bare parameter name. Mirrors codegen's structural
// argument-inference (unifyMethodReturnTypeArg). // per §14.1
static void collectBoundParamNames(
    const ast::Type* pt,
    const std::unordered_set<std::string_view>& paramNames,
    std::unordered_set<std::string_view>& boundByArg) {
    if (!pt) return;
    if (pt->kind == ast::TypeKind::Named && pt->name &&
        pt->name->parts.size() == 1 && paramNames.count(pt->name->parts[0]))
        boundByArg.insert(pt->name->parts[0]);
    for (const auto& ga : pt->genericArgs)
        collectBoundParamNames(ga.get(), paramNames, boundByArg);
    collectBoundParamNames(pt->arrayElement.get(), paramNames, boundByArg);
    collectBoundParamNames(pt->funcReturn.get(), paramNames, boundByArg);
    for (const auto& fp : pt->funcParams)
        collectBoundParamNames(fp.type.get(), paramNames, boundByArg);
}

// per §14.1/§14.2 + §A.7 (issue #391) — see the header comment. Solve a
// generic method's return-only type parameters from the call's target type.
bool ExprTC::inferReturnTargetTypeArgs(const ast::CallExpr& ce, TypeRef* target,
                                       bool* outNeedsInfer) {
    if (outNeedsInfer) *outNeedsInfer = false;
    if (!ce.callee) return false;
    // The callee must resolve to a generic method (its own `<T>` params).
    auto* ms = dynamic_cast<MethodSymbol*>(ctx_.exprSymbol(*ce.callee));
    if (!ms) return false;
    auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
    if (!md || md->genericParams.empty() || !md->returnType) return false;

    // fix #747 R43 / §14.1: EXPLICIT call-site type arguments (`Swap<int,int>(p)`)
    // fully determine the method's type parameters — there is nothing to infer,
    // so no target type is required. Without this short-circuit a call whose
    // type parameter appears only in the return type (or is bound through a
    // generic-typed parameter the argument-inference loop doesn't solve) was
    // wrongly rejected as "cannot infer ... target type required" even though
    // the type arguments were spelled out, so `var q := C.Swap<int,int>(p);`
    // failed while `var q: Pair<int,int> := C.Swap<int,int>(p);` compiled.
    const std::vector<std::unique_ptr<ast::Type>>* callTypeArgs = nullptr;
    if (auto* callMa =
            dynamic_cast<const ast::MemberAccessExpr*>(ce.callee.get()))
        callTypeArgs = &callMa->typeArgs;
    else if (auto* callId =
                 dynamic_cast<const ast::IdentifierExpr*>(ce.callee.get()))
        callTypeArgs = &callId->typeArgs;
    if (callTypeArgs && callTypeArgs->size() >= md->genericParams.size())
        return false;

    // Determine which type parameters are already bound by an argument: a
    // value parameter whose declared type is exactly the bare parameter name
    // (mirrors codegen's argument-inference loop and §14.1). These never need
    // a target — the argument solves them.
    std::unordered_set<std::string_view> paramNames;
    for (const auto& gp : md->genericParams) paramNames.insert(gp.name);
    std::unordered_set<std::string_view> boundByArg;
    for (const auto& p : md->params)
        collectBoundParamNames(p.type.get(), paramNames, boundByArg);

    // The unbound parameters are those a target must solve. If every parameter
    // is bound by an argument, this is the ordinary argument-inferred case and
    // the return-target path does not apply.
    std::unordered_set<std::string_view> unbound;
    for (const auto& gp : md->genericParams)
        if (!boundByArg.count(gp.name)) unbound.insert(gp.name);
    if (unbound.empty()) return false;

    // The declared return type must actually mention an unbound parameter for
    // a target to be able to solve it; otherwise inference is neither possible
    // nor needed (e.g. an unbound parameter used only in a `where`-clause type
    // bound is out of scope here — deferred).
    TypeRef* retTy = ctx_.resolvedType(*md->returnType);
    if (!retTy) return false;
    std::unordered_set<std::string_view> mentioned;
    collectReturnParamNames(retTy, unbound, mentioned);
    if (mentioned.empty()) return false;

    // At this point the call genuinely needs a target to resolve. Report that
    // to the caller so the no-target context can diagnose.
    if (outNeedsInfer) *outNeedsInfer = true;
    if (!target) return false;

    // Solve the unbound parameters by unifying the declared return type
    // against the target type.
    std::unordered_map<std::string_view, TypeRef*> solved;
    if (!unifyReturnAgainstTarget(retTy, target, unbound, solved))
        return false;
    // Require every unbound parameter the return type references to be solved.
    for (auto nm : mentioned)
        if (!solved.count(nm)) return false;

    // Re-type the call to the substituted return type (§14.1). Codegen
    // monomorphizes generic-method calls by re-deriving the substitution at
    // the call site; see the issue note about the codegen-side consumption of
    // a return-target-inferred argument.
    TypeRef* substituted = substituteReturnType(retTy, solved);
    if (substituted) ctx_.setExprType(&ce, substituted);
    return true;
}

// per §14.1 + §A.7 (issue #391) — see the header comment. Diagnose a
// return-only generic call in a no-target context.
void ExprTC::reportNoTargetInferenceIfNeeded(const ast::CallExpr& ce) {
    bool needsInfer = false;
    // Pass a null target: the routine then only reports whether the call needs
    // a target to resolve, without solving or re-typing anything.
    inferReturnTargetTypeArgs(ce, /*target=*/nullptr, &needsInfer);
    if (!needsInfer) return;
    std::string methodName;
    if (auto* ms = dynamic_cast<MethodSymbol*>(ctx_.exprSymbol(*ce.callee)))
        methodName = std::string(ms->name);
    std::string what = methodName.empty()
                           ? std::string("this generic method's call")
                           : ("`" + methodName + "`'s call");
    ctx_.diagnostics().report(
        Severity::Error, ce.span.begin,
        "cannot infer the type argument of " + what +
            " from its arguments; it appears only in the return type, so a "
            "target type is required — assign the result to a typed local "
            "(`var x: T := ...`) or return it from a method with a declared "
            "return type (§14.1, §A.7)");
}

void ExprTC::resolveExtensionMember(const ast::MemberAccessExpr& ma,
                                    Symbol* member) {
    if (!member) return;
    ctx_.setExprSymbol(&ma, member);
    if (auto* ms = dynamic_cast<MethodSymbol*>(member)) {
        // §27.6: a bare `x.M` in value position is a method reference of type
        // `(paramTypes) -> returnType`. The CallExpr handler (typeCall) reads
        // the MethodSymbol's decl directly for `x.M(...)`, so this Function
        // type only affects the bare-reference case. No receiver substitution:
        // an extension member is not generic over the receiver in this slice.
        auto* ft = ctx_.makeTypeRef();
        ft->kind = TypeRef::Kind::Function;
        auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
        for (auto* psym : ms->params) {
            auto* par = dynamic_cast<ParameterSymbol*>(psym);
            if (!par) continue;
            TypeRef::FuncParam fp;
            fp.modifier = par->modifier;
            fp.name = par->name;
            if (par->declType) fp.type = ctx_.resolvedType(*par->declType);
            ft->funcParams.push_back(fp);
        }
        if (md && md->returnType) {
            ft->funcReturn = ctx_.resolvedType(*md->returnType);
            ft->funcReturnOwned = md->returnOwned;
        } else {
            auto* unit = ctx_.makeTypeRef();
            unit->kind = TypeRef::Kind::Unit;
            ft->funcReturn = unit;
        }
        ctx_.setExprType(&ma, ft);
    } else if (auto* ps = dynamic_cast<PropertySymbol*>(member)) {
        // §26.1: a computed read-only extension property — its type is the
        // declared `: T`. (Property codegen is DEFERRED, #433; this types the
        // access so a `var x := recv.P` use site is well-formed for diagnosis.)
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        if (pd && pd->type) {
            if (auto* rt = ctx_.resolvedType(*pd->type))
                ctx_.setExprType(&ma, rt);
        }
    }
}

void ExprTC::resolveMemberAccess(const ast::MemberAccessExpr& ma) {
    // per §24.3: the event visibility fence. When the access target resolves
    // to an event member, the producer-side operations (`Raise`, `RaiseAsync`,
    // `Add`, `Remove`) are only in scope inside the declaring class (and inside
    // a derived class for a `protected` event); from outside, the event's
    // static type is `IEvent<T>`, which has only `Subscribe`. The event member
    // itself has no user TypeSymbol (it is `EventSource<T>` / `IEvent<T>`), so
    // this runs before the ordinary member-lookup path below, which never
    // resolves these names.
    if (ma.target) {
        if (auto* es = dynamic_cast<EventSymbol*>(ctx_.exprSymbol(*ma.target))) {
            if (ma.member == "Raise" || ma.member == "RaiseAsync" ||
                ma.member == "Add" || ma.member == "Remove")
                checkEventVisibilityFence(es, ma.member, ma.span.begin);
        }
    }
    // §7.4 (issues #418, #862): member access (and method calls) on a NULLABLE
    // value is a compile error unless the compiler has narrowed it to non-null.
    // The compiler narrows only local variables — never fields or properties
    // (§7.4 "Narrowing applies to locals only"). Two sub-cases are enforced:
    //
    // (a) SIMPLE VARIABLE (bare local or parameter IdentifierExpr): gated via
    //     `nullNarrowed_` / `narrowings_`. Safe forms are already non-nullable
    //     at this point: `x?.M` (§7.5) is a NullCondMemberExpr node;
    //     `x!.M` (§7.8) has a PostfixBangExpr target whose exprType is
    //     non-nullable.
    //
    // (b) FIELD, PROPERTY, or any other non-narrowable expression: the target's
    //     resolved exprType (set by a prior MemberAccess visit) is nullable.
    //     Fields are NEVER narrowable (TOCTOU hazard); the standard fix is to
    //     copy the field to a local first — `var copy := field; if copy <> null`.
    //     Bare-identifier field references (`_name.Length` where `_name` is an
    //     implicit-this field, exprType not set for the IdentifierExpr itself)
    //     are handled via the FieldSymbol / PropertySymbol declared type.
    if (ma.target) {
        if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                ma.target.get())) {
            if (tid->name != "this" && tid->name != "base") {
                Symbol* tsym = lookupName(tid->name);
                bool isSimpleVar =
                    tsym && (dynamic_cast<LocalSymbol*>(tsym) ||
                             dynamic_cast<ParameterSymbol*>(tsym));
                // Only gate when the variable's declared type is KNOWN to be
                // nullable. An unresolved/untyped variable (e.g. inferred from
                // an intrinsic call sema doesn't yet type) is left alone — the
                // gate must not over-reject on missing type information.
                TypeRef* dt = isSimpleVar ? declaredTypeOf(tsym) : nullptr;
                bool declaredNullable = dt && dt->nullable;
                if (isSimpleVar && declaredNullable && !isKnownNonNull(tsym)) {
                    ctx_.diagnostics().report(
                        Severity::Error, ma.span.begin,
                        std::string("member access `.") +
                        std::string(ma.member) +
                        "` on a nullable value requires narrowing it to "
                        "non-null first (§7.4): guard with `if " +
                        std::string(tid->name) +
                        " <> null`, force-unwrap with `" +
                        std::string(tid->name) +
                        "!`, or use the null-conditional `" +
                        std::string(tid->name) + "?." +
                        std::string(ma.member) + "`");
                    return;
                }
                // per §7.4 (#862): a bare identifier resolving to a FIELD or
                // PROPERTY is never narrowable — fields can be reassigned by
                // another thread between the null check and the use (TOCTOU).
                // Copy the field to a local first and narrow the local.
                if (!isSimpleVar && tsym) {
                    TypeRef* fieldType = nullptr;
                    if (auto* fs = dynamic_cast<FieldSymbol*>(tsym)) {
                        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
                        if (fd && fd->type)
                            fieldType = ctx_.resolvedType(*fd->type);
                        else if (fs->posParam && fs->posParam->type)
                            fieldType = ctx_.resolvedType(*fs->posParam->type);
                    } else if (auto* ps = dynamic_cast<PropertySymbol*>(tsym)) {
                        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
                        if (pd && pd->type)
                            fieldType = ctx_.resolvedType(*pd->type);
                    }
                    if (fieldType && fieldType->nullable) {
                        ctx_.diagnostics().report(
                            Severity::Error, ma.span.begin,
                            std::string("member access `.") +
                            std::string(ma.member) +
                            "` on nullable field `" +
                            std::string(tid->name) +
                            "` requires a local copy first (§7.4): "
                            "fields are not narrowable — copy to a local "
                            "with `var tmp := " +
                            std::string(tid->name) +
                            "` and guard the local");
                        return;
                    }
                }
            }
        } else if (dynamic_cast<const ast::MemberAccessExpr*>(ma.target.get())) {
            // per §7.4 (#862): the target is itself a member access (a field or
            // property read, e.g. `obj.Field.Member`). Fields and properties are
            // never narrowable — copy to a local first and narrow the local.
            // `x!.M` (PostfixBangExpr) and `x?.M` (NullCondMemberExpr) are
            // already non-nullable at this point, so they don't reach this branch.
            TypeRef* tgt = ctx_.exprType(*ma.target);
            if (tgt && tgt->nullable) {
                ctx_.diagnostics().report(
                    Severity::Error, ma.span.begin,
                    std::string("member access `.") +
                    std::string(ma.member) +
                    "` on a nullable field or property requires narrowing to "
                    "non-null first (§7.4): copy the nullable value to a "
                    "local variable and guard with `<> null`, "
                    "force-unwrap with `!`, or use `?.`");
                return;
            }
        }
    }
    // §15.1.2 (#1100): an inline array `[N]T` exposes ONLY `.Length` and the
    // indexer `arr[i]` (with iteration via `for var x in arr`). It is a value
    // type that does NOT implement `IEnumerable<T>` and CANNOT be extended
    // (§26.2), so LINQ / IEnumerable operators (`.Count()`, `.Sum()`,
    // `.Where()`, `.ToArray()`, …) — and any other arbitrary member — have no
    // member to resolve on it. An InlineArray receiver yields no `targetType`
    // below, so the `knownCompleteUserType` "no member" diagnostic never fired;
    // the call then typed to nothing and codegen defaulted the result (Count→0,
    // Sum→garbage, a bool result in a branch → an IR-verification crash). Reject
    // any member on an InlineArray receiver other than `.Length` and the
    // universal Object members (§8.1, synthesized on every type), directing the
    // caller to materialize a heap `Array<T>`.
    if (ma.target) {
        if (const TypeRef* recvTy = ctx_.exprType(*ma.target)) {
            if (recvTy->kind == TypeRef::Kind::InlineArray) {
                const auto& mm = ma.member;
                bool allowed =
                    mm == "Length" ||
                    mm == "ToString" || mm == "Equals" ||
                    mm == "GetHashCode" || mm == "GetType";
                if (!allowed) {
                    ctx_.diagnostics().report(
                        Severity::Error, ma.span.begin,
                        std::string("no member `") + std::string(mm) +
                        "` on inline array type `[N]T`: inline arrays expose "
                        "only `.Length` and the indexer `[i]`, and do not "
                        "implement `IEnumerable<T>` (\xC2\xA7""15.1.2) nor admit "
                        "extension methods (\xC2\xA7""26.2) — LINQ/IEnumerable "
                        "operators are unavailable; materialize a heap "
                        "`Array<T>` (iterate the slots into a fresh array) to "
                        "use them");
                    return;
                }
            }
        }
    }
    TypeSymbol* targetType = nullptr;
    // `this.X` — currentType_ takes precedence over name lookup so
    // we don't have to teach `this` to be an expression with type
    // currentType_.
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
            ma.target.get())) {
        if (id->name == "this") {
            targetType = currentType_;
        } else if (id->name == "base" && currentType_) {
            // §12.3: `base.X` resolves X against the base class, so an
            // override in the current type is bypassed and the base
            // member is found.
            for (TypeSymbol* b : currentType_->baseTypes) {
                if (b && b->kind == ast::TypeDeclKind::Class) {
                    targetType = b;
                    break;
                }
            }
            // §12.3 (#1106 defect 6): when the class has no explicit class base,
            // `base` is `Object`, whose only members are the universal §8.1
            // ones (ToString/Equals/GetHashCode/GetType/ReferenceEquals). A
            // `base.M` naming any OTHER member is not a base-class member — it
            // silently resolved to nothing and mis-lowered to 0. Reject. (A
            // class WITH a class base whose chain lacks M is already diagnosed
            // "no member M on type B" by the known-complete-user-type path.)
            if (!targetType) {
                const auto& bm = ma.member;
                bool universalObjMember =
                    bm == "ToString" || bm == "Equals" ||
                    bm == "GetHashCode" || bm == "GetType" ||
                    bm == "ReferenceEquals";
                if (!universalObjMember) {
                    ctx_.diagnostics().report(
                        Severity::Error, ma.span.begin,
                        "`base." + std::string(bm) +
                        "` does not name a base-class member: `" +
                        std::string(currentType_->name) +
                        "` has no class base (its base is `Object`), and "
                        "`Object` has no `" + std::string(bm) +
                        "` — `base.M` calls a base class's implementation "
                        "(\xC2\xA7""12.3); use `this." + std::string(bm) +
                        "` or just `" + std::string(bm) + "`");
                    return;
                }
                // §8.5 / §12.3 (#1110): here `base` is `Object` (no explicit
                // class base), so the universal `Object.ToString()` member has
                // no resolvable `targetType` — the `if (!targetType)`
                // fallthrough below `return`s WITHOUT typing the access, so the
                // call is left untyped. The user-class path types `ToString` as
                // `string` (the `if (mm == "ToString")` after the !member
                // branch), but a null-`targetType` Object base never reaches it.
                // Result: `"x" + base.ToString()` saw a non-`string` right
                // operand and was rejected (§4.6) even though `base.ToString()`
                // yields a string. Type it `string` here so the call is `string`
                // in every position (`+` operand, var-init, return). per §8.5.
                else if (bm == "ToString") {
                    ctx_.setExprType(
                        &ma, ctx_.primitiveTypeRef(PrimitiveType::String));
                    return;
                }
            }
        }
    }
    // §3.4: `int.MaxValue`, `double.NaN`, `char.MaxValue`, etc. — static
    // properties on primitive keyword aliases. The target is an identifier
    // that names a primitive type; there's no TypeSymbol in scope for it.
    if (!targetType && ma.target) {
        if (auto* id2 = dynamic_cast<const ast::IdentifierExpr*>(ma.target.get())) {
            PrimitiveType pt = primitiveFromName(id2->name);
            if (pt != PrimitiveType::None) {
                const auto& mbr = ma.member;
                // Numeric properties: MaxValue/MinValue → same type.
                if (mbr == "MaxValue" || mbr == "MinValue" ||
                    mbr == "Zero" || mbr == "One") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(pt));
                    return;
                }
                // double/float properties.
                if (pt == PrimitiveType::Double || pt == PrimitiveType::Float) {
                    if (mbr == "NaN" || mbr == "PositiveInfinity" ||
                        mbr == "NegativeInfinity" || mbr == "Epsilon") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(pt));
                        return;
                    }
                    // stdlib §9.2: all five FP predicates return bool.
                    if (mbr == "IsNaN" || mbr == "IsInfinity" ||
                        mbr == "IsPositiveInfinity" ||
                        mbr == "IsNegativeInfinity" || mbr == "IsFinite") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                        return;
                    }
                    // stdlib §9.2 (#519): IeeeEquals / BitwiseEquals — two-arg
                    // static equality variants; both return bool.
                    if (mbr == "IeeeEquals" || mbr == "BitwiseEquals") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                        return;
                    }
                }
                // int.Parse(s) → int.
                if (mbr == "Parse") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(pt));
                    return;
                }
                // char static classification methods → bool.
                // #1036: `IsSymbol` is NOT one of them — stdlib §9.5 lists only
                // IsDigit/IsLetter/IsLetterOrDigit/IsWhiteSpace/IsUpper/IsLower/
                // IsControl/IsPunctuation. It was accepted here (typed bool) but
                // has no codegen lowering in the char-classification map
                // (LlvmBackend_Calls.cpp:6950-6957), so `char.IsSymbol(c)`
                // silently returned a constant false for every input ('+', '$',
                // '=' all → false) — an invented-API accept-trap (CLAUDE.md §2).
                // Dropped from the set so it falls through to the §3.4 "char has
                // no static method 'IsSymbol'" reject, like any other unknown.
                if (pt == PrimitiveType::Char) {
                    if (mbr == "IsDigit" || mbr == "IsLetter" ||
                        mbr == "IsLetterOrDigit" || mbr == "IsWhiteSpace" ||
                        mbr == "IsUpper" || mbr == "IsLower" ||
                        mbr == "IsControl" ||
                        mbr == "IsPunctuation") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                        return;
                    }
                    if (mbr == "ToUpper" || mbr == "ToLower") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Char));
                        return;
                    }
                    // char.ToString(c) → string.
                    if (mbr == "ToString") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::String));
                        return;
                    }
                }
                // string static methods → bool or string or int.
                if (pt == PrimitiveType::String) {
                    if (mbr == "IsNullOrEmpty" || mbr == "IsNullOrWhiteSpace") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                        return;
                    }
                    // #1034: `string.Format` and `string.Concat` are NOT provided
                    // by the stdlib (§9.6 lists Join/From/Empty/Repeat; composite
                    // formatting is done with string interpolation `$"{...}"`).
                    // They were typed `string` here but have no codegen lowering,
                    // so `string.Format(...)` compiled and CRASHED (SIGSEGV) and
                    // `string.Concat(...)` returned a garbage empty string. Drop
                    // them from the accept set so they fall through to the clean
                    // "type 'string' has no static method 'Format'" §3.4 reject,
                    // exactly as any other unknown static (`string.Bogus()`).
                    if (mbr == "Join" || mbr == "Empty" ||
                        mbr == "From" || mbr == "Repeat") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::String));
                        return;
                    }
                    // string.Compare → int (ordinal comparison result).
                    if (mbr == "Compare") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                        return;
                    }
                }
                // §9.4: bool/Boolean static methods — TryParse → bool, Parse → bool.
                if (pt == PrimitiveType::Bool) {
                    if (mbr == "TryParse") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                        return;
                    }
                    if (mbr == "Parse") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                        return;
                    }
                }
                // int static methods → int or bool.
                if (pt == PrimitiveType::Int || pt == PrimitiveType::Long ||
                    pt == PrimitiveType::Byte || pt == PrimitiveType::SByte ||
                    pt == PrimitiveType::Short || pt == PrimitiveType::UShort ||
                    pt == PrimitiveType::UInt || pt == PrimitiveType::ULong) {
                    if (mbr == "Parse") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(pt));
                        return;
                    }
                    // int.TryParse(s, out result): bool — §3.4/stdlib §5.2
                    if (mbr == "TryParse") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                        return;
                    }
                    // int.MaxValue / int.MinValue: int
                    if (mbr == "MaxValue" || mbr == "MinValue") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(pt));
                        return;
                    }
                }
                if (pt == PrimitiveType::Double || pt == PrimitiveType::Float) {
                    if (mbr == "Parse") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(pt));
                        return;
                    }
                    if (mbr == "TryParse") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                        return;
                    }
                }
                // §9.3 / #884: Decimal (now Primitive) static members.
                // TryParse(string, out decimal): bool; Parse(string): decimal;
                // Zero/One/MinValue/MaxValue → decimal (covered by the generic
                // MaxValue/MinValue/Zero/One branch at line 4577, Parse at 4604).
                if (pt == PrimitiveType::Decimal) {
                    if (mbr == "TryParse") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                        return;
                    }
                }
                // §8.1 / §8.4 (#1119): Object.ReferenceEquals(a, b): bool —
                // compares object identity, ignoring any Equals override. Codegen
                // lowers it to a pointer-identity compare. (Passing value-type
                // arguments always returns false — each boxing creates a fresh
                // object — which the pointer compare yields naturally.)
                if (pt == PrimitiveType::Object && mbr == "ReferenceEquals") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                    return;
                }
                // per §3.4: the primitive-static handler above recognises a
                // fixed, closed set of names.  Any name that reached this
                // point is NOT in that set — emit a diagnostic and return so
                // the callee node is left untyped-but-diagnosed rather than
                // untyped-and-silent, which would otherwise crash codegen
                // (issue #491).
                ctx_.diagnostics().report(
                    Severity::Error, ma.span.begin,
                    "type '" + std::string(id2->name) +
                        "' has no static method or property '" +
                        std::string(mbr) + "' (§3.4)");
                return;
            }
        }
    }
    // stdlib §3.2: the Encoding singletons. `Encoding.Utf8` (and Utf16/Utf32/
    // Ascii) are `public static readonly property ...: Encoding`. The target is
    // the auto-imported `Encoding` builtin class (a synthetic TypeSymbol with a
    // null decl), so the ordinary member-lookup path has no property to find.
    // Type the access as the `Encoding` type (a Named ref whose declaration is
    // the Encoding TypeSymbol) so the subsequent `.GetBytes(s)` / `.GetString(b)`
    // call resolves. Only Utf8 is implemented; Utf16/Utf32/Ascii are rejected
    // here (their concrete GetBytes/GetString semantics are not specified by
    // §3.2 — endianness, surrogate handling, Ascii out-of-range — so a sound
    // implementation is not yet possible: decline-with-reasons per CLAUDE.md §4).
    // §6.1 / §30.8 (#461): CancellationToken.None — a static readonly property
    // on the CancellationToken builtin type name. It must be typed HERE, before
    // the `targetType` resolution below: the `CancellationToken` identifier
    // resolves to a builtin TypeSymbol, so `targetType` becomes non-null and the
    // later `if (!targetType)` block (which also contained a None branch) is
    // skipped — leaving `CancellationToken.None` with NO exprType. An untyped
    // initializer makes a `var t := CancellationToken.None` async-frame local
    // default to an `i32` frame slot (collectSuspendLocals) while its alloca is a
    // `ptr`; the cross-thread suspend then stores 8 bytes into the 4-byte slot,
    // clobbering the adjacent pending-task field → SIGSEGV on resume. Typing it
    // as CancellationToken (a ptr-lowered builtin record) gives the frame slot
    // the correct ptr width so the local round-trips. Mirrors Encoding.Utf8.
    if (ma.target) {
        if (auto* cid = dynamic_cast<const ast::IdentifierExpr*>(ma.target.get())) {
            if (cid->name == "CancellationToken" && ma.member == "None") {
                auto* ctTy = ctx_.makeTypeRef();
                ctTy->kind = TypeRef::Kind::Named;
                ctTy->declaration = builtinClassSym("CancellationToken");
                ctx_.setExprType(&ma, ctTy);
                return;
            }
        }
    }
    if (ma.target) {
        if (auto* eid = dynamic_cast<const ast::IdentifierExpr*>(ma.target.get())) {
            if (eid->name == "Encoding") {
                const auto& sg = ma.member;
                if (sg == "UTF8" || sg == "UTF16" || sg == "UTF32" ||
                    sg == "ASCII") {
                    // stdlib §3.2 v0.136: all four singletons now implemented.
                    auto* encTy = ctx_.makeTypeRef();
                    encTy->kind = TypeRef::Kind::Named;
                    encTy->declaration = builtinClassSym("Encoding");
                    ctx_.setExprType(&ma, encTy);
                    return;
                }
                // #984 / stdlib §3.2: any OTHER member of the `Encoding` builtin
                // is invalid — the only singletons are UTF8/UTF16/UTF32/ASCII.
                // Without this reject, `Encoding.<X>` for an unrecognized X (most
                // commonly `Encoding.Utf8` — the old PascalCase form) was left
                // untyped, the chained `.GetBytes(...)` had no codegen lowering,
                // and the result silently miscompiled to garbage (an
                // uninitialized local → Length 0 / SIGSEGV on index). A clear
                // diagnostic is the sound outcome.
                ctx_.diagnostics().report(
                    Severity::Error, ma.span.begin,
                    "`Encoding` has no member `" + std::string(ma.member) +
                    "` — the encoding singletons are `UTF8`, `UTF16`, `UTF32`, "
                    "and `ASCII` (stdlib §3.2)");
                return;
            }
        }
        // The `Encoding` singleton is reachable ONLY through the direct
        // `Encoding.Utf8.GetBytes(...)` / `.GetString(...)` form (the codegen
        // lowers exactly that two-level shape; there is no first-class singleton
        // OBJECT yet). Reject an INDIRECT use — a member access whose target is
        // an `Encoding`-typed value that is NOT the direct `Encoding.<singleton>`
        // member access (e.g. a local `var e := Encoding.Utf8; e.GetBytes(s)`).
        // Without this guard such a call has no codegen lowering and would
        // miscompile (a null receiver → crash). A clear compile error is the
        // sound outcome until a real singleton object is modeled.
        if (auto* tt = ctx_.exprType(*ma.target)) {
            bool targetIsEncoding =
                tt->kind == TypeRef::Kind::Named && tt->declaration &&
                tt->declaration->name == "Encoding";
            bool targetIsDirectSingleton = false;
            if (auto* inner =
                    dynamic_cast<const ast::MemberAccessExpr*>(ma.target.get())) {
                if (auto* iid = dynamic_cast<const ast::IdentifierExpr*>(
                        inner->target.get()))
                    targetIsDirectSingleton = iid->name == "Encoding";
            }
            if (targetIsEncoding && !targetIsDirectSingleton) {
                ctx_.diagnostics().report(
                    Severity::Error, ma.span.begin,
                    "an `Encoding` value is usable only as the immediate "
                    "receiver of `Encoding.UTF8.GetBytes(...)` / "
                    "`.GetString(...)` (stdlib §3.2); binding it to a variable "
                    "or using it indirectly is not yet supported");
                return;
            }
        }
    }
    if (!targetType && ma.target) {
        auto* tt = ctx_.exprType(*ma.target);
        // stdlib §5 (#651): IGrouping<TKey, T> property accesses.
        // IGrouping TypeRefs are synthetic (no declaration); detect by astNode name.
        if (tt && tt->kind == TypeRef::Kind::Named && !tt->declaration &&
            tt->astNode && tt->astNode->name &&
            !tt->astNode->name->parts.empty() &&
            tt->astNode->name->parts[0] == "IGrouping") {
            // Key: TKey (genericArgs[0]).
            if (ma.member == "Key" && !tt->genericArgs.empty() &&
                tt->genericArgs[0]) {
                ctx_.setExprType(&ma, tt->genericArgs[0]);
                return;
            }
            // Further IGrouping properties (if needed) go here.
            return;
        }
        if (tt && tt->kind == TypeRef::Kind::Named) {
            targetType = tt->declaration;
            // issue #484 / §14.2: when the receiver's TypeRef is a bare type
            // parameter (declaration==null, name is a generic-param name), look
            // the member up through the parameter's where-constraints.  Both the
            // enclosing method's genericConstraints (currentMethodDecl_) and the
            // enclosing class's genericConstraints (currentType_->decl) are
            // searched; the method's constraints shadow the class's for the same
            // param name. Guard strictly: declaration==null AND the name is a
            // single-part name (ast::QualifiedName with exactly one part) that
            // matches a declared type parameter on the method or the class —
            // never fires for ordinary named-type receivers (those have non-null
            // declarations) or for generic stdlib types (Array<T>, etc., which
            // also have null declarations but whose names are not in the method's
            // genericParams list).
            if (!targetType && tt->astNode && tt->astNode->name &&
                    tt->astNode->name->parts.size() == 1) {
                std::string_view paramName = tt->astNode->name->parts[0];
                // Collect applicable constraints from the method and the class.
                // Method constraints come first so a method-level `where T: X`
                // shadows a class-level `where T: Y` if both exist (unusual but
                // per §14.2 method constraints are more specific).
                auto tryConstraints =
                    [&](const std::vector<ast::GenericParam>& gps,
                        const std::vector<ast::GenericConstraint>& gcs) {
                    // Only act if paramName is actually a declared generic param.
                    bool isParam = false;
                    for (const auto& gp : gps)
                        if (gp.name == paramName) { isParam = true; break; }
                    if (!isParam) return;
                    // Walk each where-clause for this param.
                    for (const auto& gc : gcs) {
                        if (gc.typeParam != paramName) continue;
                        for (const auto& item : gc.items) {
                            // Only "type" items carry a TypeArg that may name a
                            // class or interface; skip "class"/"value"/"new"/etc.
                            if (item.kind != "type") continue;
                            if (!item.typeArg) continue;
                            TypeRef* ctr = ctx_.resolvedType(*item.typeArg);
                            if (!ctr || ctr->kind != TypeRef::Kind::Named ||
                                    !ctr->declaration) continue;
                            // Try to find the member on this constraint type.
                            Symbol* cm =
                                lookupMember(ctr->declaration, ma.member);
                            if (!cm) continue;
                            // Found it: resolve via the constraint type, just
                            // as the normal branch below does for a concrete
                            // receiver.  Set the exprSymbol on the MemberAccess
                            // and attach its type.  Use the constraint's
                            // TypeSymbol as targetType so the visibility check
                            // and the per-member-kind type-attachment logic
                            // later in this function fire correctly.
                            // We set targetType here so the `if (!targetType)`
                            // block (intrinsic handling) is bypassed entirely.
                            targetType = ctr->declaration;
                            return;
                        }
                    }
                };
                // Try method constraints first (most specific), then class.
                if (currentMethodDecl_ &&
                        !currentMethodDecl_->genericParams.empty()) {
                    tryConstraints(currentMethodDecl_->genericParams,
                                   currentMethodDecl_->genericConstraints);
                }
                // Class-level generic params if the method search didn't resolve.
                if (!targetType && currentType_ && currentType_->decl) {
                    if (auto* td = dynamic_cast<const ast::TypeDecl*>(
                            currentType_->decl)) {
                        if (!td->genericParams.empty()) {
                            tryConstraints(td->genericParams,
                                           td->genericConstraints);
                        }
                    }
                }
            }
        } else if (!tt) {
            // Target had no resolvable type — fall back to the
            // exprSymbol path (e.g. local declared with `var d: D`
            // when locals don't carry exprType yet but DO carry
            // exprSymbol whose decl gives the resolved type).
            auto* sym = ctx_.exprSymbol(*ma.target);
            if (auto* loc = dynamic_cast<LocalSymbol*>(sym)) {
                if (loc->declType &&
                    loc->declType->kind == TypeRef::Kind::Named) {
                    targetType = loc->declType->declaration;
                }
            } else if (auto* par = dynamic_cast<ParameterSymbol*>(sym)) {
                if (par->declType) {
                    if (auto* rt = ctx_.resolvedType(*par->declType)) {
                        if (rt->kind == TypeRef::Kind::Named) {
                            targetType = rt->declaration;
                        }
                    }
                } else if (par->resolvedDeclType &&
                           par->resolvedDeclType->kind == TypeRef::Kind::Named) {
                    // per §24.3: a context-typed lambda param (event handler).
                    targetType = par->resolvedDeclType->declaration;
                }
            } else if (auto* ts = dynamic_cast<TypeSymbol*>(sym)) {
                // Static member access `Type.member`: the target is a
                // type name, so look the member up on that type. (The
                // static-vs-instance correctness of the member is
                // enforced by the static-context checks, not here.)
                //
                // §22.12 / #696: a generic class's static members must be
                // accessed with explicit type arguments on the type name
                // (e.g. `Counter<int>.n`, not bare `Counter.n`).  Each
                // distinct instantiation has its own static state, so an
                // unqualified name is ambiguous and ill-formed.
                //
                // Exception: inside the generic class's OWN body (ts ==
                // currentType_) the bare name is the self-reference form
                // (implicitly Tag<T>.field for the current instantiation)
                // — that idiom is accepted and resolves correctly in
                // codegen when genericSubst_ is active.
                if (!ts->genericParams.empty() && ts != currentType_) {
                    auto* tid =
                        dynamic_cast<const ast::IdentifierExpr*>(ma.target.get());
                    if (tid && tid->typeArgs.empty()) {
                        ctx_.diagnostics().report(
                            Severity::Error, ma.span.begin,
                            std::string("generic type `") +
                            std::string(ts->name) +
                            "` requires type arguments when accessing its "
                            "static members from outside the class body — "
                            "use `" + std::string(ts->name) +
                            "<T>.` with the concrete type arguments "
                            "(§22.12)");
                        return;
                    }
                }
                targetType = ts;
            } else if (auto* nts = dynamic_cast<NestedTypeSymbol*>(sym)) {
                // §11.3 / #482 fix B: a NestedTypeSymbol (e.g. `Container.Status`
                // resolves to a NestedTypeSymbol for `Status`) is used as the
                // target of a further member access — e.g. `Container.Status.Inactive`.
                // NestedTypeSymbol does NOT inherit TypeSymbol, so the branch above
                // misses it. Extract the inner TypeSymbol so enum-constant lookup
                // (and other member-access paths) work correctly.
                if (nts->type) targetType = nts->type;
            }
        }
    }
    if (!targetType) {
        // §31 / #600 phase c: `e.Message` where `e` is typed as a PURE built-in
        // exception (declaration==nullptr, no user TypeSymbol — e.g. a caught
        // `DomainFaultedException e` or `ChannelClosedException e`).  The user-
        // class branch further below handles `Message` on user classes that
        // INHERIT from a built-in exception, but that branch needs targetType!=null.
        // Here targetType IS null (pure built-in), so we match by ast node name
        // against the known built-in exception set and type the access as `string`.
        // Codegen reads the message-ptr at byte offset 8 (LlvmBackend_Exprs.cpp
        // CG22+ handler) — same slot for ALL built-in exception objects.
        if (ma.target && ma.member == "Message") {
            if (auto* tt = ctx_.exprType(*ma.target)) {
                if (tt->kind == TypeRef::Kind::Named && !tt->declaration &&
                        tt->astNode && tt->astNode->name &&
                        !tt->astNode->name->parts.empty()) {
                    auto beName = [](std::string_view n) -> bool {
                        return n == "Exception"                 ||
                               n == "ArgumentException"         ||
                               n == "ArgumentNullException"     ||
                               n == "ArithmeticException"       ||
                               n == "OverflowException"         ||
                               n == "DivideByZeroException"     ||
                               n == "InvalidOperationException" ||
                               n == "DisposedAccessException"   ||
                               n == "NotImplementedException"   ||
                               n == "NullReferenceException"    ||
                               n == "IndexOutOfRangeException"  ||
                               n == "KeyNotFoundException"      ||
                               n == "MatchFailureException"     ||
                               n == "OperationCanceledException"||
                               n == "TypeInitializationException" ||
                               n == "FormatException"           ||
                               n == "StackOverflowException"    ||
                               // §9.8 / #688 / stdlib §2.15: SerializationException
                               // lives in System.Serialization (not §31.4) but is
                               // registered as a built-in exception (declaration==null).
                               // Add it here so `e.Message` is typed as string when
                               // `catch var e: SerializationException` is used.
                               n == "SerializationException"    ||
                               n == "ChannelClosedException"    ||  // §30.15 #600c
                               n == "DomainFaultedException"    ||  // §30.14 #600c
                               // #976 / stdlib §7.4 / §7.5: System.Threading +
                               // System.IO exception families.
                               n == "TimeoutException"          ||
                               n == "IOException"               ||
                               n == "FileNotFoundException"     ||
                               n == "DirectoryNotFoundException"||
                               n == "EndOfStreamException"      ||
                               n == "PathTooLongException";
                    };
                    if (beName(tt->astNode->name->parts.back())) {
                        ctx_.setExprType(
                            &ma, ctx_.primitiveTypeRef(PrimitiveType::String));
                        return;
                    }
                }
            }
        }
        // issue #494 / §29.2 / §29.3: `e.Current` property access where `e` is
        // typed as the builtin IEnumerator<T> or OwnedEnumerator<T> (no sema
        // TypeDecl). Current returns T (the first generic arg). Typing this lets
        // `var x := e.Current` and `s := s + e.Current` infer the element type.
        if (ma.target && ma.member == "Current") {
            if (auto* tt = ctx_.exprType(*ma.target)) {
                bool isBuiltinEnumerator =
                    tt->kind == TypeRef::Kind::Named && !tt->declaration &&
                    tt->astNode && tt->astNode->name &&
                    !tt->astNode->name->parts.empty() &&
                    // issue #759: the async enumerators expose Current: T too.
                    (tt->astNode->name->parts[0] == "IEnumerator" ||
                     tt->astNode->name->parts[0] == "OwnedEnumerator" ||
                     tt->astNode->name->parts[0] == "IAsyncEnumerator" ||
                     tt->astNode->name->parts[0] == "OwnedAsyncEnumerator");
                if (isBuiltinEnumerator && !tt->genericArgs.empty() &&
                        tt->genericArgs[0]) {
                    ctx_.setExprType(&ma, tt->genericArgs[0]);
                    return;
                }
            }
        }
        // #548 / stdlib §4.8 v0.135: `DictionaryEntry<K,V>.Key` is K and
        // `.Value` is V.  DictionaryEntry has no user TypeSymbol (it is a
        // builtin synthetic type), so the ordinary member-lookup path never
        // types these.  Detect by the importedName="DictionaryEntry" tag
        // (set by the for-loop sema above when element type is derived from
        // a Dictionary<K,V> source).
        if (ma.target && (ma.member == "Key" || ma.member == "Value")) {
            if (auto* tt = ctx_.exprType(*ma.target)) {
                bool isDictEntry =
                    tt->kind == TypeRef::Kind::Named && !tt->declaration &&
                    tt->importedName == "DictionaryEntry" &&
                    tt->genericArgs.size() >= 2;
                if (isDictEntry) {
                    std::size_t gi = ma.member == "Key" ? 0 : 1;
                    if (tt->genericArgs[gi])
                        ctx_.setExprType(&ma, tt->genericArgs[gi]);
                    return;
                }
                // #628 / stdlib §4.9 v0.135: `DrainedEntry<K,V>.Key` is K
                // and `.Value` is V. Mirrors DictionaryEntry handling above.
                // DrainedEntry has no user TypeSymbol; detect by importedName tag.
                bool isDrainedEntry =
                    tt->kind == TypeRef::Kind::Named && !tt->declaration &&
                    tt->importedName == "DrainedEntry" &&
                    tt->genericArgs.size() >= 2;
                if (isDrainedEntry) {
                    std::size_t gi = ma.member == "Key" ? 0 : 1;
                    if (tt->genericArgs[gi])
                        ctx_.setExprType(&ma, tt->genericArgs[gi]);
                    return;
                }
            }
        }
        // #370 / §4.8 / fix #556: `Dictionary<K,V>.Keys` is `ICollection<K>`
        // and `.Values` is `ICollection<V>`.  The Dictionary has no user
        // TypeSymbol, so the ordinary member-lookup path never types these
        // properties.  Type the access as a concrete Array<K> (resp. Array<V>)
        // carried structurally — a Named TypeRef whose astNode names "Array"
        // and whose genericArgs[0] holds the element type.
        //
        // Using "Array" (not a null astNode) is load-bearing:
        //   (a) isCollectionTypeName() in codegen requires a non-null astNode
        //       with name "Array"/… so that .ToArray() and other LINQ ops on
        //       d.Keys/d.Values route through the array-backed lazy path instead
        //       of falling through to vtable dispatch on a raw bz_array_t*
        //       (which was the root cause of the #556 SIGSEGV).
        //   (b) isBuiltinColl in the .Count sema handler at L3046 requires the
        //       same guard; with a null astNode, .Count was not typed `int` and
        //       codegen read the array struct's first word as i32 (garbage).
        //
        // The runtime does produce a bz_array_t* for both Keys and Values
        // (lowerExpr for Keys was already present; lowerExpr for Values is
        // added in fix #556), so "Array" accurately describes the runtime object.
        //
        // The static astNode is program-lifetime (intentionally leaked),
        // consistent with all other synthetic nodes (kArrayAstNode, kDictAstNode,
        // etc.) in this file.
        if (ma.target && (ma.member == "Keys" || ma.member == "Values")) {
            if (auto* tt = ctx_.exprType(*ma.target)) {
                bool isDict =
                    tt->kind == TypeRef::Kind::Named && !tt->declaration &&
                    tt->astNode && tt->astNode->name &&
                    !tt->astNode->name->parts.empty() &&
                    (tt->astNode->name->parts[0] == "Dictionary" ||
                     tt->astNode->name->parts[0] == "OwnedDictionary");
                std::size_t gi = ma.member == "Keys" ? 0 : 1;
                if (isDict && tt->genericArgs.size() > gi && tt->genericArgs[gi]) {
                    // Static synthetic astNode for "Array" (program-lifetime).
                    static const ast::Type* kKeysValuesArrayNode = []() -> ast::Type* {
                        auto* qn = new ast::QualifiedName;
                        qn->parts.push_back("Array");
                        auto* t = new ast::Type;
                        t->kind = ast::TypeKind::Named;
                        t->name.reset(qn);
                        return t;
                    }();
                    auto* coll = ctx_.makeTypeRef();
                    coll->kind = TypeRef::Kind::Named;
                    coll->declaration = nullptr;
                    coll->astNode = kKeysValuesArrayNode;
                    coll->genericArgs.push_back(tt->genericArgs[gi]);
                    ctx_.setExprType(&ma, coll);
                    return;
                }
            }
        }
        // issue #421/#420 (stdlib §4.4/§4.10/§4.11): the built-in heap
        // collections (Array/Stack/Queue/Set/LinkedList/Dictionary and their
        // Owned variants) carry no user TypeSymbol — their TypeRef is a Named
        // ref with a null `declaration`. The ordinary member-lookup path below
        // never types a PROPERTY read on them, so `a.Count` (no parens) was
        // left untyped and codegen had to infer `int` from the lowered LLVM
        // value (#420's tryLowerConsoleCall workaround). Type these built-in
        // collection properties directly: `Count` is `int` for every collection
        // (ICollection<T>.Count, stdlib §4.3); `Capacity` is `int` for the two
        // array types that declare it (Array<T>/OwnedArray<T>, stdlib §4.4).
        if (ma.target && (ma.member == "Count" || ma.member == "Capacity")) {
            if (auto* tt = ctx_.exprType(*ma.target)) {
                bool isBuiltinColl =
                    tt->kind == TypeRef::Kind::Named && !tt->declaration &&
                    tt->astNode && tt->astNode->name &&
                    !tt->astNode->name->parts.empty();
                std::string_view cn = isBuiltinColl
                    ? std::string_view(tt->astNode->name->parts[0])
                    : std::string_view();
                bool hasCount =
                    cn == "Array" || cn == "OwnedArray" ||
                    cn == "Stack" || cn == "OwnedStack" ||
                    cn == "Queue" || cn == "OwnedQueue" ||
                    cn == "Set" || cn == "OwnedSet" ||
                    cn == "LinkedList" || cn == "OwnedLinkedList" ||
                    cn == "Dictionary" || cn == "OwnedDictionary" ||
                    // §4.14 / issue #550: OwnedBuffer<T>.Count (bz_array_t-backed).
                    cn == "OwnedBuffer";
                bool hasCapacity = cn == "Array" || cn == "OwnedArray";
                bool ok = ma.member == "Count" ? hasCount : hasCapacity;
                if (ok) {
                    ctx_.setExprType(
                        &ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                    return;
                }
            }
        }
        // §4.15 (#549) / §4.14 (#779): span readonly properties (ArraySpan /
        // ReadOnlyArraySpan / Span / ReadOnlySpan). Neither has a user TypeSymbol,
        // so the ordinary member-lookup path never types these. Handle Length
        // and IsEmpty here.
        if (ma.target &&
            (ma.member == "Length" || ma.member == "IsEmpty")) {
            if (auto* tt = ctx_.exprType(*ma.target)) {
                bool isSpan = tt->kind == TypeRef::Kind::Named &&
                    !tt->declaration && tt->astNode && tt->astNode->name &&
                    !tt->astNode->name->parts.empty() &&
                    (tt->astNode->name->parts[0] == "ArraySpan" ||
                     tt->astNode->name->parts[0] == "ReadOnlyArraySpan" ||
                     tt->astNode->name->parts[0] == "Span" ||
                     tt->astNode->name->parts[0] == "ReadOnlySpan" ||
                     // §3.3 (#883): ReadOnlyStringSpan.Length / .IsEmpty
                     tt->astNode->name->parts[0] == "ReadOnlyStringSpan");
                if (isSpan) {
                    ctx_.setExprType(
                        &ma, ctx_.primitiveTypeRef(
                            ma.member == "IsEmpty" ? PrimitiveType::Bool
                                                   : PrimitiveType::Int));
                    return;
                }
            }
        }
        // per §4.6/§4.7: LinkedList<T>/OwnedLinkedList<T> node-returning properties
        // (FirstNode, LastNode, First, Last) and LinkedListNode<T> accessor
        // properties (Value, Next, Previous). None have user TypeSymbols, so the
        // ordinary member-lookup path never types them.
        if (ma.target) {
            auto* tt = ctx_.exprType(*ma.target);
            bool isBuiltinLL = tt && tt->kind == TypeRef::Kind::Named &&
                !tt->declaration && tt->astNode && tt->astNode->name &&
                !tt->astNode->name->parts.empty();
            std::string_view llcn = isBuiltinLL
                ? std::string_view(tt->astNode->name->parts[0]) : std::string_view{};

            // LinkedList<T> / OwnedLinkedList<T> node properties.
            if ((llcn == "LinkedList" || llcn == "OwnedLinkedList") &&
                !tt->genericArgs.empty() && tt->genericArgs[0]) {
                auto* elemTy = tt->genericArgs[0];
                const auto& mbr = ma.member;

                // Helper: build a LinkedListNode<T> TypeRef with the element type.
                // Codegen only needs the top-level name; we forward elemTy as the
                // generic arg in the TypeRef, not via the astNode's genericArgs.
                auto makeNodeTy = [&](bool nullable) -> TypeRef* {
                    auto* nt = ctx_.makeTypeRef();
                    nt->kind = TypeRef::Kind::Named;
                    nt->nullable = nullable;
                    auto* ann = new ast::Type();
                    ann->kind = ast::TypeKind::Named;
                    auto* nm  = new ast::QualifiedName();
                    nm->parts.push_back("LinkedListNode");
                    ann->name.reset(nm);
                    nt->astNode = ann;
                    nt->genericArgs.push_back(elemTy);
                    return nt;
                };
                auto makeNullableElemTy = [&]() -> TypeRef* {
                    auto* et = ctx_.makeTypeRef();
                    *et = *elemTy;
                    et->nullable = true;
                    return et;
                };

                if (mbr == "FirstNode" || mbr == "LastNode") {
                    // returns LinkedListNode<T>? (null when empty)
                    ctx_.setExprType(&ma, makeNodeTy(/*nullable=*/true));
                    return;
                }
                if (mbr == "First" || mbr == "Last") {
                    // returns T? (null when empty)
                    ctx_.setExprType(&ma, makeNullableElemTy());
                    return;
                }
            }

            // LinkedListNode<T> accessor properties: Value, Next, Previous.
            if (llcn == "LinkedListNode" &&
                !tt->genericArgs.empty() && tt->genericArgs[0]) {
                auto* elemTy = tt->genericArgs[0];
                const auto& mbr = ma.member;
                if (mbr == "Value") {
                    ctx_.setExprType(&ma, elemTy);
                    return;
                }
                if (mbr == "Next" || mbr == "Previous") {
                    // returns LinkedListNode<T>? — a nullable node borrow.
                    // Use makeNodeTy(nullable=true) pattern from above.
                    auto* nt = ctx_.makeTypeRef();
                    nt->kind = TypeRef::Kind::Named;
                    nt->nullable = true;
                    auto* ann = new ast::Type();
                    ann->kind = ast::TypeKind::Named;
                    auto* nm  = new ast::QualifiedName();
                    nm->parts.push_back("LinkedListNode");
                    ann->name.reset(nm);
                    nt->astNode = ann;
                    nt->genericArgs.push_back(elemTy);
                    ctx_.setExprType(&ma, nt);
                    return;
                }
            }
        }
        // issue #545: Buffer<T>.Length property — returns int.
        // The ordinary member-lookup path never finds it (no user TypeDecl).
        if (ma.target) {
            auto* tt = ctx_.exprType(*ma.target);
            bool isBuffer = tt && tt->kind == TypeRef::Kind::Named &&
                !tt->declaration && tt->astNode && tt->astNode->name &&
                !tt->astNode->name->parts.empty() &&
                tt->astNode->name->parts[0] == "Buffer";
            if (isBuffer && ma.member == "Length") {
                ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                return;
            }
        }
        // stdlib §10.3 / fix #985: RawBytes readonly properties. Length: int and
        // Pointer: IntPtr. The RawBytes TypeRef carries declaration =
        // builtinClassSym("RawBytes") (set by the RawBytes.From factory typing),
        // so the ordinary member-lookup path never types these (no user TypeDecl).
        // Without typing them here, the access fell through untyped and a
        // `var p: IntPtr := rb.Pointer` would not see the IntPtr type.
        if (ma.target) {
            auto* tt = ctx_.exprType(*ma.target);
            bool isRaw = tt && tt->kind == TypeRef::Kind::Named &&
                tt->declaration && tt->declaration->name == "RawBytes";
            if (isRaw && ma.member == "Length") {
                ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                return;
            }
            if (isRaw && ma.member == "Pointer") {
                // IntPtr: a Named type resolved by name (no user TypeDecl); codegen
                // lowers it to a pointer-sized integer (§32.7).
                auto* ip = ctx_.makeTypeRef();
                ip->kind = TypeRef::Kind::Named;
                auto* ann = new ast::Type();
                ann->kind = ast::TypeKind::Named;
                auto* nm = new ast::QualifiedName();
                nm->parts.push_back("IntPtr");
                ann->name.reset(nm);
                ip->astNode = ann;
                ctx_.setExprType(&ma, ip);
                return;
            }
        }
        // §6.4 / issue #647: Atomic<T> and AtomicInteger<T> property accesses.
        // The `Value` property getter returns T (genericArgs[0]).  The ordinary
        // member-lookup path never finds it (no user TypeDecl for Atomic<T>).
        if (ma.target) {
            auto* tt = ctx_.exprType(*ma.target);
            bool isAtomicLike = tt && tt->kind == TypeRef::Kind::Named &&
                !tt->declaration && tt->astNode && tt->astNode->name &&
                !tt->astNode->name->parts.empty() &&
                (tt->astNode->name->parts[0] == "Atomic" ||
                 tt->astNode->name->parts[0] == "AtomicInteger");
            if (isAtomicLike && ma.member == "Value") {
                TypeRef* elemTy = (!tt->genericArgs.empty() && tt->genericArgs[0])
                    ? const_cast<TypeRef*>(tt->genericArgs[0])
                    : ctx_.primitiveTypeRef(PrimitiveType::Int);
                ctx_.setExprType(&ma, elemTy);
                return;
            }
        }
        // §2.5: DateTime and TimeSpan instance property/method member accesses.
        // These are builtin value types stored as scalar i64; the ordinary
        // member-lookup path never finds their properties.
        if (ma.target) {
            auto* tt = ctx_.exprType(*ma.target);
            auto builtinTyNameLocal = [](const TypeRef* rt) -> std::string_view {
                if (!rt || rt->kind != TypeRef::Kind::Named) return {};
                if (!rt->declaration) {
                    if (rt->astNode && rt->astNode->name &&
                        !rt->astNode->name->parts.empty())
                        return rt->astNode->name->parts[0];
                    return {};
                }
                if (!rt->declaration->decl) return rt->declaration->name;
                return {};
            };
            std::string_view dtcn = builtinTyNameLocal(tt);
            if (dtcn == "DateTime") {
                const auto& mbr = ma.member;
                // Integer properties → int.
                if (mbr == "Year" || mbr == "Month" || mbr == "Day" ||
                    mbr == "Hour" || mbr == "Minute" || mbr == "Second" ||
                    mbr == "Millisecond" || mbr == "DayOfYear") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                    return;
                }
                // DayOfWeek → int (codegen treats enum as i32).
                if (mbr == "DayOfWeek") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                    return;
                }
                // Ticks → long (the raw i64 tick count).
                if (mbr == "Ticks") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Long));
                    return;
                }
                // Add*/Subtract instance methods → return DateTime or TimeSpan
                // (resolved in the call-expression handler below, not as a property).
            }
            if (dtcn == "TimeSpan") {
                const auto& mbr = ma.member;
                // Total* properties → double.
                if (mbr == "TotalDays" || mbr == "TotalHours" ||
                    mbr == "TotalMinutes" || mbr == "TotalSeconds" ||
                    mbr == "TotalMilliseconds") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Double));
                    return;
                }
                // Component properties → int.
                if (mbr == "Days" || mbr == "Hours" || mbr == "Minutes" ||
                    mbr == "Seconds" || mbr == "Milliseconds") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                    return;
                }
                // Ticks → long.
                if (mbr == "Ticks") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Long));
                    return;
                }
            }
            // §8.1 / issue #646: Stopwatch instance property reads.
            // Stopwatch is a builtin class (heap pointer); the ordinary member-
            // lookup path never finds its properties.
            if (dtcn == "Stopwatch") {
                const auto& mbr = ma.member;
                // IsRunning → bool.
                if (mbr == "IsRunning") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                    return;
                }
                // ElapsedMilliseconds → long (i64).
                if (mbr == "ElapsedMilliseconds") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Long));
                    return;
                }
                // Elapsed → TimeSpan (i64, 100-ns ticks; same representation as
                // TimeSpan returned by DateTime.Subtract etc.).
                if (mbr == "Elapsed") {
                    auto* tsTy = ctx_.makeTypeRef();
                    tsTy->kind = TypeRef::Kind::Named;
                    tsTy->declaration = builtinClassSym("TimeSpan");
                    ctx_.setExprType(&ma, tsTy);
                    return;
                }
            }
            // §2.5: DateTime static properties accessed as DateTime.Now, etc.
            // These appear as MemberAccess on the "DateTime" identifier.
            if (auto* did = dynamic_cast<const ast::IdentifierExpr*>(ma.target.get())) {
                if (did->name == "DateTime") {
                    auto* dt = ctx_.makeTypeRef();
                    dt->kind = TypeRef::Kind::Named;
                    dt->declaration = builtinClassSym("DateTime");
                    if (ma.member == "Now" || ma.member == "UtcNow" ||
                        ma.member == "Today" || ma.member == "MinValue" ||
                        ma.member == "MaxValue") {
                        ctx_.setExprType(&ma, dt);
                        return;
                    }
                }
                if (did->name == "TimeSpan") {
                    if (ma.member == "Zero") {
                        auto* ts = ctx_.makeTypeRef();
                        ts->kind = TypeRef::Kind::Named;
                        ts->declaration = builtinClassSym("TimeSpan");
                        ctx_.setExprType(&ma, ts);
                        return;
                    }
                }
            }
        }
        // §6.1 / §30.8: CancellationToken and CancellationTokenSource builtin
        // members — neither has a user TypeDecl, so the ordinary member-lookup
        // path never types their properties/methods.  Two TypeRef shapes exist:
        //   a) !declaration: TypeRef from a parsed type annotation (CancellationToken
        //      parameter/field), name in astNode->name->parts[0].
        //   b) declaration = builtinClassSym (null decl): TypeRef returned by
        //      cts.Token / CancellationToken.None, name in declaration->name.
        // Extract the name via a helper lambda matching both shapes.
        if (ma.target) {
            auto* tt = ctx_.exprType(*ma.target);
            auto builtinTyName = [](const TypeRef* rt) -> std::string_view {
                if (!rt || rt->kind != TypeRef::Kind::Named) return {};
                if (!rt->declaration) {
                    if (rt->astNode && rt->astNode->name &&
                        !rt->astNode->name->parts.empty())
                        return rt->astNode->name->parts[0];
                    return {};
                }
                if (!rt->declaration->decl) return rt->declaration->name;
                return {};
            };
            std::string_view cn = builtinTyName(tt);
            // stdlib §2.6: Guid instance member access types.
            if (cn == "Guid") {
                // ToString(): string (canonical form)
                if (ma.member == "ToString") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::String));
                    return;
                }
                // Equals(other: Guid): bool — structural equality
                if (ma.member == "Equals") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                    return;
                }
            }
            // stdlib §2.6: Guid.Empty — static readonly property on the Guid type name.
            if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(ma.target.get())) {
                if (tid->name == "Guid" && ma.member == "Empty") {
                    auto* gTy = ctx_.makeTypeRef();
                    gTy->kind = TypeRef::Kind::Named;
                    gTy->declaration = builtinClassSym("Guid");
                    ctx_.setExprType(&ma, gTy);
                    return;
                }
            }
            if (cn == "CancellationToken") {
                // IsCancellationRequested: bool property (§6.1)
                if (ma.member == "IsCancellationRequested") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                    return;
                }
                // ThrowIfCancellationRequested(): void — leave untyped (unit).
                // Register(cb): owned Subscription — deferred (see codegen note).
            }
            if (cn == "CancellationTokenSource") {
                // Token: CancellationToken (§6.1).  Use builtinClassSym so the
                // TypeRef's declaration carries the name "CancellationToken" —
                // used by subsequent IsCancellationRequested resolution, lowerTypeRef
                // (layout), and the instance-method codegen dispatch.
                if (ma.member == "Token") {
                    auto* ctTy = ctx_.makeTypeRef();
                    ctTy->kind = TypeRef::Kind::Named;
                    ctTy->declaration = builtinClassSym("CancellationToken");
                    ctx_.setExprType(&ma, ctTy);
                    return;
                }
                // IsCancellationRequested: bool (§6.1)
                if (ma.member == "IsCancellationRequested") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                    return;
                }
                // Cancel() / CancelAfter() / Dispose() → unit (leave untyped).
            }
            // per §6.8 / #652: Domain instance member access types.
            if (cn == "Domain") {
                // Name: string readonly property (§6.8).
                if (ma.member == "Name") {
                    ctx_.setExprType(&ma,
                        ctx_.primitiveTypeRef(PrimitiveType::String));
                    return;
                }
                // Dispose() → unit (leave untyped; codegen handles it).
            }
            // §30.8: CancellationToken.None — static readonly property on the
            // CancellationToken builtin type name used as an identifier expression.
            // The target's exprType is null (an identifier-as-type-name, not a
            // value expression), so we look at the exprSymbol instead.
            if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(ma.target.get())) {
                if (tid->name == "CancellationToken" && ma.member == "None") {
                    auto* ctTy = ctx_.makeTypeRef();
                    ctTy->kind = TypeRef::Kind::Named;
                    ctTy->declaration = builtinClassSym("CancellationToken");
                    ctx_.setExprType(&ma, ctTy);
                    return;
                }
            }
        }
        // CG27a (§4.6): handle known intrinsic string members directly —
        // string has no user TypeSymbol in the symbol table.
        if (ma.target) {
            auto* tt = ctx_.exprType(*ma.target);
            if (tt && tt->kind == TypeRef::Kind::Primitive) {
                if (tt->primitive == PrimitiveType::String) {
                    if (ma.member == "Length") {
                        // stdlib §9.6: Length: int
                        ctx_.setExprType(
                            &ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                        return;
                    }
                    if (ma.member == "IsAscii") {
                        ctx_.setExprType(
                            &ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                        return;
                    }
                    if (ma.member == "ToString") {
                        // string.ToString() returns itself (§9.6)
                        ctx_.setExprType(
                            &ma, ctx_.primitiveTypeRef(PrimitiveType::String));
                        return;
                    }
                    // CG27f: stdlib §9.6 string methods — result types.
                    if (ma.member == "Contains" ||
                        ma.member == "StartsWith" ||
                        ma.member == "EndsWith" ||
                        ma.member == "Equals") {
                        ctx_.setExprType(
                            &ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                        return;
                    }
                    if (ma.member == "CompareTo" ||
                        ma.member == "IndexOf" ||
                        ma.member == "LastIndexOf" ||
                        ma.member == "CountOf" ||
                        ma.member == "ToInt32" || ma.member == "ToUInt32" ||
                        ma.member == "GetHashCode") {  // stdlib §9.6, #882
                        ctx_.setExprType(
                            &ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                        return;
                    }
                    if (ma.member == "ToInt64") {
                        ctx_.setExprType(
                            &ma, ctx_.primitiveTypeRef(PrimitiveType::Long));
                        return;
                    }
                    if (ma.member == "ToDouble" || ma.member == "ToFloat") {
                        ctx_.setExprType(
                            &ma, ctx_.primitiveTypeRef(PrimitiveType::Double));
                        return;
                    }
                    if (ma.member == "ToBoolean") {
                        ctx_.setExprType(
                            &ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                        return;
                    }
                    if (ma.member == "Substring" ||
                        ma.member == "Trim" || ma.member == "TrimStart" ||
                        ma.member == "TrimEnd" || ma.member == "ToLower" ||
                        ma.member == "ToUpper" || ma.member == "Replace" ||
                        ma.member == "PadLeft" || ma.member == "PadRight" ||
                        ma.member == "Repeat" || ma.member == "Remove" ||
                        ma.member == "Insert" ||
                        ma.member == "GetField") {
                        ctx_.setExprType(
                            &ma, ctx_.primitiveTypeRef(PrimitiveType::String));
                        return;
                    }
                    // fix #500 / §9.6: Split(char|string|Array<char>) returns
                    // owned Array<string>.  Set the result type explicitly so
                    // that an unannotated local (`var parts := s.Split(',')`)
                    // infers Array<string> and downstream `.Count`/`.Get(i)`/
                    // for-in resolve correctly.  Mirror the makeConcreteCollection
                    // Result pattern used by ToArray() in typeCall().
                    if (ma.member == "Split") {
                        static const ast::Type* kArrayAstNode = []() -> ast::Type* {
                            auto* qn = new ast::QualifiedName;
                            qn->parts.push_back("Array");
                            auto* t = new ast::Type;
                            t->kind = ast::TypeKind::Named;
                            t->name.reset(qn);
                            return t;
                        }();
                        auto* tr = ctx_.makeTypeRef();
                        tr->kind = TypeRef::Kind::Named;
                        tr->declaration = nullptr;
                        tr->astNode = kArrayAstNode;
                        tr->genericArgs.push_back(
                            ctx_.primitiveTypeRef(PrimitiveType::String));
                        ctx_.setExprType(&ma, tr);
                        return;
                    }
                    // §9.6 / issue #654: ToCharArray() returns owned Array<char>.
                    // The return type is Array<char> so downstream element accesses
                    // (.Count / .Get(i)) resolve correctly.  The result is an owned
                    // value that callers discharge with `using var` per §28.
                    if (ma.member == "ToCharArray") {
                        static const ast::Type* kCharArrayAstNode = []() -> ast::Type* {
                            auto* qn = new ast::QualifiedName;
                            qn->parts.push_back("Array");
                            auto* t = new ast::Type;
                            t->kind = ast::TypeKind::Named;
                            t->name.reset(qn);
                            return t;
                        }();
                        auto* tr = ctx_.makeTypeRef();
                        tr->kind = TypeRef::Kind::Named;
                        tr->declaration = nullptr;
                        tr->astNode = kCharArrayAstNode;
                        tr->genericArgs.push_back(
                            ctx_.primitiveTypeRef(PrimitiveType::Char));
                        ctx_.setExprType(&ma, tr);
                        return;
                    }
                    // §9.6 / §3.3 (#883): string.AsSpan() and its overloads
                    // ((start), (start, length), (r: Range<int>)) return a
                    // ReadOnlyStringSpan — a non-allocating char-level view over a
                    // contiguous range of the string. (string also has
                    // AsAsciiByteSpan/TryAsAsciiByteSpan returning a
                    // ReadOnlyAsciiByteSpan — deferred; see the follow-up issue.)
                    if (ma.member == "AsSpan") {
                        static const ast::Type* kStrSpanNode = []() -> ast::Type* {
                            auto* qn = new ast::QualifiedName;
                            qn->parts.push_back("ReadOnlyStringSpan");
                            auto* t = new ast::Type;
                            t->kind = ast::TypeKind::Named;
                            t->name.reset(qn);
                            return t;
                        }();
                        auto* tr = ctx_.makeTypeRef();
                        tr->kind = TypeRef::Kind::Named;
                        tr->declaration = nullptr;
                        tr->astNode = kStrSpanNode;
                        ctx_.setExprType(&ma, tr);
                        return;
                    }
                    // fix #702 / §26.5: before reporting "not yet supported",
                    // try a user-defined extension method on string.  The
                    // generic extension-lookup block below (line ~5043) runs
                    // AFTER this block and would never be reached for an
                    // unrecognised string member because the error here used to
                    // stop sema cleanly — but the error itself suppressed valid
                    // §26.5 extension calls.  Attempt the lookup first; if a
                    // user extension resolves it, type the member access from
                    // the extension's signature and return without error.  Only
                    // emit the diagnostic when no user extension matches.
                    if (Symbol* ext = lookupExtensionMember(tt, ma.member)) {
                        resolveExtensionMember(ma, ext);
                        return;
                    }
                    // Emit a diagnostic for unrecognized string members.
                    ctx_.diagnostics().report(
                        Severity::Error, ma.span.begin,
                        std::string("string member `.") +
                        std::string(ma.member) +
                        "` is not yet supported in codegen");
                } else if (ma.member == "ToString") {
                    // CG27c: ToString() is an intrinsic on all primitive
                    // types (§5) — returns the string representation.
                    ctx_.setExprType(
                        &ma, ctx_.primitiveTypeRef(PrimitiveType::String));
                    return;
                } else if (ma.member == "IsNaN" || ma.member == "IsInfinity" ||
                           ma.member == "IsFinite" ||
                           ma.member == "IsPositiveInfinity" ||
                           ma.member == "IsNegativeInfinity") {
                    // §6.3 / stdlib §9.2: all five FP predicates — bool result.
                    ctx_.setExprType(
                        &ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                    return;
                } else if (ma.member == "Abs") {
                    // §5: Abs on numeric primitive — same type.
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(tt->primitive));
                    return;
                } else if (ma.member == "CompareTo") {
                    // fix #586 / stdlib §2.8 / §9.1–§9.5: CompareTo(other: T):
                    // int — IComparable<T> contract. Covers every numeric and
                    // char primitive (not string, which is handled above; not
                    // bool, which does not implement IComparable per §9.4).
                    using P = PrimitiveType;
                    bool isComparable =
                        tt->primitive == P::Int    || tt->primitive == P::Long  ||
                        tt->primitive == P::Short  || tt->primitive == P::Byte  ||
                        tt->primitive == P::SByte  || tt->primitive == P::UInt  ||
                        tt->primitive == P::ULong  || tt->primitive == P::UShort||
                        tt->primitive == P::Double || tt->primitive == P::Float ||
                        tt->primitive == P::Decimal|| tt->primitive == P::Char;
                    if (isComparable) {
                        ctx_.setExprType(
                            &ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                        return;
                    }
                } else if (ma.member == "Round" &&
                           tt->primitive == PrimitiveType::Decimal) {
                    // stdlib §9.3 / fix #521: decimal.Round(digits: int) → decimal.
                    ctx_.setExprType(
                        &ma, ctx_.primitiveTypeRef(PrimitiveType::Decimal));
                    return;
                }
            }
        }
        // issue #605 / §14.1 / §8.1 / §8.5: a BARE TYPE-PARAMETER receiver
        // (`val: T where T: value`) accessing a universal Object member
        // (ToString, GetHashCode, Equals, GetType). The type-param's TypeRef
        // has kind=Named, no declaration, so none of the intrinsic-type
        // branches above fire for it.  Set the MemberAccessExpr type here so
        // that the enclosing CallExpr typeCall() propagates it through the
        // `!calleeSym` → `callee exprType` fallback. Without this the
        // MemberAccess stays untyped, the CallExpr is untyped, and
        // lowerLocalDecl (§15.2) cannot infer the inferred-var's LLVM slot
        // type — it falls back to `i32`, so `var s := val.ToString()` gets
        // an i32 slot and `return s` emits `ret i32` in a `ptr`-return
        // function → IR verification failure. The fix aligns with the same
        // handling applied to record/enum receivers (§9.1/§10) at line 3834
        // above. Only fires when targetType is still null — i.e. the param
        // was not found through any constraint-bound type (only value/class/
        // new() shape constraints, not a named interface/class constraint).
        if (ma.target) {
            if (const TypeRef* recvTy = ctx_.exprType(*ma.target)) {
                if (recvTy->kind == TypeRef::Kind::Named &&
                    !recvTy->declaration && recvTy->astNode &&
                    recvTy->astNode->name &&
                    recvTy->astNode->name->parts.size() == 1) {
                    // Bare type-parameter receiver (T, TKey, TValue, …).
                    const auto& mbr = ma.member;
                    if (mbr == "ToString")
                        ctx_.setExprType(
                            &ma, ctx_.primitiveTypeRef(PrimitiveType::String));
                    else if (mbr == "GetHashCode")
                        ctx_.setExprType(
                            &ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                    else if (mbr == "Equals")
                        ctx_.setExprType(
                            &ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                    else if (mbr == "CompareTo")
                        // #903 / §14.2: `a.CompareTo(b)` on a bare type-parameter
                        // receiver — the IComparable<T> contract (a `where T:
                        // IComparable<T>` constraint, enforced at the call site,
                        // §14.2/#729). The builtin IComparable<T> has a null
                        // TypeDecl, so the constraint-member resolution above skips
                        // it; type the result int here (mirroring Equals→bool) so
                        // the call typechecks, and codegen devirtualizes to the
                        // monomorphized type's CompareTo (LlvmBackend_Calls.cpp).
                        ctx_.setExprType(
                            &ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                    else if (mbr == "GetType") {
                        auto* typeTy = ctx_.makeTypeRef();
                        typeTy->kind = TypeRef::Kind::Named;
                        typeTy->declaration = builtinClassSym("Type");
                        ctx_.setExprType(&ma, typeTy);
                    }
                }
            }
        }
        // §26.5: a user extension on a PRIMITIVE/builtin receiver (e.g.
        // `extension int`). Consulted last — after every builtin/intrinsic
        // member above — so a real builtin member always wins (no shadow).
        // The receiver's compile-time type is `ma.target`'s exprType.
        if (ma.target) {
            if (const TypeRef* recvTy = ctx_.exprType(*ma.target)) {
                // §26.5 (#714): pass outBinding to capture the free-var
                // binding when a generic extension matched via ancestor lookup.
                std::vector<std::pair<std::string, sema::TypeRef*>> binding;
                if (Symbol* ext = lookupExtensionMember(
                        recvTy, ma.member, &binding)) {
                    if (!binding.empty())
                        ctx_.setExprExtBinding(&ma, std::move(binding));
                    resolveExtensionMember(ma, ext);
                    return;
                }
            }
        }
        return;
    }
    // Lookup member by name. Slice S5: walks own members first,
    // then transitively each base type — first match wins (no
    // ambiguity resolution between base interfaces yet).
    Symbol* member = lookupMember(targetType, ma.member);
    // §26.5: extension members are consulted ONLY when no instance member
    // matched (instance members win — an extension never shadows a real
    // member, #433). The receiver's compile-time type is the access target's
    // type (a Named user type here); fall back to a TypeRef synthesized from
    // the resolved TypeSymbol when the target carried no exprType.
    if (!member && ma.target) {
        const TypeRef* recvTy = ctx_.exprType(*ma.target);
        TypeRef synth;
        if (!recvTy) {
            synth.kind = TypeRef::Kind::Named;
            synth.declaration = targetType;
            recvTy = &synth;
        }
        // §26.5 (#714): pass outBinding to capture the free-var binding.
        std::vector<std::pair<std::string, sema::TypeRef*>> binding;
        if (Symbol* ext = lookupExtensionMember(
                recvTy, ma.member, &binding)) {
            if (!binding.empty())
                ctx_.setExprExtBinding(&ma, std::move(binding));
            resolveExtensionMember(ma, ext);
            return;
        }
    }
    if (!member) {
        // §2.3: built-in class TypeSymbols (Console, Math, Convert, …)
        // have null `decl` and empty members. Assign return types for
        // their known methods so callers can infer the call result type.
        if (!targetType->decl) {
            std::string_view cls = targetType->name;
            const auto& m = ma.member;
            // §2.2 Math / §2.3 Convert static methods.
            //
            // #1037: Convert and Math are DISTINCT classes with disjoint method
            // sets (Convert = value conversions; Math = numeric functions). The
            // accept logic used to be one `cls == "Convert" || cls == "Math"`
            // block that typed the UNION on either class, so `Math.ToInt32(5)`
            // and `Convert.Sqrt(16.0)` were accepted but had no codegen lowering
            // for that class (Convert's lowering is `cls == "Convert"`, Math's is
            // `cls == "Math"`) → silent miscompile (45 / 0). And because this
            // builtin-class path (null `decl`) has no fallback "no such member"
            // reject, a fully-unknown `Math.Bogus(5)` likewise miscompiled to
            // garbage. Split per class and reject anything outside each class's
            // own set with a §3.4 diagnostic — the valid set per class is exactly
            // the set its codegen handler lowers (LlvmBackend_Calls.cpp).
            auto setInt = [&]{ ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Int)); };
            auto setLong = [&]{ ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Long)); };
            auto setDbl = [&]{ ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Double)); };
            auto setBool = [&]{ ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool)); };
            auto setStr = [&]{ ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::String)); };
            auto rejectBuiltinStatic = [&]{
                ctx_.diagnostics().report(
                    Severity::Error, ma.span.begin,
                    "'" + std::string(cls) + "' has no static method or property '" +
                        std::string(m) + "' (§3.4)");
            };
            if (cls == "Convert") {
                if (m=="ToInt32") setInt();
                else if (m=="ToInt64") setLong();
                else if (m=="ToDouble") setDbl();
                else if (m=="ToBoolean") setBool();
                // §2.3: Convert.TryToInt32(value, out result): bool.
                else if (m=="TryToInt32") setBool();
                else if (m=="ToString") setStr();
                else rejectBuiltinStatic();
            }
            else if (cls == "Math") {
                if (m=="Abs"||m=="Sign") setInt();
                else if (m=="Sqrt"||m=="Pow"||m=="Log"||m=="Log2"||m=="Log10"||
                         m=="Ceiling"||m=="Floor"||m=="Round"||m=="Truncate"||
                         // §2.2: the spec constants are `Pi`, `E`, `Tau` (the
                         // legacy `PI` spelling is retained for the existing
                         // intrinsic-path test but is not a spec name).
                         m=="Pi"||m=="PI"||m=="E"||m=="Tau"||
                         m=="Fmod"||m=="IEEERemainder"||m=="Sin"||m=="Cos"||m=="Tan"||
                         m=="Asin"||m=="Acos"||m=="Atan"||m=="Atan2"||m=="Exp"||m=="Hypot"||
                         m=="Sinh"||m=="Cosh"||m=="Tanh") setDbl();
                // Min/Max/Clamp are overloaded (int or double) — valid, but left
                // untyped here; their result type is refined from the argument in
                // typeCall (§2.2 polymorphic-return handling).
                else if (m=="Min"||m=="Max"||m=="Clamp") { /* valid, untyped */ }
                else rejectBuiltinStatic();
            }
            // Console input methods.
            if (cls == "Console") {
                if (m == "ReadLine")
                    ctx_.setExprType(
                        &ma, ctx_.primitiveTypeRef(PrimitiveType::String,
                                                   /*nullable=*/true));
                else if (m == "ReadKey")
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Char));
                // Write/WriteLine → unit (leave untyped).
            }
            // Random class methods.
            if (cls == "Random") {
                if (m == "Next" || m == "NextInt")
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                else if (m == "NextDouble" || m == "NextFloat")
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Double));
                // Seed → unit (leave untyped)
            }
            // stdlib §2.6: Guid static member types (reached via declaration lookup
            // path where the target is the Guid TypeSymbol with null decl).
            if (cls == "Guid") {
                if (m == "NewGuid" || m == "Parse") {
                    auto* gTy = ctx_.makeTypeRef();
                    gTy->kind = TypeRef::Kind::Named;
                    gTy->declaration = builtinClassSym("Guid");
                    ctx_.setExprType(&ma, gTy);
                } else if (m == "TryParse") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                } else if (m == "Empty") {
                    auto* gTy = ctx_.makeTypeRef();
                    gTy->kind = TypeRef::Kind::Named;
                    gTy->declaration = builtinClassSym("Guid");
                    ctx_.setExprType(&ma, gTy);
                } else if (m == "ToString") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::String));
                } else if (m == "Equals") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                }
            }
            // stdlib §9.4 / issue #654: Boolean static member types.
            // Reached when the receiver is the `Boolean` class identifier
            // (the keyword alias `bool` is handled via primitiveFromName above).
            if (cls == "Boolean") {
                if (m == "TryParse") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                } else if (m == "Parse") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                }
            }
            // stdlib §2.12 / issue #654: BitConverter static methods.
            // FloatToInt32Bits(float)→int, Int32BitsToFloat(int)→float,
            // DoubleToInt64Bits(double)→long, Int64BitsToDouble(long)→double.
            if (cls == "BitConverter") {
                if (m == "FloatToInt32Bits") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                } else if (m == "Int32BitsToFloat") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Float));
                } else if (m == "DoubleToInt64Bits") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Long));
                } else if (m == "Int64BitsToDouble") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Double));
                } else if (m == "Reinterpret") {
                    // stdlib §2.12 (#753): generic same-width bit reinterpretation.
                    // The result type is the single explicit type argument (the
                    // target); the source is inferred from the call argument. The
                    // same-bit-width requirement is validated in typeCall, where
                    // the argument's type is available.
                    if (!ma.typeArgs.empty() && ma.typeArgs[0] &&
                        ma.typeArgs[0]->name &&
                        !ma.typeArgs[0]->name->parts.empty()) {
                        PrimitiveType tgt = primitiveFromName(
                            ma.typeArgs[0]->name->parts.front());
                        if (tgt != PrimitiveType::None)
                            ctx_.setExprType(&ma, ctx_.primitiveTypeRef(tgt));
                    }
                } else {
                    // #1038: BitConverter is a pure-static utility class (§2.12)
                    // with exactly these five methods — the codegen handler
                    // (LlvmBackend_Calls.cpp `cls == "BitConverter"`) lowers no
                    // others. Like Math/Convert (#1037), an unknown static
                    // (`BitConverter.Bogus(1)`) was previously left untyped and
                    // silently miscompiled to a no-op; reject it with §3.4. Safe
                    // here because BitConverter has no instances, no sub-objects,
                    // and no unit-returning members (unlike Console/Random/Guid/
                    // Decimal, which are instance-bearing or have untyped-unit
                    // members and need static-vs-instance disambiguation — see
                    // issue #1038).
                    rejectBuiltinStatic();
                }
            }
            // stdlib §9.3 / fix #521: Decimal static member types.
            // Reached when the receiver is the `Decimal` class identifier.
            if (cls == "Decimal" || cls == "decimal") {  // §5: both spellings (spec L243)
                // Zero, One, MinValue, MaxValue → decimal.
                if (m == "Zero" || m == "One" || m == "MinValue" || m == "MaxValue" ||
                    m == "Parse") {
                    ctx_.setExprType(&ma,
                        ctx_.primitiveTypeRef(PrimitiveType::Decimal));
                } else if (m == "TryParse") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                }
                // ToString → string, CompareTo → int, Round → decimal (instance methods).
            }
            // §8.1 / issue #646: Stopwatch instance method and static factory
            // return types.  Start/Stop/Reset/Restart → unit (leave untyped).
            // StartNew() is also handled in the static-call path above; the
            // duplicate here handles the `sw.StartNew()` misuse gracefully
            // (won't crash, just leaves untyped — the static path is the real one).
            if (cls == "Stopwatch") {
                if (m == "StartNew") {
                    auto* swTy = ctx_.makeTypeRef();
                    swTy->kind = TypeRef::Kind::Named;
                    swTy->declaration = builtinClassSym("Stopwatch");
                    ctx_.setExprType(&ma, swTy);
                }
                // IsRunning, ElapsedMilliseconds, Elapsed: property reads, not
                // call expressions — handled in resolveMemberAccess above.
                // Start / Stop / Reset / Restart → unit (leave untyped).
            }
            // §8.2 / issue #646: Debug static methods — all return unit.
            // Assert / WriteLine: leave untyped (void).
            // No return-type annotation needed.

            // §6.1 / §30.8: CancellationToken instance property reads.
            // Reached when tok was bound via `cts.Token` (builtinClassSym decl).
            if (cls == "CancellationToken") {
                if (m == "IsCancellationRequested")
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                // Token is CancellationToken.None — handled as identifier.
                // ThrowIfCancellationRequested / Register → unit (leave untyped).
            }
            // §6.1: CancellationTokenSource instance property reads.
            if (cls == "CancellationTokenSource") {
                if (m == "Token") {
                    auto* ctTy = ctx_.makeTypeRef();
                    ctTy->kind = TypeRef::Kind::Named;
                    ctTy->declaration = builtinClassSym("CancellationToken");
                    ctx_.setExprType(&ma, ctTy);
                } else if (m == "IsCancellationRequested") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                }
                // Cancel / CancelAfter / Dispose → unit (leave untyped).
            }
            // §9.8 / §2.15 / issue #550: BinarySerializer and BinaryDeserializer
            // are compiler-known builtin classes (null `decl`, non-null astNode).
            // Type their return-producing methods so downstream code can infer
            // chain types (e.g. `using var bytes := s.ToBytes()` must know `bytes`
            // is OwnedBuffer<byte> so `bytes.AsReadOnlySpan()` can be intercepted).
            if (cls == "BinarySerializer") {
                if (m == "ToBytes") {
                    // Returns OwnedBuffer<byte> (stdlib §2.15).
                    // Set on the CallExpr so that inferred-type locals
                    // (`using var bytes := s.ToBytes()`) pick up the type
                    // at line 1692 (`lsym->declType = ctx_.exprType(*ld->initializer)`).
                    // Also set on the MemberAccessExpr callee so that the
                    // fallback callee-type propagation below picks up OwnedBuffer<byte>
                    // when `m == "ToBytes"` reaches the callee-type propagation block.
                    static const ast::Type* kOwnedBufferAstNode = []() -> ast::Type* {
                        auto* qn = new ast::QualifiedName;
                        qn->parts.push_back("OwnedBuffer");
                        auto* t = new ast::Type;
                        t->kind = ast::TypeKind::Named;
                        t->name.reset(qn);
                        return t;
                    }();
                    auto* byTy  = ctx_.primitiveTypeRef(PrimitiveType::Byte);
                    auto* obTy  = ctx_.makeTypeRef();
                    obTy->kind  = TypeRef::Kind::Named;
                    obTy->astNode = kOwnedBufferAstNode;
                    obTy->genericArgs.push_back(byTy);
                    ctx_.setExprType(&ma, obTy);
                    // NOTE: the CallExpr type is set separately in typeCall
                    // where `ce` is in scope (see the rcn == "BinarySerializer" block).
                }
                // Serialize(ISerializer) / Dispose → unit (leave untyped).
            }
            if (cls == "BinaryDeserializer") {
                // All BinaryDeserializer methods are void; leave untyped.
            }
            // §2.15 / issue #550: JsonSerializer and JsonDeserializer.
            if (cls == "JsonSerializer") {
                if (m == "ToBytes") {
                    // Returns OwnedBuffer<byte> (stdlib §2.15).
                    static const ast::Type* kJserObNode = []() -> ast::Type* {
                        auto* qn = new ast::QualifiedName;
                        qn->parts.push_back("OwnedBuffer");
                        auto* t = new ast::Type;
                        t->kind = ast::TypeKind::Named;
                        t->name.reset(qn);
                        return t;
                    }();
                    auto* byTy  = ctx_.primitiveTypeRef(PrimitiveType::Byte);
                    auto* obTy  = ctx_.makeTypeRef();
                    obTy->kind  = TypeRef::Kind::Named;
                    obTy->astNode = kJserObNode;
                    obTy->genericArgs.push_back(byTy);
                    ctx_.setExprType(&ma, obTy);
                    // CallExpr type is set in the rcn == "JsonSerializer" block above.
                } else if (m == "ToString") {
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::String));
                }
                // Serialize(ISerializer) / Dispose → unit (leave untyped).
            }
            if (cls == "JsonDeserializer") {
                // All JsonDeserializer methods are void or return -1 (int);
                // leave untyped for now (downstream uses are in synthesized
                // Deserialize which only reads typed results from binder calls).
            }
        } else if (targetType->kind == ast::TypeDeclKind::Record ||
                   targetType->kind == ast::TypeDeclKind::Enum) {
            // §8 / §9 / §10: the value-semantics members the compiler
            // synthesizes for records and enums. No MemberSymbol exists for
            // them, but the call result is well-typed so callers (and string
            // interpolation) can infer it. ToString → string (§8.5/§9.1);
            // GetHashCode → int (§8.3); Equals → bool (§8.3/§8.4).
            const auto& m = ma.member;
            // §9.2/§8.3: a direct `.Equals(...)` / `.GetHashCode()` on a body
            // record that is not auto-equality-bearing and declares no equality
            // of its own is a compile error at the call site. "Calling
            // `someRecord.Equals(other)` or `someRecord.GetHashCode()` on a
            // non-auto-equality-bearing record without a user-defined override is
            // a compile error … The error covers `Equals(Object?)`, `Equals(T)`
            // … and `GetHashCode`." (We reach this branch only when lookupMember
            // found no user member of that name, so any hand-written
            // `Equals`/`GetHashCode` is already excluded; the operator-only
            // explicit-equality form is excluded via recordHasUserDeclaredEquality.
            // ToString is *not* covered: ineligible records keep the type-name
            // default ToString per §8.5/§9.2.) Positional records, enums, and
            // eligible body records are unaffected.
            if ((m == "Equals" || m == "GetHashCode") &&
                targetType->kind == ast::TypeDeclKind::Record) {
                bool positional = false;
                if (auto* td = dynamic_cast<const ast::TypeDecl*>(targetType->decl))
                    positional = td->hasPositionalFields;
                if (!positional &&
                    !isAutoEqualityBearingBodyRecord(targetType) &&
                    !recordHasUserDeclaredEquality(targetType)) {
                    ctx_.diagnostics().report(
                        Severity::Error, ma.span.begin,
                        "`." + std::string(m) + "` is not available on record `" +
                        std::string(targetType->name) +
                        "`: it is not auto-equality-bearing (§9.2). Declare `=`"
                        " and `<>` overloads (with `Equals`/`GetHashCode`), or"
                        " simplify the record to an auto-equality-bearing form.");
                }
            }
            if (m == "ToString")
                ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::String));
            else if (m == "GetHashCode")
                ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
            else if (m == "Equals")
                ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
            else if (m == "GetType") {
                // §8.2 (#487): GetType() is a universal Object member (§8.1)
                // and returns the `Type` builtin class (stdlib §2.13).
                auto* typeTy = ctx_.makeTypeRef();
                typeTy->kind = TypeRef::Kind::Named;
                typeTy->declaration = builtinClassSym("Type");
                ctx_.setExprType(&ma, typeTy);
            } else if (m == "Serialize" || m == "Deserialize") {
                // §9.8 / issue #550: Serialize(s: ISerializer) and Deserialize
                // are synthesized by codegen for records/enums that declare
                // ISerializable / IDeserializable conformance. No MemberSymbol
                // exists; sema must not reject the call.  We check the AST
                // TypeDecl::baseTypes because TypeSymbol::baseTypes skips the
                // builtin interface bases (ISerializable / IDeserializable have
                // no user TypeDecl, so they never appear in TypeSymbol::baseTypes).
                auto* td = dynamic_cast<const ast::TypeDecl*>(targetType->decl);
                bool declaresSer = false, declaresDeser = false;
                if (td) {
                    for (const auto& bt : td->baseTypes) {
                        if (!bt || !bt->name || bt->name->parts.empty()) continue;
                        const auto& n = bt->name->parts.back();
                        if (n == "ISerializable")   declaresSer  = true;
                        if (n == "IDeserializable") declaresDeser = true;
                    }
                }
                if ((m == "Serialize" && declaresSer) ||
                    (m == "Deserialize" && declaresDeser)) {
                    // §9.8 / issue #550: Synthesis suppresses "no member".
                    // For Deserialize the static synthesized method returns the
                    // record/enum type itself (Point.Deserialize(d): Point).
                    // Set that return type on the MemberAccessExpr so that
                    // typeCall's !calleeSym fallback propagates it to the
                    // CallExpr and `var p2 := T.Deserialize(d)` infers T for p2.
                    if (m == "Deserialize" && declaresDeser && targetType) {
                        auto* retTy = ctx_.makeTypeRef();
                        retTy->kind = TypeRef::Kind::Named;
                        retTy->declaration = targetType;
                        ctx_.setExprType(&ma, retTy);
                    }
                    // Serialize returns unit (void); leave the MemberAccessExpr
                    // untyped — codegen intercepts via tryLowerSerializationCall.
                }
                // If no conformance was declared the "no member" error will
                // fire normally below via knownCompleteUserType.
            }
        }
        // issue #414 (§8.1, §11.3, §12.1): diagnose a genuinely-unknown member.
        // Left undiagnosed, an unresolved member produces no resolved symbol and
        // codegen lowers a null callee → a silent garbage/zero result. We turn
        // it into a compile error — but ONLY when sema can be CERTAIN the member
        // is absent, i.e. the receiver is a fully-resolved USER type whose member
        // set is known-complete. That is a `class`/`record`/`enum` whose
        // TypeSymbol carries a non-null `decl` (a user TypeDecl). Everything else
        // stays permissive, because builtin/intrinsic member resolution is
        // incomplete and a blanket rejection would falsely reject valid code:
        //
        //   - Builtin/intrinsic classes (Console, Math, Convert, Random, …) and
        //     the builtin collection / string / Task types have a null `decl`
        //     and no MemberSymbols; their members are dispatched by name in
        //     codegen, not via sema symbols. (Reached via the `!targetType->decl`
        //     branch above; we never get here for them with a non-null decl.)
        //   - Interfaces: default/inherited interface-member resolution is not
        //     complete here, so an interface-typed receiver is not known-complete.
        //   - The universal `Object` members (§8.1) — `GetType`, `Equals`,
        //     `GetHashCode`, `ToString`, `ReferenceEquals` — and `Dispose`
        //     (§8.7, on every non-static class) exist on EVERY type without a
        //     user MemberSymbol (codegen synthesizes them), so they must never be
        //     rejected here.
        //   - `Object`-typed and generic-parameter (`T`) receivers never reach
        //     this branch: their TypeRef has no non-null `declaration`, so
        //     `targetType` is null and the function returned earlier.
        //
        // When the receiver was flow-narrowed (§12.7, issue #413), `targetType`
        // is already the narrowed type, so a member declared only on the narrowed
        // type resolves and is not rejected.
        // #795 / §2.5 / §2.6: an IMPORTED user type is also known-complete — its
        // metadata carries its full member set (and base chain, which lookupMember
        // walks), so a member that resolves to nothing genuinely does not exist.
        // An imported type has a null `decl` (no local AST), so it would otherwise
        // miss the `decl != nullptr` gate below and an unknown-member call would
        // miscompile silently (garbage result) instead of failing the build.
        // Restricted to NON-GENERIC imported types: imported generic-class member
        // completeness is not yet guaranteed (the #428 family — #783/#710/#711),
        // so imported generics stay permissive to avoid a false rejection.
        // Builtin types (Console, Math, …) are NOT imported, so they keep their
        // null-decl permissive path; imported INTERFACES have kind Interface and
        // are excluded by the kind check (their member resolution is incomplete).
        bool importedKnownCompleteUserType =
            ctx_.isImported(targetType) && targetType->genericParams.empty();
        bool knownCompleteUserType =
            (targetType->decl != nullptr || importedKnownCompleteUserType) &&
            (targetType->kind == ast::TypeDeclKind::Class ||
             targetType->kind == ast::TypeDeclKind::Record ||
             targetType->kind == ast::TypeDeclKind::Enum);
        // §8.1 / §8.7: universal members present on every type without a
        // MemberSymbol — never diagnose these as missing.
        const auto& mm = ma.member;
        bool isUniversalObjectMember =
            mm == "ToString" || mm == "Equals" || mm == "GetHashCode" ||
            mm == "GetType" || mm == "ReferenceEquals" || mm == "Dispose";
        // §9.8 / issue #550: Serialize and Deserialize are synthesized for
        // records/enums/serialized-tree-classes that declare
        // ISerializable/IDeserializable. They have no MemberSymbol, so we must
        // not fire "no member" on them.  We check the AST TypeDecl::baseTypes
        // (TypeSymbol::baseTypes skips builtin interface bases like ISerializable
        // because they carry no user TypeDecl).
        bool isSerializationSynthesizedMember = false;
        if ((mm == "Serialize" || mm == "Deserialize") && knownCompleteUserType &&
                (targetType->kind == ast::TypeDeclKind::Record ||
                 targetType->kind == ast::TypeDeclKind::Enum ||
                 targetType->kind == ast::TypeDeclKind::Class)) {
            auto* tdSer = dynamic_cast<const ast::TypeDecl*>(targetType->decl);
            if (tdSer) {
                // For classes: only synthesized when a serialized-class clause
                // is present (§9.8 tier 2+). Without it the class must hand-write.
                bool classOk = (targetType->kind != ast::TypeDeclKind::Class) ||
                               (tdSer->serializedClass != nullptr);
                if (classOk) {
                    for (const auto& bt : tdSer->baseTypes) {
                        if (!bt || !bt->name || bt->name->parts.empty()) continue;
                        const auto& bn = bt->name->parts.back();
                        if (mm == "Serialize"   && bn == "ISerializable")   { isSerializationSynthesizedMember = true; break; }
                        if (mm == "Deserialize" && bn == "IDeserializable") { isSerializationSynthesizedMember = true; break; }
                    }
                }
            }
        }
        // §9.8 tier 2: for a `serialized tree/graph/cyclic` class, set the
        // return type of synthesized Deserialize so that inferred-type locals
        // (`using var b2 := Box.Deserialize(d)`) get the right type.
        if (isSerializationSynthesizedMember && mm == "Deserialize" &&
                targetType->kind == ast::TypeDeclKind::Class) {
            auto* retTy = ctx_.makeTypeRef();
            retTy->kind = TypeRef::Kind::Named;
            retTy->declaration = targetType;
            ctx_.setExprType(&ma, retTy);
        }
        // #595 / §12 / §31: `Message` is an inherited member on any user class
        // that derives (directly or via a user-class chain) from a built-in
        // exception type (Exception, ArgumentException, …).  Built-in exception
        // types are absent from TypeSymbol::baseTypes so we walk the AST
        // TypeDecl::baseTypes chain to detect the inheritance.  When `Message` is
        // found valid here, also set its resolved type to `string` so callers
        // (e.g. `.Length` on the result) can infer the chain type.
        bool isBuiltinExInheritedMember = false;
        if (mm == "Message" && knownCompleteUserType &&
                targetType->kind == ast::TypeDeclKind::Class) {
            // Walk the user-class TypeSymbol chain checking each AST TypeDecl's
            // base-type list for a built-in exception name.
            auto isBuiltinExName = [](std::string_view n) -> bool {
                return n == "Exception"             ||
                       n == "ArgumentException"     ||
                       n == "ArgumentNullException" ||
                       n == "ArithmeticException"   ||
                       n == "OverflowException"     ||
                       n == "DivideByZeroException" ||
                       n == "InvalidOperationException" ||
                       n == "NotImplementedException"   ||
                       n == "NullReferenceException"    ||
                       n == "IndexOutOfRangeException"  ||
                       n == "KeyNotFoundException"      ||
                       n == "MatchFailureException"     ||
                       n == "OperationCanceledException"||
                       n == "TypeInitializationException" ||
                       n == "FormatException"           ||
                       n == "StackOverflowException"    ||
                       // #976 / stdlib §7.4 / §7.5: System.Threading + System.IO.
                       n == "TimeoutException"          ||
                       n == "IOException"               ||
                       n == "FileNotFoundException"     ||
                       n == "DirectoryNotFoundException"||
                       n == "EndOfStreamException"      ||
                       n == "PathTooLongException";
            };
            for (const TypeSymbol* cur = targetType; cur && !isBuiltinExInheritedMember;) {
                auto* td = dynamic_cast<const ast::TypeDecl*>(cur->decl);
                if (td) {
                    for (const auto& bt : td->baseTypes) {
                        if (!bt || !bt->name || bt->name->parts.empty()) continue;
                        if (isBuiltinExName(bt->name->parts.back())) {
                            isBuiltinExInheritedMember = true;
                            break;
                        }
                    }
                }
                // Advance to next user-class base.
                const TypeSymbol* next = nullptr;
                for (const TypeSymbol* b : cur->baseTypes) {
                    if (b && b->kind == ast::TypeDeclKind::Class && b->decl) {
                        next = b; break;
                    }
                }
                cur = next;
            }
            if (isBuiltinExInheritedMember) {
                // `e.Message` returns a string (§31 / stdlib Exception.Message).
                ctx_.setExprType(&ma,
                    ctx_.primitiveTypeRef(PrimitiveType::String));
            }
        }
        if (knownCompleteUserType && !isUniversalObjectMember &&
                !isBuiltinExInheritedMember &&
                !isSerializationSynthesizedMember) {
            ctx_.diagnostics().report(
                Severity::Error, ma.span.begin,
                std::string("no member `") + std::string(ma.member) +
                    "` on type `" + std::string(targetType->name) +
                    "` (§11.3, §12.1)");
        }
        // #608 / §8.5 / §12: `ToString()` is universal and returns `string`.
        // Set the MemberAccessExpr's type so that callers (e.g. `.Length` on the
        // result, or an assignment `var s := e.ToString()`) can infer the type.
        // This covers the class-with-user-decl case where no MethodSymbol exists
        // (user exception class inheriting from a built-in exception base does not
        // have ToString in TypeSymbol::baseTypes, so lookup returns nullptr).
        // The record/enum cases are handled in their respective branches above.
        // per §8.5 (v0.134).
        if (mm == "ToString")
            ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::String));
        // §8.2 (#487): `GetType` is universal and returns `Type` (stdlib §2.13).
        // Set the MemberAccessExpr's type here so that: (a) a bare `obj.GetType`
        // in value position is typed, and (b) `typeCall` can propagate it to the
        // CallExpr via the callee-type path (line 526+ above). This covers the
        // class-with-user-decl case; the builtin-class, record, and enum cases are
        // handled in their respective branches above.
        if (mm == "GetType") {
            auto* typeTy = ctx_.makeTypeRef();
            typeTy->kind = TypeRef::Kind::Named;
            typeTy->declaration = builtinClassSym("Type");
            ctx_.setExprType(&ma, typeTy);
        }
        return;
    }
    // §13.1 (issue #432): full visibility check — private, protected, internal,
    // public. Replaces the old private-only slice S8 check. The helper
    // checkMemberVisibility handles all four levels and the overload-suppression
    // heuristic (a non-private accessible sibling → suppress the error).
    if (auto* msym = dynamic_cast<MemberSymbol*>(member)) {
        checkMemberVisibility(msym, ma.member, ma.span.begin);
    }
    ctx_.setExprSymbol(&ma, member);
    // §2.2 / §2.5 (issue #328 inc 3): an imported field/const member has no
    // ast::FieldDecl/ConstDecl, so its resolved type rides the loader's
    // symbol-keyed side table. Attach it for `obj.Field` access typing.
    if (ctx_.isImported(member)) {
        if (dynamic_cast<FieldSymbol*>(member) ||
            dynamic_cast<ConstMemberSymbol*>(member)) {
            if (TypeRef* rt = ctx_.importedMemberType(member))
                ctx_.setExprType(&ma,
                    substituteMemberType(rt, ma.target.get(),
                                         dynamic_cast<MemberSymbol*>(member)
                                             ? dynamic_cast<MemberSymbol*>(member)->parent
                                             : nullptr));
        } else if (dynamic_cast<PropertySymbol*>(member)) {
            // §2.5 / §22.9 (issue #503): an IMPORTED property has no
            // ast::PropertyDecl, so its type rides the importedMemberType side
            // table (set by the loader). Attach it so that `h.N` is typed for
            // downstream ownership/definite-assignment and codegen.
            if (TypeRef* rt = ctx_.importedMemberType(member))
                ctx_.setExprType(&ma,
                    substituteMemberType(rt, ma.target.get(),
                                         dynamic_cast<MemberSymbol*>(member)
                                             ? dynamic_cast<MemberSymbol*>(member)->parent
                                             : nullptr));
        } else if (auto* ec = dynamic_cast<EnumConstantSymbol*>(member)) {
            // §10 (issue #328 FINDING [1]): an IMPORTED enum-constant access
            // `Color.Green` has the enum type, exactly like the local-enum case
            // below. The non-imported path falls through to the EnumConstant
            // branch further down, but an imported member returns early here, so
            // set the type now. The enum type is the constant's parent; an
            // imported enum has no AST decl, so use the symbol's parent directly.
            // Without this a `var c := Color.Green` local and a `case c` /
            // `case Color.Green` scrutinee carry no type, and codegen's
            // enumTypeOf() cannot resolve the arms (they all lower to nothing).
            if (ec->parent) {
                TypeRef* et = ctx_.makeTypeRef();
                et->kind = TypeRef::Kind::Named;
                et->declaration = ec->parent;
                ctx_.setExprType(&ma, et);
            }
        }
        return;
    }
    // Attach the member's type for field / property access. Methods
    // and constructors are deferred to the call-resolution slice.
    if (auto* fs = dynamic_cast<FieldSymbol*>(member)) {
        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
        if (fd && fd->type) {
            if (auto* rt = ctx_.resolvedType(*fd->type)) {
                // per §14.1: substitute a generic-param field type against the
                // receiver's concrete args (issue #361 / D13).
                ctx_.setExprType(&ma,
                    substituteMemberType(rt, ma.target.get(), fs->parent));
            }
        } else if (fs->posParam && fs->posParam->type) {
            // §9: positional record field — type comes from the Param.
            if (auto* rt = ctx_.resolvedType(*fs->posParam->type)) {
                ctx_.setExprType(&ma,
                    substituteMemberType(rt, ma.target.get(), fs->parent));
            }
        }
    } else if (auto* ps = dynamic_cast<PropertySymbol*>(member)) {
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        if (pd && pd->type) {
            if (auto* rt = ctx_.resolvedType(*pd->type)) {
                ctx_.setExprType(&ma,
                    substituteMemberType(rt, ma.target.get(), ps->parent));
            }
        }
    } else if (auto* cs = dynamic_cast<ConstMemberSymbol*>(member)) {
        // per §22.12 / issue #609: a class-level const member access
        // `Class.ConstName` must carry the const's type so that type
        // inference (`var x := Class.Const`) and subsequent member access
        // (`x.ToString()`) resolve correctly. The exprType is read from
        // the explicit `: Type` annotation when present; when the const
        // has no annotation, fall back to the value expression's exprType
        // (which sema has already set via visitMember → visitExpr).
        auto* cd = dynamic_cast<const ast::ConstDecl*>(cs->decl);
        if (cd) {
            if (cd->type) {
                if (auto* rt = ctx_.resolvedType(*cd->type))
                    ctx_.setExprType(&ma, rt);
            } else if (cd->value) {
                if (auto* rt = ctx_.exprType(*cd->value))
                    ctx_.setExprType(&ma, rt);
            }
        }
    } else if (dynamic_cast<EnumConstantSymbol*>(member) && targetType) {
        // §10: an enum-constant access `E.Green` has the enum type, so a
        // local inferred from it (`var e := E.Green`) and a `case`
        // scrutinee both carry the enum type.
        TypeRef* et = ctx_.makeTypeRef();
        et->kind = TypeRef::Kind::Named;
        et->declaration = targetType;
        ctx_.setExprType(&ma, et);
    } else if (auto* ms = dynamic_cast<MethodSymbol*>(member)) {
        // §27.6 (issue #436): a bare `obj.Method` in value position (not the
        // callee of a CallExpr) is a method reference — a function value of
        // type `(paramTypes) -> returnType`. Assign a Function TypeRef here so
        // that downstream assignability checks (return-type §5.1 slice T3,
        // local-init §5.1 slice T1, assignment §5.1 slice T2) can detect a
        // type mismatch (e.g. `return a.GetVal;` where the method returns `int`
        // — the function type `() -> int` is not assignable to `int`).
        //
        // The call-expression handler (typeCall) reads the callee's exprSymbol,
        // not exprType, so assigning a Function type here does NOT affect
        // `obj.Method(args)` — the call is still typed via the MethodSymbol.
        auto* ft = ctx_.makeTypeRef();
        ft->kind = TypeRef::Kind::Function;
        // Build parameter types from the method's ParameterSymbol list.
        auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
        for (auto* psym : ms->params) {
            auto* par = dynamic_cast<ParameterSymbol*>(psym);
            if (!par) continue;
            TypeRef::FuncParam fp;
            fp.modifier = par->modifier;
            fp.name = par->name;
            if (par->declType)
                fp.type = ctx_.resolvedType(*par->declType);
            ft->funcParams.push_back(fp);
        }
        // Return type: from MethodDecl->returnType, or unit when absent.
        if (md && md->returnType) {
            ft->funcReturn = ctx_.resolvedType(*md->returnType);
            ft->funcReturnOwned = md->returnOwned;
        } else {
            auto* unit = ctx_.makeTypeRef();
            unit->kind = TypeRef::Kind::Unit;
            ft->funcReturn = unit;
        }
        ctx_.setExprType(&ma, ft);
    }
}

// §7.5 / #751: result type of a builtin string/primitive RECEIVER's
// property-like value member, for the null-conditional `x?.M` path. See the
// declaration comment in ExprTypecheck_impl.hpp for why resolveNullCondMember
// needs this (the authoritative listing is the inlined intrinsic block in
// resolveMemberAccess; only the bare-access value members are mirrored here).
TypeRef* ExprTC::builtinValueMemberType(const TypeRef* recvTy,
                                        std::string_view member) {
    if (!recvTy || recvTy->kind != TypeRef::Kind::Primitive) return nullptr;
    if (recvTy->primitive == PrimitiveType::String) {
        // stdlib §9.6: Length: int. (ToString and the rest are methods —
        // reached as `?(`, not a bare `?.` member access. `IsAscii` is also a
        // bare-access bool member and types correctly here, but its value path
        // needs a runtime scan helper that is the same gap regardless of `?.`;
        // it is left to a dedicated follow-up rather than half-enabled.)
        if (member == "Length")
            return ctx_.primitiveTypeRef(PrimitiveType::Int);
    }
    return nullptr;
}

// §7.5 null-conditional member access `x?.M`. Resolves the member on
// the target's (non-nullable) type exactly like `.`, but the result
// type is ALWAYS nullable (the access yields null when the target is
// null). The member's own type is looked up; its nullable form is the
// expression's type, so member access chains (`a?.b?.c`) resolve.
void ExprTC::resolveNullCondMember(const ast::NullCondMemberExpr& nm) {
    if (!nm.target) return;
    TypeSymbol* targetType = memberTargetType(nm.target.get());
    Symbol* member = targetType ? lookupMember(targetType, nm.member) : nullptr;
    // §7.5 / §26.5 / #458: for a NULLABLE VALUE type receiver (e.g. `int?`,
    // `bool?`, `enum?`, `record?`) memberTargetType returns null (because the
    // type is Primitive or Named-record, not a class TypeSymbol). Also fall
    // back for a Named receiver when lookupMember found nothing. In either
    // case try lookupExtensionMember — it ignores the `nullable` flag on the
    // receiver TypeRef (per extensionTargetMatchesReceiver's Primitive branch),
    // so `extension int` matches an `int?` receiver, matching §26.5 semantics
    // (extension resolution uses the unwrapped base type).
    if (!member) {
        const TypeRef* recvTy = nm.target ? ctx_.exprType(*nm.target) : nullptr;
        if (recvTy) {
            member = lookupExtensionMember(recvTy, nm.member);
        }
        // §7.5 / #751: a string/primitive built-in member (e.g. `Length`) has
        // no Symbol — resolveMemberAccess types these via a large inlined
        // intrinsic block this function does not run, so `s?.Length` was left
        // untyped. An inferred local copies that absent type, and `n ?? d`
        // (lowerCoalesce, gated on isNullableValueTy) then silently produced 0.
        // Type the value member here (nullable result per §7.5) so the inferred
        // local and `??` see a usable `int?`. The value codegen already handled
        // `s?.Length` (only the type was missing).
        if (!member && recvTy) {
            if (TypeRef* bt = builtinValueMemberType(recvTy, nm.member)) {
                ctx_.setExprType(
                    &nm, ctx_.primitiveTypeRef(bt->primitive, /*nullable=*/true));
                return;
            }
        }
    }
    if (!member) return;
    // §13.1 (issue #432): full visibility check — delegates to the shared
    // checkMemberVisibility helper (same four-level rule as resolveMemberAccess).
    if (auto* msym = dynamic_cast<MemberSymbol*>(member)) {
        checkMemberVisibility(msym, nm.member, nm.span.begin);
    }
    ctx_.setExprSymbol(&nm, member);
    TypeRef* memberTy = nullptr;
    if (auto* fs = dynamic_cast<FieldSymbol*>(member)) {
        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl)) {
            if (fd->type) memberTy = ctx_.resolvedType(*fd->type);
        } else if (fs->posParam && fs->posParam->type) {
            // §9 / #464: positional record field — type lives on the Param,
            // not on a FieldDecl. resolveNullCondMember previously missed this
            // path and left memberTy null, so the NullCondMemberExpr's TypeRef
            // was never set. lowerCoalesce then saw no TypeRef on `b?.x` and
            // fell through to `return nullptr`, silently producing 0.
            memberTy = ctx_.resolvedType(*fs->posParam->type);
        }
    } else if (auto* ps = dynamic_cast<PropertySymbol*>(member)) {
        if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl))
            if (pd->type) memberTy = ctx_.resolvedType(*pd->type);
    }
    if (!memberTy) return;
    if (memberTy->nullable) {
        ctx_.setExprType(&nm, memberTy);  // already nullable
    } else {
        TypeRef* nn = ctx_.makeTypeRef(*memberTy);
        nn->nullable = true;
        ctx_.setExprType(&nm, nn);
    }
}

// ===========================================================================
// §30.14 / §3.6 (#600): sendability derived-type property.
// Implements the §30.14 table: value-type rule, string, function types,
// class "transfer-closed" rule, stdlib overrides (Task/Shared/Channel/…),
// OwnedArray/Array per-element rules. Coinductive via `inProgress`.
// ===========================================================================

// Helper: extract the builtin-name of a TypeRef whose declaration is null
// (i.e., a builtin Named TypeRef with an astNode carrying the type name).
static std::string_view builtinTypeName(const TypeRef* rt) {
    if (!rt || rt->kind != TypeRef::Kind::Named || rt->declaration)
        return {};
    if (rt->astNode && rt->astNode->name &&
        !rt->astNode->name->parts.empty())
        return rt->astNode->name->parts[0];
    return {};
}

// Helper: does the TypeRef represent a user-declared class (not builtin)?
static bool isUserClass(const TypeRef* rt) {
    return rt && rt->kind == TypeRef::Kind::Named &&
           rt->declaration &&
           rt->declaration->kind == ast::TypeDeclKind::Class;
}

// Helper: does the TypeRef represent a user-declared record?
static bool isUserRecord(const TypeRef* rt) {
    return rt && rt->kind == TypeRef::Kind::Named &&
           rt->declaration &&
           rt->declaration->kind == ast::TypeDeclKind::Record;
}


bool ExprTC::isSendable(const TypeRef* rt,
                         std::unordered_set<const TypeSymbol*>& inProgress) const {
    if (!rt) return false;
    if (rt->kind == TypeRef::Kind::Error) return false;

    // §30.14: function-typed values are NEVER sendable.
    if (rt->kind == TypeRef::Kind::Function) return false;

    // §30.14 / §3.6: Unit type is sendable (no state).
    if (rt->kind == TypeRef::Kind::Unit) return true;

    // §30.14 value-type rule:
    //   "Value types are sendable iff they are not borrow-bearing (§9.7)
    //    and not move-only (§15.3 [N owned]T)."
    //
    // Inline array `[N]T` (not owned) is a value type but its element
    // determines borrow-bearing; `[N owned]T` is move-only → not sendable.
    if (rt->kind == TypeRef::Kind::InlineArray) {
        // [N owned]T is move-only per §15.3 → not sendable.
        if (rt->arrayOwned) return false;
        // [N]T is a value array; sendable iff element is sendable-value or string.
        // (An inline array of class elements makes the array borrow-bearing.)
        if (!rt->arrayElement) return false;
        const TypeRef* elem = rt->arrayElement;
        // Element must be a primitive (non-class) or string to be sendable.
        if (elem->kind == TypeRef::Kind::Primitive) return true;
        if (isUserRecord(elem)) {
            // A record in an inline array: sendable iff not borrow-bearing.
            // We reuse the full isSendable check on the record.
            return isSendable(elem, inProgress);
        }
        return false;
    }

    // §30.14: string is sendable.
    if (rt->kind == TypeRef::Kind::Primitive) {
        if (rt->primitive == PrimitiveType::String) return true;
        // Object is NOT sendable (runtime type behind it is unverifiable).
        if (rt->primitive == PrimitiveType::Object) return false;
        // All other primitives (int, bool, char, byte, etc.) are sendable
        // value types.
        return true;
    }

    // Named types:
    if (rt->kind == TypeRef::Kind::Named) {
        // --- Stdlib-specific overrides (§30.14 stdlib table) ---
        std::string_view bname = builtinTypeName(rt);
        if (!bname.empty()) {
            // §30.14 stdlib table: NOT sendable (domain-local or borrow-carrying).
            //   Task<T>, OwnedTask<T>, Task (bare): domain-local.
            //   Weak<T>: v0.140 lifted Shared<T> handles but NOT Weak<T> — a
            //     Weak<T> across domains leaves TryUpgrade racing with the last
            //     strong drop on another domain; the CAS in TryUpgrade is safe
            //     but the spec text was conservative: only handles (Shared<T>)
            //     were lifted, not weak handles.  Weak stays domain-local.
            //   EventSource<T>, AsyncEventSource<T>: domain-local.
            //   Semaphore: sendable per the §6.2-Mutex precedent (internally
            //     synchronized; SemaphoreToken is the non-sendable guard like Lock).
            //     §6.3 is silent — docs sentence queued in the 0.141 batch (#691).
            //   Lock: held guard is domain-local; must be released in the acquiring
            //     domain (stdlib §6.2 v0.138 / #647).
            //   SemaphoreToken: held permit guard is domain-local (same reasoning
            //     as Lock; §6.3 v0.140 / #647).
            //   CancellationTokenSource, CancellationToken: borrow-bearing record.
            // NOTE: Mutex is NOT in this list — sendable per stdlib §6.2 v0.138
            //   / #647: internally synchronized, like Channel<T>.
            // NOTE: Semaphore is NOT in this list — sendable per stdlib §6.3
            //   v0.140 / #647: internally synchronized (same reasoning as Mutex).
            if (bname == "Task" || bname == "OwnedTask" ||
                bname == "Weak" ||
                bname == "EventSource" || bname == "AsyncEventSource" ||
                bname == "SemaphoreToken" ||
                bname == "Lock" ||
                bname == "CancellationTokenSource")
                return false;
            // CancellationToken is a borrow-bearing record per §30.14
            // ("CancellationToken is a borrow-bearing record and therefore
            //  not sendable by the value-type rule").
            if (bname == "CancellationToken") return false;

            // §28.13 v0.140 / #689: Shared<T> is sendable iff T is sendable AND
            // internally synchronized (compiler-known set: Mutex, Atomic<T>,
            // AtomicInteger<T>, Semaphore — per spec §28.13 sendability rule).
            // Shared<string>/Shared<UserClass>/etc. remain domain-local.
            // Prerequisite: the control block counts are now atomic (§689 stage 1).
            if (bname == "Shared") {
                if (rt->genericArgs.empty() || !rt->genericArgs[0])
                    return false;
                const TypeRef* T = rt->genericArgs[0];
                // T must be in the compiler-known internally-synchronized set.
                std::string_view tname = builtinTypeName(T);
                if (tname == "Mutex")           return true;
                if (tname == "Atomic")          return true;
                if (tname == "AtomicInteger")   return true;
                if (tname == "Semaphore")       return true;
                // All other T: domain-local, not sendable.
                return false;
            }

            // §30.14 stdlib table: sendable iff element sendable.
            //   OwnedArray<T>, OwnedStack<T>, OwnedQueue<T>, OwnedSet<T>,
            //   OwnedLinkedList<T>, OwnedDictionary<K,V>: owning collections.
            if (bname == "OwnedArray" || bname == "OwnedStack" ||
                bname == "OwnedQueue" || bname == "OwnedSet" ||
                bname == "OwnedLinkedList") {
                // Sendable iff element type is sendable.
                if (rt->genericArgs.empty() || !rt->genericArgs[0])
                    return false;
                return isSendable(rt->genericArgs[0], inProgress);
            }
            if (bname == "OwnedDictionary") {
                // Both K and V must be sendable.
                if (rt->genericArgs.size() < 2) return false;
                if (!rt->genericArgs[0] || !rt->genericArgs[1]) return false;
                return isSendable(rt->genericArgs[0], inProgress) &&
                       isSendable(rt->genericArgs[1], inProgress);
            }

            // §30.14 stdlib table: borrowing collections (Array<T>, Stack<T>, etc.)
            //   sendable iff element is a value-sendable type (primitive or string)
            //   or a sendable class. Per spec: "A borrowing collection of sendable
            //   value/string elements is sendable; a borrowing collection of class
            //   elements (borrow slots) is NOT sendable."
            if (bname == "Array" || bname == "Stack" || bname == "Queue" ||
                bname == "Set" || bname == "LinkedList") {
                if (rt->genericArgs.empty() || !rt->genericArgs[0])
                    return false;
                const TypeRef* elem = rt->genericArgs[0];
                // String element: sendable.
                if (elem->kind == TypeRef::Kind::Primitive &&
                    elem->primitive == PrimitiveType::String)
                    return true;
                // Primitive (non-Object) element: sendable.
                if (elem->kind == TypeRef::Kind::Primitive &&
                    elem->primitive != PrimitiveType::Object)
                    return true;
                // Class element: NOT sendable (array carries borrow slots).
                if (isUserClass(elem)) return false;
                // Record element: sendable iff not borrow-bearing.
                if (isUserRecord(elem)) return isSendable(elem, inProgress);
                return false;
            }

            // §30.14 / stdlib §6.9: Channel<T> and ChannelSender<T> are
            // themselves sendable (they carry ownership of the underlying
            // channel object — a move-transfer).
            if (bname == "Channel" || bname == "ChannelSender")
                return true;

            // §30.14 / stdlib §6.4 / #647: Atomic<T> and AtomicInteger<T> are
            // sendable — internally synchronized (all operations seq_cst per §6.4
            // v0.140); may be reached by multiple domains simultaneously. Ownership
            // transfers on move. This entry is the compiler-known companion to the
            // Shared<Atomic<T>> sendability check in the "Shared" branch above.
            if (bname == "Atomic" || bname == "AtomicInteger")
                return true;
            // §30.14 / stdlib §6.2 v0.138 / #647: Mutex is sendable —
            // internally synchronized; may be reached by multiple domains
            // simultaneously (like Channel<T>). Ownership transfers on move.
            if (bname == "Mutex")
                return true;

            // §30.14 / stdlib §6.3 v0.140 / #647: Semaphore is sendable —
            // internally synchronized (counting semaphore); same reasoning as
            // Mutex above. Ownership transfers on move.
            if (bname == "Semaphore")
                return true;

            // Other builtin types with no user TypeDecl: conservatively
            // not sendable unless on the explicit list above.
            return false;
        }

        // --- User-declared types ---
        TypeSymbol* decl = rt->declaration;
        if (!decl) return false;

        // Enum: value type → sendable.
        if (decl->kind == ast::TypeDeclKind::Enum) return true;

        // Interface: NOT sendable ("no interface-typed fields" rule
        // means interface types themselves cannot be verified).
        if (decl->kind == ast::TypeDeclKind::Interface) return false;

        // Record: sendable iff not borrow-bearing per §9.7 / §30.14.
        // A record is borrow-bearing if any field is a class/interface
        // reference (non-owned) or a function type. Already covered
        // by value-type rule.
        if (decl->kind == ast::TypeDeclKind::Record) {
            const auto* td =
                dynamic_cast<const ast::TypeDecl*>(decl->decl);
            if (!td) return true; // no decl → conservative: accept

            // Collect all fields and check each.
            auto fieldSendable = [&](const TypeRef* ft) -> bool {
                if (!ft) return true;
                // Function-typed fields: never sendable per §30.14.
                if (ft->kind == TypeRef::Kind::Function) return false;
                // Class-typed field that is NOT owned: borrow → not sendable.
                // (Record fields of class type are borrows by default; only
                // an `owned` field modifier makes it an ownership transfer,
                // but records cannot have owned fields per §9.7 / §15.3.)
                // NOTE: per §9.7, record fields of class type are always
                // borrows — so a class field makes the record borrow-bearing.
                if (ft->kind == TypeRef::Kind::Named && ft->declaration &&
                    ft->declaration->kind == ast::TypeDeclKind::Class)
                    return false;
                // Interface-typed field: not sendable.
                if (ft->kind == TypeRef::Kind::Named && ft->declaration &&
                    ft->declaration->kind == ast::TypeDeclKind::Interface)
                    return false;
                // Object-typed primitive: not sendable.
                if (ft->kind == TypeRef::Kind::Primitive &&
                    ft->primitive == PrimitiveType::Object)
                    return false;
                // Builtin non-sendable: defer to recursive check.
                if (ft->kind == TypeRef::Kind::Named && !ft->declaration)
                    return isSendable(ft, inProgress);
                // Nested record: recurse.
                if (ft->kind == TypeRef::Kind::Named && ft->declaration &&
                    ft->declaration->kind == ast::TypeDeclKind::Record)
                    return isSendable(ft, inProgress);
                // Primitive (non-Object) / string: sendable.
                if (ft->kind == TypeRef::Kind::Primitive)
                    return true;
                // InlineArray: defer.
                if (ft->kind == TypeRef::Kind::InlineArray)
                    return isSendable(ft, inProgress);
                return true; // conservative: accept unknown
            };

            // Check positional fields.
            for (const ast::Param& f : td->positionalFields) {
                if (f.type) {
                    const TypeRef* ft = ctx_.resolvedType(*f.type);
                    if (!fieldSendable(ft)) return false;
                }
            }
            // Check body fields and auto-properties.
            // In Bezel records, properties declared as `public Name: T;` are
            // auto-properties (PropertyDecl with bodyKind=Auto) — these are
            // semantically fields and must be checked for sendability (§30.14).
            for (const auto& m : td->members) {
                if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m.get())) {
                    if (fd->type) {
                        const TypeRef* ft = ctx_.resolvedType(*fd->type);
                        if (!fieldSendable(ft)) return false;
                    }
                } else if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m.get())) {
                    // Auto-property: check its type for sendability.
                    if (pd->bodyKind == ast::PropertyBodyKind::Auto && pd->type) {
                        const TypeRef* ft = ctx_.resolvedType(*pd->type);
                        if (!fieldSendable(ft)) return false;
                    }
                }
            }
            return true;
        }

        // Class: sendable iff "transfer-closed" per §30.14.
        if (decl->kind == ast::TypeDeclKind::Class) {
            // Coinductive: if already in progress, assume sendable
            // (break mutual recursion).
            if (inProgress.count(decl)) return true;
            inProgress.insert(decl);

            // Rule 1: must be `sealed` (or `sealed abstract` with all-sendable
            // closed hierarchy).
            bool isSealed = false;
            bool isAbstract = false;
            for (auto mod : decl->modifiers) {
                if (mod == "sealed") isSealed = true;
                if (mod == "abstract") isAbstract = true;
            }
            if (!isSealed) { inProgress.erase(decl); return false; }

            if (isAbstract) {
                // `sealed abstract`: sendable iff every concrete subclass in the
                // module is also sendable (coinductive over the closed hierarchy).
                // Collect subclasses: scan all TypeSymbols in the module whose
                // base-type chain includes this class.
                // Implementation: walk all symbols in context and find those that
                // have `decl` in their baseTypes chain.
                bool allSendable = true;
                for (const auto& sym : ctx_.symbols()) {
                    auto* sub = dynamic_cast<TypeSymbol*>(sym.get());
                    if (!sub || sub == decl) continue;
                    if (sub->kind != ast::TypeDeclKind::Class) continue;
                    // Check if sub is a direct or indirect subclass of decl.
                    bool isSubclass = false;
                    // Simple: check baseTypes directly (direct subclasses only
                    // for the sealed-abstract case, since in Bezel a sealed abstract
                    // hierarchy is always closed and shallow per §12.5).
                    for (TypeSymbol* base : sub->baseTypes) {
                        if (base == decl) { isSubclass = true; break; }
                    }
                    if (!isSubclass) continue;
                    // This is a direct subclass: check sendability.
                    auto* subTy = ctx_.makeTypeRef();
                    subTy->kind = TypeRef::Kind::Named;
                    subTy->declaration = sub;
                    if (!isSendable(subTy, inProgress)) {
                        allSendable = false;
                        break;
                    }
                }
                inProgress.erase(decl);
                return allSendable;
            }

            // Sealed (non-abstract) class: apply Rule 2 + Rule 3.
            // Walk fields directly from the TypeDecl to check owned modifiers.
            {
                const auto* td = dynamic_cast<const ast::TypeDecl*>(decl->decl);
                if (!td) { inProgress.erase(decl); return true; }

                // Helper: check one TypeRef + owned modifier pair.
                // Rule 2: class-typed fields must be `owned` AND the class sendable.
                // Rule 3: no function, Object, or interface-typed fields.
                auto checkField = [&](const TypeRef* ft, bool isOwned) -> bool {
                    if (!ft) return true;
                    if (ft->kind == TypeRef::Kind::Function) return false;
                    if (ft->kind == TypeRef::Kind::Primitive &&
                        ft->primitive == PrimitiveType::Object) return false;
                    if (ft->kind == TypeRef::Kind::Named && ft->declaration &&
                        ft->declaration->kind == ast::TypeDeclKind::Interface)
                        return false;
                    if (ft->kind == TypeRef::Kind::Named && ft->declaration &&
                        ft->declaration->kind == ast::TypeDeclKind::Class) {
                        // Class field: must be owned AND the class must be sendable.
                        if (!isOwned) return false;
                        return isSendable(ft, inProgress);
                    }
                    return isSendable(ft, inProgress);
                };

                // Walk own fields and auto-properties.
                // (Auto-properties in classes are stored as PropertyDecl with
                // bodyKind=Auto; they are semantically fields for §30.14.)
                auto walkMembersForSend = [&](const ast::TypeDecl* atd) -> bool {
                    for (const auto& m : atd->members) {
                        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m.get())) {
                            if (!fd->type) continue;
                            const TypeRef* ft = ctx_.resolvedType(*fd->type);
                            bool isOwned = false;
                            for (auto mod : fd->modifiers)
                                if (mod == "owned") { isOwned = true; break; }
                            if (!checkField(ft, isOwned)) return false;
                        } else if (auto* pd =
                                       dynamic_cast<const ast::PropertyDecl*>(m.get())) {
                            // Auto-properties (field-like) must also satisfy §30.14.
                            if (pd->bodyKind != ast::PropertyBodyKind::Auto ||
                                !pd->type) continue;
                            const TypeRef* ft = ctx_.resolvedType(*pd->type);
                            bool isOwned = false;
                            for (auto mod : pd->modifiers)
                                if (mod == "owned") { isOwned = true; break; }
                            if (!checkField(ft, isOwned)) return false;
                        }
                    }
                    return true;
                };

                if (!walkMembersForSend(td)) {
                    inProgress.erase(decl); return false;
                }
                // Walk inherited fields from base classes.
                for (TypeSymbol* base : decl->baseTypes) {
                    if (!base || base->kind != ast::TypeDeclKind::Class) continue;
                    const auto* btd = dynamic_cast<const ast::TypeDecl*>(base->decl);
                    if (!btd) continue;
                    if (!walkMembersForSend(btd)) {
                        inProgress.erase(decl); return false;
                    }
                }
            }
            inProgress.erase(decl);
            return true;
        }
    }
    return false; // unknown kind
}

// ===========================================================================
// §30.14 (#600): capture-free entry detection for Domain.Spawn.
// A "capture-free" entry is either:
//   (a) a static method reference `Type.Method` (MemberAccessExpr whose
//       target resolves to a TypeSymbol), or
//   (b) a non-capturing lambda (a LambdaExpr whose body does NOT reference
//       any local or parameter from the enclosing scope).
// ===========================================================================

bool ExprTC::isCaptureFreeEntry(const ast::Expr* entryExpr) const {
    if (!entryExpr) return false;

    // (a) Static method reference: MemberAccessExpr whose target is a type.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(entryExpr)) {
        // The target should resolve to a TypeSymbol for a static method ref.
        if (ma->target) {
            Symbol* tgtSym = ctx_.exprSymbol(*ma->target);
            if (dynamic_cast<TypeSymbol*>(tgtSym)) return true; // static ref
        }
        return false; // instance method ref
    }

    // (b) Non-capturing lambda.
    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(entryExpr)) {
        const std::vector<Symbol*>* paramSyms = ctx_.lambdaParams(lam);
        std::unordered_set<const Symbol*> ownParams;
        if (paramSyms) {
            for (const Symbol* ps : *paramSyms) ownParams.insert(ps);
        }
        return !lambdaBodyCaptures(lam, ownParams);
    }

    // Any other expression kind: not a recognized capture-free entry form.
    return false;
}

bool ExprTC::lambdaBodyCaptures(
    const ast::LambdaExpr* lam,
    const std::unordered_set<const Symbol*>& ownParamSyms) const {
    if (!lam) return false;
    if (lam->exprBody && exprCaptures(*lam->exprBody, ownParamSyms))
        return true;
    for (const auto& s : lam->stmts) {
        if (s && stmtCaptures(*s, ownParamSyms)) return true;
    }
    return false;
}

bool ExprTC::exprCaptures(const ast::Expr& e,
                           const std::unordered_set<const Symbol*>& ownParamSyms) const {
    // Check this node.
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        Symbol* sym = ctx_.exprSymbol(*id);
        if (!sym) return false;
        // Captured if it resolves to a local or parameter NOT in our own params.
        bool isLocal = dynamic_cast<LocalSymbol*>(sym) != nullptr;
        bool isParam = dynamic_cast<ParameterSymbol*>(sym) != nullptr;
        if ((isLocal || isParam) && !ownParamSyms.count(sym)) return true;
        return false;
    }
    // Recurse into children.
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if (be->left && exprCaptures(*be->left, ownParamSyms)) return true;
        if (be->right && exprCaptures(*be->right, ownParamSyms)) return true;
    } else if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (ue->operand && exprCaptures(*ue->operand, ownParamSyms)) return true;
    } else if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target && exprCaptures(*ma->target, ownParamSyms)) return true;
    } else if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        if (nm->target && exprCaptures(*nm->target, ownParamSyms)) return true;
    } else if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
        if (ce->callee && exprCaptures(*ce->callee, ownParamSyms)) return true;
        for (const auto& a : ce->args)
            if (a.value && exprCaptures(*a.value, ownParamSyms)) return true;
    } else if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        for (const auto& a : ne->args)
            if (a.value && exprCaptures(*a.value, ownParamSyms)) return true;
    } else if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        if (ix->target && exprCaptures(*ix->target, ownParamSyms)) return true;
        for (const auto& idx : ix->indices)
            if (idx.value && exprCaptures(*idx.value, ownParamSyms)) return true;
    } else if (auto* co = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        if (co->condition && exprCaptures(*co->condition, ownParamSyms)) return true;
        if (co->thenValue && exprCaptures(*co->thenValue, ownParamSyms)) return true;
        if (co->elseValue && exprCaptures(*co->elseValue, ownParamSyms)) return true;
    } else if (auto* innerLam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
        // A nested lambda: it has its OWN params; captures from its perspective
        // may or may not propagate. For Domain.Spawn capture check, a nested
        // lambda that captures outer scope IS a capture of the Spawn entry lambda.
        const std::vector<Symbol*>* innerParams = ctx_.lambdaParams(innerLam);
        std::unordered_set<const Symbol*> allHidden = ownParamSyms;
        if (innerParams) {
            for (const Symbol* ps : *innerParams) allHidden.insert(ps);
        }
        if (lambdaBodyCaptures(innerLam, allHidden)) return true;
    }
    return false;
}

bool ExprTC::stmtCaptures(const ast::Stmt& s,
                           const std::unordered_set<const Symbol*>& ownParamSyms) const {
    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(&s)) {
        if (rs->value && exprCaptures(*rs->value, ownParamSyms)) return true;
    } else if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(&s)) {
        if (as->target && exprCaptures(*as->target, ownParamSyms)) return true;
        if (as->value && exprCaptures(*as->value, ownParamSyms)) return true;
    } else if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s)) {
        if (es->expr && exprCaptures(*es->expr, ownParamSyms)) return true;
    } else if (auto* ld = dynamic_cast<const ast::LocalDecl*>(&s)) {
        if (ld->initializer && exprCaptures(*ld->initializer, ownParamSyms)) return true;
    } else if (auto* is = dynamic_cast<const ast::IfStmt*>(&s)) {
        // IfStmt: condition is in ifBranch.condition per ast::IfStmt::Branch.
        if (is->ifBranch.condition &&
            exprCaptures(*is->ifBranch.condition, ownParamSyms)) return true;
        for (const auto& sub : is->ifBranch.body)
            if (sub && stmtCaptures(*sub, ownParamSyms)) return true;
        for (const auto& elif : is->elifBranches) {
            if (elif.condition && exprCaptures(*elif.condition, ownParamSyms)) return true;
            for (const auto& sub : elif.body)
                if (sub && stmtCaptures(*sub, ownParamSyms)) return true;
        }
        for (const auto& sub : is->elseBody)
            if (sub && stmtCaptures(*sub, ownParamSyms)) return true;
    } else if (auto* wb = dynamic_cast<const ast::DoEndBlock*>(&s)) {
        for (const auto& sub : wb->body)
            if (sub && stmtCaptures(*sub, ownParamSyms)) return true;
    }
    return false;
}

} // namespace bezel::sema
