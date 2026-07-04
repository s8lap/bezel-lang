#include "sema/ExprTypecheck_impl.hpp"

#include <unordered_set>

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

// §18.5 / stdlib §2.18 (v0.172, #812/#1339): the FORMAT portion of an encoded
// InterpPart::formatSpec (the codegen encoding is `[","WIDTH][":"FORMAT]`). A
// bare alignment (`,WIDTH` with no `:FORMAT`) carries no format and applies to
// any operand. Returns the format text (empty when there is none).
std::string_view interpFormatPortion(const std::string& spec) {
    std::string_view s = spec;
    if (s.empty()) return {};
    if (s.front() == ',') {
        auto colon = s.find(':');
        return (colon == std::string_view::npos) ? std::string_view()
                                                  : s.substr(colon + 1);
    }
    return s;
}

// §2.18: the intrinsically-formattable primitives (integers, floating, decimal).
// bool/char/string/Object are NOT formattable — a format spec on them is a
// compile error.
bool isIntrinsicallyFormattablePrim(PrimitiveType p) {
    using P = PrimitiveType;
    switch (p) {
        case P::Byte:  case P::SByte: case P::Short: case P::UShort:
        case P::Int:   case P::UInt:  case P::Long:  case P::ULong:
        case P::Float: case P::Double: case P::Decimal:
            return true;
        default:
            return false;  // Bool, Char, String, Object, None
    }
}

// The last name component of a Named TypeRef (declaration name, else the
// AST/imported spelling). Empty when unavailable.
std::string_view namedTypeName(const TypeRef* t) {
    if (!t || t->kind != TypeRef::Kind::Named) return {};
    if (t->declaration && !t->declaration->name.empty())
        return t->declaration->name;
    if (t->astNode && t->astNode->name && !t->astNode->name->parts.empty())
        return t->astNode->name->parts.back();
    if (!t->importedName.empty()) return t->importedName;
    return {};
}

// §2.18: the intrinsically-formattable temporal + Guid builtins.
bool isIntrinsicTemporalOrGuid(std::string_view n) {
    return n == "DateTime" || n == "DateOnly" || n == "TimeOnly" ||
           n == "TimeSpan" || n == "Guid";
}

// §18.5 (v0.172): does `ts` (transitively) declare `: IFormattable`? IFormattable
// is a null-declaration builtin interface, so it never lands in `baseTypes`;
// walk each level's AST base-type list by name, recursing through resolved base
// classes / user interfaces for the `interface IMyFmt: IFormattable` case.
bool declaresIFormattable(const TypeSymbol* ts) {
    if (!ts) return false;
    if (const auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl))
        for (const auto& bt : td->baseTypes)
            if (bt && bt->name && !bt->name->parts.empty() &&
                bt->name->parts.back() == "IFormattable")
                return true;
    for (const TypeSymbol* b : ts->baseTypes)
        if (declaresIFormattable(b)) return true;
    return false;
}

// §18.5 / §2.18 (v0.172): may a hole with a real format spec apply to an operand
// of type `t`? Nullable is transparent (null renders "null"; the spec applies to
// the payload). An unresolved generic type parameter is admitted conservatively
// (its concrete formattability is a bound-analysis question left to codegen).
bool isFormattableOperandType(const TypeRef* t) {
    if (!t) return true;  // already errored elsewhere; don't cascade
    if (t->kind == TypeRef::Kind::Primitive)
        return isIntrinsicallyFormattablePrim(t->primitive);
    if (t->kind == TypeRef::Kind::Named) {
        std::string_view n = namedTypeName(t);
        if (isIntrinsicTemporalOrGuid(n)) return true;
        if (t->declaration && declaresIFormattable(t->declaration)) return true;
        // Unresolved generic type parameter (no declaration): admit.
        if (!t->declaration) return true;
        return false;  // plain class/record/enum/interface, Object
    }
    return true;  // non-Named/non-Primitive (unit/function/array): not reached
                  // with a spec in practice; don't spuriously error.
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

// §6.2/#1204: returns true when `e` is a "literal-derived expression" — built
// entirely from integer or float literals via unary/binary operators.  Such
// expressions have no declared signed/unsigned type of their own and are
// compatible with any numeric operand in a comparison (e.g. `ulong > 1000`,
// `ulong > 0 - 5`).  Only named variable references or non-literal sub-exprs
// cause this to return false, triggering the signed↔unsigned rejection below.
static bool isLiteralDerivedExpr(const ast::Expr* e) {
    if (!e) return false;
    if (dynamic_cast<const ast::IntLiteralExpr*>(e))   return true;
    if (dynamic_cast<const ast::FloatLiteralExpr*>(e)) return true;
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(e))
        return isLiteralDerivedExpr(ue->operand.get());
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(e))
        return isLiteralDerivedExpr(be->left.get()) &&
               isLiteralDerivedExpr(be->right.get());
    return false;
}

// #625 / §12 / §31: true when `name` is a built-in exception type identifier.
// Mirrors the isBuiltinExName lambda in ExprTypecheck_Calls.cpp (kept in sync).
static bool isBuiltinExceptionTypeName(std::string_view n) {
    return n == "Exception"              ||
           n == "ArgumentException"      ||
           n == "ArgumentNullException"  ||
           n == "ArithmeticException"    ||
           n == "OverflowException"      ||
           n == "DivideByZeroException"  ||
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
           // #976 / stdlib §7.4 / §7.5: System.Threading + System.IO families.
           n == "TimeoutException"          ||
           n == "IOException"               ||
           n == "FileNotFoundException"     ||
           n == "DirectoryNotFoundException"||
           n == "EndOfStreamException"      ||
           n == "PathTooLongException";
}

// #625 / §12 / §31: true when `ts` (a user class TypeSymbol) or any user-class
// ancestor has a direct AST-declared base that is a built-in exception.
// Used by the unqualified identifier resolver to allow bare `Message` in the
// body of an exception-derived class without "unresolved identifier".
static bool inheritsBuiltinException(const TypeSymbol* ts) {
    if (!ts || ts->kind != ast::TypeDeclKind::Class) return false;
    for (const TypeSymbol* cur = ts; cur;) {
        auto* td = dynamic_cast<const ast::TypeDecl*>(cur->decl);
        if (td) {
            for (const auto& bt : td->baseTypes) {
                if (!bt || !bt->name || bt->name->parts.empty()) continue;
                if (isBuiltinExceptionTypeName(bt->name->parts.back()))
                    return true;
            }
        }
        // Walk to the next user-class base.
        const TypeSymbol* next = nullptr;
        for (const TypeSymbol* b : cur->baseTypes) {
            if (b && b->kind == ast::TypeDeclKind::Class && b->decl) {
                next = b;
                break;
            }
        }
        cur = next;
    }
    return false;
}

}  // namespace

