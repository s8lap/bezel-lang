#include "module/ModuleMetadataBuilder.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ast/Node.hpp"
#include "common/IntLiteral.hpp"
#include "common/OwnershipFacts.hpp"
#include "lexer/Lexer.hpp"
#include "sema/ConstFold.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Module.h"

// Module public-interface metadata builder + emit hook (§2.5 / §2.6, issue #328
// increment 2). See ModuleMetadataBuilder.hpp for the design and the §13
// public-surface filtering rules. This file converts the LIVE sema symbol table
// of the just-compiled module into the by-value Increment-1 schema, then (the
// emit hook) serializes it and embeds it into the LLVM module.

namespace bezel::module {

namespace {

// --- §13 visibility filter --------------------------------------------------

// §13.2: top-level types may be `public` or `internal` (default internal). Only
// `public` types are part of the exported, downstream-importable interface.
bool isExportedType(const sema::TypeSymbol& t) {
    return t.visibility == ast::Visibility::Public;
}

// §13.1 / §13.3: a member belongs to a public type's downstream-visible surface
// when it is `public` (visible to anyone) or `protected` (visible to derived
// classes, including derived classes in other modules — the inheritance
// contract). `private` and `internal` members can never be bound by a
// downstream importer, so they are dropped. Interface members carry no
// visibility keyword (Visibility::None) and are implicitly public (§13.3), so
// they are exported too.
bool isExportedMember(ast::Visibility v, bool inInterface) {
    if (inInterface) return true;  // §13.3 interface members are public
    return v == ast::Visibility::Public || v == ast::Visibility::Protected;
}

// --- enum / wire conversions ------------------------------------------------

MetaVisibility toMetaVisibility(ast::Visibility v, bool inInterface) {
    if (inInterface && v == ast::Visibility::None) return MetaVisibility::Public;
    switch (v) {
        case ast::Visibility::Public:    return MetaVisibility::Public;
        case ast::Visibility::Private:   return MetaVisibility::Private;
        case ast::Visibility::Internal:  return MetaVisibility::Internal;
        case ast::Visibility::Protected: return MetaVisibility::Protected;
        case ast::Visibility::None:      return MetaVisibility::None;
    }
    return MetaVisibility::None;
}

MetaTypeDeclKind toMetaTypeDeclKind(ast::TypeDeclKind k) {
    switch (k) {
        case ast::TypeDeclKind::Class:     return MetaTypeDeclKind::Class;
        case ast::TypeDeclKind::Interface: return MetaTypeDeclKind::Interface;
        case ast::TypeDeclKind::Record:    return MetaTypeDeclKind::Record;
        case ast::TypeDeclKind::Enum:      return MetaTypeDeclKind::Enum;
        // §35 (v0.200, #2044, #2121): a choice in module metadata is recorded
        // as Choice, not as a Class fallback (the previous silent fallback
        // misdrove any cross-module importer that branched on the kind).
        case ast::TypeDeclKind::Choice:    return MetaTypeDeclKind::Choice;
    }
    return MetaTypeDeclKind::Class;
}

// §4 PrimitiveType → the wire PrimitiveKind. Values are independent enums; the
// switch is the contract between sema's enum and the serialization enum.
PrimitiveKind toPrimitiveKind(sema::PrimitiveType p) {
    switch (p) {
        case sema::PrimitiveType::None:    return PrimitiveKind::None;
        case sema::PrimitiveType::Bool:    return PrimitiveKind::Bool;
        case sema::PrimitiveType::Char:    return PrimitiveKind::Char;
        case sema::PrimitiveType::String:  return PrimitiveKind::String;
        case sema::PrimitiveType::Byte:    return PrimitiveKind::Byte;
        case sema::PrimitiveType::SByte:   return PrimitiveKind::SByte;
        case sema::PrimitiveType::Short:   return PrimitiveKind::Short;
        case sema::PrimitiveType::UShort:  return PrimitiveKind::UShort;
        case sema::PrimitiveType::Int:     return PrimitiveKind::Int;
        case sema::PrimitiveType::UInt:    return PrimitiveKind::UInt;
        case sema::PrimitiveType::Long:    return PrimitiveKind::Long;
        case sema::PrimitiveType::ULong:   return PrimitiveKind::ULong;
        case sema::PrimitiveType::Float:   return PrimitiveKind::Float;
        case sema::PrimitiveType::Double:  return PrimitiveKind::Double;
        case sema::PrimitiveType::Decimal: return PrimitiveKind::Decimal;
        case sema::PrimitiveType::Object:  return PrimitiveKind::Object;
    }
    return PrimitiveKind::None;
}

std::vector<std::string> toStrings(const std::vector<std::string_view>& in) {
    std::vector<std::string> out;
    out.reserve(in.size());
    for (auto sv : in) out.emplace_back(sv);
    return out;
}

// --- §3 TypeRef → MetaType --------------------------------------------------

// Convert a resolved TypeRef (sema slice 2) into the by-value MetaType wire
// form. Named types are carried by NAME (the loader resolves them against the
// imported module's exports, §2.5) rather than by symbol pointer, so the schema
// survives across compilations.
MetaType convertType(const sema::TypeRef* ref) {
    MetaType t;
    if (ref == nullptr) {
        // No resolved type (a sema gap on this node). Emit the Error sentinel
        // — the loader treats it as unresolved rather than misreading a field.
        t.kind = TypeKind::Error;
        return t;
    }
    t.nullable = ref->nullable;
    switch (ref->kind) {
        case sema::TypeRef::Kind::Error:
            t.kind = TypeKind::Error;
            return t;
        case sema::TypeRef::Kind::Primitive:
            t.kind = TypeKind::Primitive;
            t.primitive = toPrimitiveKind(ref->primitive);
            return t;
        case sema::TypeRef::Kind::Unit:
            t.kind = TypeKind::Unit;
            return t;
        case sema::TypeRef::Kind::Named:
            t.kind = TypeKind::Named;
            // §14: carry the referent's name (a generic-parameter use resolves
            // to a TypeSymbol named after the parameter, e.g. `T`).
            if (ref->declaration != nullptr) {
                t.namedName = std::string(ref->declaration->name);
            } else if (ref->astNode && ref->astNode->name &&
                       !ref->astNode->name->parts.empty()) {
                // §2.5 / §29.3 (issue #495): builtin types (IEnumerable<T>,
                // Array<T>, Task, etc.) have no user TypeSymbol declaration —
                // their TypeRef has declaration=nullptr and astNode set. Without
                // this branch, namedName stays empty and the loader reconstructs
                // an anonymous Named TypeRef that codegen lowers to i32 (the
                // default for an unrecognised Named type), corrupting any
                // pointer-sized builtin in an imported signature.  Carry the
                // first part of the syntactic name so the loader can identify
                // the type and lowerTypeRef can route it to ptrTy() (or the
                // appropriate value-type for DateTime/Guid/etc.).
                t.namedName = std::string(ref->astNode->name->parts[0]);
            }
            for (const sema::TypeRef* arg : ref->genericArgs)
                t.genericArgs.push_back(convertType(arg));
            return t;
        case sema::TypeRef::Kind::InlineArray:
            // §15: [N]T / [N owned]T.
            //
            // fix #3183 (§15.1 / §2.8.3 rule 4, v0.254): carry N. This arm used
            // to leave `arraySize` empty — "the const-expr evaluator is
            // sema-internal and not threaded to the builder" — recording only
            // the element and ownership. It does not need to be threaded:
            // `TypeResolution` has ALREADY folded every foldable size onto
            // `TypeRef::arraySizeValue` (#734, TypeResolution_Names.cpp), so the
            // number is sitting on the very TypeRef this function is converting.
            //
            // WHAT THE OMISSION COST. `Emitter::lowerTypeRef` falls back to a
            // bare `i32` for a size it cannot see, so an importer laid out an
            // imported `[3]int` field in FOUR bytes where the exporter laid out
            // twelve. Measured under `--sanitize=address` on a real two-module
            // build of `record InlineArr(A: [3]int, N: int)`:
            //
            //   AddressSanitizer: stack-buffer-overflow
            //   WRITE of size 12 at 0x... — 8-byte object at [128, 136)
            //
            // i.e. constructing the imported record overruns its own storage.
            // §2.8.3 rule 4 requires one runtime type identity for a type across
            // the boundary, and a 4-vs-12-byte component is not one.
            //
            // The size is TEXTUAL on the wire by schema (`MetaType::arraySize`
            // is a `std::string`), and this writes the folded DECIMAL rather
            // than the source lexeme deliberately: the lexeme would make the
            // loader re-run a const-expr evaluator over identifiers that mean
            // nothing in the importer's scope (`[Limits.Max]int`), which is the
            // very coupling the original comment declined. A decimal is closed.
            //
            // An UNFOLDABLE size stays uncarried (`arraySizeValue < 0`), so the
            // shape that cannot be lowered keeps failing loudly rather than
            // acquiring a wrong number — `sema::serializationComponentType-
            // IsEmittable` and `lowerTypeRef` both key on exactly this
            // condition.
            //
            // Not a schema change: the field is emitted (`emitString`) and
            // parsed unconditionally today and already folds into
            // `semanticHash` (ModuleMetadata.cpp:114), so emission and the hash
            // fold move together by construction. Populating it DOES change the
            // hash for any module with an inline array — intended, the size is
            // part of the type's identity — so dependants must be rebuilt
            // (#3198 is the missing enforcement of that).
            t.kind = TypeKind::InlineArray;
            t.arrayOwned = ref->arrayOwned;
            if (ref->arraySizeValue >= 0)
                t.arraySize = std::to_string(ref->arraySizeValue);
            t.children.push_back(convertType(ref->arrayElement));
            return t;
        case sema::TypeRef::Kind::Function:
            // §27: (T1, …) -> R. children = params… then return; parallel
            // funcParamMods / funcParamNames per the schema (ModuleMetadata.hpp).
            t.kind = TypeKind::Function;
            for (const auto& fp : ref->funcParams) {
                t.children.push_back(convertType(fp.type));
                t.funcParamMods.emplace_back(fp.modifier);
                t.funcParamNames.emplace_back(fp.name);
            }
            t.children.push_back(convertType(ref->funcReturn));
            t.funcReturnOwned = ref->funcReturnOwned;
            return t;
    }
    t.kind = TypeKind::Error;
    return t;
}

// Resolve and convert an ast::Type via the SemaContext side table (§3 type
// resolution). Null type node → unit/error per the caller's needs; here a null
// node yields Unit (used for an omitted `(): ...` return → `()` §3.1).
MetaType convertAstType(const ast::Type* astType, sema::SemaContext& ctx) {
    if (astType == nullptr) {
        MetaType t;
        t.kind = TypeKind::Unit;  // §3.1: an omitted return type means unit
        return t;
    }
    return convertType(ctx.resolvedType(*astType));
}

// --- §22.5 / §11 / §10 textual values ---------------------------------------

// Best-effort textual rendering of a simple constant expression (literal,
// boolean, null, simple identifier/member-access). Used for §22.5 parameter
// defaults, §11 const values, and §10.2 explicit enum values. Returns "" when
// the expression is not a shape we render here — the schema fields are
// advisory (the loader re-parses them for named/optional-arg reproduction), so
// an empty rendering is a safe fallback, not a correctness gap.
//
// `ctx`, when non-null, is consulted to FOLD `IdentifierExpr` /
// `MemberAccessExpr` const references (`SomeConst`, `C.K`) to their initializer
// value rather than rendering them by NAME — fix #3213 (§22.5 / §2.5, v0.254).
// A name has no importer-side meaning (the same reason `foldImportedConstText`
// declines a name today, and the same argument #2898 made for literal
// arithmetic). The ctx-aware path is a no-op for every call site that has no
// SemaContext in scope (e.g. enum value rendering), so the legacy behavior is
// preserved.
std::string renderConstExpr(const ast::Expr* e,
                            sema::SemaContext* ctx = nullptr);

std::string renderConstExpr(const ast::Expr* e, sema::SemaContext* ctx) {
    if (e == nullptr) return "";
    if (auto* i = dynamic_cast<const ast::IntLiteralExpr*>(e))
        return std::string(i->text);
    if (auto* f = dynamic_cast<const ast::FloatLiteralExpr*>(e))
        return std::string(f->text);
    if (auto* c = dynamic_cast<const ast::CharLiteralExpr*>(e))
        return std::string(c->text);
    if (auto* s = dynamic_cast<const ast::StringLiteralExpr*>(e))
        return std::string(s->text);
    if (auto* b = dynamic_cast<const ast::BoolLiteralExpr*>(e))
        return b->value ? "true" : "false";
    if (dynamic_cast<const ast::NullLiteralExpr*>(e))
        return "null";
    // fix #3213 (§22.5 / §2.5, v0.254): a `:= default(T)` default-value
    // expression crosses the module boundary as the canonical `default(T)`
    // text. The importer recognizes this shape (sema::
    // importedDefaultTextIsMaterializable admits `default(`) and materializes
    // it as `Constant::getNullValue(lowerType(formalParamType))` — exactly the
    // path the local-twin codegen takes (LlvmBackend_Assign.cpp:10867 — the
    // same `getNullValue(lowerType(...))` arm). The T written here is the
    // DECLARED default's type spelling, but the importer does NOT consult it:
    // it materializes from the FORMAL PARAMETER's resolved type
    // (MetaParam::type, which is already on the wire), so a same-named shadow
    // on the importer side cannot silently change the value. The T is
    // nevertheless carried so `importedDefaultTextIsMaterializable` can admit
    // it by prefix without a separate wire bit, and so an error message can
    // quote the declared shape.
    if (auto* de = dynamic_cast<const ast::DefaultExpr*>(e)) {
        std::string typeName;
        if (de->type && de->type->name && !de->type->name->parts.empty())
            typeName = std::string(de->type->name->parts.back());
        else
            typeName = "T";  // §17.11 rejects `default(<unresolved>)`; be robust.
        return "default(" + typeName + ")";
    }
    // fix #3213 (§22.5 / §2.5, v0.254): a `:= SomeConst` / `:= C.K` const
    // reference is FOLDED to its value (the constant's own initializer
    // expression) rather than rendered BY NAME. Folding is recursive —
    // transitive const refs unwrap to a literal — and reuses the literal arms
    // above for the leaf. Only `ConstMemberSymbol` is folded here: a
    // §22.12-compatible const initializer is itself a compile-time constant
    // expression, and the const's `ast::ConstDecl::value` IS that expression,
    // so we have exactly the thing `renderConstExpr` already knows how to
    // render. A name has no importer-side meaning: the same reason
    // `foldImportedConstText` declines a name today, and the same argument
    // #2898 made for literal arithmetic (folded, not rendered).
    //
    // Gated on `ctx` so this arm is a no-op for the legacy call sites that
    // pass nullptr (enum value text, const member `valueText` for a const
    // whose initializer is itself a literal — already covered by the literal
    // arms, so the recursion terminates in one step).
    if (ctx != nullptr) {
        sema::Symbol* sym = ctx->exprSymbol(*e);
        if (auto* cms = dynamic_cast<sema::ConstMemberSymbol*>(sym)) {
            if (auto* cd = dynamic_cast<const ast::ConstDecl*>(cms->decl)) {
                if (cd->value) return renderConstExpr(cd->value.get(), ctx);
            }
        }
    }
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e))
        return std::string(id->name);
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
        std::string base = renderConstExpr(ma->target.get(), ctx);
        if (base.empty()) return "";
        return base + "." + std::string(ma->member);
    }
    // fix #2898 (§17.14 / §2.8.2 clause 2, v0.240): a §17.14 compile-time
    // constant that is not a BARE literal — `:= 100 + 7` — has no arm above, so
    // it rendered "" and every consumer read that as "not a compile-time
    // constant". For an imported generic's instance field that is a FALSE
    // REJECT: #2860's guard is category-blind (it asks whether a value reached
    // the slot), so an empty rendering makes it refuse a construction §2.8.2
    // clause 2 requires to behave exactly as the same-module one does.
    //
    // FOLD, don't render. The alternative — rendering `100 + 7` textually —
    // would widen what `valueText` may CONTAIN, and it is consumed as text by
    // codegen (`constValues_`, ModuleLoader.cpp), so every consumer would have
    // to become an evaluator. Folding keeps `valueText` a literal, the shape
    // they all already assume, so no consumer changes and none can regress.
    //
    // The arithmetic is `sema::foldIntConst`, the ONE §17.14 folder — already
    // shared by type resolution, ExprTypecheck and codegen's enum path — rather
    // than a second evaluator here, which would be free to drift from it.
    //
    // The resolver deliberately answers nullopt for EVERY identifier, so only
    // literal arithmetic folds. That is not a limitation to remove later
    // without thought: `renderConstExpr` above renders a bare `IdentifierExpr`
    // as its NAME, which has no guaranteed importer-side meaning, and resolving
    // names here would let a same-named-but-different const on the importing
    // side silently supply a value. A loud rejection is the correct outcome for
    // anything this cannot fold; #2874 covers carrying genuinely non-constant
    // initializers.
    //
    // Integer-only by construction (foldIntConst's domain): a float-valued
    // constant expression still renders "" and is still rejected, unchanged.
    // Range is deliberately NOT re-checked here — a folded value reaches its
    // slot by exactly the path a bare literal of the same value does, so this
    // makes `:= 100 + 7` behave identically to `:= 107` and introduces no case
    // the literal spelling does not already have (see #2926 for the missing
    // field-initializer assignability check, which governs both spellings).
    if (auto folded = sema::foldIntConst(
            *e, [](std::string_view, int) -> std::optional<long long> {
                return std::nullopt;
            }))
        return std::to_string(*folded);
    return "";
}

