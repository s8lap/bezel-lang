#include "sema/ExprTypecheck_impl.hpp"

namespace bezel::sema {

namespace {

// §5: parse an integer literal's magnitude (ignoring base prefix,
// underscores, and any type suffix). Returns true and the value via `out`
// when it parses; saturates large hex/decimal in the rare overflow case
// (which still exceeds int and is reported). Mirrors the codegen parser.
bool intLiteralMagnitude(std::string_view text, unsigned long long& out) {
    int base = 10;
    std::size_t i = 0;
    if (text.size() >= 2 && text[0] == '0') {
        char b = text[1];
        if (b == 'x' || b == 'X') { base = 16; i = 2; }
        else if (b == 'b' || b == 'B') { base = 2; i = 2; }
        else if (b == 'o' || b == 'O') { base = 8; i = 2; }
    }
    std::string digits;
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (c == '_') continue;
        bool isDigit = (c >= '0' && c <= '9');
        bool isHex =
            isDigit || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (base == 16 ? !isHex : !isDigit) break;  // stop at suffix letters
        digits.push_back(c);
    }
    if (digits.empty()) return false;
    errno = 0;
    out = std::strtoull(digits.c_str(), nullptr, base);
    return true;
}

}  // namespace

void ExprTC::visitExpr(const ast::Expr& e) {
    // Slice 3b: IdentifierExpr name lookup. `this` / `base` are
    // implicit-meaning identifiers — leave them unresolved
    // (typecheck handles their semantics in a future slice).
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        if (id->name == "this" || id->name == "base") {
            if (inStaticMember_) {
                ctx_.diagnostics().report(
                    Severity::Error, id->span.begin,
                    std::string("`") + std::string(id->name) +
                    "` is not available in a static method or "
                    "static constructor — it requires an instance "
                    "receiver (§12.3)");
            }
            // Slice S29 (§22.8): a field initializer may not
            // reference `this` / `base`.
            if (inFieldInitializer_) {
                ctx_.diagnostics().report(
                    Severity::Error, id->span.begin,
                    std::string("a field initializer may not "
                                "reference `") +
                    std::string(id->name) + "` (§22.8)");
            }
            return;
        }
        Symbol* sym = lookupName(id->name);
        if (sym) {
            ctx_.setExprSymbol(&e, sym);
            // Slice S29 (§22.8): a field initializer may not observe
            // an instance member (implicit-`this` reference).
            if (inFieldInitializer_) checkNotInstanceMember(*id, sym);
            // Slice 3d: also attach the symbol's resolved type to
            // the identifier expr so MemberAccessExpr can pick it
            // up when this identifier is the target.
            if (auto* loc = dynamic_cast<LocalSymbol*>(sym)) {
                // Slice 3g: prefer the LocalSymbol's resolved type
                // (which already accounts for inference) over re-
                // querying the LocalDecl's ascription.
                if (loc->declType) {
                    ctx_.setExprType(&e, loc->declType);
                }
            } else if (auto* par = dynamic_cast<ParameterSymbol*>(sym)) {
                // Slice 3e: ParameterSymbol::declType points at the
                // parameter's Type ast (set by symbol collection).
                // Look up its resolved TypeRef and attach.
                if (par->declType) {
                    if (auto* rt = ctx_.resolvedType(*par->declType)) {
                        ctx_.setExprType(&e, rt);
                    }
                } else if (par->resolvedDeclType) {
                    // per §24.3: a context-typed lambda param (event handler).
                    ctx_.setExprType(&e, par->resolvedDeclType);
                }
            } else if (auto* fs = dynamic_cast<FieldSymbol*>(sym)) {
                // Implicit `this.<field>` form: bare identifier
                // resolving to a class field. Same field-type rule
                // as MemberAccessExpr below.
                if (auto* fd =
                        dynamic_cast<const ast::FieldDecl*>(fs->decl)) {
                    if (fd->type) {
                        if (auto* rt = ctx_.resolvedType(*fd->type)) {
                            ctx_.setExprType(&e, rt);
                        }
                    }
                }
            }
        } else if (primitiveFromName(id->name) != PrimitiveType::None) {
            // §5.2: a primitive type name used in expression position is
            // a conversion callee (`int(x)`). It has no value symbol;
            // the enclosing call resolves the conversion (typeCall).
        } else if (id->name == "Console" || id->name == "Math" ||
                   id->name == "Convert" || id->name == "Environment" ||
                   id->name == "Random" ||
                   // §3 stdlib collection and helper classes:
                   id->name == "Array" || id->name == "OwnedArray" ||
                   id->name == "Dictionary" || id->name == "OwnedDictionary" ||
                   id->name == "Stack" || id->name == "OwnedStack" ||
                   id->name == "Queue" || id->name == "OwnedQueue" ||
                   id->name == "Set" || id->name == "OwnedSet" ||
                   id->name == "LinkedList" || id->name == "OwnedLinkedList" ||
                   id->name == "StringBuilder" || id->name == "Encoding" ||
                   id->name == "DateTime" || id->name == "TimeSpan" ||
                   // §7 System.IO:
                   id->name == "File" || id->name == "Directory" ||
                   id->name == "Path" ||
                   // §4 Linq:
                   id->name == "Linq") {
            // §2.3: these System classes are auto-imported and always
            // available without an explicit import. Give them a synthetic
            // TypeSymbol (null decl) so member access can proceed; the
            // codegen handles known methods (Write/WriteLine, etc.)
            // as intrinsics.
            auto* ts = builtinClassSym(id->name);
            ctx_.setExprSymbol(&e, ts);
        } else if (currentScope_) {
            ctx_.diagnostics().report(
                Severity::Error, id->span.begin,
                std::string("unresolved identifier `") +
                std::string(id->name) + "`");
        }
        return;
    }
    // Slice 3 main contract: every literal gets a type.
    if (auto* il = dynamic_cast<const ast::IntLiteralExpr*>(&e)) {
        // §5: an integer literal's type comes from its suffix when
        // present (`L` → long, `u` → uint, `uL`/`Lu` → ulong), else
        // int. Context (target) typing of unsuffixed literals is a
        // separate, broader change (tracked in the literal-typing
        // issue); this covers the explicit-suffix forms.
        bool hasL = false, hasU = false;
        for (char c : il->text) {
            if (c == 'l' || c == 'L') hasL = true;
            if (c == 'u' || c == 'U') hasU = true;
        }
        PrimitiveType pt = PrimitiveType::Int;
        if (hasL && hasU) pt = PrimitiveType::ULong;
        else if (hasL) pt = PrimitiveType::Long;
        else if (hasU) pt = PrimitiveType::UInt;
        // §5: an unsuffixed integer literal must fit in `int`; if it does
        // not, it either requires a suffix OR the declared target type
        // (long/ulong/uint) allows implicit widening (issue #184).
        if (pt == PrimitiveType::Int) {
            unsigned long long mag = 0;
            if (intLiteralMagnitude(il->text, mag) &&
                (errno == ERANGE || mag > 2147483647ull)) {
                using P = PrimitiveType;
                bool widened = false;
                // Widen to the declared type when in a typed variable context.
                if (currentLiteralWideningHint_ != P::None) {
                    bool fits = false;
                    switch (currentLiteralWideningHint_) {
                        case P::Long:
                            fits = (errno != ERANGE && mag <= 9223372036854775807ULL);
                            break;
                        case P::ULong:
                            fits = (errno != ERANGE);  // any positive value fits
                            break;
                        case P::UInt:
                            fits = (errno != ERANGE && mag <= 4294967295ULL);
                            break;
                        default: break;
                    }
                    if (fits) {
                        pt = currentLiteralWideningHint_;
                        widened = true;
                    }
                }
                if (!widened) {
                    ctx_.diagnostics().report(
                        Severity::Error, il->span.begin,
                        "integer literal does not fit in `int`; add a suffix "
                        "(`L` for long, `U`/`UL` for unsigned) to widen it "
                        "(§5)");
                }
            }
        }
        ctx_.setExprType(&e, ctx_.primitiveTypeRef(pt));
        return;
    }
    if (auto* fl = dynamic_cast<const ast::FloatLiteralExpr*>(&e)) {
        // §18.1: the `f`/`F` suffix gives type `float` (f32); `d`/`D`,
        // `m`/`M`, or no suffix gives `double` (f64) / decimal. Currently
        // only float and double are codegen-backed.
        PrimitiveType pt = PrimitiveType::Double;
        if (!fl->text.empty()) {
            char last = fl->text.back();
            if (last == 'f' || last == 'F') pt = PrimitiveType::Float;
            // `d`/`D` → double (already default); `m`/`M` → decimal
            // (not yet implemented, leave as double for now).
        }
        ctx_.setExprType(&e, ctx_.primitiveTypeRef(pt));
        return;
    }
    if (dynamic_cast<const ast::CharLiteralExpr*>(&e)) {
        ctx_.setExprType(&e, ctx_.primitiveTypeRef(PrimitiveType::Char));
        return;
    }
    if (dynamic_cast<const ast::StringLiteralExpr*>(&e) ||
        dynamic_cast<const ast::VerbatimStringExpr*>(&e)) {
        ctx_.setExprType(
            &e, ctx_.primitiveTypeRef(PrimitiveType::String));
        return;
    }
    if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(&e)) {
        // §18.5: visit embedded expression parts BEFORE returning, so
        // their identifiers resolve and their types are set for codegen.
        for (const auto& part : is->parts)
            if (part.expr) visitExpr(*part.expr);
        ctx_.setExprType(
            &e, ctx_.primitiveTypeRef(PrimitiveType::String));
        return;
    }
    if (dynamic_cast<const ast::BoolLiteralExpr*>(&e)) {
        ctx_.setExprType(&e, ctx_.primitiveTypeRef(PrimitiveType::Bool));
        return;
    }
    if (dynamic_cast<const ast::NullLiteralExpr*>(&e)) {
        // null is context-typed; absent the context we annotate with
        // nullable Object. Later passes refine when there's a target
        // type to constrain against.
        ctx_.setExprType(
            &e, ctx_.primitiveTypeRef(PrimitiveType::Object,
                                      /*nullable=*/true));
        return;
    }
    if (dynamic_cast<const ast::UnitExpr*>(&e)) {
        // Build a unit TypeRef on first use (no primitive entry).
        auto* unit = ctx_.makeTypeRef();
        unit->kind = TypeRef::Kind::Unit;
        ctx_.setExprType(&e, unit);
        return;
    }
    // For non-literal expressions, recurse into children so their
    // own literals get typed. The expression itself may also get a
    // type from the operator's result rules (slice 3c).
    visitChildren(e);
    // Slice 3c: binary / unary operator result typing.
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        typeBinary(*be);
    }
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        typeUnary(*ue);
    }
    // §17.12 / §2553: the postfix `!` (null-forgiving) operator yields
    // the non-nullable form of its operand's type. Recording it lets
    // member access through `x!` (and any other consumer) resolve —
    // without it, `x!.member` reads/writes silently fail (#206).
    if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        if (bg->operand) {
            if (TypeRef* ot = ctx_.exprType(*bg->operand)) {
                if (ot->nullable) {
                    TypeRef* nn = ctx_.makeTypeRef(*ot);
                    nn->nullable = false;
                    ctx_.setExprType(&e, nn);
                } else {
                    ctx_.setExprType(&e, ot);  // already non-nullable
                }
            }
        }
    }
    // §20: the `if` expression `x if c else y` has the common type of
    // its two branches. Take the then-branch's type, falling back to
    // the else-branch (e.g. when the then-branch is a bottom-typed
    // `throw`). Without this the expression is untyped, so member
    // access on its result (`(a if c else b).M`) fails to resolve.
    if (auto* cnd = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        TypeRef* tt =
            cnd->thenValue ? ctx_.exprType(*cnd->thenValue) : nullptr;
        TypeRef* et =
            cnd->elseValue ? ctx_.exprType(*cnd->elseValue) : nullptr;
        if (tt) ctx_.setExprType(&e, tt);
        else if (et) ctx_.setExprType(&e, et);
    }
    // §17.4: `is` / `is not` yield bool; `as` yields the nullable form
    // of the target type (a failed cast is null). Recording it lets
    // `(x as T)` — and member access on its result — resolve.
    if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
        if (te->op == "is" || te->op == "is not") {
            ctx_.setExprType(&e,
                             ctx_.primitiveTypeRef(PrimitiveType::Bool));
        } else if (te->op == "as" && te->testType) {
            if (TypeRef* tt = ctx_.resolvedType(*te->testType)) {
                if (tt->nullable) {
                    ctx_.setExprType(&e, tt);
                } else {
                    TypeRef* nn = ctx_.makeTypeRef(*tt);
                    nn->nullable = true;
                    ctx_.setExprType(&e, nn);
                }
            }
        }
    }
    // §19: a `case` expression's type is the common type of its arm
    // bodies — take the first expression-bodied arm's type. Without it,
    // member access on a case result (`(case … end).M`) fails to
    // resolve.
    if (auto* cse = dynamic_cast<const ast::CaseExpr*>(&e)) {
        for (const auto& arm : cse->arms) {
            if (!arm.exprBody) continue;
            if (TypeRef* at = ctx_.exprType(*arm.exprBody)) {
                ctx_.setExprType(&e, at);
                break;
            }
        }
    }
    // §15.1: IndexExpr `a[i]` — the expression type is the array's
    // element type, so member access on array elements (`a[i].field`)
    // can resolve. The element type comes from the target's InlineArray
    // TypeRef, or from an indexer-property return type (deferred).
    // §4.6: string indexer `s[i]` → char (code point).
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        if (ix->target) {
            if (auto* tt = ctx_.exprType(*ix->target)) {
                if (tt->kind == TypeRef::Kind::InlineArray && tt->arrayElement)
                    ctx_.setExprType(&e, tt->arrayElement);
                else if (tt->kind == TypeRef::Kind::Primitive &&
                         tt->primitive == PrimitiveType::String)
                    ctx_.setExprType(&e, ctx_.primitiveTypeRef(PrimitiveType::Char));
                // issue #388: heap-collection indexer `a[i]` / `dict[key]`. The
                // built-in collections resolve to a Named TypeRef with a null
                // declaration (TypeResolution_Names) and carry their type
                // arguments in genericArgs. Type the index access as the
                // collection's element/result type so codegen can lower the
                // read and the setter against it (mirrors the inline-array path
                // above):
                //   * Array<T> / OwnedArray<T> (stdlib §4.4 / §4.5): the
                //     indexer `[i: int]: T` result is T = genericArgs[0].
                //   * Dictionary<K,V> / OwnedDictionary<K,V> (§4.8 / §4.9): the
                //     indexer `[key: K]: V` result is V = genericArgs[1].
                else if (tt->kind == TypeRef::Kind::Named && !tt->declaration &&
                         tt->astNode && tt->astNode->name &&
                         !tt->astNode->name->parts.empty()) {
                    std::string_view cn = tt->astNode->name->parts[0];
                    bool isArrLike =
                        cn == "Array" || cn == "OwnedArray";
                    bool isDict =
                        cn == "Dictionary" || cn == "OwnedDictionary";
                    if (isArrLike && !tt->genericArgs.empty() &&
                        tt->genericArgs[0])
                        ctx_.setExprType(&e, tt->genericArgs[0]);
                    else if (isDict && tt->genericArgs.size() > 1 &&
                             tt->genericArgs[1])
                        ctx_.setExprType(&e, tt->genericArgs[1]);
                }
            }
        }
    }
    // Slice 3d: MemberAccessExpr resolution. Runs after the target
    // is visited (so its TypeRef is available), looks the member up
    // on the target's TypeSymbol, and annotates the access with the
    // resolved member symbol and field/property type.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        resolveMemberAccess(*ma);
    }
    // §17.5 null-conditional member access `x?.M`.
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        resolveNullCondMember(*nm);
    }
    // Slice 3f: NewExpr is typed as its constructed type. `default`
    // and `typeof` follow the same pattern (default(T) → T,
    // typeof(T) → Type per §17.1).
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        if (ne->type) {
            if (auto* rt = ctx_.resolvedType(*ne->type)) {
                ctx_.setExprType(&e, rt);
                // Slice S11: forbid `new` on abstract or static
                // class types. Interfaces and (purely declarative)
                // static-only types are also non-instantiable.
                if (rt->kind == TypeRef::Kind::Named && rt->declaration) {
                    TypeSymbol* t = rt->declaration;
                    bool isAbstract = false;
                    bool isStatic = false;
                    for (auto mod : t->modifiers) {
                        if (mod == "abstract") isAbstract = true;
                        if (mod == "static") isStatic = true;
                    }
                    if (isAbstract) {
                        ctx_.diagnostics().report(
                            Severity::Error, ne->span.begin,
                            std::string("cannot instantiate abstract "
                                        "class `") +
                            std::string(t->name) + "` with `new` "
                            "(§12.5)");
                    } else if (isStatic) {
                        ctx_.diagnostics().report(
                            Severity::Error, ne->span.begin,
                            std::string("cannot instantiate static "
                                        "class `") +
                            std::string(t->name) + "` with `new` "
                            "(§11.7)");
                    } else if (t->kind ==
                               ast::TypeDeclKind::Interface) {
                        ctx_.diagnostics().report(
                            Severity::Error, ne->span.begin,
                            std::string("cannot instantiate interface `") +
                            std::string(t->name) +
                            "` with `new` (§11.2)");
                    } else if (t->kind == ast::TypeDeclKind::Class) {
                        // Slice T7: constructor arity / arg types.
                        checkNewExprArgs(*ne, t);
                    }
                }
            }
        }
    }
    if (auto* de = dynamic_cast<const ast::DefaultExpr*>(&e)) {
        if (de->type) {
            if (auto* rt = ctx_.resolvedType(*de->type)) {
                ctx_.setExprType(&e, rt);
            }
        }
    }
    // Slice 3h: CallExpr return-type typing. When the callee
    // resolves to a MethodSymbol (via slice 3d's MemberAccessExpr
    // resolver or slice 3b's identifier resolver), the call's
    // exprType is the method's resolved return type. Overload
    // picking is not yet implemented — the first matching member
    // wins, which can give the wrong result when overloads have
    // distinct return types. Tightened with the overload-resolution
    // slice.
    if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
        typeCall(*ce);
    }
}

