#include "codegen/LlvmBackend_impl.hpp"

namespace bezel::codegen {

// #383 / §28: forward declaration — defined later in this file. A reference
// TypeRef carrying a vtable (a Named class/interface). Used by the §28.3
// unwind-disposal eligibility helpers above its definition.
static bool isClassOrInterfaceRef(const sema::TypeRef* rt);

std::string Emitter::functionName(sema::MethodSymbol* ms) {
    if (isMain(ms)) return "main";
    std::string cls = ms->parent ? std::string(ms->parent->name) : "";
    // §25: an operator's name is empty; build a unique, collision-free
    // name from its token (a type may declare several operators).
    if (ms->isOperator)
        return "bz$" + cls + "$op$" + std::string(ms->operatorToken);
    return "bz$" + cls + "$" + std::string(ms->name);
}


void Emitter::declareExterns() {
    for (const auto& sym : ctx_.symbols()) {
        auto* xb = dynamic_cast<const sema::ExternBlockSymbol*>(sym.get());
        if (!xb) continue;
        for (sema::Symbol* d : xb->decls) {
            auto* xd = dynamic_cast<sema::ExternDeclSymbol*>(d);
            if (!xd || !xd->externDecl) continue;
            const ast::ExternDecl* ed = xd->externDecl;
            llvm::Type* retTy = ed->returnType
                                    ? lowerType(ed->returnType.get())
                                    : llvm::Type::getVoidTy(llvm_);
            std::vector<llvm::Type*> paramTys;
            for (const auto& p : ed->params)
                paramTys.push_back(lowerType(p.type.get()));
            auto* fnTy = llvm::FunctionType::get(retTy, paramTys, false);
            externFns_[xd] = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage,
                std::string(xd->name), module_.get());
        }
    }
}


void Emitter::computeEnumValues() {
    for (const auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (!ts || ts->kind != ast::TypeDeclKind::Enum) continue;
        auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
        if (!td) continue;
        std::int64_t running = 0;
        for (const auto& ec : td->enumConstants) {
            std::int64_t val = running;
            if (ec.value) {
                if (auto* lit = dynamic_cast<const ast::IntLiteralExpr*>(
                        ec.value.get()))
                    val = parseIntLiteral(lit->text);
            }
            for (const sema::Symbol* m : ts->members) {
                if (dynamic_cast<const sema::EnumConstantSymbol*>(m) &&
                    m->name == ec.name) {
                    enumValue_[m] = val;
                    break;
                }
            }
            running = val + 1;
        }
    }
}


llvm::Value* Emitter::enumConstant(const ast::Expr& e) {
    auto* sym = ctx_.exprSymbol(e);
    if (!dynamic_cast<sema::EnumConstantSymbol*>(sym)) return nullptr;
    auto it = enumValue_.find(sym);
    if (it == enumValue_.end()) return nullptr;
    return llvm::ConstantInt::get(
        i32(), static_cast<std::uint64_t>(it->second), /*isSigned=*/true);
}


const sema::TypeSymbol* Emitter::enumTypeOf(const ast::Expr& e) {
    // CG30: `this` inside an enum instance method — sema does not set
    // an exprType for `this`; use currentEnumMethod_ instead.
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        if ((id->name == "this" || id->name == "base") && currentEnumMethod_)
            return currentEnumMethod_;
    }
    const sema::TypeRef* rt = ctx_.exprType(e);
    if (!rt) {
        sema::Symbol* sym = ctx_.exprSymbol(e);
        if (auto* loc = dynamic_cast<sema::LocalSymbol*>(sym))
            rt = loc->declType;
        else if (auto* par = dynamic_cast<sema::ParameterSymbol*>(sym))
            rt = par->declType ? ctx_.resolvedType(*par->declType)
                               : nullptr;
    }
    if (!rt || rt->kind != sema::TypeRef::Kind::Named) return nullptr;
    const sema::TypeSymbol* ts = rt->declaration;
    if (ts && ts->kind == ast::TypeDeclKind::Enum) return ts;
    return nullptr;
}


llvm::Value* Emitter::enumConstantOfPatternExpr(const ast::Expr& e,
                                       const sema::TypeSymbol* enumTy) {
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e))
        return enumConstantByName(enumTy, ma->member);
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e))
        return enumConstantByName(enumTy, id->name);
    return nullptr;
}


llvm::Value* Emitter::enumConstantByName(const sema::TypeSymbol* enumTy,
                                std::string_view name) {
    if (!enumTy) return nullptr;
    for (const sema::Symbol* m : enumTy->members) {
        if (m->name != name) continue;
        if (!dynamic_cast<const sema::EnumConstantSymbol*>(m)) continue;
        auto it = enumValue_.find(m);
        if (it == enumValue_.end()) return nullptr;
        return llvm::ConstantInt::get(
            i32(), static_cast<std::uint64_t>(it->second),
            /*isSigned=*/true);
    }
    return nullptr;
}


llvm::Value* Emitter::constMemberValue(const ast::Expr& e) {
    auto* cs = dynamic_cast<sema::ConstMemberSymbol*>(ctx_.exprSymbol(e));
    if (!cs) return nullptr;
    auto* cd = dynamic_cast<const ast::ConstDecl*>(cs->decl);
    if (cd && cd->value) return lowerExpr(*cd->value);
    // §22.12 (issue #328 FINDING [3] BUG A): an IMPORTED const has no AST decl —
    // its compile-time value was carried textually in the dependency metadata.
    // A class-level const initializer "must be a compile-time-constant
    // expression" (§22.12), so the library emitted NO storage symbol for it; we
    // FOLD the value here rather than load it from a (non-existent) global.
    if (!cd) return importedConstValue(e, cs);
    return nullptr;
}

llvm::Value* Emitter::importedConstValue(const ast::Expr& e,
                                         sema::ConstMemberSymbol* cs) {
    const std::string* text = ctx_.importedConstValue(cs);
    if (!text || text->empty()) return nullptr;
    // The const's primitive type drives how the textual value is parsed and what
    // LLVM constant is emitted. Prefer the read site's resolved type, then the
    // imported member's recorded type (§22.12 / issue #328 inc 3 side table).
    const sema::TypeRef* rt = ctx_.exprType(e);
    if (!rt || rt->kind != sema::TypeRef::Kind::Primitive)
        rt = ctx_.importedMemberType(cs);
    return foldImportedConstText(*text, rt);
}

// §22.12 / §22.8 (issue #328 FINDING [3] / [7]): materialize a textual,
// compile-time-constant value carried in a dependency's metadata as an LLVM
// constant of type `rt`. Shared by the imported-const read path and the
// imported-field-initializer replay (FINDING [7]). Returns null when the text is
// not a shape this folds (e.g. a by-name reference to another const/enum, which
// renderConstExpr emitted as an identifier rather than a digit string) —
// declining is a loud failure rather than an unsound 0.
llvm::Value* Emitter::foldImportedConstText(const std::string& text,
                                            const sema::TypeRef* rt) {
    if (text.empty()) return nullptr;
    sema::PrimitiveType prim =
        (rt && rt->kind == sema::TypeRef::Kind::Primitive)
            ? rt->primitive
            : sema::PrimitiveType::Int;
    switch (prim) {
        case sema::PrimitiveType::Bool:
            return llvm::ConstantInt::get(i1(), text == "true" ? 1 : 0);
        case sema::PrimitiveType::Char:
            return llvm::ConstantInt::get(
                i32(), static_cast<std::uint64_t>(parseCharLiteral(text)),
                /*isSigned=*/false);
        case sema::PrimitiveType::Float:
        case sema::PrimitiveType::Double: {
            std::string digits;
            for (char c : text) {
                if (c == '_' || c == 'f' || c == 'F' || c == 'd' || c == 'D')
                    continue;
                digits.push_back(c);
            }
            double v = digits.empty() ? 0.0 : std::strtod(digits.c_str(), nullptr);
            if (prim == sema::PrimitiveType::Float)
                return llvm::ConstantFP::get(llvm::Type::getFloatTy(llvm_),
                                             static_cast<float>(v));
            return llvm::ConstantFP::get(llvm::Type::getDoubleTy(llvm_), v);
        }
        case sema::PrimitiveType::String:
            // A string-literal const value is rendered with its surrounding
            // quotes by renderConstExpr; lowerStringLiteralConst takes the inner
            // text, so strip the delimiters when present.
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                return lowerStringLiteralConst(
                    std::string_view(text).substr(1, text.size() - 2), false);
            return lowerStringLiteralConst(text, false);
        default:
            // All integer primitives (§4): width comes from the const's type.
            break;
    }
    // Only fold an actual integer LITERAL. §22.12 permits a const initializer to
    // reference another const/enum, which the metadata renders BY NAME (not as a
    // digit string); folding that as an integer would silently produce 0.
    // Decline instead (CLAUDE.md decline-with-reasons): returning null leaves
    // the read unlowered — a loud failure — rather than an unsound value. The
    // repro and the common case are literal consts, which are covered.
    const char c0 = text.empty() ? '\0' : text.front();
    if (!((c0 >= '0' && c0 <= '9') || c0 == '+' || c0 == '-')) return nullptr;
    llvm::Type* ty = rt && rt->kind == sema::TypeRef::Kind::Primitive
                         ? lowerPrimitive(rt->primitive)
                         : i32();
    return llvm::ConstantInt::get(
        ty, static_cast<std::uint64_t>(parseIntLiteral(text)),
        /*isSigned=*/true);
}


void Emitter::declareStatics() {
    for (const auto& sym : ctx_.symbols()) {
        auto* fs = dynamic_cast<const sema::FieldSymbol*>(sym.get());
        if (!fs || !isStaticField(fs)) continue;
        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
        llvm::Type* ty = fd ? lowerType(fd->type.get()) : i32();
        std::string cls = fs->parent ? std::string(fs->parent->name) : "";
        // §2.5 / §2.6 step 4 (issue #328 FINDING [3] BUG B): a static field's
        // storage global is part of the module's exported, externally-linkable
        // surface — an importer references it by its mangled name. Emit it with
        // EXTERNAL linkage so the symbol is global (not module-local `d`), so a
        // cross-module read binds to THIS definition at link time. The
        // definition (with its initializer) lives in the exporting module only.
        auto* gv = new llvm::GlobalVariable(
            *module_, ty, /*isConstant=*/false,
            llvm::GlobalValue::ExternalLinkage,
            constInitFor(fd, ty),
            "bz$" + cls + "$" + std::string(fs->name));
        staticGlobals_[fs] = gv;
    }

    // §2.5 / §2.6 step 4 (issue #328 FINDING [3] BUG B): declare each IMPORTED
    // static field as an EXTERNAL global (declaration, no initializer) bound to
    // the dependency's exact mangled name (`bz$<Class>$<field>`, matching the
    // exporter above), so a cross-module read LOADS from the linked definition
    // rather than a fresh zero copy. This is the storage analogue of
    // declareImportedFunctions for methods; like it, it engages only when
    // imports were loaded, leaving the single-module path untouched.
    if (!ctx_.hasImports()) return;
    for (sema::Symbol* sym : ctx_.importedSymbols()) {
        auto* fs = dynamic_cast<sema::FieldSymbol*>(sym);
        if (!fs || !isStaticField(fs) || staticGlobals_.count(fs)) continue;
        sema::TypeRef* tr = ctx_.importedMemberType(fs);
        llvm::Type* ty = tr ? lowerTypeRef(tr) : i32();
        std::string cls = fs->parent ? std::string(fs->parent->name) : "";
        auto* gv = new llvm::GlobalVariable(
            *module_, ty, /*isConstant=*/false,
            llvm::GlobalValue::ExternalLinkage,
            /*Initializer=*/nullptr,  // external declaration: no definition here
            "bz$" + cls + "$" + std::string(fs->name));
        staticGlobals_[fs] = gv;
    }
}


llvm::Constant* Emitter::constInitFor(const ast::FieldDecl* fd, llvm::Type* ty) {
    if (fd && fd->initializer) {
        const ast::Expr* e = fd->initializer.get();
        if (auto* lit = dynamic_cast<const ast::IntLiteralExpr*>(e)) {
            return llvm::ConstantInt::get(
                ty, static_cast<std::uint64_t>(parseIntLiteral(lit->text)),
                /*isSigned=*/true);
        }
        if (auto* b = dynamic_cast<const ast::BoolLiteralExpr*>(e)) {
            return llvm::ConstantInt::get(ty, b->value ? 1 : 0);
        }
        // CG24: float/double literal initializer for static float fields.
        if (auto* fl = dynamic_cast<const ast::FloatLiteralExpr*>(e)) {
            std::string digits;
            for (char c : fl->text) {
                if (c == '_' || c == 'f' || c == 'F' || c == 'd' ||
                    c == 'D') continue;
                digits.push_back(c);
            }
            double v = digits.empty() ? 0.0 : std::strtod(digits.c_str(), nullptr);
            if (ty->isFloatTy())
                return llvm::ConstantFP::get(llvm::Type::getFloatTy(llvm_), (float)v);
            return llvm::ConstantFP::get(llvm::Type::getDoubleTy(llvm_), v);
        }
        // CG27a: string literal initializer for static string fields.
        if (auto* sl = dynamic_cast<const ast::StringLiteralExpr*>(e)) {
            // lowerStringLiteral creates globals and returns a GV*.
            // Since we're in declareStatics (before emitMethod bodies),
            // the builder's insert point is not set — set it temporarily
            // to avoid assertion errors in CreateStore etc. The string
            // literal only creates globals (no instruction emission needed).
            if (!currentFn_) {
                // Create a temporary function context so lowerStringLiteral
                // can use builder_ safely (though it doesn't emit instructions).
                auto* strGV = lowerStringLiteralConst(sl->text, false);
                return strGV;
            }
            auto* strGV = lowerStringLiteralConst(sl->text, false);
            return strGV;
        }
        if (auto* vs = dynamic_cast<const ast::VerbatimStringExpr*>(e)) {
            auto* strGV = lowerStringLiteralConst(vs->text, true);
            return strGV;
        }
    }
    return llvm::Constant::getNullValue(ty);
}


llvm::Constant* Emitter::lowerStringLiteralConst(std::string_view text, bool verbatim) {
    auto cps = processStringCodePoints(text, verbatim);
    uint8_t width = codePointsWidth(cps);
    llvm::Constant* dataArr = makeCodePointArray(cps, width);
    unsigned id = stringLiteralIdx_++;
    auto* dataGV = new llvm::GlobalVariable(
        *module_, dataArr->getType(), /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, dataArr,
        ".bz.str.data." + std::to_string(id));
    dataGV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    dataGV->setAlignment(llvm::Align(width));
    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
    llvm::Constant* lenC = llvm::ConstantInt::get(i64(), cps.size());
    llvm::Constant* widC = llvm::ConstantInt::get(i8Ty, width);
    // #422: constant global literal — SENTINEL rc (never retained/freed).
    llvm::Constant* strVal = llvm::ConstantStruct::get(
        stringType(), {dataGV, lenC, widC, stringRcSentinel()});
    auto* strGV = new llvm::GlobalVariable(
        *module_, stringType(), /*isConstant=*/true,
        llvm::GlobalValue::PrivateLinkage, strVal,
        ".bz.str." + std::to_string(id));
    return strGV;  // Constant pointer to the bz_string struct
}


llvm::GlobalVariable* Emitter::staticFieldGlobal(const ast::Expr& e) {
    // A static field reached either bare (`Count`) or qualified
    // (`Type.Count`, a member access) resolves to the same backing
    // global. Match on the field symbol; only static fields are in
    // staticGlobals_, so an instance field's FieldSymbol misses here
    // (and is handled by fieldAddr instead).
    auto* fs = dynamic_cast<sema::FieldSymbol*>(ctx_.exprSymbol(e));
    if (!fs) return nullptr;
    auto it = staticGlobals_.find(fs);
    return it == staticGlobals_.end() ? nullptr : it->second;
}


void Emitter::declareStructs() {
    for (const auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (ts && ts->kind == ast::TypeDeclKind::Class) classStruct(ts);
    }
}


void Emitter::declareFunctions() {
    for (const auto& sym : ctx_.symbols()) {
        auto* ms = dynamic_cast<sema::MethodSymbol*>(sym.get());
        if (!ms) continue;
        CallableInfo ci;
        if (!callableInfo(ms, ci) ||
            ci.bodyKind == ast::MethodBodyKind::Abstract)
            continue;

        llvm::Type* retTy = isMain(ms) ? i32()
                                       : lowerType(ci.returnType);
        std::vector<llvm::Type*> paramTys;
        if (!isStaticMethod(ms)) {
            // CG30: enum instance methods take the enum value (i32) as
            // `this`, not a pointer — enums are value types.
            bool isEnumMethod = ms->parent &&
                ms->parent->kind == ast::TypeDeclKind::Enum;
            paramTys.push_back(isEnumMethod ? i32() : ptrTy());
        }
        for (const auto& p : *ci.params) {
            paramTys.push_back(isByRefMod(p.modifier)
                                   ? ptrTy()  // CG50: ref/out by reference
                                   : lowerType(p.type.get()));
        }
        auto* fnType = llvm::FunctionType::get(retTy, paramTys,
                                               /*isVarArg=*/false);
        auto* fn = llvm::Function::Create(
            fnType, llvm::Function::ExternalLinkage, functionName(ms),
            module_.get());
        methodFns_[ms] = fn;
    }

    // CG10: declare each instance constructor — `void(this, params...)`.
    for (const auto& sym : ctx_.symbols()) {
        auto* cs = dynamic_cast<sema::ConstructorSymbol*>(sym.get());
        if (!cs || cs->isStatic) continue;
        auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
        if (!cd) continue;
        std::vector<llvm::Type*> paramTys;
        paramTys.push_back(ptrTy());  // this
        for (const auto& p : cd->params) {
            paramTys.push_back(lowerType(p.type.get()));
        }
        auto* fnType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(llvm_), paramTys, /*isVarArg=*/false);
        std::string cls = cs->parent ? std::string(cs->parent->name) : "";
        auto* fn = llvm::Function::Create(
            fnType, llvm::Function::ExternalLinkage, "bz$" + cls + "$ctor",
            module_.get());
        ctorFns_[cs] = fn;
    }

    // CG47: declare static constructors (§22.7) as `void()` functions.
    for (const auto& sym : ctx_.symbols()) {
        auto* cs = dynamic_cast<sema::ConstructorSymbol*>(sym.get());
        if (!cs || !cs->isStatic) continue;
        if (!dynamic_cast<const ast::ConstructorDecl*>(cs->decl)) continue;
        std::string cls = cs->parent ? std::string(cs->parent->name) : "";
        auto* fnType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(llvm_), {}, /*isVarArg=*/false);
        auto* fn = llvm::Function::Create(
            fnType, llvm::Function::ExternalLinkage, "bz$" + cls + "$cctor",
            module_.get());
        staticCtors_.push_back({cs, fn});
    }

    // CG13 / §28.2: declare, for every class, the public idempotent Dispose
    // `void(this)` (vtable slot 1) AND an unguarded "DisposeCore" that runs the
    // destructor + owned-field auto-disposal + base-core chain. The public
    // Dispose flag-guards a single call to its core (run-once, §28.6); the base
    // chain (§28.4 step 3) targets the base CORE so the per-object flag — set
    // once at the most-derived public entry — never short-circuits a base
    // level's destructor.
    for (const auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (!ts || ts->kind != ast::TypeDeclKind::Class) continue;
        auto* fnType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(llvm_), {ptrTy()}, /*isVarArg=*/false);
        auto* fn = llvm::Function::Create(
            fnType, llvm::Function::ExternalLinkage,
            "bz$" + std::string(ts->name) + "$Dispose", module_.get());
        disposerFns_[ts] = fn;
        auto* coreFn = llvm::Function::Create(
            fnType, llvm::Function::InternalLinkage,
            "bz$" + std::string(ts->name) + "$DisposeCore", module_.get());
        disposerCoreFns_[ts] = coreFn;
    }

    // §2.5 / §2.6 step 4 (issue #328 inc 3): declare each IMPORTED method as an
    // EXTERNAL function (declare, not define) so a cross-module call resolves
    // against the dependency's object at link time. The mangled name matches
    // ModuleMetadataBuilder's emitter (functionName: `bz$<Class>$<method>`), so
    // B's call to A.f binds to A's emitted symbol. Imported symbols live in the
    // SemaContext's IMPORTED list (not `ctx_.symbols()`), so the loops above —
    // which both declare AND emit bodies — never touched them; this is the only
    // codegen path that engages imports, and only when imports were loaded.
    if (ctx_.hasImports()) declareImportedFunctions();
}