// Try to read a plain non-negative integer from an int-literal expression
// (§10.2 explicit enum value). Returns true + value on success.
bool tryEvalIntLiteral(const ast::Expr* e, std::int64_t& out) {
    auto* i = dynamic_cast<const ast::IntLiteralExpr*>(e);
    if (i == nullptr) return false;
    // fix #2936: decode through the ONE literal parser. This was
    // `std::strtoll(text, &end, 0)`, whose base-0 auto-detection covers `0x`
    // but NOT grammar A.8's `0b` binary form and not `_` digit separators — so
    // a §10.2 explicit enum value written `0b101` continued as if it were 0,
    // and `1_000` as if it were 1.
    out = common::parseIntLiteral(i->text);
    return true;
}

// --- §22.16 contract-record carriage (#2054) -------------------------------

struct CanonicalToken {
    Token token;
    std::size_t offset = 0;  // absolute offset in the compilation source
};

std::vector<CanonicalToken> lexCanonicalTokens(std::string_view text,
                                                std::size_t baseOffset) {
    DiagnosticEngine diagnostics;
    Lexer lexer(text, diagnostics);
    std::vector<CanonicalToken> out;
    for (;;) {
        Token tok = lexer.next();
        if (tok.kind == TokenKind::EndOfFile) break;
        out.push_back({tok, baseOffset + tok.location.offset});
    }
    return out;
}

std::string moduleNameFor(sema::SemaContext& ctx) {
    const sema::ModuleSymbol* mod = ctx.module();
    if (!mod) return {};
    if (auto* decl = dynamic_cast<const ast::ModuleDecl*>(mod->decl)) {
        if (decl->name && !decl->name->parts.empty()) {
            std::string out;
            for (std::size_t i = 0; i < decl->name->parts.size(); ++i) {
                if (i) out += '.';
                out += std::string(decl->name->parts[i]);
            }
            return out;
        }
    }
    return std::string(mod->name);
}

bool hasModifier(const std::vector<std::string_view>& modifiers,
                 std::string_view wanted) {
    return std::find(modifiers.begin(), modifiers.end(), wanted) !=
           modifiers.end();
}

std::string qualifiedSymbolName(const sema::Symbol* symbol,
                                std::string_view moduleName) {
    if (!symbol) return {};
    if (auto* member = dynamic_cast<const sema::MemberSymbol*>(symbol)) {
        if (member->parent) {
            std::string out(moduleName);
            if (!out.empty()) out += '.';
            out += std::string(member->parent->name);
            out += '.';
            out += std::string(member->name);
            return out;
        }
    }
    if (auto* type = dynamic_cast<const sema::TypeSymbol*>(symbol)) {
        std::string out(moduleName);
        if (!out.empty()) out += '.';
        out += std::string(type->name);
        return out;
    }
    // Builtins and contextual names have no declaration-site module. Keeping
    // their spelling is deterministic and avoids inventing a stdlib module
    // qualification that the resolver did not provide.
    return std::string(symbol->name);
}

std::string fallbackExprText(const ast::Expr* e) {
    if (!e) return {};
    std::string literal = renderConstExpr(e);
    if (!literal.empty()) return literal;
    if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
        return "(" + fallbackExprText(b->left.get()) + " " +
               std::string(b->op) + " " + fallbackExprText(b->right.get()) +
               ")";
    }
    if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e))
        return std::string(u->op) + " " + fallbackExprText(u->operand.get());
    if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e))
        return fallbackExprText(m->target.get()) + "." + std::string(m->member);
    if (auto* m = dynamic_cast<const ast::NullCondMemberExpr*>(e))
        return fallbackExprText(m->target.get()) + "?." + std::string(m->member);
    if (auto* c = dynamic_cast<const ast::CallExpr*>(e))
        return fallbackExprText(c->callee.get()) + "(...)";
    if (auto* i = dynamic_cast<const ast::IdentifierExpr*>(e))
        return std::string(i->name);
    return "<expression>";
}

std::string canonicalClauseText(const ast::Expr& expr,
                                const sema::SemaContext& ctx,
                                std::string_view source,
                                const std::vector<sema::Symbol*>& params,
                                const std::vector<sema::Symbol*>& genericParams,
                                std::string_view moduleName,
                                const sema::TypeSymbol* ownerType) {
    const std::size_t begin = expr.span.begin.offset;
    const std::size_t end = expr.span.end.offset;
    if (source.empty() || expr.span.begin.line == 0 || end <= begin ||
        end > source.size())
        return fallbackExprText(&expr);

    const std::vector<CanonicalToken> tokens =
        lexCanonicalTokens(source.substr(begin, end - begin), begin);
    if (tokens.empty()) return fallbackExprText(&expr);

    std::unordered_map<std::size_t, std::string> replacements;
    std::unordered_map<std::string, std::size_t> genericByName;
    for (std::size_t i = 0; i < genericParams.size(); ++i) {
        if (genericParams[i])
            genericByName.emplace(std::string(genericParams[i]->name), i);
    }

    std::unordered_map<std::string, std::string> ownerMembers;
    std::unordered_set<const sema::TypeSymbol*> ownerVisited;
    for (auto* type = ownerType;
         type && ownerVisited.insert(type).second;) {
        for (sema::Symbol* symbol : type->members) {
            auto* member = dynamic_cast<const sema::MemberSymbol*>(symbol);
            if (!member) continue;
            std::string qualified = qualifiedSymbolName(member, moduleName);
            if (!qualified.empty()) ownerMembers.emplace(member->name, qualified);
        }
        const sema::TypeSymbol* next = nullptr;
        for (const sema::TypeSymbol* base : type->baseTypes) {
            if (base && base->kind == ast::TypeDeclKind::Class) {
                next = base;
                break;
            }
        }
        type = next;
    }

    for (const auto& [node, symbol] : ctx.allExprSymbols()) {
        if (!node || !symbol || node->span.begin.line == 0) continue;
        const auto* id = dynamic_cast<const ast::IdentifierExpr*>(node);
        if (!id) continue;
        const std::size_t off = node->span.begin.offset;
        if (off < begin || off >= end) continue;
        if (id->name == "this" || id->name == "base") continue;
        if (auto* parameter = dynamic_cast<const sema::ParameterSymbol*>(symbol)) {
            auto it = std::find(params.begin(), params.end(), parameter);
            if (it != params.end()) {
                replacements[off] = "%" +
                    std::to_string(static_cast<std::size_t>(it - params.begin()));
                continue;
            }
        }
        if (auto* generic = dynamic_cast<const sema::GenericParamSymbol*>(symbol)) {
            auto it = genericByName.find(std::string(generic->name));
            if (it != genericByName.end()) {
                replacements[off] = "%" + std::to_string(it->second);
                continue;
            }
        }
        std::string qualified = qualifiedSymbolName(symbol, moduleName);
        if (!qualified.empty()) replacements[off] = std::move(qualified);
    }
    // MemberAccessExpr nodes do not have a separate member-token location in
    // the AST. For the outer nodes (the parser stamps their spans), locate the
    // member token in the exact `.`/`?.` token pair. Bare field references are
    // covered by the IdentifierExpr loop above; this adds declaration-qualified
    // spelling for the common explicit-receiver form without rewriting syntax.
    std::vector<const ast::MemberAccessExpr*> memberNodes;

    for (const auto& [node, symbol] : ctx.allExprSymbols()) {
        if (!node || !symbol) continue;
        auto* member = dynamic_cast<const ast::MemberAccessExpr*>(node);
        if (!member || member->span.begin.line == 0) continue;
        if (member->span.end.offset <= begin || member->span.begin.offset >= end)
            continue;
        memberNodes.push_back(member);
    }
    std::sort(memberNodes.begin(), memberNodes.end(),
              [](const auto* a, const auto* b) {
                  return a->span.begin.offset < b->span.begin.offset;
              });
    for (const auto* member : memberNodes) {
        sema::Symbol* symbol = ctx.exprSymbol(*member);
        if (!symbol) continue;
        std::string qualified = qualifiedSymbolName(symbol, moduleName);
        if (qualified.empty()) continue;
        for (std::size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i].offset < member->span.begin.offset ||
                tokens[i].offset >= member->span.end.offset ||
                tokens[i].token.text != member->member)
                continue;
            if (tokens[i - 1].token.text == "." ||
                tokens[i - 1].token.text == "?.") {
                replacements[tokens[i].offset] = qualified;
                break;
            }
        }
    }

    // For nested postfix nodes the parser intentionally leaves `span` empty
    // (the enclosing expression owns the stamped range). Use the resolved
    // member name as a second, ambiguity-safe lookup so `this._field` and
    // `Math.Abs(x)` receive the same declaration qualification even when they
    // occur below a binary/call node. If two declaring types expose the same
    // member spelling in this clause, leave that spelling unchanged rather than
    // guessing; the source-qualified outer node path remains available above.
    std::unordered_map<std::string, std::string> memberQualified;
    std::unordered_set<std::string> ambiguousMembers;
    auto rememberMember = [&](const ast::Expr* node, sema::Symbol* symbol,
                              std::string_view name) {
        if (!node || !symbol || name.empty()) return;
        if (node->span.begin.line != 0 &&
            (node->span.end.offset <= begin || node->span.begin.offset >= end))
            return;
        std::string qualified = qualifiedSymbolName(symbol, moduleName);
        if (qualified.empty()) return;
        auto [it, inserted] = memberQualified.emplace(std::string(name),
                                                       qualified);
        if (!inserted && it->second != qualified)
            ambiguousMembers.insert(std::string(name));
    };
    for (const auto& [node, symbol] : ctx.allExprSymbols()) {
        if (!node || !symbol) continue;
        if (auto* member = dynamic_cast<const ast::MemberAccessExpr*>(node))
            rememberMember(member, symbol, member->member);
        else if (auto* member =
                     dynamic_cast<const ast::NullCondMemberExpr*>(node))
            rememberMember(member, symbol, member->member);
    }

    std::string out;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (i) out.push_back(' ');
        auto it = replacements.find(tokens[i].offset);
        if (it != replacements.end()) {
            out += it->second;
            continue;
        }
        if (ownerType && tokens[i].token.kind == TokenKind::Identifier) {
            const bool bare = i == 0 ||
                (tokens[i - 1].token.text != "." &&
                 tokens[i - 1].token.text != "?.");
            const bool thisMember = i >= 2 &&
                (tokens[i - 1].token.text == "." ||
                 tokens[i - 1].token.text == "?.") &&
                (tokens[i - 2].token.text == "this" ||
                 tokens[i - 2].token.text == "base");
            if (bare || thisMember) {
                auto ownerIt = ownerMembers.find(
                    std::string(tokens[i].token.text));
                if (ownerIt != ownerMembers.end()) {
                    out += ownerIt->second;
                    continue;
                }
            }
        }
        if (i > 0 && (tokens[i - 1].token.text == "." ||
                      tokens[i - 1].token.text == "?.") &&
            tokens[i].token.kind == TokenKind::Identifier) {
            auto memberIt = memberQualified.find(
                std::string(tokens[i].token.text));
            if (memberIt != memberQualified.end() &&
                !ambiguousMembers.count(std::string(tokens[i].token.text))) {
                out += memberIt->second;
                continue;
            }
        }
        // Type-parameter occurrences inside `typeof`/`new` type syntax may not
        // have an Expr side-table entry. Positionalize those lexical names too,
        // but never a member suffix (`obj.T`) and never a keyword/literal.
        if (tokens[i].token.kind == TokenKind::Identifier &&
            (i == 0 || (tokens[i - 1].token.text != "." &&
                        tokens[i - 1].token.text != "?."))) {
            auto git = genericByName.find(std::string(tokens[i].token.text));
            if (git != genericByName.end()) {
                out += "%" + std::to_string(git->second);
                continue;
            }
        }
        out += std::string(tokens[i].token.text);
    }
    return out;
}

