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

    // Arity-based fallback used whenever §22.6 type resolution does not
    // apply (named args, modifier args, undecidable types, or a single
    // matching candidate): re-select the first same-name overload whose
    // parameter count matches the call's argument count, preserving the
    // prior first-wins behavior.
    auto arityFallback = [&]() -> MethodSymbol* {
        if (initial->params.size() == ce.args.size()) return initial;
        for (Symbol* m : initial->parent->members) {
            auto* cand = dynamic_cast<MethodSymbol*>(m);
            if (cand && cand->name == initial->name &&
                cand->params.size() == ce.args.size())
                return cand;
        }
        return initial;
    };

    // §22.6 applies only to all-positional calls. Named-argument binding
    // (§22.5) is a separate, deferred concern; bail to arity selection.
    for (const auto& a : ce.args)
        if (!a.name.empty()) return arityFallback();

    // Collect same-name candidates filtered by this call's arity. Default
    // parameters are not yet folded into §22.6 here (optional-param arity
    // is handled by checkArgsAgainstParams' min/max); restrict to exact
    // parameter-count matches so the candidate set is unambiguous on arity.
    std::vector<MethodSymbol*> cands;
    std::vector<const ast::MethodDecl*> decls;
    for (Symbol* m : initial->parent->members) {
        auto* cand = dynamic_cast<MethodSymbol*>(m);
        if (!cand || cand->name != initial->name) continue;
        if (cand->params.size() != ce.args.size()) continue;
        auto* md = dynamic_cast<const ast::MethodDecl*>(cand->decl);
        if (!md) continue;
        // Defensive: a candidate whose own arity (decl params) disagrees
        // with the symbol's recorded params is skipped.
        if (md->params.size() != ce.args.size()) continue;
        cands.push_back(cand);
        decls.push_back(md);
    }
    if (cands.empty()) return arityFallback();
    if (cands.size() == 1) return cands[0];

    // Any argument carrying a call-site modifier (ref/out/...) is outside
    // the value-conversion ranking of §22.6's worked examples; fall back.
    for (const auto& a : ce.args)
        if (!a.modifier.empty() || a.isDiscard) return arityFallback();

    // Build the argument-type and per-candidate parameter-type vectors.
    std::vector<const TypeRef*> argTypes(ce.args.size(), nullptr);
    for (std::size_t i = 0; i < ce.args.size(); ++i)
        if (ce.args[i].value) argTypes[i] = ctx_.exprType(*ce.args[i].value);

    std::vector<std::vector<const TypeRef*>> paramTypes(cands.size());
    std::vector<std::string> candNames(cands.size());
    for (std::size_t c = 0; c < cands.size(); ++c) {
        const auto& params = decls[c]->params;
        paramTypes[c].resize(params.size(), nullptr);
        for (std::size_t i = 0; i < params.size(); ++i)
            if (params[i].type)
                paramTypes[c][i] = ctx_.resolvedType(*params[i].type);
        candNames[c] = methodSignatureString(initial->name, params);
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
    if (!md || !ms->parent) return;
    // Bail if any argument is named (named-arg matching is deferred).
    for (const auto& a : ce.args) {
        if (!a.name.empty()) return;
    }
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

// Slices T5/T6/T7: shared check of an all-positional argument list
// against a parameter list — argument count within [required,
// total] (optional params lower the minimum), then per-argument
// assignability. Callers must have already ruled out named args
// and overload ambiguity.
void ExprTC::checkArgsAgainstParams(const std::vector<ast::Param>& params,
                                    const std::vector<ast::CallArgument>& args,
                                    SourceLocation loc,
                                    const std::string& calleeDesc) {
    for (const auto& a : args) {
        if (!a.name.empty()) return;  // named-arg call — defer
    }
    std::size_t total = params.size();
    std::size_t required = 0;
    for (const auto& p : params) {
        if (!p.defaultValue) ++required;
    }
    std::size_t got = args.size();
    if (got < required || got > total) {
        ctx_.diagnostics().report(
            Severity::Error, loc,
            calleeDesc + " has " + std::to_string(got) +
            " argument(s) but expects " +
            (required == total
                 ? std::to_string(total)
                 : std::to_string(required) + "–" +
                       std::to_string(total)) +
            " (§22.5)");
        return;
    }
    for (std::size_t i = 0; i < got && i < total; ++i) {
        const ast::CallArgument& a = args[i];
        if (!a.value || !a.modifier.empty() || a.isDiscard) continue;
        const ast::Param& p = params[i];
        if (!p.type) continue;
        checkAssignable(ctx_.exprType(*a.value),
                        ctx_.resolvedType(*p.type),
                        a.value->span.begin,
                        std::string("argument ") +
                        std::to_string(i + 1) + " to " + calleeDesc);
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
        // §22.6 applies identically to constructor overload sets (§22.7).
        // Named-argument binding is deferred → fall back (no check) on a
        // named-arg `new`.
        for (const auto& a : ne.args)
            if (!a.name.empty()) return;
        // Candidates filtered by this call's arity (all-positional). A
        // call-site modifier on an argument is outside §22.6's ranking;
        // skip resolution in that case.
        for (const auto& a : ne.args)
            if (!a.modifier.empty() || a.isDiscard) return;
        std::vector<const ast::ConstructorDecl*> decls;
        for (ConstructorSymbol* cs : ctors) {
            auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
            if (cd && cd->params.size() == ne.args.size()) decls.push_back(cd);
        }
        if (decls.empty()) return;  // no same-arity ctor — defer (no check)
        if (decls.size() == 1) {
            checkArgsAgainstParams(decls[0]->params, ne.args, ne.span.begin,
                                   std::string("`new ") +
                                       std::string(t->name) + "`");
            return;
        }
        std::vector<const TypeRef*> argTypes(ne.args.size(), nullptr);
        for (std::size_t i = 0; i < ne.args.size(); ++i)
            if (ne.args[i].value)
                argTypes[i] = ctx_.exprType(*ne.args[i].value);
        std::vector<std::vector<const TypeRef*>> paramTypes(decls.size());
        std::vector<std::string> candNames(decls.size());
        for (std::size_t c = 0; c < decls.size(); ++c) {
            const auto& params = decls[c]->params;
            paramTypes[c].resize(params.size(), nullptr);
            for (std::size_t i = 0; i < params.size(); ++i)
                if (params[i].type)
                    paramTypes[c][i] = ctx_.resolvedType(*params[i].type);
            candNames[c] = std::string("new ") + std::string(t->name) +
                           methodSignatureString("", params);
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
        // empty case really is the no-arg default.)
        for (const auto& a : ne.args) {
            if (!a.name.empty()) return;
        }
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

void ExprTC::resolveMemberAccess(const ast::MemberAccessExpr& ma) {
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
        }
        // Slice 3d intentionally does NOT diagnose unknown members.
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
    // Attach the member's type for field / property access. Methods
    // and constructors are deferred to the call-resolution slice.
    if (auto* fs = dynamic_cast<FieldSymbol*>(member)) {
        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
        if (fd && fd->type) {
            if (auto* rt = ctx_.resolvedType(*fd->type)) {
                ctx_.setExprType(&ma, rt);
            }
        } else if (fs->posParam && fs->posParam->type) {
            // §9: positional record field — type comes from the Param.
            if (auto* rt = ctx_.resolvedType(*fs->posParam->type)) {
                ctx_.setExprType(&ma, rt);
            }
        }
    } else if (auto* ps = dynamic_cast<PropertySymbol*>(member)) {
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        if (pd && pd->type) {
            if (auto* rt = ctx_.resolvedType(*pd->type)) {
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