// §17.1 result-type table for binary operators. Slice 3c covers the
// common cases with no operator-overload resolution: equality and
// relational ops produce bool; arithmetic / shift / bitwise produce
// the left operand's type (when the operands type-match); logical
// ops `and` / `or` produce bool; `??` produces the non-nullable
// result type of the LHS; range ops produce a Range value (slice 3c
// leaves Range as Error).
// §25: the user-defined `static operator <op>` method that applies to a
// binary expression, if either operand's type (class or record)
// declares one. `=`/`<>` on classes are excluded (they invoke Equals,
// §25.2).
MethodSymbol* ExprTC::findBinaryOperator(const ast::BinaryExpr& be) {
    auto onType = [&](const ast::Expr* operand) -> MethodSymbol* {
        if (!operand) return nullptr;
        TypeSymbol* ts = memberTargetType(operand);
        if (!ts) return nullptr;
        // §25: user-defined `operator =` and `<>` on class types take
        // precedence over built-in reference equality — only skip when
        // no user-defined operator exists (handled by falling through).
        for (Symbol* m : ts->members) {
            auto* ms = dynamic_cast<MethodSymbol*>(m);
            if (ms && ms->isOperator && ms->operatorToken == be.op &&
                ms->params.size() == 2)
                return ms;
        }
        return nullptr;
    };
    if (auto* m = onType(be.left.get())) return m;
    return onType(be.right.get());
}