std::optional<std::int64_t> constantIntValue(
    const ast::Expr* e, const sema::SemaContext* ctx, int depth = 0) {
    if (!e || depth > 32) return std::nullopt;
    (void)ctx;
    if (auto* literal = dynamic_cast<const ast::IntLiteralExpr*>(e)) {
        char* end = nullptr;
        std::string text(literal->text);
        const long long value = std::strtoll(text.c_str(), &end, 0);
        if (end == text.c_str()) return std::nullopt;
        return static_cast<std::int64_t>(value);
    }
    if (auto* unary = dynamic_cast<const ast::UnaryExpr*>(e)) {
        auto value = constantIntValue(unary->operand.get(), ctx, depth + 1);
        if (!value) return std::nullopt;
        if (unary->op == "+") return *value;
        if (unary->op == "-") return -*value;
        if (unary->op == "~") return ~*value;
        return std::nullopt;
    }
    auto* binary = dynamic_cast<const ast::BinaryExpr*>(e);
    if (!binary) return std::nullopt;
    auto lhs = constantIntValue(binary->left.get(), ctx, depth + 1);
    auto rhs = constantIntValue(binary->right.get(), ctx, depth + 1);
    if (!lhs || !rhs) return std::nullopt;
    if (binary->op == "+") return *lhs + *rhs;
    if (binary->op == "-") return *lhs - *rhs;
    if (binary->op == "*") return *lhs * *rhs;
    if (binary->op == "/" && *rhs != 0) return *lhs / *rhs;
    if (binary->op == "mod" && *rhs != 0) return *lhs % *rhs;
    if (binary->op == "&") return *lhs & *rhs;
    if (binary->op == "|") return *lhs | *rhs;
    if (binary->op == "^") return *lhs ^ *rhs;
    if (binary->op == "<<") return *lhs << *rhs;
    if (binary->op == ">>") return *lhs >> *rhs;
    return std::nullopt;
}

std::optional<bool> constantBoolValue(const ast::Expr* e,
                                      const sema::SemaContext* ctx,
                                      int depth = 0) {
    if (!e || depth > 32) return std::nullopt;
    if (auto* literal = dynamic_cast<const ast::BoolLiteralExpr*>(e))
        return literal->value;
    if (auto* unary = dynamic_cast<const ast::UnaryExpr*>(e)) {
        if (unary->op == "not") {
            auto value = constantBoolValue(unary->operand.get(), ctx, depth + 1);
            if (value) return !*value;
        }
        return std::nullopt;
    }
    auto* binary = dynamic_cast<const ast::BinaryExpr*>(e);
    if (!binary) return std::nullopt;
    auto lhsBool = constantBoolValue(binary->left.get(), ctx, depth + 1);
    auto rhsBool = constantBoolValue(binary->right.get(), ctx, depth + 1);
    if (binary->op == "and" && lhsBool && rhsBool) return *lhsBool && *rhsBool;
    if (binary->op == "or" && lhsBool && rhsBool) return *lhsBool || *rhsBool;
    if ((binary->op == "=" || binary->op == "<>") && lhsBool && rhsBool)
        return binary->op == "=" ? *lhsBool == *rhsBool
                                   : *lhsBool != *rhsBool;

    auto lhsInt = constantIntValue(binary->left.get(), ctx, depth + 1);
    auto rhsInt = constantIntValue(binary->right.get(), ctx, depth + 1);
    if (!lhsInt || !rhsInt) return std::nullopt;
    if (binary->op == "=") return *lhsInt == *rhsInt;
    if (binary->op == "<>") return *lhsInt != *rhsInt;
    if (binary->op == "<") return *lhsInt < *rhsInt;
    if (binary->op == ">") return *lhsInt > *rhsInt;
    if (binary->op == "<=") return *lhsInt <= *rhsInt;
    if (binary->op == ">=") return *lhsInt >= *rhsInt;
    return std::nullopt;
}

MetaContractTier contractTier(const ast::ContractClause& clause,
                              const sema::SemaContext& ctx) {
    // §22.16: `static` demands proof. The v0.204 prover floor also records a
    // declaration-only constant-true clause as proven; opportunistic call-site
    // discharge is deliberately not consulted here.
    if (clause.isStatic) return MetaContractTier::Proven;
    if (auto value = constantBoolValue(clause.expr.get(), &ctx); value && *value)
        return MetaContractTier::Proven;
    return MetaContractTier::Checked;
}

MetaContractRecord makeContractRecord(const ast::ContractClause& clause,
                                      const sema::SemaContext& ctx,
                                      std::string_view source,
                                      const std::vector<sema::Symbol*>& params,
                                      const std::vector<sema::Symbol*>& generics,
                                      std::string_view moduleName,
                                      const sema::TypeSymbol* ownerType) {
    MetaContractRecord out;
    out.kind = clause.kind == ast::ContractClause::Kind::Requires
                   ? MetaContractKind::Requires
                   : MetaContractKind::Ensures;
    out.isStatic = clause.isStatic;
    out.tier = contractTier(clause, ctx);
    if (clause.expr)
        out.canonicalText = canonicalClauseText(
            *clause.expr, ctx, source, params, generics, moduleName, ownerType);
    return out;
}

std::vector<MetaContractRecord> convertContracts(
    const std::vector<ast::ContractClause>& clauses,
    const std::vector<ast::Param>& astParams,
    const std::vector<sema::Symbol*>& paramSymbols,
    const std::vector<sema::Symbol*>& genericSymbols,
    sema::SemaContext& ctx, std::string_view source,
    std::string_view moduleName, const sema::TypeSymbol* ownerType) {
    std::vector<MetaContractRecord> out;
    // §22.4: `when` is the shipped static-ensures family. It is emitted before
    // authored clauses because its source position is in the parameter list and
    // its desugared declaration precedes the post-signature clause region.
    for (std::size_t i = 0; i < astParams.size(); ++i) {
        const ast::Param& p = astParams[i];
        if (p.outNullPostcond == ast::OutNullWhen::None) continue;
        MetaContractRecord c;
        c.kind = MetaContractKind::Ensures;
        c.isStatic = true;
        c.tier = MetaContractTier::Proven;
        const char* relation =
            p.outNullPostcond == ast::OutNullWhen::WhenTrue ? "not result or "
                                                             : "result or ";
        c.canonicalText = std::string(relation) + "%" + std::to_string(i) +
                          " <> null";
        out.push_back(std::move(c));
    }
    for (const auto& clause : clauses)
        out.push_back(makeContractRecord(clause, ctx, source, paramSymbols,
                                         genericSymbols, moduleName, ownerType));
    return out;
}

MetaContractRecord convertInvariant(const ast::InvariantDecl& inv,
                                    const std::vector<sema::Symbol*>& generics,
                                    sema::SemaContext& ctx,
                                    std::string_view source,
                                    std::string_view moduleName,
                                    const sema::TypeSymbol* ownerType) {
    MetaContractRecord out;
    out.kind = MetaContractKind::Invariant;
    out.isStatic = inv.isStatic;
    out.tier = inv.isStatic ? MetaContractTier::Proven
                            : MetaContractTier::Checked;
    if (inv.expr)
        out.canonicalText = canonicalClauseText(
            *inv.expr, ctx, source, {}, generics, moduleName, ownerType);
    return out;
}

void collectInvariants(const sema::TypeSymbol& type,
                       sema::SemaContext& ctx, std::string_view source,
                       std::string_view moduleName,
                       std::vector<MetaContractRecord>& out,
                       std::unordered_set<const sema::TypeSymbol*>& visiting) {
    if (!visiting.insert(&type).second) return;
    for (const sema::TypeSymbol* base : type.baseTypes) {
        if (base && base->kind == ast::TypeDeclKind::Class)
            collectInvariants(*base, ctx, source, moduleName, out, visiting);
    }
    auto* td = dynamic_cast<const ast::TypeDecl*>(type.decl);
    if (td) {
        for (const auto& member : td->members) {
            auto* inv = dynamic_cast<const ast::InvariantDecl*>(member.get());
            if (!inv) continue;
            out.push_back(convertInvariant(*inv, type.genericParams, ctx, source,
                                           moduleName, &type));
        }
    } else {
        // Imported base classes have no AST in this compilation. Re-emit the
        // loader's side-table records so a local derived class's interface keeps
        // the required base-first invariant carriage.
        for (const auto& in : type.invariantContracts) {
            MetaContractRecord c;
            c.kind = static_cast<MetaContractKind>(in.kind);
            c.isStatic = in.isStatic;
            c.canonicalText = in.canonicalText;
            c.tier = static_cast<MetaContractTier>(in.tier);
            out.push_back(std::move(c));
        }
    }
    visiting.erase(&type);
}

std::vector<MetaContractRecord> typeInvariants(const sema::TypeSymbol& type,
                                               sema::SemaContext& ctx,
                                               std::string_view source,
                                               std::string_view moduleName) {
    std::vector<MetaContractRecord> out;
    std::unordered_set<const sema::TypeSymbol*> visiting;
    collectInvariants(type, ctx, source, moduleName, out, visiting);
    return out;
}

// --- §22 parameters ---------------------------------------------------------

MetaParam convertParam(const sema::ParameterSymbol& p, sema::SemaContext& ctx) {
    MetaParam mp;
    mp.name = std::string(p.name);
    mp.modifier = std::string(p.modifier);  // "", "ref", "out", "owned", ...
    // Prefer a context-supplied resolved type (§24.3 event-handler params),
    // else the resolved type of the syntactic annotation.
    if (p.resolvedDeclType != nullptr)
        mp.type = convertType(p.resolvedDeclType);
    else
        mp.type = convertAstType(p.declType, ctx);
    // §22.4 / §12.1 (#2392): carry the `when true`/`when false` out-
    // nullability postcondition so an importer can reconstruct
    // ParameterSymbol::outNullPostcond for an override-signature comparison
    // against this (possibly base) parameter. The `when` clause is ALSO
    // exported as a contract record (below, canonical text like
    // "not result or %0 <> null") — that is the §2.8.2 contract-record
    // mechanism for §22.16 proving/consumption, a distinct purpose from this
    // per-parameter field, which exists so the override-match check has a
    // structured value to compare rather than needing to parse contract text.
    mp.outNullWhen = static_cast<std::uint8_t>(p.outNullPostcond);
    // §22.5 default value (textual). The ast::Param carries the default
    // expression; the ParameterSymbol's `decl` is not the Param, so we read the
    // default from the owning declaration where available. We only have the
    // type node here, so defaults are filled from the member walk below.
    return mp;
}

std::vector<MetaParam> convertParams(const std::vector<sema::Symbol*>& params,
                                     sema::SemaContext& ctx) {
    std::vector<MetaParam> out;
    out.reserve(params.size());
    for (sema::Symbol* s : params) {
        if (auto* p = dynamic_cast<const sema::ParameterSymbol*>(s))
            out.push_back(convertParam(*p, ctx));
    }
    return out;
}

// Fill §22.5 default-value text on a converted parameter list from the AST
// Param list (the symbol list parallels the AST param list in order). The
// `ctx` is forwarded to `renderConstExpr` so the ctx-aware arms for
// `default(T)` and `:= C.K` const references (#3213, v0.254) can resolve the
// expression's symbol — the legacy sites that do not need resolution pass
// `nullptr` and see the old literal-only behavior.
void fillParamDefaults(std::vector<MetaParam>& out,
                       const std::vector<ast::Param>& astParams,
                       sema::SemaContext* ctx) {
    const std::size_t n = std::min(out.size(), astParams.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (astParams[i].defaultValue) {
            out[i].hasDefault = true;
            out[i].defaultText =
                renderConstExpr(astParams[i].defaultValue.get(), ctx);
        }
    }
}

// --- §14 generic parameters -------------------------------------------------

// Convert a method/type's generic parameters. Variance comes from the symbol;
// constraints (§14.2) come from the declaring AST node's GenericConstraints.
std::vector<MetaGenericParam> convertGenericParams(
    const std::vector<sema::Symbol*>& gps,
    const std::vector<ast::GenericConstraint>& constraints,
    sema::SemaContext& ctx) {
    std::vector<MetaGenericParam> out;
    out.reserve(gps.size());
    for (sema::Symbol* s : gps) {
        auto* gp = dynamic_cast<const sema::GenericParamSymbol*>(s);
        if (gp == nullptr) continue;
        MetaGenericParam mg;
        mg.name = std::string(gp->name);
        mg.variance = std::string(gp->variance);  // "", "in", "out"
        // §14.2: gather constraints whose `where T:` parameter matches.
        for (const ast::GenericConstraint& gc : constraints) {
            if (gc.typeParam != gp->name) continue;
            for (const ast::ConstraintItem& ci : gc.items) {
                if (ci.kind == "type" && ci.typeArg) {
                    mg.constraintTypes.push_back(convertAstType(ci.typeArg.get(),
                                                                ctx));
                } else if (!ci.kind.empty()) {
                    mg.specialConstraints.emplace_back(ci.kind);  // class/value/new/...
                }
            }
            if (!gc.modifierConstraint.empty())
                mg.specialConstraints.emplace_back(gc.modifierConstraint);
        }
        out.push_back(std::move(mg));
    }
    return out;
}

// --- members ----------------------------------------------------------------

// Forward declaration — defined after appendEnumConstants (§11.3, issue #502):
// convertMember needs to call convertTypeDecl for nested types.
MetaTypeDecl convertTypeDecl(const sema::TypeSymbol& t, sema::SemaContext& ctx,
                              std::string_view source);