// §2.5 (issue #328 inc 3): see the call site in declareFunctions(). One external
// `llvm::Function` per imported method, registered in methodFns_ so the call
// path (directCall) finds it. The body-emit loop iterates methodFns_ but
// emitFunctionBody early-returns for an imported method (callableInfo fails: no
// ast::MethodDecl), so no body is ever emitted for an external.
void Emitter::declareImportedFunctions() {
    for (sema::Symbol* sym : ctx_.importedSymbols()) {
        auto* ms = dynamic_cast<sema::MethodSymbol*>(sym);
        if (!ms || methodFns_.count(ms)) continue;
        // Reconstruct the function type from the imported signature. Static
        // methods take no implicit receiver; instance methods take a `this`
        // pointer as the leading parameter (matching declareFunctions above).
        sema::TypeRef* retRef = ctx_.importedReturnType(ms);
        llvm::Type* retTy = retRef ? lowerTypeRef(retRef)
                                   : llvm::Type::getVoidTy(llvm_);
        std::vector<llvm::Type*> paramTys;
        if (!isStaticMethod(ms)) paramTys.push_back(ptrTy());
        for (sema::Symbol* p : ms->params) {
            auto* ps = dynamic_cast<sema::ParameterSymbol*>(p);
            sema::TypeRef* pt = ps ? ps->resolvedDeclType : nullptr;
            paramTys.push_back(isByRefMod(ps ? ps->modifier : std::string_view())
                                   ? ptrTy()
                                   : (pt ? lowerTypeRef(pt) : i32()));
        }
        auto* fnType = llvm::FunctionType::get(retTy, paramTys,
                                               /*isVarArg=*/false);
        auto* fn = llvm::Function::Create(
            fnType, llvm::Function::ExternalLinkage, functionName(ms),
            module_.get());
        methodFns_[ms] = fn;
    }
}

// §2.5 / §2.6 step 4 / §22.7 (issue #328 FINDING [7]): declare an IMPORTED
// constructor as an EXTERNAL `void(ptr this, params...)` function so a
// cross-module `new ImportedT(args)` binds to the dependency's emitted
// `bz$<Type>$ctor` at link time. The signature mirrors declareFunctions' own
// ctor declaration above (this pointer first, then a parameter per ctor param,
// by-ref params passed as pointers); the parameter types come from the imported
// ctor's reconstructed ParameterSymbols (resolvedDeclType). Cached in ctorFns_
// so repeated `new`s of the same type reuse one declaration. Engages only on the
// imported path (lowerNew gates on ctx_.isImported), so the single-module ctor
// declaration loop is untouched.
llvm::Function* Emitter::declareImportedCtor(const sema::ConstructorSymbol* cs) {
    auto it = ctorFns_.find(const_cast<sema::ConstructorSymbol*>(cs));
    if (it != ctorFns_.end()) return it->second;
    std::vector<llvm::Type*> paramTys;
    paramTys.push_back(ptrTy());  // this
    for (sema::Symbol* p : cs->params) {
        auto* ps = dynamic_cast<sema::ParameterSymbol*>(p);
        sema::TypeRef* pt = ps ? ps->resolvedDeclType : nullptr;
        paramTys.push_back(isByRefMod(ps ? ps->modifier : std::string_view())
                               ? ptrTy()
                               : (pt ? lowerTypeRef(pt) : i32()));
    }
    auto* fnType = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                           paramTys, /*isVarArg=*/false);
    std::string cls = cs->parent ? std::string(cs->parent->name) : "";
    auto* fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage,
                                      "bz$" + cls + "$ctor", module_.get());
    ctorFns_[const_cast<sema::ConstructorSymbol*>(cs)] = fn;
    return fn;
}


void Emitter::buildVtable(const sema::TypeSymbol* t) {
    if (vtableMethods_.count(t)) return;
    std::vector<sema::MethodSymbol*> slots;
    std::unordered_map<std::string_view, unsigned> nameSlot;
    if (const sema::TypeSymbol* base = baseClassOf(t)) {
        buildVtable(base);
        slots = vtableMethods_[base];
        nameSlot = vtableNameSlot_[base];
    }
    // Vtable: [0]=descriptor, [1]=Dispose, [2..2+G)=interface-method
    // slots, [2+G..]=virtual methods. So a virtual method's slot is its
    // position in `slots` + (2 + G).
    const unsigned base = 2 + ifaceSlotCount_;
    for (sema::Symbol* m : t->members) {
        auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
        if (!ms || isStaticMethod(ms) || !isVirtual(ms)) continue;
        auto it = nameSlot.find(ms->name);
        if (it != nameSlot.end()) {       // override: reuse base slot
            slots[it->second - base] = ms;
            vtableSlot_[ms] = it->second;
        } else {                          // new virtual: fresh slot
            unsigned slot = static_cast<unsigned>(slots.size()) + base;
            slots.push_back(ms);
            nameSlot[ms->name] = slot;
            vtableSlot_[ms] = slot;
        }
    }
    vtableMethods_[t] = std::move(slots);
    vtableNameSlot_[t] = std::move(nameSlot);
}


void Emitter::declareVtables() {
    // Pass 0 (CG35): assign each interface method a fixed vtable slot in
    // [2, 2+G), shared across all implementers.
    for (const auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (!ts || ts->kind != ast::TypeDeclKind::Interface) continue;
        for (sema::Symbol* m : ts->members) {
            auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
            if (ms && !isStaticMethod(ms))
                ifaceMethodSlot_[ms] = 2 + ifaceSlotCount_++;
        }
    }
    // Pass A: assign vtable slots for all classes (base-first).
    for (const auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (ts && ts->kind == ast::TypeDeclKind::Class) buildVtable(ts);
    }
    // Pass B: declare a descriptor-array global (correctly sized, no
    // initializer yet) for every class/interface, so descriptors can
    // cross-reference each other in pass C.
    std::unordered_map<const sema::TypeSymbol*,
                       std::vector<const sema::TypeSymbol*>> supersOf;
    for (const auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (!ts) continue;
        if (ts->kind != ast::TypeDeclKind::Class &&
            ts->kind != ast::TypeDeclKind::Interface)
            continue;
        std::vector<const sema::TypeSymbol*> supers;
        collectTypeAndBases(ts, supers);
        supersOf[ts] = supers;
        auto* arrTy = llvm::ArrayType::get(ptrTy(), supers.size() + 1);
        descArray_[ts] = new llvm::GlobalVariable(
            *module_, arrTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage, nullptr,
            "td$" + std::string(ts->name));
    }
    // Pass C: fill each descriptor with [self + transitive bases, null].
    for (auto& [ts, supers] : supersOf) {
        std::vector<llvm::Constant*> elems;
        for (const sema::TypeSymbol* s : supers) elems.push_back(descArray_[s]);
        elems.push_back(llvm::ConstantPointerNull::get(ptrTy()));
        auto* arrTy = llvm::cast<llvm::ArrayType>(
            descArray_[ts]->getValueType());
        descArray_[ts]->setInitializer(
            llvm::ConstantArray::get(arrTy, elems));
    }
    // Pass D: build each class's vtable global.
    for (const auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (!ts || ts->kind != ast::TypeDeclKind::Class) continue;
        const auto& slots = vtableMethods_[ts];
        // [0] descriptor, [1] Dispose, [2..2+G) interface slots,
        // [2+G..] virtual methods.
        std::vector<llvm::Constant*> elems(2 + ifaceSlotCount_ +
                                               slots.size(),
                                           llvm::ConstantPointerNull::get(
                                               ptrTy()));
        elems[0] = descArray_[ts];
        auto dit = disposerFns_.find(ts);
        if (dit != disposerFns_.end())
            elems[1] = llvm::cast<llvm::Constant>(dit->second);
        // CG35: fill the interface-method slots this class implements.
        std::vector<const sema::TypeSymbol*> supers;
        collectTypeAndBases(ts, supers);
        std::unordered_set<const sema::TypeSymbol*> superSet(supers.begin(),
                                                            supers.end());
        for (const auto& [im, slot] : ifaceMethodSlot_) {
            if (!im->parent || !superSet.count(im->parent)) continue;
            if (sema::MethodSymbol* impl =
                    findMethodByName(ts, im->name)) {
                auto mit = methodFns_.find(impl);
                if (mit != methodFns_.end())
                    elems[slot] = llvm::cast<llvm::Constant>(mit->second);
            }
        }
        // Virtual methods at [2+G..].
        for (std::size_t i = 0; i < slots.size(); ++i) {
            auto mit = methodFns_.find(slots[i]);
            if (mit != methodFns_.end())
                elems[2 + ifaceSlotCount_ + i] =
                    llvm::cast<llvm::Constant>(mit->second);
        }
        auto* arrTy = llvm::ArrayType::get(ptrTy(), elems.size());
        auto* init = llvm::ConstantArray::get(arrTy, elems);
        vtables_[ts] = new llvm::GlobalVariable(
            *module_, arrTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage, init,
            "vt$" + std::string(ts->name));
    }
}


void Emitter::collectTypeAndBases(const sema::TypeSymbol* ts,
                         std::vector<const sema::TypeSymbol*>& out) {
    std::vector<const sema::TypeSymbol*> frontier{ts};
    std::unordered_set<const sema::TypeSymbol*> seen{ts};
    std::size_t i = 0;
    while (i < frontier.size()) {
        const sema::TypeSymbol* t = frontier[i++];
        out.push_back(t);
        for (const sema::TypeSymbol* b : t->baseTypes) {
            if (b && seen.insert(b).second) frontier.push_back(b);
        }
    }
}


llvm::GlobalVariable* Emitter::vtableOf(const sema::TypeSymbol* t) {
    auto it = vtables_.find(t);
    return it == vtables_.end() ? nullptr : it->second;
}


const std::vector<std::pair<const char*, const char*>>&
Emitter::builtinExceptionHierarchy() {
    static const std::vector<std::pair<const char*, const char*>> h = {
        {"Exception", ""},
        {"ArgumentException", "Exception"},
        {"ArgumentNullException", "ArgumentException"},
        {"ArithmeticException", "Exception"},
        {"OverflowException", "ArithmeticException"},
        {"DivideByZeroException", "ArithmeticException"},
        {"InvalidOperationException", "Exception"},
        {"NotImplementedException", "Exception"},
        {"NullReferenceException", "Exception"},
        {"IndexOutOfRangeException", "Exception"},
        // issue #388 / stdlib §4.8: Dictionary<K,V>'s indexer getter raises
        // KeyNotFoundException for an absent key. It is a stdlib type (in
        // System.Collections.Generic), but the codegen synthesizes it at the
        // indexer-read guard, so it joins the set of runtime-throwable
        // exceptions here (and so a `catch KeyNotFoundException` resolves).
        {"KeyNotFoundException", "Exception"},
        {"MatchFailureException", "Exception"},
        {"OperationCanceledException", "Exception"},
        {"TypeInitializationException", "Exception"},
        {"FormatException", "Exception"},
        {"StackOverflowException", "Exception"},
    };
    return h;
}


void Emitter::setupBuiltinExceptions() {
    const auto& hier = builtinExceptionHierarchy();
    std::unordered_map<std::string, std::string> parent;
    for (const auto& [name, par] : hier) parent[name] = par;

    // Shared no-op Dispose (built-in exceptions own nothing).
    auto* dispTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                           {ptrTy()}, false);
    builtinDisposeFn_ = llvm::Function::Create(
        dispTy, llvm::Function::InternalLinkage, "bz$exDispose",
        module_.get());
    {
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry",
                                            builtinDisposeFn_);
        llvm::IRBuilder<> b(bb);
        b.CreateRetVoid();
    }

    // Pass 1: declare descriptor globals (sized by ancestor-chain len).
    std::unordered_map<std::string, std::vector<std::string>> chainOf;
    for (const auto& [name, par] : hier) {
        std::vector<std::string> chain;
        for (std::string c = name; !c.empty(); c = parent[c])
            chain.push_back(c);
        chainOf[name] = chain;
        auto* arrTy = llvm::ArrayType::get(ptrTy(), chain.size() + 1);
        builtinDesc_[name] = new llvm::GlobalVariable(
            *module_, arrTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage, nullptr,
            std::string("btd$") + name);
    }
    // Pass 2: fill descriptors [self..Exception, null].
    for (const auto& [name, chain] : chainOf) {
        std::vector<llvm::Constant*> elems;
        for (const std::string& c : chain) elems.push_back(builtinDesc_[c]);
        elems.push_back(llvm::ConstantPointerNull::get(ptrTy()));
        auto* arrTy = llvm::cast<llvm::ArrayType>(
            builtinDesc_[name]->getValueType());
        builtinDesc_[name]->setInitializer(
            llvm::ConstantArray::get(arrTy, elems));
    }
    // Pass 3: vtable per built-in = [descriptor, Dispose].
    for (const auto& [name, par] : hier) {
        std::vector<llvm::Constant*> v = {builtinDesc_[name],
                                          builtinDisposeFn_};
        auto* arrTy = llvm::ArrayType::get(ptrTy(), v.size());
        builtinVtable_[name] = new llvm::GlobalVariable(
            *module_, arrTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(arrTy, v),
            std::string("bvt$") + name);
    }
}


llvm::GlobalVariable* Emitter::descriptorForTypeName(const sema::TypeRef* rt,
                                            const ast::Type* tyNode) {
    if (rt && rt->declaration) {
        auto it = descArray_.find(rt->declaration);
        if (it != descArray_.end()) return it->second;
    }
    if (tyNode && tyNode->name && !tyNode->name->parts.empty()) {
        auto it = builtinDesc_.find(
            std::string(tyNode->name->parts.back()));
        if (it != builtinDesc_.end()) return it->second;
    }
    return nullptr;
}


void Emitter::emitThrowBuiltin(const char* name) {
    auto vit = builtinVtable_.find(name);
    if (vit == builtinVtable_.end()) return;
    // 16-byte object: slot0=vtable, slot1=empty message ptr (calloc zeros it).
    llvm::Value* obj = builder_.CreateCall(
        callocFn(), {builder_.getInt64(1), builder_.getInt64(16)});
    builder_.CreateStore(vit->second, obj);
    builder_.CreateStore(obj, excGlobal());
    emitUnwindDisposals();
    builder_.CreateBr(unwindBB());
}


bool Emitter::exprReferencesName(const ast::Expr& e,
                                 std::string_view name) const {
    if (name.empty()) return false;
    // Leaves that obviously cannot name a local.
    if (dynamic_cast<const ast::IntLiteralExpr*>(&e) ||
        dynamic_cast<const ast::FloatLiteralExpr*>(&e) ||
        dynamic_cast<const ast::CharLiteralExpr*>(&e) ||
        dynamic_cast<const ast::StringLiteralExpr*>(&e) ||
        dynamic_cast<const ast::VerbatimStringExpr*>(&e) ||
        dynamic_cast<const ast::BoolLiteralExpr*>(&e) ||
        dynamic_cast<const ast::NullLiteralExpr*>(&e) ||
        dynamic_cast<const ast::UnitExpr*>(&e) ||
        dynamic_cast<const ast::TypeOfExpr*>(&e) ||
        dynamic_cast<const ast::DefaultExpr*>(&e))
        return false;
    // `nameof(X)` names a symbol but does not read it at runtime; even if X is
    // the caught binding, no live reference to the object is produced — but to
    // stay conservative (and because it is a rare arm-body shape), treat any
    // textual match as a reference: NameOfExpr falls through to the fail-safe.
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e))
        return id->name == name;
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e))
        return ma->target && exprReferencesName(*ma->target, name);
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(&e))
        return nm->target && exprReferencesName(*nm->target, name);
    if (auto* bang = dynamic_cast<const ast::PostfixBangExpr*>(&e))
        return bang->operand && exprReferencesName(*bang->operand, name);
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e))
        return un->operand && exprReferencesName(*un->operand, name);
    if (auto* tt = dynamic_cast<const ast::TypeTestExpr*>(&e))
        return tt->operand && exprReferencesName(*tt->operand, name);
    if (auto* th = dynamic_cast<const ast::ThrowExpr*>(&e))
        return th->operand && exprReferencesName(*th->operand, name);
    if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(&e))
        return (bin->left  && exprReferencesName(*bin->left,  name)) ||
               (bin->right && exprReferencesName(*bin->right, name));
    if (auto* cond = dynamic_cast<const ast::ConditionalExpr*>(&e))
        return (cond->thenValue && exprReferencesName(*cond->thenValue, name)) ||
               (cond->condition && exprReferencesName(*cond->condition, name)) ||
               (cond->elseValue && exprReferencesName(*cond->elseValue, name));
    if (auto* rg = dynamic_cast<const ast::RangeExpr*>(&e))
        return (rg->lo   && exprReferencesName(*rg->lo,   name)) ||
               (rg->hi   && exprReferencesName(*rg->hi,   name)) ||
               (rg->step && exprReferencesName(*rg->step, name));
    auto argsRef = [&](const std::vector<ast::CallArgument>& args) {
        for (const auto& a : args)
            if (a.value && exprReferencesName(*a.value, name)) return true;
        return false;
    };
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e))
        return argsRef(ne->args);
    if (auto* call = dynamic_cast<const ast::CallExpr*>(&e))
        return (call->callee && exprReferencesName(*call->callee, name)) ||
               argsRef(call->args);
    if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e))
        return (nc->callee && exprReferencesName(*nc->callee, name)) ||
               argsRef(nc->args);
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e))
        return (ix->target && exprReferencesName(*ix->target, name)) ||
               argsRef(ix->indices);
    // Any expression shape not handled above (interpolated string, `with`,
    // collection literal, lambda, case, …) conservatively counts as "might
    // reference": the caught exception is then NOT freed, accepting the
    // residual leak rather than risking a use-after-free.
    return true;
}


void Emitter::emitThrowValue(llvm::Value* excVal, const ast::Expr* excAst) {
    if (excVal) {
        // #387 / §31.1: a `throw <newExpr>` leaving a catch arm. The caught
        // exception is a borrow whose lifetime is the arm body; the spec
        // forbids it being retained (assigned to an owned field, returned as
        // owned, captured). Constructing a new exception therefore never
        // transfers ownership of the caught one — so when the thrown
        // expression provably cannot reference the caught binding, the caught
        // object's lifetime is ending with nothing retaining it and it must be
        // freed here (else it leaks; #387). When it MIGHT reference it (it is
        // passed into the new exception, directly or transitively), we keep the
        // status quo and do NOT free — the caught object stays in flight,
        // accepting a single-object leak rather than risking a use-after-free
        // on a possibly-wrapped reference. Bare `throw;` (excVal == the caught
        // object) and re-throw of the same value never free it (it is going
        // back in flight). `excAst` is null for the expression-position throw,
        // which then conservatively never frees.
        if (currentCaughtExc_ && excVal != currentCaughtExc_ && excAst &&
            !exprReferencesName(*excAst, currentCaughtName_)) {
            // #387 / §28.2: the caught object's lifetime is ending here with
            // nothing retaining it — dispatch its idempotent Dispose and reclaim
            // its block once. This is the sole free on this throw path: the
            // caught-exception scope disposal is SKIPPED on unwind (isUnwind).
            emitDisposeAndReclaim(currentCaughtExc_);
        }
        builder_.CreateStore(excVal, excGlobal());
    } else if (currentCaughtExc_) {
        // Bare `throw` re-raises the arm's caught exception (the global
        // was cleared when the arm was entered).
        builder_.CreateStore(currentCaughtExc_, excGlobal());
    }
    emitUnwindDisposals();  // CG21: dispose unwound using-vars
    builder_.CreateBr(unwindBB());
}


void Emitter::emitGuardThrow(llvm::Value* cond, const char* name) {
    auto* errBB = llvm::BasicBlock::Create(llvm_, "chk.err", currentFn_);
    auto* cont = llvm::BasicBlock::Create(llvm_, "chk.cont", currentFn_);
    builder_.CreateCondBr(cond, errBB, cont);
    builder_.SetInsertPoint(errBB);
    emitThrowBuiltin(name);
    builder_.SetInsertPoint(cont);
}


const ast::DestructorDecl* Emitter::findDestructor(const sema::TypeSymbol* ts) {
    for (const sema::Symbol* m : ts->members) {
        if (dynamic_cast<const sema::DestructorSymbol*>(m)) {
            return dynamic_cast<const ast::DestructorDecl*>(m->decl);
        }
    }
    return nullptr;
}


void Emitter::emitDisposerBody(const sema::TypeSymbol* ts, llvm::Function* fn) {
    // §28.2 / §28.4 / §28.6: `fn` is the PUBLIC, idempotent Dispose (vtable slot
    // 1). It guards a single call to the UNGUARDED core (disposerCoreFns_[ts])
    // with the per-object run-once flag, so the destructor body, owned-field
    // auto-disposal and base chain execute AT MOST ONCE per object no matter how
    // many dispose sites reach this Dispose (explicit `.Dispose()`, scope-end
    // auto-dispose, owned-field auto-dispose, exception unwind). The block free
    // is NOT performed here; it is done once by the disposal site that holds the
    // owning slot (emitDisposals / emitOwnedFieldDisposals / the caught-exception
    // and unwind paths), so a re-dispatch never reads an already-freed object.
    llvm::Function* coreFn = disposerCoreFns_[ts];
    emitDisposerCore(ts, coreFn);

    currentFn_ = fn;
    currentThis_ = &*fn->args().begin();
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    disposeStack_.clear();
    loopStack_.clear(); finallyStack_.clear();
    propagateBB_ = nullptr;
    unwindTarget_ = nullptr;
    currentCaughtExc_ = nullptr;
    currentCaughtName_ = {};
    unwindScopeDepth_ = 0;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));
    // §28.2: load the run-once flag; if already set, this is a repeat disposal —
    // a no-op (§28.2: "subsequent calls are no-ops"). Else set it BEFORE running
    // the core so a re-entrant dispose during the core (e.g. a destructor that
    // disposes `this` transitively) still short-circuits.
    llvm::StructType* st = classStruct(ts);
    unsigned fi = disposedFlagIndex(ts);
    llvm::Value* flagAddr =
        builder_.CreateStructGEP(st, currentThis_, fi, "disposed.flag");
    llvm::Value* already = builder_.CreateICmpNE(
        builder_.CreateLoad(disposedFlagTy(), flagAddr),
        llvm::ConstantInt::get(disposedFlagTy(), 0), "already.disposed");
    auto* doBB = llvm::BasicBlock::Create(llvm_, "dispose.do", fn);
    auto* retBB = llvm::BasicBlock::Create(llvm_, "dispose.ret", fn);
    builder_.CreateCondBr(already, retBB, doBB);
    builder_.SetInsertPoint(doBB);
    builder_.CreateStore(llvm::ConstantInt::get(disposedFlagTy(), 1), flagAddr);
    builder_.CreateCall(coreFn, {currentThis_});
    builder_.CreateBr(retBB);
    builder_.SetInsertPoint(retBB);
    builder_.CreateRetVoid();
}


