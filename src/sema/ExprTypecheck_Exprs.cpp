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

// §6.2: a primitive that participates in mixed-type arithmetic promotion.
// These are the numeric types (§4.2/§4.3/§4.4) plus `char`, which §5.1
// admits implicit conversions from (to int and wider). `bool`, `string`,
// `Object`, and `None` do not promote.
bool isPromotablePrimitive(PrimitiveType p) {
    using P = PrimitiveType;
    switch (p) {
        case P::Byte:  case P::SByte: case P::Short: case P::UShort:
        case P::Int:   case P::UInt:  case P::Long:  case P::ULong:
        case P::Float: case P::Double: case P::Decimal: case P::Char:
            return true;
        default:
            return false;
    }
}

// The source spelling of a primitive type, for diagnostics.
const char* primitiveSpelling(PrimitiveType p) {
    using P = PrimitiveType;
    switch (p) {
        case P::Bool:    return "bool";
        case P::Char:    return "char";
        case P::String:  return "string";
        case P::Byte:    return "byte";
        case P::SByte:   return "sbyte";
        case P::Short:   return "short";
        case P::UShort:  return "ushort";
        case P::Int:     return "int";
        case P::UInt:    return "uint";
        case P::Long:    return "long";
        case P::ULong:   return "ulong";
        case P::Float:   return "float";
        case P::Double:  return "double";
        case P::Decimal: return "decimal";
        case P::Object:  return "Object";
        case P::None:    return "<none>";
    }
    return "<unknown>";
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
            // per §26.1 / fix #456: inside a PRIMITIVE/builtin extension body
            // (currentType_ is null — no TypeSymbol for the extended type), set
            // `this`'s exprType to the extension's target TypeRef so that
            // resolveMemberAccess can call ctx_.exprType(*ma.target) and then
            // lookupExtensionMember on it. Without this, `this.OtherExtMethod()`
            // from inside `extension int` leaves `this` untyped → the extension
            // member lookup is never reached → codegen emits ret i32 0.
            // Named-type extensions (class/record/enum) already work because
            // resolveMemberAccess reads currentType_ directly — not exprType.
            if (id->name == "this" && !currentType_ && currentExtension_) {
                if (auto* ed = dynamic_cast<const ast::ExtensionDecl*>(
                        currentExtension_->decl)) {
                    if (ed->targetType) {
                        if (TypeRef* extTy = ctx_.resolvedType(*ed->targetType))
                            ctx_.setExprType(&e, extTy);
                    }
                }
            }
            return;
        }
        Symbol* sym = lookupName(id->name);
        if (sym) {
            ctx_.setExprSymbol(&e, sym);
            // Slice S29 (§22.8): a field initializer may not observe
            // an instance member (implicit-`this` reference).
            if (inFieldInitializer_) checkNotInstanceMember(*id, sym);
            // per §14.1 / §22.12 (issue #520): when the identifier is a
            // generic class name with explicit type args (`Registry<int>`),
            // build an instantiated TypeRef (kind=Named, declaration=TypeSymbol,
            // genericArgs populated with the resolved type args) and attach it
            // as the exprType.  The subsequent `.member` MemberAccessExpr then
            // finds a Named TypeRef with a declaration and resolves the static
            // member against the concretely instantiated class.
            // When typeArgs is empty (the common case) this block is skipped.
            if (!id->typeArgs.empty()) {
                if (auto* ts = dynamic_cast<TypeSymbol*>(sym)) {
                    auto* instTy = ctx_.makeTypeRef();
                    instTy->kind = TypeRef::Kind::Named;
                    instTy->declaration = ts;
                    for (const auto& ta : id->typeArgs) {
                        if (ta) {
                            TypeRef* resolved = ctx_.resolvedType(*ta);
                            instTy->genericArgs.push_back(resolved);
                        } else {
                            instTy->genericArgs.push_back(nullptr);
                        }
                    }
                    ctx_.setExprType(&e, instTy);
                    // Return early — no further local/param/field type
                    // attachment is needed; the exprType is now set.
                    return;
                }
            }
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
                } else if (fs->posParam && fs->posParam->type) {
                    // §9.1: a positional record field's type comes from the
                    // Param node, not a FieldDecl (fs->decl is the TypeDecl).
                    // Without this, a bare identifier `Data` inside a record
                    // method body (implicit this.Data where Data: Inner) has no
                    // exprType, so `Data.Val` — a MemberAccessExpr on Data —
                    // cannot resolve targetType and `Val`'s symbol is never set.
                    // Codegen's fieldAddr then finds no symbol → returns nullptr
                    // → the method body emits `ret i32 0` (wrong result).
                    if (auto* rt = ctx_.resolvedType(*fs->posParam->type)) {
                        ctx_.setExprType(&e, rt);
                    }
                }
            }
            // §12.7: flow narrowing. Inside the controlled block of `if x is T`
            // (or the else of `if x is not T`), a reference to the narrowed
            // variable takes the narrowed type `T` instead of its declared
            // base type — overriding whatever exprType the declared-type rules
            // above attached. Member access on this identifier (`x.M`) then
            // resolves against `T` (resolveMemberAccess reads the target's
            // exprType first). Narrowing applies only to simple variables
            // (locals and parameters), keyed by their resolved symbol.
            if (auto nt = narrowings_.find(sym); nt != narrowings_.end()) {
                ctx_.setExprType(&e, nt->second);
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
                   // stdlib §2.6: Guid is auto-imported (a builtin value type).
                   id->name == "Guid" ||
                   // §7 System.IO:
                   id->name == "File" || id->name == "Directory" ||
                   id->name == "Path" ||
                   // §4 Linq:
                   id->name == "Linq" ||
                   // §6.1 / §30.8 (stdlib §6.1): CancellationToken is auto-imported
                   // (§2.3); used in expression position for `CancellationToken.None`.
                   // CancellationTokenSource requires `import System.Threading` per
                   // spec; import-gating not yet enforced (same stance as §4.8
                   // KeyNotFoundException).
                   id->name == "CancellationToken" ||
                   id->name == "CancellationTokenSource" ||
                   // §30.10 / stdlib §6.7: the `Task` static-helper class is
                   // auto-imported (Task.Delay, Task.FromResult, …). Only used in
                   // expression position as the receiver of a static helper call.
                   id->name == "Task" ||
                   // §30.14 (#600): Domain is a static class auto-imported for
                   // Domain.Spawn; recognized here so Domain.Spawn(…) can resolve
                   // as a call on a known-class receiver.
                   id->name == "Domain") {
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
        // §18.1: the `f`/`F` suffix gives type `float` (f32); `d`/`D` or
        // no suffix gives `double` (f64); `m`/`M` gives `decimal` (§4.4).
        PrimitiveType pt = PrimitiveType::Double;
        bool isDecimalLiteral = false;
        if (!fl->text.empty()) {
            char last = fl->text.back();
            if (last == 'f' || last == 'F') {
                pt = PrimitiveType::Float;
            } else if (last == 'm' || last == 'M') {
                // §4.4: `m`/`M` suffix → decimal literal. Type it correctly
                // as decimal (not double) so the suffix is not silently
                // discarded (#521). Full 128-bit decimal arithmetic is not
                // yet codegen-backed, so emit a diagnostic here rather than
                // letting the program reach codegen and either miscompile
                // (decimal lowered as i32) or crash the IR verifier (a
                // double ConstantFP stored into an i32 global for static
                // fields). The diagnostic is a deferred-feature error, not a
                // permanent restriction; remove it when §4.4 is implemented.
                pt = PrimitiveType::Decimal;
                isDecimalLiteral = true;
            }
            // `d`/`D` → double (already default).
        }
        ctx_.setExprType(&e, ctx_.primitiveTypeRef(pt));
        // #521: diagnose decimal literals before they reach codegen.
        if (isDecimalLiteral) {
            ctx_.diagnostics().report(
                Severity::Error, e.span.begin,
                "decimal literals are not yet fully implemented (§4.4, #521);"
                " full 128-bit decimal arithmetic is deferred");
        }
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
    // §20: the `if` expression `x if c else y` has the COMMON type of its
    // two branches (the type the other branch widens/subtypes into). A
    // bottom-typed (`throw`) branch imposes no constraint; incompatible
    // branches (e.g. the `throw E if c else x` shape, which parses as
    // `throw (E if c else x)`) get a clean §20 error rather than crashing
    // codegen's PHI. typeConditional records the result type on `e` so
    // member access on the result (`(a if c else b).M`) resolves.
    if (auto* cnd = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        typeConditional(*cnd, e);
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
    // §19 + §20: a `case` expression's type is the COMMON type of its
    // (non-diverging) arm bodies — folded through the same common-type rule
    // as the `if` expression. A bottom-typed (`throw`/diverging) arm imposes
    // no constraint; arms with no common type get a clean §20 error rather
    // than crashing codegen's result PHI. Recorded on `e` so member access on
    // a case result (`(case … end).M`) resolves.
    if (auto* cse = dynamic_cast<const ast::CaseExpr*>(&e)) {
        typeCase(*cse, e);
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
                         tt->primitive == PrimitiveType::String) {
                    // §3.4 / §4.6 / issue #474: a string indexer distinguishes
                    // the index kind. `s[i]` (a single int) returns the i-th
                    // `char` (code point); `s[r]` for a `Range<int>` value `r`
                    // returns the SUBSTRING spanning `r` — a fresh `string`.
                    // Detect the Range form by the index being a range literal
                    // (`s[a..b]`, `s[..b]`, `s[a..]`, `s[..]`) so the access is
                    // typed `string`; otherwise it is `char`.
                    bool rangeIndex =
                        !ix->indices.empty() && ix->indices[0].value &&
                        dynamic_cast<const ast::RangeExpr*>(
                            ix->indices[0].value.get()) != nullptr;
                    ctx_.setExprType(
                        &e, rangeIndex
                                ? ctx_.primitiveTypeRef(PrimitiveType::String)
                                : ctx_.primitiveTypeRef(PrimitiveType::Char));
                }
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
                //   * issue #401 / §4.1 / §4.2: the int-indexed list INTERFACES
                //     IReadOnlyList<T> (get-only), IList<T> (get/set borrowing),
                //     and IOwnedList<T> (get/owned-set) each declare an indexer
                //     `[i: int]: T`; the element type is T = genericArgs[0], the
                //     same position as Array<T>. (No collection interface
                //     declares a key-indexer, so dictionaries are concrete-only.)
                else if (tt->kind == TypeRef::Kind::Named && !tt->declaration &&
                         tt->astNode && tt->astNode->name &&
                         !tt->astNode->name->parts.empty()) {
                    std::string_view cn = tt->astNode->name->parts[0];
                    bool isArrLike =
                        cn == "Array" || cn == "OwnedArray" ||
                        // issue #401: list interfaces are array-backed; their
                        // indexer result type is the element T = genericArgs[0].
                        cn == "IReadOnlyList" || cn == "IList" ||
                        cn == "IOwnedList";
                    // §4.15 (#549): ArraySpan<T>/ReadOnlyArraySpan<T> indexer
                    // returns T = genericArgs[0].
                    bool isSpan =
                        cn == "ArraySpan" || cn == "ReadOnlyArraySpan";
                    bool isDict =
                        cn == "Dictionary" || cn == "OwnedDictionary";
                    if ((isArrLike || isSpan) && !tt->genericArgs.empty() &&
                        tt->genericArgs[0])
                        ctx_.setExprType(&e, tt->genericArgs[0]);
                    else if (isDict && tt->genericArgs.size() > 1 &&
                             tt->genericArgs[1])
                        ctx_.setExprType(&e, tt->genericArgs[1]);
                }
                // §23.4: USER-DEFINED indexer `obj[i]` on a class/record/
                // interface type. The result type is the indexer property's
                // declared type. The ordinary member-lookup path types named
                // properties (resolveMemberAccess) but never reaches an indexer
                // (it has no member name — `obj[i]` is an IndexExpr, not a
                // MemberAccess), so the IndexExpr was left untyped. Type it here
                // by finding the indexer property (stored under `.this`, walked
                // transitively over base types) and resolving its declared type,
                // substituting generic-param result types against the receiver's
                // concrete args (#361 / §14.1) — exactly as resolveMemberAccess
                // does for a named property. This matters downstream: the
                // ownership pass's borrow-vs-copy predicate (§28.3) consults the
                // IndexExpr's type — a VALUE-typed indexer result is a copy, not
                // a borrow, so returning it from an owned `using var` local is
                // not a borrow escape (a CLASS/borrow-typed result still is).
                else if (tt->kind == TypeRef::Kind::Named && tt->declaration) {
                    if (auto* ps = dynamic_cast<PropertySymbol*>(
                            lookupMember(tt->declaration, ".this"))) {
                        if (ps->isIndexer) {
                            if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(
                                    ps->decl)) {
                                if (pd->type) {
                                    if (auto* rt = ctx_.resolvedType(*pd->type))
                                        ctx_.setExprType(
                                            &e, substituteMemberType(
                                                    rt, ix->target.get(),
                                                    ps->parent));
                                }
                            }
                        }
                    }
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
                // per §15.4 / §22.7 (#599): a collection literal `[..]`
                // in a `new` argument position is a compile error when the
                // constructor parameter is not a collection type.  The
                // stdlib builtin collection types (Array<T>, Stack<T>, etc.)
                // are represented as Named TypeRefs with a null declaration
                // and expose only `constructor()` / `constructor(capacity:
                // int)` — neither accepts a collection literal (§15.4
                // requires a collection-typed target context for `[..]`).
                // Codegen silently ignores all `new Array<T>(...)` arguments
                // and always emits an empty-capacity allocation, so the
                // misuse produces a silently empty collection (#599).
                // Detect and reject here before codegen sees the node.
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty()) {
                    std::string_view colName = rt->astNode->name->parts[0];
                    bool isBuiltinColl =
                        colName == "Array"     || colName == "OwnedArray"  ||
                        colName == "Stack"     || colName == "OwnedStack"  ||
                        colName == "Queue"     || colName == "OwnedQueue"  ||
                        colName == "Set"       || colName == "OwnedSet"    ||
                        colName == "LinkedList"|| colName == "OwnedLinkedList" ||
                        colName == "Dictionary"|| colName == "OwnedDictionary" ||
                        colName == "StringBuilder"  ||
                        // §28.13 / stdlib §2.16: Shared<T>/Weak<T> constructors
                        // take an owned value, not a collection literal.
                        colName == "Shared"    || colName == "Weak";
                    if (isBuiltinColl) {
                        for (const auto& a : ne->args) {
                            if (a.value && dynamic_cast<const ast::CollectionLiteralExpr*>(
                                               a.value.get())) {
                                // Report on the literal's position.
                                ctx_.diagnostics().report(
                                    Severity::Error, a.value->span.begin,
                                    std::string("collection literal `[..]` cannot be "
                                        "passed as an argument to `new ") +
                                    std::string(colName) +
                                    "<...>` — the constructor takes an optional "
                                    "`int` capacity, not a collection literal; use a "
                                    "typed-target declaration instead: "
                                    "`var x: " + std::string(colName) +
                                    "<T> := [...]` (\xC2\xA7""15.4, #599)");
                            }
                        }
                    }
                }
                // §28.13 / stdlib §2.16 (#603): Shared<T> / Weak<T> require T to
                // be a proper class type.  Runtime-managed (string, function),
                // value (int/record/enum), and interface types have no disposal
                // obligation to share and are rejected at instantiation, mirroring
                // the §14.4 / OwnedArray<T: class> constraint.
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty()) {
                    std::string_view shName = rt->astNode->name->parts[0];
                    if (shName == "Shared" || shName == "Weak") {
                        // Check the first generic argument for a proper class type.
                        bool argIsClass = false;
                        std::string badTypeName;
                        if (!rt->genericArgs.empty() && rt->genericArgs[0]) {
                            const TypeRef* ta = rt->genericArgs[0];
                            if (ta->kind == TypeRef::Kind::Named &&
                                ta->declaration &&
                                ta->declaration->kind == ast::TypeDeclKind::Class) {
                                argIsClass = true;
                            } else {
                                // Produce a human-readable type name for the diagnostic.
                                if (ta->kind == TypeRef::Kind::Primitive) {
                                    // Map PrimitiveType → name for the diagnostic.
                                    using P = sema::PrimitiveType;
                                    switch (ta->primitive) {
                                        case P::Bool:    badTypeName = "bool"; break;
                                        case P::Char:    badTypeName = "char"; break;
                                        case P::String:  badTypeName = "string"; break;
                                        case P::Byte:    badTypeName = "byte"; break;
                                        case P::SByte:   badTypeName = "sbyte"; break;
                                        case P::Short:   badTypeName = "short"; break;
                                        case P::UShort:  badTypeName = "ushort"; break;
                                        case P::Int:     badTypeName = "int"; break;
                                        case P::UInt:    badTypeName = "uint"; break;
                                        case P::Long:    badTypeName = "long"; break;
                                        case P::ULong:   badTypeName = "ulong"; break;
                                        case P::Float:   badTypeName = "float"; break;
                                        case P::Double:  badTypeName = "double"; break;
                                        case P::Decimal: badTypeName = "decimal"; break;
                                        case P::Object:  badTypeName = "Object"; break;
                                        default:         badTypeName = "primitive type"; break;
                                    }
                                }
                                else if (ta->kind == TypeRef::Kind::Function)
                                    badTypeName = "function type";
                                else if (ta->kind == TypeRef::Kind::Named) {
                                    if (ta->declaration) {
                                        badTypeName = std::string(ta->declaration->name);
                                        if (ta->declaration->kind ==
                                            ast::TypeDeclKind::Interface)
                                            badTypeName += " (interface)";
                                        else if (ta->declaration->kind ==
                                                 ast::TypeDeclKind::Record)
                                            badTypeName += " (record/value type)";
                                        else if (ta->declaration->kind ==
                                                 ast::TypeDeclKind::Enum)
                                            badTypeName += " (enum/value type)";
                                    } else if (ta->astNode && ta->astNode->name &&
                                               !ta->astNode->name->parts.empty()) {
                                        badTypeName = std::string(
                                            ta->astNode->name->parts.back());
                                        badTypeName += " (runtime-managed or value type)";
                                    }
                                }
                                if (badTypeName.empty())
                                    badTypeName = "<unknown>";
                            }
                        } else {
                            badTypeName = "<no type argument>";
                        }
                        if (!argIsClass && !badTypeName.empty()) {
                            ctx_.diagnostics().report(
                                Severity::Error, ne->span.begin,
                                std::string(shName) +
                                "<T> requires T to be a proper class type: "
                                "`" + badTypeName + "` is not a class — "
                                "runtime-managed and value types have no "
                                "disposal obligation to share "
                                "(\xC2\xA7""28.13 / stdlib \xC2\xA7""2.16, #603)");
                        }
                    }
                }
                // §30.14 / §30.15 (#600): Channel<T> and ChannelSender<T>
                // instantiation — element type T must be sendable per §3.6.
                // Checked at instantiation so the error is at the `new` site.
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty()) {
                    std::string_view chName = rt->astNode->name->parts[0];
                    if (chName == "Channel" || chName == "ChannelSender") {
                        if (rt->genericArgs.empty() || !rt->genericArgs[0]) {
                            ctx_.diagnostics().report(
                                Severity::Error, ne->span.begin,
                                std::string(chName) +
                                "<T> requires a type argument (§30.15, #600)");
                        } else {
                            const TypeRef* elemTy = rt->genericArgs[0];
                            if (!isSendable(elemTy)) {
                                // Produce a human-readable name for the type arg.
                                std::string tName;
                                if (elemTy->kind == TypeRef::Kind::Named) {
                                    if (elemTy->declaration)
                                        tName = std::string(elemTy->declaration->name);
                                    else if (elemTy->astNode && elemTy->astNode->name &&
                                             !elemTy->astNode->name->parts.empty())
                                        tName = std::string(elemTy->astNode->name->parts[0]);
                                } else if (elemTy->kind == TypeRef::Kind::Primitive) {
                                    using P = sema::PrimitiveType;
                                    switch (elemTy->primitive) {
                                        case P::Bool:    tName = "bool"; break;
                                        case P::Int:     tName = "int"; break;
                                        case P::String:  tName = "string"; break;
                                        default:         tName = "primitive type"; break;
                                    }
                                } else if (elemTy->kind == TypeRef::Kind::Function) {
                                    tName = "function type";
                                } else if (elemTy->kind == TypeRef::Kind::InlineArray) {
                                    tName = elemTy->arrayOwned ? "[N owned]T" : "[N]T";
                                }
                                if (tName.empty()) tName = "<type>";
                                ctx_.diagnostics().report(
                                    Severity::Error, ne->span.begin,
                                    std::string("type argument `") + tName +
                                    "` of `" + std::string(chName) + "<T>` is not "
                                    "sendable: the element type must satisfy the §30.14 "
                                    "sendable-type rules (not borrow-bearing, not "
                                    "move-only, not function-typed, not domain-local; "
                                    "class types must be transfer-closed/sealed) "
                                    "(\xC2\xA7""30.14/\xC2\xA7""30.15, #600)");
                            }
                        }
                    }
                }
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
                    } else if (t->kind == ast::TypeDeclKind::Record &&
                               ctx_.isImported(t) && !t->genericParams.empty()) {
                        // §14.1 / §2.5 (#428): an IMPORTED GENERIC RECORD is NOT
                        // routed through checkNewExprArgs (records have no
                        // constructor-arity check here), so without this guard a
                        // `new ImportedRecord<Args>(...)` slipped past every check
                        // and codegen monomorphized it through the record path —
                        // which has no per-instantiation struct specialization, so
                        // a non-`int` type argument silently MISCOMPILED (the
                        // field kept the i32 placeholder layout). Reject it with
                        // the honest §14 diagnostic until cross-module generic-type
                        // monomorphization is supported (deferred — #428; the
                        // generic CLASS case is gated in checkImportedNewExprArgs).
                        ctx_.diagnostics().report(
                            Severity::Error, ne->span.begin,
                            std::string("constructing an imported generic record "
                                        "(`new ") +
                                std::string(t->name) +
                                "<...>`) is not yet supported: a generic record is "
                                "monomorphized per-instantiation (\xC2\xA7""14) and "
                                "its per-instantiation layout is not yet carried "
                                "across a module boundary (\xC2\xA7""2.5)");
                    }
                }
            }
        }
    }
    // §17.10: typeof(T) → Type.  The `Type` sealed class is defined in the
    // standard library (stdlib §2.13) and is a builtin Named type (arity 0,
    // no user TypeDecl). The sema type of a TypeOfExpr is a Named TypeRef
    // backed by a builtinClassSym("Type") declaration. This prevents codegen
    // from seeing an untyped expression and emitting silent garbage (#487).
    //
    // #511: emit the "not yet implemented" diagnostic HERE in sema so that
    // chained uses such as `typeof(T).Name` fire the error unconditionally
    // regardless of context.  The expression is still TYPED as `Type` so that
    // no secondary "untyped expression" cascades are emitted and the
    // future-implementation path (System.Reflection §11) is preserved.
    if (auto* to = dynamic_cast<const ast::TypeOfExpr*>(&e)) {
        (void)to; // the queriedType is already resolved by TypeResolution
        auto* typeTy = ctx_.makeTypeRef();
        typeTy->kind = TypeRef::Kind::Named;
        typeTy->declaration = builtinClassSym("Type");
        ctx_.setExprType(&e, typeTy);
        ctx_.diagnostics().report(
            Severity::Error, e.span.begin,
            "reflection is not yet implemented: 'typeof' "
            "(§17.10, #487); deferred pending System.Reflection §11");
    }
    // §17.9: nameof(QualifiedName) → string. The result is the rightmost
    // identifier of the operand, as a compile-time string constant. No
    // runtime resolution is performed; sema just types the expression as
    // string (codegen emits the name text as a string literal global).
    if (auto* no = dynamic_cast<const ast::NameOfExpr*>(&e)) {
        (void)no; // name is resolved at codegen from the AST node
        ctx_.setExprType(&e, ctx_.primitiveTypeRef(PrimitiveType::String));
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
                // For LOCAL records the flag lives on the ast::TypeDecl; for
                // IMPORTED records (no ast node) it lives on TypeSymbol itself
                // (set by SymbolCollection and carried in module metadata per
                // issue #501 fix). Check TypeSymbol::hasPositionalFields first;
                // if false and a TypeDecl is available, also check there.
                bool positional = ts->hasPositionalFields;
                if (!positional) {
                    if (auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl))
                        positional = td->hasPositionalFields;
                }
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
    // §7.11 "No three-valued logic": `and` / `or` require both operands to be
    // non-nullable `bool`. A `bool?` operand is a compile error.
    if (op == "and" || op == "or") {
        auto isBoolNullable = [&](const ast::Expr* e) -> bool {
            if (!e) return false;
            const TypeRef* t = ctx_.exprType(*e);
            return t && t->kind == TypeRef::Kind::Primitive &&
                   t->primitive == PrimitiveType::Bool && t->nullable;
        };
        if (isBoolNullable(be.left.get()) || isBoolNullable(be.right.get())) {
            ctx_.diagnostics().report(
                Severity::Error, be.span.begin,
                "operator `" + std::string(op) +
                "` requires non-nullable `bool` operands (§7.11); a `bool?`"
                " operand is not permitted — there is no three-valued logic."
                " Use `??` to coalesce the `bool?` to a `bool` first, or"
                " narrow with an explicit null check.");
            ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
            return;
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
        // §2.5 (stdlib L264–265): TimeSpan + TimeSpan → TimeSpan and
        // TimeSpan - TimeSpan → TimeSpan are compiler-intrinsic operators
        // (TimeSpan is a builtin value type backed by a scalar i64 tick count;
        // these operations are plain i64 add/sub). They are declared as
        // `public static method operator +/-(a,b: TimeSpan): TimeSpan` in the
        // stdlib but are NOT user-defined operators in any Bezel source body, so
        // findBinaryOperator cannot find them. Intercept here before the
        // user-Named-type guard below. The operator * (TimeSpan * double) is
        // deliberately excluded — it requires cross-type widening that is
        // deferred per the existing datetime_timespan_test.cpp note.
        if (lt && lt->kind == TypeRef::Kind::Named &&
            lt->declaration && !lt->declaration->decl &&
            lt->declaration->name == "TimeSpan" &&
            (op == "+" || op == "-")) {
            auto* tsTy = ctx_.makeTypeRef();
            tsTy->kind = TypeRef::Kind::Named;
            tsTy->declaration = builtinClassSym("TimeSpan");
            ctx_.setExprType(&be, tsTy);
            return;
        }
        // §25: arithmetic operators (+, -, *, /, mod) are NOT defined on
        // user class/record types unless the type declares a matching
        // `static method operator <op>` in its own body. `findBinaryOperator`
        // already searched the operand types above and found nothing; if we
        // arrive here with a Named left-operand type, there is no applicable
        // overload and the expression is a compile error. (Bitwise/shift ops
        // have their own guard below; string `+` passes through on a Primitive
        // String type which is not Named, so this does not affect it.)
        if (lt && lt->kind == TypeRef::Kind::Named &&
            (op == "+" || op == "-" || op == "*" || op == "/" || op == "mod")) {
            std::string typeName =
                lt->declaration ? std::string(lt->declaration->name) : "?";
            ctx_.diagnostics().report(
                Severity::Error, be.span.begin,
                std::string("operator `") + std::string(op) +
                "` is not defined for type `" + typeName +
                "` (§25); declare `static method operator " + std::string(op) +
                "(a: " + typeName + ", b: " + typeName + ")` inside `" +
                typeName + "` to overload it");
            return;
        }
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
        // §7.11 lifted operators: when EITHER operand is a nullable value
        // primitive (`int?`, `double?`, `bool?`, …), the arithmetic / bitwise /
        // shift result is lifted to `T?` — `null` when either operand is `null`,
        // otherwise the underlying operator applied to the present payloads. A
        // mixed `T?` + `T` expression also lifts (the non-nullable operand is
        // treated as present), and the result is still `T?`. The result's
        // underlying type is computed exactly as the non-nullable case would
        // (so `int? + int?` is `int?`, and a future widening pair such as
        // `int? + long?` widens identically), then marked nullable. (Equality
        // `=`/`<>` does not reach this block — it returns `bool` above per §8.4
        // — and relational ops also return `bool`, never lift to `bool?`.) The
        // codegen lowering for these operators lives in
        // LlvmBackend_Exprs.cpp::lowerBinary.
        {
            TypeRef* rt = be.right ? ctx_.exprType(*be.right) : nullptr;
            auto isNullablePrim = [](const TypeRef* t) {
                return t && t->kind == TypeRef::Kind::Primitive && t->nullable;
            };
            bool lNull = isNullablePrim(lt);
            bool rNull = isNullablePrim(rt);
            if ((lNull || rNull) && lt && rt &&
                lt->kind == TypeRef::Kind::Primitive &&
                rt->kind == TypeRef::Kind::Primitive) {
                // Determine the underlying (non-nullable) result primitive using
                // the same §6.2 widening rule as the non-nullable path. For
                // shift ops the result follows the left operand. For same-prim
                // operands the result is that primitive.
                PrimitiveType resultPrim = lt->primitive;
                if (op != "<<" && op != ">>" &&
                    lt->primitive != rt->primitive) {
                    if (primitiveWidens(lt->primitive, rt->primitive))
                        resultPrim = rt->primitive;       // left widens to right
                    else if (primitiveWidens(rt->primitive, lt->primitive))
                        resultPrim = lt->primitive;       // right widens to left
                    else if (isPromotablePrimitive(lt->primitive) &&
                             isPromotablePrimitive(rt->primitive)) {
                        // §6.2: no implicit conversion in either direction — a
                        // compile error, mirroring the non-nullable branch below.
                        ctx_.diagnostics().report(
                            Severity::Error, be.span.begin,
                            std::string("operator `") + std::string(op) +
                            "` has no implicit conversion between operand types `" +
                            primitiveSpelling(lt->primitive) + "` and `" +
                            primitiveSpelling(rt->primitive) +
                            "` (§6.2); add an explicit cast (§5.2) to convert one"
                            " operand to the other's type.");
                        return;
                    }
                }
                TypeRef* res = ctx_.makeTypeRef();
                res->kind = TypeRef::Kind::Primitive;
                res->primitive = resultPrim;
                res->nullable = true;   // §7.11: lifted result is T?
                ctx_.setExprType(&be, res);
                return;
            }
        }
        // §6.2 mixed-type arithmetic: "the smaller-range operand is implicitly
        // promoted to the larger type using §5.1. The result has the larger
        // type." For the non-shift operators, when both operands are primitive
        // numerics of different types, the result is the wider operand — the
        // one the other implicitly widens to per §5.1. (Shift ops `<<`/`>>`
        // keep the left operand's type: the right operand is a shift count,
        // not an arithmetic operand, so no promotion applies.)
        // #521 / §4.4: a helper used below to guard any arithmetic operation
        // whose resolved result type is `decimal`. Full 128-bit decimal
        // arithmetic is not yet codegen-backed (lowerPrimitive(Decimal) falls
        // through to i32, which would silently miscompile every operation and
        // produce SIGFPE on division by zero). Diagnose here in sema so the
        // program never reaches codegen in a broken state.
        auto diagDecimalArith = [&](const TypeRef* resultTy) -> bool {
            if (resultTy && resultTy->kind == TypeRef::Kind::Primitive &&
                resultTy->primitive == PrimitiveType::Decimal) {
                ctx_.diagnostics().report(
                    Severity::Error, be.span.begin,
                    "decimal arithmetic is not yet implemented (§4.4, #521);"
                    " full 128-bit decimal arithmetic is deferred");
                return true;
            }
            return false;
        };
        if (op != "<<" && op != ">>") {
            TypeRef* rt = be.right ? ctx_.exprType(*be.right) : nullptr;
            if (lt && rt &&
                lt->kind == TypeRef::Kind::Primitive &&
                rt->kind == TypeRef::Kind::Primitive &&
                lt->primitive != rt->primitive &&
                !lt->nullable && !rt->nullable) {
                if (primitiveWidens(lt->primitive, rt->primitive)) {
                    // left widens to right → result is right (the wider).
                    // #521: if the wider type is decimal, diagnose before
                    // allowing the expression to reach codegen.
                    if (diagDecimalArith(rt)) return;
                    ctx_.setExprType(&be, rt);
                    return;
                }
                if (primitiveWidens(rt->primitive, lt->primitive)) {
                    // right widens to left → result is left (the wider).
                    // #521: same guard for decimal-result from right→left widen.
                    if (diagDecimalArith(lt)) return;
                    ctx_.setExprType(&be, lt);
                    return;
                }
                // §6.2: "If no implicit conversion exists between the operand
                // types (e.g. `double` and `decimal`), the operation is a
                // compile error and an explicit cast is required." This covers
                // every numeric pair §5.1 leaves with no conversion in either
                // direction — `double`/`decimal`, `float`/`decimal`, and the
                // signed/unsigned same-or-narrower-width pairs (`int`/`uint`,
                // `long`/`ulong`, `short`/`ushort`, `byte`/`sbyte`, …). Both
                // operands must be promotable primitives (numeric or `char`);
                // a non-numeric mismatch (e.g. `bool`/`int`) is diagnosed by
                // the operand-type rules elsewhere, not here (issue #410).
                if (isPromotablePrimitive(lt->primitive) &&
                    isPromotablePrimitive(rt->primitive)) {
                    ctx_.diagnostics().report(
                        Severity::Error, be.span.begin,
                        std::string("operator `") + std::string(op) +
                        "` has no implicit conversion between operand types `" +
                        primitiveSpelling(lt->primitive) + "` and `" +
                        primitiveSpelling(rt->primitive) +
                        "` (§6.2); add an explicit cast (§5.2) to convert one"
                        " operand to the other's type.");
                    // Leave the result un-typed (the operation is rejected),
                    // matching the bitwise-operand error above.
                    return;
                }
            }
        }
        // #521 / §4.4: same-type decimal arithmetic (both operands are
        // decimal, so no widening path was taken above). Diagnose before
        // typing the result as decimal so codegen never sees it.
        if (diagDecimalArith(lt)) return;
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

// §28.8.1 / §28.11 (issue #423): classify the receiver of a `move coll[i]`.
// `move coll[i]` is admitted only on the enumerated CONCRETE owning receivers
// (§28.11: `OwnedArray<T>`, `OwnedDictionary<K, V>`, class-typed `OwnedBuffer<T>`,
// class-typed `Span<T>`, and the owning inline arrays). An INTERFACE-typed
// receiver is not among them — the interface owns no storage, so there is no
// statically-known slot to leave in moved-from state; codegen's
// `lowerMoveFromCollectionSlot` recognizes only the concrete owning collections
// and would mishandle the moved-out element (a double-free at runtime). Return
// the interface's type name when the receiver is interface-typed, else "".
std::string
ExprTC::moveThroughInterfaceSlotName(const ast::Expr& operand) const {
    const auto* ix = dynamic_cast<const ast::IndexExpr*>(&operand);
    if (!ix || !ix->target) return {};
    const TypeRef* tt = ctx_.exprType(*ix->target);
    if (!tt || tt->kind != TypeRef::Kind::Named) return {};
    // A user-declared `interface` resolves to a Named TypeRef whose declaration
    // is an interface TypeDecl (§11.2). Its name is the diagnostic subject.
    if (tt->declaration) {
        if (tt->declaration->kind == ast::TypeDeclKind::Interface)
            return std::string(tt->declaration->name);
        return {};
    }
    // The built-in collection interfaces resolve to a Named TypeRef with a NULL
    // declaration (TypeResolution_Names) carrying the name on the AST node. The
    // only built-in interfaces that declare an int-indexer — the sole shape a
    // `move coll[i]` can take among built-ins — are the list interfaces
    // `IReadOnlyList<T>`, `IList<T>`, and `IOwnedList<T>` (stdlib §4.1–§4.2);
    // every other built-in interface has no indexer, so `coll[i]` would not
    // typecheck on it. (`Array`/`OwnedArray`/`OwnedDictionary`/… are concrete
    // CLASSES, not interfaces, and are correctly NOT matched here.)
    if (tt->astNode && tt->astNode->name && !tt->astNode->name->parts.empty()) {
        std::string_view cn = tt->astNode->name->parts[0];
        if (cn == "IReadOnlyList" || cn == "IList" || cn == "IOwnedList")
            return std::string(cn);
    }
    return {};
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
        // §7.11: `not bool?` is a compile error.
        if (ue.operand) {
            const TypeRef* ot = ctx_.exprType(*ue.operand);
            if (ot && ot->kind == TypeRef::Kind::Primitive &&
                ot->primitive == PrimitiveType::Bool && ot->nullable) {
                ctx_.diagnostics().report(
                    Severity::Error, ue.span.begin,
                    "`not` requires a non-nullable `bool` operand (§7.11); a"
                    " `bool?` operand is not permitted — there is no"
                    " three-valued logic. Use `??` to coalesce the `bool?` to"
                    " a `bool` first, or narrow with an explicit null check.");
            }
        }
        ctx_.setExprType(&ue, ctx_.primitiveTypeRef(PrimitiveType::Bool));
        return;
    }
    // §28.8.1 / §28.11 (issue #423): `move coll[i]` is admitted only on the
    // enumerated CONCRETE owning receivers (§28.11). A `move` through an
    // INTERFACE-typed indexer slot (`IOwnedList<T>`, `IList<T>`,
    // `IReadOnlyList<T>`, or a user interface exposing an indexer) is rejected:
    // the interface owns no storage, so there is no slot to leave in moved-from
    // state, and codegen would mishandle the moved-out element (a double-free at
    // runtime). The diagnostic still falls through to set the result type so a
    // downstream consumer of the move result does not cascade unrelated errors.
    if (op == "move" && ue.operand) {
        std::string iface = moveThroughInterfaceSlotName(*ue.operand);
        if (!iface.empty()) {
            ctx_.diagnostics().report(
                Severity::Error, ue.span.begin,
                std::string("`move` is not admitted through an interface-typed "
                            "indexer slot: `") + iface +
                "` is an interface and owns no storage, so there is no slot to "
                "take ownership of (§28.8.1 / §28.11 — `move coll[i]` is "
                "admitted only on concrete owning collections: `OwnedArray<T>`, "
                "`OwnedDictionary<K, V>`, class-typed `OwnedBuffer<T>` / "
                "`Span<T>`, and owning inline arrays)");
        }
    }
    // `move x` (§28.3) keeps the operand's type — the result is the
    // same value with ownership transferred. Without the type, member
    // access on a moved value (`var s := move r; s.M`) fails to resolve.
    if (op == "+" || op == "-" || op == "~" || op == "move") {
        TypeRef* ot = ue.operand ? ctx_.exprType(*ue.operand) : nullptr;
        if (ot) ctx_.setExprType(&ue, ot);
        return;
    }
    // `await` — async-readiness 3b (§30.2 / §30.3 / §30.5).
    if (op == "await") {
        // §30.2 / Appendix C.6: `await` is valid only inside an `async`
        // method, an `async` lambda, or the synchronous prefix of `await for`
        // (the latter is itself only reachable inside an async context). A
        // static field initializer / static constructor is never async, so an
        // `await` there is caught by this same gate.
        if (!inAsyncContext_) {
            ctx_.diagnostics().report(
                Severity::Error, ue.span.begin,
                "`await` is valid only inside an `async` method or `async` "
                "lambda (§30.2)");
            // Fall through and still produce the result type so downstream
            // typing of the awaited value doesn't cascade unrelated errors.
        }
        // Result type (§30.3 / §30.5):
        //   await Task          -> unit  (no result value)
        //   await Task<T>       -> T     (Pattern A: a borrow the task owns;
        //                                  the borrow/ownership at the await
        //                                  site is the ownership pass's job —
        //                                  3b produces the element type T)
        //   await OwnedTask<T>  -> T     (Pattern B: ownership transfers; the
        //                                  result type is the element type T;
        //                                  `owned`-ness is tracked by the
        //                                  ownership pass, not by TypeRef)
        // Awaiting any other task shape (async-iterator MoveNextAsync, etc.)
        // is typed elsewhere — see the deferral note below.
        TypeRef* ot = ue.operand ? ctx_.exprType(*ue.operand) : nullptr;
        // §30.2 / Appendix C.7 (#444 / #509): `await` requires a Task /
        // Task<T> / OwnedTask<T> operand. When the operand's type is known and
        // is NOT one of those three forms, reject it with a clean diagnostic
        // rather than silently miscompiling.
        // NOTE: as of #509, the parser correctly handles `Method<T>(…)` as an
        // explicit-type-arg call (§14.1) so `Task.FromResult<int>(42)` now
        // resolves to `Task<int>` and passes through this guard normally.
        // The guard remains for other non-Task operands (plain integer literals,
        // arbitrary bool expressions, etc.).  `await for` desugaring passes
        // Task<bool> through this path, which IS a Named Task type and is NOT
        // flagged; any unresolved / unknown type (ot == nullptr) is left
        // undiagnosed to avoid false positives.
        if (ot) {
            bool isTaskType = false;
            if (ot->kind == TypeRef::Kind::Named && !ot->nullable &&
                ot->astNode && ot->astNode->name &&
                ot->astNode->name->parts.size() == 1) {
                std::string_view head = ot->astNode->name->parts.front();
                isTaskType = (head == "Task" || head == "OwnedTask");
            }
            if (!isTaskType) {
                ctx_.diagnostics().report(
                    Severity::Error, ue.span.begin,
                    "`await` requires a `Task`, `Task<T>`, or `OwnedTask<T>` "
                    "operand (§30.2 / §30.3).");
                return;
            }
        }
        if (ot && ot->kind == TypeRef::Kind::Named && !ot->nullable &&
            ot->astNode && ot->astNode->name &&
            ot->astNode->name->parts.size() == 1) {
            std::string_view head = ot->astNode->name->parts.front();
            if (head == "Task") {
                if (ot->genericArgs.empty()) {
                    // `await Task` → unit (no result value).
                    auto* unit = ctx_.makeTypeRef();
                    unit->kind = TypeRef::Kind::Unit;
                    ctx_.setExprType(&ue, unit);
                } else if (ot->genericArgs.front()) {
                    // `await Task<T>` → T (Pattern A; element type).
                    ctx_.setExprType(&ue, ot->genericArgs.front());
                }
            } else if (head == "OwnedTask" && !ot->genericArgs.empty() &&
                       ot->genericArgs.front()) {
                // `await OwnedTask<T>` → T (Pattern B; element type).
                ctx_.setExprType(&ue, ot->genericArgs.front());
            }
            // NOTE (deferred, §30.7): the result typing specific to async
            // iteration — `await for` desugaring's `MoveNextAsync()` / the
            // owned-yield `GetOwnedAsyncEnumerator` path — is deeper than this
            // increment (it depends on the for-loop iteration-variable typing
            // and the IAsyncEnumerator<T> interface members) and is left to
            // the async-iterator typing milestone. 3b scopes the await result
            // rule to the Task / Task<T> / OwnedTask<T> forms above.
        }
        return;
    }
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
            // §28.13 / stdlib §2.16 (#603): `out using var x` declares a
            // using-var–scoped Shared<T> handle from Weak<T>.TryUpgrade.
            // Mirror the `out var` path above but skip the formal-param
            // type inference (TryUpgrade is a builtin with no MethodDecl)
            // and instead derive the type directly from the receiver:
            //   Weak<T> receiver → Shared<T> out type.
            if (a.modifier == "out using var" && a.value && currentScope_) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                                   a.value.get())) {
                    auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                    lsym->name = id->name;
                    lsym->decl = id;
                    // Derive Shared<T> from Weak<T> receiver type.
                    if (auto* maCallee =
                            dynamic_cast<const ast::MemberAccessExpr*>(
                                ce->callee.get())) {
                        if (maCallee->target) {
                            if (const TypeRef* rt =
                                    ctx_.exprType(*maCallee->target)) {
                                if (rt->kind == TypeRef::Kind::Named &&
                                    !rt->declaration && rt->astNode &&
                                    rt->astNode->name &&
                                    !rt->astNode->name->parts.empty() &&
                                    rt->astNode->name->parts[0] == "Weak" &&
                                    !rt->genericArgs.empty() &&
                                    rt->genericArgs[0]) {
                                    // Build a synthetic Shared<T> astNode
                                    // (same lambda pattern as kWeakNode in
                                    // ExprTypecheck_Calls.cpp ~L1762).
                                    static const ast::Type* kSharedNode =
                                        []() -> ast::Type* {
                                            auto* qn = new ast::QualifiedName;
                                            qn->parts.push_back("Shared");
                                            auto* t  = new ast::Type;
                                            t->kind  = ast::TypeKind::Named;
                                            t->name.reset(qn);
                                            return t;
                                        }();
                                    TypeRef* sharedTy = ctx_.makeTypeRef();
                                    sharedTy->kind = TypeRef::Kind::Named;
                                    sharedTy->declaration = nullptr;
                                    sharedTy->astNode = kSharedNode;
                                    sharedTy->genericArgs.push_back(
                                        const_cast<TypeRef*>(rt->genericArgs[0]));
                                    lsym->declType = sharedTy;
                                }
                            }
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
        // §30.6 (#488): an `async` lambda is the intersection of a closure
        // (§27.5) and an async state machine (§30.1/§30.2) — codegen lowers it
        // via emitAsyncLambda (the closure entry drives the state machine; the
        // captures + params round-trip the async frame). No diagnostic here:
        // the async-readiness gate below (`inAsyncContext_ = lam->isAsync`)
        // makes `await` valid inside the body, and the lambda's function type is
        // inferred normally (the funcReturn is the body's natural result T — the
        // awaited value). The lambda's `(…) -> Task<T>` form is supplied by the
        // call-site annotation (a typed local / function-typed parameter), which
        // codegen reads as the authoritative Task<T> at the closure-call site;
        // function-to-function assignability is not signature-checked (§5.1
        // function-type matching is a future slice), so the inferred `(…) -> T`
        // and the annotated `(…) -> Task<T>` coexist without a spurious error.
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
        // async-readiness 3b (§30.2 / §30.6): the lambda is its own async
        // boundary — `await` inside it is valid iff the lambda is `async`,
        // regardless of the enclosing method's async-ness.
        bool saveAsync = inAsyncContext_;
        inAsyncContext_ = lam->isAsync;
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
        inAsyncContext_ = saveAsync;
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
                // §19.3: "Destructuring applies to records only — classes do
                // not have a positional-destructuring surface … To match a
                // class hierarchy, use a type-test pattern (`Dog`, `var d:
                // Dog`) and access fields through the bound name."
                // Detect whether the named type is a class and, if so, emit
                // a compile error. We check BEFORE the record search so that
                // a class whose name happens to shadow a record is caught.
                for (const auto& sym : ctx_.symbols()) {
                    if (auto* ts = dynamic_cast<TypeSymbol*>(sym.get())) {
                        if (ts->name == lastName &&
                            ts->kind == ast::TypeDeclKind::Class) {
                            ctx_.diagnostics().report(
                                Severity::Error, dp->span.begin,
                                "positional destructuring pattern `" +
                                std::string(lastName) +
                                "(...)` is not valid on a class type — "
                                "classes do not have a positional-"
                                "destructuring surface (§19.3); use a "
                                "type-test pattern (`" +
                                std::string(lastName) +
                                "` or `var x: " +
                                std::string(lastName) +
                                "`) instead");
                            break;
                        }
                    }
                }
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
                    TypeRef* fieldTy = nullptr;
                    if (i < fields.size() && fields[i]->posParam &&
                        fields[i]->posParam->type) {
                        fieldTy = ctx_.resolvedType(*fields[i]->posParam->type);
                    }
                    if (!sp.nakedBinding.empty()) {
                        // Bare `Pt(x, y)` — nakedBinding carries the name.
                        auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                        lsym->name = sp.nakedBinding;
                        lsym->decl = reinterpret_cast<const ast::Node*>(&sp);
                        lsym->declType = fieldTy;
                        armScope.names[sp.nakedBinding] = lsym;
                    } else if (auto* vbp = dynamic_cast<const ast::BindingPattern*>(
                                   sp.pattern.get())) {
                        // §19.3: `Pt(var x, var y)` — equivalent to bare form;
                        // register the bound name keyed on the BindingPattern
                        // node (codegen keys its slot on the same pointer).
                        auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                        lsym->name = vbp->name;
                        lsym->decl = vbp;
                        lsym->declType = vbp->type
                            ? ctx_.resolvedType(*vbp->type) : fieldTy;
                        armScope.names[vbp->name] = lsym;
                    }
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
        // §19.6: an expression-position `case` whose value cannot be proven
        // to be covered by some arm raises MatchFailureException at runtime;
        // the compiler emits a non-exhaustiveness warning here. Statement-
        // position `case` (CaseStmt) is exempt — an unmatched value simply
        // falls through with no action (§19.6).
        checkCaseExhaustiveness(*ce, scrutTy);
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
        // fix #499 / §9.8: give the RangeExpr itself a Range-named type so
        // that `var r := a..b` infers lsym->declType = Range (not null).
        // The element type is inferred from lo (or hi if lo is absent); it is
        // recorded as the first generic arg so codegen can read it back when
        // choosing the integer width for the Range struct's lo/hi fields.
        // A Named TypeRef with declaration = builtinClassSym("Range") and
        // declaration->decl = nullptr is the same shape used for DateTime /
        // TimeSpan (scalar/struct value builtins), and lowerTypeRef will
        // recognise it by declaration->name == "Range".
        {
            TypeRef* elemTy = nullptr;
            if (re->lo)  elemTy = ctx_.exprType(*re->lo);
            if (!elemTy && re->hi) elemTy = ctx_.exprType(*re->hi);
            auto* rt = ctx_.makeTypeRef();
            rt->kind = TypeRef::Kind::Named;
            rt->declaration = builtinClassSym("Range");
            if (elemTy) rt->genericArgs.push_back(elemTy);
            ctx_.setExprType(&e, rt);
        }
    }
    // Identifier / This / Base / TypeOf / NameOf / Default —
    // no expression children to recurse into.
}

} // namespace bezel::sema