// Convert a single exported member. Returns false to skip (kind not part of the
// exported surface — e.g. a destructor, which §13.3 notes is not user-callable;
// its public face is Dispose()).
// `isGenericType` — true when the owning type has generic parameters (§14);
// only then are ctor/dtor body texts extracted for Foundation C.
bool convertMember(const sema::MemberSymbol& m, bool inInterface,
                   sema::SemaContext& ctx, MetaMember& out,
                   bool isGenericType, std::string_view source) {
    out.visibility = toMetaVisibility(m.visibility, inInterface);
    out.modifiers = toStrings(m.modifiers);
    out.isPure = hasModifier(m.modifiers, "pure");
    out.name = std::string(m.name);
    // fix #3140 (§9.8, family F2 of umbrella #3152): the member's `serialized`
    // clause, taken from the SYMBOL rather than re-derived from whichever AST
    // node this member happens to have. SymbolCollection already resolved it
    // once, from `ast::Param::serialized` for a §9.1 positional field and
    // `ast::PropertyDecl::serialized` for an auto-property — the only two
    // storage-bearing sites the A.4 grammar admits it on — so reading the symbol
    // is what keeps the exported wire contract and the local emitters answering
    // from one place. (An EnumConstant's rename rides `enumWireName`, family F4,
    // and is filled where the constant is converted.)
    out.serializedSkip     = m.serializedSkip;
    out.serializedOptional = m.serializedOptional;
    out.serializedWireName = std::string(m.serializedWireName);

    if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m.decl)) {
        out.kind = MetaMemberKind::Field;
        out.type = convertAstType(fd->type.get(), ctx);
        // §2.5 / §22.8 (issue #328 FINDING [7]): carry an INSTANCE field's
        // constant-foldable initializer textually so the importer can replay it
        // during cross-module construction (step 2 of §22.8). The single-module
        // path emits the initializer at the `new` site; the importer has no AST,
        // so a foldable initializer crosses the boundary via this text (mirrors
        // the §22.12 const valueText path). renderConstExpr returns "" for a
        // non-foldable initializer (`new Array()`, `DateTime.Now`, an expression
        // referencing other members) — that initializer does NOT cross the
        // boundary and is a documented limitation.
        //
        // Static fields of a NON-generic type are exported as EXTERNAL storage
        // globals (their initializer runs once in the DECLARING module's own
        // compilation, §22.12 rule 1), so they need no textual carriage here —
        // no other compilation ever needs to run that initializer.
        //
        // §2.8.2 clause 2 / §22.12 rule 1 / §22.13 (#2351): a GENERIC type's
        // static field is different — each consumer's compilation independently
        // monomorphizes its OWN copy of the per-instantiation static state
        // (#1513's LinkOnceODR-shared cell), and the exporting module's own
        // compilation never itself instantiates every consumer's type
        // arguments, so it cannot run the initializer on the consumer's behalf.
        // §2.8.2 clause 2 requires "an external instantiation behaves
        // identically to a same-module instantiation of the same generic at
        // the same arguments" — so the initializer EXPRESSION source text must
        // cross the boundary (mirrors ctorBodyText/methodBodyText) for the
        // importer to reconstruct and run it. See fieldInitText's doc comment
        // (ModuleMetadata.hpp) for the wire shape.
        bool isStaticField = false;
        for (auto mod : m.modifiers)
            if (mod == "static") isStaticField = true;
        if (!isStaticField && fd->initializer) {
            out.valueText = renderConstExpr(fd->initializer.get());
        }
        // fix #2860 (§2.8.2 "No restrictions beyond visibility" / §22.8 step 2,
        // v0.240): also carry a GENERIC type's INSTANCE field initializer text.
        // A non-generic public class replays every instance field initializer
        // through its exported `bz$<Type>$init` (#328 FINDING [7]), so the
        // `valueText` fold is only a shortcut there. A GENERIC class exports no
        // such symbol — the importer monomorphizes construction — so an
        // initializer `valueText` cannot render (`new Cell()`, `[1,2,3]`, a
        // reference to another member) reaches the consumer as NOTHING, and the
        // field is silently left at its §7.9 default: a non-nullable `owned`
        // field parked on the null watermark. §2.8.2 requires an implementation
        // "unable to carry some body shape in its interface format" to "reject
        // the export, or the import, with a diagnostic naming the shape"; this
        // text is what lets the IMPORT side name the shape at the construction
        // site (SemaContext::importedInstanceFieldInitText → emitOwnFieldInit)
        // instead of shipping the silent default. It is emitted for EVERY
        // generic instance-field initializer, foldable or not, so the consumer's
        // test is the category-blind "did a value actually reach the slot?"
        // rather than a guess about which initializer shapes fold.
        if (isGenericType && fd->initializer && !source.empty()) {
            const ast::Expr* init = fd->initializer.get();
            if (init->span.begin.offset < init->span.end.offset &&
                init->span.end.offset <= source.size()) {
                out.fieldInitText = std::string(
                    source.substr(init->span.begin.offset,
                                  init->span.end.offset -
                                      init->span.begin.offset));
            }
        }
        return true;
    }
    // §9.1 positional record field — the FieldSymbol's decl is the TypeDecl and
    // the type lives on posParam.
    if (auto* fs = dynamic_cast<const sema::FieldSymbol*>(&m)) {
        if (fs->posParam != nullptr) {
            out.kind = MetaMemberKind::Field;
            out.type = convertAstType(fs->posParam->type.get(), ctx);
            // fix #2516 (§9.2/§9.3/§2.8.3 rule 4): carry the positional vs
            // body distinction across the wire — see isPositional's field
            // comment in ModuleMetadata.hpp.
            out.isPositional = true;
            // fix #3164 (§22.10 "`owned` parameters and capture" / §22.3 /
            // §2.8.3 rule 4, v0.254): a §22.10 primary-constructor parameter's
            // `owned` marker. It rides `ast::Param::modifier` and NOWHERE else
            // — `SymbolCollection::collectTypeDecl` builds the positional
            // FieldSymbol without copying it — so `out.modifiers` (assigned
            // from `m.modifiers` at the top of this function) was empty for
            // every positional field and the marker reached the wire from
            // neither side. An importer therefore saw EVERY imported primary-
            // constructor parameter as a borrow: `CalleeFormals::modifier()`
            // (Ownership_impl.hpp) could only answer "", so
            // `new ImportedC(owned r)` drew the spurious "this parameter
            // position does not take ownership" §22.3 reject while the
            // unmarked `new ImportedC(r)` recorded no transfer at all — the
            // #2659 double-free shape, one module further out.
            //
            // Only `owned` can appear here: §22.10 admits plain and `owned`
            // parameters only, and the parser rejects `ref`/`out`/`out owned`/
            // `scoped` at the primary-ctor parameter list (Parser_Decls.cpp),
            // so a well-formed program never reaches this line with anything
            // else. Filtering to `owned` rather than copying the string
            // verbatim keeps that guarantee local to this line instead of
            // resting on a check three files away, and keeps the set of
            // modifiers an imported positional field can carry ENUMERABLE —
            // see the commit message for the reader-by-reader diff.
            //
            // A RECORD's positional field cannot reach this at all:
            // `parsePositionalFieldList` admits no modifiers (Appendix A.3
            // `PositionalField ::= Identifier ":" Type SerializedClause?
            // EqualityClause?`), which is §28.5's "Records may not have `owned`
            // fields" enforced syntactically. Measured: `record R(owned h: T)`
            // is a PARSE error, so this is a class-primary-constructor-only
            // carriage.
            if (fs->posParam->modifier == "owned")
                out.modifiers.emplace_back("owned");
            // fix #3221 (§22.10 "primary-constructor parameters and capture"
            // / §9.8 / §2.8.3 rule 4, v0.254): a §22.10 primary-ctor
            // parameter that is referenced in a body position (ExprTypecheck
            // sets `fs->posParamBodyRef`) is captured into a synthetic private
            // instance field of the same name (#2325's capture enumeration).
            // That synthetic field IS §22.8 instance storage, so the importer's
            // `isInstanceFieldStorageSymbol` predicate (the FIELD twin of
            // `isAutoPropertyStorageSymbol`, fix #3182) must count it — which
            // asks `fs->posParamBodyRef`. Without this bit the importer
            // answers FALSE on every imported captured parameter, and an
            // imported base whose only storage is a captured primary-ctor
            // parameter slips past #2364's base-with-storage guard exactly
            // the way a plain body field did before #3152/#3182. MEASURED on
            // `226aca1`: a `class Base(v: int); public property V: int => v;
            // end Base;` base imported by a derived `class D: Base,
            // ISerializable, IDeserializable` COMPILED AND RAN (exit 9) while
            // its character-identical single-module twin was rejected with
            // #2364. The carriage mirrors isPositional (#2516, the 17th
            // scalar) — the same "the loader already reconstructs posParam,
            // just bridge the bit" mechanism.
            //
            // Records cannot reach this: their positional fields carry no
            // modifiers AND no body references — `record R(x: int)` declares
            // `x` only in the parameter list, never in a body, so the local
            // `posParamBodyRef` stays false; even if it were ever true,
            // records have no instance fields beyond the positional ones, so
            // the answer is structurally the same. Measured: `record
            // R(v: int)` local compilation emits `posParamBodyRef = false`.
            out.posParamBodyRef = fs->posParamBodyRef;
            // fix #3202 (§22.10 "primary constructors" / §22.5 "Optional
            // parameters and named arguments" / §2.8.3 rule 4, v0.254):
            // carry the positional-parameter's default-value expression
            // textually. `parsePositionalFieldList` admits no default syntax
            // (A.3 `PositionalField ::= Identifier ":" Type SerializedClause?
            // EqualityClause?`), so a RECORD cannot reach this line — §28.5's
            // "records may not have owned fields" and §9.1's no-defaults
            // rule are enforced together at the grammar. Only a §22.10
            // CLASS's primary parameter carries a default. `renderConstExpr`
            // returns "" for a non-foldable default (an arbitrary call,
            // `new T()` against a non-trivial-dispose class, anything that
            // isn't a literal/`default(T)`/const reference per #3213); that
            // empty text is the exact "no default" reading the loader and
            // the codegen default arm both treat as "required", which is
            // the safe direction: a default the exporter could not render
            // is a default the importer cannot materialize, so an under-
            // supplied `new` continues to surface the §22.5 reject rather
            // than passing a wrong-value zero. Same machinery
            // `fillParamDefaults` uses for method parameters; the
            // positional arm simply did not reach it.
            if (fs->posParam && fs->posParam->defaultValue)
                out.posParamDefaultText = renderConstExpr(
                    fs->posParam->defaultValue.get(), &ctx);
            return true;
        }
    }
    if (auto* meth = dynamic_cast<const sema::MethodSymbol*>(&m)) {
        out.kind = MetaMemberKind::Method;
        out.isOperator = meth->isOperator;
        if (auto* md = dynamic_cast<const ast::MethodDecl*>(m.decl)) {
            out.isPure = hasModifier(md->modifiers, "pure");
            out.type = convertAstType(md->returnType.get(), ctx);
            out.returnOwned = md->returnOwned;
            out.genericParams = convertGenericParams(
                meth->genericParams, md->genericConstraints, ctx);
            out.params = convertParams(meth->params, ctx);
            fillParamDefaults(out.params, md->params, &ctx);
            std::vector<sema::Symbol*> contractGenerics;
            if (meth->parent) {
                contractGenerics.insert(contractGenerics.end(),
                                        meth->parent->genericParams.begin(),
                                        meth->parent->genericParams.end());
            }
            contractGenerics.insert(contractGenerics.end(),
                                    meth->genericParams.begin(),
                                    meth->genericParams.end());
            out.contracts = convertContracts(
                md->contractClauses, md->params, meth->params,
                contractGenerics, ctx, source, moduleNameFor(ctx),
                meth->parent);
            // §14.1 / #783: for a GENERIC class, export the full method
            // declaration source text (extracted via the AST span stamped by
            // parseMethodDeclBody) so the importer can re-parse and monomorphize
            // it per instantiation with genericSubst_ active — the same
            // mechanism ctorBodyText uses for constructors. Only block/expression
            // bodies have a span (abstract methods have no body to inline; an
            // operator goes down the OperatorDecl arm). Skip when the span is
            // absent/degenerate (the importer then keeps the §14.1/#783 gate).
            if (isGenericType && !source.empty() &&
                md->span.begin.offset < md->span.end.offset &&
                md->span.end.offset <= source.size()) {
                out.methodBodyText = std::string(
                    source.substr(md->span.begin.offset,
                                  md->span.end.offset - md->span.begin.offset));
            }
        } else if (auto* od = dynamic_cast<const ast::OperatorDecl*>(m.decl)) {
            // §25 operator overload.
            out.isPure = od->isPure;
            out.type = convertAstType(od->returnType.get(), ctx);
            out.returnOwned = od->returnOwned;
            out.params = convertParams(meth->params, ctx);
            fillParamDefaults(out.params, od->params, &ctx);
            // fix #3226 (§25 / §14.1, v0.254): for a GENERIC record/class with
            // an operator whose RETURN TYPE IS A RECORD (the umbrella #3152 F6
            // residue), the importer must monomorphize the body per instantiation
            // — the exporter compiled it once against the i32-PLACEHOLDER layout,
            // so calling it with `T = string` (a different record layout, `%{ptr,
            // ptr}` vs `%{i32, i32}`) reaches codegen with mismatched arg/return
            // widths and the IR verifier aborts ("Call parameter type does not
            // match function signature"). Mirror the MethodDecl branch's
            // methodBodyText export verbatim: stamp `od->span` is in place since
            // #2418 (OperatorDecl's span is set by parseOperatorDeclBody). The
            // importer parses this text inside its synthetic single-type context
            // (parseAndAttachOperatorBody in ModuleLoader.cpp) and attaches the
            // re-parsed ast::OperatorDecl to the imported MethodSymbol, which
            // lets the codegen HARD route's #1059 emitInlineGenericMethod
            // pathway (it already accepts OperatorDecl via callableInfo) emit a
            // per-instantiation specialization. The primitive-return case (the
            // #3159 regression pin, where `T = int` happens to equal the i32
            // placeholder and the call is therefore always type-correct) needs
            // nothing here — the failure mode is exclusively the record-return
            // shape with a non-placeholder concrete type argument.
            if (isGenericType && !source.empty() &&
                od->span.begin.offset < od->span.end.offset &&
                od->span.end.offset <= source.size()) {
                out.methodBodyText = std::string(
                    source.substr(od->span.begin.offset,
                                  od->span.end.offset - od->span.begin.offset));
            }
        }
        return true;
    }
    if (auto* ctor = dynamic_cast<const sema::ConstructorSymbol*>(&m)) {
        out.kind = MetaMemberKind::Constructor;
        out.params = convertParams(ctor->params, ctx);
        if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(m.decl)) {
            out.isPure = cd->isPure;
            fillParamDefaults(out.params, cd->params, &ctx);
            std::vector<sema::Symbol*> contractGenerics;
            if (ctor->parent) {
                contractGenerics.insert(contractGenerics.end(),
                                        ctor->parent->genericParams.begin(),
                                        ctor->parent->genericParams.end());
            }
            out.contracts = convertContracts(
                cd->contractClauses, cd->params, ctor->params,
                contractGenerics, ctx, source, moduleNameFor(ctx),
                ctor->parent);
            // §14 / §22.7 / #2525: export record constructor declaration
            // text as well as generic-class bodies. Imported record construction
            // is lowered inline by value, so the consumer needs the body to
            // execute an explicit constructor (the signature alone is not
            // sufficient to recover its field stores).
            if ((isGenericType ||
                 (ctor->parent && ctor->parent->kind == ast::TypeDeclKind::Record)) &&
                !source.empty() &&
                cd->span.begin.offset < cd->span.end.offset &&
                cd->span.end.offset <= source.size()) {
                out.ctorBodyText = std::string(
                    source.substr(cd->span.begin.offset,
                                  cd->span.end.offset - cd->span.begin.offset));
            }
        }
        return true;
    }
    if (auto* prop = dynamic_cast<const sema::PropertySymbol*>(&m)) {
        out.kind =
            prop->isIndexer ? MetaMemberKind::Indexer : MetaMemberKind::Property;
        if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m.decl)) {
            out.type = convertAstType(pd->type.get(), ctx);
            if (prop->isIndexer) {
                out.params = convertParams(prop->indexerParams, ctx);
                fillParamDefaults(out.params, pd->indexerParams, &ctx);
            }
            // fix #2467 (§9.2/§2.5, v0.235): carry an instance AUTO-PROPERTY's
            // constant-foldable initializer (`property X: T := v;`) textually,
            // mirroring the FieldDecl branch above exactly (same renderConstExpr
            // / §22.8 FINDING [7] wire precedent — `valueText` already "rides
            // every member", ModuleMetadata.hpp v2/v3 notes, so this populates
            // an existing always-serialized-but-previously-always-empty field
            // for Property, no schema bump). The FieldDecl branch above only
            // ever matches `m.decl` as an `ast::FieldDecl` — an auto-property's
            // `PropertyDecl` never takes that branch — so an auto-property's
            // own default value was NOT carried at all before this fix, a
            // strictly separate gap from the Field case (which already
            // worked). This is the root cause of #2467's confirmed repro (a
            // fully public `record BodOnly { public property W: int := 42; }`
            // reading `0` after import): §9.2's record construction
            // (`buildRecordValue`, LlvmBackend_Exprs2.cpp) reads a body
            // member's initializer off `m->decl`, which is null for every
            // imported symbol (no AST crosses the boundary) — with no wire
            // carriage at all, the importer had nothing to fall back to. See
            // buildRecordValue's decl-null fallback (this issue) for the
            // consuming side.
            //
            // Scoped identically to the Field case: only a NON-STATIC
            // auto-property's foldable initializer crosses (a static
            // auto-property is admitted by §22.12 rule 2 but follows the
            // external-storage-global story like a static field — no
            // textual carriage needed here). Applies to BOTH classes and
            // records — for classes this rides alongside the FieldDecl
            // carriage as inert, unconsumed data (classes replay field/auto-
            // property initializers via the exported `bz$<Type>$init`
            // function, #2452, which needs no textual carriage at all,
            // exactly as the Field branch's own comment already notes for
            // the class case) — only `buildRecordValue`'s new fallback
            // (this issue) reads it.
            if (pd->bodyKind == ast::PropertyBodyKind::Auto &&
                pd->autoInitializer) {
                bool isStaticProp = false;
                for (auto mod : m.modifiers)
                    if (mod == "static") { isStaticProp = true; break; }
                if (!isStaticProp)
                    out.valueText = renderConstExpr(pd->autoInitializer.get());
            }
            // fix #2478 (§9.2/§22.8, v0.235): carry whether this Property is
            // declared in AUTO form (a real instance-storage member, §9.2) so
            // the importer's `collectInstanceStorage`/`collectRecordValue`
            // storage-layout walk can classify it correctly — mirroring
            // `sema::hasAstAutoPropertyDecl`'s own predicate (Auto bodyKind,
            // non-static) so the wire-carried classification and the local-decl
            // classification agree by construction. See `out.isAutoProperty`'s
            // field comment in ModuleMetadata.hpp for the full rationale and
            // the confirmed live memory-safety hazard this closes.
            //
            // fix #2523 (§12.1/§12.5, v0.236, sibling of #2478): the
            // `!prop->isIndexer` exclusion the original #2478 carried (because
            // an indexer is never *storage*) is dropped — the bit is now set
            // for Auto-body INDEXERS as well, so the importer-side
            // `ctx_.importedPropertyIsAutoStorage(base)` correctly reports
            // "is in §12.5 shorthand form" for the abstract-indexer override
            // check (`checkPropertyOverrideSignatureMatch`,
            // TypeResolution_Checks.cpp ~2680). Storage consumers are
            // UNAFFECTED — `sema/Symbol.hpp`'s `isAutoPropertyStorageSymbol`
            // helper still filters `ps->isIndexer` explicitly
            // (`isAutoPropertyStorageSymbol`, sema/Symbol.hpp)
            // before consulting this side table, so an imported Auto-body
            // indexer never appears in `collectInstanceStorage`/
            // `collectRecordStorage`. The `!prop->isIndexer` exclusion was
            // only here because #2478 thought of the bit in pure-storage
            // terms; the #2523 reuse re-derives the same Auto-bodyKind/non-
            // static predicate at a SEMANTIC level (§12.5 abstract shorthand
            // = get-only, regardless of storage). `sema::hasAstAutoPropertyDecl`
            // itself is deliberately LEFT UNCHANGED (still excludes indexers)
            // — its many call sites (definite assignment, record equality,
            // ARC) treat the predicate as "real storage member", which an
            // indexer isn't; only this wire-bit repurposes it.
            out.isAutoProperty = pd->bodyKind == ast::PropertyBodyKind::Auto;
            for (auto mod : m.modifiers)
                if (mod == "static") { out.isAutoProperty = false; break; }
            // fix #2939 (§12.5/§23.1/§9.2, v0.240): also exclude `abstract`.
            // An `abstract` simple-form property (`public abstract property
            // X: T;`) records `bodyKind == Auto` but has no body by §12.5,
            // verbatim — "`abstract` on a member declares it without a
            // body; every concrete subclass must override it" — so it has
            // no backing field to back. Without this exclusion the wire bit
            // answers `true` for an abstract simple-form property, the
            // importer's `importedPropertyIsAutoStorage` follows, and
            // `isAutoPropertyStorageSymbol`'s decl-null fallback hands that
            // `true` back to `collectInstanceStorage`/`storageMemberOf` —
            // same wasted-slot shape as the local predicate, just cross
            // module. Mirrors the matching exclusion added to
            // `sema::hasAstAutoPropertyDecl` so the wire classification
            // and the local-decl classification agree on the abstract case
            // by construction (the same agreement #2478's body already
            // guarantees for non-abstract members, and #2523's comment
            // already names as the predicate's invariant).
            for (auto mod : m.modifiers)
                if (mod == "abstract") { out.isAutoProperty = false; break; }
            // fix #2400 (§23/§2.8.2/§2.8.3 rule 4): mirror
            // LlvmBackend_impl.hpp's propertyHasSetter exactly. That codegen
            // helper only ever operates on a LOCAL decl (the same `pd`
            // available here, in the same translation unit as the export),
            // so its logic is duplicated at export time rather than shared
            // across the module/codegen boundary. See hasSetter's field
            // comment in ModuleMetadata.hpp for why an importer cannot
            // recover this from `modifiers` alone.
            if (pd->bodyKind == ast::PropertyBodyKind::Expression) {
                out.hasSetter = false;
            } else if (pd->bodyKind == ast::PropertyBodyKind::Auto) {
                out.hasSetter = true;
                for (auto mod : pd->modifiers)
                    if (mod == "readonly") { out.hasSetter = false; break; }
                // fix #2939 follow-on wave-1-regression (§12.5/§23.1/§9.2,
                // v0.240): mirror `propertyDeclHasSetter`'s abstract
                // exclusion. An abstract Auto-bodied property
                // (`public abstract property X: T;`) is implicitly get-only
                // by §12.5 — "abstract on a member declares it without a
                // body" — so it has no setter. Without this exclusion the
                // wire bit answers `true` and the imported-base half of
                // `checkPropertyOverrideSignatureMatch` (the
                // `TypeResolution_Checks.cpp` override-match check at the
                // imported branch) spuriously demanded a setter on the
                // derived side, falsely rejecting the canonical
                // `abstract property X: T;` + `public override property
                // X: T => 7;` idiom across the module boundary. The
                // correction stanza that lived at the override check
                // (the `baseAbstract && isAutoShorthand` correction) used
                // `isAutoProperty` (now correctly false for abstract after
                // #2939) as its proxy; the wire bit itself is the single
                // source of truth, so the correction becomes a no-op.
                for (auto mod : pd->modifiers)
                    if (mod == "abstract") { out.hasSetter = false; break; }
            } else {
                out.hasSetter = false;
                for (const auto& a : pd->accessors)
                    if (a.isSet) { out.hasSetter = true; break; }
            }
            // fix #2461 (§22.3/§23, v0.236): mirror
            // ExprTypecheck_Calls.cpp's propertyDeclIsSetOnly predicate's
            // INVERSE exactly — that LOCAL read-of-write-only gate returns
            // true (write-only) iff Block bodyKind with a `set` arm but no
            // `get` arm, so hasGetter is its negation on the getter axis:
            // Expression/Auto bodyKinds always have a getter (the former's
            // body IS the getter; the latter is backed by compiler storage
            // with a synthesized get), and a Block bodyKind has a getter iff
            // any accessor is a `get` arm. Computed here (export time) rather
            // than shared across the module/codegen boundary because, like
            // hasSetter above, the importer never reconstructs an
            // ast::PropertyDecl to derive this from. Applied to BOTH
            // properties and indexers (an indexer may also be write-only — a
            // `set`-only indexer block — and §22.3's copy-in/getter rule
            // applies to an indexer slot ref/out target identically, per
            // checkRefOutPropertyOperand's IndexExpr path).
            if (pd->bodyKind == ast::PropertyBodyKind::Expression) {
                out.hasGetter = true;
            } else if (pd->bodyKind == ast::PropertyBodyKind::Auto) {
                out.hasGetter = true;
            } else {
                out.hasGetter = false;
                for (const auto& a : pd->accessors)
                    if (!a.isSet) { out.hasGetter = true; break; }
            }
            // fix #2415 (§13.6/§23.3/§2.8.2 clause 1/§2.8.3 rule 4): mirror
            // ExprTypecheck.cpp's local `:=`-assignment-target `sv`
            // computation exactly (and checkRefOutPropertyOperand's ref/out
            // mirror of the same logic) — an accessor-level visibility
            // override (`pd->accessors[i].visibility` for a block property,
            // `pd->autoAccessorVis` for the `private set;` auto-property
            // shorthand, §13.6) wins when present; otherwise the setter
            // inherits the property's own declared visibility. Computed
            // here (export time) rather than shared across the
            // module/codegen boundary because, like hasSetter above, the
            // importer never reconstructs an ast::PropertyDecl to read this
            // from. Only meaningful when out.hasSetter is true; harmless
            // (ignored on read) otherwise.
            ast::Visibility sv = pd->visibility;
            if (pd->bodyKind == ast::PropertyBodyKind::Block) {
                for (const auto& a : pd->accessors)
                    if (a.isSet && a.visibility != ast::Visibility::None) {
                        sv = a.visibility;
                        break;
                    }
            } else if (pd->bodyKind == ast::PropertyBodyKind::Auto &&
                       pd->hasAutoAccessorVis && pd->autoAccessorIsSet) {
                sv = pd->autoAccessorVis;
            }
            out.setterVisibility = toMetaVisibility(sv, inInterface);
            // §14.1 / §23 / §23.1 / §23.3 / §23.4 / §2.5 / §2.8.2 /
            // §2.8.3 rule 4 (fix #2893, v0.240): export the FULL
            // PropertyDecl source text (from the visibility/modifier
            // prefix through the terminating `;` — `pd->span` is stamped
            // since #2418's parser-side change) for a Property or Indexer
            // member of a GENERIC class. Mirrors ctorBodyText /
            // methodBodyText exactly: the importer re-parses it in a
            // fresh SemaContext (ModuleLoader.cpp parseAndAttachPropertyBody,
            // the property/indexer twin of parseAndAttachCtorBody /
            // -MethodBody), attaches the resulting ast::PropertyDecl to
            // the imported PropertySymbol, and the existing
            // computedGetterOf / blockGetterOf / computedSetterOf /
            // lowerPropertyCompound call-site code falls through to the
            // inline-body path on `pd != null`, with the active
            // `genericSubst_` lowering `pd->type` / `pd->indexerParams`
            // to the substituted layout (T → %record.Pt, not T → i32).
            // This is the cross-module equivalent of the local
            // generic-property lowering that #2884's own T8 control
            // pinned (the same generic class, used inside its OWN
            // module, is fully correct in every spelling — proving the
            // codegen path is right; the only gap is the body-text
            // carriage this fix adds).
            //
            // SCOPE — only an Expression-bodied or Block-bodied
            // property/indexer of a GENERIC class carries a body worth
            // re-parsing:
            //   * Auto body (`property X: T := v;` / `property X: T;`)
            //     — no user body to inline; the storage layout is the
            //     existing `propIsAutoStorage_` /
            //     `isAutoPropertyStorageSymbol` path (#2478). The
            //     exporter exports `isAutoProperty=true` instead.
            //   * abstract (`public abstract property X: T;`) — no body
            //     by §12.5; the §12.5 abstract-shorthand path is the
            //     `isAutoProperty=true` flag above (#2939).
            //   * non-generic class — uses the external
            //     `bz$<Class>$<Prop>$propget` ABI; no per-instantiation
            //     body needed.
            // Same predicate the ctor/method carriages use. The
            // `isGenericType` gate (the parent's `genericParams` is
            // non-empty) is the only predicate; the per-class shape
            // guards above are folded into the bodyKind check. The
            // span gate (`begin.offset < end.offset && end.offset <=
            // source.size()`) matches the ctor/method arms — a
            // degenerate span (zero-width or out of range) is silently
            // skipped, leaving `propertyBodyText` empty (the safe
            // pre-fix state — the §14.1/#783 sema gate for methods
            // stays in force).
            //
            // fix #2893 (v0.240): this block MUST live INSIDE the
            // `if (auto* pd = ...)` arm above so `pd` is in scope
            // (the original commit `5116fc0` placed it outside the
            // `if (pd)` close, and the salvage WIP for the parallel
            // #3348 scaffolding re-applied that placement — both
            // failed to compile with "use of undeclared identifier
            // 'pd'" at the `pd->span` access).
            if (isGenericType &&
                (pd->bodyKind == ast::PropertyBodyKind::Expression ||
                 pd->bodyKind == ast::PropertyBodyKind::Block) &&
                !source.empty() &&
                pd->span.begin.offset < pd->span.end.offset &&
                pd->span.end.offset <= source.size()) {
                out.propertyBodyText = std::string(
                    source.substr(pd->span.begin.offset,
                                  pd->span.end.offset - pd->span.begin.offset));
            }
        }
        return true;
    }
    if (dynamic_cast<const sema::EventSymbol*>(&m)) {
        out.kind = MetaMemberKind::Event;
        if (auto* ed = dynamic_cast<const ast::EventDecl*>(m.decl)) {
            out.type = convertAstType(ed->type.get(), ctx);  // §24 payload type
            // fix #2421 (§24/§2.8.2): carry the two ast::EventDecl layout
            // classification bits across the boundary so the importer's
            // collectInstanceStorage/emitOwnFieldInit/ctor-cleanup (isStatic)
            // and ifaceEventIndex_ mirror + itab$C$I build (isAsync) classify
            // an imported event IDENTICALLY to the exporter — the only way to
            // avoid an itable/struct-layout mismatch (see MetaMember::
            // isEventStatic/isEventAsync's field comment).
            out.isEventStatic = ed->isStatic;
            out.isEventAsync = ed->isAsync;
        }
        return true;
    }
    if (dynamic_cast<const sema::ConstMemberSymbol*>(&m)) {
        out.kind = MetaMemberKind::Const;
        if (auto* cd = dynamic_cast<const ast::ConstDecl*>(m.decl)) {
            out.type = convertAstType(cd->type.get(), ctx);
            // §22.12: a class-level `const` initializer "must be a compile-time-
            // constant expression", so its value is carried textually in the
            // metadata and FOLDED at the importer's read site (issue #328
            // FINDING [3] BUG A) — the library emits NO storage global for a
            // const, so a cross-module read cannot bind to one.
            out.valueText = renderConstExpr(cd->value.get());
        }
        return true;
    }
    // Destructor (§13.3: not part of the public surface — Dispose() is): skip.
    if (dynamic_cast<const sema::DestructorSymbol*>(&m)) return false;
    // §11.3 (schema v4, issue #502): emit public nested types so that a
    // downstream importer can reconstruct a NestedTypeSymbol and resolve
    // `new Outer.Inner()` / `Outer.Inner.Method()` across module boundaries.
    if (auto* nts = dynamic_cast<const sema::NestedTypeSymbol*>(&m)) {
        if (!nts->type) return false;  // degenerate nested type (parse error)
        out.kind = MetaMemberKind::NestedType;
        // The NestedTypeSymbol wrapper's name/visibility/modifiers are already
        // set above (out.name = mm.name, out.visibility = ..., out.modifiers = ...).
        // Emit the inner TypeSymbol's full declaration as `nestedType`.
        out.nestedType = std::make_unique<MetaTypeDecl>(
            convertTypeDecl(*nts->type, ctx, source));
        return true;
    }
    return false;
}

