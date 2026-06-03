#include "sema/ExprTypecheck_impl.hpp"

namespace bezel::sema {

// This translation unit holds the call-resolution and member-access
// definitions of the ExprTC worker: CallExpr return-type / arity checking
// (typeCall and friends) and the `.` / `?.` member-access resolvers.

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
    // §22.6: arity-based overload resolution. The callee resolved to one
    // method of its name; when that one's parameter count doesn't match
    // this call's argument count, re-select the same-named overload
    // whose parameter count does. (Type-based tie-breaking between
    // equal-arity overloads is a later refinement.)
    if (auto* ms0 = dynamic_cast<MethodSymbol*>(calleeSym)) {
        if (ms0->parent && ms0->params.size() != ce.args.size()) {
            for (Symbol* m : ms0->parent->members) {
                auto* cand = dynamic_cast<MethodSymbol*>(m);
                if (cand && cand->name == ms0->name &&
                    cand->params.size() == ce.args.size()) {
                    ctx_.setExprSymbol(ce.callee.get(), cand);
                    calleeSym = cand;
                    break;
                }
            }
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
                        // per §3: Contains → bool, IndexOf → int, Distinct → Array<T>
                        else if (m == "Contains")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        else if (m == "IndexOf")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                        else if (m == "Distinct")
                            ctx_.setExprType(&ce, recvTy);
                        // per §5 LINQ: Any/All → bool, Count → int, Where/Select → Array<T>
                        else if (m == "Any" || m == "All")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Bool));
                        else if (m == "Count" && !ce.args.empty())
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
                        else if (m == "Where" || m == "Select" || m == "Skip" || m == "Take" ||
                                 m == "OrderBy" || m == "OrderByDescending")
                            ctx_.setExprType(&ce, recvTy);  // same Array<T> type
                        else if (m == "Sum" || m == "Min" || m == "Max")
                            ctx_.setExprType(&ce, ctx_.primitiveTypeRef(P::Int));
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

// Slice T7: check `new T(args)` against T's sole constructor (if
// any). Zero declared constructors → the implicit default ctor
// takes no arguments. Multiple constructors → overload resolution
// is deferred, so skip.
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
    if (ctors.size() > 1) return;  // overloaded — defer
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