void ExprTC::typeBinary(const ast::BinaryExpr& be) {
    const auto& op = be.op;
    // §25: a user-defined operator overload takes precedence over the
    // built-in result-type rules; the expression is that method's call.
    if (op != "and" && op != "or" && op != "in") {
        if (MethodSymbol* opm = findBinaryOperator(be)) {
            ctx_.setExprSymbol(&be, opm);
            if (auto* od = dynamic_cast<const ast::OperatorDecl*>(opm->decl))
                if (od->returnType)
                    if (auto* rt = ctx_.resolvedType(*od->returnType))
                        ctx_.setExprType(&be, rt);
            return;
        }
    }
    // §9.2/§8.3: `=` / `<>` on a body record that is not auto-equality-bearing
    // and that declares no equality of its own is a compile error at the use
    // site. "A body record is **not** auto-equality-bearing — and `=` on it
    // without an explicit overload is a compile error — when it contains [an
    // explicit field, a side-effecting computed property, or a non-trivial
    // constructor body]." A user `=`/`<>` overload would already have been
    // selected by findBinaryOperator above (so we never reach here for those);
    // a record carrying a hand-written `Equals`/`GetHashCode` (but no operator)
    // is likewise excluded via recordHasUserDeclaredEquality. Positional
    // records, enums, and eligible body records are unaffected.
    if (op == "=" || op == "<>") {
        for (const ast::Expr* operand : {be.left.get(), be.right.get()}) {
            TypeSymbol* ts = memberTargetType(operand);
            if (ts && ts->kind == ast::TypeDeclKind::Record &&
                !isAutoEqualityBearingBodyRecord(ts) &&
                !recordHasUserDeclaredEquality(ts)) {
                // §9.3: skip positional / combined-form records (they get §9.1
                // positional value equality, not the body-record rule).
                bool positional = false;
                if (auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl))
                    positional = td->hasPositionalFields;
                if (!positional) {
                    ctx_.diagnostics().report(
                        Severity::Error, be.span.begin,
                        "`" + std::string(op) + "` is not available on record `" +
                        std::string(ts->name) +
                        "`: it is not auto-equality-bearing (§9.2). Declare `=`"
                        " and `<>` overloads (with `Equals`/`GetHashCode`), or"
                        " simplify the record to an auto-equality-bearing form.");
                    break;
                }
            }
        }
    }
    if (op == "=" || op == "<>" || op == "<" || op == ">" ||
        op == "<=" || op == ">=" || op == "and" || op == "or") {
        ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
        return;
    }
    // `in` is a membership test → bool.
    if (op == "in") {
        ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
        return;
    }
    // Arithmetic / shift / bitwise: result type follows the left
    // operand's type when both sides are present and known.
    if (op == "+" || op == "-" || op == "*" || op == "/" ||
        op == "mod" || op == "&" || op == "|" || op == "^" ||
        op == "<<" || op == ">>") {
        TypeRef* lt = be.left ? ctx_.exprType(*be.left) : nullptr;
        // §17.1: bitwise/shift ops (&, |, ^, <<, >>) require integer operands.
        // Reject them on string/reference types to prevent IR corruption.
        if ((op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") && lt) {
            bool lIsStr = lt->kind == TypeRef::Kind::Primitive &&
                lt->primitive == PrimitiveType::String;
            bool lIsRef = lt->kind == TypeRef::Kind::Named;
            if (lIsStr || lIsRef) {
                ctx_.diagnostics().report(Severity::Error, be.span.begin,
                    std::string("operator `") + std::string(op) +
                    "` requires integer operands; use `+` for string concatenation");
                return;
            }
        }
        if (lt) ctx_.setExprType(&be, lt);
        return;
    }
    // `??` null-coalescing (§17.1 L12): the result is the non-nullable
    // form of the LHS — `a ?? b` is `a` when non-null, else `b`, and the
    // spec requires the RHS to be compatible with the non-nullable form
    // of the left (§ null-coalescing). Deriving the result from the LHS
    // (rather than the RHS) keeps the whole `??` typed even when the RHS
    // is a call or postfix-`!` expression whose own type is not recorded.
    if (op == "??") {
        TypeRef* lt = be.left ? ctx_.exprType(*be.left) : nullptr;
        // When the LHS is a null literal (typed as nullable Object) or is
        // untyped, prefer the RHS type — `null ?? x` has x's type.
        bool lhsIsNullLiteral =
            be.left && dynamic_cast<const ast::NullLiteralExpr*>(be.left.get());
        if (lt && !lhsIsNullLiteral) {
            if (lt->nullable) {
                TypeRef* nn = ctx_.makeTypeRef(*lt);
                nn->nullable = false;
                ctx_.setExprType(&be, nn);
            } else {
                ctx_.setExprType(&be, lt);  // already non-nullable
            }
        } else if (be.right) {
            // LHS untyped or null literal — fall back to the RHS type.
            if (TypeRef* rt = ctx_.exprType(*be.right))
                ctx_.setExprType(&be, rt);
        }
        return;
    }
    // Range ops, etc.: leave un-typed (later slices handle).
}

