#include "module/ModuleMetadataBuilder.hpp"

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "ast/Node.hpp"
#include "sema/SemaContext.hpp"
#include "sema/Symbol.hpp"
#include "sema/TypeRef.hpp"

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
            if (ref->declaration != nullptr)
                t.namedName = std::string(ref->declaration->name);
            for (const sema::TypeRef* arg : ref->genericArgs)
                t.genericArgs.push_back(convertType(arg));
            return t;
        case sema::TypeRef::Kind::InlineArray:
            // §15: [N]T / [N owned]T. The size is a compile-time constant
            // expression; the schema carries it textually (the loader
            // re-evaluates it). We leave it empty here — the const-expr
            // evaluator is sema-internal and not threaded to the builder — and
            // record only the element + ownership, which is the type-level shape
            // a downstream importer needs.
            t.kind = TypeKind::InlineArray;
            t.arrayOwned = ref->arrayOwned;
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
std::string renderConstExpr(const ast::Expr* e);

std::string renderConstExpr(const ast::Expr* e) {
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
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e))
        return std::string(id->name);
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
        std::string base = renderConstExpr(ma->target.get());
        if (base.empty()) return "";
        return base + "." + std::string(ma->member);
    }
    return "";
}

// Try to read a plain non-negative integer from an int-literal expression
// (§10.2 explicit enum value). Returns true + value on success.
bool tryEvalIntLiteral(const ast::Expr* e, std::int64_t& out) {
    auto* i = dynamic_cast<const ast::IntLiteralExpr*>(e);
    if (i == nullptr) return false;
    // The lexeme may carry a sign or suffix in richer programs; for the §10.2
    // continuation rule we only need plain decimal literals (the common case).
    // std::strtoll parses the leading integer and ignores a trailing suffix.
    char* end = nullptr;
    std::string text(i->text);
    long long v = std::strtoll(text.c_str(), &end, 0);
    if (end == text.c_str()) return false;  // not a number
    out = static_cast<std::int64_t>(v);
    return true;
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
// Param list (the symbol list parallels the AST param list in order).
void fillParamDefaults(std::vector<MetaParam>& out,
                       const std::vector<ast::Param>& astParams) {
    const std::size_t n = std::min(out.size(), astParams.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (astParams[i].defaultValue) {
            out[i].hasDefault = true;
            out[i].defaultText = renderConstExpr(astParams[i].defaultValue.get());
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

// Convert a single exported member. Returns false to skip (kind not part of the
// exported surface — e.g. a destructor, which §13.3 notes is not user-callable;
// its public face is Dispose()).
bool convertMember(const sema::MemberSymbol& m, bool inInterface,
                   sema::SemaContext& ctx, MetaMember& out) {
    out.visibility = toMetaVisibility(m.visibility, inInterface);
    out.modifiers = toStrings(m.modifiers);
    out.name = std::string(m.name);

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
        // boundary and is a documented limitation. Static fields are exported as
        // EXTERNAL storage globals (their initializer runs in the exporter), so
        // they need no valueText here.
        bool isStaticField = false;
        for (auto mod : m.modifiers)
            if (mod == "static") isStaticField = true;
        if (!isStaticField && fd->initializer)
            out.valueText = renderConstExpr(fd->initializer.get());
        return true;
    }
    // §9.1 positional record field — the FieldSymbol's decl is the TypeDecl and
    // the type lives on posParam.
    if (auto* fs = dynamic_cast<const sema::FieldSymbol*>(&m)) {
        if (fs->posParam != nullptr) {
            out.kind = MetaMemberKind::Field;
            out.type = convertAstType(fs->posParam->type.get(), ctx);
            return true;
        }
    }
    if (auto* meth = dynamic_cast<const sema::MethodSymbol*>(&m)) {
        out.kind = MetaMemberKind::Method;
        out.isOperator = meth->isOperator;
        if (auto* md = dynamic_cast<const ast::MethodDecl*>(m.decl)) {
            out.type = convertAstType(md->returnType.get(), ctx);
            out.returnOwned = md->returnOwned;
            out.genericParams = convertGenericParams(
                meth->genericParams, md->genericConstraints, ctx);
            out.params = convertParams(meth->params, ctx);
            fillParamDefaults(out.params, md->params);
        } else if (auto* od = dynamic_cast<const ast::OperatorDecl*>(m.decl)) {
            // §25 operator overload.
            out.type = convertAstType(od->returnType.get(), ctx);
            out.returnOwned = od->returnOwned;
            out.params = convertParams(meth->params, ctx);
            fillParamDefaults(out.params, od->params);
        }
        return true;
    }
    if (auto* ctor = dynamic_cast<const sema::ConstructorSymbol*>(&m)) {
        out.kind = MetaMemberKind::Constructor;
        out.params = convertParams(ctor->params, ctx);
        if (auto* cd = dynamic_cast<const ast::ConstructorDecl*>(m.decl))
            fillParamDefaults(out.params, cd->params);
        return true;
    }
    if (auto* prop = dynamic_cast<const sema::PropertySymbol*>(&m)) {
        out.kind =
            prop->isIndexer ? MetaMemberKind::Indexer : MetaMemberKind::Property;
        if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m.decl)) {
            out.type = convertAstType(pd->type.get(), ctx);
            if (prop->isIndexer) {
                out.params = convertParams(prop->indexerParams, ctx);
                fillParamDefaults(out.params, pd->indexerParams);
            }
        }
        return true;
    }
    if (dynamic_cast<const sema::EventSymbol*>(&m)) {
        out.kind = MetaMemberKind::Event;
        if (auto* ed = dynamic_cast<const ast::EventDecl*>(m.decl))
            out.type = convertAstType(ed->type.get(), ctx);  // §24 payload type
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
    // Destructor (§13.3: not part of the public surface — Dispose() is) and
    // nested types are not emitted as members in this increment.
    return false;
}

// --- §10 enum constants -----------------------------------------------------

// Append the enum constants of `td` to `out.members` with their underlying
// values per §10.1 (implicit from 0) / §10.2 (explicit + continuation).
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
        out.members.push_back(std::move(m));
    }
}