// §17.11 (#951): does `default(T)` have no well-defined zero value? See the
// header declaration. Recurses into a record's positional fields and an inline
// array's element type.
bool ExprTC::defaultIllFormed(const TypeRef* t) {
    if (!t || t->nullable) return false;  // nullable defaults to null — OK
    switch (t->kind) {
        case TypeRef::Kind::Primitive:
            // `string` is a runtime-managed reference (§4.6) with no zero. Value
            // primitives (int/bool/char/…) have a zero. (Non-nullable Object is
            // also a reference type, but left out here to keep this consistent
            // with the original #951 scope — a separate consideration.)
            return t->primitive == PrimitiveType::String;
        case TypeRef::Kind::Function:
            return true;  // a function value (§28.9) has no zero
        case TypeRef::Kind::InlineArray:
            return defaultIllFormed(t->arrayElement);
        case TypeRef::Kind::Named: {
            if (!t->declaration) return false;  // generic param / unresolved
            auto k = t->declaration->kind;
            if (k == ast::TypeDeclKind::Class ||
                k == ast::TypeDeclKind::Interface)
                return !t->declaration->isValueBuiltin;
            if (k == ast::TypeDeclKind::Enum) return false;
            if (k == ast::TypeDeclKind::Record) {
                auto* td =
                    dynamic_cast<const ast::TypeDecl*>(t->declaration->decl);
                if (!td) return false;  // imported record — defer (don't reject)
                for (const auto& pf : td->positionalFields) {
                    if (!pf.type) continue;
                    if (defaultIllFormed(ctx_.resolvedType(*pf.type)))
                        return true;
                }
                return false;
            }
            return false;
        }
        default:
            return false;  // Unit / Error
    }
}

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
            // §10.6 (#772): a bare enum constant `Green` resolved via the
            // case-scrutinee enum scope injection carries the enum's type, so an
            // arm body that yields it has a determinable common arm type (§20).
            // GATED on `inEnumScrutineeCaseArms_` so it applies ONLY inside such
            // a case's arms — NOT in an enum-constant initializer like
            // `C := A + 100`, where a bare sibling `A` is its integer value for
            // arithmetic (typing it as the enum there breaks the fold). The
            // qualified `Color.Green` form gets its type via MemberAccess.
            // // per §10.6
            if (inEnumScrutineeCaseArms_) {
                if (auto* ec = dynamic_cast<EnumConstantSymbol*>(sym)) {
                    if (ec->parent) {
                        auto* et = ctx_.makeTypeRef();
                        et->kind = TypeRef::Kind::Named;
                        et->declaration = ec->parent;
                        ctx_.setExprType(&e, et);
                    }
                }
            }
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
                // §14 / §12 (#989): when the field is INHERITED from a generic
                // base (fs->parent has generic params), call substituteMemberType
                // with a null target (= implicit `this`) so the base's T-arg is
                // resolved from currentType_'s base-chain — exactly as the
                // MemberAccessExpr path does for explicit `this.field` access.
                if (auto* fd =
                        dynamic_cast<const ast::FieldDecl*>(fs->decl)) {
                    if (fd->type) {
                        if (auto* rt = ctx_.resolvedType(*fd->type)) {
                            ctx_.setExprType(
                                &e, substituteMemberType(rt, nullptr,
                                                         fs->parent));
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
                        ctx_.setExprType(
                            &e, substituteMemberType(rt, nullptr, fs->parent));
                    }
                }
            } else if (auto* ps = dynamic_cast<PropertySymbol*>(sym)) {
                // §12.1 / §23: implicit `this.<auto-property>` form: a bare
                // identifier resolving to a class or record auto-property
                // (bodyKind == Auto).  The property's declared type gives the
                // expression type so that downstream checks (isStringExprType,
                // isStringFieldLvalue, etc.) see the correct type.
                // Without this, `Name := name` in a ctor body where `Name` is
                // a string auto-property has exprType=null → lowerStringFieldStore
                // skips the retain → the ctor stores the string pointer without
                // incrementing the rc, causing a reference-count imbalance when
                // the synthesized deserializer attempts to release the binder's
                // reference after the ctor has "consumed" it via plain store.
                if (!ps->isIndexer) {
                    if (auto* pd =
                            dynamic_cast<const ast::PropertyDecl*>(ps->decl)) {
                        if (pd->type) {
                            if (auto* rt = ctx_.resolvedType(*pd->type)) {
                                ctx_.setExprType(&e, rt);
                            }
                        }
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
                   id->name == "Array" || id->name == "OwningArray" ||
                   id->name == "Dictionary" || id->name == "OwningDictionary" ||
                   id->name == "Stack" || id->name == "OwningStack" ||
                   id->name == "Queue" || id->name == "OwningQueue" ||
                   id->name == "Set" || id->name == "OwningSet" ||
                   id->name == "LinkedList" || id->name == "OwningLinkedList" ||
                   id->name == "StringBuilder" || id->name == "Encoding" ||
                   id->name == "DateTime" || id->name == "TimeSpan" ||
                   // stdlib §2.5 / #1357: DateOnly and TimeOnly are auto-imported
                   // value-type builtins with static Parse/TryParse factory methods;
                   // recognized here so DateOnly.Parse(…)/TimeOnly.Parse(…) resolve.
                   id->name == "DateOnly" || id->name == "TimeOnly" ||
                   // stdlib §2.6: Guid is auto-imported (a builtin value type).
                   id->name == "Guid" ||
                   // stdlib §9.4 / issue #654: Boolean is auto-imported (a builtin
                   // record type, alias `bool`).  Needed for Boolean.TryParse.
                   id->name == "Boolean" ||
                   // stdlib §2.12 / issue #654: BitConverter — static class for
                   // IEEE 754 bit-reinterpretation (FloatToInt32Bits etc.).
                   id->name == "BitConverter" ||
                   // stdlib §9.3: Decimal is auto-imported (a builtin value type).
                   // fix #521: needed for Decimal.Parse, Decimal.Zero, etc.
                   id->name == "Decimal" || id->name == "decimal" ||
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
                   id->name == "Domain" ||
                   // §8.1 / issue #646: Stopwatch — static factory StartNew() and
                   // instance methods. Recognized here so Stopwatch.StartNew() and
                   // sw.Start()/Stop()/… resolve as calls on a known-class receiver.
                   id->name == "Stopwatch" ||
                   // §8.2 / issue #646: Debug — static class for Assert/WriteLine.
                   // Recognized here so Debug.Assert(…) and Debug.WriteLine(…)
                   // resolve as calls on a known-class receiver.
                   id->name == "Debug" ||
                   // stdlib §10.1/§10.2/§10.3 / §32.6 / fix #977: CString, WString,
                   // RawBytes are FFI-boundary classes with static factory methods
                   // (CString.From(s), WString.From(s), RawBytes.From(buffer)).
                   // They resolve as builtin class receivers so member access on
                   // the type name (CString.From) is accepted in expression position.
                   id->name == "CString" ||
                   id->name == "WString" ||
                   id->name == "RawBytes") {
            // §2.3: these System classes are auto-imported and always
            // available without an explicit import. Give them a synthetic
            // TypeSymbol (null decl) so member access can proceed; the
            // codegen handles known methods (Write/WriteLine, etc.)
            // as intrinsics.
            auto* ts = builtinClassSym(id->name);
            ctx_.setExprSymbol(&e, ts);
        } else if (id->name == "Message" && currentType_ &&
                   !inStaticMember_ &&
                   inheritsBuiltinException(currentType_)) {
            // #625 / §12 / §31: unqualified `Message` inside an INSTANCE method
            // body of a class that inherits (directly or via a user-class chain)
            // from a built-in exception type resolves as the inherited stdlib
            // Exception.Message property (a string read of the slot at byte
            // offset 8 in the exception layout).  No user TypeSymbol or
            // FieldSymbol exists for it; the codegen's GEP-at-byte-8 path
            // handles the read (same as `this.Message` — see the
            // MemberAccessExpr case in LlvmBackend_Exprs.cpp CG22+).
            // Static methods are excluded: there is no `this` in a static
            // context so Message would have no meaningful receiver.
            // Do NOT set a symbol — there is none; just set the type to string
            // so downstream (type-assignability, .Length, etc.) can use it.
            ctx_.setExprType(&e,
                ctx_.primitiveTypeRef(PrimitiveType::String));
        } else if (
            // §2.2 v0.164 / §2.7 / #1365: an import alias (e.g. `ML` in
            // `import MathLib as ML;`) or a module-name prefix segment
            // (e.g. `MathLib` in `MathLib.Calc.Square(4)`) is NOT a type
            // symbol in scope, but is a valid qualifier in a dotted member-
            // access chain.  Suppress "unresolved identifier" so the
            // enclosing MemberAccessExpr's resolveMemberAccess can handle
            // the full qualifier correctly (§2.7 rule 1 for aliases, rule 2
            // for module-qualified names).
            !importAliases_.empty() &&
            importAliases_.count(std::string(id->name))) {
            // Import alias: silently pass — resolveMemberAccess will handle
            // the enclosing `Alias.TypeName` pattern.
        } else if (
            !sourceImports_.empty() && [&]() {
                const std::string name(id->name);
                // Exact match: `MathLib` in `import MathLib;`.
                if (sourceImports_.count(name)) return true;
                // Prefix match: `BankingApp` when `BankingApp.Txn` is imported.
                for (const std::string& mod : sourceImports_) {
                    if (mod.size() > name.size() &&
                        mod.substr(0, name.size()) == name &&
                        mod[name.size()] == '.')
                        return true;
                }
                return false;
            }()) {
            // Module-name prefix: silently pass — resolveMemberAccess will
            // handle the enclosing `ModuleName.TypeName` pattern.
        } else if (&e == maTargetHint_ && isInScopeTypeParam(id->name)) {
            // §11.8 (v0.176, #807): `T.Member(...)` — the receiver of a
            // member access names an in-scope generic type parameter. Mark it
            // with a GenericParamSymbol so resolveMemberAccess dispatches
            // through the parameter's static-interface constraint. A bare `T`
            // in any OTHER expression position (not a member-access target)
            // keeps today's "unresolved identifier" error.
            ctx_.setExprSymbol(&e, typeParamMarker(id->name));
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
        } else {
            // §4.2 / §5 (#749, #1155): an explicitly L/U/UL-suffixed literal
            // names its type, and its value must be representable in that
            // type's range. A magnitude that overflows 64 bits is
            // unrepresentable in any integer type (e.g. `99999999999999999999999UL`,
            // which once silently miscompiled an enum member to 0, #749). A
            // magnitude that fits 64 bits but exceeds the *named* suffix's range
            // — an `L` literal in (2^63, 2^64), or a `U` literal in (2^32, 2^64)
            // — does not fit the type the suffix selects and once silently
            // wrapped (`9223372036854775808L` → long.MinValue, `5000000000U` →
            // wrapped uint, #1155). Reject both.
            unsigned long long mag = 0;
            if (intLiteralMagnitude(il->text, mag)) {
                if (errno == ERANGE) {
                    ctx_.diagnostics().report(
                        Severity::Error, il->span.begin,
                        "integer literal is too large to represent (exceeds the "
                        "64-bit range of any integer type) (§5)");
                } else {
                    // §4.2: per-type maximum representable magnitude. `long`
                    // admits |long.MinValue| = 2^63 only as the direct operand
                    // of a unary minus (#1155); a bare `long` literal caps at
                    // long.MaxValue = 2^63 − 1.
                    using P = PrimitiveType;
                    unsigned long long maxMag;
                    switch (pt) {
                        case P::Long:
                            maxMag = literalUnderUnaryMinus_
                                         ? 9223372036854775808ULL   // 2^63
                                         : 9223372036854775807ULL;  // 2^63 − 1
                            break;
                        case P::UInt:
                            maxMag = 4294967295ULL;  // 2^32 − 1
                            break;
                        case P::ULong:
                        default:
                            maxMag = 18446744073709551615ULL;  // 2^64 − 1
                            break;
                    }
                    if (mag > maxMag) {
                        ctx_.diagnostics().report(
                            Severity::Error, il->span.begin,
                            std::string("integer literal does not fit in `") +
                            primitiveSpelling(pt) + "` (§4.2)");
                    }
                }
            }
        }
        ctx_.setExprType(&e, ctx_.primitiveTypeRef(pt));
        return;
    }
    if (auto* fl = dynamic_cast<const ast::FloatLiteralExpr*>(&e)) {
        // §18.1 / §4.4: the `f`/`F` suffix gives type `float` (f32);
        // `d`/`D` or no suffix gives `double` (f64); `m`/`M` gives `decimal`.
        // per §4.4 (fix #521): decimal is fully implemented as a 128-bit type
        // with 96-bit mantissa + scale + sign (.NET-compatible layout).
        PrimitiveType pt = PrimitiveType::Double;
        if (!fl->text.empty()) {
            char last = fl->text.back();
            if (last == 'f' || last == 'F') {
                pt = PrimitiveType::Float;
            } else if (last == 'm' || last == 'M') {
                // §4.4: `m`/`M` suffix → decimal literal.
                pt = PrimitiveType::Decimal;
            }
            // `d`/`D` → double (already default).
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
        // §18.5 dispatch rule (v0.172, #812/#1339): a hole WITH a real format
        // spec (not a bare `,`alignment) requires an operand that is either
        // intrinsically formattable (a numeric/temporal/Guid) or implements the
        // auto-imported `IFormattable` interface (stdlib §2.18). A format spec on
        // any other operand — bool/char/string/Object or a plain user type — is a
        // compile error naming this rule. Alignment-only holes are unrestricted.
        for (const auto& part : is->parts) {
            if (!part.expr) continue;
            if (interpFormatPortion(part.formatSpec).empty()) continue;
            const TypeRef* t = ctx_.exprType(*part.expr);
            if (t && t->kind != TypeRef::Kind::Error &&
                !isFormattableOperandType(t)) {
                std::string what;
                if (t->kind == TypeRef::Kind::Primitive)
                    what = std::string("`") + primitiveSpelling(t->primitive) + "`";
                else if (!namedTypeName(t).empty())
                    what = std::string("`") + std::string(namedTypeName(t)) + "`";
                else
                    what = "this operand";
                ctx_.diagnostics().report(
                    Severity::Error, e.span.begin,
                    "a format spec requires an intrinsically-formattable operand "
                    "(a numeric/temporal/Guid) or one implementing `IFormattable`; "
                    + what + " is neither (\xC2\xA7""18.5)");
            }
        }
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
    // §4.2 (#1155): flag a unary-minus's direct integer-literal operand so
    // its range-check admits the negative-boundary magnitude (long.MinValue =
    // -2^63, written as `-9223372036854775808L`).
    bool savedNegLit = literalUnderUnaryMinus_;
    if (auto* ue = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        literalUnderUnaryMinus_ =
            (ue->op == "-" && ue->operand &&
             dynamic_cast<const ast::IntLiteralExpr*>(ue->operand.get()) !=
                 nullptr);
    } else {
        literalUnderUnaryMinus_ = false;
    }
    visitChildren(e);
    literalUnderUnaryMinus_ = savedNegLit;
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
                if (tt->kind == TypeRef::Kind::InlineArray && tt->arrayElement) {
                    // §15.6 (#746 R41-E): "Slicing is not supported on inline
                    // arrays in this version." A range index on an inline array
                    // otherwise reached codegen and emitted a `%bz.range` struct
                    // as a GEP index (malformed IR); reject it cleanly here.
                    if (!ix->indices.empty() && ix->indices[0].value &&
                        dynamic_cast<const ast::RangeExpr*>(
                            ix->indices[0].value.get()))
                        ctx_.diagnostics().report(
                            Severity::Error, e.span.begin,
                            "range slicing `[a..b]` is not supported on inline "
                            "array types `[N]T` (§15.6)");
                    ctx_.setExprType(&e, tt->arrayElement);
                }
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
                    // R33: also a Range-typed VALUE — `var r := 0..2; s[r]` —
                    // where the index is a Range<int> variable / call result,
                    // not a syntactic range literal. Without this the result is
                    // mistyped `char`, the slice value is coerced away, and the
                    // codegen range-slice produces an empty string.
                    if (!rangeIndex && !ix->indices.empty() &&
                        ix->indices[0].value) {
                        if (auto* it = ctx_.exprType(*ix->indices[0].value))
                            if (it->kind == TypeRef::Kind::Named &&
                                it->declaration == builtinClassSym("Range"))
                                rangeIndex = true;
                    }
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
                //   * Array<T> / OwningArray<T> (stdlib §4.4 / §4.5): the
                //     indexer `[i: int]: T` result is T = genericArgs[0].
                //   * Dictionary<K,V> / OwningDictionary<K,V> (§4.8 / §4.9): the
                //     indexer `[key: K]: V` result is V = genericArgs[1].
                //   * issue #401 / §4.1 / §4.2: the int-indexed list INTERFACES
                //     IReadOnlyList<T> (get-only), IList<T> (get/set borrowing),
                //     and IOwningList<T> (get/owned-set) each declare an indexer
                //     `[i: int]: T`; the element type is T = genericArgs[0], the
                //     same position as Array<T>. (No collection interface
                //     declares a key-indexer, so dictionaries are concrete-only.)
                else if (tt->kind == TypeRef::Kind::Named && !tt->declaration &&
                         tt->astNode && tt->astNode->name &&
                         !tt->astNode->name->parts.empty()) {
                    std::string_view cn = tt->astNode->name->parts[0];
                    // issue #545: Buffer<T>[i] → T (genericArgs[0]).
                    if (cn == "Buffer" && !tt->genericArgs.empty() &&
                        tt->genericArgs[0]) {
                        ctx_.setExprType(&e, tt->genericArgs[0]);
                    }
                    bool isArrLike =
                        cn == "Array" || cn == "OwningArray" ||
                        // §4.14 (#743/f11): OwningBuffer<T>[i] is element-indexed
                        // exactly like OwningArray<T> — its result type is the
                        // element T = genericArgs[0]. Without this it fell to the
                        // untyped path, where the ownership checker assumed a
                        // borrow and falsely rejected `return buf[0]` for a
                        // value-type T (§28.3 borrow-lifetime).
                        cn == "OwningBuffer" ||
                        // issue #401: list interfaces are array-backed; their
                        // indexer result type is the element T = genericArgs[0].
                        cn == "IReadOnlyList" || cn == "IList" ||
                        cn == "IOwningList";
                    // §4.15 (#549) / §4.14 (#779): ArraySpan/ReadOnlyArraySpan
                    // (borrowing) and Span/ReadOnlySpan (owning) indexer returns
                    // T = genericArgs[0].
                    bool isSpan =
                        cn == "ArraySpan" || cn == "ReadOnlyArraySpan" ||
                        cn == "Span" || cn == "ReadOnlySpan";
                    bool isDict =
                        cn == "Dictionary" || cn == "OwningDictionary";
                    if ((isArrLike || isSpan) && !tt->genericArgs.empty() &&
                        tt->genericArgs[0]) {
                        // §15.6 / fix #690: `arr[range]` on Array<T> produces
                        // a new Array<T> — the result type is the ARRAY type
                        // (tt), not the element type (T). Only Array<T> supports
                        // range slicing; OwningArray<T>, span types, and list
                        // interfaces use the scalar-element path.
                        bool isRangeIdx =
                            !ix->indices.empty() && ix->indices[0].value &&
                            dynamic_cast<const ast::RangeExpr*>(
                                ix->indices[0].value.get()) != nullptr;
                        // §15.6 (#1079): also a Range-typed VALUE — `var r := 0..2;
                        // arr[r]` — not a syntactic range literal. Mirrors the
                        // string-slice path above. Without this `arr[r]` was typed
                        // as the element T (not Array<T>), so `.Count` read garbage
                        // and `for var x in arr[r]` found no collection-typed
                        // iterable ("codegen for this for statement" reject). The
                        // codegen indexer-get already lowers a Range-variable
                        // subscript via bz_array_slice; only the type was missing.
                        if (!isRangeIdx && !ix->indices.empty() &&
                            ix->indices[0].value) {
                            if (auto* it = ctx_.exprType(*ix->indices[0].value))
                                if (it->kind == TypeRef::Kind::Named &&
                                    it->declaration == builtinClassSym("Range"))
                                    isRangeIdx = true;
                        }
                        // §15.6 (#746 R41-E): range slicing produces a NEW Array<T>
                        // of copied references; it is admitted only on Array<T>.
                        // "Slicing is not admitted on OwningArray<T>" — the
                        // copy-returning semantics would manufacture a second
                        // owner for each owned element. Reject it instead of
                        // silently typing the (miscompiled) result as the element.
                        if (isRangeIdx && cn == "OwningArray")
                            ctx_.diagnostics().report(
                                Severity::Error, e.span.begin,
                                "range slicing `[a..b]` is not admitted on "
                                "`OwningArray<T>` — it would manufacture a second "
                                "owner for each owned element (§15.6)");
                        if (isRangeIdx && cn == "Array")
                            ctx_.setExprType(&e, tt);
                        else {
                            TypeRef* elem = tt->genericArgs[0];
                            // §4.14 (#1157): for class-typed T, an
                            // `OwningBuffer<T>` stores `T?` in each slot
                            // (nullable references, all null until set). Its
                            // indexer is declared `[i: int]: T?`, so the
                            // indexer-get must be typed `T?` — an empty-slot
                            // read then requires a null guard / `?.` (a bare
                            // `buf[i].member` would null-deref → SIGSEGV) and
                            // `buf[i] := null` (the natural §28.8.1 restoration)
                            // is accepted. Value-typed T (primitive / record /
                            // enum, incl. the value-builtins DateTime/Range/…)
                            // stores `T` directly with value defaults, so its
                            // indexer stays non-nullable `T`. A bare generic
                            // param (no declaration) is left non-nullable —
                            // its category is unknown until instantiation.
                            // per §7.4 / #1157 (#1208): Span<T> and
                            // ReadOnlySpan<T> (owning spans over OwningBuffer<T>)
                            // store `T?` in each slot for class/interface T —
                            // identical semantics to OwningBuffer<T> itself.
                            // ArraySpan / ReadOnlyArraySpan (borrowing views of
                            // Array<T>) store `T` directly (non-nullable) and
                            // are excluded.
                            if ((cn == "OwningBuffer" ||
                                 cn == "Span" || cn == "ReadOnlySpan") &&
                                elem && !elem->nullable &&
                                elem->kind == TypeRef::Kind::Named &&
                                elem->declaration &&
                                (elem->declaration->kind ==
                                     ast::TypeDeclKind::Class ||
                                 elem->declaration->kind ==
                                     ast::TypeDeclKind::Interface) &&
                                !elem->declaration->isValueBuiltin) {
                                TypeRef* nn = ctx_.makeTypeRef(*elem);
                                nn->nullable = true;
                                elem = nn;
                            }
                            ctx_.setExprType(&e, elem);
                        }
                    } else if (isDict && tt->genericArgs.size() > 1 &&
                             tt->genericArgs[1])
                        ctx_.setExprType(&e, tt->genericArgs[1]);
                    // §3.3 (#883): ReadOnlyStringSpan[i] → char (the i-th code
                    // point of the viewed range). Non-generic, so its element
                    // type is the fixed `char` primitive (not genericArgs[0]).
                    else if (cn == "ReadOnlyStringSpan")
                        ctx_.setExprType(
                            &e, ctx_.primitiveTypeRef(PrimitiveType::Char));
                    // #794 / §14.2 + §23.4: `x[i]` where `x` has the type of a
                    // bare generic parameter `T` constrained `where T: IIndexable`
                    // and the constraint interface declares an indexer. The stdlib
                    // branches above don't match (cn names a type parameter, not a
                    // collection), so the IndexExpr was left untyped → the
                    // ownership pass assumed a borrow and the result fell to 0.
                    // Type it as the constraint interface's indexer return type
                    // (codegen then devirtualizes through genericSubstSemaTy_).
                    // Mirrors the #484 method-via-constraint resolver
                    // (ExprTypecheck_Calls.cpp): walk the type parameter's
                    // where-constraints on the enclosing method, then the class.
                    if (!ctx_.exprType(e)) {
                        auto tryIndexerConstraint =
                            [&](const std::vector<ast::GenericParam>& gps,
                                const std::vector<ast::GenericConstraint>& gcs)
                            -> bool {
                            bool isParam = false;
                            for (const auto& gp : gps)
                                if (gp.name == cn) { isParam = true; break; }
                            if (!isParam) return false;
                            for (const auto& gc : gcs) {
                                if (gc.typeParam != cn) continue;
                                for (const auto& item : gc.items) {
                                    if (item.kind != "type" || !item.typeArg)
                                        continue;
                                    TypeRef* ctr =
                                        ctx_.resolvedType(*item.typeArg);
                                    if (!ctr ||
                                        ctr->kind != TypeRef::Kind::Named ||
                                        !ctr->declaration)
                                        continue;
                                    auto* ps = dynamic_cast<PropertySymbol*>(
                                        lookupMember(ctr->declaration, ".this"));
                                    if (!ps || !ps->isIndexer) continue;
                                    auto* pd =
                                        dynamic_cast<const ast::PropertyDecl*>(
                                            ps->decl);
                                    if (!pd || !pd->type) continue;
                                    if (auto* rt = ctx_.resolvedType(*pd->type)) {
                                        ctx_.setExprType(
                                            &e, substituteMemberType(
                                                    rt, ix->target.get(),
                                                    ps->parent));
                                        return true;
                                    }
                                }
                            }
                            return false;
                        };
                        bool done = false;
                        if (currentMethodDecl_ &&
                            !currentMethodDecl_->genericParams.empty())
                            done = tryIndexerConstraint(
                                currentMethodDecl_->genericParams,
                                currentMethodDecl_->genericConstraints);
                        if (!done && currentType_ && currentType_->decl)
                            if (auto* td = dynamic_cast<const ast::TypeDecl*>(
                                    currentType_->decl))
                                if (!td->genericParams.empty())
                                    tryIndexerConstraint(
                                        td->genericParams,
                                        td->genericConstraints);
                    }
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
                    // §23.4 (#1172): resolve the indexer OVERLOAD for
                    // `obj[a, b, ...]` by its FULL parameter list — the
                    // candidate's parameter COUNT must equal the number of index
                    // arguments AND every parameter type must match the
                    // corresponding argument (an exact structural match is
                    // preferred; a sole arity-matching candidate is accepted with
                    // implicit conversion, as the single-indexer path always
                    // has). Candidates are gathered over the receiver type and
                    // its bases (an inherited indexer is a valid candidate),
                    // mirroring lookupMember's frontier walk and the codegen
                    // selector (Emitter::selectIndexer) so codegen lowers exactly
                    // the overload typed here. This replaces the prior
                    // first-parameter-only match (#774), which ignored arity and
                    // later parameters: it selected the wrong overload (`[r,c]`
                    // vs `[r,s]` → IR_FAIL), or a 1-parameter overload for a
                    // 2-argument call (dropping the extra index → WRONG_VALUE),
                    // and accepted an arity-mismatched call outright.
                    const std::size_t nargs = ix->indices.size();
                    std::vector<const TypeRef*> argTys;
                    argTys.reserve(nargs);
                    for (const auto& idxArg : ix->indices)
                        argTys.push_back(idxArg.value
                                             ? ctx_.exprType(*idxArg.value)
                                             : nullptr);
                    auto allParamsMatch =
                        [&](const ast::PropertyDecl* cpd) -> bool {
                        for (std::size_t k = 0; k < nargs; ++k) {
                            const TypeRef* a = argTys[k];
                            const TypeRef* p =
                                cpd->indexerParams[k].type
                                    ? ctx_.resolvedType(
                                          *cpd->indexerParams[k].type)
                                    : nullptr;
                            if (!p || !a || p->kind != a->kind) return false;
                            if (p->kind == TypeRef::Kind::Primitive) {
                                if (p->primitive != a->primitive) return false;
                            } else if (p->kind == TypeRef::Kind::Named) {
                                if (p->declaration != a->declaration)
                                    return false;
                            } else {
                                return false;
                            }
                        }
                        return true;
                    };
                    PropertySymbol* ps = nullptr;        // exact full-list match
                    PropertySymbol* arityOnly = nullptr;  // first arity match
                    bool anyIndexer = false;
                    std::vector<TypeSymbol*> frontier{tt->declaration};
                    for (std::size_t fi = 0; fi < frontier.size() && !ps; ++fi) {
                        TypeSymbol* t = frontier[fi];
                        for (Symbol* m : t->members) {
                            auto* cand = dynamic_cast<PropertySymbol*>(m);
                            if (!cand || !cand->isIndexer) continue;
                            anyIndexer = true;
                            auto* cpd = dynamic_cast<const ast::PropertyDecl*>(
                                cand->decl);
                            if (!cpd || cpd->indexerParams.size() != nargs)
                                continue;
                            if (!arityOnly) arityOnly = cand;
                            if (allParamsMatch(cpd)) { ps = cand; break; }
                        }
                        for (TypeSymbol* b : t->baseTypes)
                            if (b) frontier.push_back(b);
                    }
                    // No exact full-list match: a sole arity-matching candidate
                    // is the selection (the arguments are implicitly converted,
                    // exactly as the single-indexer path always did).
                    if (!ps) ps = arityOnly;
                    if (ps && ps->isIndexer) {
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
                    } else if (anyIndexer && nargs > 0) {
                        // §23.4: the receiver declares indexer(s) but none has
                        // the argument count — e.g. `g[i, j]` against a
                        // 1-parameter indexer (previously accepted, silently
                        // dropping the extra index). Reject cleanly.
                        ctx_.diagnostics().report(
                            Severity::Error, e.span.begin,
                            "no indexer overload matches the " +
                                std::to_string(nargs) +
                                (nargs == 1 ? " argument" : " arguments") +
                                " (§23.4)");
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
        // §22.12 (cf. C# CS0176): a static member must be accessed through the
        // type name (`C._x`), not through an instance (`c._x`). The latter was
        // silently accepted (and previously crashed codegen, #747). Reject it.
        if (ma->target) {
            sema::Symbol* mem = ctx_.exprSymbol(e);
            auto isStaticMod = [](const sema::MemberSymbol* m) {
                for (auto md : m->modifiers)
                    if (md == "static") return true;
                return false;
            };
            bool memStatic = false;
            if (auto* fs = dynamic_cast<sema::FieldSymbol*>(mem))
                memStatic = isStaticMod(fs);
            else if (auto* ms = dynamic_cast<sema::MethodSymbol*>(mem))
                memStatic = isStaticMod(ms) && !ms->isOperator &&
                            ms->extensionDecl == nullptr;
            else if (auto* ps = dynamic_cast<sema::PropertySymbol*>(mem))
                memStatic = isStaticMod(ps);
            if (memStatic) {
                // A type-name receiver (`C._x`) or primitive-keyword receiver
                // (`int.MaxValue`) is fine; an instance value receiver is not.
                sema::Symbol* recv = ctx_.exprSymbol(*ma->target);
                bool recvIsType =
                    dynamic_cast<sema::TypeSymbol*>(recv) != nullptr ||
                    // §11.8 (v0.176, #807): a type-parameter receiver
                    // (`T.Zero()`) IS a type-name receiver — the static
                    // dispatches through the constraint at instantiation.
                    dynamic_cast<sema::GenericParamSymbol*>(recv) != nullptr;
                bool recvIsPrim = false;
                if (auto* rid = dynamic_cast<const ast::IdentifierExpr*>(
                        ma->target.get()))
                    recvIsPrim =
                        primitiveFromName(rid->name) != PrimitiveType::None;
                if (!recvIsType && !recvIsPrim && ctx_.exprType(*ma->target)) {
                    ctx_.diagnostics().report(
                        Severity::Error, ma->span.begin,
                        "static member `" + std::string(ma->member) +
                        "` must be accessed through the type name, not an "
                        "instance (§22.12)");
                }
            }
        }
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
                // §5.4 / §10 (#960): `new` cannot construct an enum. An enum
                // value is produced by the int→enum CAST `E(n)` (§5.4), not by
                // construction — an enum has no constructor (§10). `new E(...)`
                // (and nested `new Outer.E(...)`) were silently accepted; reject
                // and point at the cast form. (The correct cast `E(n)` is a
                // CallExpr, not a NewExpr, and is unaffected.)
                if (rt->kind == TypeRef::Kind::Named && rt->declaration &&
                    rt->declaration->kind == ast::TypeDeclKind::Enum) {
                    ctx_.diagnostics().report(
                        Severity::Error, ne->span.begin,
                        "`new` cannot construct an enum — an enum value is "
                        "produced by the cast `" +
                        std::string(rt->declaration->name) +
                        "(n)` from its underlying integer, not with `new` "
                        "(§5.4/§10)");
                }
                // §14.1 (#992): `new T()` where T is a generic type parameter
                // requires `where T: new()` on the enclosing generic declaration.
                // Without that constraint the body type-checks under the weakest
                // assumption its constraints establish, and construction cannot
                // be assumed available.  `where T: class`, `where T: value`, a
                // type-bound, or no constraint at all are NOT sufficient.
                // Detection: a generic type parameter resolves to a Named TypeRef
                // with declaration==nullptr; we then verify the name matches one
                // of the enclosing method or class generic params and that at
                // least one `new` constraint item is listed for it.
                if (rt->kind == TypeRef::Kind::Named &&
                    rt->declaration == nullptr &&
                    ne->type && ne->type->name &&
                    !ne->type->name->parts.empty()) {
                    std::string_view paramName = ne->type->name->parts.front();
                    // Determine whether paramName is a generic type parameter
                    // in the enclosing method or class, and whether a `new()`
                    // constraint is present for it.
                    bool isTypeParam = false;
                    bool hasNewConstraint = false;
                    auto checkGPs =
                        [&](const std::vector<ast::GenericParam>& gps,
                            const std::vector<ast::GenericConstraint>& gcs) {
                        for (const auto& gp : gps)
                            if (gp.name == paramName) { isTypeParam = true; break; }
                        if (!isTypeParam) return;
                        for (const auto& gc : gcs) {
                            if (gc.typeParam != paramName) continue;
                            for (const auto& item : gc.items)
                                if (item.kind == "new") { hasNewConstraint = true; return; }
                        }
                    };
                    bool isMethodParam = false;
                    if (currentMethodDecl_ &&
                        !currentMethodDecl_->genericParams.empty()) {
                        for (const auto& gp : currentMethodDecl_->genericParams)
                            if (gp.name == paramName) { isMethodParam = true; break; }
                        if (isMethodParam)
                            checkGPs(currentMethodDecl_->genericParams,
                                     currentMethodDecl_->genericConstraints);
                    }
                    if (!isMethodParam && currentType_ && currentType_->decl)
                        if (auto* td = dynamic_cast<const ast::TypeDecl*>(
                                currentType_->decl))
                            if (!td->genericParams.empty())
                                checkGPs(td->genericParams,
                                         td->genericConstraints);
                    if (isTypeParam && !hasNewConstraint) {
                        ctx_.diagnostics().report(
                            Severity::Error, ne->span.begin,
                            "`new " + std::string(paramName) + "()` requires "
                            "the type parameter `" + std::string(paramName) +
                            "` to be constrained `where " +
                            std::string(paramName) + ": new()` — the body "
                            "type-checks under the weakest assumption its "
                            "constraints establish, and construction is not "
                            "guaranteed without the `new()` constraint (§14.1, "
                            "#992)");
                    }
                }
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
                        colName == "Array"     || colName == "OwningArray"  ||
                        colName == "Stack"     || colName == "OwningStack"  ||
                        colName == "Queue"     || colName == "OwningQueue"  ||
                        colName == "Set"       || colName == "OwningSet"    ||
                        colName == "LinkedList"|| colName == "OwningLinkedList" ||
                        colName == "Dictionary"|| colName == "OwningDictionary" ||
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
                // stdlib §4.4 v0.165 (#1375): the sized-fill constructor
                // `Array<T>(size: int, fill: T)` requires `fill` to be
                // assignable to the element type T (and `size` to be int-like).
                // Codegen fills each slot with `fill` via a raw element-width
                // store, so a type-mismatched fill (`new Array<int>(4, "x")`)
                // would silently write a wrong-width value into every slot —
                // reject it here, matching every other typed constructor arg.
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty() &&
                    rt->astNode->name->parts[0] == "Array" &&
                    ne->args.size() == 2 &&
                    ne->args[0].value && ne->args[1].value &&
                    !rt->genericArgs.empty() && rt->genericArgs[0]) {
                    // size (arg 0): must be int-like.
                    checkAssignable(
                        ctx_.exprType(*ne->args[0].value),
                        ctx_.primitiveTypeRef(PrimitiveType::Int),
                        ne->args[0].value->span.begin,
                        "size argument of the sized `Array<T>` constructor",
                        ne->args[0].value.get());
                    // fill (arg 1): must be assignable to the element type T.
                    // §4.2 typed-context narrowing: a bare in-range int literal
                    // fill bound to a narrower integer element type is admitted at
                    // that type (the doc's canonical read-loop is
                    // `new Array<byte>(64 * 1024, 0)`, §7.1) — retype the literal
                    // before the assignability check, mirroring the record-arg and
                    // local-decl narrowing paths.
                    {
                        TypeRef* fillTy = ctx_.exprType(*ne->args[1].value);
                        TypeRef* elemTy = rt->genericArgs[0];
                        if (fillTy && elemTy &&
                            fillTy->kind == TypeRef::Kind::Primitive &&
                            fillTy->primitive == PrimitiveType::Int &&
                            elemTy->kind == TypeRef::Kind::Primitive) {
                            PrimitiveType tgt = elemTy->primitive;
                            bool narrower =
                                tgt == PrimitiveType::Byte ||
                                tgt == PrimitiveType::SByte ||
                                tgt == PrimitiveType::Short ||
                                tgt == PrimitiveType::UShort ||
                                tgt == PrimitiveType::UInt ||
                                tgt == PrimitiveType::Long ||
                                tgt == PrimitiveType::ULong;
                            if (narrower) {
                                auto cv = foldIntConstExprTC(*ne->args[1].value);
                                auto fits = [](long long v, PrimitiveType pt) {
                                    switch (pt) {
                                        case PrimitiveType::SByte:
                                            return v >= -128 && v <= 127;
                                        case PrimitiveType::Byte:
                                            return v >= 0 && v <= 255;
                                        case PrimitiveType::Short:
                                            return v >= -32768 && v <= 32767;
                                        case PrimitiveType::UShort:
                                            return v >= 0 && v <= 65535;
                                        case PrimitiveType::UInt:
                                            return v >= 0 && v <= 4294967295LL;
                                        case PrimitiveType::ULong:
                                            return v >= 0;
                                        default:
                                            return true;  // Long fits any int
                                    }
                                };
                                if (cv && fits(*cv, tgt))
                                    ctx_.setExprType(ne->args[1].value.get(),
                                                     elemTy);
                            }
                        }
                    }
                    checkAssignable(
                        ctx_.exprType(*ne->args[1].value),
                        rt->genericArgs[0],
                        ne->args[1].value->span.begin,
                        "fill value of the sized `Array<T>` constructor",
                        ne->args[1].value.get());
                }
                // §28.13 / stdlib §2.16 (#603): Shared<T> / Weak<T> require T to
                // be a proper class type.  Runtime-managed (string, function),
                // value (int/record/enum), and interface types have no disposal
                // obligation to share and are rejected at instantiation, mirroring
                // the §14.4 / OwningArray<T: class> constraint.
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
                            } else if (ta->kind == TypeRef::Kind::Named &&
                                       !ta->declaration &&
                                       ta->astNode && ta->astNode->name &&
                                       !ta->astNode->name->parts.empty()) {
                                // §28.13 v0.140 / #689: compiler-known stdlib class types
                                // (Mutex, Atomic<T>, AtomicInteger<T>, Semaphore, Channel<T>,
                                // ChannelSender<T>, and other compiler-known classes like Lock,
                                // OwningArray<T>, etc.) have no user TypeDecl but are classes.
                                // Accept all builtin named types here; the Shared<T> sendability
                                // rule in isSendable (ExprTypecheck_Calls.cpp) further restricts
                                // which of these are sendable across domains.
                                std::string_view taname = ta->astNode->name->parts[0];
                                // The full set of compiler-known class types (not value types,
                                // not records/enums/interfaces): Mutex, Lock, Atomic<T>,
                                // AtomicInteger<T>, Semaphore, Channel<T>, ChannelSender<T>,
                                // Task, OwningTask, OwningArray, OwningStack, OwningQueue,
                                // OwningSet, OwningLinkedList, OwningDictionary, Array, Stack,
                                // Queue, Set, LinkedList, Dictionary, EventSource,
                                // AsyncEventSource, Shared, Weak.
                                // Excluded: string (primitive), Object (primitive), int etc.
                                // For simplicity: if the type arg is a Named builtin (no decl)
                                // and is not a primitive, treat it as a class type.
                                // The "T must be a class" guard's purpose is to exclude
                                // primitives, records, enums, interfaces, and functions —
                                // all of which either have no heap-allocated class disposal
                                // obligation or are represented differently.
                                // Builtin named types without a user decl are all class-like
                                // (heap-allocated, runtime-managed, disposable).
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
                // §6.2 / issue #647: `new Lock()` is a compile error.
                // Lock has NO public constructor — instances are obtained only
                // from Mutex.Acquire() / TryAcquire().  Reject `new Lock(...)` here.
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty() &&
                    rt->astNode->name->parts[0] == "Lock") {
                    ctx_.diagnostics().report(
                        Severity::Error, ne->span.begin,
                        "`Lock` has no public constructor: obtain a Lock via "
                        "`mutex.Acquire()` or `mutex.TryAcquire(timeout)` "
                        "(stdlib \xC2\xA7""6.2, #647)");
                }
                // §6.3 / issue #647: `new SemaphoreToken()` is a compile error.
                // SemaphoreToken has NO public constructor — instances are obtained
                // only from Semaphore.Acquire() / TryAcquire().
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty() &&
                    rt->astNode->name->parts[0] == "SemaphoreToken") {
                    ctx_.diagnostics().report(
                        Severity::Error, ne->span.begin,
                        "`SemaphoreToken` has no public constructor: obtain a "
                        "SemaphoreToken via `semaphore.Acquire()` or "
                        "`semaphore.TryAcquire(timeout)` "
                        "(stdlib \xC2\xA7""6.3, #647)");
                }
                // §6.4 / issue #647: `new Atomic<T>(initial)` — T-restriction check.
                // Atomic<T> requires T to be a cell-sized primitive type per §6.4:
                // {bool, char, byte, sbyte, short, ushort, int, uint, long, ulong,
                //  IntPtr, UIntPtr}.  Any other type argument is a compile error.
                // Verbatim diagnostic from §6.4: "Atomic<T> requires a cell-sized
                // primitive type; T is not eligible. For floating-point atomics,
                // use Atomic<int> with a bit-cast; for record-typed atomics, use
                // a Mutex."
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty() &&
                    rt->astNode->name->parts[0] == "Atomic") {
                    bool tOk = false;
                    if (!rt->genericArgs.empty() && rt->genericArgs[0]) {
                        const TypeRef* ta = rt->genericArgs[0];
                        if (ta->kind == TypeRef::Kind::Primitive) {
                            using P = sema::PrimitiveType;
                            auto p = ta->primitive;
                            tOk = (p == P::Bool || p == P::Char ||
                                   p == P::Byte || p == P::SByte ||
                                   p == P::Short || p == P::UShort ||
                                   p == P::Int || p == P::UInt ||
                                   p == P::Long || p == P::ULong);
                        } else if (ta->kind == TypeRef::Kind::Named &&
                                   !ta->declaration && ta->astNode &&
                                   ta->astNode->name &&
                                   !ta->astNode->name->parts.empty()) {
                            // IntPtr and UIntPtr are registered as nullary builtin
                            // Named types (not PrimitiveType); accept them here.
                            auto& tn = ta->astNode->name->parts[0];
                            tOk = (tn == "IntPtr" || tn == "UIntPtr");
                        }
                    } else {
                        // No type argument provided — sema will report "wrong arity";
                        // avoid a second diagnostic.
                        tOk = true;
                    }
                    if (!tOk) {
                        ctx_.diagnostics().report(
                            Severity::Error, ne->span.begin,
                            "Atomic<T> requires a cell-sized primitive type; T is "
                            "not eligible. For floating-point atomics, use "
                            "Atomic<int> with a bit-cast; for record-typed atomics, "
                            "use a Mutex. (stdlib \xC2\xA7""6.4, #647)");
                    }
                }
                // §6.4 / issue #647: `new AtomicInteger<T>(initial)` — T-restriction.
                // AtomicInteger<T> requires T to be a primitive integer type:
                // {byte, sbyte, short, ushort, int, uint, long, ulong}.
                // bool, char, IntPtr, UIntPtr are NOT eligible (use Atomic<T>
                // and CompareExchange directly per §6.4).
                // Diagnostic NOT verbatim (§6.4 has none for AtomicInteger);
                // wording mirrors §6.4 §AtomicInteger restriction prose.
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty() &&
                    rt->astNode->name->parts[0] == "AtomicInteger") {
                    bool tOk = false;
                    if (!rt->genericArgs.empty() && rt->genericArgs[0]) {
                        const TypeRef* ta = rt->genericArgs[0];
                        if (ta->kind == TypeRef::Kind::Primitive) {
                            using P = sema::PrimitiveType;
                            auto p = ta->primitive;
                            tOk = (p == P::Byte || p == P::SByte ||
                                   p == P::Short || p == P::UShort ||
                                   p == P::Int || p == P::UInt ||
                                   p == P::Long || p == P::ULong);
                        }
                        // bool, char, IntPtr, UIntPtr are NOT eligible.
                    } else {
                        tOk = true;  // arity error reported elsewhere
                    }
                    if (!tOk) {
                        ctx_.diagnostics().report(
                            Severity::Error, ne->span.begin,
                            "AtomicInteger<T> requires a primitive integer type "
                            "(byte, sbyte, short, ushort, int, uint, long, ulong); "
                            "T is not eligible. For bool, char, IntPtr, or UIntPtr "
                            "atomics, use Atomic<T> and CompareExchange directly. "
                            "(stdlib \xC2\xA7""6.4, #647)");
                    }
                }
                // issue #545: `new Buffer<T>(n)` — T-restriction check.
                // Buffer<T> requires T to be a value-type primitive in this revision:
                // {bool, char, byte, sbyte, short, ushort, int, uint, long, ulong,
                //  float, double, decimal}.  Class, record, string, and interface
                // elements re-open the §5.20 borrow-storing / zeroed-slot-disposal
                // questions; they are explicitly out of scope for this slice.
                // Diagnostic: "Buffer<T> requires a value-type element in this
                // revision; class elements await the §5.20 design (#545)."
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty() &&
                    rt->astNode->name->parts[0] == "Buffer") {
                    bool tOk = false;
                    if (!rt->genericArgs.empty() && rt->genericArgs[0]) {
                        const TypeRef* ta = rt->genericArgs[0];
                        // Accept only primitive value types.
                        tOk = (ta->kind == TypeRef::Kind::Primitive);
                        // string is PrimitiveType::String — REJECT (runtime-managed).
                        if (tOk && ta->primitive == sema::PrimitiveType::String)
                            tOk = false;
                    } else {
                        // No type arg — arity error reported elsewhere; suppress
                        // the redundant restriction diagnostic.
                        tOk = true;
                    }
                    if (!tOk) {
                        ctx_.diagnostics().report(
                            Severity::Error, ne->span.begin,
                            "Buffer<T> requires a value-type element in this "
                            "revision; class elements await the \xC2\xA7""5.20 "
                            "design (#545)");
                    }
                }
                // §2.15 / issue #631: `new BinaryDeserializer(arg)` requires
                // `arg` to be a `ReadOnlySpan<byte>` (the result of calling
                // `.AsReadOnlySpan()` on an `OwningBuffer<byte>`). Passing the
                // owned buffer directly (`new BinaryDeserializer(bytes)` where
                // `bytes: OwningBuffer<byte>`) is a type error — the constructor
                // signature is `BinaryDeserializer(input: ReadOnlySpan<byte>)`
                // (stdlib §2.15). Without this check the wrong-arg form compiled
                // silently and segfaulted at runtime.
                if (rt->kind == TypeRef::Kind::Named && !rt->declaration &&
                    rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty() &&
                    rt->astNode->name->parts[0] == "BinaryDeserializer" &&
                    !ne->args.empty() && ne->args[0].value) {
                    const TypeRef* arg0 = ctx_.exprType(*ne->args[0].value);
                    if (arg0 && arg0->kind == TypeRef::Kind::Named &&
                        !arg0->declaration && arg0->astNode &&
                        arg0->astNode->name &&
                        !arg0->astNode->name->parts.empty() &&
                        arg0->astNode->name->parts[0] == "OwningBuffer") {
                        ctx_.diagnostics().report(
                            Severity::Error, ne->args[0].value->span.begin,
                            "`new BinaryDeserializer(...)` expects "
                            "`ReadOnlySpan<byte>` as its first argument "
                            "(stdlib \xC2\xA7""2.15); got `OwningBuffer<byte>` — "
                            "call `.AsReadOnlySpan()` first "
                            "(\xC2\xA7""2.15, #631)");
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
                    } else if (t->kind == ast::TypeDeclKind::Record) {
                        // #1105 (§9.1): validate a POSITIONAL record's primary-
                        // constructor `new` args (count + per-field type). Record
                        // construction previously skipped all arg checking, so
                        // too-few / too-many / wrong-typed args were silently
                        // accepted and miscompiled. Only positional records are
                        // routed here; checkNewExprArgs validates the positional
                        // fields and returns (generic records defer to #428).
                        if (auto* rtd =
                                dynamic_cast<const ast::TypeDecl*>(t->decl))
                            if (rtd->hasPositionalFields)
                                checkNewExprArgs(*ne, t);
                    }
                    // §14.1 / §2.5 (#428 phase B): imported generic RECORD
                    // construction is now supported — codegen performs per-
                    // instantiation specialization in the importing module using
                    // the same local recordStruct path, with the substitution
                    // built from TypeSymbol::genericParams (populated by the
                    // ModuleLoader).  The prior §14 guard that rejected this
                    // case is removed here; the residual gate for imported
                    // generic CLASS is still in checkImportedNewExprArgs.
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
                // §17.11 (#951): `default(T)` is a compile error when T has no
                // well-defined zero value — a non-nullable REFERENCE type
                // (string/class/interface/function), or recursively a record
                // with a non-nullable-ref field or an inline array with
                // non-nullable-ref elements. The null such a default would
                // produce violates the non-null contract (§7.9, the same defect
                // the type system prevents for an uninitialized non-nullable
                // reference field). `default(T?)` (nullable) and value types
                // (value-builtins/enums/records of value fields) stay accepted.
                // (The generic-parameter constrained case is the #864 check
                // below; defaultIllFormed returns false for a bare type param.)
                if (defaultIllFormed(rt)) {
                    std::string detail;
                    if (rt->kind == TypeRef::Kind::Named && rt->declaration &&
                        rt->declaration->kind == ast::TypeDeclKind::Record)
                        detail = "a record any of whose fields is a "
                                 "non-nullable reference type";
                    else if (rt->kind == TypeRef::Kind::InlineArray)
                        detail = "an inline array whose element type is a "
                                 "non-nullable reference type";
                    else
                        detail = "a non-nullable reference type (a class, "
                                 "interface, `string`, or function type)";
                    ctx_.diagnostics().report(
                        Severity::Error, de->span.begin,
                        "`default(T)` is a compile error for " + detail +
                        " — it has no well-defined zero value; write "
                        "`default(T?)` for the nullable counterpart where "
                        "applicable (§17.11)");
                }
                // §17.11 (#864 part 1): `default(T)` under `where T: class` or
                // a type-bound constraint is a compile error — the constraint
                // guarantees T is non-nullable, and `default(T)` would produce
                // null, which does not fit T's contract. `default(T?)` (nullable)
                // stays accepted; only the non-nullable form triggers this error.
                // `where T: value` and unconstrained T are OK (value types have
                // a well-defined zero; unconstrained T may be a value type).
                if (rt->kind == TypeRef::Kind::Named &&
                    rt->declaration == nullptr &&   // generic type parameter
                    !rt->nullable &&                // non-nullable `T`, not `T?`
                    de->type->name && !de->type->name->parts.empty()) {
                    std::string_view paramName = de->type->name->parts.front();
                    // Walk the enclosing method's constraints first, then the
                    // enclosing class's constraints (for class-level params).
                    auto checkConstraintsFor =
                        [&](const std::vector<ast::GenericConstraint>& gcs) {
                        for (const auto& gc : gcs) {
                            if (gc.typeParam != paramName) continue;
                            for (const auto& item : gc.items) {
                                if (item.kind == "class" || item.kind == "type") {
                                    ctx_.diagnostics().report(
                                        Severity::Error, de->span.begin,
                                        "`default(T)` is a compile error when T "
                                        "is constrained non-null (`where " +
                                        std::string(paramName) + ": " +
                                        (item.kind == "class"
                                             ? "class"
                                             : (item.typeArg && item.typeArg->name &&
                                                !item.typeArg->name->parts.empty()
                                                    ? std::string(item.typeArg->name->parts.front())
                                                    : "type")) +
                                        "`) — the null value `default(T)` does not "
                                        "fit T's non-null contract (§17.11); "
                                        "write `default(T?)` to produce null of the "
                                        "nullable counterpart");
                                    return;
                                }
                            }
                        }
                    };
                    // Check method-level constraints first; fall through to
                    // class-level constraints only when the name is not a
                    // method-level generic param (avoids false positives when
                    // the class has a same-named param with looser constraints).
                    bool isMethodParam = false;
                    if (currentMethodDecl_) {
                        for (const auto& gp : currentMethodDecl_->genericParams)
                            if (gp.name == paramName) { isMethodParam = true; break; }
                        if (isMethodParam)
                            checkConstraintsFor(currentMethodDecl_->genericConstraints);
                    }
                    if (!isMethodParam && currentType_ && currentType_->decl)
                        if (auto* td = dynamic_cast<const ast::TypeDecl*>(
                                currentType_->decl))
                            checkConstraintsFor(td->genericConstraints);
                }
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
    // §22.6 / §25 (#1131): collect ALL binary-operator overloads of `be.op`
    // declared on the operand's type. A type may declare several overloads of the
    // same operator symbol (e.g. `operator +(V, int)` and `operator +(V, V)`);
    // returning only the FIRST and validating just that one made every later
    // overload unreachable (the matching overload was wrongly rejected). The
    // caller (`tryOperand`) picks the candidate whose parameters match the
    // operands in order.
    auto candidatesOn = [&](const ast::Expr* operand) -> std::vector<MethodSymbol*> {
        std::vector<MethodSymbol*> out;
        if (!operand) return out;
        TypeSymbol* ts = memberTargetType(operand);
        if (!ts) return out;
        // §25: user-defined `operator =` and `<>` on class types take
        // precedence over built-in reference equality — only skip when
        // no user-defined operator exists (handled by falling through).
        for (Symbol* m : ts->members) {
            auto* ms = dynamic_cast<MethodSymbol*>(m);
            if (ms && ms->isOperator && ms->operatorToken == be.op &&
                ms->params.size() == 2)
                out.push_back(ms);
        }
        return out;
    };
    // §25 (#1096): the operator's operands must match its declared parameters
    // IN ORDER (left → param 0, right → param 1). Bezel does not auto-commute
    // operator overloads — §25 defines no commutation, and silently swapping
    // would be unsound for non-commutative operators (`4 - v` ≠ `v - 4`). The
    // matched type is found on either operand, but the operand at each position
    // must be the type the corresponding parameter declares; otherwise the
    // operator does not apply. (Previously a reversed-operand match, e.g.
    // `4 * v` against `operator *(Vec, int)`, was accepted, and codegen emitted
    // the args in source order → an IR-verification crash.)
    auto operandMatchesParam = [&](const ast::Expr* operand,
                                   Symbol* paramSym) -> bool {
        auto* ps = dynamic_cast<ParameterSymbol*>(paramSym);
        if (!ps) return false;
        TypeRef* pt = ps->declType ? ctx_.resolvedType(*ps->declType)
                                   : ps->resolvedDeclType;
        TypeRef* at = operand ? ctx_.exprType(*operand) : nullptr;
        if (!pt || !at) return false;
        // A named-type (record/class/enum) parameter must receive an operand of
        // that exact declared type — the discriminating position for the
        // reversed-operand (commutation) case.
        // §7.11 (#1132): user-defined operator lifting over nullable operands is
        // DEFERRED — a `Vec?` operand does NOT match a non-nullable `Vec`
        // parameter. Without the nullability gate the operator resolved and
        // codegen passed the `{i8, Vec}` wrapper struct to a function expecting a
        // bare `Vec` (malformed IR). Requiring nullability agreement makes the
        // operator not resolve, so the use is cleanly rejected below.
        // §8.4 (#1248): `=`/`<>` are exempt from the nullability gate — a `T?`
        // operand IS allowed to match a non-nullable `T` parameter for these two
        // operators because codegen wraps the call in the null-safe equality
        // protocol (lowerNullSafeRecordEquality), which unwraps the `{i8, T}`
        // payload to bare `T` before invoking the user operator. All other
        // operators remain gated (§7.11: lifting over `T?` is deferred).
        if (pt->kind == TypeRef::Kind::Named)
            return at->kind == TypeRef::Kind::Named &&
                   pt->declaration == at->declaration &&
                   (pt->nullable || !at->nullable ||
                    be.op == "=" || be.op == "<>");
        // A primitive parameter accepts a primitive operand (numeric widening /
        // coercion is handled downstream); the kinds must still agree.
        if (pt->kind == TypeRef::Kind::Primitive)
            return at->kind == TypeRef::Kind::Primitive;
        return false;
    };
    // #1270 (§25 / §14.1): `operandMatchesParam` matches a Named operand against a
    // Named parameter by DECLARATION only, ignoring generic type args — so a binary
    // operator on a generic type (`operator +(a,b: Box<T>)`) wrongly accepted
    // MISMATCHED instantiations (`Box<int> + Box<bool>`, where the single param `T`
    // cannot unify to both). When the operator's declaring type is generic, unify
    // both parameter types against their operands into ONE binding map: a
    // type-parameter name bound to two conflicting concrete types fails
    // (unifyReturnAgainstTarget checks consistency), so the operator does not apply.
    // Non-generic operators (empty genericParams) and consistent instantiations are
    // unaffected.
    auto genericArgsConsistent = [&](MethodSymbol* m) -> bool {
        if (!m->parent || m->parent->genericParams.empty()) return true;
        std::unordered_set<std::string_view> paramNames;
        for (Symbol* gp : m->parent->genericParams)
            if (gp) paramNames.insert(gp->name);
        auto declTypeOf = [&](Symbol* paramSym) -> TypeRef* {
            auto* ps = dynamic_cast<ParameterSymbol*>(paramSym);
            if (!ps) return nullptr;
            return ps->declType ? ctx_.resolvedType(*ps->declType)
                                : ps->resolvedDeclType;
        };
        std::unordered_map<std::string_view, TypeRef*> solved;
        TypeRef* p0 = declTypeOf(m->params[0]);
        TypeRef* p1 = declTypeOf(m->params[1]);
        TypeRef* a0 = be.left ? ctx_.exprType(*be.left) : nullptr;
        TypeRef* a1 = be.right ? ctx_.exprType(*be.right) : nullptr;
        if (p0 && a0 && !unifyReturnAgainstTarget(p0, a0, paramNames, solved))
            return false;
        if (p1 && a1 && !unifyReturnAgainstTarget(p1, a1, paramNames, solved))
            return false;
        return true;
    };
    auto tryOperand = [&](const ast::Expr* operand) -> MethodSymbol* {
        // #1131: examine EVERY overload of the operator declared on `operand`'s
        // type and return the first whose parameters match the operands in order.
        for (MethodSymbol* m : candidatesOn(operand)) {
            if (operandMatchesParam(be.left.get(), m->params[0]) &&
                operandMatchesParam(be.right.get(), m->params[1]) &&
                genericArgsConsistent(m))
                return m;
        }
        return nullptr;
    };
    MethodSymbol* fromLeft = tryOperand(be.left.get());
    MethodSymbol* fromRight = tryOperand(be.right.get());
    // §25.2 (#1347, v0.160): if distinct applicable overloads are found on
    // BOTH operand types, the expression is a compile error. The compiler
    // declines to guess which is intended (mirroring §22.6's overload-
    // resolution stance). Operands of the same type find the same symbol —
    // `fromLeft == fromRight` — which is unambiguous and not an error.
    if (fromLeft && fromRight && fromLeft != fromRight) {
        // Collect the declaring-type names for the error message.
        std::string leftTypeName, rightTypeName;
        if (fromLeft->parent)  leftTypeName  = std::string(fromLeft->parent->name);
        if (fromRight->parent) rightTypeName = std::string(fromRight->parent->name);
        ctx_.diagnostics().report(
            Severity::Error, be.span.begin,
            "ambiguous operator `" + std::string(be.op) + "`: distinct "
            "applicable overloads found on both operand types "
            "(candidates: `" + leftTypeName + "::operator " +
            std::string(be.op) + "` and `" + rightTypeName +
            "::operator " + std::string(be.op) + "`) — "
            "remove a conflicting import or qualify the call (§25.2)");
        return nullptr;
    }
    return fromLeft ? fromLeft : fromRight;
}