// §17.1 result-type table for unary operators (slice 3c).
// §25: a user-defined unary `operator <op>` (one parameter) declared on
// the operand's type. Overloadable prefix unary operators are `-`/`~`.
MethodSymbol* ExprTC::findUnaryOperator(const ast::UnaryExpr& ue) {
    if (ue.op != "-" && ue.op != "~") return nullptr;
    TypeSymbol* ts = ue.operand ? memberTargetType(ue.operand.get())
                                : nullptr;
    if (!ts) return nullptr;
    for (Symbol* m : ts->members) {
        auto* ms = dynamic_cast<MethodSymbol*>(m);
        if (ms && ms->isOperator && ms->operatorToken == ue.op &&
            ms->params.size() == 1)
            return ms;
    }
    return nullptr;
}

void ExprTC::typeUnary(const ast::UnaryExpr& ue) {
    const auto& op = ue.op;
    // §25: a user-defined unary operator overload.
    if (MethodSymbol* opm = findUnaryOperator(ue)) {
        ctx_.setExprSymbol(&ue, opm);
        if (auto* od = dynamic_cast<const ast::OperatorDecl*>(opm->decl))
            if (od->returnType)
                if (auto* rt = ctx_.resolvedType(*od->returnType))
                    ctx_.setExprType(&ue, rt);
        return;
    }
    if (op == "not") {
        ctx_.setExprType(&ue, ctx_.primitiveTypeRef(PrimitiveType::Bool));
        return;
    }
    // `move x` (§28.3) keeps the operand's type — the result is the
    // same value with ownership transferred. Without the type, member
    // access on a moved value (`var s := move r; s.M`) fails to resolve.
    if (op == "+" || op == "-" || op == "~" || op == "move") {
        TypeRef* ot = ue.operand ? ctx_.exprType(*ue.operand) : nullptr;
        if (ot) ctx_.setExprType(&ue, ot);
        return;
    }
    // `await` — result-type rule is deeper (Task<T> -> T); not typed
    // here.
}