// --- §10 enum constants -----------------------------------------------------

// Append the enum constants of `td` to `out.members` with their underlying
// values per §10.1 (implicit from 0) / §10.2 (explicit + continuation).
// §35.1 / §35.3 / §35.4 (fix #3144, family F9 of umbrella #3152): export the
// choice's CASES. The walk is SYMBOL-side (`t.members` → `ChoiceCaseSymbol`),
// not `td->choiceCases`, so the exporter and every consumer read one list in
// one order — the same order `choiceCasesOf` gives codegen, which is the tag
// order. The per-case payload comes off `cc->caseDecl` (never null on this
// side: SymbolCollection points it at the real `ast::ChoiceCase`).
//
// Before this, `MetaTypeDeclKind::Choice` crossed and the cases did not, so an
// importer laid the choice out as `{ i32 }` (tag only) against the exporter's
// full `{ i32, payload0, payload1, … }` — a silent ABI mismatch measured as a
// SEGV inside the dependency's own compiled function. See the field comment on
// `MetaTypeDecl::choiceCases`.
void appendChoiceCases(const sema::TypeSymbol& t, sema::SemaContext& ctx,
                       MetaTypeDecl& out) {
    for (const sema::Symbol* m : t.members) {
        const auto* cc = dynamic_cast<const sema::ChoiceCaseSymbol*>(m);
        if (!cc || !cc->caseDecl) continue;
        MetaChoiceCase mc;
        mc.name = std::string(cc->name);
        // §A.3: the `serialized` clause on a case admits the RENAME form only.
        if (cc->caseDecl->serialized) {
            mc.hasSerialized = true;
            mc.serializedName = std::string(cc->caseDecl->serialized->name);
        }
        for (const ast::Param& pf : cc->caseDecl->payload) {
            MetaChoicePayloadField mp;
            mp.name = std::string(pf.name);
            mp.modifier = std::string(pf.modifier);  // "" or "owned" (§28.1)
            mp.type = convertAstType(pf.type.get(), ctx);
            if (pf.serialized) {
                mp.hasSerialized = true;
                mp.serializedSkip = pf.serialized->isSkip;
                mp.serializedOptional = pf.serialized->isOptional;
                mp.serializedName = std::string(pf.serialized->name);
            }
            mc.payload.push_back(std::move(mp));
        }
        out.choiceCases.push_back(std::move(mc));
    }
}