// --- types ------------------------------------------------------------------

MetaTypeDecl convertTypeDecl(const sema::TypeSymbol& t, sema::SemaContext& ctx) {
    MetaTypeDecl out;
    out.name = std::string(t.name);
    out.kind = toMetaTypeDeclKind(t.kind);
    out.visibility = toMetaVisibility(t.visibility, /*inInterface=*/false);
    out.modifiers = toStrings(t.modifiers);

    const bool inInterface = (t.kind == ast::TypeDeclKind::Interface);

    // §14 generic params + their §14.2 constraints (from the TypeDecl AST).
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(t.decl)) {
        out.genericParams =
            convertGenericParams(t.genericParams, td->genericConstraints, ctx);
    }

    // §12 base class + interfaces (resolved TypeRefs, parallel to baseTypes).
    for (sema::TypeRef* baseRef : t.baseTypeRefs)
        out.baseTypes.push_back(convertType(baseRef));

    // Members — only the §13 exported surface.
    for (sema::Symbol* ms : t.members) {
        auto* member = dynamic_cast<const sema::MemberSymbol*>(ms);
        if (member == nullptr) continue;
        if (!isExportedMember(member->visibility, inInterface)) continue;
        MetaMember mm;
        if (convertMember(*member, inInterface, ctx, mm))
            out.members.push_back(std::move(mm));
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

ModuleMetadata buildModuleMetadata(sema::SemaContext& ctx) {
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
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(child);
        if (ts == nullptr) continue;  // aliases/extensions/externs: see note
        if (!isExportedType(*ts)) continue;  // §13.2 internal types not exported
        md.types.push_back(convertTypeDecl(*ts, ctx));
        md.exports.push_back(std::string(ts->name));  // §2.4 exported name
    }

    // NOTE (decline-with-reasons, CLAUDE.md §4): the schema also carries
    // top-level functions (MetaFunction) and constants (MetaConstant), but the
    // current AST/parser has NO module-level free-function or free-const
    // TopDecl — TopDecl is only TypeDecl / TypeAlias / ExtensionDecl /
    // ExternBlock (ast::SourceFile::topDecls). So nothing populates md.functions
    // / md.constants here; the fields stay empty until that grammar lands. Type
    // aliases, extensions, and extern blocks are likewise out of this
    // increment's scope (their cross-module-import semantics are Increment 3+).
    return md;
}

std::size_t embedModuleMetadata(llvm::Module& module, sema::SemaContext& ctx) {
    // §2.6 step 4: build → serialize → embed. Build the by-value metadata for
    // the just-compiled module and serialize it with Increment 1's bitstream
    // codec.
    ModuleMetadata md = buildModuleMetadata(ctx);
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