void Emitter::emitDisposerCore(const sema::TypeSymbol* ts, llvm::Function* fn) {
    currentFn_ = fn;
    currentThis_ = &*fn->args().begin();
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    disposeStack_.clear();
    loopStack_.clear(); finallyStack_.clear();
    propagateBB_ = nullptr;
    unwindTarget_ = nullptr;
    currentCaughtExc_ = nullptr;
    currentCaughtName_ = {};
    unwindScopeDepth_ = 0;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));
    bool terminated = false;
    if (const ast::DestructorDecl* dd = findDestructor(ts)) {
        // #383 / §28.8.1: suppress dynamic disposal of `using var u := move
        // this._f;` inside the destructor — the auto field-dispose below would
        // dispose the same (moved-from) field, double-freeing it.
        emittingDisposerBody_ = true;
        terminated = lowerScopedBlock(dd->body);
        emittingDisposerBody_ = false;
    }
    // §28.4: after the destructor body, dispose owned fields (reverse
    // declaration order), then chain to the base class's CORE disposer.
    // (A destructor that returns early skips this — destructors with
    // both an early return and owned fields are a deferred corner.)
    if (!terminated) {
        emitOwnedFieldDisposals(ts);
        emitBaseDisposeChain(ts);
        builder_.CreateRetVoid();
    }
}


void Emitter::emitOwnedFieldDisposals(const sema::TypeSymbol* ts) {
    // Each owned field carries either a class/interface object (disposed via
    // its vtable) or — #362 / stdlib §4.5 — a heap collection (freed via
    // bz_array_free, cascading into class elements for an owning collection).
    struct OwnedField {
        const sema::FieldSymbol* fs;
        const sema::TypeRef* collTy;  // non-null → collection field
    };
    std::vector<OwnedField> owned;
    for (const sema::Symbol* m : ts->members) {
        auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
        if (!fs) continue;
        if (fieldIndex_.find(fs) == fieldIndex_.end()) continue;
        if (isOwnedReferenceField(fs)) {
            owned.push_back({fs, nullptr});
        } else if (isOwnedBuiltinExceptionField(fs)) {
            // #405 / §28.5 / §31.1: an `owned` field of BUILTIN exception type.
            // It has no user TypeSymbol (so isOwnedReferenceField rejects it),
            // but it is an ordinary owned heap class instance and is disposed
            // through the same dynamic-dispatch path as a class field below
            // (vtable slot 1 is the shared no-op bz$exDispose; the block is
            // reclaimed, the runtime-managed message string is not freed).
            owned.push_back({fs, nullptr});
        } else if (const sema::TypeRef* ct = ownedCollectionFieldType(fs)) {
            owned.push_back({fs, ct});
        }
    }
    for (auto it = owned.rbegin(); it != owned.rend(); ++it) {
        llvm::StructType* st = classStruct(ts);
        unsigned idx = fieldIndex_[it->fs];
        llvm::Value* addr = builder_.CreateStructGEP(st, currentThis_, idx);
        llvm::Value* val = builder_.CreateLoad(ptrTy(), addr);
        // #362 / stdlib §4.5: an owned collection field frees its backing and
        // cascades disposal into its class elements (for an owning collection).
        // bz_array_free is null-safe, so no field null-check is needed here.
        if (it->collTy) {
            llvm::Function* elemDisp = owningCollectionElemDisposer(it->collTy);
            llvm::Value* disp = elemDisp
                ? llvm::cast<llvm::Value>(elemDisp)
                : llvm::ConstantPointerNull::get(ptrTy());
            auto* fn = declareC("bz_array_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {val, disp});
            continue;
        }
        llvm::Value* isNull = builder_.CreateICmpEQ(
            val, llvm::ConstantPointerNull::get(ptrTy()));
        auto* dispBB = llvm::BasicBlock::Create(llvm_, "owned.dispose",
                                                currentFn_);
        auto* contBB = llvm::BasicBlock::Create(llvm_, "owned.cont",
                                                currentFn_);
        builder_.CreateCondBr(isNull, contBB, dispBB);
        builder_.SetInsertPoint(dispBB);
        // §28.4 step 2: dispatch the field object's idempotent Dispose and
        // reclaim its block once, then null the field. §28.5 (line 5066): a
        // destructor body may explicitly `field.Dispose()` BEFORE this auto-
        // dispose — that explicit call only dispatched the field's idempotent
        // Dispose (it did not free), so reclaiming here frees the field exactly
        // once (no double-free). Nulling the field after makes any further
        // disposal a guarded no-op (the #396 slot-null precedent), so a second
        // §28.4-step-2 pass (or a re-entrant dispose) finds null and skips.
        emitDisposeAndReclaim(val);
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(contBB);
    }
}


llvm::GlobalVariable* Emitter::excGlobal() {
    if (!excGlobal_) {
        excGlobal_ = new llvm::GlobalVariable(
            *module_, ptrTy(), /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantPointerNull::get(ptrTy()), "bz$exc");
    }
    return excGlobal_;
}


llvm::BasicBlock* Emitter::propagateBB() {
    if (!propagateBB_) {
        auto* save = builder_.GetInsertBlock();
        propagateBB_ = llvm::BasicBlock::Create(llvm_, "propagate",
                                                currentFn_);
        builder_.SetInsertPoint(propagateBB_);
        // per §33.3 / §22.13 (line 3845): propagateBB is the per-function
        // fallback when an exception reaches a frame with no enclosing handler
        // (unwindTarget_ unset). For an inner frame the exception is still live
        // in bz$exc, so propagating == returning (emitImplicitReturn leaves
        // bz$exc set and the caller's emitPendingCheck continues the unwind).
        // For `main` (the entry point — the only function named "main", §33.2),
        // this block IS the process boundary: the exception is genuinely
        // uncaught. §33.3 reserves exit code 0 for NORMAL completion (and a
        // legitimate `return 0`); §22.13 distinguishes "Main returning normally"
        // from "Main returning by an uncaught exception that the runtime
        // intercepts". Emitting `ret i32 0` here (the old emitImplicitReturn for
        // an i32 function) conflated the two — every uncaught exception reported
        // success. Instead, hand off to the runtime's abnormal-termination
        // interceptor, which writes to stderr and exits non-zero. This fires
        // ONLY for main's no-handler boundary: inner frames and every
        // caught/handled path (unwindTarget_ set → not propagateBB) are
        // untouched, so finally + owned-local unwind disposal still run.
        if (currentFn_->getName() == "main") {
            auto* fn = declareC("bz_unhandled_exception",
                                llvm::Type::getVoidTy(llvm_), {});
            fn->setDoesNotReturn();
            builder_.CreateCall(fn, {});
            builder_.CreateUnreachable();
        } else {
            emitImplicitReturn(currentFn_);
        }
        if (save) builder_.SetInsertPoint(save);
    }
    return propagateBB_;
}


llvm::BasicBlock* Emitter::unwindBB() {
    return unwindTarget_ ? unwindTarget_ : propagateBB();
}


void Emitter::emitUnwindDisposals() {
    // #383: the unwind path — a `caughtException` entry is skipped (the
    // exception stays in flight; the enclosing handler frees it).
    for (std::size_t i = disposeStack_.size(); i-- > unwindScopeDepth_;)
        emitDisposals(disposeStack_[i], /*isUnwind=*/true);
}


void Emitter::emitPendingCheck() {
    llvm::Value* exc = builder_.CreateLoad(ptrTy(), excGlobal());
    llvm::Value* pending = builder_.CreateICmpNE(
        exc, llvm::ConstantPointerNull::get(ptrTy()));
    auto* unwind =
        llvm::BasicBlock::Create(llvm_, "call.unwind", currentFn_);
    auto* cont = llvm::BasicBlock::Create(llvm_, "call.cont", currentFn_);
    builder_.CreateCondBr(pending, unwind, cont);
    builder_.SetInsertPoint(unwind);
    emitUnwindDisposals();
    builder_.CreateBr(unwindBB());
    builder_.SetInsertPoint(cont);
}


void Emitter::emitDynamicDispose(llvm::Value* obj) {
    // §28.2 / §28.4: dispatch the object's PUBLIC, idempotent Dispose through
    // its vtable (slot 1) — which runs the destructor body, owned-field
    // disposals and the full base chain, AT MOST ONCE per object (the run-once
    // flag, §28.6). This does NOT free the object's heap block.
    //
    // #380 / §28: "dispose" still means run the destructor AND reclaim the block
    // (no GC). But the free is now performed by the disposal SITE (see
    // emitDisposeAndReclaim), not here. Reason: explicit `.Dispose()` (§28.5
    // line 5066) plus the scope-end / owned-field auto-dispose reach Dispose
    // TWICE on one object; if the first dispatch freed the block, the second
    // dispatch's vtable load would read freed memory (use-after-free) regardless
    // of any flag. Splitting "dispatch" (idempotent, repeatable) from "reclaim"
    // (done once, by the owning slot's auto-dispose) keeps repeated disposal
    // safe. vtable layout: [0]=type descriptor, [1]=Dispose, [2+]=methods.
    llvm::Value* vptr = builder_.CreateLoad(ptrTy(), obj);
    llvm::Value* slot = builder_.CreateGEP(ptrTy(), vptr,
                                           {builder_.getInt32(1)});
    llvm::Value* disposeFn = builder_.CreateLoad(ptrTy(), slot);
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    builder_.CreateCall(fnTy, disposeFn, {obj});
}


void Emitter::emitDisposeAndReclaim(llvm::Value* obj) {
    // #380 / §28: an auto-dispose site that OWNS the slot/field holding `obj`
    // (using-var scope-end, owned-field auto-dispose, exception unwind, the
    // caught-exception normal-exit free). It is the LAST dispose of this object
    // — sema guarantees no use-after-dispose, and any earlier explicit
    // `.Dispose()` only dispatched (it did not free). So dispatch the idempotent
    // Dispose (a no-op if already run) and THEN reclaim the block exactly once.
    // The caller is responsible for null-ing its slot afterward where a second
    // dispatch through the same slot is reachable. free(null) is a no-op.
    emitDynamicDispose(obj);
    builder_.CreateCall(freeFn(), {obj});
}


llvm::Function* Emitter::elementDisposerThunk() {
    // #362 / stdlib §4.5: a standalone `void(void*)` thunk that runs an owning
    // collection element's destructor via the element's own vtable (slot 1),
    // skipping a null element. Built once with its own IRBuilder so the active
    // `builder_` insert point is untouched.
    if (elementDisposerThunk_) return elementDisposerThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$elemDispose", module_.get());
    llvm::Argument* obj = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* dispBB = llvm::BasicBlock::Create(llvm_, "elem.dispose", fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "elem.ret", fn);
    llvm::IRBuilder<> b(entry);
    // A null/already-moved slot is safe to skip (no double-dispose / UAF).
    llvm::Value* isNull = b.CreateICmpEQ(
        obj, llvm::ConstantPointerNull::get(ptrTy()));
    b.CreateCondBr(isNull, retBB, dispBB);
    b.SetInsertPoint(dispBB);
    // vtable layout: [0]=descriptor, [1]=Dispose. Dispatch through the element.
    llvm::Value* vptr = b.CreateLoad(ptrTy(), obj);
    llvm::Value* slot = b.CreateGEP(ptrTy(), vptr, {b.getInt32(1)});
    llvm::Value* disposeFn = b.CreateLoad(ptrTy(), slot);
    auto* dispTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                           {ptrTy()}, /*isVarArg=*/false);
    b.CreateCall(dispTy, disposeFn, {obj});
    // #380 / §28: an owning collection's element is a heap class instance; after
    // its (most-derived) Dispose runs the destructor + base chain, reclaim its
    // block. This thunk is the element-dispose path that does NOT route through
    // emitDynamicDispose (it is invoked per live element by bz_array_free), so
    // the free must be emitted here too. obj is non-null in this block.
    b.CreateCall(freeFn(), {obj});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    elementDisposerThunk_ = fn;
    return fn;
}


bool Emitter::collectionElemIsClass(const sema::TypeRef* collTy, unsigned idx) {
    if (!collTy || collTy->genericArgs.size() <= idx) return false;
    const sema::TypeRef* el = collTy->genericArgs[idx];
    if (!el || el->kind != sema::TypeRef::Kind::Named || !el->declaration)
        return false;
    return el->declaration->kind == ast::TypeDeclKind::Class;
}


llvm::Function* Emitter::owningCollectionElemDisposer(
        const sema::TypeRef* collTy) {
    // #362 / stdlib §4.5: only OWNING collections (name prefixed "Owned") with
    // a class element type cascade disposal into their elements. Borrowing
    // collections (Array<T> etc.) and value/string elements return null — only
    // the backing storage is freed.
    if (!collTy || !collTy->astNode || !collTy->astNode->name ||
        collTy->astNode->name->parts.empty())
        return nullptr;
    std::string_view n = collTy->astNode->name->parts[0];
    if (n.rfind("Owned", 0) != 0) return nullptr;  // not an owning collection
    if (!collectionElemIsClass(collTy, 0)) return nullptr;
    return elementDisposerThunk();
}


llvm::Function* Emitter::owningDictValueDisposer(const sema::TypeRef* dictTy) {
    // #394 / stdlib §4.9: only an OwnedDictionary<K,V> with a CLASS value type
    // owns its values and must dispose each surviving one at scope end. A
    // borrowing Dictionary<K,V> (and any value-typed V) returns null — only the
    // backing table and its key bytes are freed. The owned thing is the VALUE
    // (genericArgs[1]), not the key (§4.9: "Owns its values; keys are
    // value-like"); the shared elementDisposerThunk disposes+frees any class
    // object via its own vtable (slot 1), so it applies to the stored value
    // pointer exactly as it does to an OwnedArray element.
    if (!dictTy || !dictTy->astNode || !dictTy->astNode->name ||
        dictTy->astNode->name->parts.empty())
        return nullptr;
    if (dictTy->astNode->name->parts[0] != "OwnedDictionary") return nullptr;
    if (!collectionElemIsClass(dictTy, 1)) return nullptr;
    return elementDisposerThunk();
}


llvm::Function* Emitter::valueBoxFreeThunk() {
    // #401 / stdlib §4.8 + language spec §3.2 / §9: a standalone `void(void*)`
    // thunk that just `free`s the pointer (null-safe), with NO destructor and no
    // vtable dispatch. A record value in a borrowing Dictionary<K, record> is
    // heap-BOXED (see the encode in LlvmBackend_{Calls,Assign}.cpp); a record is
    // a value type (§3.2 "value types ... do not [carry a disposal obligation]";
    // §9 "A record is a value type"), so the box has nothing to dispose inside it
    // — it is just storage to reclaim. Built once with its own IRBuilder so the
    // active `builder_` insert point is untouched (mirrors elementDisposerThunk).
    if (valueBoxFreeThunk_) return valueBoxFreeThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$valBoxFree", module_.get());
    llvm::Argument* box = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* freeBB = llvm::BasicBlock::Create(llvm_, "box.free", fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "box.ret", fn);
    llvm::IRBuilder<> b(entry);
    // A null box (e.g. a moved-from / never-stored slot) is safe to skip.
    llvm::Value* isNull = b.CreateICmpEQ(
        box, llvm::ConstantPointerNull::get(ptrTy()));
    b.CreateCondBr(isNull, retBB, freeBB);
    b.SetInsertPoint(freeBB);
    b.CreateCall(freeFn(), {box});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    valueBoxFreeThunk_ = fn;
    return fn;
}


llvm::Function* Emitter::dictValueBoxFreer(const sema::TypeRef* dictTy) {
    // #401 / stdlib §4.8: only a BORROWING Dictionary<K,V> whose value type V is
    // a RECORD stores a heap box per value (the encode in
    // LlvmBackend_{Calls,Assign}.cpp boxes exactly `valRec && isStructTy()`); its
    // disposal must reclaim each surviving box. An OwnedDictionary<K, class>
    // routes through owningDictValueDisposer instead (#394: dispose+free a class
    // object), and a borrowing Dictionary with a scalar/string value stores no
    // box (the value is inttoptr'd or is the pointer itself) — both return null
    // here, so the value box is never freed twice. The record check mirrors the
    // encode's broad test (any record declaration, equality-bearing or not), NOT
    // the narrower recordTypeOfRef, so it matches every boxed value exactly.
    if (!dictTy || !dictTy->astNode || !dictTy->astNode->name ||
        dictTy->astNode->name->parts.empty())
        return nullptr;
    if (dictTy->astNode->name->parts[0] != "Dictionary") return nullptr;
    if (dictTy->genericArgs.size() <= 1) return nullptr;
    const sema::TypeRef* valTy = dictTy->genericArgs[1];
    bool valIsRecord =
        valTy && valTy->kind == sema::TypeRef::Kind::Named &&
        valTy->declaration &&
        valTy->declaration->kind == ast::TypeDeclKind::Record;
    if (!valIsRecord) return nullptr;
    return valueBoxFreeThunk();
}


void Emitter::emitBaseDisposeChain(const sema::TypeSymbol* ts) {
    // §28.4 step 3: chain to the base class's CORE disposer (unguarded), NOT its
    // public Dispose. The per-object run-once flag (§28.2 / §28.6) is checked
    // and set exactly once at the most-derived public Dispose entry; calling the
    // base's public Dispose here would re-check that same flag (same object) and
    // see it already set, skipping the base destructor + base owned fields. The
    // core runs the base's destructor + owned-field disposal + its own base
    // chain unconditionally, which is correct because the whole chain executes
    // within the single guarded most-derived disposal.
    for (const sema::TypeSymbol* base : ts->baseTypes) {
        if (!base || base->kind != ast::TypeDeclKind::Class) continue;
        auto it = disposerCoreFns_.find(base);
        if (it != disposerCoreFns_.end()) builder_.CreateCall(it->second,
                                                          {currentThis_});
        return;  // single base class
    }
}


void Emitter::emitFunctionBody(sema::MethodSymbol* ms, llvm::Function* fn) {
    CallableInfo ci;
    if (!callableInfo(ms, ci)) return;

    // §29.3: detect iterator methods BEFORE creating the entry block.
    // emitIteratorMethod handles its own entry block creation + bindParameters.
    if (ci.bodyKind == ast::MethodBodyKind::Block && ci.stmts &&
        containsYield(*ci.stmts)) {
        emitIteratorMethod(ms, fn, ci);
        return;
    }

    currentFn_ = fn;
    currentThis_ = nullptr;
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    disposeStack_.clear();
    loopStack_.clear(); finallyStack_.clear();
    propagateBB_ = nullptr;
    unwindTarget_ = nullptr;
    currentCaughtExc_ = nullptr;
    currentCaughtName_ = {};
    unwindScopeDepth_ = 0;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));

    // §28.3 (line 4910): compute which owned locals/params are unwind-disposal
    // ELIGIBLE (never moved / disposed / reassigned) before lowering, so
    // registration can consult unwindIneligible_. Block bodies only — an
    // expression body has no statements that could leak an owned local on
    // unwind, and no dispose scope for owned params (a documented gap).
    unwindIneligible_.clear();
    unwindIneligibleFn_ = fn;
    currentReturnTypeNode_ = ci.returnType;  // §8.6: for return-value boxing
    // #422 / §4.6 / §28.9: which `var` string locals may be released at scope
    // end (mirrors collectUnwindIneligible; consulted in lowerLocalDecl).
    stringReleaseEligible_.clear();
    stringReleaseEligibleFn_ = fn;
    // #422 / §27.5 / §28.9: which `var` closure (function-typed) locals may be
    // released at scope end (mirrors stringReleaseEligible_).
    closureReleaseEligible_.clear();
    closureReleaseEligibleFn_ = fn;
    // #422 / §8.6 / §28.9: which `var` Object/interface locals holding a boxed
    // VALUE may be released at scope end (mirrors stringReleaseEligible_).
    boxReleaseEligible_.clear();
    boxReleaseEligibleFn_ = fn;
    if (ci.bodyKind == ast::MethodBodyKind::Block && ci.stmts) {
        collectUnwindIneligible(*ci.stmts);
        collectStringReleaseEligible(*ci.stmts);
        collectClosureReleaseEligible(*ci.stmts);
        collectBoxReleaseEligible(*ci.stmts);
    }

    bindParameters(fn, /*hasThis=*/!isStaticMethod(ms), &ms->params);

    // CG47: run every static constructor once, before the program's
    // body. The entry point Main is the single root, so calling them at
    // its start runs them before any user code (§22.7).
    if (isMain(ms)) {
        for (auto& [cs, sfn] : staticCtors_) builder_.CreateCall(sfn, {});
    }

    // CG30: for enum instance methods, record the enum type so that
    // `case this` arms can resolve enum constant patterns.
    const sema::TypeSymbol* savedEnumMethod = currentEnumMethod_;
    if (!isStaticMethod(ms) && ms->parent &&
        ms->parent->kind == ast::TypeDeclKind::Enum)
        currentEnumMethod_ = ms->parent;

    bool terminated = false;
    if (ci.bodyKind == ast::MethodBodyKind::Expression && ci.exprBody) {
        llvm::Value* v = lowerExpr(*ci.exprBody);
        // §8.6: box a value-typed `=> expr` result returned through an
        // Object/interface return type (so the caller's is/as/dispatch works).
        if (v && currentReturnTypeNode_)
            v = boxForTarget(v, ctx_.resolvedType(*currentReturnTypeNode_),
                             ci.exprBody);
        terminated = emitReturn(fn, v);
    } else if (ci.bodyKind == ast::MethodBodyKind::Block) {
        terminated = lowerScopedBlock(*ci.stmts);
    }
    if (!terminated) emitImplicitReturn(fn);
    currentEnumMethod_ = savedEnumMethod;
}


