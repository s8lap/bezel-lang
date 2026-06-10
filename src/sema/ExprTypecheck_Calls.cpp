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

// §22.6 method-overload resolution; see header for contract.
MethodSymbol* ExprTC::resolveMethodOverload(MethodSymbol* initial,
                                            const ast::CallExpr& ce) {
    if (!initial || !initial->parent) return initial;

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
        for (Symbol* m : initial->parent->members) {
            auto* cand = dynamic_cast<MethodSymbol*>(m);
            if (!cand || cand->name != initial->name) continue;
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
    for (Symbol* m : initial->parent->members) {
        auto* cand = dynamic_cast<MethodSymbol*>(m);
        if (!cand || cand->name != initial->name) continue;
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
    if (cands.size() == 1) return cands[0];

    // Any argument carrying a call-site modifier (ref/out/...) is outside
    // the value-conversion ranking of §22.6's worked examples; fall back.
    for (const auto& a : ce.args)
        if (!a.modifier.empty() || a.isDiscard) return arityFallback();

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
    }
    if (auto* ts = dynamic_cast<TypeSymbol*>(calleeSym)) {
        if (ts->kind == ast::TypeDeclKind::Enum) {
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
                    // Build the argument-inferred substitution map: for each
                    // method generic param, find the first value parameter whose
                    // declared type is exactly that bare param name, and map
                    // the param to the corresponding call argument's sema type.
                    std::unordered_map<std::string_view, TypeRef*> argSubst;
                    for (const auto& gp : md->genericParams) {
                        for (std::size_t pi = 0;
                             pi < md->params.size() &&
                             pi < ce.args.size(); ++pi) {
                            const ast::Type* pt = md->params[pi].type.get();
                            if (!pt || pt->kind != ast::TypeKind::Named ||
                                !pt->name ||
                                pt->name->parts.size() != 1 ||
                                pt->name->parts[0] != gp.name)
                                continue;
                            if (!ce.args[pi].value) continue;
                            TypeRef* argTy = ctx_.exprType(*ce.args[pi].value);
                            if (argTy) { argSubst[gp.name] = argTy; break; }
                        }
                    }
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
            if (TypeRef* rt = ctx_.importedReturnType(ms))
                ctx_.setExprType(&ce, rt);
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
                    return false;
                };
                // stdlib §5: chained LINQ on a lazy IEnumerable<T> receiver.
                // The receiver carries no declaration/astNode (the lazy
                // sentinel) but does carry the element type as genericArgs[0].
                bool isLazyRecv = recvTy && recvTy->kind == TypeRef::Kind::Named &&
                    !recvTy->declaration && !recvTy->astNode &&
                    !recvTy->genericArgs.empty();
                if (isLazyRecv) {
                    TypeRef* srcElem = recvTy->genericArgs[0];
                    if (m == "Any" || m == "All")
                        ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                    else if (m == "Count" || m == "Sum" || m == "Min" || m == "Max")
                        ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                    // stdlib §5/§5.1: Average returns double (the int/long/double
                    // selector overloads and the per-type no-selector form all
                    // produce double; decimal is the lone exception, not modeled).
                    else if (m == "Average")
                        ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Double));
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
                        else if (m == "Get" || m == "Peek" || m == "Pop" || m == "Dequeue") {
                            // Return type = element type (first generic arg)
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
                        else if (m == "Contains" || m == "TryAdd")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        else if (m == "IndexOf")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                        // per §5 LINQ: Any/All → bool, Count(pred) → int
                        else if (m == "Any" || m == "All")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        else if (m == "Count" && !ce.args.empty())
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                        else if (m == "Sum" || m == "Min" || m == "Max")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                        // stdlib §5/§5.1: Average → double (int/long/double forms).
                        else if (m == "Average")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Double));
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
                    if (colName == "Array" && !recvTy->genericArgs.empty() &&
                        recvTy->genericArgs[0] &&
                        (m == "AsSpan" || m == "AsReadOnlySpan") &&
                        (ce.args.size() <= 2)) {
                        std::string_view spanName =
                            (m == "AsSpan") ? "ArraySpan" : "ReadOnlyArraySpan";
                        // Build a synthetic ArraySpan<T> / ReadOnlyArraySpan<T> TypeRef.
                        // Re-use the static-astNode pattern used by makeConcreteCollectionResult.
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
                        auto* tr = ctx_.makeTypeRef();
                        tr->kind = TypeRef::Kind::Named;
                        tr->declaration = nullptr;
                        tr->astNode = (m == "AsSpan") ? kArraySpanNode : kROArraySpanNode;
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
                    // issue #494 / §29.2 / §29.3: builtin IEnumerator<T> /
                    // OwnedEnumerator<T> call-expression methods.
                    //   MoveNext(): bool — advances the iterator (vtable slot 2)
                    // (Current is a readonly property, typed in resolveMemberAccess.)
                    if (colName == "IEnumerator" || colName == "OwnedEnumerator") {
                        if (m == "MoveNext" && ce.args.empty())
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        // Dispose() → unit; no type needed.
                    }
                    // Dictionary methods
                    if (colName == "Dictionary" || colName == "OwnedDictionary") {
                        if (m == "Count")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                        else if (m == "ContainsKey" || m == "Remove" || m == "TryGetValue")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
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
                    }
                    // File static methods
                    if (colName == "File") {
                        if (m == "ReadAllText")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::String));
                        else if (m == "Exists" || m == "Delete")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        // ReadAllLines → Array<string>; sema type set when callee
                        // is typed explicitly (var lines: Array<string> := ...).
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
                            m == "MinValue" || m == "MaxValue" || m == "FromTicks")
                            ctx_.setExprType(&ce, makeDTType());
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
                            auto* tt = makeTaskTypeRef();
                            if (argTy) tt->genericArgs.push_back(argTy);
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
                    // §4.15 (#549): ArraySpan<T> / ReadOnlyArraySpan<T> instance
                    // method return types.  The receiver carries an astNode-backed
                    // TypeRef (no declaration) so rcn == "ArraySpan" etc.
                    if (rcn == "ArraySpan" || rcn == "ReadOnlyArraySpan") {
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
                        } else if (m == "AsReadOnly" && rcn == "ArraySpan" &&
                                   !recvTy->genericArgs.empty() && recvTy->genericArgs[0]) {
                            // ArraySpan<T>.AsReadOnly() → ReadOnlyArraySpan<T>
                            static const ast::Type* kRONode = []() -> ast::Type* {
                                auto* qn = new ast::QualifiedName;
                                qn->parts.push_back("ReadOnlyArraySpan");
                                auto* t = new ast::Type;
                                t->kind = ast::TypeKind::Named;
                                t->name.reset(qn);
                                return t;
                            }();
                            auto* tr = ctx_.makeTypeRef();
                            tr->kind = TypeRef::Kind::Named;
                            tr->declaration = nullptr;
                            tr->astNode = kRONode;
                            tr->genericArgs.push_back(recvTy->genericArgs[0]);
                            ctx_.setExprType(&ce, tr);
                        }
                        // Fill/CopyFrom/CopyTo/Reverse → unit (void); no type set.
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
    if (dynamic_cast<const ast::NullCondMemberExpr*>(ce.callee.get())) {
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
    // Bail if the name has more than one method overload in the type.
    int sameName = 0;
    for (Symbol* m : ms->parent->members) {
        if (dynamic_cast<MethodSymbol*>(m) && m->name == ms->name) {
            ++sameName;
        }
    }
    if (sameName != 1) return;
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
        if (td->hasPositionalFields) return;
    }
    std::vector<ConstructorSymbol*> ctors;
    for (Symbol* m : t->members) {
        if (auto* cs = dynamic_cast<ConstructorSymbol*>(m)) {
            if (!cs->isStatic) ctors.push_back(cs);
        }
    }
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
    // §14.1 / §2.5 / §2.6 (issue #328 FINDING [2]; #428): constructing an
    // IMPORTED GENERIC type is not yet supported. Generics are monomorphized
    // per-instantiation (§14), so a generic template exports NO stable
    // `bz$<Type>$init`/`$ctor` symbol the importer could link against; the
    // importer would have to monomorphize the generic at each
    // `new ImportedGeneric<Args>(...)` use site exactly as it does a LOCAL
    // `new T<Args>(...)`. Sound construction needs importer-side per-instantiation
    // infrastructure that does not yet exist for imports — a generic CLASS always
    // carries a §28.3 disposal obligation dispatched through a vtable the importer
    // would have to emit locally; a generic RECORD needs per-instantiation
    // record-struct specialization (absent even single-module, so a non-`int`
    // field would silently miscompile). Both are larger features, deferred to a
    // follow-up (see #428). Reject the construction here with an honest diagnostic
    // instead of letting codegen emit a dangling external `bz$<Type>$init`
    // reference (a cryptic link error). (Imported generic STATIC METHODS and LOCAL
    // generic instantiation are unaffected — only imported generic-type `new`.)
    // The generic-RECORD case is gated separately at the NewExpr typecheck, since
    // records are not routed through this constructor check.
    if (!t->genericParams.empty()) {
        ctx_.diagnostics().report(
            Severity::Error, ne.span.begin,
            std::string("constructing an imported generic type (`new ") +
                std::string(t->name) +
                "<...>`) is not yet supported: a generic type is monomorphized "
                "per-instantiation (\xC2\xA7""14) and its body is not carried in "
                "the dependency's metadata, so it cannot be instantiated across "
                "a module boundary (\xC2\xA7""2.5)");
        return;
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
    // a LINQ source and is excluded.
    if (recvTy->declaration) return nullptr;
    return recvTy->genericArgs[0];
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
bool ExprTC::checkMemberVisibility(const MemberSymbol* msym,
                                   std::string_view memberName,
                                   SourceLocation loc) {
    if (!msym || !msym->parent) return false;
    TypeSymbol* declaring = msym->parent;
    const ast::Visibility vis = msym->visibility;

    // public — always allowed (§13.1).
    if (vis == ast::Visibility::Public || vis == ast::Visibility::None)
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
    if (!declared || !target || !parent) return declared;
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
    TypeRef* recvTy = ctx_.exprType(*target);
    if (!recvTy || recvTy->declaration != parent ||
        recvTy->genericArgs.empty()) {
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
    if (!recvTy || recvTy->declaration != parent || recvTy->genericArgs.empty())
        return declared;
    // Build the parent-param-name -> concrete-arg map, positional by index so
    // each param maps to the same-index argument (param `B` at genericParams[1]
    // -> recvTy->genericArgs[1]) rather than to the first argument.
    std::unordered_map<std::string_view, TypeRef*> classSubst;
    for (std::size_t gi = 0; gi < parent->genericParams.size() &&
                             gi < recvTy->genericArgs.size(); ++gi) {
        if (parent->genericParams[gi] && recvTy->genericArgs[gi])
            classSubst[parent->genericParams[gi]->name] =
                recvTy->genericArgs[gi];
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

    // Determine which type parameters are already bound by an argument: a
    // value parameter whose declared type is exactly the bare parameter name
    // (mirrors codegen's argument-inference loop and §14.1). These never need
    // a target — the argument solves them.
    std::unordered_set<std::string_view> paramNames;
    for (const auto& gp : md->genericParams) paramNames.insert(gp.name);
    std::unordered_set<std::string_view> boundByArg;
    for (const auto& p : md->params) {
        const ast::Type* pt = p.type.get();
        if (pt && pt->kind == ast::TypeKind::Named && pt->name &&
            pt->name->parts.size() == 1 && paramNames.count(pt->name->parts[0]))
            boundByArg.insert(pt->name->parts[0]);
    }

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
    // §7.4 (issue #418): member access (and method calls) on a NULLABLE value is
    // a compile error unless the compiler has narrowed it to non-null. This gate
    // is SCOPED TO SIMPLE VARIABLES — bare locals and parameters — exactly the
    // operands the compiler narrows (§7.4 "Narrowing applies to locals only").
    // Non-null knowledge comes from: a null guard (`if x <> null` / `is not
    // null` then-block, `if x = null` else-block, an early-return `= null`
    // guard), an assignment of a non-null value, a §12.7 type-narrowing, and the
    // all-paths join of an if (see the IfStmt handler). The safe forms stay
    // legal because they never reach this check with a bare-variable target:
    // `x?.M` (§7.5) is a distinct AST node (NullCondMemberExpr); `x!.M` (§7.8)
    // has a PostfixBangExpr (non-variable) target.
    //
    // DEFERRED (see issue #418 / §7.4): a NULLABLE FIELD or property access
    // (`this._c.M`, `obj.Field.M`) is NOT gated here. §7.4 resolves that case
    // only by copying the field to a local first (fields are un-narrowable for
    // the TOCTOU reason §7.4 states); enforcing it would reject the existing
    // field-guard idiom wholesale without a narrowing path to the guarded form.
    // The original unsoundness (#416) for the common simple-variable case is
    // closed; the field case remains a documented gap.
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
                if (pt == PrimitiveType::Char) {
                    if (mbr == "IsDigit" || mbr == "IsLetter" ||
                        mbr == "IsLetterOrDigit" || mbr == "IsWhiteSpace" ||
                        mbr == "IsUpper" || mbr == "IsLower" ||
                        mbr == "IsPunctuation" || mbr == "IsSymbol") {
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
                    if (mbr == "Concat" || mbr == "Join" || mbr == "Empty" ||
                        mbr == "From" || mbr == "Format" || mbr == "Repeat") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::String));
                        return;
                    }
                    // string.Compare → int (ordinal comparison result).
                    if (mbr == "Compare") {
                        ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
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
                if (sg == "Utf8") {
                    auto* encTy = ctx_.makeTypeRef();
                    encTy->kind = TypeRef::Kind::Named;
                    encTy->declaration = builtinClassSym("Encoding");
                    ctx_.setExprType(&ma, encTy);
                    return;
                }
                if (sg == "Utf16" || sg == "Utf32" || sg == "Ascii") {
                    ctx_.diagnostics().report(
                        Severity::Error, ma.span.begin,
                        "`Encoding." + std::string(sg) +
                        "` is not yet implemented; only `Encoding.Utf8` is "
                        "available (stdlib §3.2). The Utf16/Utf32/Ascii "
                        "GetBytes/GetString semantics are not specified, so "
                        "they are deferred rather than guessed");
                    return;
                }
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
                    "receiver of `Encoding.Utf8.GetBytes(...)` / "
                    "`.GetString(...)` (stdlib §3.2); binding it to a variable "
                    "or using it indirectly is not yet supported");
                return;
            }
        }
    }
    if (!targetType && ma.target) {
        auto* tt = ctx_.exprType(*ma.target);
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
                    (tt->astNode->name->parts[0] == "IEnumerator" ||
                     tt->astNode->name->parts[0] == "OwnedEnumerator");
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
                    cn == "Dictionary" || cn == "OwnedDictionary";
                bool hasCapacity = cn == "Array" || cn == "OwnedArray";
                bool ok = ma.member == "Count" ? hasCount : hasCapacity;
                if (ok) {
                    ctx_.setExprType(
                        &ma, ctx_.primitiveTypeRef(PrimitiveType::Int));
                    return;
                }
            }
        }
        // §4.15 (#549): ArraySpan<T>/ReadOnlyArraySpan<T> readonly properties.
        // Neither type has a user TypeSymbol, so the ordinary member-lookup path
        // never types these.  Handle Length and IsEmpty here.
        if (ma.target &&
            (ma.member == "Length" || ma.member == "IsEmpty")) {
            if (auto* tt = ctx_.exprType(*ma.target)) {
                bool isSpan = tt->kind == TypeRef::Kind::Named &&
                    !tt->declaration && tt->astNode && tt->astNode->name &&
                    !tt->astNode->name->parts.empty() &&
                    (tt->astNode->name->parts[0] == "ArraySpan" ||
                     tt->astNode->name->parts[0] == "ReadOnlyArraySpan");
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
                        ma.member == "ToInt32" || ma.member == "ToUInt32") {
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
                if (Symbol* ext = lookupExtensionMember(recvTy, ma.member)) {
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
        if (Symbol* ext = lookupExtensionMember(recvTy, ma.member)) {
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
            // Convert methods (§9 stdlib).
            if (cls == "Convert" || cls == "Math") {
                auto setInt = [&]{ ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Int)); };
                auto setLong = [&]{ ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Long)); };
                auto setDbl = [&]{ ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Double)); };
                auto setBool = [&]{ ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::Bool)); };
                auto setStr = [&]{ ctx_.setExprType(&ma, ctx_.primitiveTypeRef(PrimitiveType::String)); };
                if (m=="ToInt32"||m=="Abs"||m=="Sign") setInt();
                else if (m=="ToInt64") setLong();
                else if (m=="ToDouble"||m=="Sqrt"||m=="Pow"||m=="Log"||m=="Log2"||m=="Log10"||
                         m=="Ceiling"||m=="Floor"||m=="Round"||m=="Truncate"||
                         // §2.2: the spec constants are `Pi`, `E`, `Tau` (the
                         // legacy `PI` spelling is retained for the existing
                         // intrinsic-path test but is not a spec name).
                         m=="Pi"||m=="PI"||m=="E"||m=="Tau"||
                         m=="Fmod"||m=="IEEERemainder"||m=="Sin"||m=="Cos"||m=="Tan"||
                         m=="Asin"||m=="Acos"||m=="Atan"||m=="Atan2"||m=="Exp"||m=="Hypot"||
                         m=="Sinh"||m=="Cosh"||m=="Tanh") setDbl();
                // Min/Max/Clamp are overloaded (int or double) — leave untyped.
                else if (m=="ToBoolean") setBool();
                // §2.3: Convert.TryToInt32(value, out result): bool.
                else if (m=="TryToInt32") setBool();
                else if (m=="ToString") setStr();
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
        bool knownCompleteUserType =
            targetType->decl != nullptr &&
            (targetType->kind == ast::TypeDeclKind::Class ||
             targetType->kind == ast::TypeDeclKind::Record ||
             targetType->kind == ast::TypeDeclKind::Enum);
        // §8.1 / §8.7: universal members present on every type without a
        // MemberSymbol — never diagnose these as missing.
        const auto& mm = ma.member;
        bool isUniversalObjectMember =
            mm == "ToString" || mm == "Equals" || mm == "GetHashCode" ||
            mm == "GetType" || mm == "ReferenceEquals" || mm == "Dispose";
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
                       n == "StackOverflowException";
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
                !isBuiltinExInheritedMember) {
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

} // namespace bezel::sema