void ExprTC::visitChildren(const ast::Expr& e) {
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        if (be->left) visitExpr(*be->left);
        if (be->right) visitExpr(*be->right);
    } else if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        if (ue->operand) visitExpr(*ue->operand);
    } else if (auto* ma =
                   dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) visitExpr(*ma->target);
    } else if (auto* nm =
                   dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        if (nm->target) visitExpr(*nm->target);
    } else if (auto* ce = dynamic_cast<const ast::CallExpr*>(&e)) {
        if (ce->callee) visitExpr(*ce->callee);
        // per §24.3: an event-handler argument (`<event>.Subscribe(h)` / .Add /
        // .Remove) — push the event payload type so an untyped handler lambda's
        // parameter is typed as `T`, letting its body resolve `e.Field`.
        TypeRef* eventPayload = ce->callee
            ? eventHandlerPayloadType(ce->callee.get()) : nullptr;
        // stdlib §5 (D7): for a LINQ operator/aggregate over a collection or
        // lazy receiver, the lambda argument is `(T) -> …` over the source
        // element type T. Seed an untyped single-param lambda's parameter with
        // T (same mechanism as the event-handler payload above) so its body
        // resolves member access on the element (e.g. `p.x` over Array<Point>).
        TypeRef* linqElem = ce->callee
            ? linqLambdaElementType(ce->callee.get()) : nullptr;
        // Resolve callee's parameter list to type out-var locals.
        sema::Symbol* calleeSym2 = ce->callee ? ctx_.exprSymbol(*ce->callee) : nullptr;
        const ast::MethodDecl* calleeMd2 = nullptr;
        if (auto* ms2 = dynamic_cast<sema::MethodSymbol*>(calleeSym2))
            calleeMd2 = dynamic_cast<const ast::MethodDecl*>(ms2->decl);
        std::size_t pos2 = 0;
        for (const auto& a : ce->args) {
            // §22.4: `out var x` declares x in the current scope.
            // Register the symbol BEFORE visiting the IdentifierExpr so
            // lookup finds it. Infer type from the callee's parameter.
            if (a.modifier == "out var" && a.value && currentScope_) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                                   a.value.get())) {
                    auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                    lsym->name = id->name;
                    lsym->decl = id;
                    // Infer type from the callee's formal parameter.
                    if (calleeMd2 && pos2 < calleeMd2->params.size() &&
                        calleeMd2->params[pos2].type) {
                        lsym->declType = ctx_.resolvedType(
                            *calleeMd2->params[pos2].type);
                        // out params are nullable refs — strip nullable
                        // (the variable holds the non-nullable value).
                        if (lsym->declType && lsym->declType->nullable) {
                            TypeRef* nn = ctx_.makeTypeRef(*lsym->declType);
                            nn->nullable = false;
                            lsym->declType = nn;
                        }
                    }
                    currentScope_->names[id->name] = lsym;
                }
            }
            if (a.value) {
                // per §24.3 / stdlib §5 (D7): type the (single) untyped lambda
                // param as the event payload T (handler) or the LINQ source
                // element T, so its body resolves member access on the param.
                TypeRef* savedPending = pendingLambdaParamType_;
                if (auto* lam =
                        dynamic_cast<const ast::LambdaExpr*>(a.value.get())) {
                    if (eventPayload)     pendingLambdaParamType_ = eventPayload;
                    else if (linqElem)    pendingLambdaParamType_ = linqElem;
                    (void)lam;
                }
                visitExpr(*a.value);
                pendingLambdaParamType_ = savedPending;
            }
            ++pos2;
        }
    } else if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
        if (nc->callee) visitExpr(*nc->callee);
        for (const auto& a : nc->args) {
            if (a.value) visitExpr(*a.value);
        }
    } else if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        if (ix->target) visitExpr(*ix->target);
        for (const auto& a : ix->indices) {
            if (a.value) visitExpr(*a.value);
        }
    } else if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        if (bg->operand) visitExpr(*bg->operand);
    } else if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        for (const auto& a : ne->args) {
            if (a.value) visitExpr(*a.value);
        }
    } else if (auto* ce = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        if (ce->condition) {
            visitExpr(*ce->condition);
            // §20: the `if` expression's condition must be bool.
            checkConditionIsBool(*ce->condition, "if-expression");
        }
        if (ce->thenValue) visitExpr(*ce->thenValue);
        if (ce->elseValue) visitExpr(*ce->elseValue);
    } else if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
        if (te->operand) visitExpr(*te->operand);
    } else if (auto* th = dynamic_cast<const ast::ThrowExpr*>(&e)) {
        if (th->operand) {
            visitExpr(*th->operand);
        } else if (catchDepth_ == 0) {
            // Slice S27: bare `throw` expression re-raise is valid
            // only inside a catch arm body (§31.5 / §17.12).
            ctx_.diagnostics().report(
                Severity::Error, e.span.begin,
                "bare `throw` (re-raise) is only valid inside a "
                "catch arm body (§31.5)");
        }
    } else if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
        // §A.7 / §21.7: lambdas create their own scope with parameter
        // symbols. Each parameter becomes a ParameterSymbol whose
        // declType is inferred from the explicit type annotation (untyped
        // lambda params remain null — callers must supply context).
        // per §24.3: an event-handler lambda — give its single untyped param
        // the event payload type T. Consumed once (cleared) so nested lambdas
        // don't inherit it.
        TypeRef* eventParamType = pendingLambdaParamType_;
        pendingLambdaParamType_ = nullptr;
        Scope lamScope;
        lamScope.parent = currentScope_;
        std::vector<sema::Symbol*> lamParamSyms;
        for (const auto& p : lam->params) {
            auto* ps = ctx_.makeSymbol<ParameterSymbol>();
            ps->name = p.name;
            ps->modifier = p.modifier;
            if (p.type) ps->declType = p.type.get();
            else if (eventParamType && lam->params.size() == 1)
                ps->resolvedDeclType = eventParamType;  // §24.3 handler param
            lamParamSyms.push_back(ps);
            if (!p.name.empty()) lamScope.names[p.name] = ps;
        }
        ctx_.setLambdaParams(lam, std::move(lamParamSyms));
        // §21.7: suppress enclosing callable's return context;
        // §21.5: reset loop/catch depth across the lambda boundary.
        Scope* saveScope = currentScope_;
        ReturnCtx saveRet = returnCtx_;
        TypeRef* saveRetTy = currentReturnType_;
        int saveLoop = loopDepth_;
        int saveCatch = catchDepth_;
        currentScope_ = &lamScope;
        returnCtx_ = ReturnCtx::None;
        currentReturnType_ = nullptr;
        loopDepth_ = 0;
        catchDepth_ = 0;
        if (lam->exprBody) visitExpr(*lam->exprBody);
        for (const auto& s : lam->stmts) if (s) visitStmt(*s);
        currentScope_ = saveScope;
        returnCtx_ = saveRet;
        currentReturnType_ = saveRetTy;
        loopDepth_ = saveLoop;
        catchDepth_ = saveCatch;
        // §A.7: set the lambda's exprType to a Function TypeRef so
        // that `var f := lam; f(...)` calls type-check correctly and
        // lowerCall can detect it as a function-typed local.
        {
            auto* lamTy = ctx_.makeTypeRef();
            lamTy->kind = TypeRef::Kind::Function;
            for (const auto& p : lam->params) {
                TypeRef::FuncParam fp;
                fp.modifier = p.modifier;
                fp.name = p.name;
                if (p.type) fp.type = ctx_.resolvedType(*p.type);
                lamTy->funcParams.push_back(std::move(fp));
            }
            // Infer the return type: from exprBody for expression lambdas,
            // or by scanning block-body stmts for the first `return expr`.
            TypeRef* inferredReturn = nullptr;
            if (lam->exprBody) {
                inferredReturn = ctx_.exprType(*lam->exprBody);
            } else {
                // Scan stmts (flat, not recursive) for a `return EXPR`.
                for (const auto& s : lam->stmts) {
                    if (!s) continue;
                    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s.get())) {
                        if (rs->value) { inferredReturn = ctx_.exprType(*rs->value); break; }
                    }
                    // Check common wrappers: DoEndBlock, IfStmt body.
                    auto scanInner = [&](const std::vector<std::unique_ptr<ast::Stmt>>& inner) {
                        for (const auto& ss : inner) {
                            if (!ss) continue;
                            if (auto* rs2 = dynamic_cast<const ast::ReturnStmt*>(ss.get()))
                                if (rs2->value && !inferredReturn)
                                    inferredReturn = ctx_.exprType(*rs2->value);
                        }
                    };
                    if (auto* bs = dynamic_cast<const ast::DoEndBlock*>(s.get()))
                        scanInner(bs->body);
                    if (auto* ifs = dynamic_cast<const ast::IfStmt*>(s.get())) {
                        scanInner(ifs->ifBranch.body);
                        for (auto& el : ifs->elifBranches) scanInner(el.body);
                        scanInner(ifs->elseBody);
                    }
                    if (inferredReturn) break;
                }
                // Fallback: use int (i32) for lambdas with no visible return.
                if (!inferredReturn)
                    inferredReturn = ctx_.primitiveTypeRef(PrimitiveType::Int);
            }
            lamTy->funcReturn = inferredReturn;
            ctx_.setExprType(lam, lamTy);
        }
    } else if (auto* ce = dynamic_cast<const ast::CaseExpr*>(&e)) {
        if (ce->scrutinee) visitExpr(*ce->scrutinee);
        TypeRef* scrutTy =
            ce->scrutinee ? ctx_.exprType(*ce->scrutinee) : nullptr;
        for (const auto& arm : ce->arms) {
            // §19: a BindingPattern arm introduces a new scope with the
            // bound name visible inside the guard and body (per §19.2).
            auto* bp =
                dynamic_cast<const ast::BindingPattern*>(arm.pattern.get());
            auto* dp =
                dynamic_cast<const ast::DestructuringPattern*>(arm.pattern.get());
            if (bp && currentScope_) {
                Scope armScope;
                armScope.parent = currentScope_;
                auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                lsym->name = bp->name;
                lsym->decl = bp;
                lsym->declType = bp->type ? ctx_.resolvedType(*bp->type)
                                          : scrutTy;
                armScope.names[bp->name] = lsym;
                Scope* saveScope = currentScope_;
                currentScope_ = &armScope;
                if (arm.guard) visitExpr(*arm.guard);
                if (arm.exprBody) visitExpr(*arm.exprBody);
                for (const auto& s : arm.stmts) if (s) visitStmt(*s);
                currentScope_ = saveScope;
            } else if (dp && currentScope_ && dp->typeName) {
                // §19: DestructuringPattern — same logic as in CaseStmt.
                TypeSymbol* recTy = nullptr;
                std::string_view lastName = dp->typeName->parts.empty()
                    ? std::string_view{}
                    : dp->typeName->parts.back();
                // Search ctx_.symbols() for the record TypeSymbol by name.
                // This is more robust than searching only module children
                // since symbols may be nested or added during synthesis.
                for (const auto& sym : ctx_.symbols()) {
                    if (auto* ts = dynamic_cast<TypeSymbol*>(sym.get())) {
                        if (ts->name == lastName &&
                            ts->kind == ast::TypeDeclKind::Record) {
                            recTy = ts; break;
                        }
                    }
                }
                std::vector<FieldSymbol*> fields;
                if (recTy) {
                    for (Symbol* m : recTy->members) {
                        if (auto* fs = dynamic_cast<FieldSymbol*>(m)) {
                            bool isStatic = false;
                            for (auto& mod : fs->modifiers)
                                if (mod == "static") { isStatic = true; break; }
                            if (!isStatic) fields.push_back(fs);
                        }
                    }
                }
                Scope armScope;
                armScope.parent = currentScope_;
                for (std::size_t i = 0; i < dp->subPatterns.size(); ++i) {
                    const auto& sp = dp->subPatterns[i];
                    if (sp.nakedBinding.empty()) continue;
                    TypeRef* fieldTy = nullptr;
                    if (i < fields.size() && fields[i]->posParam &&
                        fields[i]->posParam->type) {
                        fieldTy = ctx_.resolvedType(*fields[i]->posParam->type);
                    }
                    auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                    lsym->name = sp.nakedBinding;
                    lsym->decl = reinterpret_cast<const ast::Node*>(&sp);
                    lsym->declType = fieldTy;
                    armScope.names[sp.nakedBinding] = lsym;
                }
                Scope* saveScope = currentScope_;
                currentScope_ = &armScope;
                if (arm.guard) visitExpr(*arm.guard);
                if (arm.exprBody) visitExpr(*arm.exprBody);
                for (const auto& s : arm.stmts) if (s) visitStmt(*s);
                currentScope_ = saveScope;
            } else {
                if (arm.guard) visitExpr(*arm.guard);
                if (arm.exprBody) visitExpr(*arm.exprBody);
                for (const auto& s : arm.stmts) if (s) visitStmt(*s);
            }
        }
    } else if (auto* we = dynamic_cast<const ast::WithExpr*>(&e)) {
        if (we->source) visitExpr(*we->source);
        for (const auto& ia : we->assigns) {
            if (ia.value) visitExpr(*ia.value);
        }
        // §A.7: a `source with …` expression has the same type as its
        // source (an object initializer / record copy preserves type).
        if (we->source) {
            if (auto* st = ctx_.exprType(*we->source))
                ctx_.setExprType(&e, st);
        }
    } else if (auto* cl =
                   dynamic_cast<const ast::CollectionLiteralExpr*>(&e)) {
        for (const auto& k : cl->keys) if (k) visitExpr(*k);
        for (const auto& v : cl->elements) if (v) visitExpr(*v);
    } else if (auto* re = dynamic_cast<const ast::RangeExpr*>(&e)) {
        // §16: visit lo, hi, and step so that identifiers inside range
        // bounds (e.g. `1..n`) get their symbols resolved.
        if (re->lo) visitExpr(*re->lo);
        if (re->hi) visitExpr(*re->hi);
        if (re->step) visitExpr(*re->step);
    }
    // Identifier / This / Base / TypeOf / NameOf / Default —
    // no expression children to recurse into.
}

} // namespace bezel::sema