void Emitter::emitStaticConstructorBody(sema::ConstructorSymbol* cs,
                               llvm::Function* fn) {
    auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
    if (!cd) return;
    currentFn_ = fn;
    currentReturnTypeNode_ = nullptr;  // ctors return void
    currentThis_ = nullptr;
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    disposeStack_.clear();
    loopStack_.clear(); finallyStack_.clear();
    propagateBB_ = nullptr;
    unwindTarget_ = nullptr;
    currentCaughtExc_ = nullptr;
    currentCaughtName_ = {};
    unwindScopeDepth_ = 0;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));
    if (!lowerScopedBlock(cd->body)) builder_.CreateRetVoid();
}


void Emitter::emitConstructorBody(sema::ConstructorSymbol* cs, llvm::Function* fn) {
    auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
    if (!cd) return;
    currentFn_ = fn;
    currentReturnTypeNode_ = nullptr;  // ctors return void
    currentThis_ = nullptr;
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    disposeStack_.clear();
    loopStack_.clear(); finallyStack_.clear();
    propagateBB_ = nullptr;
    unwindTarget_ = nullptr;
    currentCaughtExc_ = nullptr;
    currentCaughtName_ = {};
    unwindScopeDepth_ = 0;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));
    // §28.3 (line 4910): eligibility of owned ctor params for unwind disposal.
    unwindIneligible_.clear();
    unwindIneligibleFn_ = fn;
    collectUnwindIneligible(cd->body);
    bindParameters(fn, /*hasThis=*/true, &cs->params);

    // CG16 (§12.8): a leading `base(...)`/`this(...)` chain call runs
    // before the body. `this(...)` delegates to a sibling ctor (which
    // runs the base chain); `base(...)` runs the base ctor. With
    // neither written, an implicit `base()` runs the base class's
    // parameterless ctor.
    bool isThis = false;
    std::size_t startIdx = 0;
    if (!cd->body.empty()) {
        if (const ast::CallExpr* chain =
                chainCallOf(*cd->body[0], isThis)) {
            emitChainCall(chain, isThis, cs);
            startIdx = 1;
        } else {
            emitImplicitBase(cs->parent, currentThis_);
        }
    } else {
        emitImplicitBase(cs->parent, currentThis_);
    }
    // CG20: a base/this constructor (the chain) may throw — unwind
    // before running the rest of this constructor's body.
    emitPendingCheck();

    bool terminated = lowerScopedBlock(cd->body, startIdx);
    if (!terminated) builder_.CreateRetVoid();
}


const ast::CallExpr* Emitter::chainCallOf(const ast::Stmt& s, bool& isThis) {
    auto* es = dynamic_cast<const ast::ExpressionStmt*>(&s);
    if (!es || !es->expr) return nullptr;
    auto* call = dynamic_cast<const ast::CallExpr*>(es->expr.get());
    if (!call || !call->callee) return nullptr;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(call->callee.get());
    if (!id) return nullptr;
    if (id->name == "this") { isThis = true; return call; }
    if (id->name == "base") { isThis = false; return call; }
    return nullptr;
}


const sema::TypeSymbol* Emitter::baseClassOf(const sema::TypeSymbol* t) {
    for (const sema::TypeSymbol* b : t->baseTypes) {
        if (b && b->kind == ast::TypeDeclKind::Class) return b;
    }
    return nullptr;
}


bool Emitter::hasAnyConstructor(const sema::TypeSymbol* t) {
    for (sema::Symbol* m : t->members) {
        auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m);
        if (cs && !cs->isStatic) return true;
    }
    return false;
}


int Emitter::ctorArityCount(const sema::TypeSymbol* cls, std::size_t n,
                   const sema::ConstructorSymbol* exclude) {
    int cnt = 0;
    for (sema::Symbol* m : cls->members) {
        auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m);
        if (!cs || cs->isStatic || cs == exclude) continue;
        if (cs->params.size() == n) cnt++;
    }
    return cnt;
}


void Emitter::emitChainCall(const ast::CallExpr* call, bool isThis,
                   sema::ConstructorSymbol* cs) {
    const sema::TypeSymbol* targetCls =
        isThis ? cs->parent : baseClassOf(cs->parent);
    if (!targetCls) return;  // base() at the root (Object): nothing to run
    std::size_t n = call->args.size();
    sema::ConstructorSymbol* target =
        ctorByArity(targetCls, n, isThis ? cs : nullptr);
    if (!target) {
        // Explicit `base()` with no matching base ctor: the base's own
        // base may still need an implicit chain.
        if (!isThis && n == 0) emitImplicitBase(targetCls, currentThis_);
        return;
    }
    auto it = ctorFns_.find(target);
    if (it == ctorFns_.end()) return;
    std::vector<llvm::Value*> args;
    args.push_back(currentThis_);
    for (const auto& a : call->args) {
        if (!a.value) return;
        llvm::Value* v = lowerExpr(*a.value);
        if (!v) return;
        args.push_back(v);
    }
    if (args.size() == it->second->arg_size()) {
        builder_.CreateCall(it->second, args);
    }
}


void Emitter::emitImplicitBase(const sema::TypeSymbol* cls, llvm::Value* obj) {
    const sema::TypeSymbol* b = baseClassOf(cls);
    if (!b) return;
    if (sema::ConstructorSymbol* target = ctorByArity(b, 0, nullptr)) {
        auto it = ctorFns_.find(target);
        if (it != ctorFns_.end() && it->second->arg_size() == 1) {
            builder_.CreateCall(it->second, {obj});
        }
    } else {
        emitImplicitBase(b, obj);
    }
}


void Emitter::bindParameters(llvm::Function* fn, bool hasThis,
                    const std::vector<sema::Symbol*>* paramSyms) {
    std::size_t paramIdx = 0;
    bool first = true;
    pendingOwnedParams_.clear();
    for (auto& arg : fn->args()) {
        if (hasThis && first) {
            currentThis_ = &arg;
            first = false;
            continue;
        }
        first = false;
        sema::Symbol* psym =
            (paramSyms && paramIdx < paramSyms->size())
                ? (*paramSyms)[paramIdx]
                : nullptr;
        auto* ps = dynamic_cast<sema::ParameterSymbol*>(psym);
        if (ps && isByRefMod(ps->modifier)) {
            // CG50: a ref/out parameter — the argument is the caller's
            // storage address; reads/writes go through it.
            llvm::Type* elemTy =
                ps->declType ? lowerType(ps->declType) : i32();
            refParams_[psym] = {&arg, elemTy};
        } else {
            llvm::AllocaInst* slot = entryAlloca(arg.getType());
            builder_.CreateStore(&arg, slot);
            if (psym) paramSlots_[psym] = slot;
            // §28.3 (line 4910): an `owned` parameter is auto-disposed on an
            // exception unwind out of the method. Defer its registration until
            // the function-body root dispose scope exists (lowerScopedBlock
            // registers pendingOwnedParams_ into the outermost scope), since no
            // scope is open yet here.
            if (psym) pendingOwnedParams_.push_back({psym, slot});
        }
        ++paramIdx;
    }
}


const std::vector<sema::Symbol*>* Emitter::paramSymbolsOf(const ast::MethodDecl* md) {
    for (const auto& sym : ctx_.symbols()) {
        if (sym->decl != md) continue;
        if (auto* ms = dynamic_cast<sema::MethodSymbol*>(sym.get())) {
            return &ms->params;
        }
    }
    return nullptr;
}


bool Emitter::emitReturn(llvm::Function* fn, llvm::Value* v) {
    if (fn->getReturnType()->isVoidTy()) builder_.CreateRetVoid();
    else {
        llvm::Type* retTy = fn->getReturnType();
        // CG-nullable: coerce plain primitive to nullable-prim struct
        // when the function's return type is {i1, primType}.
        if (v && v->getType() != retTy &&
            llvm::isa<llvm::StructType>(retTy)) {
            if (auto* st = llvm::dyn_cast<llvm::StructType>(retTy)) {
                if (st->getNumElements() == 2 && st->getElementType(0) == nullablePrimHasValTy()) {
                    if (v->getType()->isPointerTy() &&
                        llvm::isa<llvm::ConstantPointerNull>(v)) {
                        v = llvm::ConstantAggregateZero::get(retTy);
                    } else if (v->getType() == st->getElementType(1)) {
                        llvm::Value* res = builder_.CreateInsertValue(
                            llvm::UndefValue::get(retTy),
                            llvm::ConstantInt::get(nullablePrimHasValTy(), 1), {0u});
                        v = builder_.CreateInsertValue(res, v, {1u});
                    }
                }
            }
        }
        builder_.CreateRet(v ? v : llvm::Constant::getNullValue(retTy));
    }
    return true;
}


void Emitter::emitImplicitReturn(llvm::Function* fn) {
    if (fn->getReturnType()->isVoidTy()) builder_.CreateRetVoid();
    else builder_.CreateRet(
        llvm::Constant::getNullValue(fn->getReturnType()));
}


void Emitter::lowerLocalDecl(const ast::Node* declNode, const ast::Expr* init,
                    const ast::Type* declType) {
    // §15.1 / §16: collection literal `[e1, e2, …]` used as an inline-array
    // initializer.  lowerExpr returns null for CollectionLiteralExpr (not a
    // general expression), so handle it here where we have the slot.
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(init)) {
        if (!cl->isDict && !cl->elements.empty()) {
            llvm::Type* arrTy = declType ? lowerType(declType) : nullptr;
            if (!arrTy) {
                // Infer from the first element.
                llvm::Value* first = lowerExpr(*cl->elements[0]);
                if (!first) return;
                arrTy = llvm::ArrayType::get(first->getType(),
                                             cl->elements.size());
            }
            llvm::AllocaInst* slot = entryAlloca(arrTy);
            locals_[declNode] = slot;
            // Store each element via GEP.
            auto* aty = llvm::dyn_cast<llvm::ArrayType>(arrTy);
            llvm::Type* elemTy = aty ? aty->getElementType() : i32();
            for (std::size_t i = 0; i < cl->elements.size(); ++i) {
                if (!cl->elements[i]) continue;
                llvm::Value* ev = lowerExpr(*cl->elements[i]);
                if (!ev) continue;
                // Coerce element to the array's element type if needed.
                if (ev->getType() != elemTy) {
                    if (ev->getType()->isIntegerTy() && elemTy->isIntegerTy()) {
                        unsigned eb = ev->getType()->getIntegerBitWidth();
                        unsigned db = elemTy->getIntegerBitWidth();
                        ev = db > eb ? builder_.CreateSExt(ev, elemTy)
                                     : builder_.CreateTrunc(ev, elemTy);
                    }
                }
                llvm::Value* gep = builder_.CreateGEP(
                    arrTy, slot, {builder_.getInt32(0), builder_.getInt32(i)});
                builder_.CreateStore(ev, gep);
            }
            return;
        }
    }
    llvm::Value* v = init ? lowerExpr(*init) : nullptr;
    // CG40: a diverging initializer (`var x := throw E;`) terminated the
    // block; allocate the slot but emit no store.
    if (diverged_) return;
    // §8.6: when the local is declared Object/interface and the initializer is
    // a value type, box it — `var o: Object := new Rect(3,4)` allocates a heap
    // wrapper whose first word is the value type's box vtable, so `is`/`as`/
    // dispatch recognize the boxed value (findings 05/06/08/12/13). A reference
    // source (class/string/interface) is left as its bare pointer (#399).
    if (v && declType)
        v = boxForTarget(v, ctx_.resolvedType(*declType), init);
    llvm::Type* declaredTy = declType ? lowerType(declType) : nullptr;
    // §5.1: when the local is declared with an explicit wider numeric type
    // and the initializer lowered to a narrower numeric LLVM type (e.g.
    // `var d: double := n;` with `n: int`), materialize the implicit widening
    // sema already admitted. Without this the int's raw bits are stored into
    // the FP slot and every read reinterprets them (issue #408 / finding [15]:
    // an `anyext from i32` f64 load that the backend cannot select, or a
    // subnormal value). The slot is then sized to the converted value's type.
    // issue #421 / #410: thread the initializer's signedness so an unsigned
    // source (e.g. `var d: double := someUint`) widens via UIToFP/ZExt instead
    // of the signed default — otherwise a large uint would read as negative.
    if (v && declaredTy)
        v = coerceNumericToType(v, declaredTy,
                                init && isUnsignedTargetExpr(*init));
    llvm::Type* slotTy = v ? v->getType() : (declaredTy ? declaredTy : i32());
    // Prefer the declared type when the value's type is less specific —
    // e.g. `var arr: [N]T := null` produces a ptr null literal but the
    // slot must be [N x ptr], not just ptr.
    if (declaredTy && slotTy != declaredTy) {
        if (llvm::isa<llvm::ArrayType>(declaredTy) &&
            (v == nullptr || dynamic_cast<const ast::NullLiteralExpr*>(init)))
            slotTy = declaredTy;
    }
    // CG-nullable: when the declared type is a nullable-prim struct but
    // the value is a plain primitive, wrap it: int? := 5 → {true, 5}.
    // Also fix: int? := null should produce the null struct, not ptr null.
    if (declaredTy && llvm::isa<llvm::StructType>(declaredTy) && v) {
        if (v->getType() != declaredTy) {
            if (auto* st = llvm::dyn_cast<llvm::StructType>(declaredTy)) {
                if (st->getNumElements() == 2 &&
                    st->getElementType(0) == nullablePrimHasValTy()) {
                    if (v->getType()->isPointerTy() &&
                        llvm::isa<llvm::ConstantPointerNull>(v)) {
                        // null → {false, 0}
                        v = llvm::ConstantAggregateZero::get(declaredTy);
                    } else if (v->getType() == st->getElementType(1)) {
                        // plain prim → {true, value}
                        llvm::Value* res = builder_.CreateInsertValue(
                            llvm::UndefValue::get(declaredTy),
                            llvm::ConstantInt::get(nullablePrimHasValTy(), 1), {0u});
                        v = builder_.CreateInsertValue(res, v, {1u});
                    }
                    slotTy = declaredTy;
                }
            }
        }
    }
    // §29.3: in an iterator MoveNext body, map locals to state-struct fields
    // so they persist across yield returns.
    llvm::AllocaInst* slot = nullptr;
    if (iterStateStruct_ && iterSelf_) {
        auto fit = iterLocalFields_.find(declNode);
        if (fit != iterLocalFields_.end()) {
            // Use the struct field as the "slot" — it's a GEP result.
            // We can't store a GEP as an AllocaInst, so we must use a
            // different path: store the GEP in a separate alloca-of-ptr
            // that points to the struct field. Or simply: create an alloca
            // that shadows the struct field (load/store to it), and at each
            // yield point save/restore all locals from the struct.
            // SIMPLEST CORRECT APPROACH: create an alloca AND store it in
            // the struct field by GEP. At each yield, save the alloca to struct.
            // For now: use entryAlloca but track it for save/restore.
            slot = entryAlloca(slotTy);
            // Record this slot → field mapping for yield save/restore.
            iterAllocaToField_[slot] = fit->second;
            locals_[declNode] = slot;
            // Restore from struct at this point (in case we're in state > 0).
            llvm::Value* fptr = builder_.CreateStructGEP(
                iterStateStruct_, iterSelf_, fit->second);
            llvm::Value* saved = builder_.CreateLoad(slotTy, fptr);
            // Only restore if state != 0 (first entry); use v if state=0.
            // For simplicity: always load from struct, then overwrite with init below.
            // (State 0: struct field is 0-initialized; override with init is correct.)
            // Actually: just DON'T restore — let the init below set the value.
            // The struct field will be set by the yield handler when saving.
            (void)saved;  // will be overwritten by init
        }
    }
    if (!slot) slot = entryAlloca(slotTy);
    locals_[declNode] = slot;
    // CG-generic: record the concrete substitution for this local so that
    // method calls on it (e.g. sp2.GetFirst()) can recover the concrete type
    // args even when the sema type is still generic. Prefer the active
    // genericSubst_ (set during field access), but fall back to
    // lastCallGenericSubst_ (set during method calls that return generic types).
    if (slotTy->isPointerTy()) {
        if (!genericSubst_.empty())
            localGenericSubst_[declNode] = genericSubst_;
        else if (!lastCallGenericSubst_.empty()) {
            localGenericSubst_[declNode] = lastCallGenericSubst_;
            lastCallGenericSubst_.clear();
        }
    }
    if (v) {
        builder_.CreateStore(v, slot);
    } else if (llvm::dyn_cast<llvm::ArrayType>(slotTy)) {
        // §15.1: inline arrays are zero-initialized when declared
        // without an initializer (like all locals with explicit type).
        builder_.CreateStore(llvm::ConstantAggregateZero::get(slotTy),
                             slot);
    }
    // #422 / §4.6 / §28.9: if this `var` string local was proven non-escaping
    // and initialized by a codegen +1 producer (string `+` / interpolation),
    // register a scope-end bz_string_release of its single producer reference.
    // The slot holds exactly that one reference (the value `v` just stored, at
    // rc=1); no other reference exists (proven by the escape walk), so releasing
    // it at scope end frees the data+header exactly once — no UAF, no
    // double-free. Gated on currentFn_ matching the analyzed function (a
    // lambda/thunk body lowered mid-function is not analyzed → its string locals
    // just leak, never double-freed). disposeStack_ is non-empty inside any
    // lowered block body.
    if (v && currentFn_ == stringReleaseEligibleFn_ &&
        stringReleaseEligible_.count(static_cast<const void*>(declNode)) &&
        !disposeStack_.empty()) {
        Disposal d{slot, nullptr};
        d.stringRelease = true;
        disposeStack_.back().push_back(d);
    }
    // #422 / §27.5 / §28.9: if this `var` closure (function-typed) local was
    // proven non-escaping and initialized by a codegen +1 producer (a lambda
    // expression or a method reference), register a scope-end
    // bz_closure_release of its single producer reference. The slot holds
    // exactly that one reference (the cell `v` just stored, at rc=1); no other
    // reference exists (proven by the escape walk), so releasing it at scope end
    // frees the cell + its env block exactly once — no UAF, no double-free, and
    // (per §28.9) no captured value is touched (captures are borrows). Gated on
    // currentFn_ matching the analyzed function (a lambda/thunk body lowered
    // mid-function is not analyzed → its closure locals just leak, never
    // double-freed).
    if (v && currentFn_ == closureReleaseEligibleFn_ &&
        closureReleaseEligible_.count(static_cast<const void*>(declNode)) &&
        !disposeStack_.empty()) {
        Disposal d{slot, nullptr};
        d.closureRelease = true;
        disposeStack_.back().push_back(d);
    }
    // #422 / §8.6 / §28.9: if this `var` Object/interface local was proven
    // non-escaping and initialized by a codegen +1 BOX producer (a boxable-value
    // initializer that boxForTarget wrapped), register a scope-end bz_box_release
    // of its single producer reference. The slot holds exactly that one reference
    // (the box `v` just stored, at rc=1); no other reference exists (proven by
    // the escape walk), so releasing it at scope end frees the box wrapper
    // exactly once — no UAF, no double-free. The boxed value is a value-type leaf
    // (no owned sub-objects), so only the wrapper is freed. CRITICALLY, this NEVER
    // fires on a class instance coerced to Object: such a local's initializer has
    // a reference static type, so isPlusOneBoxProducer is false and it is never in
    // boxReleaseEligible_ — the class instance keeps its own ownership/disposal
    // path untouched. Gated on currentFn_ matching the analyzed function (a
    // lambda/thunk body lowered mid-function is not analyzed → its boxed locals
    // just leak, never double-freed).
    if (v && currentFn_ == boxReleaseEligibleFn_ &&
        boxReleaseEligible_.count(static_cast<const void*>(declNode)) &&
        !disposeStack_.empty()) {
        Disposal d{slot, nullptr};
        d.boxRelease = true;
        disposeStack_.back().push_back(d);
    }
}