void appendEnumConstants(const ast::TypeDecl& td, MetaTypeDecl& out) {
    std::int64_t next = 0;  // §10.1: declaration order from zero
    for (const ast::EnumConstant& ec : td.enumConstants) {
        MetaMember m;
        m.kind = MetaMemberKind::EnumConstant;
        m.name = std::string(ec.name);
        m.visibility = MetaVisibility::Public;  // §10 constants are public
        std::int64_t value = 0;
        bool known = false;
        if (ec.value) {
            // §10.2 explicit value (only plain integer literals are evaluated
            // here; a non-literal const expression leaves the value unknown,
            // and the continuation counter resets to "unknown+1" — i.e. we stop
            // claiming to know subsequent implicit values).
            if (tryEvalIntLiteral(ec.value.get(), value)) {
                known = true;
                next = value + 1;
            } else {
                known = false;
            }
        } else {
            value = next;       // §10.1 / §10.2 continuation
            known = true;
            next = value + 1;
        }
        m.hasEnumValue = known;
        m.enumValue = value;
        // fix #3140 (§9.8, family F4 of umbrella #3152): the constant's
        // `serialized "<name>"` rename — the string §9.8 puts on the wire. The
        // VALUE has crossed since #328; the wire NAME had no channel at all, so
        // `emitEnumSerialize` read it off the `ast::EnumConstant` and an
        // imported enum (which has no AST) serialized to nothing.
        if (ec.serialized && !ec.serialized->name.empty())
            m.enumWireName = std::string(ec.serialized->name);
        out.members.push_back(std::move(m));
    }
}

// --- types ------------------------------------------------------------------