void ExprTC::typeBinary(const ast::BinaryExpr& be) {
    const auto& op = be.op;
    // §25: a user-defined operator overload takes precedence over the
    // built-in result-type rules; the expression is that method's call.
    if (op != "and" && op != "or" && op != "in") {
        if (MethodSymbol* opm = findBinaryOperator(be)) {
            ctx_.setExprSymbol(&be, opm);
            if (auto* od = dynamic_cast<const ast::OperatorDecl*>(opm->decl)) {
                if (od->returnType)
                    if (auto* rt = ctx_.resolvedType(*od->returnType)) {
                        // §25 / §14.1 (#1260): the operator is declared inside a
                        // GENERIC record/class (`operator +(a,b: Box<T>): Box<T>`).
                        // Its return type embeds the declaring type's params (`T`),
                        // which `resolvedType` leaves unsubstituted. Substitute them
                        // from the concrete operand instantiation (`Box<int>`) so
                        // the result is `Box<int>` — otherwise `r := a + b` was typed
                        // `Box<T>` and a downstream `r.value` resolved to the opaque
                        // param `T` (a later `r.value + 1` then wrongly reported
                        // "operator + not defined for `?`"). Reuses the #597 member-
                        // return substitution (substituteMemberType → classSubst).
                        if (opm->parent &&
                            !opm->parent->genericParams.empty()) {
                            auto isParentInst = [&](const ast::Expr* e) -> bool {
                                TypeRef* t = e ? ctx_.exprType(*e) : nullptr;
                                return t && t->kind == TypeRef::Kind::Named &&
                                       t->declaration == opm->parent &&
                                       !t->genericArgs.empty();
                            };
                            const ast::Expr* recv =
                                isParentInst(be.left.get())  ? be.left.get()
                                : isParentInst(be.right.get()) ? be.right.get()
                                                               : nullptr;
                            if (recv)
                                rt = substituteMemberType(rt, recv, opm->parent);
                        }
                        ctx_.setExprType(&be, rt);
                    }
            } else if (ctx_.isImported(opm)) {
                // §25 / §2.5 (#1485): an imported user-defined operator overload
                // carries no ast::OperatorDecl (imported symbols have no AST
                // node — decl == nullptr), so the dynamic_cast above yields null
                // and the BinaryExpr's result type would never be set. Without
                // it, `r := a + b` infers null, and a downstream `r.field`
                // fails member resolution → codegen silently drops the whole
                // expression (fieldAddr() returns null) or reads garbage. The
                // loader stashed the operator's resolved return type keyed on
                // the symbol; use it. Mirrors the imported-regular-method
                // fallback in ExprTypecheck_Calls.cpp.
                if (auto* rt = ctx_.importedReturnType(opm))
                    ctx_.setExprType(&be, rt);
            }
            return;
        }
        // §11.8 (v0.176, #807): constraint-operator dispatch — a binary
        // operator on TYPE-PARAMETER-typed operands resolves through a static
        // operator member of the parameter's `where T: I<T>` constraint when
        // no other resolution applies (findBinaryOperator found nothing above:
        // a type-parameter TypeRef has no declaration to search). Both
        // operands must be the SAME parameter; the interface member's shape is
        // the §2.19 recurrence form `static operator <op>(a: T, b: T): R`.
        // Resolution to the concrete type's operator (or the primitive
        // intrinsic op) happens at instantiation in codegen.
        {
            auto typeParamNameOf = [&](const ast::Expr* e) -> std::string_view {
                if (!e) return {};
                const TypeRef* t = ctx_.exprType(*e);
                if (!t || t->kind != TypeRef::Kind::Named || t->declaration ||
                    t->nullable || !t->astNode || !t->astNode->name ||
                    t->astNode->name->parts.size() != 1)
                    return {};
                std::string_view n = t->astNode->name->parts[0];
                return isInScopeTypeParam(n) ? n : std::string_view{};
            };
            std::string_view lp = typeParamNameOf(be.left.get());
            if (!lp.empty() && lp == typeParamNameOf(be.right.get())) {
                TypeRef* bound = nullptr;
                if (MethodSymbol* iop =
                        findConstraintStaticOperator(lp, op, &bound)) {
                    ctx_.setExprSymbol(&be, iop);
                    // Result type: the operator's declared return with the
                    // interface's recurrence parameter substituted by the
                    // call-site parameter (i.e. the operand's own TypeRef);
                    // a concrete return (bool for `<`) passes through.
                    TypeRef* resTy = nullptr;
                    if (auto* od = dynamic_cast<const ast::OperatorDecl*>(
                            iop->decl)) {
                        if (od->returnType)
                            resTy = ctx_.resolvedType(*od->returnType);
                    }
                    if (resTy && resTy->kind == TypeRef::Kind::Named &&
                        !resTy->declaration) {
                        // Bare recurrence-parameter return → the operand type.
                        resTy = be.left ? ctx_.exprType(*be.left) : nullptr;
                    }
                    if (resTy) ctx_.setExprType(&be, resTy);
                    return;
                }
            }
        }
        // §7.11 (#1132): user-defined operator lifting over nullable operands is
        // DEFERRED. The operator did NOT resolve above (the nullability gate in
        // operandMatchesParam), but it IS declared on the operand's underlying
        // type and an operand is nullable. Emit a precise diagnostic rather than
        // the generic "operator not defined" message below — the operator exists;
        // only its lifting over `T?` is unsupported. (`=`/`<>` are excluded: they
        // are already null-safe per §8.4.)
        if (op != "=" && op != "<>") {
            auto nullableUserOperand = [&](const ast::Expr* operand) -> bool {
                if (!operand) return false;
                const TypeRef* t = ctx_.exprType(*operand);
                if (!t || !t->nullable) return false;
                TypeSymbol* ts = memberTargetType(operand);
                if (!ts || (ts->kind != ast::TypeDeclKind::Record &&
                            ts->kind != ast::TypeDeclKind::Class))
                    return false;
                for (Symbol* m : ts->members) {
                    auto* ms = dynamic_cast<MethodSymbol*>(m);
                    if (ms && ms->isOperator && ms->operatorToken == op &&
                        ms->params.size() == 2)
                        return true;
                }
                return false;
            };
            if (nullableUserOperand(be.left.get()) ||
                nullableUserOperand(be.right.get())) {
                ctx_.diagnostics().report(
                    Severity::Error, be.span.begin,
                    "operator `" + std::string(op) + "` does not lift over a "
                    "nullable operand of a user-defined-operator type — lifting "
                    "of user-defined operators over `T?` is deferred (\xC2\xA7"
                    "7.11); unwrap the operand(s) with `!` or `??` to the "
                    "non-nullable type first");
                // Error recovery: type the result as the left operand's type so
                // downstream uses do not cascade an "untyped expression" error.
                if (TypeRef* lt = be.left ? ctx_.exprType(*be.left) : nullptr)
                    ctx_.setExprType(&be, lt);
                return;
            }
        }
    }
    // §25.2 (#746 R41-C): a relational operator (`<`, `>`, `<=`, `>=`) on a
    // record/class operand requires a matching user-defined overload — none is
    // auto-generated. A partial comparison group (e.g. only `<`/`>` declared,
    // then `<=` used) otherwise fell through to the built-in result-type rule,
    // and codegen emitted a struct-level `icmp` on the aggregate (malformed IR).
    // Reject the undefined use cleanly. (Primitive operands have no
    // memberTargetType and keep the built-in relational.)
    if (op == "<" || op == ">" || op == "<=" || op == ">=") {
        for (const ast::Expr* operand : {be.left.get(), be.right.get()}) {
            TypeSymbol* ts = memberTargetType(operand);
            // Exclude the value-builtins (DateTime/TimeSpan/Guid/…): they carry
            // a kind==Class TypeSymbol but support a built-in relational compare
            // with no user overload (§2.5).
            if (ts && !ts->isValueBuiltin &&
                (ts->kind == ast::TypeDeclKind::Record ||
                 ts->kind == ast::TypeDeclKind::Class)) {
                ctx_.diagnostics().report(
                    Severity::Error, be.span.begin,
                    "operator `" + std::string(op) +
                    "` is not defined for type `" + std::string(ts->name) +
                    "` — comparison operators must be explicitly overloaded "
                    "(§25.2)");
                ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                return;
            }
        }
    }
    // §25.1 (#1096): an arithmetic/bitwise/shift operator on a record/class
    // operand requires a matching user-defined overload whose parameters match
    // the operand ORDER (findBinaryOperator enforces the order; Bezel does not
    // auto-commute). An undeclared operator, or a reversed-operand use like
    // `4 * v` against `operator *(Vec, int)`, otherwise fell through to the
    // built-in numeric rule and codegen emitted a struct-level arithmetic op →
    // an IR-verification crash. Reject cleanly. (Primitive and value-builtin
    // operands — e.g. string `+`, decimal arithmetic, DateTime — keep their
    // built-in operators: they have no record/class memberTargetType here.)
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "mod" ||
        op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
        for (const ast::Expr* operand : {be.left.get(), be.right.get()}) {
            TypeSymbol* ts = memberTargetType(operand);
            if (ts && !ts->isValueBuiltin &&
                (ts->kind == ast::TypeDeclKind::Record ||
                 ts->kind == ast::TypeDeclKind::Class)) {
                ctx_.diagnostics().report(
                    Severity::Error, be.span.begin,
                    "operator `" + std::string(op) +
                    "` is not defined for type `" + std::string(ts->name) +
                    "` with these operands — declare a matching `operator " +
                    std::string(op) +
                    "` whose parameters match the operand order (Bezel does not "
                    "auto-commute operator overloads) (§25)");
                if (auto* opTy = ctx_.exprType(*operand))
                    ctx_.setExprType(&be, opTy);
                return;
            }
        }
    }
    // §8.4 / §10 (#1137): enum comparison (`=`/`<>`/`<`/`<=`/`>`/`>=`) is an
    // underlying-integer comparison between operands of the SAME enum type (spec
    // §8.4 table: "Enum | Underlying integer comparison"). The operands are
    // reduced to their underlying integers before any type check, so an
    // `enum = int` or `enumA = enumB` (different enums) silently compiled as a
    // raw int comparison. Require both operands to be the same enum: if either
    // operand is an enum, the other must be the same enum (a non-enum or a
    // different enum is a compile error). A `null` literal operand is exempt —
    // null comparison of a nullable enum (`c = null`) is governed by §8.4's
    // null-safe equality, not this rule.
    if (op == "=" || op == "<>" || op == "<" || op == ">" ||
        op == "<=" || op == ">=") {
        const TypeRef* lt = be.left ? ctx_.exprType(*be.left) : nullptr;
        const TypeRef* rt = be.right ? ctx_.exprType(*be.right) : nullptr;
        auto enumOf = [&](const TypeRef* t) -> const TypeSymbol* {
            if (t && t->kind == TypeRef::Kind::Named && t->declaration &&
                t->declaration->kind == ast::TypeDeclKind::Enum)
                return t->declaration;
            return nullptr;
        };
        bool leftNull = be.left &&
            dynamic_cast<const ast::NullLiteralExpr*>(be.left.get());
        bool rightNull = be.right &&
            dynamic_cast<const ast::NullLiteralExpr*>(be.right.get());
        const TypeSymbol* le = enumOf(lt);
        const TypeSymbol* re = enumOf(rt);
        // Only enforce when BOTH operands have a RESOLVED type — an unresolved
        // operand (e.g. `this` inside an enum method, or an error expression) is
        // left to other diagnostics rather than risk a false positive.
        if (lt && rt && !leftNull && !rightNull && (le || re) && le != re) {
            std::string other = le ? (re ? std::string(re->name) : "a non-enum")
                                   : "a non-enum";
            ctx_.diagnostics().report(
                Severity::Error, be.span.begin,
                "operator `" + std::string(op) + "` compares enum `" +
                std::string(le ? le->name : re->name) + "` with " +
                (le && re ? ("a different enum `" + other + "`")
                          : std::string("a non-enum operand")) +
                " — an enum may be compared only with a value of the same enum "
                "type (\xC2\xA7""8.4 / \xC2\xA7""10).");
            ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
            return;
        }
    }
    // §9.2 (v0.156)/§8.3: `=` / `<>` on a body record that is not auto-equality-
    // bearing and that declares no explicit operator is a compile error at the
    // use site.  Disqualifiers include an explicit field, a side-effecting
    // computed property, a non-trivial constructor body, and — per §9.2 v0.156
    // — an `override` of `Equals`, `GetHashCode`, or `ToString`.  A user
    // `=`/`<>` overload would already have been selected by findBinaryOperator
    // above (so we never reach here for those); recordHasUserDeclaredEquality
    // only returns true for operator overloads now (not bare Equals/GetHashCode
    // — see #1249).  Positional records, enums, and eligible body records are
    // unaffected.
    if (op == "=" || op == "<>") {
        for (const ast::Expr* operand : {be.left.get(), be.right.get()}) {
            TypeSymbol* ts = memberTargetType(operand);
            if (ts && ts->kind == ast::TypeDeclKind::Record &&
                !isAutoEqualityBearingBodyRecord(ts, ctx_) &&
                !recordHasUserDeclaredEquality(ts)) {
                // §9.3 (v0.158, #1383): positional/combined-form records are no
                // longer unconditionally skipped. A positional record whose
                // fields fall outside the admissible-component rule gets the same
                // use-site error as a non-auto-equality-bearing body record (§9.3
                // "use-site-error discipline"). For LOCAL records the flag lives
                // on the ast::TypeDecl; for IMPORTED records (no ast node) it
                // lives on TypeSymbol itself (issue #501 fix).
                bool positional = ts->hasPositionalFields;
                if (!positional) {
                    if (auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl))
                        positional = td->hasPositionalFields;
                }
                bool isEq = positional
                                ? isPositionalRecordEqualityBearing(ts, ctx_)
                                : false;
                if (!isEq) {
                    ctx_.diagnostics().report(
                        Severity::Error, be.span.begin,
                        "`" + std::string(op) + "` is not available on record `" +
                        std::string(ts->name) +
                        "`: it is not auto-equality-bearing (§9.2/§9.3). Declare"
                        " `=` and `<>` overloads (with `Equals`/`GetHashCode`),"
                        " or simplify the record to an auto-equality-bearing form.");
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
        // §6 (#1040): `and`/`or`/`not` have boolean-only semantics, so a
        // NON-bool operand (int/uint/char/string/class/...) is a compile error.
        // Without this it slipped through here — the result was typed `bool`
        // below — and crashed codegen, which builds the short-circuit as a PHI
        // and hit an LLVM PHINode "operands must be the same type" assertion on
        // the non-bool operand values (`int(u and 255)` / `int(5 and 3)`).
        // (The nullable-`bool` case is handled above; here a KNOWN type that is
        // not a `bool` primitive is rejected. Untyped/error operands are left to
        // their own diagnostics to avoid cascading.) Bitwise integer ops use
        // `&`/`|`/`^` (§6.5), which are distinct operators.
        auto isNonBool = [&](const ast::Expr* e) -> bool {
            if (!e) return false;
            const TypeRef* t = ctx_.exprType(*e);
            return t && !(t->kind == TypeRef::Kind::Primitive &&
                          t->primitive == PrimitiveType::Bool);
        };
        if (isNonBool(be.left.get()) || isNonBool(be.right.get())) {
            ctx_.diagnostics().report(
                Severity::Error, be.span.begin,
                "operator `" + std::string(op) +
                "` requires `bool` operands (§6: `and`/`or`/`not` have"
                " boolean-only semantics). Use the bitwise operators"
                " `&`/`|`/`^` for integer bit operations.");
            ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
            return;
        }
    }
    if (op == "=" || op == "<>" || op == "<" || op == ">" ||
        op == "<=" || op == ">=" || op == "and" || op == "or") {
        // per §6.2/§5.1 (#1204): relational/equality operators require that
        // the two operand types be compatible (one must implicitly widen to
        // the other, §5.1).  When both operands are non-nullable, non-literal
        // typed primitives with no widening relation (e.g. `int` vs `uint`,
        // `double` vs `decimal`), reject with the same §6.2 diagnostic the
        // arithmetic path uses.  An operand that is a literal-derived
        // expression (a bare integer or float literal, or a unary/binary
        // expression built entirely from such literals) is treated as
        // "adoptable" and does NOT trigger the rejection — this preserves
        // patterns like `ulong > 1000` or `ulong > 0 - 5` which work without
        // an explicit cast because the constant has no declared signedness.
        if (op != "and" && op != "or") {
            TypeRef* lt = be.left  ? ctx_.exprType(*be.left)  : nullptr;
            TypeRef* rt = be.right ? ctx_.exprType(*be.right) : nullptr;
            if (lt && rt &&
                lt->kind == TypeRef::Kind::Primitive &&
                rt->kind == TypeRef::Kind::Primitive &&
                !lt->nullable && !rt->nullable &&
                lt->primitive != rt->primitive &&
                !primitiveWidens(lt->primitive, rt->primitive) &&
                !primitiveWidens(rt->primitive, lt->primitive) &&
                isPromotablePrimitive(lt->primitive) &&
                isPromotablePrimitive(rt->primitive) &&
                !isLiteralDerivedExpr(be.left.get()) &&
                !isLiteralDerivedExpr(be.right.get())) {
                ctx_.diagnostics().report(
                    Severity::Error, be.span.begin,
                    std::string("operator `") + std::string(op) +
                    "` has no implicit conversion between operand types `" +
                    primitiveSpelling(lt->primitive) + "` and `" +
                    primitiveSpelling(rt->primitive) +
                    "` (§6.2/§5.1); add an explicit cast (§5.2) to convert"
                    " one operand to the other's type.");
                ctx_.setExprType(&be, ctx_.primitiveTypeRef(PrimitiveType::Bool));
                return;
            }
        }
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
        // §4.6 (spec ruling): `s + t` concatenates two STRINGS. The compiler does
        // not implicitly convert other types to `string` — `"x=" + 7` was
        // silently converted, `"v=" + 1.5` IR-failed (`sitofp ptr`), and
        // `42 + " h"` returned garbage. Require both operands to be string; reject
        // a mixed `string + <non-string>` (or reverse) and point at the remedies.
        if (op == "+") {
            TypeRef* rt0 = be.right ? ctx_.exprType(*be.right) : nullptr;
            auto isStr = [](const TypeRef* t) {
                return t && t->kind == TypeRef::Kind::Primitive &&
                       t->primitive == PrimitiveType::String;
            };
            if (isStr(lt) != isStr(rt0)) {  // exactly one operand is string
                ctx_.diagnostics().report(
                    Severity::Error, be.span.begin,
                    "`+` requires both operands to be `string` — the compiler "
                    "does not implicitly convert other types to `string`; use "
                    "interpolation (`$\"...{x}...\"`) or `.ToString()` (§4.6)");
                auto* sTy = ctx_.makeTypeRef();
                sTy->kind = TypeRef::Kind::Primitive;
                sTy->primitive = PrimitiveType::String;
                ctx_.setExprType(&be, sTy);
                return;
            }
        }
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
        // §6.5 / §6.1 / §17.1: integer-operand operator type checks.
        //   • Bitwise/shift (`& | ^ << >>`) are "not valid on `bool`, `char`, or
        //     floating-point types" (§6.5) — they require integer operands, and
        //     boolean operations use `and`/`or`/`not`. Reject all three operand
        //     kinds (§7.11's lift list applies only where the base operator is
        //     valid for `T`, so it does not extend bitwise to `bool?`/`char?`).
        //   • `mod` is defined on integer types and decimal, but NOT on float or
        //     double (§6.1).
        // (String/reference bitwise keeps its existing concatenation hint.)
        {
            const bool isBitwise = (op == "&" || op == "|" || op == "^" ||
                                    op == "<<" || op == ">>");
            TypeRef* rt0 = be.right ? ctx_.exprType(*be.right) : nullptr;
            auto primOf = [](TypeRef* t) -> PrimitiveType {
                return (t && t->kind == TypeRef::Kind::Primitive)
                           ? t->primitive : PrimitiveType::None;
            };
            auto isFloatPrim = [](PrimitiveType p) {
                return p == PrimitiveType::Float || p == PrimitiveType::Double;
            };
            // String/reference left operand keeps the concatenation hint.
            if (isBitwise && lt &&
                ((lt->kind == TypeRef::Kind::Primitive &&
                  lt->primitive == PrimitiveType::String) ||
                 lt->kind == TypeRef::Kind::Named)) {
                ctx_.diagnostics().report(Severity::Error, be.span.begin,
                    std::string("operator `") + std::string(op) +
                    "` requires integer operands; use `+` for string concatenation");
                return;
            }
            if (isBitwise) {
                for (TypeRef* o : {lt, rt0}) {
                    PrimitiveType p = primOf(o);
                    // §6.5: not valid on bool, char, or floating-point.
                    const char* badKind =
                        isFloatPrim(p)               ? "floating-point types"
                        : (p == PrimitiveType::Bool) ? "`bool` (boolean operations "
                                                       "use `and`/`or`/`not`)"
                        : (p == PrimitiveType::Char)  ? "`char`"
                                                      : nullptr;
                    if (badKind) {
                        ctx_.diagnostics().report(Severity::Error, be.span.begin,
                            std::string("operator `") + std::string(op) +
                            "` is not valid on " + badKind + " — bitwise and "
                            "shift operators require integer operands (§6.5)");
                        return;
                    }
                }
            }
            if (op == "mod") {
                for (TypeRef* o : {lt, rt0}) {
                    if (isFloatPrim(primOf(o))) {
                        ctx_.diagnostics().report(Severity::Error, be.span.begin,
                            "`mod` is not defined on `float` or `double` — use "
                            "`Math.Fmod` or `Math.IEEERemainder` (§6.1)");
                        return;
                    }
                }
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
                // §7.11 / §7.4 / §12.7 (#914): the lifted result is `T?` UNLESS
                // every nullable operand is a simple variable proved non-null by
                // null-flow narrowing (`nullNarrowed_`) — then both payloads are
                // definitely present, so the result is the non-nullable `T`. (A
                // non-nullable operand is trivially present.) Codegen still emits
                // the lifted {flag,value} value; the non-nullable-target coercion
                // sites unwrap it (#914 unwrapNarrowedNullable), and its flag is
                // 1 for a narrowed operand, so the extracted value is correct.
                auto narrowedPresent = [&](const ast::Expr* e,
                                           const TypeRef* t) -> bool {
                    if (!t || t->kind != TypeRef::Kind::Primitive ||
                        !t->nullable)
                        return true;   // non-nullable operand: present
                    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e))
                        if (Symbol* s = ctx_.exprSymbol(*id))
                            return nullNarrowed_.count(s) > 0;
                    return false;
                };
                bool bothPresent = narrowedPresent(be.left.get(), lt) &&
                                   narrowedPresent(be.right.get(), rt);
                res->nullable = !bothPresent;   // §7.11: lifted result is T?
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
        // per §5.1 / §6.2: when both operands are primitive numerics of
        // different types, the result is the wider operand. Shift ops `<<`/`>>`
        // keep the left operand's type (right is the shift count, no promotion).
        if (op != "<<" && op != ">>") {
            TypeRef* rt = be.right ? ctx_.exprType(*be.right) : nullptr;
            if (lt && rt &&
                lt->kind == TypeRef::Kind::Primitive &&
                rt->kind == TypeRef::Kind::Primitive &&
                lt->primitive != rt->primitive &&
                !lt->nullable && !rt->nullable) {
                if (primitiveWidens(lt->primitive, rt->primitive)) {
                    // left widens to right → result is right (the wider).
                    ctx_.setExprType(&be, rt);
                    return;
                }
                if (primitiveWidens(rt->primitive, lt->primitive)) {
                    // right widens to left → result is left (the wider).
                    ctx_.setExprType(&be, lt);
                    return;
                }
                // §6.2: no implicit conversion between the operand types
                // (e.g. `double` and `decimal`, `float` and `decimal`,
                // signed/unsigned same-or-narrower-width pairs).
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
                    return;
                }
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
        // #1104: the result is nullable iff the RHS is nullable — `a ?? b` is
        // `a`'s value when non-null, else `b`. When `b` is itself nullable
        // (`int? ?? int?`) the result CAN be null (both null), so it stays `T?`;
        // only when the RHS is non-nullable (`int? ?? 5`) does the result narrow
        // to the non-nullable `T`. Typing both-nullable as non-nullable made the
        // codegen value (`{i8,T}`) disagree with the slot type (bare `T`) on a
        // chained `(a ?? b) ?? c` → wrong value / PHI-mismatch crash.
        bool rhsNullable = false;
        if (be.right)
            if (TypeRef* rt = ctx_.exprType(*be.right))
                rhsNullable = rt->nullable;
        if (lt && !lhsIsNullLiteral) {
            if (lt->nullable && !rhsNullable) {
                TypeRef* nn = ctx_.makeTypeRef(*lt);
                nn->nullable = false;
                ctx_.setExprType(&be, nn);
            } else {
                // RHS nullable → keep LHS's nullability; or LHS already
                // non-nullable.
                ctx_.setExprType(&be, lt);
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
// the operand's type. Overloadable prefix unary operators are `+`/`-`/`~`.
MethodSymbol* ExprTC::findUnaryOperator(const ast::UnaryExpr& ue) {
    // §25.1: the overloadable unary operators are `+`, `-`, `~` (and `++`/`--`,
    // which are handled at their statement/expression sites, not here). Unary
    // `+` was previously omitted, so a record's `operator +(c)` was never
    // resolved and `+c` silently lowered to the primitive identity — the user
    // overload was ignored. The `params.size() == 1` filter below distinguishes
    // the unary overload from the binary `operator +(a, b)`.
    if (ue.op != "+" && ue.op != "-" && ue.op != "~") return nullptr;
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
// (§28.11: `OwningArray<T>`, `OwningDictionary<K, V>`, class-typed `OwningBuffer<T>`,
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
    // `IReadOnlyList<T>`, `IList<T>`, and `IOwningList<T>` (stdlib §4.1–§4.2);
    // every other built-in interface has no indexer, so `coll[i]` would not
    // typecheck on it. (`Array`/`OwningArray`/`OwningDictionary`/… are concrete
    // CLASSES, not interfaces, and are correctly NOT matched here.)
    if (tt->astNode && tt->astNode->name && !tt->astNode->name->parts.empty()) {
        std::string_view cn = tt->astNode->name->parts[0];
        if (cn == "IReadOnlyList" || cn == "IList" || cn == "IOwningList")
            return std::string(cn);
    }
    return {};
}

// §28.11 / §4.15 (#871): a borrowing-span indexer slot owns no storage, so
// `move span[i]` cannot transfer ownership. The borrowing spans view a
// non-owning source — `ArraySpan<T>` / `ReadOnlyArraySpan<T>` view a borrowing
// `Array<T>`, and the read-only spans (`ReadOnlySpan<T>`, `IReadOnlySpan<T>`,
// `ReadOnlyStringSpan`, `ReadOnlyAsciiByteSpan`) view a borrowed string / buffer
// — so their slots hold borrows with nothing to move out. (The OWNING `Span<T>`
// over an `OwningBuffer` is intentionally NOT matched.) Returns the span type
// name when `operand` is `span[i]` over such a span, else the empty string.
std::string
ExprTC::moveThroughBorrowingSpanSlotName(const ast::Expr& operand) const {
    const auto* ix = dynamic_cast<const ast::IndexExpr*>(&operand);
    if (!ix || !ix->target) return {};
    const TypeRef* tt = ctx_.exprType(*ix->target);
    if (!tt || tt->kind != TypeRef::Kind::Named) return {};
    // The span types may resolve either as a built-in (null declaration, name on
    // the AST node) or via a record declaration; read the name from whichever is
    // present so both representations are covered.
    std::string_view name;
    if (tt->declaration)
        name = tt->declaration->name;
    else if (tt->astNode && tt->astNode->name &&
             !tt->astNode->name->parts.empty())
        name = tt->astNode->name->parts[0];
    if (name == "ArraySpan" || name == "ReadOnlyArraySpan" ||
        name == "ReadOnlySpan" || name == "IReadOnlySpan" ||
        name == "ReadOnlyStringSpan" || name == "ReadOnlyAsciiByteSpan")
        return std::string(name);
    return {};
}

void ExprTC::typeUnary(const ast::UnaryExpr& ue) {
    const auto& op = ue.op;
    // §25: a user-defined unary operator overload.
    if (MethodSymbol* opm = findUnaryOperator(ue)) {
        ctx_.setExprSymbol(&ue, opm);
        if (auto* od = dynamic_cast<const ast::OperatorDecl*>(opm->decl)) {
            if (od->returnType)
                if (auto* rt = ctx_.resolvedType(*od->returnType))
                    ctx_.setExprType(&ue, rt);
        } else if (ctx_.isImported(opm)) {
            // §25 / §2.5 (#1485): an imported unary operator overload has no
            // ast::OperatorDecl (decl == nullptr for imported symbols), so the
            // result type must come from the loader's stashed return type —
            // otherwise the UnaryExpr is left untyped and a downstream member
            // access on its value-typed result is silently dropped. Mirrors the
            // binary-operator fallback above and the imported-method fallback in
            // ExprTypecheck_Calls.cpp.
            if (auto* rt = ctx_.importedReturnType(opm))
                ctx_.setExprType(&ue, rt);
        }
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
            // §6 (#1040): `not` is boolean-only, so a NON-bool operand
            // (int/uint/char/...) is a compile error. Without this it slipped
            // through (result typed `bool` below) and codegen emitted malformed
            // IR (`not 5` / `not u`). The nullable-`bool` case is handled above;
            // here a KNOWN non-`bool` primitive/type is rejected. Use the
            // bitwise complement `~` (§6.5) for an integer bit-flip.
            else if (ot && !(ot->kind == TypeRef::Kind::Primitive &&
                             ot->primitive == PrimitiveType::Bool)) {
                ctx_.diagnostics().report(
                    Severity::Error, ue.span.begin,
                    "`not` requires a `bool` operand (§6: `and`/`or`/`not` have"
                    " boolean-only semantics). Use the bitwise complement `~`"
                    " for an integer bit-flip.");
            }
        }
        ctx_.setExprType(&ue, ctx_.primitiveTypeRef(PrimitiveType::Bool));
        return;
    }
    // §28.8.1 / §28.11 (issue #423): `move coll[i]` is admitted only on the
    // enumerated CONCRETE owning receivers (§28.11). A `move` through an
    // INTERFACE-typed indexer slot (`IOwningList<T>`, `IList<T>`,
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
                "admitted only on concrete owning collections: `OwningArray<T>`, "
                "`OwningDictionary<K, V>`, class-typed `OwningBuffer<T>` / "
                "`Span<T>`, and owning inline arrays)");
        }
        // §28.11 / §4.15 (#871): `move arrSpan[i]` is rejected for a borrowing
        // span (ArraySpan/ReadOnlyArraySpan view a borrowing Array<T>; the
        // read-only spans view a borrowed string/buffer). Their slots hold
        // borrows with no ownership to transfer; accepting the move corrupts
        // memory at runtime (a SIGSEGV / double-free of a borrow-backed slot).
        std::string span = moveThroughBorrowingSpanSlotName(*ue.operand);
        if (!span.empty()) {
            ctx_.diagnostics().report(
                Severity::Error, ue.span.begin,
                std::string("`move` is not admitted through a borrowing span "
                            "slot: `") + span +
                "` views a borrowing source (a non-owning `Array<T>`, `string`, "
                "or read-only buffer), so its slots hold borrows with no "
                "ownership to transfer (§28.11 / §4.15 — `move arrSpan[i]` is "
                "rejected; use the plain indexer-get to read the borrow)");
        }
        // §4.14 (#779): `move sp[i]` on a VALUE-typed owning `Span<T>` is not
        // admitted — value elements are read with the plain indexer and copied
        // at the use site (spec §4.14: "For value T: move span[i] is not
        // admitted"). The class-typed owning `Span<T>` DOES admit `move` (it
        // transfers the owned slot — handled by lowerMoveFromCollectionSlot).
        if (const auto* mix =
                dynamic_cast<const ast::IndexExpr*>(ue.operand.get())) {
            if (mix->target) {
                const TypeRef* tt = ctx_.exprType(*mix->target);
                if (tt && tt->kind == TypeRef::Kind::Named && !tt->declaration &&
                    tt->astNode && tt->astNode->name &&
                    !tt->astNode->name->parts.empty() &&
                    tt->astNode->name->parts[0] == "Span") {
                    const TypeRef* el =
                        (!tt->genericArgs.empty()) ? tt->genericArgs[0] : nullptr;
                    bool elemIsClass = false;
                    if (el && el->kind == TypeRef::Kind::Named) {
                        if (el->declaration &&
                            (el->declaration->kind == ast::TypeDeclKind::Class ||
                             el->declaration->kind ==
                                 ast::TypeDeclKind::Interface))
                            elemIsClass = true;
                        else if (!el->declaration && el->astNode &&
                                 el->astNode->name &&
                                 !el->astNode->name->parts.empty()) {
                            auto en = el->astNode->name->parts[0];
                            if (en == "Shared" || en == "Weak")
                                elemIsClass = true;
                        }
                    }
                    if (!elemIsClass)
                        ctx_.diagnostics().report(
                            Severity::Error, ue.span.begin,
                            "`move` is not admitted through a value-typed "
                            "`Span<T>` slot — value elements are read with the "
                            "plain indexer and copied at the use site (§4.14)");
                }
            }
        }
    }
    // `move x` (§28.3) keeps the operand's type — the result is the
    // same value with ownership transferred. Without the type, member
    // access on a moved value (`var s := move r; s.M`) fails to resolve.
    if (op == "+" || op == "-" || op == "~" || op == "move") {
        TypeRef* ot = ue.operand ? ctx_.exprType(*ue.operand) : nullptr;
        // §6.5: unary `~` (bitwise complement) is "not valid on bool, char, or
        // floating-point types" — it requires an integer operand. (`not` is the
        // boolean complement.) Primitive-only; a user type overloading `~` is
        // resolved earlier via findUnaryOperator and never reaches here.
        if (op == "~" && ot && ot->kind == TypeRef::Kind::Primitive) {
            PrimitiveType p = ot->primitive;
            const char* badKind =
                (p == PrimitiveType::Float || p == PrimitiveType::Double)
                    ? "floating-point types"
                : (p == PrimitiveType::Bool)
                    ? "`bool` (use `not` for boolean complement)"
                : (p == PrimitiveType::Char) ? "`char`" : nullptr;
            if (badKind) {
                ctx_.diagnostics().report(Severity::Error, ue.span.begin,
                    std::string("operator `~` is not valid on ") + badKind +
                    " — the bitwise complement requires an integer operand "
                    "(§6.5)");
                return;
            }
        }
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
        //   await OwningTask<T>  -> T     (Pattern B: ownership transfers; the
        //                                  result type is the element type T;
        //                                  `owned`-ness is tracked by the
        //                                  ownership pass, not by TypeRef)
        // Awaiting any other task shape (async-iterator MoveNextAsync, etc.)
        // is typed elsewhere — see the deferral note below.
        TypeRef* ot = ue.operand ? ctx_.exprType(*ue.operand) : nullptr;
        // §30.2 / Appendix C.7 (#444 / #509): `await` requires a Task /
        // Task<T> / OwningTask<T> operand. When the operand's type is known and
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
                isTaskType = (head == "Task" || head == "OwningTask");
            }
            if (!isTaskType) {
                ctx_.diagnostics().report(
                    Severity::Error, ue.span.begin,
                    "`await` requires a `Task`, `Task<T>`, or `OwningTask<T>` "
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
            } else if (head == "OwningTask" && !ot->genericArgs.empty() &&
                       ot->genericArgs.front()) {
                // `await OwningTask<T>` → T (Pattern B; element type).
                ctx_.setExprType(&ue, ot->genericArgs.front());
            }
            // NOTE (deferred, §30.7): the result typing specific to async
            // iteration — `await for` desugaring's `MoveNextAsync()` / the
            // owned-yield `GetOwningAsyncEnumerator` path — is deeper than this
            // increment (it depends on the for-loop iteration-variable typing
            // and the IAsyncEnumerator<T> interface members) and is left to
            // the async-iterator typing milestone. 3b scopes the await result
            // rule to the Task / Task<T> / OwningTask<T> forms above.
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
        // §11.8 (v0.176, #807): mark the target node while visiting it so the
        // IdentifierExpr handler can recognize a type-parameter receiver
        // (`T.Member(...)`) without resolving bare `T` anywhere else.
        const ast::Expr* savedHint = maTargetHint_;
        maTargetHint_ = ma->target.get();
        if (ma->target) visitExpr(*ma->target);
        maTargetHint_ = savedHint;
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
        sema::TypeSymbol* calleeParent2 = nullptr;
        if (auto* ms2 = dynamic_cast<sema::MethodSymbol*>(calleeSym2)) {
            calleeMd2 = dynamic_cast<const ast::MethodDecl*>(ms2->decl);
            calleeParent2 = ms2->parent;
        }
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
                        // §11.8 (v0.176, #807): a STATIC interface member
                        // called through a constrained type parameter
                        // (`T.TryParse(s, out var v)`) declares its out param
                        // in terms of the INTERFACE's recurrence parameter
                        // (`value: T?` on IParsable<T>). Substitute it against
                        // the receiver (whose exprType resolveMemberAccess set
                        // to the constraint instantiation `I<T>`) so `v` is
                        // typed by the CALL SITE's type parameter, not the
                        // interface's unrelated `T` spelling.
                        if (lsym->declType && calleeParent2 &&
                            calleeParent2->kind ==
                                ast::TypeDeclKind::Interface) {
                            if (auto* maCal =
                                    dynamic_cast<const ast::MemberAccessExpr*>(
                                        ce->callee.get())) {
                                if (maCal->target)
                                    lsym->declType = substituteMemberType(
                                        lsym->declType, maCal->target.get(),
                                        calleeParent2);
                            }
                        }
                        // out params are nullable refs — strip nullable
                        // (the variable holds the non-nullable value).
                        if (lsym->declType && lsym->declType->nullable) {
                            TypeRef* nn = ctx_.makeTypeRef(*lsym->declType);
                            nn->nullable = false;
                            lsym->declType = nn;
                        }
                    }
                    // v0.171 (#1336): BUILTIN borrowing extraction Trys have no
                    // MethodDecl (calleeMd2 == null), so the block above leaves
                    // declType null. Their out param is `T?`/`V?` — infer the
                    // nullable element/value type from the receiver so the inline
                    // `out var x` recipient is nullable (read with `!` / a null
                    // check on the success path).
                    //   Stack/Queue/OwningStack/OwningQueue
                    //     TryPop/TryDequeue/TryPeek    → T?  (genericArgs[0])
                    //   Dictionary/SortedDictionary/OwningDictionary
                    //     TryGetValue                  → V?  (genericArgs[1])
                    if (!lsym->declType) {
                        if (auto* maCallee =
                                dynamic_cast<const ast::MemberAccessExpr*>(
                                    ce->callee.get())) {
                            if (maCallee->target) {
                                if (const TypeRef* rt =
                                        ctx_.exprType(*maCallee->target)) {
                                    if (rt->kind == TypeRef::Kind::Named &&
                                        !rt->declaration && rt->astNode &&
                                        rt->astNode->name &&
                                        !rt->astNode->name->parts.empty()) {
                                        const std::string_view cn =
                                            rt->astNode->name->parts[0];
                                        const std::string_view mn =
                                            maCallee->member;
                                        const TypeRef* elemT = nullptr;
                                        bool posElem =
                                            (cn == "Stack" || cn == "Queue" ||
                                             cn == "OwningStack" ||
                                             cn == "OwningQueue") &&
                                            (mn == "TryPop" ||
                                             mn == "TryDequeue" ||
                                             mn == "TryPeek");
                                        bool valElem =
                                            (cn == "Dictionary" ||
                                             cn == "SortedDictionary" ||
                                             cn == "OwningDictionary") &&
                                            mn == "TryGetValue";
                                        if (posElem && !rt->genericArgs.empty())
                                            elemT = rt->genericArgs[0];
                                        else if (valElem &&
                                                 rt->genericArgs.size() >= 2)
                                            elemT = rt->genericArgs[1];
                                        if (elemT) {
                                            TypeRef* nn =
                                                ctx_.makeTypeRef(*elemT);
                                            nn->nullable = true;
                                            lsym->declType = nn;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    currentScope_->names[id->name] = lsym;
                }
            }
            // §22.4 / v0.160 (#1305/#1344): `out owned var x` is the inline
            // declaration form for an `out owned T` parameter.  Register the
            // symbol exactly as for `out var`, so that subsequent uses of `x`
            // in the same scope resolve correctly.  Ownership tracking (marking
            // the local Owned after the call) is handled in
            // markOutOwnedArgs (Ownership_Checks.cpp). per §22.4
            if (a.modifier == "out owned var" && a.value && currentScope_) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                                   a.value.get())) {
                    auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                    lsym->name = id->name;
                    lsym->decl = id;
                    // Infer type from the callee's `out owned T` parameter.
                    if (calleeMd2 && pos2 < calleeMd2->params.size() &&
                        calleeMd2->params[pos2].type) {
                        lsym->declType = ctx_.resolvedType(
                            *calleeMd2->params[pos2].type);
                        // `out owned T?` parameter: strip nullable so the
                        // local holds the non-nullable owned value.
                        if (lsym->declType && lsym->declType->nullable) {
                            TypeRef* nn = ctx_.makeTypeRef(*lsym->declType);
                            nn->nullable = false;
                            lsym->declType = nn;
                        }
                    }
                    // §28.11 / v0.160 (#1305): for BUILTIN owning-collection Try*
                    // methods (TryPop/TryDequeue/TryPopFirst/TryPopLast/TryTake/
                    // TryPopValue/TryReceive) there is no MethodDecl (calleeMd2 is
                    // null), so the callee-param path above leaves declType null.
                    // This means ctx_.exprType on the local returns null later,
                    // and `item?.Dispose()` / `item.Dispose()` in
                    // lowerNullCondCall fail the recvIsClassOrIface check and emit
                    // nothing (the element leaks). Mirror the `out using var`
                    // inference: derive the element type from the receiver's
                    // generic args. per §28.11, §22.4
                    if (!lsym->declType) {
                        if (auto* maCallee =
                                dynamic_cast<const ast::MemberAccessExpr*>(
                                    ce->callee.get())) {
                            if (maCallee->target) {
                                if (const TypeRef* rt =
                                        ctx_.exprType(*maCallee->target)) {
                                    if (rt->kind == TypeRef::Kind::Named &&
                                        !rt->declaration && rt->astNode &&
                                        rt->astNode->name &&
                                        !rt->astNode->name->parts.empty()) {
                                        const std::string_view cn =
                                            rt->astNode->name->parts[0];
                                        const std::string_view mn =
                                            maCallee->member;
                                        // Owning-collection Try* methods —
                                        // element type from genericArgs[0] (T),
                                        // or genericArgs[1] (V) for
                                        // OwningDictionary.TryPopValue.
                                        const TypeRef* elemT = nullptr;
                                        if ((cn == "OwningStack" &&
                                             mn == "TryPop" && pos2 == 0) ||
                                            (cn == "OwningQueue" &&
                                             mn == "TryDequeue" && pos2 == 0) ||
                                            // §4.5 (#831/#1354): OwningArray
                                            // TryPopFirst/TryPopLast write the
                                            // element type T to the out param.
                                            (cn == "OwningArray" &&
                                             (mn == "TryPopFirst" ||
                                              mn == "TryPopLast") &&
                                             pos2 == 0) ||
                                            (cn == "OwningLinkedList" &&
                                             (mn == "TryPopFirst" ||
                                              mn == "TryPopLast") &&
                                             pos2 == 0) ||
                                            (cn == "OwningSet" &&
                                             mn == "TryTake" && pos2 == 1) ||
                                            (cn == "Channel" &&
                                             mn == "TryReceive" && pos2 == 0)) {
                                            if (!rt->genericArgs.empty() &&
                                                rt->genericArgs[0])
                                                elemT = rt->genericArgs[0];
                                        } else if (cn == "OwningDictionary" &&
                                                   mn == "TryPopValue" &&
                                                   pos2 == 1 &&
                                                   rt->genericArgs.size() >= 2 &&
                                                   rt->genericArgs[1]) {
                                            elemT = rt->genericArgs[1];
                                        }
                                        if (elemT) {
                                            // v0.171 (#1336): every v0.171
                                            // extraction Try's out param is `T?`
                                            // — the inline recipient is nullable
                                            // (its `!`/null-guarded discharge
                                            // narrows on the success path).
                                            // TryTake/TryReceive keep their
                                            // pre-v0.171 non-nullable local (they
                                            // are not part of the v0.171 sweep).
                                            bool v171 =
                                                mn == "TryPop" ||
                                                mn == "TryDequeue" ||
                                                mn == "TryPopFirst" ||
                                                mn == "TryPopLast" ||
                                                mn == "TryPopValue";
                                            TypeRef* nn =
                                                ctx_.makeTypeRef(*elemT);
                                            nn->nullable = v171 ? true : false;
                                            lsym->declType = nn;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    currentScope_->names[id->name] = lsym;
                }
            }
            // §28.13 / §28.11 / stdlib §2.16 (#603 #890): `out using var x`
            // declares a using-var–scoped handle whose type is derived from
            // the receiver's generic element argument.
            //
            // Handled cases (all are builtins with no MethodDecl):
            //   Weak<T>.TryUpgrade                           → Shared<T> (arg 0)
            //   OwningStack<T>.TryPop                         → T         (arg 0)
            //   OwningQueue<T>.TryDequeue                     → T         (arg 0)
            //   OwningArray<T>.TryPopFirst/TryPopLast         → T         (arg 0)
            //   OwningLinkedList<T>.TryPopFirst/TryPopLast    → T         (arg 0)
            //   OwningSet<T>.TryTake                          → T (T?, arg 1)
            //   OwningDictionary<K,V>.TryPopValue             → V         (arg 1)
            //   Channel<T>.TryReceive                        → T (T?, arg 0)
            //
            // For T?-typed out params the spec still delivers the non-nullable
            // T to the `out using var` (parallel to the `out var` strip above).
            if (a.modifier == "out using var" && a.value && currentScope_) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                                   a.value.get())) {
                    auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                    lsym->name = id->name;
                    lsym->decl = id;
                    // Derive the type from the builtin receiver.
                    if (auto* maCallee =
                            dynamic_cast<const ast::MemberAccessExpr*>(
                                ce->callee.get())) {
                        if (maCallee->target) {
                            if (const TypeRef* rt =
                                    ctx_.exprType(*maCallee->target)) {
                                if (rt->kind == TypeRef::Kind::Named &&
                                    !rt->declaration && rt->astNode &&
                                    rt->astNode->name &&
                                    !rt->astNode->name->parts.empty()) {
                                    const std::string_view cn =
                                        rt->astNode->name->parts[0];
                                    const std::string_view mn =
                                        maCallee->member;

                                    if (cn == "Weak" &&
                                        mn == "TryUpgrade" &&
                                        pos2 == 0 &&
                                        !rt->genericArgs.empty() &&
                                        rt->genericArgs[0]) {
                                        // Weak<T>.TryUpgrade → Shared<T>
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

                                    } else if (
                                        // per §28.11, #890: owning-collection
                                        // Try* methods — element type from
                                        // receiver's generic argument.
                                        ((cn == "OwningStack" &&
                                          mn == "TryPop" && pos2 == 0) ||
                                         (cn == "OwningQueue" &&
                                          mn == "TryDequeue" && pos2 == 0) ||
                                         // §4.5 (#831/#1354): OwningArray
                                         // TryPopFirst/TryPopLast (out using var).
                                         (cn == "OwningArray" &&
                                          (mn == "TryPopFirst" ||
                                           mn == "TryPopLast") &&
                                          pos2 == 0) ||
                                         (cn == "OwningLinkedList" &&
                                          (mn == "TryPopFirst" ||
                                           mn == "TryPopLast") &&
                                          pos2 == 0) ||
                                         (cn == "OwningSet" &&
                                          mn == "TryTake" && pos2 == 1) ||
                                         (cn == "Channel" &&
                                          mn == "TryReceive" && pos2 == 0)) &&
                                        !rt->genericArgs.empty() &&
                                        rt->genericArgs[0]) {
                                        // Element type = genericArgs[0] (T).
                                        // v0.171 (#1336): the v0.171 extraction
                                        // Trys (TryPop/TryDequeue/TryPopFirst/
                                        // TryPopLast) deliver `owned T?` — the
                                        // recipient is nullable. TryTake/
                                        // TryReceive are NOT part of the v0.171
                                        // sweep and keep the non-nullable local.
                                        const TypeRef* elemT = rt->genericArgs[0];
                                        bool v171 =
                                            mn == "TryPop" ||
                                            mn == "TryDequeue" ||
                                            mn == "TryPopFirst" ||
                                            mn == "TryPopLast";
                                        TypeRef* nn = ctx_.makeTypeRef(*elemT);
                                        nn->nullable = v171 ? true : false;
                                        lsym->declType = nn;

                                    } else if (
                                        // OwningDictionary<K,V>.TryPopValue
                                        // — value type = genericArgs[1] (V).
                                        cn == "OwningDictionary" &&
                                        mn == "TryPopValue" &&
                                        pos2 == 1 &&
                                        rt->genericArgs.size() >= 2 &&
                                        rt->genericArgs[1]) {
                                        // v0.171 (#1336): `owned V?` recipient.
                                        const TypeRef* valT = rt->genericArgs[1];
                                        TypeRef* nn = ctx_.makeTypeRef(*valT);
                                        nn->nullable = true;
                                        lsym->declType = nn;
                                    }
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
                TypeRef* savedLamRet = pendingLambdaReturnType_;
                std::vector<TypeRef*> savedLamParams = pendingLambdaParamTypes_;
                if (auto* lam =
                        dynamic_cast<const ast::LambdaExpr*>(a.value.get())) {
                    if (eventPayload)     pendingLambdaParamType_ = eventPayload;
                    else if (linqElem)    pendingLambdaParamType_ = linqElem;
                    // stdlib §5 (#1124): an `Aggregate(seed, (acc, x) => …)` fold
                    // lambda is `(TAccum, T) -> TAccum`. Seed BOTH parameters — acc
                    // from the seed's type, x from the source element — so an
                    // un-annotated `(acc, x)` resolves its body (else the inferred
                    // params stay untyped and codegen emits `ret void` against the
                    // accumulator return type → IR_FAIL).
                    else if (auto aggP = aggregateFoldLambdaParamTypes(*ce);
                             aggP.size() == 2 && lam->params.size() == 2)
                        pendingLambdaParamTypes_ = aggP;
                    // fix #829: a lambda bound to a function-typed parameter
                    // `(T1, …, Tn) -> R` — thread the positional Ti so an untyped
                    // lambda arg `(a, b) => …` gets its inferred param types (the
                    // body's operator/member resolution then sees real types).
                    // Skip a GENERIC callee: there the function-param type may be
                    // an unresolved type parameter (`(T) -> int`) inferred from
                    // another argument; the existing generic-inference path types
                    // the lambda param correctly, and threading the bare `T` here
                    // would override it with the unsubstituted parameter (#505).
                    if (calleeMd2 && calleeMd2->genericParams.empty() &&
                        pos2 < calleeMd2->params.size() &&
                        calleeMd2->params[pos2].type) {
                        if (auto* pt = ctx_.resolvedType(
                                *calleeMd2->params[pos2].type)) {
                            if (pt->kind == TypeRef::Kind::Function &&
                                !pt->funcParams.empty()) {
                                // #1140 / §14: a function-typed param `(T)->R`
                                // on a generic class Box<T> instantiated as
                                // Box<int> must yield `(int)->R` for lambda
                                // param inference, not the raw class type-param
                                // T. Build the class-level substitution map
                                // from the receiver's bound generic args
                                // (mirrors substituteMemberType's direct path).
                                std::unordered_map<std::string_view, TypeRef*>
                                    classSubst;
                                if (auto* ms2 = dynamic_cast<
                                        sema::MethodSymbol*>(calleeSym2)) {
                                    TypeSymbol* parentTs = ms2->parent;
                                    if (parentTs &&
                                        !parentTs->genericParams.empty()) {
                                        const ast::Expr* recv = nullptr;
                                        if (auto* maC = dynamic_cast<
                                                const ast::MemberAccessExpr*>(
                                                    ce->callee.get()))
                                            recv = maC->target.get();
                                        TypeRef* recvTy =
                                            recv ? ctx_.exprType(*recv)
                                                 : nullptr;
                                        // Fallback: local/param declared type.
                                        if (recv && (!recvTy ||
                                            recvTy->declaration != parentTs ||
                                            recvTy->genericArgs.empty())) {
                                            Symbol* sym =
                                                ctx_.exprSymbol(*recv);
                                            if (auto* loc = dynamic_cast<
                                                    LocalSymbol*>(sym)) {
                                                if (loc->declType &&
                                                    loc->declType->declaration
                                                        == parentTs &&
                                                    !loc->declType->genericArgs
                                                        .empty())
                                                    recvTy = loc->declType;
                                            } else if (auto* par =
                                                           dynamic_cast<
                                                               ParameterSymbol*>(
                                                               sym)) {
                                                if (par->declType) {
                                                    if (auto* prt =
                                                            ctx_.resolvedType(
                                                                *par->declType))
                                                        if (prt->declaration
                                                                == parentTs &&
                                                            !prt->genericArgs
                                                                .empty())
                                                            recvTy = prt;
                                                }
                                            }
                                        }
                                        if (recvTy &&
                                            recvTy->declaration == parentTs &&
                                            !recvTy->genericArgs.empty()) {
                                            for (std::size_t gi = 0;
                                                 gi < parentTs->genericParams
                                                              .size() &&
                                                 gi < recvTy->genericArgs
                                                              .size(); ++gi) {
                                                if (parentTs->genericParams[gi]
                                                    && recvTy->genericArgs[gi])
                                                    classSubst[parentTs
                                                        ->genericParams[gi]
                                                        ->name] =
                                                        recvTy->genericArgs[gi];
                                            }
                                        }
                                    }
                                }
                                pendingLambdaParamTypes_.clear();
                                for (const auto& fp : pt->funcParams) {
                                    TypeRef* paramTy = fp.type;
                                    if (!classSubst.empty())
                                        paramTy = substituteReturnType(
                                            paramTy, classSubst);
                                    pendingLambdaParamTypes_.push_back(paramTy);
                                }
                            }
                        }
                    }
                    // issue #777 / §30.6: when an async lambda is bound to a
                    // function-typed parameter `(…) -> Task<T>` / `OwningTask<T>`,
                    // thread the peeled element T so the lambda visitor rejects a
                    // forgotten `await` in its body (same as the typed-local
                    // path). Positional binding via calleeMd2/pos2 mirrors the
                    // out-var inference above; the common positional call gets
                    // the hint, named/overloaded forms simply do not.
                    if (lam->isAsync && calleeMd2 &&
                        pos2 < calleeMd2->params.size() &&
                        calleeMd2->params[pos2].type) {
                        if (auto* pt = ctx_.resolvedType(
                                *calleeMd2->params[pos2].type)) {
                            if (pt->kind == TypeRef::Kind::Function &&
                                pt->funcReturn &&
                                isTaskShapedRef(pt->funcReturn) &&
                                !pt->funcReturn->genericArgs.empty())
                                pendingLambdaReturnType_ =
                                    pt->funcReturn->genericArgs.front();
                        }
                    }
                    (void)lam;
                }
                // #1051 / §5.1: target-type an unsuffixed integer literal
                // argument to a `uint`/`long`/`ulong` parameter, so a literal
                // larger than `int.MaxValue` widens to the parameter type rather
                // than being rejected — mirroring the variable-initializer hint
                // (#184). Reset to None for any other parameter type so a too-
                // large literal for an `int` parameter still rejects.
                PrimitiveType savedArgHint = currentLiteralWideningHint_;
                currentLiteralWideningHint_ = PrimitiveType::None;
                if (calleeMd2 && pos2 < calleeMd2->params.size() &&
                    calleeMd2->params[pos2].type) {
                    if (auto* prt = ctx_.resolvedType(
                            *calleeMd2->params[pos2].type)) {
                        if (prt->kind == TypeRef::Kind::Primitive &&
                            (prt->primitive == PrimitiveType::Long ||
                             prt->primitive == PrimitiveType::ULong ||
                             prt->primitive == PrimitiveType::UInt))
                            currentLiteralWideningHint_ = prt->primitive;
                    }
                }
                visitExpr(*a.value);
                currentLiteralWideningHint_ = savedArgHint;
                pendingLambdaParamType_ = savedPending;
                pendingLambdaReturnType_ = savedLamRet;
                pendingLambdaParamTypes_ = savedLamParams;
            }
            // per §7.4 (#1188): a `ref`/`out` argument can be reassigned by the
            // callee — including being set to null — so a prior null-narrowing
            // (`nullNarrowed_`) or §12.7 type-narrowing (`narrowings_`) on the
            // passed-in local no longer holds after the call. Invalidate it,
            // mirroring the AssignmentStmt invalidation, so a later member
            // access re-requires a guard instead of dereferencing a nulled
            // value (ACCEPT_TRAP → SIGSEGV). A by-value argument cannot mutate
            // the caller's local, so its narrowing survives. The `out var` /
            // `out using var` forms declare a fresh local that has no prior
            // narrowing, so erasing is a harmless no-op there.
            if (a.value &&
                (a.modifier == "ref" || a.modifier == "out" ||
                 a.modifier == "out var" || a.modifier == "out using var" ||
                 a.modifier == "out owned var")) {  // per §22.4 / v0.160
                if (auto* aid = dynamic_cast<const ast::IdentifierExpr*>(
                        a.value.get())) {
                    if (aid->name != "this" && aid->name != "base") {
                        if (Symbol* asym = lookupName(aid->name)) {
                            if (dynamic_cast<LocalSymbol*>(asym) ||
                                dynamic_cast<ParameterSymbol*>(asym)) {
                                narrowings_.erase(asym);
                                nullNarrowed_.erase(asym);
                            }
                        }
                    }
                }
            }
            ++pos2;
        }
    } else if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
        if (nc->callee) visitExpr(*nc->callee);
        for (const auto& a : nc->args) {
            if (a.value) visitExpr(*a.value);
        }
        // §5.3 (#839): `T?(x)` — nullable-target primitive cast. The lexer
        // produces `?(` as a 2-char punctuator, so `long?(x)` is parsed as a
        // NullCondCallExpr whose callee is the bare primitive-type keyword.
        // When the callee is a primitive name with no symbol, set the result
        // type to that primitive nullable (e.g. `long?`) so that inferred
        // locals, null checks, and the `!` unwrap all see the correct type.
        bool typedAsPrimCast = false;
        if (nc->callee) {
            if (auto* id =
                    dynamic_cast<const ast::IdentifierExpr*>(nc->callee.get())) {
                if (!ctx_.exprSymbol(*nc->callee)) {
                    PrimitiveType pt = primitiveFromName(id->name);
                    if (pt != PrimitiveType::None) {
                        ctx_.setExprType(
                            &e, ctx_.primitiveTypeRef(pt, /*nullable=*/true));
                        typedAsPrimCast = true;
                    }
                }
            }
        }
        if (!typedAsPrimCast) {
            // §27.7 (#738): `f?(args)` invokes the nullable function value `f`
            // when non-null, else is a no-op; the result is the function's
            // return type made NULLABLE (null when f is null). Type it so an
            // inferred local / null check / arithmetic on the result resolves
            // (previously untyped → codegen produced a wrong value).
            if (nc->callee) {
                if (TypeRef* ft = ctx_.exprType(*nc->callee))
                    if (ft->kind == TypeRef::Kind::Function && ft->funcReturn) {
                        TypeRef* r = ctx_.makeTypeRef();
                        *r = *ft->funcReturn;
                        r->nullable = true;
                        ctx_.setExprType(&e, r);
                    }
            }
        }
    } else if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        if (ix->target) visitExpr(*ix->target);
        for (const auto& a : ix->indices) {
            if (a.value) visitExpr(*a.value);
        }
    } else if (auto* bg = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        if (bg->operand) visitExpr(*bg->operand);
    } else if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        // §22.4 (#968): `out var x` at a CONSTRUCTOR call site declares x in the
        // current scope, exactly as for a method call (the CallExpr path above).
        // Without this `new C(out var y)` reported `unresolved identifier y`.
        // Resolve the constructed type's single (or arity-matching) constructor
        // to infer the out-var's type from the matching `out` parameter.
        const ast::ConstructorDecl* ctorDecl = nullptr;
        if (ne->type) {
            if (auto* rt = ctx_.resolvedType(*ne->type)) {
                if (rt->kind == TypeRef::Kind::Named && rt->declaration) {
                    std::vector<const ast::ConstructorDecl*> cds;
                    for (Symbol* m : rt->declaration->members)
                        if (auto* cs = dynamic_cast<ConstructorSymbol*>(m))
                            if (!cs->isStatic)
                                if (auto* cd =
                                        dynamic_cast<const ast::ConstructorDecl*>(
                                            cs->decl))
                                    cds.push_back(cd);
                    if (cds.size() == 1)
                        ctorDecl = cds[0];
                    else
                        for (auto* cd : cds)
                            if (cd->params.size() == ne->args.size()) {
                                ctorDecl = cd;
                                break;
                            }
                }
            }
        }
        std::size_t posn = 0;
        for (const auto& a : ne->args) {
            if (a.modifier == "out var" && a.value && currentScope_) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                                   a.value.get())) {
                    auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                    lsym->name = id->name;
                    lsym->decl = id;
                    if (ctorDecl && posn < ctorDecl->params.size() &&
                        ctorDecl->params[posn].type) {
                        lsym->declType = ctx_.resolvedType(
                            *ctorDecl->params[posn].type);
                        // out params are nullable refs — strip nullable (the
                        // variable holds the non-nullable value), as for methods.
                        if (lsym->declType && lsym->declType->nullable) {
                            TypeRef* nn = ctx_.makeTypeRef(*lsym->declType);
                            nn->nullable = false;
                            lsym->declType = nn;
                        }
                    }
                    currentScope_->names[id->name] = lsym;
                }
            }
            // §22.4 / v0.160 (#1305): `out owned var x` at a constructor call
            // site declares x inline (parallel to `out var` above). per §22.4
            if (a.modifier == "out owned var" && a.value && currentScope_) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                                   a.value.get())) {
                    auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                    lsym->name = id->name;
                    lsym->decl = id;
                    if (ctorDecl && posn < ctorDecl->params.size() &&
                        ctorDecl->params[posn].type) {
                        lsym->declType = ctx_.resolvedType(
                            *ctorDecl->params[posn].type);
                        if (lsym->declType && lsym->declType->nullable) {
                            TypeRef* nn = ctx_.makeTypeRef(*lsym->declType);
                            nn->nullable = false;
                            lsym->declType = nn;
                        }
                    }
                    currentScope_->names[id->name] = lsym;
                }
            }
            // #1051 / §5.1: target-type an unsuffixed integer literal constructor
            // argument to a `uint`/`long`/`ulong` parameter (an explicit ctor
            // parameter, or a positional-record field), so a literal > int.MaxValue
            // widens instead of being rejected. Reset to None otherwise so a too-
            // large literal for an `int` field still rejects.
            PrimitiveType savedNewHint = currentLiteralWideningHint_;
            currentLiteralWideningHint_ = PrimitiveType::None;
            {
                PrimitiveType hintPrim = PrimitiveType::None;
                auto primOf = [&](const ast::Type* ty) {
                    if (ty)
                        if (auto* prt = ctx_.resolvedType(*ty))
                            if (prt->kind == TypeRef::Kind::Primitive)
                                hintPrim = prt->primitive;
                };
                if (ctorDecl && posn < ctorDecl->params.size() &&
                    ctorDecl->params[posn].type) {
                    primOf(ctorDecl->params[posn].type.get());
                } else if (ne->type) {
                    if (auto* rt = ctx_.resolvedType(*ne->type))
                        if (rt->kind == TypeRef::Kind::Named && rt->declaration)
                            if (auto* td = dynamic_cast<const ast::TypeDecl*>(
                                    rt->declaration->decl))
                                if (td->hasPositionalFields &&
                                    posn < td->positionalFields.size() &&
                                    td->positionalFields[posn].type) {
                                    const ast::Type* ft =
                                        td->positionalFields[posn].type.get();
                                    // #1052: a generic positional-record field
                                    // whose declared type is a bare type
                                    // parameter (`Box<T>(V: T)`) — resolve T to
                                    // the concrete generic argument of THIS
                                    // instantiation (`Box<uint>` → uint) so the
                                    // literal widens to it.
                                    bool subst = false;
                                    if (ft->name && ft->name->parts.size() == 1 &&
                                        !td->genericParams.empty()) {
                                        for (std::size_t k = 0;
                                             k < td->genericParams.size(); ++k)
                                            if (td->genericParams[k].name ==
                                                    ft->name->parts[0]) {
                                                if (k < rt->genericArgs.size() &&
                                                    rt->genericArgs[k] &&
                                                    rt->genericArgs[k]->kind ==
                                                        TypeRef::Kind::Primitive)
                                                    hintPrim =
                                                        rt->genericArgs[k]->primitive;
                                                subst = true;
                                                break;
                                            }
                                    }
                                    if (!subst) primOf(ft);
                                }
                }
                if (hintPrim == PrimitiveType::Long ||
                    hintPrim == PrimitiveType::ULong ||
                    hintPrim == PrimitiveType::UInt)
                    currentLiteralWideningHint_ = hintPrim;
            }
            if (a.value) visitExpr(*a.value);
            currentLiteralWideningHint_ = savedNewHint;
            ++posn;
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
            checkThrowableType(*th->operand);  // §31.1 (#739)
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
        // fix #829: the inferred positional parameter types from the lambda's
        // expected function type (typed-local init / assignment / call arg).
        // Consumed once and cleared so a nested lambda doesn't inherit them.
        std::vector<TypeRef*> inferredParamTypes = pendingLambdaParamTypes_;
        pendingLambdaParamTypes_.clear();
        // issue #777 / §30.6: the expected async element type T (peeled from a
        // `(…) -> Task<T>` context), threaded in to detect a forgotten `await`.
        // Consumed once so nested lambdas don't inherit it.
        TypeRef* lamRetHint = pendingLambdaReturnType_;
        pendingLambdaReturnType_ = nullptr;
        Scope lamScope;
        lamScope.parent = currentScope_;
        std::vector<sema::Symbol*> lamParamSyms;
        for (size_t pi = 0; pi < lam->params.size(); ++pi) {
            const auto& p = lam->params[pi];
            auto* ps = ctx_.makeSymbol<ParameterSymbol>();
            ps->name = p.name;
            ps->modifier = p.modifier;
            if (p.type) ps->declType = p.type.get();
            else if (eventParamType && lam->params.size() == 1)
                ps->resolvedDeclType = eventParamType;  // §24.3 handler param
            // fix #829: an untyped param takes its inferred type from the
            // lambda's expected function type, positionally.
            else if (pi < inferredParamTypes.size() && inferredParamTypes[pi])
                ps->resolvedDeclType = inferredParamTypes[pi];
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
        // #721: a lambda inside an iterator method is its own callable, not an
        // iterator — `return;` in it is governed by the lambda's own rules.
        bool saveIter = inIteratorMethod_;
        inIteratorMethod_ = false;
        // §29.9 (#747 R42): mark that we are inside a lambda body so a `yield`
        // in it is rejected (yield is method-level only). Saved/restored for
        // nested lambdas.
        bool saveInLambda = inLambda_;
        inLambda_ = true;
        currentScope_ = &lamScope;
        returnCtx_ = ReturnCtx::None;
        currentReturnType_ = nullptr;
        loopDepth_ = 0;
        catchDepth_ = 0;
        if (lam->exprBody) visitExpr(*lam->exprBody);
        for (const auto& s : lam->stmts) if (s) visitStmt(*s);
        // issue #777 / §30.6: forgotten `await` in an expression-bodied async
        // lambda. The body must produce the element type T threaded in as
        // lamRetHint, not a `Task` value. A Task-shaped body where T is not
        // itself Task-shaped is a forgotten await — codegen would store the
        // task pointer into the element-sized async result slot (corruption /
        // SIGSEGV). The legitimate `(…) -> Task<Task<U>>` case is exempt because
        // lamRetHint (= Task<U>) is itself Task-shaped.
        if (lam->isAsync && lam->exprBody && lamRetHint &&
            !isTaskShapedRef(lamRetHint) &&
            isTaskShapedRef(ctx_.exprType(*lam->exprBody))) {
            ctx_.diagnostics().report(
                Severity::Error, lam->exprBody->span.begin,
                "an expression-bodied `async` lambda's body must produce the "
                "element type of its `Task<T>`/`OwningTask<T>` return, not a "
                "`Task` value; add `await` (e.g. `async () => await Producer()`)"
                " (§30.6)");
        }
        currentScope_ = saveScope;
        returnCtx_ = saveRet;
        currentReturnType_ = saveRetTy;
        inAsyncContext_ = saveAsync;
        inIteratorMethod_ = saveIter;
        inLambda_ = saveInLambda;
        loopDepth_ = saveLoop;
        catchDepth_ = saveCatch;
        // §A.7: set the lambda's exprType to a Function TypeRef so
        // that `var f := lam; f(...)` calls type-check correctly and
        // lowerCall can detect it as a function-typed local.
        {
            auto* lamTy = ctx_.makeTypeRef();
            lamTy->kind = TypeRef::Kind::Function;
            for (size_t pi = 0; pi < lam->params.size(); ++pi) {
                const auto& p = lam->params[pi];
                TypeRef::FuncParam fp;
                fp.modifier = p.modifier;
                fp.name = p.name;
                if (p.type) fp.type = ctx_.resolvedType(*p.type);
                // fix #829: reflect an inferred param type in the lambda's own
                // function type so a later `f(...)` call type-checks against it.
                else if (pi < inferredParamTypes.size() && inferredParamTypes[pi])
                    fp.type = inferredParamTypes[pi];
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
            // §30.6 (#800, partial): the function type of an ASYNC lambda has a
            // `Task<T>` (or `OwningTask<T>` for a class-typed, ownership-
            // transferring result) return — the body's natural result T is
            // wrapped, exactly as for an async method (§30.1). Without this the
            // INFERRED type of `var f := async () => do return 1; end` was
            // `() -> T`, so `await f()` was rejected ("await requires a Task
            // operand"); an explicit `f: () -> Task<T>` annotation supplied the
            // wrapping but inference did not.
            //
            // Applies to BOTH block- and expression-bodied async lambdas (#800
            // Layer-2). The codegen reads this wrapped funcReturn (Task<T>) for an
            // expression-bodied async lambda too (LlvmBackend_Exprs.cpp lowerLambda
            // derives the closure's async result type from the lambda's exprType
            // funcReturn when it is async), so the body's value is wrapped into the
            // outer Task instead of returned raw.
            if (lam->isAsync && inferredReturn) {
                bool ownedResult =
                    inferredReturn->kind == TypeRef::Kind::Named &&
                    inferredReturn->declaration &&
                    inferredReturn->declaration->kind == ast::TypeDeclKind::Class;
                auto* taskTy = ctx_.makeTypeRef();
                taskTy->kind = TypeRef::Kind::Named;
                taskTy->nullable = false;
                auto* ann = new ast::Type();
                ann->kind = ast::TypeKind::Named;
                auto* nm = new ast::QualifiedName();
                nm->parts.push_back(ownedResult ? "OwningTask" : "Task");
                ann->name.reset(nm);
                taskTy->astNode = ann;
                taskTy->genericArgs.push_back(inferredReturn);
                inferredReturn = taskTy;
            }
            lamTy->funcReturn = inferredReturn;
            ctx_.setExprType(lam, lamTy);
        }
    } else if (auto* ce = dynamic_cast<const ast::CaseExpr*>(&e)) {
        if (ce->scrutinee) visitExpr(*ce->scrutinee);
        validateCaseArms(ce->arms);  // §16.4 / §19.6 (#738)
        TypeRef* scrutTy =
            ce->scrutinee ? ctx_.exprType(*ce->scrutinee) : nullptr;
        // §10.6 (#772): "Inside a `case` whose scrutinee is an enum, the enum
        // prefix may be omitted." This holds throughout the construct, INCLUDING
        // arm bodies (`Red => Green`), not only patterns. Inject the scrutinee
        // enum's constants into a scope that parents the arm bodies so a bare
        // `Green` resolves; the per-arm binding/destructuring scopes set
        // `parent = currentScope_`, so they inherit it too. Restored after the
        // loop. (The qualified `Color.Green` form already worked.) // per §10.6
        Scope armEnumScope;
        Scope* preArmEnumScope = currentScope_;
        bool savedEnumArms = inEnumScrutineeCaseArms_;
        bool scrutIsEnum = scrutTy && scrutTy->kind == TypeRef::Kind::Named &&
            scrutTy->declaration &&
            scrutTy->declaration->kind == ast::TypeDeclKind::Enum;
        if (scrutIsEnum && currentScope_) {
            armEnumScope.parent = currentScope_;
            for (Symbol* m : scrutTy->declaration->members)
                if (dynamic_cast<EnumConstantSymbol*>(m))
                    armEnumScope.names[m->name] = m;
            currentScope_ = &armEnumScope;
            inEnumScrutineeCaseArms_ = true;
        }
        // §19/§5.1 (#1202): a relational (`> EXPR`) or range (`lo..hi`) pattern
        // operand is compared against the scrutinee by codegen with a single
        // ICmp/FCmp keyed on the scrutinee's representation (litAtWidth/reconcile,
        // LlvmBackend_Exprs.cpp). When the operand's primitive is in a DIFFERENT
        // comparison class than the scrutinee — a string pointer vs a scalar, a
        // bool i1 vs a wider int, a float vs an integer, a decimal vs either —
        // those reach CreateICmp with mismatched operand types and abort the
        // backend. Mirror the #964 ConstantPattern guard: reject such a pattern
        // here with the same §19 diagnostic. A genuine cross-numeric
        // accept/reconcile is a separate §19.3 ruling and is out of scope. A
        // `decimal` SCRUTINEE is excluded — its codegen path (decOperand/decCmp,
        // #1152) already bridges any numeric bound, so a decimal scrutinee with
        // an integer/float bound compiles and must keep doing so.
        auto cmpClassOf = [](PrimitiveType p) -> int {
            switch (p) {
                case PrimitiveType::String: return 1;
                case PrimitiveType::Bool:   return 2;
                case PrimitiveType::Char:
                case PrimitiveType::Byte:  case PrimitiveType::SByte:
                case PrimitiveType::Short: case PrimitiveType::UShort:
                case PrimitiveType::Int:   case PrimitiveType::UInt:
                case PrimitiveType::Long:  case PrimitiveType::ULong:
                    return 3;  // integer family — codegen sext/zext-bridges widths
                case PrimitiveType::Float:
                case PrimitiveType::Double:
                    return 4;  // floating family
                case PrimitiveType::Decimal: return 5;
                default: return 0;  // None/Object — not a relational/range operand
            }
        };
        auto checkPatternOperand = [&](const ast::Expr* operand) {
            if (!operand) return;
            if (!(ce->scrutinee && scrutTy &&
                  scrutTy->kind == TypeRef::Kind::Primitive))
                return;
            const TypeRef* ot = ctx_.exprType(*operand);
            if (!ot || ot->kind != TypeRef::Kind::Primitive) return;
            int sc = cmpClassOf(scrutTy->primitive);
            int oc = cmpClassOf(ot->primitive);
            // Fire only when the scrutinee is one of the directly-compared
            // classes (string/bool/integer/float) and the operand is a primitive
            // in a different comparison class. A decimal scrutinee (sc == 5) is
            // excluded — see the note above.
            if ((sc >= 1 && sc <= 4) && oc != 0 && sc != oc)
                ctx_.diagnostics().report(
                    Severity::Error, operand->span.begin,
                    "case pattern operand type is not compatible with the "
                    "scrutinee type (\xC2\xA7""19) — a `case` pattern operand "
                    "must be comparable to the scrutinee");
        };
        for (const auto& arm : ce->arms) {
            // §19 / §A (#727): in a SCRUTINEE-LESS `case`, an arm's pattern is a
            // boolean CONDITION expression (a ConstantPattern) — typecheck its
            // constants so identifiers resolve and codegen can lower them.
            // §19 / §A.8 (#957): a scrutinee-BEARING arm whose pattern is a
            // ConstantPattern referencing a named const (`C.K`, a module-level
            // const) must also be typechecked, so the const acquires an
            // exprType/symbol and codegen can lower it. Enum-constant patterns
            // over an enum scrutinee are excluded — those are bare/qualified
            // member names resolved by name against the scrutinee enum in
            // codegen (enumConstantOfPatternExpr); visiting them here would
            // either fail to resolve (plain scope) or divert that path.
            if (!ce->scrutinee || !scrutIsEnum) {
                if (auto* cp = dynamic_cast<const ast::ConstantPattern*>(
                        arm.pattern.get()))
                    for (const auto& c : cp->constants) {
                        if (!c) continue;
                        visitExpr(*c);
                        // §19 (#964/#1213): a scrutinee-bearing arm whose
                        // constant has a comparison class incompatible with the
                        // scrutinee crashes codegen (ICmp with mismatched LLVM
                        // operand types — SIGABRT in ICmpInst::AssertOK). The
                        // #964 guard was string/bool-only; #1213 extends it to
                        // cover float/decimal constants over an integer scrutinee
                        // (and the symmetric directions) by reusing the same
                        // cmpClassOf-based checkPatternOperand that #1202 added
                        // for RelOpPattern and RangePattern. per §19/§5.1.
                        checkPatternOperand(c.get());  // per §19/§5.1 (#1213)
                    }
            }
            // §19.3 (#726): a relational pattern (`> limit`) carries an operand
            // expression whose identifiers must be typed so codegen can lower a
            // VARIABLE operand (a literal already worked). Visit it.
            if (auto* rop =
                    dynamic_cast<const ast::RelOpPattern*>(arm.pattern.get())) {
                if (rop->expr) {
                    visitExpr(*rop->expr);
                    // per §19/§5.1 (#1202)
                    checkPatternOperand(rop->expr.get());
                }
            }
            // §19.3 / §A.8 (#1202): a range pattern's bounds are constant
            // operands; type them (so the compatibility check and codegen see
            // their types) and reject a bound whose comparison class is
            // incompatible with the scrutinee instead of aborting the backend.
            if (auto* rangep =
                    dynamic_cast<const ast::RangePattern*>(arm.pattern.get())) {
                if (auto* rng = dynamic_cast<const ast::RangeExpr*>(
                        rangep->rangeExpr.get())) {
                    if (rng->lo) {
                        visitExpr(*rng->lo);
                        checkPatternOperand(rng->lo.get());  // per §19/§5.1 (#1202)
                    }
                    if (rng->hi) {
                        visitExpr(*rng->hi);
                        checkPatternOperand(rng->hi.get());  // per §19/§5.1 (#1202)
                    }
                }
            }
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
                // §19.5 (#1203): a typed binding pattern `var n: T` is a
                // type-test + binding. If T is a reference type (class,
                // interface, or string) but the scrutinee's static type is a
                // primitive value type (non-nullable, not string, not Object),
                // the test can NEVER match — a value-type primitive is never
                // a reference type. Reject it here rather than miscompiling
                // (an unconditional wrong-value match for string-typed binding,
                // or SIGABRT in emitIsA for a class-typed binding on an i32).
                // per §19.5 (#1203)
                if (bp->type && scrutTy && lsym->declType) {
                    bool scrutIsValuePrim =
                        scrutTy->kind == TypeRef::Kind::Primitive &&
                        !scrutTy->nullable &&
                        scrutTy->primitive != PrimitiveType::String &&
                        scrutTy->primitive != PrimitiveType::Object;
                    if (scrutIsValuePrim) {
                        TypeRef* bndTy = lsym->declType;
                        bool bndIsRefType =
                            (bndTy->kind == TypeRef::Kind::Primitive &&
                             bndTy->primitive == PrimitiveType::String) ||
                            (bndTy->kind == TypeRef::Kind::Named &&
                             bndTy->declaration &&
                             (bndTy->declaration->kind ==
                                  ast::TypeDeclKind::Class ||
                              bndTy->declaration->kind ==
                                  ast::TypeDeclKind::Interface));
                        if (bndIsRefType)
                            ctx_.diagnostics().report(
                                Severity::Error, bp->span.begin,
                                "binding pattern type is incompatible with "
                                "the scrutinee's static type — a value-type "
                                "scrutinee can never match a reference-typed "
                                "binding (§19.5)");
                    }
                }
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
                // §19.3 / Appendix C: "a record-destructuring pattern's
                // positional-binding count matches the record's positional-
                // field count." Without this check an over- or under-bound
                // pattern (`Pt(x, y, z)` on a 2-field record) was accepted, then
                // codegen's patternMatches could not destructure it and bailed,
                // leaving malformed IR ("case.end has no terminator").
                if (recTy && !fields.empty() &&
                    dp->subPatterns.size() != fields.size()) {
                    ctx_.diagnostics().report(
                        Severity::Error, dp->span.begin,
                        "record-destructuring pattern `" + std::string(lastName) +
                        "(...)` binds " + std::to_string(dp->subPatterns.size()) +
                        " value(s) but record `" + std::string(lastName) +
                        "` has " + std::to_string(fields.size()) +
                        " positional field(s); the binding count must match "
                        "(§19.3)");
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
                        lsym->decl = sp.nakedAnchor.get();  // #1271 real Node anchor
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
                // #726 / §19.3: also bind the names inside NESTED destructuring
                // sub-patterns (`Outer(Inner(v), b)`). The top-level loop above
                // only handles naked/`var` binds; a nested DestructuringPattern
                // sub-pattern was skipped, so `v` never entered armScope and the
                // body's identifier resolution failed. Walk a worklist of nested
                // patterns and register their binds.
                // #1281 (§19.3): the nested binding's declType MUST be its inner
                // record field's type — the top-level binds set it, but the nested
                // walk left it null ("best-effort"). With a null declType a nested
                // binding of a class/record field could not resolve a member access
                // in the arm body (`Outer(Inner(x)) => x.Id` — sema can't type `x`,
                // so `x.Id` returns no symbol → fieldAddr null → the case-expr's
                // `case.end` is left unterminated → IR verification failure). Set
                // it from the inner record's positional field at each index.
                {
                    auto fieldsOfRecord =
                        [&](const ast::QualifiedName* tn) -> std::vector<FieldSymbol*> {
                        std::vector<FieldSymbol*> out;
                        if (!tn || tn->parts.empty()) return out;
                        std::string_view nm = tn->parts.back();
                        for (const auto& sym : ctx_.symbols()) {
                            auto* ts = dynamic_cast<TypeSymbol*>(sym.get());
                            if (!ts || ts->name != nm ||
                                ts->kind != ast::TypeDeclKind::Record)
                                continue;
                            for (Symbol* m : ts->members) {
                                auto* fs = dynamic_cast<FieldSymbol*>(m);
                                if (!fs) continue;
                                bool isStatic = false;
                                for (auto& md : fs->modifiers)
                                    if (md == "static") { isStatic = true; break; }
                                if (!isStatic) out.push_back(fs);
                            }
                            break;
                        }
                        return out;
                    };
                    std::vector<const ast::DestructuringPattern*> nestedWork;
                    for (const auto& sp : dp->subPatterns)
                        if (sp.nakedBinding.empty())
                            if (auto* nd = dynamic_cast<
                                    const ast::DestructuringPattern*>(
                                    sp.pattern.get()))
                                nestedWork.push_back(nd);
                    while (!nestedWork.empty()) {
                        const ast::DestructuringPattern* nd = nestedWork.back();
                        nestedWork.pop_back();
                        std::vector<FieldSymbol*> ndFields =
                            fieldsOfRecord(nd->typeName.get());
                        for (std::size_t i = 0; i < nd->subPatterns.size(); ++i) {
                            const auto& s = nd->subPatterns[i];
                            TypeRef* fieldTy = nullptr;
                            if (i < ndFields.size() && ndFields[i]->posParam &&
                                ndFields[i]->posParam->type)
                                fieldTy = ctx_.resolvedType(
                                    *ndFields[i]->posParam->type);
                            if (!s.nakedBinding.empty()) {
                                auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                                lsym->name = s.nakedBinding;
                                lsym->decl = s.nakedAnchor.get();  // #1271 real Node anchor
                                lsym->declType = fieldTy;  // #1281
                                armScope.names[s.nakedBinding] = lsym;
                            } else if (auto* vbp =
                                           dynamic_cast<const ast::BindingPattern*>(
                                               s.pattern.get())) {
                                auto* lsym = ctx_.makeSymbol<LocalSymbol>();
                                lsym->name = vbp->name;
                                lsym->decl = vbp;
                                lsym->declType = vbp->type
                                    ? ctx_.resolvedType(*vbp->type) : fieldTy;  // #1281
                                armScope.names[vbp->name] = lsym;
                            } else if (auto* deeper = dynamic_cast<
                                           const ast::DestructuringPattern*>(
                                           s.pattern.get())) {
                                nestedWork.push_back(deeper);
                            }
                        }
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
        currentScope_ = preArmEnumScope;  // §10.6 (#772): pop the enum scope
        inEnumScrutineeCaseArms_ = savedEnumArms;
        // §19.6: an expression-position `case` whose value cannot be proven
        // to be covered by some arm raises MatchFailureException at runtime;
        // the compiler emits a non-exhaustiveness warning here. Statement-
        // position `case` (CaseStmt) is exempt — an unmatched value simply
        // falls through with no action (§19.6).
        checkCaseExhaustiveness(*ce, scrutTy);
    } else if (auto* we = dynamic_cast<const ast::WithExpr*>(&e)) {
        if (we->source) visitExpr(*we->source);
        // §9.5 (#933): `with` on a CLASS (or interface) source is the
        // object-initializer form (§22.11), valid ONLY on a freshly-`new`-
        // constructed instance. Applied to an existing class instance, codegen
        // mutates that instance in place and returns an alias of it (lowerWith)
        // — silently violating non-destructive-update expectations. The spec is
        // explicit: "`with` does not apply to existing class instances for
        // non-destructive update; classes use property assignment." Records
        // (value types) are a copy and accept any source, so this gate is
        // class/interface-only and skips a `new` source.
        if (we->source) {
            if (auto* st = ctx_.exprType(*we->source)) {
                if (st->kind == TypeRef::Kind::Named && st->declaration &&
                    st->declaration->kind != ast::TypeDeclKind::Record &&
                    st->declaration->kind != ast::TypeDeclKind::Enum &&
                    !dynamic_cast<const ast::NewExpr*>(we->source.get())) {
                    ctx_.diagnostics().report(
                        Severity::Error, we->source->span.begin,
                        "`with` does not apply to an existing class instance — "
                        "the object-initializer `with` form is valid only on a "
                        "`new` class expression; mutate an existing instance "
                        "through property assignment instead (§9.5)");
                }
            }
        }
        // §9.5, #875-878: validate each `with` assignment target.
        // Resolve the source's TypeSymbol so we can look up named fields.
        TypeSymbol* srcTs = nullptr;
        if (we->source) {
            if (auto* st = ctx_.exprType(*we->source)) {
                if (st->kind == TypeRef::Kind::Named && st->declaration &&
                    st->declaration->kind == ast::TypeDeclKind::Record)
                    srcTs = st->declaration;
            }
        }
        // §9.5 (#935): a field name may appear at most once in a `with`
        // initializer list. The names are static identifiers, so a duplicate is
        // always statically detectable; the first assignment would be dead code.
        // This mirrors the collection-literal rule (§15.4): a duplicate
        // Dictionary key is a compile error when all duplicates are compile-time
        // constants. Applies to record copies and class object-initializers
        // alike (independent of the source-type validation below).
        std::unordered_set<std::string_view> seenWithNames;
        for (const auto& ia : we->assigns) {
            if (ia.value) visitExpr(*ia.value);
            if (!seenWithNames.insert(ia.name).second) {
                ctx_.diagnostics().report(
                    Severity::Error,
                    ia.value ? ia.value->span.begin : we->span.begin,
                    "field `" + std::string(ia.name) + "` is assigned more "
                    "than once in this `with` expression (§9.5)");
                continue;
            }
            // §9.5: validate only when we have a known record source type.
            if (!srcTs) continue;
            SourceLocation valLoc =
                ia.value ? ia.value->span.begin : we->span.begin;
            Symbol* mem = lookupMember(srcTs, ia.name);
            if (!mem) {
                // #876: nonexistent field name.
                ctx_.diagnostics().report(
                    Severity::Error, valLoc,
                    "field `" + std::string(ia.name) + "` does not exist "
                    "in record `" + std::string(srcTs->name) +
                    "` — `with` targets must be positional-field properties, "
                    "auto-properties, or public fields (§9.5, #876)");
                continue;
            }
            // §13.1 (#934): enforce member visibility on `with` targets.
            // A private property/field is inaccessible from outside the
            // declaring type — the same rule that governs ordinary member
            // access (checkMemberVisibility). Without this check a `with`
            // expression silently bypassed visibility for private backing
            // storage (BAD_ACCEPT).
            if (auto* msym = dynamic_cast<MemberSymbol*>(mem))
                checkMemberVisibility(msym, ia.name, valLoc);
            if (dynamic_cast<MethodSymbol*>(mem) ||
                dynamic_cast<ConstructorSymbol*>(mem) ||
                dynamic_cast<DestructorSymbol*>(mem)) {
                // #877: method is not a valid with target.
                ctx_.diagnostics().report(
                    Severity::Error, valLoc,
                    "`" + std::string(ia.name) + "` is a method, not a "
                    "valid `with` target — `with` targets must be "
                    "positional-field properties, auto-properties, or "
                    "public fields (§9.5, #877)");
                continue;
            }
            if (auto* ps = dynamic_cast<PropertySymbol*>(mem)) {
                if (!isAutoPropertySymbol(ps)) {
                    // #878: computed (expression-bodied) property has no
                    // backing storage and is not a valid with target.
                    ctx_.diagnostics().report(
                        Severity::Error, valLoc,
                        "`" + std::string(ia.name) + "` is a computed "
                        "property and cannot be a `with` target — `with` "
                        "requires backing storage (auto-property) (§9.5, #878)");
                    continue;
                }
                // Auto-property: check replacement value type (§5.1, #875).
                if (ia.value) {
                    TypeRef* valTy = ctx_.exprType(*ia.value);
                    if (auto* pd =
                            dynamic_cast<const ast::PropertyDecl*>(ps->decl)) {
                        if (pd->type) {
                            TypeRef* fieldTy = ctx_.resolvedType(*pd->type);
                            checkAssignable(
                                valTy, fieldTy, valLoc,
                                "replacement value for `" +
                                std::string(ia.name) + "`",
                                ia.value.get());
                        }
                    }
                }
                continue;
            }
            if (auto* fs = dynamic_cast<FieldSymbol*>(mem)) {
                // Valid field target: check replacement value type (§5.1, #875).
                if (ia.value) {
                    TypeRef* valTy = ctx_.exprType(*ia.value);
                    TypeRef* fieldTy = nullptr;
                    if (fs->posParam && fs->posParam->type) {
                        fieldTy = ctx_.resolvedType(*fs->posParam->type);
                    } else if (auto* fd =
                                   dynamic_cast<const ast::FieldDecl*>(
                                       fs->decl)) {
                        if (fd->type)
                            fieldTy = ctx_.resolvedType(*fd->type);
                    }
                    if (fieldTy)
                        checkAssignable(
                            valTy, fieldTy, valLoc,
                            "replacement value for `" +
                            std::string(ia.name) + "`",
                            ia.value.get());
                }
                continue;
            }
            // Any other member kind (EnumConstant, NestedType, etc.) is invalid.
            ctx_.diagnostics().report(
                Severity::Error, valLoc,
                "`" + std::string(ia.name) + "` is not a valid `with` "
                "target — `with` requires a positional-field property, "
                "auto-property, or public field (§9.5)");
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
        // §15.4 (#1297): a Dictionary<K,V> / OwningDictionary<K,V> literal (the
        // dict form has a non-empty `keys` vector) with DUPLICATE keys is a
        // compile error when all the duplicates are compile-time constants;
        // only when duplicates are not statically detectable does the last
        // value win at runtime. Detect duplicates among the literal-constant
        // keys (int/string/char/bool) — the common case — and report the second
        // occurrence. Non-constant / not-yet-handled keys are skipped (they fall
        // back to the runtime last-wins path, never a false positive).
        if (!cl->keys.empty()) {
            auto constKeyCanon =
                [](const ast::Expr* k) -> std::optional<std::string> {
                if (auto* il = dynamic_cast<const ast::IntLiteralExpr*>(k)) {
                    std::string s;
                    for (char c : il->text) if (c != '_') s += c;
                    if (s.empty()) return std::nullopt;
                    bool neg = false;
                    if (s[0] == '+' || s[0] == '-') { neg = (s[0]=='-'); s = s.substr(1); }
                    int base = 10;
                    if (s.size() > 2 && s[0] == '0' &&
                        (s[1] == 'x' || s[1] == 'X')) { base = 16; s = s.substr(2); }
                    else if (s.size() > 2 && s[0] == '0' &&
                        (s[1] == 'b' || s[1] == 'B')) { base = 2; s = s.substr(2); }
                    else if (s.size() > 2 && s[0] == '0' &&
                        (s[1] == 'o' || s[1] == 'O')) { base = 8; s = s.substr(2); }
                    try {
                        long long v = std::stoll(s, nullptr, base);
                        return std::string("i:") + std::to_string(neg ? -v : v);
                    } catch (...) { return std::nullopt; }
                }
                if (auto* sl = dynamic_cast<const ast::StringLiteralExpr*>(k))
                    return std::string("s:") + std::string(sl->text);
                if (auto* cl2 = dynamic_cast<const ast::CharLiteralExpr*>(k))
                    return std::string("c:") + std::string(cl2->text);
                if (auto* bl = dynamic_cast<const ast::BoolLiteralExpr*>(k))
                    return std::string("b:") + (bl->value ? "1" : "0");
                return std::nullopt;
            };
            std::unordered_set<std::string> seenKeys;
            for (const auto& k : cl->keys) {
                if (!k) continue;
                auto canon = constKeyCanon(k.get());
                if (!canon) continue;
                if (!seenKeys.insert(*canon).second) {
                    ctx_.diagnostics().report(
                        Severity::Error, k->span.begin,
                        "duplicate key in this dictionary literal \xE2\x80\x94 "
                        "the same compile-time-constant key appears more than "
                        "once, which is a compile error (\xC2\xA7""15.4); remove "
                        "the redundant entry (the earlier value would be dead)");
                }
            }
        }
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