void Emitter::registerUsingDisposal(const ast::LocalDecl* ld) {
    if (disposeStack_.empty()) return;
    auto it = locals_.find(ld);
    if (it == locals_.end()) return;
    // per §2.11: a `using var` of type `Subscription` auto-disposes at scope
    // end by removing its registration (bz_subscription_dispose), not via a
    // class vtable. A Subscription has no user TypeSymbol, so it never appears
    // in `disposerFns_`; recognize it by its declared type, else (for an
    // inferred local bound to a literal `.Subscribe(...)`, whose result sema
    // does not type) structurally by its initializer.
    bool isSub = false;
    if (ld->type)
        isSub = isSubscriptionType(ctx_.resolvedType(*ld->type));
    if (!isSub && ld->initializer)
        isSub = isSubscriptionExpr(*ld->initializer);
    if (isSub) {
        disposeStack_.back().push_back({it->second, nullptr, /*subscription=*/true});
        return;
    }
    // #362 / stdlib §4.5: a `using var` bound to a heap collection
    // (Array<T>/OwnedArray<T>/Dictionary/Set/…). These are builtins with no
    // user TypeSymbol, so classOfLocal returns null; recognize them by name and
    // register a collection disposal. For an OWNING collection of class
    // elements the disposal cascades into the elements (the leak fixed here);
    // a borrowing collection or value elements free only the backing.
    if (const sema::TypeRef* collTy = collectionTypeOfLocal(ld)) {
        if (isCollectionTypeName(collTy)) {
            Disposal d{it->second, nullptr};
            d.collection = true;
            d.collectionElemDisposer = owningCollectionElemDisposer(collTy);
            disposeStack_.back().push_back(d);
            return;
        }
    }
    // #394 / stdlib §4.9 + #401 / stdlib §4.8: a `using var` bound to a
    // hash-backed dictionary (Dictionary<K,V>/OwnedDictionary<K,V>). Dicts are
    // excluded from isCollectionTypeName (they free via bz_dict_free, not
    // bz_array_free), so they get their own disposal kind here. The per-value
    // callback is one of exactly two mutually exclusive things (or null):
    //   * #394: an OwnedDictionary<K, class> → cascade disposal into each
    //     surviving CLASS value (dispose + free via its vtable).
    //   * #401: a borrowing Dictionary<K, record> → free each surviving record
    //     value BOX (plain free; a record is a value type with nothing to
    //     dispose, §3.2 / §9).
    // A borrowing Dictionary with a scalar/string value stores no box, so both
    // return null and only the backing table + key bytes are freed. Keys are
    // never disposed (§4.9: keys are value-like; the dictionary owns its values
    // only).
    if (const sema::TypeRef* dictTy = dictTypeOfLocal(ld)) {
        Disposal d{it->second, nullptr};
        d.dict = true;
        d.dictValueDisposer = owningDictValueDisposer(dictTy);
        if (!d.dictValueDisposer)
            d.dictValueDisposer = dictValueBoxFreer(dictTy);
        disposeStack_.back().push_back(d);
        return;
    }

    const sema::TypeSymbol* cls = classOfLocal(ld);
    if (cls && disposerFns_.find(cls) != disposerFns_.end()) {
        disposeStack_.back().push_back({it->second, cls});
        return;
    }
    // #405 / §28.10 / §31.1: a `using var` whose type is a BUILTIN exception
    // (e.g. `using var e := new InvalidOperationException("x");`). Built-in
    // exceptions have no user TypeSymbol, so classOfLocal returns null and the
    // branch above misses them; they are nevertheless ordinary owned heap class
    // instances and must be disposed + reclaimed at scope end. Dispose
    // dynamically through the object's own vtable (slot 1 = no-op bz$exDispose;
    // the 16-byte block is reclaimed, the runtime-managed message string is
    // not). The value is never an in-flight exception here: a `using var` forbids
    // the move that a `throw` would require (§28.10), so this slot and the
    // exception-machinery free can never alias (no double-free).
    if (usingVarOwnsBuiltinException(ld)) {
        Disposal d{it->second, nullptr};
        d.dynamicDispose = true;
        disposeStack_.back().push_back(d);
        return;
    }
    // #383 / §28.8.1: `using var u := move x.f;` — the using var now owns the
    // moved-out class object and discharges it at scope end (dynamically; the
    // concrete class is the field's runtime type). Skipped inside a destructor
    // body to avoid double-freeing the same field (see emittingDisposerBody_).
    if (usingVarIsMovedClassObject(ld)) {
        Disposal d{it->second, nullptr};
        d.dynamicDispose = true;
        disposeStack_.back().push_back(d);
        return;
    }
    // #383 / §28: a `using var` owning a class/interface object whose concrete
    // class classOfLocal does not capture — an interface-typed slot, or a call
    // returning `owned T` (NOT a borrow return, per §28.10). Dispose via the
    // object's own vtable.
    if (usingVarOwnsClassObjectDynamically(ld)) {
        Disposal d{it->second, nullptr};
        d.dynamicDispose = true;
        disposeStack_.back().push_back(d);
        return;
    }
}


const sema::TypeSymbol* Emitter::unwindOwnedLocalClass(const ast::LocalDecl* ld) {
    // §28.3: a plain `var` owned local eligible for exception-unwind disposal.
    // Restrict to a definitely-owned, definitely-assigned class/interface object
    // so the unwind disposal (dynamic dispatch) is always valid:
    //   (a) initializer is `new ClassType(...)`  — owned, class, assigned; or
    //   (b) initializer is a call returning `owned T` (T class/interface).
    // Collections/dicts/subscriptions are excluded (they free via their own
    // runtime calls, not class dispatch, and are not the §28.3 leak findings);
    // a borrow initializer (no `owned`) carries no disposal obligation; an
    // uninitialized/null/value-typed local has nothing to dispose.
    if (!ld->initializer) return nullptr;
    if (collectionTypeOfLocal(ld) || dictTypeOfLocal(ld)) return nullptr;
    if (ld->type && isSubscriptionType(ctx_.resolvedType(*ld->type)))
        return nullptr;
    if (isSubscriptionExpr(*ld->initializer)) return nullptr;
    // (a) `new ClassType(...)` — an owned class instance.
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(ld->initializer.get())) {
        const sema::TypeSymbol* cls = classOfLocal(ld);
        if (cls && cls->kind == ast::TypeDeclKind::Class &&
            disposerFns_.find(cls) != disposerFns_.end())
            return cls;
        // An interface-/abstract-typed `new` is unusual; fall through.
        (void)ne;
        return nullptr;
    }
    // (b) a call returning `owned T` (class/interface). Dispatch is dynamic, so
    // the concrete class is not needed — return a sentinel non-null class when
    // the static type is a class with a disposer, else rely on dynamic dispatch.
    if (auto* call = dynamic_cast<const ast::CallExpr*>(ld->initializer.get())) {
        if (call->callee) {
            auto* ms = dynamic_cast<sema::MethodSymbol*>(
                ctx_.exprSymbol(*call->callee));
            if (ms) {
                auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
                if (md && md->returnOwned &&
                    isClassOrInterfaceRef(ctx_.exprType(*ld->initializer))) {
                    // Use the resolved class when available (for the disposer
                    // map); dispatch is dynamic in emitDisposals regardless.
                    if (auto* rt = ctx_.exprType(*ld->initializer))
                        if (rt->kind == sema::TypeRef::Kind::Named &&
                            rt->declaration &&
                            rt->declaration->kind == ast::TypeDeclKind::Class)
                            return rt->declaration;
                    // Interface return: dynamic dispatch with a null cls.
                    return reinterpret_cast<const sema::TypeSymbol*>(1);
                }
            }
        }
    }
    return nullptr;
}


void Emitter::registerUnwindOwnedLocal(const ast::LocalDecl* ld) {
    // §28.3 (line 4910): a plain `var` owned local is auto-disposed when an
    // exception unwinds out of its scope (an `unwindOnly` Disposal — normal exit
    // skips it). Only register the conservative eligible subset (see
    // unwindOwnedLocalClass + unwindIneligible_) so the unwind disposal can
    // never double-dispose a value that was moved out / disposed / reassigned on
    // the way to the throw.
    if (disposeStack_.empty()) return;
    if (emittingDisposerBody_) return;   // disposer locals handled by field pass
    // Only register when the ineligible analysis covers the current body — see
    // unwindIneligibleFn_ (skips lambda/thunk bodies whose moves/disposes were
    // not analyzed, to avoid a double-dispose).
    if (currentFn_ != unwindIneligibleFn_) return;
    if (unwindIneligible_.count(static_cast<const void*>(ld))) return;
    const sema::TypeSymbol* cls = unwindOwnedLocalClass(ld);
    if (!cls) return;
    auto it = locals_.find(ld);
    if (it == locals_.end()) return;
    Disposal d{it->second, nullptr};
    d.unwindOnly = true;
    disposeStack_.back().push_back(d);
}


void Emitter::registerUnwindOwnedParam(sema::Symbol* psym,
                                       llvm::AllocaInst* slot) {
    // §28.3 (line 4910): an `owned` parameter is auto-disposed when an exception
    // unwinds out of its method (same unwindOnly mechanism as plain owned
    // locals). Registered in the outermost scope so it covers the whole body.
    if (disposeStack_.empty()) return;
    if (emittingDisposerBody_) return;
    if (currentFn_ != unwindIneligibleFn_) return;
    if (unwindIneligible_.count(static_cast<const void*>(psym))) return;
    auto* ps = dynamic_cast<sema::ParameterSymbol*>(psym);
    if (!ps) return;
    // Only an `owned` parameter carries a disposal obligation (§28.8); `ref`/
    // `out`/borrow params do not. (`out owned` is by-ref — the callee writes the
    // caller's storage and does not own at entry — and is excluded.)
    if (ps->modifier != "owned") return;
    // Must be a class/interface type to have a Dispose to call.
    const sema::TypeRef* rt =
        ps->declType ? ctx_.resolvedType(*ps->declType) : ps->resolvedDeclType;
    if (!isClassOrInterfaceRef(rt)) return;
    Disposal d{slot, nullptr};
    d.unwindOnly = true;
    disposeStack_.front().push_back(d);
}


namespace {
// §28.3: a conservative walk that records every local/parameter consumed in a
// value (move / owned-arg / assignment / field-store / explicit-dispose)
// position. Borrow-position reads — a method/property/indexer RECEIVER (other
// than `.Dispose()`), and a `return x` operand (a normal-exit discharge the
// unwindOnly disposal already skips) — do NOT taint the local. Anything not
// recognized as borrow-safe is treated as consuming (sound over-approximation:
// at worst it leaves a value un-disposed on its unwind path, never double-
// disposed). `mark(node)` records the ineligible key (a LocalDecl node for a
// local, the ParameterSymbol for a parameter).
struct UnwindIneligibleWalker {
    const sema::SemaContext& ctx;
    std::unordered_set<const void*>& out;

    // Record the symbol behind an identifier as consumed/ineligible.
    void markIdent(const ast::Expr* e) {
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(e);
        if (!id) { expr(e, /*consuming=*/true); return; }
        sema::Symbol* s = ctx.exprSymbol(*id);
        if (auto* loc = dynamic_cast<sema::LocalSymbol*>(s)) {
            if (loc->decl) out.insert(static_cast<const void*>(loc->decl));
        } else if (dynamic_cast<sema::ParameterSymbol*>(s)) {
            out.insert(static_cast<const void*>(s));
        }
    }

    // Walk an expression. `consuming` = the value of this expression is moved /
    // consumed by its context, so a bare identifier here taints its local.
    void expr(const ast::Expr* e, bool consuming) {
        if (!e) return;
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e)) {
            if (consuming) markIdent(id);
            return;
        }
        if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
            // `move x` always consumes its operand.
            if (u->op == "move") markIdent(u->operand.get());
            else expr(u->operand.get(), /*consuming=*/true);
            return;
        }
        if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            // Receiver of a member access is a BORROW (not consumed) — e.g.
            // `x.field`, and (handled in CallExpr) `x.Method(...)`.
            expr(m->target.get(), /*consuming=*/false);
            return;
        }
        if (auto* m = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
            expr(m->target.get(), /*consuming=*/false);
            return;
        }
        if (auto* p = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
            expr(p->operand.get(), consuming);
            return;
        }
        if (auto* c = dynamic_cast<const ast::CallExpr*>(e)) {
            // `x.Dispose()` consumes/disposes x; any other `x.Method(...)`
            // borrows the receiver. Bare-call arguments are conservatively
            // treated as owned-arg moves (consuming).
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                    c->callee.get())) {
                if (ma->member == "Dispose") markIdent(ma->target.get());
                else expr(ma->target.get(), /*consuming=*/false);
            } else {
                expr(c->callee.get(), /*consuming=*/false);
            }
            for (auto& a : c->args) expr(a.value.get(), /*consuming=*/true);
            return;
        }
        if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                    c->callee.get())) {
                if (ma->member == "Dispose") markIdent(ma->target.get());
                else expr(ma->target.get(), /*consuming=*/false);
            } else {
                expr(c->callee.get(), /*consuming=*/false);
            }
            for (auto& a : c->args) expr(a.value.get(), /*consuming=*/true);
            return;
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
            expr(ix->target.get(), /*consuming=*/false);  // indexer receiver = borrow
            for (auto& a : ix->indices) expr(a.value.get(), /*consuming=*/true);
            return;
        }
        if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
            expr(b->left.get(), /*consuming=*/false);
            expr(b->right.get(), /*consuming=*/false);
            return;
        }
        if (auto* t = dynamic_cast<const ast::TypeTestExpr*>(e)) {
            expr(t->operand.get(), /*consuming=*/false);
            return;
        }
        if (auto* c = dynamic_cast<const ast::ConditionalExpr*>(e)) {
            expr(c->condition.get(), /*consuming=*/false);
            expr(c->thenValue.get(), consuming);
            expr(c->elseValue.get(), consuming);
            return;
        }
        if (auto* t = dynamic_cast<const ast::ThrowExpr*>(e)) {
            expr(t->operand.get(), /*consuming=*/true);
            return;
        }
        if (auto* r = dynamic_cast<const ast::RangeExpr*>(e)) {
            expr(r->lo.get(), false); expr(r->hi.get(), false);
            expr(r->step.get(), false);
            return;
        }
        if (auto* n = dynamic_cast<const ast::NewExpr*>(e)) {
            for (auto& a : n->args) expr(a.value.get(), /*consuming=*/true);
            return;
        }
        if (auto* w = dynamic_cast<const ast::WithExpr*>(e)) {
            expr(w->source.get(), /*consuming=*/false);
            for (auto& a : w->assigns) expr(a.value.get(), /*consuming=*/true);
            return;
        }
        if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
            for (auto& el : cl->elements) expr(el.get(), /*consuming=*/true);
            for (auto& k : cl->keys) expr(k.get(), /*consuming=*/true);
            return;
        }
        if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
            for (auto& part : is->parts) expr(part.expr.get(), false);
            return;
        }
        if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(e)) {
            // A closure capturing an owned local borrows it (§28.9); treat a
            // captured local as consumed conservatively so it is not unwind-
            // disposed while the closure may still reference it.
            if (lam->exprBody) expr(lam->exprBody.get(), /*consuming=*/true);
            for (auto& s : lam->stmts) stmt(s.get());
            return;
        }
        if (auto* ce = dynamic_cast<const ast::CaseExpr*>(e)) {
            expr(ce->scrutinee.get(), /*consuming=*/false);
            for (auto& arm : ce->arms) {
                expr(arm.guard.get(), false);
                expr(arm.exprBody.get(), consuming);
                for (auto& s : arm.stmts) stmt(s.get());
            }
            return;
        }
        // Any other expression form: recurse conservatively as borrow (it has
        // no owned-consuming sub-positions we recognize); leaf literals no-op.
    }

    void stmt(const ast::Stmt* s) {
        if (!s) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            // `var s := x;` moves x (owned local read in an owning init); a
            // `move`/call/new RHS is handled by expr(consuming). Treat the whole
            // initializer as a consuming position.
            expr(ld->initializer.get(), /*consuming=*/true);
            return;
        }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s)) {
            expr(lc->value.get(), /*consuming=*/true); return;
        }
        if (auto* r = dynamic_cast<const ast::ReturnStmt*>(s)) {
            // `return x` is a normal-exit discharge; the unwindOnly disposal is
            // skipped on the return path, so it does NOT taint x. (A throw before
            // the return still disposes x via unwind.) Treat as borrow so the
            // common accumulator-returned local stays eligible.
            expr(r->value.get(), /*consuming=*/false); return;
        }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s)) {
            expr(es->expr.get(), /*consuming=*/false); return;
        }
        if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(s)) {
            expr(ts->value.get(), /*consuming=*/true); return;
        }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
            // `x := ...` reassigns x — its prior owned value's obligation is
            // released here, so x is no longer the same owned value on the way
            // to a later throw. Taint the target (a bare-identifier lvalue) and
            // walk the RHS as consuming (`x := y` moves y).
            markIdent(as->target.get());
            expr(as->value.get(), /*consuming=*/true);
            return;
        }
        if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(s)) {
            expr(inc->target.get(), /*consuming=*/false); return;
        }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(s)) {
            expr(ds->value.get(), /*consuming=*/true); return;
        }
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s)) {
            expr(ys->value.get(), /*consuming=*/true); return;
        }
        if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
            expr(is->ifBranch.condition.get(), false);
            for (auto& b : is->ifBranch.body) stmt(b.get());
            for (auto& br : is->elifBranches) {
                expr(br.condition.get(), false);
                for (auto& b : br.body) stmt(b.get());
            }
            for (auto& b : is->elseBody) stmt(b.get());
            return;
        }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
            expr(ws->condition.get(), false);
            for (auto& b : ws->body) stmt(b.get());
            return;
        }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
            expr(fs->iterable.get(), false); expr(fs->withExpr.get(), false);
            for (auto& b : fs->body) stmt(b.get());
            return;
        }
        if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
            for (auto& b : d->body) stmt(b.get());
            return;
        }
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
            for (auto& b : ts->body) stmt(b.get());
            for (auto& c : ts->catches) {
                expr(c.guard.get(), false);
                stmt(c.body.get());
            }
            for (auto& b : ts->finallyBody) stmt(b.get());
            return;
        }
        if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
            expr(cs->scrutinee.get(), false);
            for (auto& arm : cs->arms) {
                expr(arm.guard.get(), false);
                expr(arm.exprBody.get(), false);
                for (auto& a : arm.stmts) stmt(a.get());
            }
            return;
        }
        // BreakStmt / ContinueStmt: no sub-expressions.
    }
};
}  // namespace


void Emitter::collectUnwindIneligible(
        const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    unwindIneligible_.clear();
    UnwindIneligibleWalker w{ctx_, unwindIneligible_};
    for (auto& s : body) w.stmt(s.get());
}


// #422 / §4.6 / §28.9: is `init` a codegen-internal +1 string producer?
// A string `+` BinaryExpr or an InterpolatedStringExpr — both lower to a fresh
// malloc'd rc=1 header that codegen owns end-to-end. Binding such a result to a
// local consumes its single reference. A string-returning CALL is deliberately
// NOT included: the callee may return a borrow (a string field, a parameter)
// without handing off a +1, so caller-side release of a call result could free a
// value the callee still references (UAF). Restricting to `+`/interpolation
// keeps the producer/consumer balance entirely inside codegen.
bool Emitter::isPlusOneStringProducer(const ast::Expr* init) const {
    if (!init) return false;
    if (dynamic_cast<const ast::InterpolatedStringExpr*>(init)) return true;
    if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(init)) {
        if (bin->op != "+") return false;
        auto* rt = ctx_.exprType(*init);
        return rt && rt->kind == sema::TypeRef::Kind::Primitive &&
               rt->primitive == sema::PrimitiveType::String;
    }
    return false;
}

namespace {
// #422: a strict, conservative walk that decides whether a SPECIFIC string
// `var` local (identified by its LocalDecl node) ever escapes its scope. A
// local is "escaped" if its identifier appears anywhere except a non-consuming
// READ position (concat/comparison operand, a string-method/index receiver, an
// interpolation part, or an inner null-forgiving `s!`). Every other position —
// a call argument, an assignment/return/throw/yield/discard operand, a binding
// to another local, a `new`/`with`/collection element, a lambda capture, a
// `move`, a `for ... in s` iterable, etc. — is treated as an ESCAPE. The walk
// is an OVER-approximation of escape: any construct it does not explicitly
// recognize as a safe read leaves the local escaped (→ no release → leak, the
// safe direction). Only a local that NEVER escapes is released at scope end,
// where releasing its single producer reference is provably free of UAF (no
// other reference exists) and of double-free (once per scope exit).
//
// `safeRead`: true when a bare identifier in the current expression position is
// a non-consuming read; false when it would escape.
struct StringEscapeWalker {
    const sema::SemaContext& ctx;
    const void* target;   // the LocalDecl node we are testing
    bool escaped = false;
    // FINDING [1] / §28.9 / §28.3: true while walking inside a lambda body. A
    // closure's heap env block captures (borrows) the target local and outlives
    // the producing scope (§28.9: the closure value itself is runtime-managed,
    // "the captured borrow's lifetime is tracked separately"). So once inside a
    // lambda body, ANY read of the captured outer local is an ESCAPE regardless
    // of its safe-read sub-position (method receiver, concat operand,
    // interpolation part, `is`/`as` operand): the env still aliases it after the
    // scope ends, so a scope-end release would free a value the closure holds
    // (use-after-free). The lambda's OWN params/locals are distinct LocalDecls
    // and never match `target`, so they are unaffected.
    bool inLambda = false;

    bool isTargetIdent(const ast::Expr* e) {
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(e);
        if (!id) return false;
        auto* loc = dynamic_cast<sema::LocalSymbol*>(ctx.exprSymbol(*id));
        return loc && loc->decl &&
               static_cast<const void*>(loc->decl) == target;
    }