MetaTypeDecl convertTypeDecl(const sema::TypeSymbol& t, sema::SemaContext& ctx,
                              std::string_view source) {
    MetaTypeDecl out;
    out.name = std::string(t.name);
    out.kind = toMetaTypeDeclKind(t.kind);
    out.visibility = toMetaVisibility(t.visibility, /*inInterface=*/false);
    out.modifiers = toStrings(t.modifiers);
    // §9.2 (schema v4, issue #501): carry the positional-record flag so an
    // importer can reconstruct TypeSymbol::hasPositionalFields and pass the
    // §9.2 auto-equality check without an ast::TypeDecl.
    out.hasPositionalFields = t.hasPositionalFields;

    const bool inInterface = (t.kind == ast::TypeDeclKind::Interface);
    // fix #2871 (§2.8.2 clause 2 / §22.12 rule 1 / §22.13 / §9, v0.240): this
    // predicate was `kind == Class`, which names a CATEGORY where the thing that
    // actually matters is a PROPERTY — "this type is generic, so each consumer
    // monomorphizes its OWN copy of its state and bodies, and the exporting
    // module's compilation cannot run them on the consumer's behalf". That is
    // true of a generic RECORD (§9) exactly as it is of a generic class, and a
    // record fell out of every gate below.
    //
    // Consequence before this fix, MEASURED as a real two-module build: a
    // `public static readonly Seed: int := 107;` on an imported generic RECORD
    // read **0** in the importer with NO diagnostic at any severity, where the
    // identical declarations compiled as ONE module read 107. It was silent
    // rather than rejected because #2351's reject fires on the reconstruction
    // path this flag also gates — so a record reached neither the carriage nor
    // the rejection. The generic-class twin has been a hard located reject since
    // #2351; only the record spelling was silently wrong.
    const bool isGenericMonomorphized =
        !t.genericParams.empty() &&
        (t.kind == ast::TypeDeclKind::Class ||
         t.kind == ast::TypeDeclKind::Record);

    // §14 generic params + their §14.2 constraints (from the TypeDecl AST).
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(t.decl)) {
        out.genericParams =
            convertGenericParams(t.genericParams, td->genericConstraints, ctx);
    }

    // §12 base class + interfaces (resolved TypeRefs, parallel to baseTypes).
    for (sema::TypeRef* baseRef : t.baseTypeRefs)
        out.baseTypes.push_back(convertType(baseRef));

    // fix #2950 (§12 / §31.4 / §2.8.2, v0.240): the BUILT-IN exception base,
    // which the loop above structurally cannot carry — a built-in exception has
    // no user TypeSymbol, so it is never in `baseTypeRefs` in the first place.
    // It decides the class's LAYOUT (the §31.4 header), so an importer that
    // cannot see it lays the class out 32 bytes shorter than this exporter did
    // and then calls this exporter's own compiled constructor on the short
    // block. Only the DIRECT base is carried; an ancestor chain reconstructs
    // itself, because each exported ancestor carries its own.
    out.builtinExceptionBase = t.builtinExceptionBase;

    // fix #3191 (§3.6 / §28.9 / §21.9 / §2.8.2, family M2 of umbrella #3152):
    // the §3.6 TRIVIAL-DISPOSE verdict, which the member list structurally
    // cannot carry. `convertMember` skips the destructor outright (§13.3: not
    // part of the public surface — `Dispose()` is) and there is no
    // `MetaMemberKind::Destructor`, so the importer's structural walk found no
    // `DestructorSymbol` on ANY non-generic imported class and classified every
    // one of them trivial-dispose.
    //
    // §2.8.2 requires this: the artifact interface carries "the **derived type
    // properties** (§3.6): carried as computed verdicts for non-generic public
    // types", because they are "computed from structure the public surface does
    // not show — private fields, destructor presence", and omitting them "would
    // force the consumer to either reject imported types at those rules or
    // guess."
    //
    // The exporter is the side that CAN see it: here `t.members` still holds the
    // `DestructorSymbol`, so the shared predicate gives the true verdict. Only
    // the class kinds have one — the predicate's walk is over class members and
    // base classes — and it is a pure read of `t`, which is why the parameter is
    // `const` (see OwnershipFacts.hpp).
    out.nonTrivialDispose = common::isNonTrivialDisposeClass(&t);

    // fix #3139 (§9.8 / §18.5 / §21.4 / §2.8.2, family F1 of #3152): the
    // BUILT-IN INTERFACE bases, which the `baseTypeRefs` loop above cannot
    // carry for exactly the same structural reason — `ISerializable`,
    // `IDeserializable`, `IFormattable` and `IEnumerable<T>` have no user
    // TypeSymbol, so Type Resolution's `if (ref->declaration)` guard drops
    // them from `baseTypeRefs` on this side too. They decide what an importer
    // may CALL on the type (§9.8's synthesized `Serialize`/`Deserialize`,
    // §18.5's format-spec admission, §9.8's component eligibility), so an
    // importer that cannot see them REJECTS a program this exporter compiles.
    // Only the DIRECT bases are carried; an ancestor chain reconstructs itself
    // because each exported ancestor carries its own.
    //
    // `nonSymbolBaseRefs` also holds an enum's §10.3 primitive underlying base
    // and, on an erroring compile, an unresolved base. Neither is a
    // conformance and neither has a use on the far side (the underlying width
    // rides `enumUnderlyingType`; an erroring module never gets this far), so
    // only Named entries cross.
    for (sema::TypeRef* ifaceRef : t.nonSymbolBaseRefs)
        if (ifaceRef && ifaceRef->kind == sema::TypeRef::Kind::Named)
            out.builtinInterfaceBases.push_back(convertType(ifaceRef));

    // §22.16 / §2.8.2 (#2054): carry invariant records in base-first order.
    // The record list is declaration-derived; no per-call-site discharge is
    // consulted while emitting an interface.
    out.invariants = typeInvariants(t, ctx, source, moduleNameFor(ctx));

    // Members — the §13 exported surface (public/protected).
    // §14/#662c / #695: for generic classes, also include private METHOD
    // signatures so the importer can reconstruct stubs for re-parsing the
    // ctor/dtor body source text (which may call private methods).  Only
    // signatures are exported — no body text, no different ABI — so the
    // private-member contract (not callable by downstream code outside the
    // class) is preserved; the stubs are used solely for sema name-resolution
    // inside the re-parsed ctor/dtor body.
    for (sema::Symbol* ms : t.members) {
        auto* member = dynamic_cast<const sema::MemberSymbol*>(ms);
        if (member == nullptr) continue;
        bool isMethod = dynamic_cast<const sema::MethodSymbol*>(ms) != nullptr;
        bool isField  = !isMethod &&
            dynamic_cast<const sema::FieldSymbol*>(ms) != nullptr;
        // fix #2473 (§9.2/§23.1/§22.8/§13, v0.236): an instance auto-property
        // (`private property W: int := 41;` — backed by compiler-generated
        // storage, §23.1 "the compiler generates the backing field") is a real
        // instance-storage member exactly like a plain field, but is a
        // PropertySymbol, not a FieldSymbol — so the existing `isField` test
        // never matched it, and a private/internal auto-property was dropped
        // from `out.members` entirely (the identical layout-aliasing/
        // under-allocation hazard #2456 fixed for plain FIELDS, just for the
        // auto-property subtype of storage member; #2478 closed the
        // IMPORTER-SIDE classification but presupposed the PropertySymbol
        // exists in `t->members` at all, which for a private/internal
        // auto-property it did not). `sema::hasAstAutoPropertyDecl` excludes
        // indexers and static auto-properties, so this matches ONLY instance-
        // storage auto-properties — the storage members
        // `collectInstanceStorage`/`collectRecordStorage`
        // (LlvmBackend_impl.hpp) count via `sema::isAutoPropertyStorageSymbol`
        // (sema/Symbol.hpp), whose importer-side arm is the #2478 wire bit this
        // very line computes. On the EXPORTER side (here) the decl is always
        // present, so the AST-only predicate is exact — and it is also the
        // right one to call: this is the authority the wire bit records, so
        // reading the bit back here would be circular (#3152).
        bool isAutoPropStorage = !isMethod && sema::hasAstAutoPropertyDecl(ms);
        bool exported = isExportedMember(member->visibility, inInterface);
        // For generic classes, also include private/internal METHODS so the
        // importer's ctor/dtor body re-parsing can resolve self-calls (#695).
        bool includeForCtorBody = isGenericMonomorphized && isMethod &&
            !exported &&
            (member->visibility == ast::Visibility::Private ||
             member->visibility == ast::Visibility::Internal);
        // §14.1 / #1224: for generic classes, also include private/internal
        // FIELDS so that the importer's method body re-parsing
        // (parseAndAttachMethodBody / buildSyntheticMethodSource) can resolve
        // bare field references such as `_count` inside a static method.
        // Without this, a method body that accesses a private field fails to
        // sema-check in the synthetic fresh context → parseAndAttachMethodBody
        // returns false → ms->decl stays null → the per-instantiation inline
        // monomorphization path is not taken → all instantiations silently
        // share the template-level global, violating §22.12 ("each distinct
        // instantiation has its own static state"). Only name + type is
        // exported — no initializer, no ABI change. Mirrors the private-method
        // rationale (#695): the field is not accessible outside the class;
        // the stub is used solely for sema name-resolution in re-parsed bodies.
        bool includeForMethodBody = isGenericMonomorphized && (isField || isAutoPropStorage) &&
            !exported &&
            (member->visibility == ast::Visibility::Private ||
             member->visibility == ast::Visibility::Internal);
        // fix #2456 (§22.8 / §2.8.2-§2.8.3, v0.235): a non-static instance
        // field occupies a slot in the class's PHYSICAL layout regardless of
        // its §13 visibility — declaration order, not exported-surface
        // membership, drives §22.8's storage layout. Before this, a
        // private/internal field on a NON-generic class was invisible to
        // `out.members` entirely (only the generic-class case above,
        // `includeForMethodBody`, carried private fields, and only for sema
        // name-resolution in re-parsed bodies). `collectInstanceStorage`/
        // `classStruct` (LlvmBackend_Layout.cpp) walk an imported base's
        // `t->members` with NO visibility gate at all to lay out a LOCAL
        // derived class's own fields — so a hidden private base field left
        // every subsequent field one slot short of the exporter's own
        // compiled layout: the derived class's own first field silently
        // ALIASED the base's real (invisible) field's physical slot, and a
        // *direct* `new ImportedBase()` under-allocated the instance
        // entirely (missing the hidden field's slot) — a genuine heap-
        // buffer-overflow the moment the exporter's real (correctly-sized)
        // init/ctor/accessor bodies wrote to it. Confirmed via direct
        // instrumentation: the exporter's own compilation of a class with
        // `private _v: int := 42;` lays out `_v` at struct index 2; the
        // importer's reconstruction — before this fix — had ZERO storage
        // members for that same class, so a locally-declared subclass's own
        // `_own` field landed at index 2 too, aliasing `_v`.
        // A static field is excluded: it has no instance-layout footprint
        // (process-global storage, §22.12) — this fix is scoped to §22.8
        // instance layout only, not the separate static cross-module story.
        //
        // fix #2468 (§9.1/§22.8, v0.235): extended to RECORDS. This was
        // originally scoped to CLASSES ONLY because a positional record's
        // storage order interleaves body fields BEFORE the positional fields
        // in `t.members` (confirmed by instrumentation — a record's
        // positional field is appended to `t.members` AFTER its body fields,
        // regardless of source order), and `buildRecordValue`'s positional-
        // constructor-argument-to-slot mapping (LlvmBackend_Exprs2.cpp) used
        // to assume `args[i]` maps to `fields[i]` by raw storage index —
        // widening this for records regressed a previously-CORRECT read
        // (`r.x` went from the right value to 0 once a private record body
        // field became visible for layout). That ordering defect is now
        // root-caused and fixed by #2464: `buildRecordValue` maps each
        // positional argument to the correct POSITIONAL field (the trailing
        // `args.size()` entries of `fields`/`t->members`, which stay a
        // contiguous block regardless of body-field count) rather than by
        // raw index, so a newly-visible hidden body field only shifts WHERE
        // the positional block starts, never which entries are in it.
        // Re-verified directly (this issue): the exact repro that motivated
        // the classes-only scoping —
        //   public record Rec(x: int)
        //     private _hidden: int := 77;
        //     public property Hidden: int => _hidden;
        //   end Rec;
        // — now round-trips `r.x` correctly cross-module with this gate
        // widened (see xmod_record_private_field_layout_import_2468_test.cpp
        // T1). Classes have an analogous non-interleaving guarantee:
        // `collectInstanceStorage` always lays the base class's storage down
        // FIRST, monotonically, before any derived class's own fields
        // (§22.8), so a newly-visible base field only ever shifts LATER
        // fields.
        //
        // NOTE: this makes a private/internal record body field's STORAGE
        // SLOT visible (fixing the identical aliasing/under-allocation
        // hazard #2456 fixes for classes — §9.1's storage layout is a
        // property of declaration order, not of the §13 exported surface,
        // exactly as §22.8 is for classes) but does NOT make its declared
        // DEFAULT VALUE correct cross-module for a PRIVATE member — that
        // value replay gap is #2467, fixed in this same issue pair only for
        // the already-exported (public) case; see buildRecordValue
        // (LlvmBackend_Exprs2.cpp) and this loop's
        // `mm.valueText.clear()` below, which deliberately keeps a
        // layout-only member's carriage genuinely layout-only (no
        // initializer, no ABI change — mirrors the class precedent).
        bool isStaticFieldMember = false;
        for (auto mod : member->modifiers)
            if (mod == "static") { isStaticFieldMember = true; break; }
        // fix #2473 (§9.2/§23.1/§22.8): widen `includeForLayout` to also
        // match an instance auto-property (`isAutoPropStorage`, recognized
        // above) — the same storage-layout hazard #2456 (classes) / #2468
        // (records) fixed for plain fields applies verbatim to an
        // auto-property's compiler-generated backing slot. `isAutoPropStorage`
        // already excludes static auto-properties, so `!isStaticFieldMember`
        // is redundant for that arm but kept for the plain-field path; an
        // indexer (PropertySymbol with `isIndexer`) is excluded by
        // `hasAstAutoPropertyDecl` and never reaches this gate.
        bool includeForLayout = (isField || isAutoPropStorage) &&
            !exported && !isStaticFieldMember &&
            (t.kind == ast::TypeDeclKind::Class ||
             t.kind == ast::TypeDeclKind::Record);
        if (!exported && !includeForCtorBody && !includeForMethodBody &&
            !includeForLayout)
            continue;
        MetaMember mm;
        if (convertMember(*member, inInterface, ctx, mm, isGenericMonomorphized, source)) {
            // A field included ONLY for layout (not otherwise exported, and
            // not already carried for generic-body name resolution) is
            // name-invisible outside the class — strip the initializer text
            // convertMember unconditionally attaches for any foldable field
            // initializer. Only the field's NAME, TYPE, and declaration-order
            // POSITION are needed to reconstruct the physical layout; the
            // default value is not, and dropping it keeps this genuinely a
            // layout-only carriage (mirrors the "no initializer, no ABI
            // change" contract the generic-class `includeForMethodBody` case
            // above already documents for the same reason).
            if (includeForLayout && !exported && !includeForMethodBody) {
                mm.valueText.clear();
                // fix #2860 (§2.8.2 definition-site visibility, v0.240): keep
                // `fieldInitText` for a GENERIC class. The whole reason this
                // clears the initializer carriage — "the default value is not
                // needed to reconstruct the physical layout" — holds only where
                // some OTHER mechanism replays the initializer, which for a
                // non-generic class is the exported `bz$<Type>$init`. A generic
                // class has none: its construction is monomorphized in the
                // consumer, so a private field's initializer that does not cross
                // is silently dropped there, exactly like a public one (and
                // §2.8.2's definition-site clause is explicit that the interface
                // "must therefore carry whatever internal machinery its exported
                // generic bodies reach"). This is the text the consumer names in
                // the §2.8.2 diagnostic; it adds no member, no slot and no ABI —
                // the member itself was already being carried for layout.
                if (!isGenericMonomorphized) mm.fieldInitText.clear();
            }
            out.members.push_back(std::move(mm));
        }
    }

    // §10 enum constants (declared on the TypeDecl, collected as symbols but
    // their values come from the AST per §10.1/§10.2).
    if (t.kind == ast::TypeDeclKind::Enum) {
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(t.decl)) {
            // The symbol walk above already skipped EnumConstantSymbols (they
            // are not MemberSymbol-with-a-decl-type kinds we handle); compute
            // their values from the AST so §10.2 continuation is correct.
            // (EnumConstantSymbol is a MemberSymbol; convertMember returns
            // false for it, so it was filtered out — re-add here with values.)
            appendEnumConstants(*td, out);
        }
    }

    // §35.1 (fix #3144, family F9 of #3152): the choice's cases + payloads.
    // `convertMember` returns false for a ChoiceCaseSymbol (it is not one of
    // the MetaMemberKind shapes), so the symbol walk above filtered them out —
    // exactly as it filters EnumConstantSymbols, and re-added here for the same
    // reason.
    if (t.kind == ast::TypeDeclKind::Choice) appendChoiceCases(t, ctx, out);

    // §14 / #662c: for a GENERIC class, extract the destructor body source text
    // so the importer can run user destructor side-effects (#662 foundation C).
    // The destructor is skipped by convertMember (§13.3 not user-callable), so
    // we walk members here to find a DestructorDecl with a non-empty body.
    if (isGenericMonomorphized && !source.empty()) {
        for (sema::Symbol* ms : t.members) {
            const auto* ds = dynamic_cast<const sema::DestructorSymbol*>(ms);
            if (!ds) continue;
            if (auto* dd = dynamic_cast<const ast::DestructorDecl*>(ds->decl)) {
                if (!dd->body.empty() &&
                    dd->span.begin.offset < dd->span.end.offset &&
                    dd->span.end.offset <= source.size()) {
                    out.dtorBodyText = std::string(
                        source.substr(dd->span.begin.offset,
                                      dd->span.end.offset -
                                          dd->span.begin.offset));
                }
            }
            break;  // at most one destructor per class
        }
    }

    // §9.8 / §2.8.3 rule 4 (#3179, family F10 of umbrella #3152): the
    // §9.8 class wire SHAPE and `polymorphic(...)` REGISTRY, on the symbol
    // side (mirrors the place #3140's F2 work wrote them — see
    // `sema::TypeSymbol::serializedShape` / `::serializedPolyCases` in
    // Symbol.hpp). Until this line existed, the §9.8 class serialization
    // surface was correctly ELIGIBLE on the importer side (#3140's interlock
    // is gone, #3185 confirmed the choice-case surface is sound, and the
    // resolver's pre-pass writes the symbol shape on the local side) but the
    // shape and registry were NEVER WRITTEN to the wire — `t.decl` is non-
    // null here so the resolver ran, `t.serializedShape` is the resolver's
    // chosen "tree" / "graph" / "cyclic" string, and the importer has no
    // other path to learn it.
    //
    // Read from the SYMBOL, not from `t.decl->serializedClass->shape`, for
    // the same reason the other class-level deriving properties (the §31.4
    // exception base, #2950; the §28.9 dispose verdict, #3191; the built-in
    // interface bases, #3139) all read from the symbol here: the symbol is
    // what the resolver populates, and reading both sides would re-introduce
    // the AST / symbol drift this same family (F2 #3140) recorded. The
    // symbol-side writer IS authoritative on the local side, by the same
    // resolvability argument.
    //
    // Conditional on `kind == Class`: every non-class kind has an empty
    // shape and registry on the symbol, so a record / enum / choice with no
    // outgoing §9.8 carriage is byte-identical to a pre-#3179 build.
    if (t.kind == ast::TypeDeclKind::Class) {
        out.serializedShape = std::string(t.serializedShape);
        for (const auto& pc : t.serializedPolyCases) {
            MetaTypeDecl::MetaPolyCase outPc;
            outPc.discriminator = std::string(pc.discriminator);
            // The resolver populates SerializedPolyCase::type with a
            // sema::TypeSymbol (the resolved concrete type for the
            // registry entry). The wire carries a MetaType describing the
            // type by name and structure; since the registry's entries are
            // named type references (each `: ISerializable, IDeserializable`
            // is a class or interface, not a primitive or a generic
            // construction), the named-name-with-module form suffices — same
            // convention #3144 uses for `MetaChoicePayloadField::type` and
            // #3139 uses for `builtinInterfaceBases`. The Loader resolves
            // this on import via `convertAstType`, the same code path the
            // rest of the export uses.
            if (pc.type) {
                outPc.type.kind = TypeKind::Named;
                outPc.type.namedName = std::string(pc.type->name);
                // moduleName slot is left empty: §9.8 tier 4 lists entries
                // by their short name in the registry; same-compilation
                // entries are the common case, and #3144's choice carries
                // the module name through the `baseTypeRefs` source rather
                // than inline. A cross-module concrete type named in a
                // polymorphic registry will lose resolution on import (and
                // get rejected at sema, loudly, as a missing type) — same
                // conservative shape every other tier-4-class surface has.
                if (!outPc.type.namedName.empty())
                    out.serializedPolyCases.push_back(std::move(outPc));
            } else {
                // No resolved concrete type: still emit the discriminator
                // (it can be useful as a hint in the importer's diagnostic);
                // the type field's error sentinel will surface as a
                // SerializationException at read.
                out.serializedPolyCases.push_back(std::move(outPc));
            }
        }
    }

    return out;
}

