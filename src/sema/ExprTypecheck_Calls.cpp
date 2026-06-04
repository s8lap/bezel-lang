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
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(ce.callee.get())) {
            if (ma->target) {
                auto* recvTy = ctx_.exprType(*ma->target);
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
                // stdlib §5: type a deferred LINQ operator `m` whose source
                // element type is `srcElem`. ToArray materializes to Array<T>
                // (modeled here as the lazy element type carried on the call;
                // codegen builds the concrete owned Array<T>). Returns true if
                // `m` was a LINQ operator (typed) — caller falls through
                // otherwise.
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
                    else if (m == "First" || m == "Last" || m == "Single" ||
                             m == "ElementAt")
                        ctx_.setExprType(&ce, srcElem);
                    else if (m == "ToArray" || m == "ToSet")
                        // Materializers (stdlib §5). Typed as the lazy sentinel
                        // carrying the element type; codegen builds the concrete
                        // owned Array<T>/Set<T>. The result is bound to an
                        // explicitly-typed local in practice, which supplies the
                        // collection type for downstream member access.
                        ctx_.setExprType(&ce, makeLazyEnumerable(srcElem));
                    else if (typeLinqOperator(srcElem)) { /* typed */ }
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
                        else if (m == "Contains")
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
                        // per §5 LINQ: ToArray/ToSet materialize the (already
                        // eager, for an Array source) collection. Typed as the
                        // lazy sentinel carrying the element type.
                        else if ((m == "ToArray" || m == "ToSet") &&
                                 !recvTy->genericArgs.empty())
                            ctx_.setExprType(&ce, makeLazyEnumerable(recvTy->genericArgs[0]));
                        // per §5 LINQ: Where/Select/Skip/Take/Distinct/OrderBy
                        // are LAZY — they return a deferred IEnumerable<T>, not
                        // a materialized Array<T> (stdlib §5).
                        else if (!recvTy->genericArgs.empty() &&
                                 typeLinqOperator(recvTy->genericArgs[0])) {
                            /* typed as lazy IEnumerable<T> */
                        }
                        // Add/Push/Enqueue/Set/RemoveAt/Clear → unit (void)
                    }
                    // Dictionary methods
                    if (colName == "Dictionary" || colName == "OwnedDictionary") {
                        if (m == "Count")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                        else if (m == "ContainsKey" || m == "Remove" || m == "TryGetValue")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                    }
                    // StringBuilder methods
                    if (colName == "StringBuilder") {
                        if (m == "ToString")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::String));
                        else if (m == "Length" || m == "Count")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                        else if (m == "Append" || m == "AppendLine" || m == "Clear")
                            ctx_.setExprType(&ce, recvTy);  // returns same StringBuilder
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
                    // DateTime
                    if (colName == "DateTime" && (m == "Now" || m == "UtcNow"))
                        ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Long));
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
        // §6 / stdlib §5: Math.Abs and Math.Sign are polymorphic — the
        // return type matches the argument type, not always Int. If the
        // member access was typed as Int but the first argument is
        // float/double, override with Double so the string formatter and
        // type inference work correctly.
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
                        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                                ce.callee.get())) {
                            if (ma->member == "Abs" || ma->member == "Sign" ||
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
    bool elemLambda =
        m == "Where" || m == "Select" || m == "Count" || m == "Any" ||
        m == "All" || m == "First" || m == "Last" || m == "Single" ||
        m == "OrderBy" || m == "OrderByDescending" || m == "ForEach" ||
        m == "Min" || m == "Max" || m == "Sum";
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
    // Only a bare type-parameter reference needs substitution: an unresolved
    // Named type whose single name part matches one of parent's params.
    if (declared->kind != TypeRef::Kind::Named || declared->declaration)
        return declared;
    if (!declared->astNode || !declared->astNode->name ||
        declared->astNode->name->parts.size() != 1)
        return declared;
    std::string_view paramName = declared->astNode->name->parts[0];
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
    // Map the param name to its positional concrete arg.
    for (std::size_t gi = 0; gi < parent->genericParams.size() &&
                             gi < recvTy->genericArgs.size(); ++gi) {
        if (parent->genericParams[gi] &&
            parent->genericParams[gi]->name == paramName) {
            TypeRef* arg = recvTy->genericArgs[gi];
            if (!arg) return declared;
            // Preserve a `T?` annotation on the field through substitution.
            if (declared->nullable && !arg->nullable) {
                TypeRef* nn = ctx_.makeTypeRef(*arg);
                nn->nullable = true;
                return nn;
            }
            return arg;
        }
    }
    return declared;
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
    // through to the substituted type's nullable form).
    if (declared->kind == TypeRef::Kind::Named && !declared->declaration &&
        declared->astNode && declared->astNode->name &&
        declared->astNode->name->parts.size() == 1) {
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
    if (declared->kind == TypeRef::Kind::Named && declared->declaration &&
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
                    if (mbr == "IsNaN" || mbr == "IsInfinity") {
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
            }
        }
    }
    if (!targetType && ma.target) {
        auto* tt = ctx_.exprType(*ma.target);
        if (tt && tt->kind == TypeRef::Kind::Named) {
            targetType = tt->declaration;
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
            }
        }
    }
    if (!targetType) {
        // #370 / §4.8: `Dictionary<K,V>.Keys` is `ICollection<K>` and
        // `.Values` is `ICollection<V>`. The Dictionary has no user
        // TypeSymbol (its TypeRef carries no declaration), so the ordinary
        // member-lookup path never types these properties. Type the access as
        // a collection-of-K (resp. V) carried structurally — a Named TypeRef
        // with the element type as genericArgs[0]. That lets a `for var k in
        // d.Keys` loop infer the loop variable's type as K (so `k.field`
        // resolves over a record key). Codegen recognizes the `d.Keys` AST
        // shape directly (issue #370), so it does not depend on this type.
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
                    auto* coll = ctx_.makeTypeRef();
                    coll->kind = TypeRef::Kind::Named;
                    coll->declaration = nullptr;
                    coll->astNode = nullptr;
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
                    // §9.6: Split returns Array<string> — no simple TypeRef,
                    // but suppress the diagnostic; codegen handles it.
                    if (ma.member == "Split") return;
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
                           ma.member == "IsFinite") {
                    // §6.3: FP predicates — bool result.
                    ctx_.setExprType(
                        &ma, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                    return;
                } else if (ma.member == "Abs") {
                    // §5: Abs on numeric primitive — same type.
                    ctx_.setExprType(&ma, ctx_.primitiveTypeRef(tt->primitive));
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
                         m=="PI"||m=="E"||
                         m=="Fmod"||m=="IEEERemainder"||m=="Sin"||m=="Cos"||m=="Tan"||
                         m=="Asin"||m=="Acos"||m=="Atan"||m=="Atan2"||m=="Exp"||m=="Hypot"||
                         m=="Sinh"||m=="Cosh"||m=="Tanh") setDbl();
                // Min/Max/Clamp are overloaded (int or double) — leave untyped.
                else if (m=="ToBoolean") setBool();
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
        if (knownCompleteUserType && !isUniversalObjectMember) {
            ctx_.diagnostics().report(
                Severity::Error, ma.span.begin,
                std::string("no member `") + std::string(ma.member) +
                    "` on type `" + std::string(targetType->name) +
                    "` (§11.3, §12.1)");
        }
        return;
    }
    // Slice S8: visibility check. A `private` member is accessible
    // only from inside the declaring type. Walk the member's
    // parent TypeSymbol; if it isn't currentType_, diagnose.
    // Other visibilities (internal/protected/public) are deferred.
    // Exception: if there are also public members with the same name
    // (overloads), suppress the error — the codegen picks by arity.
    if (auto* msym = dynamic_cast<MemberSymbol*>(member)) {
        if (msym->visibility == ast::Visibility::Private &&
            msym->parent && msym->parent != currentType_) {
            // Check if there is any public member with the same name
            // in the same type — if so, this is an overload set and
            // the codegen will select the accessible one.
            bool hasPublicOverload = false;
            for (Symbol* sib : msym->parent->members) {
                if (sib && sib != member && sib->name == member->name) {
                    if (auto* sm = dynamic_cast<MemberSymbol*>(sib)) {
                        if (sm->visibility != ast::Visibility::Private)
                            hasPublicOverload = true;
                    }
                }
            }
            if (!hasPublicOverload) {
                ctx_.diagnostics().report(
                    Severity::Error, ma.span.begin,
                    std::string("`") + std::string(ma.member) +
                    "` is private to type `" +
                    std::string(msym->parent->name) +
                    "` and not accessible from `" +
                    std::string(currentType_ ? currentType_->name
                                             : std::string_view("(global)")) +
                    "` (§13.1 — `private` is visible to the declaring "
                    "class only)");
            }
        }
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
    } else if (dynamic_cast<EnumConstantSymbol*>(member) && targetType) {
        // §10: an enum-constant access `E.Green` has the enum type, so a
        // local inferred from it (`var e := E.Green`) and a `case`
        // scrutinee both carry the enum type.
        TypeRef* et = ctx_.makeTypeRef();
        et->kind = TypeRef::Kind::Named;
        et->declaration = targetType;
        ctx_.setExprType(&ma, et);
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
    if (!targetType) return;
    Symbol* member = lookupMember(targetType, nm.member);
    if (!member) return;
    if (auto* msym = dynamic_cast<MemberSymbol*>(member)) {
        if (msym->visibility == ast::Visibility::Private &&
            msym->parent && msym->parent != currentType_) {
            // Suppress error if a non-private sibling with the same name
            // exists (overload set) — codegen will pick the accessible one.
            bool hasPublicOverload = false;
            for (Symbol* sib : msym->parent->members) {
                if (sib && sib != member && sib->name == member->name) {
                    if (auto* sm = dynamic_cast<MemberSymbol*>(sib)) {
                        if (sm->visibility != ast::Visibility::Private)
                            hasPublicOverload = true;
                    }
                }
            }
            if (!hasPublicOverload) {
                ctx_.diagnostics().report(
                    Severity::Error, nm.span.begin,
                    std::string("`") + std::string(nm.member) +
                    "` is private to type `" +
                    std::string(msym->parent->name) +
                    "` and not accessible from `" +
                    std::string(currentType_ ? currentType_->name
                                             : std::string_view("(global)")) +
                    "` (§13.1 — `private` is visible to the declaring "
                    "class only)");
            }
        }
    }
    ctx_.setExprSymbol(&nm, member);
    TypeRef* memberTy = nullptr;
    if (auto* fs = dynamic_cast<FieldSymbol*>(member)) {
        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl))
            if (fd->type) memberTy = ctx_.resolvedType(*fd->type);
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