    void expr(const ast::Expr* e, bool safeRead) {
        if (!e || escaped) return;
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e)) {
            // FINDING [1]: inside a lambda body, a captured-local read always
            // escapes — the env outlives the scope (safe-read does not apply).
            if (isTargetIdent(id) && (!safeRead || inLambda)) escaped = true;
            return;
        }
        // Concat / comparison: both operands are non-consuming reads.
        if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
            expr(b->left.get(), /*safeRead=*/true);
            expr(b->right.get(), /*safeRead=*/true);
            return;
        }
        // `s.Member` / `s.Method(...)`: the receiver is a read.
        if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            expr(m->target.get(), /*safeRead=*/true);
            return;
        }
        if (auto* m = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
            expr(m->target.get(), /*safeRead=*/true);
            return;
        }
        // Call: the receiver (callee) is a read; every ARGUMENT escapes.
        if (auto* c = dynamic_cast<const ast::CallExpr*>(e)) {
            expr(c->callee.get(), /*safeRead=*/true);
            for (auto& a : c->args) expr(a.value.get(), /*safeRead=*/false);
            return;
        }
        if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
            expr(c->callee.get(), /*safeRead=*/true);
            for (auto& a : c->args) expr(a.value.get(), /*safeRead=*/false);
            return;
        }
        // Indexing `s[i]`: receiver is a read; indices escape (defensive).
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
            expr(ix->target.get(), /*safeRead=*/true);
            for (auto& a : ix->indices) expr(a.value.get(), /*safeRead=*/false);
            return;
        }
        // `s!` forwards s onward — inherit the parent position.
        if (auto* p = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
            expr(p->operand.get(), safeRead);
            return;
        }
        // Interpolation parts are reads (each is concatenated).
        if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
            for (auto& part : is->parts) expr(part.expr.get(), /*safeRead=*/true);
            return;
        }
        // `s is T` reads only.
        if (auto* t = dynamic_cast<const ast::TypeTestExpr*>(e)) {
            expr(t->operand.get(), /*safeRead=*/true);
            return;
        }
        // Conditional: condition is a read; each branch forwards the value on
        // to the parent position, so inherit `safeRead`.
        if (auto* c = dynamic_cast<const ast::ConditionalExpr*>(e)) {
            expr(c->condition.get(), /*safeRead=*/true);
            expr(c->thenValue.get(), safeRead);
            expr(c->elseValue.get(), safeRead);
            return;
        }
        // Range endpoints are reads.
        if (auto* r = dynamic_cast<const ast::RangeExpr*>(e)) {
            expr(r->lo.get(), /*safeRead=*/true);
            expr(r->hi.get(), /*safeRead=*/true);
            expr(r->step.get(), /*safeRead=*/true);
            return;
        }
        // Everything that can carry the value to a longer-lived location is an
        // ESCAPE for any contained occurrence of the target: new/with/collection
        // literal/lambda/case/unary(`move`)/throw-expr. Recurse with
        // safeRead=false so an occurrence taints, and bail conservatively (we do
        // not attempt to prove these safe).
        if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
            expr(u->operand.get(), /*safeRead=*/false); return;
        }
        if (auto* th = dynamic_cast<const ast::ThrowExpr*>(e)) {
            expr(th->operand.get(), /*safeRead=*/false); return;
        }
        if (auto* n = dynamic_cast<const ast::NewExpr*>(e)) {
            for (auto& a : n->args) expr(a.value.get(), /*safeRead=*/false);
            return;
        }
        if (auto* w = dynamic_cast<const ast::WithExpr*>(e)) {
            expr(w->source.get(), /*safeRead=*/false);
            for (auto& a : w->assigns) expr(a.value.get(), /*safeRead=*/false);
            return;
        }
        if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
            for (auto& el : cl->elements) expr(el.get(), /*safeRead=*/false);
            for (auto& k : cl->keys) expr(k.get(), /*safeRead=*/false);
            return;
        }
        if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(e)) {
            // FINDING [1] / §28.9 / §28.3: a lambda captures the local into its
            // heap env, which outlives the producing scope. Walk the body with
            // inLambda set so EVERY read of the captured target — even in a
            // safe-read sub-position (`cap.Length`, `cap + "!"`, `$"[{cap}]"`,
            // `cap is T`) — is flagged as an escape, preventing a scope-end
            // release that would free a value the env still aliases (UAF).
            bool savedInLambda = inLambda;
            inLambda = true;
            if (lam->exprBody) expr(lam->exprBody.get(), /*safeRead=*/false);
            for (auto& s : lam->stmts) stmt(s.get());
            inLambda = savedInLambda;
            return;
        }
        if (auto* ce = dynamic_cast<const ast::CaseExpr*>(e)) {
            expr(ce->scrutinee.get(), /*safeRead=*/false);
            for (auto& arm : ce->arms) {
                expr(arm.guard.get(), /*safeRead=*/false);
                expr(arm.exprBody.get(), /*safeRead=*/false);
                for (auto& s : arm.stmts) stmt(s.get());
            }
            return;
        }
        // Leaf literals (int/float/char/string/bool/null/unit/typeof/nameof/
        // default) contain no identifiers — nothing to do. Any OTHER expression
        // type is unknown to this walk; conservatively, if it could contain the
        // target we cannot prove non-escape — but leaves cannot, so this is
        // safe. (An unrecognized composite node would simply not be recursed
        // into; to stay sound we do not silently allow it: a composite the
        // grammar can produce is one of the cases above.)
    }

    void stmt(const ast::Stmt* s) {
        if (!s || escaped) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            // Binding the target into ANOTHER local escapes it (the new local
            // may itself be returned/stored). Walk the initializer as an escape
            // position — but its sub-reads (e.g. `s.ToUpper()` receiver) are
            // still recognized as safe by expr().
            expr(ld->initializer.get(), /*safeRead=*/false);
            return;
        }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s)) {
            expr(lc->value.get(), /*safeRead=*/false); return;
        }
        if (auto* r = dynamic_cast<const ast::ReturnStmt*>(s)) {
            expr(r->value.get(), /*safeRead=*/false); return;
        }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s)) {
            // A discarded expression (e.g. `Console.WriteLine(s);`). Its own
            // sub-structure decides safe vs escape (the call args escape).
            expr(es->expr.get(), /*safeRead=*/false); return;
        }
        if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(s)) {
            expr(ts->value.get(), /*safeRead=*/false); return;
        }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
            // The target being the local itself is a reassignment → escape
            // (overwrite handling is out of this increment). The RHS escapes.
            if (isTargetIdent(as->target.get())) { escaped = true; return; }
            expr(as->target.get(), /*safeRead=*/false);
            expr(as->value.get(), /*safeRead=*/false);
            return;
        }
        if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(s)) {
            // `s++` on a string is not valid, but be defensive: reassignment.
            if (isTargetIdent(inc->target.get())) { escaped = true; return; }
            expr(inc->target.get(), /*safeRead=*/false); return;
        }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(s)) {
            expr(ds->value.get(), /*safeRead=*/false); return;
        }
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s)) {
            expr(ys->value.get(), /*safeRead=*/false); return;
        }
        if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
            expr(is->ifBranch.condition.get(), /*safeRead=*/true);
            for (auto& b : is->ifBranch.body) stmt(b.get());
            for (auto& br : is->elifBranches) {
                expr(br.condition.get(), /*safeRead=*/true);
                for (auto& b : br.body) stmt(b.get());
            }
            for (auto& b : is->elseBody) stmt(b.get());
            return;
        }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
            expr(ws->condition.get(), /*safeRead=*/true);
            for (auto& b : ws->body) stmt(b.get());
            return;
        }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
            // `for var x in s do ... end` — iterating the local. Conservatively
            // an escape (the enumerator may retain the source). expr(false).
            expr(fs->iterable.get(), /*safeRead=*/false);
            expr(fs->withExpr.get(), /*safeRead=*/false);
            for (auto& b : fs->body) stmt(b.get());
            return;
        }
        if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
            for (auto& b : d->body) stmt(b.get());
            return;
        }
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
            for (auto& b : ts->body) stmt(b.get());
            for (auto& c : ts->catches) {
                expr(c.guard.get(), /*safeRead=*/true);
                stmt(c.body.get());
            }
            for (auto& b : ts->finallyBody) stmt(b.get());
            return;
        }
        if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
            expr(cs->scrutinee.get(), /*safeRead=*/false);
            for (auto& arm : cs->arms) {
                expr(arm.guard.get(), /*safeRead=*/false);
                expr(arm.exprBody.get(), /*safeRead=*/false);
                for (auto& a : arm.stmts) stmt(a.get());
            }
            return;
        }
        // BreakStmt / ContinueStmt: no sub-expressions. Any other statement type
        // is not produced by the grammar here; leave the local eligible only if
        // nothing above tainted it (the enumeration mirrors the complete set
        // walked by UnwindIneligibleWalker).
    }
};
}  // namespace


// #422 / §27.5 / §28.9: is `init` a codegen-internal +1 closure producer?
// A LambdaExpr, or a method reference used as a VALUE (a bare IdentifierExpr or
// a MemberAccessExpr whose resolved symbol is a MethodSymbol, where the static
// type of the expression is a function type — i.e. the method is referenced, not
// called). Both lower to a fresh calloc'd rc=1 closure cell (lowerLambda /
// makeMethodThunk) that codegen owns end-to-end, so binding the result to a
// local consumes its single reference. A closure-returning CALL is deliberately
// NOT included: the callee may hand back a function value WITHOUT a fresh +1
// (e.g. returning a closure stored in a field, or a method-reference parameter),
// so caller-side release of a call result could free a value the callee still
// references (UAF). Restricting to lambda/method-reference producers keeps the
// producer/consumer balance entirely inside codegen.
bool Emitter::isPlusOneClosureProducer(const ast::Expr* init) const {
    if (!init) return false;
    if (dynamic_cast<const ast::LambdaExpr*>(init)) return true;
    // A method reference used as a VALUE: a bare IdentifierExpr (a static method
    // of the current type) or a MemberAccessExpr `T.M` / `obj.M` whose resolved
    // symbol is a MethodSymbol — the exact shape codegen lowers to a fresh +1
    // closure cell via makeMethodThunk (LlvmBackend_Exprs.cpp:1204/1214,
    // LlvmBackend_Layout.cpp makeMethodThunk). A method *call* is a distinct
    // CallExpr node (never an IdentifierExpr / MemberAccessExpr), so this cannot
    // misfire on a call. (Sema defers a method reference's Function type to the
    // call-resolution slice — ExprTypecheck_Calls.cpp:1904 leaves exprType unset
    // for a bare reference — so we key off the symbol kind, which sema DOES set,
    // matching codegen exactly.) A property/field reference has a
    // Property/Field symbol, not a MethodSymbol, so it is not treated as a
    // closure-cell producer here (it lowers differently) and stays leaked —
    // safe.
    if (auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(*init))) {
        // A bare IdentifierExpr method reference is lowered to a cell ONLY for a
        // static method (Exprs.cpp:1204 gates on isStaticMethod); an instance
        // `obj.M` member access is lowered to a cell for either (Exprs.cpp:1214).
        // Match those gates exactly so eligibility never arms a release for an
        // init that did not actually produce a +1 cell.
        if (dynamic_cast<const ast::MemberAccessExpr*>(init)) return true;
        if (dynamic_cast<const ast::IdentifierExpr*>(init) && isStaticMethod(ms))
            return true;
    }
    return false;
}

namespace {
// #422 / §27.5 / §28.9: a strict, conservative walk that decides whether a
// SPECIFIC closure (function-typed) `var` local ever escapes its scope. A
// closure local is "escaped" if its identifier appears anywhere except a
// non-consuming READ position. For a function value the only non-consuming reads
// are: the CALLEE of a direct call `f(...)` (invocation borrows the value for
// the call; it neither retains nor frees it), and a reference-equality operand
// `f = g` / `f <> g` (§27.7.1). Every other position — a call ARGUMENT (the
// callee may store the function value in a field that outlives the call, since
// function-typed fields are permitted and runtime-managed), an
// assignment/return/throw/yield/discard operand, a binding to another local, a
// `new`/`with`/collection element, a lambda capture, a `move`, a `for ... in f`
// iterable, etc. — is an ESCAPE. The walk OVER-approximates escape: any
// construct it does not explicitly recognize as a safe read leaves the local
// escaped (→ no release → leak, the safe direction). Only a local that NEVER
// escapes is released at scope end, where releasing its single producer
// reference is provably free of UAF (no other reference exists) and of
// double-free (once per scope exit). Captures are not touched on release
// (they are borrows, §28.9), so there is no double-dispose of a capture either.
//
// `safeRead`: true when a bare identifier in the current expression position is
// a non-consuming read; false when it would escape.
struct ClosureEscapeWalker {
    const sema::SemaContext& ctx;
    const void* target;   // the LocalDecl node we are testing
    bool escaped = false;
    // FINDING [1] / §28.9 / §28.3: true while walking inside a lambda body. A
    // captured closure local read inside a nested lambda is held by that
    // lambda's env, which outlives the producing scope, so it must be an ESCAPE
    // even in an otherwise-safe-read position (a call CALLEE `cap()` or a
    // reference-equality operand). See the String walker for the full rationale.
    bool inLambda = false;

    bool isTargetIdent(const ast::Expr* e) {
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(e);
        if (!id) return false;
        auto* loc = dynamic_cast<sema::LocalSymbol*>(ctx.exprSymbol(*id));
        return loc && loc->decl &&
               static_cast<const void*>(loc->decl) == target;
    }

    void expr(const ast::Expr* e, bool safeRead) {
        if (!e || escaped) return;
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e)) {
            // FINDING [1]: inside a lambda body, a captured-local read always
            // escapes — the env outlives the scope (safe-read does not apply).
            if (isTargetIdent(id) && (!safeRead || inLambda)) escaped = true;
            return;
        }
        // Reference equality `f = g` / `f <> g` (§27.7.1): both operands are
        // non-consuming reads. (Any other binary op on a function value is not
        // valid, but a contained occurrence is still treated as a read here —
        // harmless, since the only ops the grammar admits on function values are
        // `=`/`<>`.)
        if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
            expr(b->left.get(), /*safeRead=*/true);
            expr(b->right.get(), /*safeRead=*/true);
            return;
        }
        // Call: the callee `f(...)` is a non-consuming read (invocation); every
        // ARGUMENT escapes (the callee may store the function value).
        if (auto* c = dynamic_cast<const ast::CallExpr*>(e)) {
            expr(c->callee.get(), /*safeRead=*/true);
            for (auto& a : c->args) expr(a.value.get(), /*safeRead=*/false);
            return;
        }
        if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
            // `f?(...)` null-conditional call (§27.7): callee is a read.
            expr(c->callee.get(), /*safeRead=*/true);
            for (auto& a : c->args) expr(a.value.get(), /*safeRead=*/false);
            return;
        }
        // `f!` forwards f onward — inherit the parent position.
        if (auto* p = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
            expr(p->operand.get(), safeRead);
            return;
        }
        // Conditional: condition is a read; each branch forwards the value on to
        // the parent position, so inherit `safeRead`.
        if (auto* c = dynamic_cast<const ast::ConditionalExpr*>(e)) {
            expr(c->condition.get(), /*safeRead=*/true);
            expr(c->thenValue.get(), safeRead);
            expr(c->elseValue.get(), safeRead);
            return;
        }
        // `f is T` reads only.
        if (auto* t = dynamic_cast<const ast::TypeTestExpr*>(e)) {
            expr(t->operand.get(), /*safeRead=*/true);
            return;
        }
        // Member access / indexing / interpolation do not apply to a function
        // value; treat any contained occurrence conservatively as an ESCAPE
        // (safeRead=false) rather than recognize a safe read that cannot occur.
        if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            expr(m->target.get(), /*safeRead=*/false);
            return;
        }
        if (auto* m = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
            expr(m->target.get(), /*safeRead=*/false);
            return;
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
            expr(ix->target.get(), /*safeRead=*/false);
            for (auto& a : ix->indices) expr(a.value.get(), /*safeRead=*/false);
            return;
        }
        // Everything that can carry the value to a longer-lived location is an
        // ESCAPE for any contained occurrence: new/with/collection literal/
        // lambda/case/unary(`move`)/throw-expr. Recurse with safeRead=false.
        if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
            expr(u->operand.get(), /*safeRead=*/false); return;
        }
        if (auto* th = dynamic_cast<const ast::ThrowExpr*>(e)) {
            expr(th->operand.get(), /*safeRead=*/false); return;
        }
        if (auto* n = dynamic_cast<const ast::NewExpr*>(e)) {
            for (auto& a : n->args) expr(a.value.get(), /*safeRead=*/false);
            return;
        }
        if (auto* w = dynamic_cast<const ast::WithExpr*>(e)) {
            expr(w->source.get(), /*safeRead=*/false);
            for (auto& a : w->assigns) expr(a.value.get(), /*safeRead=*/false);
            return;
        }
        if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
            for (auto& el : cl->elements) expr(el.get(), /*safeRead=*/false);
            for (auto& k : cl->keys) expr(k.get(), /*safeRead=*/false);
            return;
        }
        if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(e)) {
            // FINDING [1] / §28.9 / §28.3: a lambda captures the local into its
            // heap env, which outlives the scope. Walk the body with inLambda
            // set so EVERY read of the captured target — even a call callee
            // `cap()` or a reference-equality operand — is flagged as an escape,
            // preventing a scope-end release of a value the env still aliases.
            bool savedInLambda = inLambda;
            inLambda = true;
            if (lam->exprBody) expr(lam->exprBody.get(), /*safeRead=*/false);
            for (auto& s : lam->stmts) stmt(s.get());
            inLambda = savedInLambda;
            return;
        }
        if (auto* ce = dynamic_cast<const ast::CaseExpr*>(e)) {
            expr(ce->scrutinee.get(), /*safeRead=*/false);
            for (auto& arm : ce->arms) {
                expr(arm.guard.get(), /*safeRead=*/false);
                expr(arm.exprBody.get(), /*safeRead=*/false);
                for (auto& s : arm.stmts) stmt(s.get());
            }
            return;
        }
        // Range endpoints read only (a function value cannot be a range bound,
        // but recurse defensively as reads).
        if (auto* r = dynamic_cast<const ast::RangeExpr*>(e)) {
            expr(r->lo.get(), /*safeRead=*/true);
            expr(r->hi.get(), /*safeRead=*/true);
            expr(r->step.get(), /*safeRead=*/true);
            return;
        }
        // Leaf literals contain no identifiers — nothing to do. An unrecognized
        // composite the grammar can produce is one of the cases above.
    }

    void stmt(const ast::Stmt* s) {
        if (!s || escaped) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            // Binding the target into ANOTHER local escapes it (the new local
            // may itself be returned/stored).
            expr(ld->initializer.get(), /*safeRead=*/false);
            return;
        }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s)) {
            expr(lc->value.get(), /*safeRead=*/false); return;
        }
        if (auto* r = dynamic_cast<const ast::ReturnStmt*>(s)) {
            expr(r->value.get(), /*safeRead=*/false); return;
        }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s)) {
            // A discarded expression (e.g. `f(args);` — the call callee is a
            // safe read, the args escape; decided by expr()).
            expr(es->expr.get(), /*safeRead=*/false); return;
        }
        if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(s)) {
            expr(ts->value.get(), /*safeRead=*/false); return;
        }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
            // Reassigning the local itself → escape (overwrite handling is out of
            // this increment). The RHS escapes.
            if (isTargetIdent(as->target.get())) { escaped = true; return; }
            expr(as->target.get(), /*safeRead=*/false);
            expr(as->value.get(), /*safeRead=*/false);
            return;
        }
        if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(s)) {
            if (isTargetIdent(inc->target.get())) { escaped = true; return; }
            expr(inc->target.get(), /*safeRead=*/false); return;
        }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(s)) {
            expr(ds->value.get(), /*safeRead=*/false); return;
        }
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s)) {
            expr(ys->value.get(), /*safeRead=*/false); return;
        }
        if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
            expr(is->ifBranch.condition.get(), /*safeRead=*/true);
            for (auto& b : is->ifBranch.body) stmt(b.get());
            for (auto& br : is->elifBranches) {
                expr(br.condition.get(), /*safeRead=*/true);
                for (auto& b : br.body) stmt(b.get());
            }
            for (auto& b : is->elseBody) stmt(b.get());
            return;
        }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
            expr(ws->condition.get(), /*safeRead=*/true);
            for (auto& b : ws->body) stmt(b.get());
            return;
        }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
            // `for var x in f do ... end` — iterating a function value is not
            // valid, but be conservative: an escape.
            expr(fs->iterable.get(), /*safeRead=*/false);
            expr(fs->withExpr.get(), /*safeRead=*/false);
            for (auto& b : fs->body) stmt(b.get());
            return;
        }
        if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
            for (auto& b : d->body) stmt(b.get());
            return;
        }
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
            for (auto& b : ts->body) stmt(b.get());
            for (auto& c : ts->catches) {
                expr(c.guard.get(), /*safeRead=*/true);
                stmt(c.body.get());
            }
            for (auto& b : ts->finallyBody) stmt(b.get());
            return;
        }
        if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
            expr(cs->scrutinee.get(), /*safeRead=*/false);
            for (auto& arm : cs->arms) {
                expr(arm.guard.get(), /*safeRead=*/false);
                expr(arm.exprBody.get(), /*safeRead=*/false);
                for (auto& a : arm.stmts) stmt(a.get());
            }
            return;
        }
        // BreakStmt / ContinueStmt: no sub-expressions. The enumeration mirrors
        // the complete set walked by UnwindIneligibleWalker / StringEscapeWalker.
    }
};
}  // namespace