// Pin `gv` in the module's `llvm.used` list so it is never dead-stripped (it
// exists only to carry its section bytes; nothing references it). Reimplements
// the relevant slice of ModuleUtils::appendToUsed without depending on the
// TransformUtils LLVM component. `llvm.used` is an appending-linkage array of
// i8* in the special "llvm.metadata" section; we replace any existing one with
// a superset that includes `gv` (this module only ever adds one such global,
// so in practice there is no prior `llvm.used` to merge with).
void pinInLlvmUsed(llvm::Module& module, llvm::GlobalVariable* gv) {
    auto* ptrTy = llvm::PointerType::getUnqual(module.getContext());
    std::vector<llvm::Constant*> used;
    if (auto* existing = module.getGlobalVariable("llvm.used")) {
        if (auto* init =
                llvm::dyn_cast<llvm::ConstantArray>(existing->getInitializer())) {
            for (unsigned i = 0, n = init->getNumOperands(); i < n; ++i)
                used.push_back(init->getOperand(i));
        }
        existing->eraseFromParent();
    }
    used.push_back(llvm::ConstantExpr::getBitCast(gv, ptrTy));

    auto* arrTy = llvm::ArrayType::get(ptrTy, used.size());
    auto* newUsed = new llvm::GlobalVariable(
        module, arrTy, /*isConstant=*/false,
        llvm::GlobalValue::AppendingLinkage,
        llvm::ConstantArray::get(arrTy, used), "llvm.used");
    newUsed->setSection("llvm.metadata");
}

}  // namespace

// §26.5 rule 4 (#2329): preserve an extension block's declared `where` head in
// a compact source-like form. The loader re-parses these clauses into the
// synthetic ExtensionDecl; no constraint-resolution policy is duplicated in
// the metadata layer.
static std::string extensionConstraintTypeText(const ast::Type& t) {
    std::string out;
    if (t.name) {
        for (std::size_t i = 0; i < t.name->parts.size(); ++i) {
            if (i) out += '.';
            out += std::string(t.name->parts[i]);
        }
    }
    if (out.empty()) out = "?";
    if (!t.genericArgs.empty()) {
        out += '<';
        for (std::size_t i = 0; i < t.genericArgs.size(); ++i) {
            if (i) out += ", ";
            out += extensionConstraintTypeText(*t.genericArgs[i]);
        }
        out += '>';
    }
    if (t.nullable) out += '?';
    return out;
}

static std::vector<std::string> extensionConstraintTexts(
    const std::vector<ast::GenericConstraint>& constraints) {
    std::vector<std::string> out;
    out.reserve(constraints.size());
    for (const ast::GenericConstraint& gc : constraints) {
        std::string text = "where " + std::string(gc.typeParam) + ": ";
        if (!gc.modifierConstraint.empty()) {
            text += std::string(gc.modifierConstraint);
        } else {
            for (std::size_t i = 0; i < gc.items.size(); ++i) {
                if (i) text += ", ";
                const ast::ConstraintItem& item = gc.items[i];
                if (item.kind == "type" && item.typeArg)
                    text += extensionConstraintTypeText(*item.typeArg);
                else
                    text += std::string(item.kind);
            }
        }
        out.push_back(std::move(text));
    }
    return out;
}

// §26 / §2.5 (#735): convert one `public extension T` block into its wire form,
// or return false to skip it (a shape whose cross-module dispatch is not exactly
// the exported `bz$ext$<T>$<M>` symbol — see MetaExtension's header note).
static bool convertExtension(const sema::ExtensionSymbol& ext,
                             sema::SemaContext& ctx, MetaExtension& out,
                             std::string_view source,
                             std::string_view sourceFile) {
    const auto* decl = dynamic_cast<const ast::ExtensionDecl*>(ext.decl);
    if (decl == nullptr || !decl->targetType) return false;
    // §13.2 / §26.3: an `internal` block (the default) is not downstream-visible.
    if (decl->visibility != ast::Visibility::Public) return false;
    // §26.5 rule 4 (#2329): extension blocks are nameless, so diagnostics need
    // their original declaration site and complete declared head after import.
    out.sourceSpan.file = std::string(sourceFile);
    out.sourceSpan.beginLine = decl->span.begin.line;
    out.sourceSpan.beginColumn = decl->span.begin.column;
    out.sourceSpan.endLine = decl->span.end.line;
    out.sourceSpan.endColumn = decl->span.end.column;
    out.genericConstraints = extensionConstraintTexts(decl->genericConstraints);
    // §26.4: a block with `where` constraints, or a target carrying type
    // arguments, binds block-level type parameters — the exported symbol was
    // compiled against the exporter's placeholder layout, so dispatching it
    // importer-side needs the #783 monomorphization machinery. Not exported.
    if (!decl->genericConstraints.empty()) return false;
    if (!decl->targetType->genericArgs.empty()) return false;
    const sema::TypeRef* target = ctx.resolvedType(*decl->targetType);
    if (target == nullptr) return false;
    // §26.4: a bare single-segment identifier in the head introduces a fresh
    // block-level type parameter; TypeResolution leaves such a head as a
    // null-declaration Named ref. Only a resolved type (or a primitive, §26.2)
    // has a stable cross-module identity to dispatch against.
    if (target->kind == sema::TypeRef::Kind::Named &&
        target->declaration == nullptr)
        return false;
    out.target = convertType(target);
    out.visibility = toMetaVisibility(decl->visibility, /*inInterface=*/false);
    for (sema::Symbol* m : ext.members) {
        // fix #2583 (§26.3, §26.5 step 2, v0.236): the prior declaration
        // accepted only methods and silently dropped property members on the
        // (incorrect) rationale that "A property has no exported accessor
        // symbol (it is inlined at the use site), so only methods cross". The
        // inlining-at-use-site story is true for a LOCAL extension property
        // (codegen calls computedGetterOf, which lowers the body in the local
        // sema context). For a CROSS-MODULE extension property the importer
        // has no local sema body to call, so the property must ride the wire
        // and the importer must re-parse + attach the body — exactly mirroring
        // how a generic-class property rides in `propertyBodyText` (#2893).
        // The method branch below is unchanged.
        if (auto* prop = dynamic_cast<const sema::PropertySymbol*>(m)) {
            // §26.3: an extension property is expression-bodied only
            // (`property Name: Type => expr;`); the block-accessor and auto
            // forms are not admitted by parser/sema. Restate the gate here
            // as a belt-and-braces — a stale or buggy AST shape that reaches
            // this site gets the same decline.
            auto* pd = dynamic_cast<const ast::PropertyDecl*>(prop->decl);
            if (pd == nullptr) continue;
            if (pd->bodyKind != ast::PropertyBodyKind::Expression) continue;
            if (pd->isIndexer) continue;
            if (!isExportedMember(prop->visibility, /*inInterface=*/false))
                continue;
            // (Generic extension properties are impossible by construction:
            // PropertySymbol carries no genericParams — only FunctionSymbol /
            // MethodSymbol / TypeSymbol / ExtensionSymbol do, and the parser /
            // sema never admit a generic property. Importer-side
            // monomorphization is required only for generic methods, handled
            // below.)
            MetaMember mm;
            // Reuse the regular Property-branch of convertMember: it sets
            // kind=Property, type, hasGetter/hasSetter (true/false — §26.3
            // computed read-only), isAutoProperty=false (extensions are
            // never auto), setterVisibility=Public, and — when source
            // carries the full PropertyDecl span — propertyBodyText so the
            // importer can re-parse it. visibility/modifiers ride alongside.
            if (convertMember(*prop, /*inInterface=*/false, ctx, mm,
                              /*isGenericType=*/false, source)) {
                // §26.3: extension properties have no setter — the spec read
                // "computed read-only properties" is the only form. Force the
                // wire shape to match even if the exporter-derived conversion
                // accidentally disagreed (a defensive belt).
                mm.hasSetter = false;
                out.members.push_back(std::move(mm));
            }
            continue;
        }
        auto* meth = dynamic_cast<const sema::MethodSymbol*>(m);
        // §26.3: extensions declare methods and computed read-only properties.
        // A property has no exported accessor symbol (it is inlined at the use
        // site), so only methods cross; a generic extension method would need
        // importer-side monomorphization (as above).
        if (meth == nullptr || meth->isOperator) continue;
        if (!meth->genericParams.empty()) continue;
        if (!isExportedMember(meth->visibility, /*inInterface=*/false)) continue;
        if (dynamic_cast<const ast::MethodDecl*>(meth->decl) == nullptr) continue;
        MetaMember mm;
        if (convertMember(*meth, /*inInterface=*/false, ctx, mm,
                          /*isGenericType=*/false, source))
            out.members.push_back(std::move(mm));
    }
    return !out.members.empty();
}

ModuleMetadata buildModuleMetadata(sema::SemaContext& ctx,
                                    std::string_view source,
                                    std::string_view sourceFile) {
    ModuleMetadata md;
    sema::ModuleSymbol* mod = ctx.module();
    if (mod == nullptr) return md;  // no module (front-end error path)

    // §2.1: the module name is the full dotted form (`module Bank.Core;` →
    // "Bank.Core"). It is the identity a downstream `import` matches (§13.4), so
    // the metadata must carry the WHOLE qualified name. ModuleSymbol::name holds
    // only the last identifier ("Core"), so reconstruct the dotted name from the
    // ModuleDecl's QualifiedName.
    md.moduleName = std::string(mod->name);
    if (auto* mdecl = dynamic_cast<const ast::ModuleDecl*>(mod->decl)) {
        if (mdecl->name && !mdecl->name->parts.empty()) {
            std::string dotted;
            for (std::size_t i = 0; i < mdecl->name->parts.size(); ++i) {
                if (i != 0) dotted += '.';
                dotted += std::string(mdecl->name->parts[i]);
            }
            md.moduleName = std::move(dotted);
        }
    }

    for (sema::Symbol* child : mod->children) {
        // §26 / §2.5 (#735): a `public extension T` block IS part of the
        // exported surface — §26.5 step 2 requires an importer to search "all
        // imported modules" for extension blocks on the receiver's type.
        if (auto* ext = dynamic_cast<const sema::ExtensionSymbol*>(child)) {
            MetaExtension me;
            if (convertExtension(*ext, ctx, me, source, sourceFile))
                md.extensions.push_back(std::move(me));
            continue;
        }
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(child);
        if (ts == nullptr) continue;  // aliases/externs: see note
        if (!isExportedType(*ts)) continue;  // §13.2 internal types not exported
        // §2.20/#2047: the auto-imported stdlib prelude decl (Result) is NOT
        // part of this module's public interface — every importer injects its
        // own copy, so exporting it would (a) pollute the interface with a
        // decl the module never wrote and (b) mis-kind it as a Class in the
        // schema (Choice has no MetaTypeDeclKind yet, #2121). Also keeps the
        // driver-path metadata identical to the metadata-only build path (the
        // object_section_roundtrip pin).
        if (auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl))
            if (td->isSynthesizedPrelude) continue;
        md.types.push_back(convertTypeDecl(*ts, ctx, source));
        md.exports.push_back(std::string(ts->name));  // §2.4 exported name
    }

    // NOTE (decline-with-reasons, CLAUDE.md §4): the schema also carries
    // top-level functions (MetaFunction) and constants (MetaConstant), but the
    // current AST/parser has NO module-level free-function or free-const
    // TopDecl — TopDecl is only TypeDecl / TypeAlias / ExtensionDecl /
    // ExternBlock (ast::SourceFile::topDecls). So nothing populates md.functions
    // / md.constants here; the fields stay empty until that grammar lands. Type
    // aliases and extern blocks are likewise out of this increment's scope
    // (their cross-module-import semantics are Increment 3+). Extension blocks
    // ARE exported (§26 / #735) — see the loop above and MetaExtension.
    return md;
}

std::size_t embedModuleMetadata(llvm::Module& module, sema::SemaContext& ctx,
                                std::string_view source, bool isExecutable,
                                std::string_view sourceFile) {
    // §2.6 step 4: build → serialize → embed. Build the by-value metadata for
    // the just-compiled module and serialize it with Increment 1's bitstream
    // codec.
    ModuleMetadata md = buildModuleMetadata(ctx, source, sourceFile);
    // §2.8.4 rule 1 / rule 4 (issue #1733, schema v6): record the artifact kind
    // so module::loadDependency can reject this artifact if it is presented as a
    // dependency while being an executable (it carries a native entry symbol).
    md.isExecutable = isExecutable;
    std::vector<std::uint8_t> bytes = serialize(md);

    llvm::LLVMContext& llvm = module.getContext();
    // A byte array constant holding the serialized blob.
    auto* i8 = llvm::Type::getInt8Ty(llvm);
    auto* arrTy = llvm::ArrayType::get(i8, bytes.size());
    std::vector<llvm::Constant*> elems;
    elems.reserve(bytes.size());
    for (std::uint8_t b : bytes)
        elems.push_back(llvm::ConstantInt::get(i8, b));
    llvm::Constant* init = llvm::ConstantArray::get(arrTy, elems);

    // A constant global placed in the dedicated `.bezel.meta` section. The
    // section name carries the bytes into the object (a non-allocated
    // SHT_PROGBITS section the linker preserves by name into the final
    // artifact). The downstream tool recovers the bytes by section name (objcopy
    // / LLVM object API), not by symbol.
    //
    // The object-emission path (codegen::emitObjectFile) lowers IR straight to
    // machine code with no optimization / global-DCE pass, so an unreferenced
    // global is still emitted. To be robust against any future addition of such
    // a pass, we also pin the global in `llvm.used` by hand (the
    // ModuleUtils::appendToUsed helper would pull in the TransformUtils LLVM
    // component, which is not linked; building the array directly avoids that
    // extra dependency).
    auto* gv = new llvm::GlobalVariable(
        module, arrTy, /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, init, kMetadataGlobalName);
    gv->setSection(kMetadataSectionName);
    gv->setAlignment(llvm::Align(1));
    pinInLlvmUsed(module, gv);

    return bytes.size();
}

}  // namespace bezel::module