void Emitter::collectClosureReleaseEligible(
        const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    closureReleaseEligible_.clear();
    // Find every `var` (non-using) closure LocalDecl whose initializer is a
    // codegen-internal +1 producer (a lambda / method reference), then run the
    // escape walk for each. A decl survives as eligible iff the walk proves it
    // never escapes. (Function-typed locals never appear in `using var` — §28.9
    // — but the isUsing guard mirrors the string path defensively.)
    std::vector<const ast::LocalDecl*> candidates;
    std::function<void(const ast::Stmt*)> scan = [&](const ast::Stmt* s) {
        if (!s) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            if (!ld->isUsing && isPlusOneClosureProducer(ld->initializer.get()))
                candidates.push_back(ld);
        }
        if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
            for (auto& b : is->ifBranch.body) scan(b.get());
            for (auto& br : is->elifBranches)
                for (auto& b : br.body) scan(b.get());
            for (auto& b : is->elseBody) scan(b.get());
        } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
            for (auto& b : ws->body) scan(b.get());
        } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
            for (auto& b : fs->body) scan(b.get());
        } else if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
            for (auto& b : d->body) scan(b.get());
        } else if (auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
            for (auto& b : ts->body) scan(b.get());
            for (auto& c : ts->catches) scan(c.body.get());
            for (auto& b : ts->finallyBody) scan(b.get());
        } else if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
            for (auto& arm : cs->arms)
                for (auto& a : arm.stmts) scan(a.get());
        }
    };
    for (auto& s : body) scan(s.get());

    for (const ast::LocalDecl* ld : candidates) {
        ClosureEscapeWalker w{ctx_, static_cast<const void*>(ld)};
        for (auto& s : body) { w.stmt(s.get()); if (w.escaped) break; }
        if (!w.escaped)
            closureReleaseEligible_.insert(static_cast<const void*>(ld));
    }
}


void Emitter::collectStringReleaseEligible(
        const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    stringReleaseEligible_.clear();
    // Find every `var` (non-using) string LocalDecl whose initializer is a
    // codegen-internal +1 producer, then run the escape walk for each. A decl
    // survives as eligible iff the walk proves it never escapes.
    std::vector<const ast::LocalDecl*> candidates;
    std::function<void(const ast::Stmt*)> scan = [&](const ast::Stmt* s) {
        if (!s) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            if (!ld->isUsing && isPlusOneStringProducer(ld->initializer.get()))
                candidates.push_back(ld);
        }
        // Recurse into nested blocks so locals declared inside if/while/for/
        // do/try/case bodies are also considered.
        if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
            for (auto& b : is->ifBranch.body) scan(b.get());
            for (auto& br : is->elifBranches)
                for (auto& b : br.body) scan(b.get());
            for (auto& b : is->elseBody) scan(b.get());
        } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
            for (auto& b : ws->body) scan(b.get());
        } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
            for (auto& b : fs->body) scan(b.get());
        } else if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
            for (auto& b : d->body) scan(b.get());
        } else if (auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
            for (auto& b : ts->body) scan(b.get());
            for (auto& c : ts->catches) scan(c.body.get());
            for (auto& b : ts->finallyBody) scan(b.get());
        } else if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
            for (auto& arm : cs->arms)
                for (auto& a : arm.stmts) scan(a.get());
        }
    };
    for (auto& s : body) scan(s.get());

    for (const ast::LocalDecl* ld : candidates) {
        StringEscapeWalker w{ctx_, static_cast<const void*>(ld)};
        for (auto& s : body) { w.stmt(s.get()); if (w.escaped) break; }
        if (!w.escaped)
            stringReleaseEligible_.insert(static_cast<const void*>(ld));
    }
}


// #422 / §8.6 / §28.9: is local `ld` a codegen-internal +1 box producer? It is
// when the declared target type is Object or an interface AND the initializer's
// static type is a boxable VALUE type (record/enum/primitive) or a nullable
// value type — i.e. exactly the cases boxForTarget/boxValue allocate a fresh
// rc=1 box wrapper for. A class instance coerced to Object (the #399 path) has a
// REFERENCE initializer (class/interface/string/Object) for which boxForTarget
// is a no-op — NOT a producer, never released, so the class model is untouched.
// An INFERRED local (no `: Type`) is not a producer either: without a declared
// Object/interface target boxForTarget never boxes (the local takes the value
// type directly). Mirrors the boxForTarget gate in lowerLocalDecl exactly.
bool Emitter::isPlusOneBoxProducer(const ast::LocalDecl* ld) {
    if (!ld || !ld->type || !ld->initializer) return false;
    const sema::TypeRef* declTy = ctx_.resolvedType(*ld->type);
    if (!isObjectOrInterfaceTarget(declTy)) return false;
    const sema::TypeRef* srcTy = ctx_.exprType(*ld->initializer);
    if (!srcTy) return false;
    return isBoxableValueType(srcTy) || isNullableValueTy(srcTy);
}

namespace {
// #422: a strict, conservative walk that decides whether a SPECIFIC boxed-value
// Object/interface `var` local (identified by its LocalDecl node) ever escapes
// its scope. The box wrapper is a fresh +1 the local uniquely holds; it is safe
// to release at scope end ONLY if no other reference to the wrapper can exist.
// A boxed Object's only NON-CONSUMING reads are: the operand of `is`/`is not`,
// the operand of `as` (which copies the payload bytes — it never retains the box
// pointer), and a `= null` / `<> null` comparison. EVERY other position — a call
// argument, a METHOD/PROPERTY receiver (an interface/Object method call could
// stash the receiver), an assignment/return/throw/yield/discard operand, a
// binding to another local, a `new`/`with`/collection element, a lambda capture,
// a `move`, a `for ... in` iterable, a reassignment of the local — is treated as
// an ESCAPE. The walk is an OVER-approximation of escape: any construct it does
// not explicitly recognize as a safe read leaves the local escaped (→ no release
// → leak, the safe direction). Note this is STRICTER than the string walker:
// a string method receiver is a safe read (strings are immutable leaves the
// runtime owns), but a boxed-Object method receiver is treated as an escape,
// because `o.SomeMethod()` dispatches through the box and the callee could
// retain the receiver. Only a local that NEVER escapes is released at scope end.
struct BoxEscapeWalker {
    const sema::SemaContext& ctx;
    const void* target;   // the LocalDecl node we are testing
    bool escaped = false;
    // FINDING [1] / §28.9 / §28.3: true while walking inside a lambda body. A
    // captured boxed-Object local read inside a lambda is held by the lambda's
    // env, which outlives the producing scope, so it must be an ESCAPE even in
    // an otherwise-safe-read position (`cap is T` / `cap as T` / `cap = null`).
    // See the String walker for the full rationale.
    bool inLambda = false;

    bool isTargetIdent(const ast::Expr* e) {
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(e);
        if (!id) return false;
        auto* loc = dynamic_cast<sema::LocalSymbol*>(ctx.exprSymbol(*id));
        return loc && loc->decl &&
               static_cast<const void*>(loc->decl) == target;
    }

    void expr(const ast::Expr* e, bool safeRead) {
        if (!e || escaped) return;
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e)) {
            // FINDING [1]: inside a lambda body, a captured-local read always
            // escapes — the env outlives the scope (safe-read does not apply).
            if (isTargetIdent(id) && (!safeRead || inLambda)) escaped = true;
            return;
        }
        // `o is T` / `o as T`: the operand is a non-consuming read (`is` walks
        // the descriptor; `as` to a value type copies payload bytes, to a
        // reference type yields the pointer via select — neither retains the box
        // for longer than the expression).
        if (auto* t = dynamic_cast<const ast::TypeTestExpr*>(e)) {
            expr(t->operand.get(), /*safeRead=*/true);
            return;
        }
        // `o = null` / `o <> null`: a null comparison reads only. A comparison
        // against a NON-null operand is treated as an escape (defensive: it could
        // be `o = somethingThatAliases`); only the literal-null forms are safe.
        if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
            if (b->op == "=" || b->op == "<>") {
                bool lNull = dynamic_cast<const ast::NullLiteralExpr*>(
                                 b->left.get()) != nullptr;
                bool rNull = dynamic_cast<const ast::NullLiteralExpr*>(
                                 b->right.get()) != nullptr;
                expr(b->left.get(), /*safeRead=*/rNull);
                expr(b->right.get(), /*safeRead=*/lNull);
                return;
            }
            // Any other binary op on an Object operand is unusual; treat both
            // operands as escapes (conservative).
            expr(b->left.get(), /*safeRead=*/false);
            expr(b->right.get(), /*safeRead=*/false);
            return;
        }
        // A METHOD / PROPERTY receiver is an ESCAPE for a boxed Object (the call
        // dispatches through the box and the callee could retain the receiver).
        if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            expr(m->target.get(), /*safeRead=*/false);
            return;
        }
        if (auto* m = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
            expr(m->target.get(), /*safeRead=*/false);
            return;
        }
        if (auto* c = dynamic_cast<const ast::CallExpr*>(e)) {
            expr(c->callee.get(), /*safeRead=*/false);
            for (auto& a : c->args) expr(a.value.get(), /*safeRead=*/false);
            return;
        }
        if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
            expr(c->callee.get(), /*safeRead=*/false);
            for (auto& a : c->args) expr(a.value.get(), /*safeRead=*/false);
            return;
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
            expr(ix->target.get(), /*safeRead=*/false);
            for (auto& a : ix->indices) expr(a.value.get(), /*safeRead=*/false);
            return;
        }
        // `o!` forwards o onward — inherit the parent position.
        if (auto* p = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
            expr(p->operand.get(), safeRead);
            return;
        }
        // Conditional: condition reads; each branch forwards the value to the
        // parent position (inherit safeRead).
        if (auto* c = dynamic_cast<const ast::ConditionalExpr*>(e)) {
            expr(c->condition.get(), /*safeRead=*/true);
            expr(c->thenValue.get(), safeRead);
            expr(c->elseValue.get(), safeRead);
            return;
        }
        // Everything else that can carry the box to a longer-lived location is an
        // ESCAPE: new/with/collection/lambda/case/unary(`move`)/throw-expr/
        // interpolation/range. Recurse with safeRead=false so any occurrence
        // taints, and bail conservatively.
        if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
            expr(u->operand.get(), /*safeRead=*/false); return;
        }
        if (auto* th = dynamic_cast<const ast::ThrowExpr*>(e)) {
            expr(th->operand.get(), /*safeRead=*/false); return;
        }
        if (auto* n = dynamic_cast<const ast::NewExpr*>(e)) {
            for (auto& a : n->args) expr(a.value.get(), /*safeRead=*/false);
            return;
        }
        if (auto* w = dynamic_cast<const ast::WithExpr*>(e)) {
            expr(w->source.get(), /*safeRead=*/false);
            for (auto& a : w->assigns) expr(a.value.get(), /*safeRead=*/false);
            return;
        }
        if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
            for (auto& el : cl->elements) expr(el.get(), /*safeRead=*/false);
            for (auto& k : cl->keys) expr(k.get(), /*safeRead=*/false);
            return;
        }
        if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
            for (auto& part : is->parts) expr(part.expr.get(), /*safeRead=*/false);
            return;
        }
        if (auto* r = dynamic_cast<const ast::RangeExpr*>(e)) {
            expr(r->lo.get(), /*safeRead=*/false);
            expr(r->hi.get(), /*safeRead=*/false);
            expr(r->step.get(), /*safeRead=*/false);
            return;
        }
        if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(e)) {
            // FINDING [1] / §28.9 / §28.3: a lambda captures the local into its
            // heap env, which outlives the scope. Walk the body with inLambda
            // set so EVERY read of the captured target — even `cap is T` /
            // `cap as T` / `cap = null` — is flagged as an escape, preventing a
            // scope-end release of a value the env still aliases.
            bool savedInLambda = inLambda;
            inLambda = true;
            if (lam->exprBody) expr(lam->exprBody.get(), /*safeRead=*/false);
            for (auto& s : lam->stmts) stmt(s.get());
            inLambda = savedInLambda;
            return;
        }
        if (auto* ce = dynamic_cast<const ast::CaseExpr*>(e)) {
            expr(ce->scrutinee.get(), /*safeRead=*/false);
            for (auto& arm : ce->arms) {
                expr(arm.guard.get(), /*safeRead=*/false);
                expr(arm.exprBody.get(), /*safeRead=*/false);
                for (auto& s : arm.stmts) stmt(s.get());
            }
            return;
        }
        // Leaf literals contain no identifiers — nothing to do.
    }

    void stmt(const ast::Stmt* s) {
        if (!s || escaped) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            // Binding the target into ANOTHER local escapes it (the new local may
            // be returned/stored/aliased). Walk the initializer as an escape
            // position — its sub-reads (e.g. `o as T`) are still recognized.
            expr(ld->initializer.get(), /*safeRead=*/false);
            return;
        }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s)) {
            expr(lc->value.get(), /*safeRead=*/false); return;
        }
        if (auto* r = dynamic_cast<const ast::ReturnStmt*>(s)) {
            expr(r->value.get(), /*safeRead=*/false); return;
        }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s)) {
            expr(es->expr.get(), /*safeRead=*/false); return;
        }
        if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(s)) {
            expr(ts->value.get(), /*safeRead=*/false); return;
        }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
            // Reassigning the local itself is an escape (overwrite handling is
            // out of this increment); the RHS escapes.
            if (isTargetIdent(as->target.get())) { escaped = true; return; }
            expr(as->target.get(), /*safeRead=*/false);
            expr(as->value.get(), /*safeRead=*/false);
            return;
        }
        if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(s)) {
            if (isTargetIdent(inc->target.get())) { escaped = true; return; }
            expr(inc->target.get(), /*safeRead=*/false); return;
        }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(s)) {
            expr(ds->value.get(), /*safeRead=*/false); return;
        }
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s)) {
            expr(ys->value.get(), /*safeRead=*/false); return;
        }
        if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
            expr(is->ifBranch.condition.get(), /*safeRead=*/true);
            for (auto& b : is->ifBranch.body) stmt(b.get());
            for (auto& br : is->elifBranches) {
                expr(br.condition.get(), /*safeRead=*/true);
                for (auto& b : br.body) stmt(b.get());
            }
            for (auto& b : is->elseBody) stmt(b.get());
            return;
        }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
            expr(ws->condition.get(), /*safeRead=*/true);
            for (auto& b : ws->body) stmt(b.get());
            return;
        }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
            expr(fs->iterable.get(), /*safeRead=*/false);
            expr(fs->withExpr.get(), /*safeRead=*/false);
            for (auto& b : fs->body) stmt(b.get());
            return;
        }
        if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
            for (auto& b : d->body) stmt(b.get());
            return;
        }
        if (auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
            for (auto& b : ts->body) stmt(b.get());
            for (auto& c : ts->catches) {
                expr(c.guard.get(), /*safeRead=*/true);
                stmt(c.body.get());
            }
            for (auto& b : ts->finallyBody) stmt(b.get());
            return;
        }
        if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
            expr(cs->scrutinee.get(), /*safeRead=*/false);
            for (auto& arm : cs->arms) {
                expr(arm.guard.get(), /*safeRead=*/false);
                expr(arm.exprBody.get(), /*safeRead=*/false);
                for (auto& a : arm.stmts) stmt(a.get());
            }
            return;
        }
        // BreakStmt / ContinueStmt: no sub-expressions.
    }
};
}  // namespace


void Emitter::collectBoxReleaseEligible(
        const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    boxReleaseEligible_.clear();
    // Find every `var` (non-using) Object/interface LocalDecl whose initializer
    // is a codegen-internal +1 box producer, then run the escape walk for each.
    // A decl survives as eligible iff the walk proves it never escapes. (`using
    // var` is excluded defensively, mirroring the string/closure paths; boxed
    // value types never carry a disposal obligation so they never appear in a
    // meaningful `using var`.)
    std::vector<const ast::LocalDecl*> candidates;
    std::function<void(const ast::Stmt*)> scan = [&](const ast::Stmt* s) {
        if (!s) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            if (!ld->isUsing && isPlusOneBoxProducer(ld))
                candidates.push_back(ld);
        }
        if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
            for (auto& b : is->ifBranch.body) scan(b.get());
            for (auto& br : is->elifBranches)
                for (auto& b : br.body) scan(b.get());
            for (auto& b : is->elseBody) scan(b.get());
        } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
            for (auto& b : ws->body) scan(b.get());
        } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
            for (auto& b : fs->body) scan(b.get());
        } else if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
            for (auto& b : d->body) scan(b.get());
        } else if (auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
            for (auto& b : ts->body) scan(b.get());
            for (auto& c : ts->catches) scan(c.body.get());
            for (auto& b : ts->finallyBody) scan(b.get());
        } else if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
            for (auto& arm : cs->arms)
                for (auto& a : arm.stmts) scan(a.get());
        }
    };
    for (auto& s : body) scan(s.get());

    for (const ast::LocalDecl* ld : candidates) {
        BoxEscapeWalker w{ctx_, static_cast<const void*>(ld)};
        for (auto& s : body) { w.stmt(s.get()); if (w.escaped) break; }
        if (!w.escaped)
            boxReleaseEligible_.insert(static_cast<const void*>(ld));
    }
}


// #383 / §28: a reference TypeRef carrying a vtable — a user class or
// interface (Named, with a class/interface declaration). Such objects are
// disposed dynamically through their own vtable slot 1.
static bool isClassOrInterfaceRef(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return false;
    return rt->declaration->kind == ast::TypeDeclKind::Class ||
           rt->declaration->kind == ast::TypeDeclKind::Interface;
}


bool Emitter::usingVarOwnsClassObjectDynamically(const ast::LocalDecl* ld) {
    // (a) Interface-typed slot. classOfLocal returns the interface symbol but
    // an interface has no disposerFns_ entry; the concrete object behind it
    // still has a vtable, so dispose it dynamically. An interface-typed slot
    // initialized by `new`/factory always holds an owned class instance here
    // (matching the existing class-annotation policy, which disposes a
    // class-typed using var regardless of owned/borrow — see
    // abstract_dynamic_dispose_test's using_var_subclass_disposed_dynamically).
    if (ld->type) {
        const sema::TypeRef* rt = ctx_.resolvedType(*ld->type);
        if (rt && rt->kind == sema::TypeRef::Kind::Named && rt->declaration &&
            rt->declaration->kind == ast::TypeDeclKind::Interface)
            return true;
    }
    // (b) An inferred using var whose initializer is a call returning `owned T`
    // (T a class/interface). §28.10: a borrow return (no `owned`) assumes NO
    // disposal — so only the `owned`-return case registers here. A `new`/
    // class-annotation initializer was already handled by classOfLocal; an
    // interface-annotation by (a). This case covers `using var x := Make();`
    // where `Make(): owned IShape` / `owned Square` and x has no annotation.
    if (!ld->type) {
        if (auto* call = dynamic_cast<const ast::CallExpr*>(
                ld->initializer.get())) {
            if (call->callee) {
                auto* ms = dynamic_cast<sema::MethodSymbol*>(
                    ctx_.exprSymbol(*call->callee));
                if (ms) {
                    auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
                    if (md && md->returnOwned &&
                        isClassOrInterfaceRef(ctx_.exprType(*ld->initializer)))
                        return true;
                }
            }
        }
    }
    return false;
}


bool Emitter::usingVarOwnsBuiltinException(const ast::LocalDecl* ld) {
    // #405 / §28.10 / §31.1: the `using var` owns a BUILTIN exception object
    // (Exception or a §31.4 subtype). Recognize it from the declared type, a
    // `new <builtin-exception>(...)` initializer, or the initializer's resolved
    // type. A built-in exception is an owned heap class instance; its block must
    // be reclaimed at scope end (dispatch is dynamic, vtable slot 1).
    if (ld->type && isBuiltinExceptionTypeRef(ctx_.resolvedType(*ld->type)))
        return true;
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(ld->initializer.get())) {
        if (ne->type && isBuiltinExceptionTypeRef(ctx_.resolvedType(*ne->type)))
            return true;
    }
    if (ld->initializer &&
        isBuiltinExceptionTypeRef(ctx_.exprType(*ld->initializer)))
        return true;
    return false;
}


bool Emitter::usingVarIsMovedClassObject(const ast::LocalDecl* ld) {
    // #383 / §28.8.1: `using var u := move <expr>` — the moved value is owned
    // and discharged at scope end. Inside a destructor body the field's
    // auto-dispose pass would also dispose it (codegen has no moved-from
    // tracking), so suppress there.
    if (emittingDisposerBody_) return false;
    auto* un = dynamic_cast<const ast::UnaryExpr*>(ld->initializer.get());
    if (!un || un->op != "move") return false;
    // The moved value's type is the field/slot element type; require a
    // class/interface object (records and value types free their own storage
    // and never reach the dynamic-dispose path).
    return isClassOrInterfaceRef(ctx_.exprType(*ld->initializer));
}


const sema::TypeRef* Emitter::collectionTypeOfLocal(const ast::LocalDecl* ld) {
    // #362: recover the local's resolved collection TypeRef (with generic args).
    // Prefer the declared type, then a `new` initializer's type, then the
    // initializer's inferred expression type.
    if (ld->type) {
        if (auto* rt = ctx_.resolvedType(*ld->type))
            if (isCollectionTypeName(rt)) return rt;
    }
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(ld->initializer.get())) {
        if (ne->type) {
            if (auto* rt = ctx_.resolvedType(*ne->type))
                if (isCollectionTypeName(rt)) return rt;
        }
    }
    if (ld->initializer) {
        if (auto* rt = ctx_.exprType(*ld->initializer))
            if (isCollectionTypeName(rt)) return rt;
    }
    return nullptr;
}


const sema::TypeRef* Emitter::dictTypeOfLocal(const ast::LocalDecl* ld) {
    // #394: recover a local's resolved Dictionary/OwnedDictionary TypeRef (with
    // generic args). Mirrors collectionTypeOfLocal but recognizes the dict
    // family (which isCollectionTypeName deliberately excludes — dicts are
    // hash-backed and freed via bz_dict_free, not bz_array_free).
    if (ld->type) {
        if (auto* rt = ctx_.resolvedType(*ld->type))
            if (isDictTypeName(rt)) return rt;
    }
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(ld->initializer.get())) {
        if (ne->type) {
            if (auto* rt = ctx_.resolvedType(*ne->type))
                if (isDictTypeName(rt)) return rt;
        }
    }
    if (ld->initializer) {
        if (auto* rt = ctx_.exprType(*ld->initializer))
            if (isDictTypeName(rt)) return rt;
    }
    return nullptr;
}


const sema::TypeSymbol* Emitter::classOfLocal(const ast::LocalDecl* ld) {
    if (ld->type) {
        if (auto* rt = ctx_.resolvedType(*ld->type)) {
            if (rt->kind == sema::TypeRef::Kind::Named) return rt->declaration;
        }
    }
    if (auto* ne =
            dynamic_cast<const ast::NewExpr*>(ld->initializer.get())) {
        if (ne->type) {
            if (auto* rt = ctx_.resolvedType(*ne->type)) {
                if (rt->kind == sema::TypeRef::Kind::Named)
                    return rt->declaration;
            }
        }
    }
    return nullptr;
}


bool Emitter::lowerScopedBlock(const std::vector<std::unique_ptr<ast::Stmt>>& body,
                      std::size_t startIdx) {
    disposeStack_.emplace_back();
    // §28.3 (line 4910): the function-body ROOT scope (the first one pushed)
    // owns the unwind disposal of the method's `owned` parameters. Register them
    // here, where a scope finally exists, then clear so nested scopes don't
    // re-register. (A lambda/thunk body that opens its own root scope inherits
    // an empty pendingOwnedParams_ unless its own bindParameters filled it.)
    if (disposeStack_.size() == 1 && !pendingOwnedParams_.empty()) {
        auto pending = std::move(pendingOwnedParams_);
        pendingOwnedParams_.clear();
        for (auto& [psym, slot] : pending)
            registerUnwindOwnedParam(psym, slot);
    }
    bool terminated = false;
    for (std::size_t i = startIdx; i < body.size(); ++i) {
        if (body[i] && lowerStmt(*body[i])) { terminated = true; break; }
    }
    if (!terminated) emitDisposals(disposeStack_.back());
    disposeStack_.pop_back();
    return terminated;
}


void Emitter::emitDisposals(const std::vector<Disposal>& scope, bool isUnwind) {
    for (auto it = scope.rbegin(); it != scope.rend(); ++it) {
        // #422 / §4.6 / §28.9: a provably-non-escaping `var` string local.
        // Release its single (producer) reference at scope end — on BOTH the
        // normal and the unwind exit (the reference dies either way; a given
        // exit goes one way only, so this runs exactly once per scope exit). The
        // local was proven non-escaping, so nothing else holds the pointer:
        // bz_string_release frees the data+header exactly once (no UAF, no
        // double-free). bz_string_release is null-safe and a no-op on a
        // sentinel/literal — but an eligible local always holds a heap +1 value.
        if (it->stringRelease) {
            llvm::Value* s = builder_.CreateLoad(ptrTy(), it->slot, "str.rel");
            emitStringRelease(s);
            continue;
        }
        // #422 / §27.5 / §28.9: a provably-non-escaping `var` closure
        // (function-typed) local. Release its single (producer) reference at
        // scope end — on BOTH the normal and unwind exit (the reference dies
        // either way; a given exit goes one way only, so this runs exactly once
        // per scope exit). The local was proven non-escaping, so nothing else
        // holds the cell pointer: bz_closure_release frees the cell + its env
        // block exactly once (no UAF, no double-free of the cell). Captured
        // values are borrows (§28.9), so the env's CONTENTS are not freed — no
        // double-dispose of a capture. bz_closure_release is null-safe.
        if (it->closureRelease) {
            llvm::Value* c = builder_.CreateLoad(ptrTy(), it->slot, "cl.rel");
            emitClosureRelease(c);
            continue;
        }
        // #422 / §8.6 / §28.9: a provably-non-escaping `var` Object/interface
        // local holding a BOXED VALUE. Release its single (producer) reference at
        // scope end — on BOTH the normal and unwind exit (the reference dies
        // either way; a given exit goes one way only, so this runs exactly once
        // per scope exit). The local was proven non-escaping, so nothing else
        // holds the box pointer: bz_box_release frees the box wrapper exactly
        // once (no UAF, no double-free). The boxed value is a value-type leaf, so
        // only the wrapper is freed. This is NEVER a class instance (a class
        // coerced to Object is not a box producer, so it is never registered
        // here) — the class-instance ownership/disposal path is untouched.
        // bz_box_release is null-safe (a null T? boxes to a null Object).
        if (it->boxRelease) {
            llvm::Value* b = builder_.CreateLoad(ptrTy(), it->slot, "box.rel");
            emitBoxRelease(b);
            continue;
        }
        // #383 / §31.1: the caught exception object. On a normal handled exit
        // it is freed (destructor + base chain + block, via its vtable); on the
        // unwind path it is SKIPPED — a re-raise/throw leaving the arm keeps it
        // in flight for the enclosing handler. It is non-null here (it was the
        // in-flight exception that matched this arm).
        if (it->caughtException) {
            if (isUnwind) continue;
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), it->slot);
            // §28.2 / #380: dispatch the idempotent Dispose and reclaim the
            // block once. The caught object is in flight only for this arm and
            // freed exactly here on a normal handled exit (a re-raise skips it
            // via the isUnwind branch above).
            emitDisposeAndReclaim(obj);
            continue;
        }
        // §28.3 (line 4910): a plain `var` owned local / `owned` parameter. It is
        // auto-disposed ONLY when an exception unwinds out of its scope; on a
        // normal exit it must have reached a discharge point (sema-enforced), so
        // disposing it here would double-dispose a moved-out / returned value.
        // On the unwind path: dispatch its idempotent Dispose and reclaim the
        // block once, null-guarded (an uninitialized/conditional owned local's
        // slot may be null — registration is restricted to definitely-assigned,
        // never-moved/disposed owned locals/params, but the null-check is kept
        // for the nullable-owned and defensive cases). The dispatch is dynamic
        // (runtime class), matching every other class-instance dispose site.
        if (it->unwindOnly) {
            if (!isUnwind) continue;
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                obj, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB = llvm::BasicBlock::Create(llvm_, "unwind.dispose",
                                                    currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "unwind.cont",
                                                    currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            emitDisposeAndReclaim(obj);
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // per §2.11: a `using var` Subscription auto-disposes by removing its
        // registration through the events runtime — it has no vtable. (Dispose
        // is idempotent, so a null/already-removed handle is safe.)
        if (it->subscription) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* sub = builder_.CreateLoad(ptrTy(), addr);
            auto* fn = declareC("bz_subscription_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {sub});
            continue;
        }
        // #362 / stdlib §4.5: a `using var` heap collection. Free the backing
        // via bz_array_free; for an owning collection of class elements the
        // element-disposer thunk cascades disposal into each (non-null) element
        // exactly once (the thunk null-checks; the array iterates live slots).
        if (it->collection) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* arr = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* disp = it->collectionElemDisposer
                ? llvm::cast<llvm::Value>(it->collectionElemDisposer)
                : llvm::ConstantPointerNull::get(ptrTy());
            auto* fn = declareC("bz_array_free",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {arr, disp});
            continue;
        }
        // #394 / stdlib §4.9: a `using var` hash-backed dictionary. Free the
        // backing table (and its malloc'd key bytes) via bz_dict_free; for an
        // OwnedDictionary with a class VALUE type the value-disposer thunk
        // cascades disposal into each surviving (non-null) value exactly once
        // (bz_dict_free iterates the live entries; the thunk null-checks). A
        // value DISPLACED earlier by the indexer setter (#388) was already
        // removed from the table and disposed there, so it is not re-disposed
        // here — no double-free. bz_dict_free is null-safe.
        if (it->dict) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* tbl = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* disp = it->dictValueDisposer
                ? llvm::cast<llvm::Value>(it->dictValueDisposer)
                : llvm::ConstantPointerNull::get(ptrTy());
            auto* fn = declareC("bz_dict_free",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {tbl, disp});
            continue;
        }
        // #383 / §28: a `using var` owning a class/interface object whose
        // concrete class is not statically known here (interface-typed slot,
        // call returning `owned T`, or `move x.f`). Dispose dynamically through
        // its own vtable, null-guarded — an interface/nullable slot may hold
        // null. The dynamic dispatch runs the most-derived destructor + base
        // chain (idempotent, §28.2) and reclaims the block once (#380); the slot
        // is nulled afterward so any further disposal through it is a no-op
        // (§28.2; the #396 slot-null precedent).
        if (it->dynamicDispose) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                obj, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB = llvm::BasicBlock::Create(llvm_, "using.dispose",
                                                    currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "using.cont",
                                                    currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            emitDisposeAndReclaim(obj);
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        if (disposerFns_.find(it->cls) == disposerFns_.end()) continue;
        // CG18 / §28.2: dispatch Dispose dynamically through the object's vtable
        // (idempotent) and reclaim the block once, so a `using var` bound to a
        // subclass instance runs the correct runtime Dispose. This scope-end
        // disposal runs exactly once per scope (normal and unwind exits are
        // mutually exclusive; a break/continue terminates the block first).
        // §27.5: a captured ("boxed") `using var` local lives in a shared
        // heap cell; load the object through the cell, not the cell pointer.
        // #415 / §28.2 / §28.5 (line 5066): an earlier explicit `x.Dispose()`
        // now RECLAIMS the block and NULLS this slot (the explicit-dispose
        // lowering in LlvmBackend_Calls.cpp), so this scope-end site must skip a
        // null slot — reloading null and calling emitDisposeAndReclaim(null)
        // would dispatch through a null vtable. Null-guard (mirroring the
        // dynamicDispose path) and null the slot after reclaim so the object is
        // freed EXACTLY ONCE across the explicit + scope-end paths (no double-
        // free): whichever site disposes first reclaims and nulls; the other
        // finds null and skips.
        llvm::Type* ety = nullptr;
        llvm::Value* addr = boxedStorage(it->slot, ety);
        llvm::Value* obj = builder_.CreateLoad(ptrTy(), addr);
        llvm::Value* isNull = builder_.CreateICmpEQ(
            obj, llvm::ConstantPointerNull::get(ptrTy()));
        auto* dispBB = llvm::BasicBlock::Create(llvm_, "using.cls.dispose",
                                                currentFn_);
        auto* contBB = llvm::BasicBlock::Create(llvm_, "using.cls.cont",
                                                currentFn_);
        builder_.CreateCondBr(isNull, contBB, dispBB);
        builder_.SetInsertPoint(dispBB);
        emitDisposeAndReclaim(obj);
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(contBB);
    }
}


void Emitter::emitAllDisposals() {
    for (auto it = disposeStack_.rbegin(); it != disposeStack_.rend(); ++it)
        emitDisposals(*it);
}


void Emitter::emitLoopDisposals(std::size_t loopScopeDepth) {
    for (std::size_t i = disposeStack_.size(); i-- > loopScopeDepth;)
        emitDisposals(disposeStack_[i]);
}


bool Emitter::lowerBody(const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    for (const auto& s : body) {
        if (s && lowerStmt(*s)) return true;
    }
    return false;
}


// §8.6: the stable spelling of each boxable primitive (and string), used to key
// primBoxDesc_/primBoxVtable_ and to match a type-test target's name.
static const char* primBoxName(sema::PrimitiveType p) {
    using P = sema::PrimitiveType;
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
        default:         return nullptr;  // Object / Decimal / None: no box desc
    }
}


// §8.6: build the box descriptor (vtable[0] type-tag array) + box vtable globals
// for boxable value types — user records and enums, and the built-in primitives
// + string. A boxed value is `{ vtable*, value-bytes }`; its first word points
// here so `emitIsA` (`*obj` → vtable, `vtable[0]` → descriptor array) recognizes
// the boxed value's dynamic type uniformly with a class instance. For a record
// that implements interfaces (§9.6), the box vtable's interface-method slots are
// filled with the record's implementations so virtual dispatch on a boxed record
// reaches the right method. Runs after declareVtables/setupBuiltinExceptions so
// every interface descriptor (used in a record's descriptor chain) already
// exists.
void Emitter::setupBoxDescriptors() {
    // --- Records & enums: descriptor + box vtable -------------------------
    // Pass A: declare a descriptor-array global for each record/enum (sized by
    // its [self + transitive bases] chain). Records list their interfaces in
    // baseTypes, so `o is IShape` on a boxed Rect works too.
    std::unordered_map<const sema::TypeSymbol*,
                       std::vector<const sema::TypeSymbol*>> supersOf;
    for (const auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (!ts) continue;
        if (ts->kind != ast::TypeDeclKind::Record &&
            ts->kind != ast::TypeDeclKind::Enum)
            continue;
        if (descArray_.count(ts)) continue;  // never overwrite a class/iface
        std::vector<const sema::TypeSymbol*> supers;
        collectTypeAndBases(ts, supers);
        supersOf[ts] = supers;
        auto* arrTy = llvm::ArrayType::get(ptrTy(), supers.size() + 1);
        descArray_[ts] = new llvm::GlobalVariable(
            *module_, arrTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage, nullptr,
            "btd$" + std::string(ts->name));
    }
    // Pass B: fill each descriptor with [self + transitive bases, null]. A base
    // that is itself a record/enum is in descArray_ from Pass A; an interface
    // base is in descArray_ from declareVtables.
    for (auto& [ts, supers] : supersOf) {
        std::vector<llvm::Constant*> elems;
        for (const sema::TypeSymbol* s : supers) {
            auto it = descArray_.find(s);
            llvm::Constant* d = it != descArray_.end()
                ? llvm::cast<llvm::Constant>(it->second)
                : llvm::ConstantPointerNull::get(ptrTy());
            elems.push_back(d);
        }
        elems.push_back(llvm::ConstantPointerNull::get(ptrTy()));
        auto* arrTy = llvm::cast<llvm::ArrayType>(
            descArray_[ts]->getValueType());
        descArray_[ts]->setInitializer(
            llvm::ConstantArray::get(arrTy, elems));
    }
    // Pass C: build each record's box vtable. The layout matches a class
    // vtable's prefix — [descArray, Dispose, iface-slots…] — so interface
    // dispatch (`vtable[ifaceSlot]`) on a boxed record finds the method. Enums
    // get the same shape (no iface methods filled). Slot 1 (Dispose) is the
    // shared no-op: boxed value types are runtime-managed (§28.9) and own
    // nothing.
    for (auto& [ts, supers] : supersOf) {
        std::vector<llvm::Constant*> elems(2 + ifaceSlotCount_,
                                           llvm::ConstantPointerNull::get(
                                               ptrTy()));
        elems[0] = descArray_[ts];
        if (builtinDisposeFn_) elems[1] = builtinDisposeFn_;
        // §9.6 / §8.6: fill the interface-method slots this record implements.
        // The real record method takes `this` = a pointer to the bare record
        // bytes (field indices from 0). A boxed record's receiver, however, is
        // the BOX pointer { vtable*, i64 rc, record-bytes } (#422); the record
        // bytes start TWO words in (past the vtable word and the rc word). So the
        // box vtable stores a small THUNK that advances `this` past both words,
        // then tail-calls the real method. This keeps the class-dispatch
        // convention (this at offset 0) untouched.
        std::unordered_set<const sema::TypeSymbol*> superSet(supers.begin(),
                                                             supers.end());
        for (const auto& [im, slot] : ifaceMethodSlot_) {
            if (!im->parent || !superSet.count(im->parent)) continue;
            sema::MethodSymbol* impl = findMethodByName(ts, im->name);
            if (!impl) continue;
            auto mit = methodFns_.find(impl);
            if (mit == methodFns_.end()) continue;
            llvm::Function* real = mit->second;
            llvm::FunctionType* fnTy = real->getFunctionType();
            auto* thunk = llvm::Function::Create(
                fnTy, llvm::Function::InternalLinkage,
                "bz$boxthunk$" + std::string(ts->name) + "$" +
                    std::string(im->name),
                module_.get());
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", thunk);
            llvm::IRBuilder<> tb(bb);
            std::vector<llvm::Value*> callArgs;
            auto ai = thunk->arg_begin();
            // this (arg 0): box ptr → record bytes (+2 words: past vtable + rc).
            llvm::Value* recThis = tb.CreateGEP(
                ptrTy(), &*ai, {llvm::ConstantInt::get(i32(), 2)}, "boxthis");
            callArgs.push_back(recThis);
            ++ai;
            for (; ai != thunk->arg_end(); ++ai) callArgs.push_back(&*ai);
            llvm::Value* r = tb.CreateCall(fnTy, real, callArgs);
            if (fnTy->getReturnType()->isVoidTy()) tb.CreateRetVoid();
            else tb.CreateRet(r);
            elems[slot] = thunk;
        }
        auto* arrTy = llvm::ArrayType::get(ptrTy(), elems.size());
        boxVtable_[ts] = new llvm::GlobalVariable(
            *module_, arrTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(arrTy, elems),
            "bvt$box$" + std::string(ts->name));
    }

    // --- Primitives: descriptor + 1-word box vtable -----------------------
    // Each boxable value primitive gets a 2-element descriptor [self, null] and
    // a 1-element vtable [descriptor]. `emitIsA` reads only vtable[0], so one
    // word suffices for `o is int`. (string is included so a boxed-value box
    // could in principle carry it, but `is string` itself is deferred — see
    // lowerTypeTest: a raw string operand has no vtable to walk.)
    using P = sema::PrimitiveType;
    static const P prims[] = {P::Bool, P::Char, P::String, P::Byte, P::SByte,
                              P::Short, P::UShort, P::Int, P::UInt, P::Long,
                              P::ULong, P::Float, P::Double};
    for (P p : prims) {
        const char* nm = primBoxName(p);
        if (!nm || primBoxDesc_.count(nm)) continue;
        auto* descTy = llvm::ArrayType::get(ptrTy(), 2);
        auto* desc = new llvm::GlobalVariable(
            *module_, descTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage, nullptr,
            std::string("ptd$") + nm);
        desc->setInitializer(llvm::ConstantArray::get(
            descTy, {desc, llvm::ConstantPointerNull::get(ptrTy())}));
        primBoxDesc_[nm] = desc;
        auto* vtTy = llvm::ArrayType::get(ptrTy(), 1);
        primBoxVtable_[nm] = new llvm::GlobalVariable(
            *module_, vtTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(vtTy, {desc}),
            std::string("pvt$") + nm);
    }
}


// §8.6: is `rt` a value type that boxes when coerced to Object/interface?
bool Emitter::isBoxableValueType(const sema::TypeRef* rt) {
    if (!rt) return false;
    using K = sema::TypeRef::Kind;
    using P = sema::PrimitiveType;
    if (rt->kind == K::Primitive) {
        // string/Object are reference types; everything else (numeric, bool,
        // char) is a value primitive that boxes.
        return rt->primitive != P::String && rt->primitive != P::Object &&
               rt->primitive != P::None && rt->primitive != P::Decimal;
    }
    if (rt->kind == K::Named && rt->declaration)
        return rt->declaration->kind == ast::TypeDeclKind::Record ||
               rt->declaration->kind == ast::TypeDeclKind::Enum;
    return false;  // inline arrays / unit: deferred (not among the findings)
}


// §8.6: a declared location whose type triggers boxing of a value-typed source.
bool Emitter::isObjectOrInterfaceTarget(const sema::TypeRef* target) {
    if (!target) return false;
    using K = sema::TypeRef::Kind;
    using P = sema::PrimitiveType;
    if (target->kind == K::Primitive) return target->primitive == P::Object;
    if (target->kind == K::Named && target->declaration)
        return target->declaration->kind == ast::TypeDeclKind::Interface;
    return false;
}


// §8.6: the box descriptor for a value type's type-test target.
llvm::GlobalVariable* Emitter::boxDescriptorFor(const sema::TypeRef* rt) {
    if (!rt) return nullptr;
    using K = sema::TypeRef::Kind;
    if (rt->kind == K::Named && rt->declaration) {
        auto it = descArray_.find(rt->declaration);
        return it != descArray_.end() ? it->second : nullptr;
    }
    if (rt->kind == K::Primitive) {
        const char* nm = primBoxName(rt->primitive);
        if (!nm) return nullptr;
        auto it = primBoxDesc_.find(nm);
        return it != primBoxDesc_.end() ? it->second : nullptr;
    }
    return nullptr;
}


// §8.6: the box vtable for a boxable value type.
llvm::GlobalVariable* Emitter::boxVtableFor(const sema::TypeRef* rt) {
    if (!rt) return nullptr;
    using K = sema::TypeRef::Kind;
    if (rt->kind == K::Named && rt->declaration) {
        auto it = boxVtable_.find(rt->declaration);
        return it != boxVtable_.end() ? it->second : nullptr;
    }
    if (rt->kind == K::Primitive) {
        const char* nm = primBoxName(rt->primitive);
        if (!nm) return nullptr;
        auto it = primBoxVtable_.find(nm);
        return it != primBoxVtable_.end() ? it->second : nullptr;
    }
    return nullptr;
}


}  // namespace bezel::codegen
