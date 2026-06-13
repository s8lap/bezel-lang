#include "codegen/LlvmBackend_impl.hpp"

// §4.4 fix #521: decimal compile-time literal parser (defined in bezel_runtime.cpp).
extern "C" void bz_decimal_parse_cstr(const char* s, void* out);

namespace bezel::codegen {

// #383 / §28: forward declaration — defined later in this file. A reference
// TypeRef carrying a vtable (a Named class/interface). Used by the §28.3
// unwind-disposal eligibility helpers above its definition.
static bool isClassOrInterfaceRef(const sema::TypeRef* rt);

std::string Emitter::functionName(sema::MethodSymbol* ms) {
    if (isMain(ms)) return "main";
    // §26 (#433): an extension method has no parent type — name it after its
    // extended (target) type so distinct `extension`s don't collide and a
    // class method `bz$<Class>$M` never aliases an extension `M`.
    if (isExtensionMethod(ms)) {
        std::string tgt = "?";
        if (const ast::Type* tt = ms->extensionDecl->targetType.get())
            if (tt->name && !tt->name->parts.empty())
                tgt = std::string(tt->name->parts.back());
        return "bz$ext$" + tgt + "$" + std::string(ms->name);
    }
    std::string cls = ms->parent ? std::string(ms->parent->name) : "";
    // §25: an operator's name is empty; build a unique, collision-free
    // name from its token (a type may declare several operators).
    if (ms->isOperator)
        return "bz$" + cls + "$op$" + std::string(ms->operatorToken);
    return "bz$" + cls + "$" + std::string(ms->name);
}


// §26.1 (#433): the receiver type for an extension method's `this` parameter.
llvm::Type* Emitter::extensionThisType(const sema::MethodSymbol* ms,
                                       bool* byPointer) {
    if (byPointer) *byPointer = false;
    if (!ms || !ms->extensionDecl || !ms->extensionDecl->targetType)
        return nullptr;
    const sema::TypeRef* rt =
        ctx_.resolvedType(*ms->extensionDecl->targetType);
    if (!rt) return nullptr;
    // A class/interface receiver: `this` is the object pointer (like a normal
    // method). A record receiver: pass a pointer to a spilled value so the
    // body's `this.field` reuses the record field-access machinery.
    if (rt->kind == sema::TypeRef::Kind::Named && rt->declaration) {
        auto k = rt->declaration->kind;
        if (k == ast::TypeDeclKind::Class ||
            k == ast::TypeDeclKind::Interface) {
            if (byPointer) *byPointer = true;
            return ptrTy();
        }
        if (k == ast::TypeDeclKind::Record) {
            if (byPointer) *byPointer = true;
            return ptrTy();  // pointer to the record value (spilled at call)
        }
        // enum → underlying integer, by value.
        return lowerTypeRef(rt);
    }
    // Primitive / builtin value receiver — by value (e.g. i32 for `int`).
    // CRITICAL (#433): this is what fixes the `extension int` crash — the
    // receiver is the VALUE, not a GEP'd pointer.
    return lowerTypeRef(rt);
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
            // per §32 / issue #496: if the module already contains a function
            // with this name (e.g. malloc/calloc/free declared internally by
            // the codegen runtime), REUSE it instead of creating a second
            // declaration. LLVM renames a conflicting second declaration
            // (malloc → malloc.1), producing a dangling symbol at link time.
            // The call site in lowerExternCall coerces argument widths so the
            // canonical internal signature is always used at the LLVM level.
            if (auto* existing = module_->getFunction(std::string(xd->name))) {
                externFns_[xd] = existing;
                continue;
            }
            auto* fnTy = llvm::FunctionType::get(retTy, paramTys, false);
            externFns_[xd] = llvm::Function::Create(
                fnTy, llvm::Function::ExternalLinkage,
                std::string(xd->name), module_.get());
        }
    }
}


// §10 / #482 fix A: populate enumValue_ for all constants of a single enum
// TypeSymbol whose AST TypeDecl is available. Extracted so it can be called
// both for module-level enums (via the symbols() walk) and for enums nested
// inside a class (via computeEnumValues's second pass below).
void Emitter::populateEnumValuesFromDecl(const sema::TypeSymbol* ts) {
    auto* td = dynamic_cast<const ast::TypeDecl*>(ts->decl);
    if (!td) return;
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

void Emitter::computeEnumValues() {
    for (const auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (!ts || ts->kind != ast::TypeDeclKind::Enum) continue;
        populateEnumValuesFromDecl(ts);
    }
    // §10 / #482 fix A: module-level class symbols may contain NESTED enum
    // types (NestedTypeSymbol members whose ->type->kind == Enum). The loop
    // above only visits module-level TypeSymbols, so nested enums' constants
    // were never entered in enumValue_ — making every nested-enum constant
    // lower to 0 regardless of its declared value. Walk every class TypeSymbol
    // and populate nested-enum constants exactly like the module-level case.
    // per §10: a nested enum has the same underlying-integer semantics as a
    // top-level enum; its constants are accessible as Outer.Inner.Constant.
    for (const auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (!ts || ts->kind == ast::TypeDeclKind::Enum) continue;
        for (const sema::Symbol* m : ts->members) {
            if (auto* nts = dynamic_cast<const sema::NestedTypeSymbol*>(m)) {
                if (nts->type &&
                    nts->type->kind == ast::TypeDeclKind::Enum) {
                    populateEnumValuesFromDecl(nts->type);
                }
            }
        }
    }
    // §10/§10.2 (issue #328 FINDING [1]): an IMPORTED enum has no AST TypeDecl,
    // so the loop above skips it (the `if (!td) continue;` guard) and its
    // constants would otherwise lower to 0. The dependency metadata carried each
    // constant's underlying value, which the loader stored on the reconstructed
    // EnumConstantSymbol::value. Mirror it into enumValue_ so the SAME value
    // lookup used by `int(enum)`, enum equality, and `case` arms resolves for
    // imported enums exactly as for local ones. Imported symbols live in a list
    // separate from symbols() (see SemaContext), hence the extra walk.
    for (const sema::Symbol* sym : ctx_.importedSymbols()) {
        auto* ec = dynamic_cast<const sema::EnumConstantSymbol*>(sym);
        if (!ec) continue;
        enumValue_[ec] = ec->value;
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

    // §24.1 (#477 B6): a STATIC event's backing EventSource<T> is a static
    // owned field — give it a `ptr` global, null until program start. It is
    // allocated (bz_event_source_new) in Main's prologue alongside the static
    // constructors (§22.7) and read by eventSourcePtr. Synchronous events only
    // (async deferred, mirroring collectInstanceStorage). The global is
    // INTERNAL: a static event's backing source is not part of the cross-module
    // surface (external code reaches it only through the declaring class's
    // static accessor methods, not the field), and program-start init is
    // single-module here.
    for (const auto& sym : ctx_.symbols()) {
        auto* es = dynamic_cast<const sema::EventSymbol*>(sym.get());
        if (!es) continue;
        auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl);
        if (!ed || !ed->isStatic || ed->isAsync) continue;
        std::string cls = es->parent ? std::string(es->parent->name) : "";
        auto* gv = new llvm::GlobalVariable(
            *module_, ptrTy(), /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantPointerNull::get(ptrTy()),
            "bz$" + cls + "$" + std::string(es->name) + "$evt");
        staticEventGlobals_[es] = gv;
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
        // CG24: float/double/decimal literal initializer for static float fields.
        if (auto* fl = dynamic_cast<const ast::FloatLiteralExpr*>(e)) {
            // §4.4 fix #521: a decimal literal (m/M suffix) → parse into the
            // .NET-compatible bz_decimal_t struct constant.
            if (!fl->text.empty()) {
                char last = fl->text.back();
                if (last == 'm' || last == 'M') {
                    std::string digits;
                    for (char c : fl->text) {
                        if (c == '_' || c == 'm' || c == 'M') continue;
                        digits.push_back(c);
                    }
                    uint32_t dec[4] = {0, 0, 0, 0};
                    bz_decimal_parse_cstr(digits.c_str(), dec);
                    llvm::StructType* dst = decimalStructType();
                    return llvm::ConstantStruct::get(dst, {
                        llvm::ConstantInt::get(i32(), dec[0]),
                        llvm::ConstantInt::get(i32(), dec[1]),
                        llvm::ConstantInt::get(i32(), dec[2]),
                        llvm::ConstantInt::get(i32(), dec[3])
                    });
                }
            }
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
    auto* sym_raw = ctx_.exprSymbol(e);
    auto* fs = dynamic_cast<sema::FieldSymbol*>(sym_raw);
    if (!fs) return nullptr;

    // §22.12 / #536 / #697 / #701: each distinct instantiation of a generic class
    // must use its own static-field global (per §22.12: "each distinct
    // instantiation has its own static state"). The base declareStatics() global
    // is named `bz$Counter$n` and is shared — redirect to a per-instantiation
    // global named `bz$Counter$int$n` / `bz$Counter$string$n` etc.
    //
    // Two sources of type-argument information (tried in priority order):
    //   (B) The access is a MemberAccessExpr whose target carries CONCRETE type
    //       args in its sema-resolved TypeRef (kind=Named, genericArgs
    //       populated). Extract them FIRST, regardless of genericSubst_ state.
    //       This handles: non-generic context (#697), AND a generic method body
    //       that accesses a DIFFERENT generic class's static (e.g. Wrapper<U>
    //       accessing Counter<int>.n: param "T" ∉ genericSubst_{U→…}).  #701.
    //   (A) Fallback (only when B found nothing): genericSubst_ is active and
    //       the access is bare (unqualified, inside the owning class's own
    //       method body, e.g. `_count` from Counter<T>.Inc()).  Resolve via
    //       genericSubst_ param-name lookup.  Per §22.12 / #536.
    if (fs->parent && !fs->parent->genericParams.empty()) {
        // Collect (llvmType, identityToken, mangleName) for each of the parent
        // class's generic params, from whichever source is active.
        std::vector<llvm::Type*> substTypes;
        std::vector<const void*> substIds;
        std::string typeArgMangle;

        // Helper: append one type argument (semaTy) to the three key vectors.
        auto appendTypeArg = [&](const sema::TypeRef* semaTy) {
            llvm::Type* llTy = semaTy ? lowerTypeRef(semaTy) : i32();
            substTypes.push_back(llTy);
            substIds.push_back(typeRefIdentityToken(semaTy));
            if (!typeArgMangle.empty()) typeArgMangle += "_";
            if (semaTy) {
                if (semaTy->kind == sema::TypeRef::Kind::Named &&
                        semaTy->declaration)
                    typeArgMangle += std::string(semaTy->declaration->name);
                else if (semaTy->kind == sema::TypeRef::Kind::Primitive) {
                    switch (semaTy->primitive) {
                        case sema::PrimitiveType::Bool:    typeArgMangle += "bool"; break;
                        case sema::PrimitiveType::Char:    typeArgMangle += "char"; break;
                        case sema::PrimitiveType::String:  typeArgMangle += "string"; break;
                        case sema::PrimitiveType::Byte:    typeArgMangle += "byte"; break;
                        case sema::PrimitiveType::SByte:   typeArgMangle += "sbyte"; break;
                        case sema::PrimitiveType::Short:   typeArgMangle += "short"; break;
                        case sema::PrimitiveType::UShort:  typeArgMangle += "ushort"; break;
                        case sema::PrimitiveType::Int:     typeArgMangle += "int"; break;
                        case sema::PrimitiveType::UInt:    typeArgMangle += "uint"; break;
                        case sema::PrimitiveType::Long:    typeArgMangle += "long"; break;
                        case sema::PrimitiveType::ULong:   typeArgMangle += "ulong"; break;
                        case sema::PrimitiveType::Float:   typeArgMangle += "float"; break;
                        case sema::PrimitiveType::Double:  typeArgMangle += "double"; break;
                        case sema::PrimitiveType::Decimal: typeArgMangle += "decimal"; break;
                        default: typeArgMangle += "prim"; break;
                    }
                } else {
                    typeArgMangle += "t";
                }
            } else {
                typeArgMangle += "t";
            }
        };

        // Path (B): §22.12 / #697 / #701: the access is a MemberAccessExpr
        // whose target IdentifierExpr carries CONCRETE type args in its
        // sema-resolved TypeRef (kind=Named, genericArgs populated).  Extract
        // those first — unconditionally, regardless of whether genericSubst_ is
        // active.  This covers:
        //   • non-generic context (e.g. static Main): Counter<int>.n → #697.
        //   • generic method body with DIFFERENT param names (e.g. Wrapper<U>
        //     accessing Counter<int>.n): Path (A) would miss because "T" ∉
        //     genericSubst_{U→string} — #701 fix.
        // The extraction is identical to the original #697 path; only the guard
        // `!genericSubst_.empty()` has been removed so it is reachable in both
        // contexts.
        {
            const sema::TypeRef* targetTR = nullptr;
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
                if (ma->target)
                    targetTR = ctx_.exprType(*ma->target);
            }
            if (targetTR && targetTR->kind == sema::TypeRef::Kind::Named &&
                    targetTR->declaration == fs->parent &&
                    targetTR->genericArgs.size() ==
                        fs->parent->genericParams.size()) {
                for (std::size_t i = 0; i < fs->parent->genericParams.size(); ++i)
                    appendTypeArg(targetTR->genericArgs[i]);
            }
        }

        if (substTypes.empty() && !genericSubst_.empty()) {
            // Path (A): fallback — inside a monomorphized generic method/class
            // body, and the access is bare (no MemberAccessExpr target, e.g.
            // `_count` from inside Counter<T>'s own method body).  Resolve each
            // of the parent's type params via genericSubst_.  This path is only
            // reached when Path (B) found nothing, which happens exactly when the
            // access does NOT carry its own concrete type args (bare unqualified
            // access inheriting the enclosing instantiation).  Per §22.12 / #536.
            for (const sema::Symbol* gp : fs->parent->genericParams) {
                auto sit = genericSubst_.find(gp->name);
                if (sit == genericSubst_.end()) continue;
                auto semaSit = genericSubstSemaTy_.find(gp->name);
                const sema::TypeRef* semaTy =
                    (semaSit != genericSubstSemaTy_.end())
                        ? semaSit->second : nullptr;
                // Override llvmType from genericSubst_ (not lowerTypeRef) to
                // match the original per-instantiation key scheme.
                substTypes.push_back(sit->second);
                substIds.push_back(typeRefIdentityToken(semaTy));
                if (!typeArgMangle.empty()) typeArgMangle += "_";
                if (semaTy) {
                    if (semaTy->kind == sema::TypeRef::Kind::Named &&
                            semaTy->declaration)
                        typeArgMangle += std::string(semaTy->declaration->name);
                    else if (semaTy->kind == sema::TypeRef::Kind::Primitive) {
                        switch (semaTy->primitive) {
                            case sema::PrimitiveType::Bool:    typeArgMangle += "bool"; break;
                            case sema::PrimitiveType::Char:    typeArgMangle += "char"; break;
                            case sema::PrimitiveType::String:  typeArgMangle += "string"; break;
                            case sema::PrimitiveType::Byte:    typeArgMangle += "byte"; break;
                            case sema::PrimitiveType::SByte:   typeArgMangle += "sbyte"; break;
                            case sema::PrimitiveType::Short:   typeArgMangle += "short"; break;
                            case sema::PrimitiveType::UShort:  typeArgMangle += "ushort"; break;
                            case sema::PrimitiveType::Int:     typeArgMangle += "int"; break;
                            case sema::PrimitiveType::UInt:    typeArgMangle += "uint"; break;
                            case sema::PrimitiveType::Long:    typeArgMangle += "long"; break;
                            case sema::PrimitiveType::ULong:   typeArgMangle += "ulong"; break;
                            case sema::PrimitiveType::Float:   typeArgMangle += "float"; break;
                            case sema::PrimitiveType::Double:  typeArgMangle += "double"; break;
                            case sema::PrimitiveType::Decimal: typeArgMangle += "decimal"; break;
                            default: typeArgMangle += "prim"; break;
                        }
                    } else {
                        typeArgMangle += "t";
                    }
                } else {
                    typeArgMangle += "t";
                }
            }
        }

        // Only redirect if we found at least one bound parent type param.
        if (!substTypes.empty()) {
            auto key = std::make_pair(static_cast<const sema::Symbol*>(fs),
                                      std::make_pair(substTypes, substIds));
            auto git = genericInstStaticGlobals_.find(key);
            if (git != genericInstStaticGlobals_.end()) return git->second;

            // First access for this (field, instantiation) pair — check
            // staticGlobals_ has the template-level global so we can copy its
            // type and initializer.
            auto baseIt = staticGlobals_.find(fs);
            if (baseIt == staticGlobals_.end()) return nullptr;
            llvm::GlobalVariable* base = baseIt->second;

            // Name: bz$Counter$int$n — insert the type-arg mangle between
            // the class name and the field name.
            std::string cls = fs->parent ? std::string(fs->parent->name) : "";
            std::string fname = std::string(fs->name);
            std::string gname = "bz$" + cls + "$" + typeArgMangle + "$" + fname;

            // Create the per-instantiation global with the same type and
            // constant initializer as the template-level global (which was
            // initialized by constInitFor in declareStatics).
            //
            // §22.12 / #684: for an imported generic, the template-level global
            // is an external declaration created in declareStatics with nullptr
            // initializer (ExternalLinkage, no definition here). Calling
            // base->getInitializer() on such a global asserts in LLVM because
            // external declarations have no initializer. Use zero-init for the
            // per-instantiation copy; each importing module owns its own
            // InternalLinkage instance (per §22.12 "each distinct instantiation
            // has its own static state"). Any non-trivial constant initializer
            // only occurs for locally-defined generics (non-imported), where
            // base->hasInitializer() is always true.
            llvm::Constant* instInit = base->hasInitializer()
                ? base->getInitializer()
                : llvm::Constant::getNullValue(base->getValueType());
            auto* gv = new llvm::GlobalVariable(
                *module_, base->getValueType(), /*isConstant=*/false,
                llvm::GlobalValue::InternalLinkage,
                instInit,
                gname);
            genericInstStaticGlobals_[key] = gv;
            return gv;
        }
    }

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
            // §26.1 (#433): an extension method's `this` is the EXTENDED type
            // — by value for a primitive/value receiver (i32 for `extension
            // int`, fixing the malformed-IR crash), by pointer for a class
            // (object pointer) or record (spilled value pointer).
            if (isExtensionMethod(ms)) {
                llvm::Type* thisTy = extensionThisType(ms, /*byPointer=*/nullptr);
                paramTys.push_back(thisTy ? thisTy : ptrTy());
            } else {
                // CG30: enum instance methods take the enum value (i32) as
                // `this`, not a pointer — enums are value types.
                bool isEnumMethod = ms->parent &&
                    ms->parent->kind == ast::TypeDeclKind::Enum;
                paramTys.push_back(isEnumMethod ? i32() : ptrTy());
            }
        }
        // fix #633 / §33.4: Main(args: Array<string>) must match the C ABI
        // entry-point signature `int main(int argc, char** argv)`, NOT the
        // Bezel-typed `int main(ptr args)`.  The OS/C runtime passes `argc`
        // as an i32 and `argv` as a ptr; any other signature silently receives
        // the integer `argc` where the ptr is expected → immediate SIGSEGV.
        // The args Array<string> is constructed from (argc, argv) in the
        // function prolog (see emitMethodBody below) via bz_args_array.
        if (isMain(ms) && !ci.params->empty()) {
            paramTys.push_back(i32());  // argc
            paramTys.push_back(ptrTy());  // argv (char**)
        } else {
            for (const auto& p : *ci.params) {
                paramTys.push_back(isByRefMod(p.modifier)
                                       ? ptrTy()  // CG50: ref/out by reference
                                       : lowerType(p.type.get()));
            }
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

    // CG47 / fix #582 / §22.13: declare static initialisers.
    //
    // Two kinds exist per type (in order):
    //  (a) A synthesized "sfinit" void() function — emitted when the type has
    //      ≥1 non-constant static field initializer.  Evaluates those
    //      initializers in textual declaration order and stores each result
    //      into its backing global.  §22.13: "Within a single type, static
    //      fields are initialized in textual declaration order."
    //  (b) The user-written "cctor" void() function (§22.7) — emitted when
    //      the source declares `static constructor()`.  §22.13: "The static
    //      constructor, if any, runs after all static field initializers
    //      complete."
    //
    // We iterate ctx_.symbols() in declaration order so that, across types,
    // an earlier-declared type's sfinit+cctor always precede a later type's.
    // Within a single type, sfinit (if any) is inserted BEFORE cctor (if
    // any), satisfying the §22.13 ordering guarantee.  This works because
    // SymbolCollection adds a class's TypeSymbol BEFORE its member symbols
    // (constructors, fields), so the TypeSymbol and the synthesized sfinit
    // entry for it always precede the user-written ConstructorSymbol entry.
    //
    // Helper: does `t` have any static field whose initializer is NOT a
    // compile-time constant (i.e. not handled by constInitFor)?  Those need
    // a runtime store emitted by the synthesized sfinit function.
    auto needsRuntimeFieldInit = [&](const sema::TypeSymbol* t) -> bool {
        for (const sema::Symbol* m : t->members) {
            auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
            if (!fs || !isStaticField(fs)) continue;
            auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
            if (!fd || !fd->initializer) continue;
            const ast::Expr* e = fd->initializer.get();
            // Mirrors constInitFor: only these expressions yield a
            // compile-time LLVM Constant; everything else needs runtime eval.
            if (dynamic_cast<const ast::IntLiteralExpr*>(e))     continue;
            if (dynamic_cast<const ast::BoolLiteralExpr*>(e))    continue;
            if (dynamic_cast<const ast::FloatLiteralExpr*>(e))   continue;
            if (dynamic_cast<const ast::StringLiteralExpr*>(e))  continue;
            if (dynamic_cast<const ast::VerbatimStringExpr*>(e)) continue;
            return true;  // non-constant initializer found
        }
        return false;
    };
    // Track which types already have a synthesized sfinit so we do not
    // double-emit one when the same TypeSymbol appears multiple times.
    std::unordered_set<const sema::TypeSymbol*> sfInitEmitted;
    auto* voidFnType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(llvm_), {}, /*isVarArg=*/false);
    for (const auto& sym : ctx_.symbols()) {
        // (a) Synthesized sfinit: emitted just before the user cctor entry for
        //     the same type, so field inits run first.
        if (auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get())) {
            if (!sfInitEmitted.count(ts) && needsRuntimeFieldInit(ts)) {
                sfInitEmitted.insert(ts);
                std::string cls(ts->name);
                auto* fn = llvm::Function::Create(
                    voidFnType, llvm::Function::InternalLinkage,
                    "bz$" + cls + "$sfinit", module_.get());
                staticCtors_.push_back({nullptr, ts, fn});
            }
        }
        // (b) User-written cctor.
        if (auto* cs = dynamic_cast<sema::ConstructorSymbol*>(sym.get())) {
            if (!cs->isStatic) continue;
            if (!dynamic_cast<const ast::ConstructorDecl*>(cs->decl)) continue;
            std::string cls = cs->parent ? std::string(cs->parent->name) : "";
            auto* fn = llvm::Function::Create(
                voidFnType, llvm::Function::ExternalLinkage,
                "bz$" + cls + "$cctor", module_.get());
            staticCtors_.push_back({cs, cs->parent, fn});
        }
    }

    // CG13 / §28.2: declare, for every class, the public idempotent Dispose
    // `void(this)` (vtable slot 2, CG35-itable) AND an unguarded "DisposeCore" that runs the
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
        // §2.5 / §2.6 / §28.4 step 3 (issue #328 / #103): the DisposeCore (the
        // unguarded destructor + owned-field disposal + base-core chain) is
        // EXTERNAL for a PUBLIC, non-generic class, because a LOCAL class deriving
        // from this imported base must chain to THIS core (not the flag-guarded
        // public Dispose, which the most-derived disposal has already tripped) so
        // the base destructor + base owned fields run within the single guarded
        // most-derived disposal. A non-public/generic type keeps INTERNAL linkage
        // (single-module codegen and private types unchanged); only genuinely
        // exported types widen the link surface, matching the vt$/init pattern.
        bool exportedCore = ts->visibility == ast::Visibility::Public &&
                            ts->genericParams.empty();
        auto* coreFn = llvm::Function::Create(
            fnType,
            exportedCore ? llvm::Function::ExternalLinkage
                         : llvm::Function::InternalLinkage,
            "bz$" + std::string(ts->name) + "$DisposeCore", module_.get());
        disposerCoreFns_[ts] = coreFn;

        // §2.5 / §2.6 / §22.8 (issue #328 item 2+3): a PUBLIC, non-generic class
        // gets an EXPORTED `bz$<Type>$init(ptr this)` — the §22.8 step-1+2
        // initializer (vtable install + field initializers) an importer calls
        // cross-module. (Item 2's vtable install + item 3's non-foldable field
        // init both ride this single exported symbol.) A generic template's
        // initialization is per-instantiation (no stable linkable symbol) and a
        // non-public type is not part of the importable construction surface, so
        // neither is exported — single-module codegen and private types are
        // unchanged, and only genuinely-exported types widen the link surface.
        if (ts->visibility == ast::Visibility::Public &&
            ts->genericParams.empty()) {
            auto* initFn = llvm::Function::Create(
                fnType, llvm::Function::ExternalLinkage,
                "bz$" + std::string(ts->name) + "$init", module_.get());
            initFns_[ts] = initFn;
        }
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

    // §2.5 / §2.6 / §28.4 (issue #328 item 1): declare each IMPORTED class's
    // external `bz$<Type>$Dispose` and register it in disposerFns_, so a `using
    // var`/owned local of an imported type is RECOGNIZED as disposable (the
    // disposal-registration and disposal-emit gates both consult disposerFns_)
    // and discharged at scope end. The registered function is only a membership
    // gate + a linkable declaration: the actual scope-end disposal dispatches
    // DYNAMICALLY through the object's vtable slot 2 (emitDisposeAndReclaim),
    // which the imported vtable (installed by bz$<Type>$init) routes to the
    // exporter's real Dispose — so the destructor runs cross-module exactly once.
    // Imported entries are NEVER emitted a body (the run() loop skips imported
    // types; this is a declaration only).
    if (ctx_.hasImports()) {
        for (sema::Symbol* sym : ctx_.importedSymbols()) {
            auto* ts = dynamic_cast<sema::TypeSymbol*>(sym);
            if (!ts || ts->kind != ast::TypeDeclKind::Class) continue;
            if (disposerFns_.count(ts)) continue;
            auto* fnType = llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy()}, /*isVarArg=*/false);
            auto* fn = llvm::Function::Create(
                fnType, llvm::Function::ExternalLinkage,
                "bz$" + std::string(ts->name) + "$Dispose", module_.get());
            disposerFns_[ts] = fn;
            // §2.5 / §2.6 / §28.4 step 3 (issue #328 / #103): also declare the
            // imported class's external DisposeCore, so a LOCAL class deriving
            // from this imported base chains its disposal to the base's destructor
            // (emitBaseDisposeChain calls disposerCoreFns_[base]). Without it the
            // base destructor + base owned fields never run on the derived's
            // disposal. The exporter emits it with EXTERNAL linkage for public,
            // non-generic classes; declare it under the same condition so we never
            // emit an external reference the dependency does not define.
            if (!disposerCoreFns_.count(ts) &&
                ts->visibility == ast::Visibility::Public &&
                ts->genericParams.empty()) {
                auto* coreFn = llvm::Function::Create(
                    fnType, llvm::Function::ExternalLinkage,
                    "bz$" + std::string(ts->name) + "$DisposeCore",
                    module_.get());
                disposerCoreFns_[ts] = coreFn;
            }
        }
    }
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

// §2.5 / §22.9 (issue #503): emit a named, ExternalLinkage getter function for
// every public non-virtual non-indexer PropertySymbol on every public non-generic
// local class. The function `bz$<Class>$<Prop>$propget(ptr %this) -> T` has the
// same signature as the virtual-property accessor (ensurePropertyAccessorFn),
// which is InternalLinkage. A separate ExternalLinkage wrapper is used so the
// importer can call it without touching the (potentially inlined) local accessor.
//
// The body calls `ensurePropertyAccessorFn(ts, ps, /*isSet=*/false)` (which does
// the real work and caches the getter) — then the wrapper just tail-calls it.
// If ensurePropertyAccessorFn returns nullptr (abstract with no body) the
// wrapper is not emitted; abstract non-virtual properties don't exist in practice
// but guard for safety.
//
// Virtual / override / abstract properties already dispatch via the vtable (the
// slot carries the concrete accessor). Only plain, non-virtual getters need a
// stable cross-module symbol. Static properties have no `this` parameter and are
// handled separately by the static-field path; they're excluded here.
void Emitter::emitExportedPropertyGetters() {
    for (const auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (!ts || ts->kind != ast::TypeDeclKind::Class) continue;
        if (ts->visibility != ast::Visibility::Public) continue;
        if (!ts->genericParams.empty()) continue;
        for (sema::Symbol* m : ts->members) {
            auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
            if (!ps || ps->isIndexer) continue;
            // Skip virtual/override/abstract/static — they have other dispatch paths.
            if (isVirtualProperty(ps)) continue;
            bool isStatic = false;
            for (auto mod : ps->modifiers) if (mod == "static") isStatic = true;
            if (isStatic) continue;
            // Only public properties are part of the cross-module surface.
            if (ps->visibility != ast::Visibility::Public) continue;
            // Only getters — properties with no getter (setter-only, §22.9) skip.
            auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
            if (!pd) continue;
            // Derive the return type.
            llvm::Type* valTy = lowerType(pd->type.get());
            if (!valTy) valTy = i32();
            std::string clsName = std::string(ts->name);
            std::string propName = std::string(ps->name);
            std::string mangled = "bz$" + clsName + "$" + propName + "$propget";
            // Avoid double-declaration if the function was already created
            // (shouldn't happen, but guard).
            if (module_->getNamedValue(mangled)) continue;
            auto* fnTy = llvm::FunctionType::get(valTy, {ptrTy()}, false);
            auto* fn = llvm::Function::Create(
                fnTy, llvm::GlobalValue::ExternalLinkage, mangled, *module_);
            // Emit the body: call ensurePropertyAccessorFn to get the concrete
            // getter (arrow or block), then forward the result.
            // ensurePropertyAccessorFn requires the current emitter state; save/
            // restore it exactly as other synthesized functions do.
            llvm::Function* innerFn =
                ensurePropertyAccessorFn(ts, ps, /*isSet=*/false);
            if (!innerFn) {
                // No concrete getter body (abstract or interface with no body).
                // Erase the incomplete declaration so the module stays valid.
                fn->eraseFromParent();
                continue;
            }
            auto* savedFn = currentFn_;
            auto* savedInsert = builder_.GetInsertBlock();
            currentFn_ = fn;
            auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
            builder_.SetInsertPoint(entry);
            llvm::Value* thisArg = &*fn->args().begin();
            // Forward: call the (InternalLinkage) inner getter with `this`.
            llvm::Value* result = builder_.CreateCall(
                innerFn->getFunctionType(), innerFn, {thisArg});
            builder_.CreateRet(result);
            currentFn_ = savedFn;
            if (savedInsert) builder_.SetInsertPoint(savedInsert);
        }
    }
}


// §2.5 / §22.9 (issue #503): declare an external getter function for every
// imported non-virtual PropertySymbol, so the property-access lowering can call
// it cross-module. The signature matches what the exporter emits in
// emitExportedPropertyGetters: `bz$<Class>$<Prop>$propget(ptr %this) -> T`.
// Only engages for non-virtual properties (virtual ones dispatch via the vtable
// slot already populated by buildVtable). Must run after declareVtables
// (which calls buildVtable for imported types), so propGetSlot_ is accurate.
void Emitter::declareImportedPropertyGetters() {
    for (sema::Symbol* sym : ctx_.importedSymbols()) {
        auto* ps = dynamic_cast<sema::PropertySymbol*>(sym);
        if (!ps || ps->isIndexer) continue;
        // Skip virtual: they dispatch through the vtable slot.
        if (propGetSlot_.count(ps)) continue;
        // Also skip interface properties: those dispatch through the itable.
        if (ps->parent && ps->parent->kind == ast::TypeDeclKind::Interface)
            continue;
        // Skip if already declared.
        if (importedPropGetterFns_.count(ps)) continue;
        // Derive the return type from the side table.
        sema::TypeRef* retRef = ctx_.importedMemberType(ps);
        llvm::Type* retTy = retRef ? lowerTypeRef(retRef) : i32();
        std::string clsName = ps->parent ? std::string(ps->parent->name) : "";
        std::string mangled =
            "bz$" + clsName + "$" + std::string(ps->name) + "$propget";
        // Re-use an existing module declaration if present (e.g. a double-
        // import from two transitive dependencies with the same symbol).
        if (auto* existing = module_->getFunction(mangled)) {
            importedPropGetterFns_[ps] = existing;
            continue;
        }
        auto* fnTy = llvm::FunctionType::get(retTy, {ptrTy()}, false);
        auto* fn = llvm::Function::Create(
            fnTy, llvm::Function::ExternalLinkage, mangled, module_.get());
        importedPropGetterFns_[ps] = fn;
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
    std::vector<VSlot> slots;
    std::unordered_map<std::string, unsigned> nameSlot;
    if (const sema::TypeSymbol* base = baseClassOf(t)) {
        buildVtable(base);
        slots = vtableMethods_[base];
        nameSlot = vtableNameSlot_[base];
    }
    // Vtable (CG35-itable + §8.6 #417): [0]=td$C, [1]=itmap$C, [2]=Dispose,
    // [3]=ToString, [4..]=virtual members. So a virtual member's slot is its
    // position in `slots` + 4 (ToString occupies the fixed slot 3, like Dispose
    // occupies the fixed slot 2). See declareVtables Pass D for the slot fill.
    //
    // #481 / §12.1 / §12.5 / §23: virtual METHODS and virtual PROPERTY accessors
    // are enumerated together in member declaration order, each getting its own
    // slot. An `override` (method or property accessor) reuses the base member's
    // slot, matched by name (a setter uses "name$set" so get/set are distinct).
    // For a method-only class the layout is exactly the pre-#481 one (a property
    // just appends/interleaves additional slots); the IR verifier + the existing
    // virtual/interface dispatch tests guard against a perturbed method index.
    const unsigned base = 4;
    auto assignSlot = [&](const std::string& key, VSlot vs) -> unsigned {
        auto it = nameSlot.find(key);
        if (it != nameSlot.end()) {       // override: reuse base slot
            slots[it->second - base] = vs;
            return it->second;
        }
        unsigned slot = static_cast<unsigned>(slots.size()) + base;
        slots.push_back(vs);
        nameSlot[key] = slot;
        return slot;
    };
    for (sema::Symbol* m : t->members) {
        if (auto* ms = dynamic_cast<sema::MethodSymbol*>(m)) {
            if (isStaticMethod(ms) || !isVirtual(ms)) continue;
            vtableSlot_[ms] = assignSlot(std::string(ms->name),
                                         VSlot{ms, nullptr, false});
            continue;
        }
        if (auto* ps = dynamic_cast<sema::PropertySymbol*>(m)) {
            if (!isVirtualProperty(ps)) continue;
            propGetSlot_[ps] = assignSlot(std::string(ps->name),
                                          VSlot{nullptr, ps, false});
            if (propertyHasSetter(ps))
                propSetSlot_[ps] = assignSlot(std::string(ps->name) + "$set",
                                              VSlot{nullptr, ps, true});
        }
    }
    vtableMethods_[t] = std::move(slots);
    vtableNameSlot_[t] = std::move(nameSlot);
}


// §24.7 (#477 B7): emit a per-class event accessor `ptr get(ptr this)` that
// returns the object's backing EventSource<T> (the event's instance field).
// Cached per EventSymbol. Self-contained: saves/restores the builder so it may
// run during declareVtables (before method bodies are emitted).
llvm::Function* Emitter::emitEventAccessor(const sema::TypeSymbol* cls,
                                           const sema::EventSymbol* es) {
    if (auto it = eventAccessorFns_.find(es); it != eventAccessorFns_.end())
        return it->second;
    auto idxIt = fieldIndex_.find(es);
    if (idxIt == fieldIndex_.end()) return nullptr;  // no backing slot

    auto* fnTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
    std::string name = "bz$" + std::string(cls->name) + "$" +
                       std::string(es->name) + "$evtget";
    auto* fn = llvm::Function::Create(
        fnTy, llvm::GlobalValue::InternalLinkage, name, module_.get());
    eventAccessorFns_[es] = fn;

    // Save/restore the per-function emitter state (mirrors other synthesized
    // function bodies, e.g. emitInstanceInitFunction).
    llvm::Function* savedFn = currentFn_;
    auto* savedInsert = builder_.GetInsertBlock();

    currentFn_ = fn;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    builder_.SetInsertPoint(entry);
    llvm::Value* thisArg = &*fn->args().begin();
    llvm::StructType* st = classStruct(cls);
    llvm::Value* slot = builder_.CreateStructGEP(st, thisArg, idxIt->second);
    llvm::Value* src = builder_.CreateLoad(ptrTy(), slot, "evt.src");
    builder_.CreateRet(src);

    currentFn_ = savedFn;
    if (savedInsert) builder_.SetInsertPoint(savedInsert);
    return fn;
}


void Emitter::declareVtables() {
    // Pass 0 (CG35-itable): assign each interface method its 0-based position
    // within that interface's non-static method declaration order. This index
    // (ifaceMethodIndex_[ms]) is used to GEP into the per-(class×interface)
    // itable array at dispatch time. The old dense shared slot region
    // (ifaceMethodSlot_ / ifaceSlotCount_) is gone; each interface now has its
    // own per-pair itable, looked up at runtime via bz_itable_lookup.
    for (const auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (!ts || ts->kind != ast::TypeDeclKind::Interface) continue;
        unsigned idx = 0;
        for (sema::Symbol* m : ts->members) {
            auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
            if (ms && !isStaticMethod(ms))
                ifaceMethodIndex_[ms] = idx++;
        }
        // §24.7 (#477 B7): interface events follow the methods in the itab,
        // each holding a per-class EventSource accessor. Indices continue from
        // the method count so method positions are unchanged. Sync events only.
        for (sema::Symbol* m : ts->members) {
            auto* es = dynamic_cast<sema::EventSymbol*>(m);
            if (!es) continue;
            auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl);
            if (ed && !ed->isAsync) ifaceEventIndex_[es] = idx++;
        }
        // #481 / §13 / §23: interface properties follow the events in the itab,
        // each holding a per-class accessor function (getter, then setter if the
        // interface property declares one). Indices continue from the running
        // count so method/event positions are unchanged.
        for (sema::Symbol* m : ts->members) {
            auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
            if (!ps || ps->isIndexer) continue;
            ifacePropertyGetIndex_[ps] = idx++;
            if (propertyHasSetter(ps)) ifacePropertySetIndex_[ps] = idx++;
        }
    }
    // §13 / §2.5 / §2.6 (issue #426 / #328): populate ifaceMethodIndex_ for
    // IMPORTED interfaces too. A cross-module interface call (either on an
    // imported object or through an imported interface type held by the importer)
    // reaches the dispatch path in LlvmBackend_Calls.cpp with ms->parent being an
    // imported TypeSymbol. Pass 0 above only walked ctx_.symbols() (local types),
    // so ifaceMethodIndex_.find(ms) would return end() and dispatch would silently
    // bail. Mirror the same 0-based positional scheme over the imported interface's
    // non-static members, in the order the loader reconstructed them (which mirrors
    // the exporter's declaration order — the loader preserves member order). The
    // importer thus computes the SAME index k the exporter stored in its itab$C$I,
    // letting bz_itable_lookup + GEP(itab, k) reach the correct function pointer.
    // Imported symbols live in a separate list (ctx_.importedSymbols()), so the
    // local Pass 0 loop never touched them; this is the only place they are indexed.
    if (ctx_.hasImports()) {
        for (sema::Symbol* sym : ctx_.importedSymbols()) {
            auto* ts = dynamic_cast<sema::TypeSymbol*>(sym);
            if (!ts || ts->kind != ast::TypeDeclKind::Interface) continue;
            unsigned idx = 0;
            for (sema::Symbol* m : ts->members) {
                auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
                if (ms && !isStaticMethod(ms))
                    ifaceMethodIndex_[ms] = idx++;
            }
        }
    }
    // Pass A: assign vtable slots for all classes (base-first).
    for (const auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (ts && ts->kind == ast::TypeDeclKind::Class) buildVtable(ts);
    }
    // §2.5 / §2.6 / §12 (issue #328 item 2): assign vtable slots for IMPORTED
    // classes too, so a VIRTUAL method call on an imported object dispatches
    // through the object's vtable to the correct override (vtableSlot_ /
    // vtableMethods_ gain entries for the imported methods). buildVtable reads
    // each type's members (carrying reconstructed `virtual`/`override` modifiers,
    // §12) and base chain — both reconstructed by the loader — so it computes the
    // SAME slot numbering the exporter used, provided neither module introduces
    // interface slots into the shared region (a cross-module interface-dispatch
    // limitation superseded by the ifaceMethodIndex_ fix above; class-virtual
    // dispatch — the item-2 soundness fix — is also correct). No vtable GLOBAL is
    // emitted for an imported type (Pass D below iterates only local symbols); the
    // importer installs the EXTERNAL `vt$<Type>` via the exporter's bz$<Type>$init.
    if (ctx_.hasImports()) {
        for (sema::Symbol* sym : ctx_.importedSymbols()) {
            auto* ts = dynamic_cast<sema::TypeSymbol*>(sym);
            if (ts && ts->kind == ast::TypeDeclKind::Class) buildVtable(ts);
        }
    }
    // Pass B: declare a descriptor-array global (correctly sized, no
    // initializer yet) for every class/interface, so descriptors can
    // cross-reference each other in pass C.
    //
    // #596 / §21 / §31 / §12: built-in exception types (e.g. Exception,
    // ArgumentException) have no user TypeSymbol and are absent from
    // TypeSymbol::baseTypes (declaration == nullptr), so collectTypeAndBases
    // never reaches them.  For a user class that inherits a built-in exception
    // base (e.g. `class ErrA: Exception`), the descriptor array must also list
    // the builtin ancestor descriptors (btd$Exception, …) so emitIsA's linear
    // walk finds them when testing `e is Exception` or matching a catch arm.
    // We build a parallel map builtinChainOf to record the names, then use it
    // in Pass C to append the builtin GlobalVariable* entries.
    //
    // Build a parent map once for the builtin hierarchy so we can walk upward.
    std::unordered_map<std::string, std::string> builtinParent;
    for (const auto& [name, par] : builtinExceptionHierarchy())
        builtinParent[name] = par;

    std::unordered_map<const sema::TypeSymbol*,
                       std::vector<const sema::TypeSymbol*>> supersOf;
    // #596: builtin ancestor-chain names per user-class TypeSymbol.
    std::unordered_map<const sema::TypeSymbol*,
                       std::vector<std::string>> builtinChainOf;
    for (const auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (!ts) continue;
        if (ts->kind != ast::TypeDeclKind::Class &&
            ts->kind != ast::TypeDeclKind::Interface)
            continue;
        std::vector<const sema::TypeSymbol*> supers;
        collectTypeAndBases(ts, supers);
        supersOf[ts] = supers;
        // #596 / §12 / §21 / §31: collect the full builtin ancestor chain
        // for any user class (or user-class ancestor) that directly inherits a
        // built-in exception type.  directBuiltinExceptionBase walks the AST
        // TypeDecl::baseTypes chain at each user-class level to find the first
        // built-in name; we then walk builtinParent to reach the root.
        std::vector<std::string> builtinChain;
        std::string bn = directBuiltinExceptionBase(ts);
        while (!bn.empty()) {
            builtinChain.push_back(bn);
            auto pit = builtinParent.find(bn);
            bn = (pit != builtinParent.end()) ? pit->second : std::string{};
        }
        builtinChainOf[ts] = builtinChain;
        auto* arrTy = llvm::ArrayType::get(
            ptrTy(), supers.size() + builtinChain.size() + 1);
        // §2.5 / §2.6 / §12 (issue #328 / #103): a PUBLIC, non-generic class or
        // interface's type DESCRIPTOR (`td$<Type>`) is part of its cross-module
        // surface. A LOCAL class deriving from an IMPORTED base must embed the
        // base's descriptor in its own descriptor chain (Pass C) and reference it
        // for `is`/`as`; an importer implementing/using an imported interface or
        // boxing an imported value type likewise needs the descriptor. So the
        // descriptor carries EXTERNAL linkage under the same export condition the
        // vtable (`vt$<Type>`) uses, letting the importer link against this exact
        // descriptor instead of fabricating its own (which would break `is`/`as`
        // identity and, for a missing imported-base entry, crash global emission).
        // A non-public type or a generic template (per-instantiation descriptor)
        // keeps INTERNAL linkage, so single-module codegen and private types are
        // unchanged.
        bool exportedDesc = ts->visibility == ast::Visibility::Public &&
                            ts->genericParams.empty();
        descArray_[ts] = new llvm::GlobalVariable(
            *module_, arrTy, /*isConstant=*/true,
            exportedDesc ? llvm::GlobalValue::ExternalLinkage
                         : llvm::GlobalValue::InternalLinkage,
            nullptr, "td$" + std::string(ts->name));
    }
    // §2.5 / §2.6 / §12 (issue #328 / #103): declare EXTERNAL descriptor globals
    // for IMPORTED public, non-generic classes/interfaces that a local type's
    // descriptor chain (Pass C) or vtable (Pass D) must reference — chiefly a
    // local class deriving from an imported base, whose descriptor must embed the
    // imported base's `td$<Base>` and whose vtable's slot 0 is its own descriptor
    // (which transitively references the base's). Without this, descArray_[base]
    // is a null map default in Pass C → a malformed ConstantArray → SIGSEGV in
    // LLVM global emission. The size matches the exporter's: [self + transitive
    // bases, null], computed from the loader-reconstructed base chain so the
    // declared type exactly matches the exporter's definition. The body lives in
    // the dependency object (exported above); here we only declare it.
    if (ctx_.hasImports()) {
        for (sema::Symbol* sym : ctx_.importedSymbols()) {
            auto* ts = dynamic_cast<sema::TypeSymbol*>(sym);
            if (!ts) continue;
            if (ts->kind != ast::TypeDeclKind::Class &&
                ts->kind != ast::TypeDeclKind::Interface)
                continue;
            if (descArray_.count(ts)) continue;
            if (ts->visibility != ast::Visibility::Public ||
                !ts->genericParams.empty())
                continue;
            std::vector<const sema::TypeSymbol*> supers;
            collectTypeAndBases(ts, supers);
            auto* arrTy = llvm::ArrayType::get(ptrTy(), supers.size() + 1);
            descArray_[ts] = new llvm::GlobalVariable(
                *module_, arrTy, /*isConstant=*/true,
                llvm::GlobalValue::ExternalLinkage, /*init=*/nullptr,
                "td$" + std::string(ts->name));
        }
    }
    // Pass C: fill each descriptor with [self + transitive bases, null]. A base
    // that is imported is in descArray_ as an EXTERNAL declaration from the loop
    // above; guard with .find so a not-yet-reconstructed base lowers to null
    // rather than a default-inserted null map entry (which yields a malformed
    // ConstantArray and crashes global emission, issue #103).
    // #596 / §21 / §31 / §12: after user-type supers, also append the builtin
    // ancestor descriptors (builtinDesc_) so emitIsA finds them when testing
    // `e is Exception` or dispatching a `catch var e: Exception` arm.
    for (auto& [ts, supers] : supersOf) {
        std::vector<llvm::Constant*> elems;
        for (const sema::TypeSymbol* s : supers) {
            auto it = descArray_.find(s);
            elems.push_back(it != descArray_.end()
                                ? llvm::cast<llvm::Constant>(it->second)
                                : llvm::ConstantPointerNull::get(ptrTy()));
        }
        // #596: append builtin exception ancestor descriptors (btd$Exception, …)
        // for any user class that inherits a built-in exception type.
        for (const std::string& bn : builtinChainOf[ts]) {
            auto bit = builtinDesc_.find(bn);
            elems.push_back(bit != builtinDesc_.end()
                                ? llvm::cast<llvm::Constant>(bit->second)
                                : llvm::ConstantPointerNull::get(ptrTy()));
        }
        elems.push_back(llvm::ConstantPointerNull::get(ptrTy()));
        auto* arrTy = llvm::cast<llvm::ArrayType>(
            descArray_[ts]->getValueType());
        descArray_[ts]->setInitializer(
            llvm::ConstantArray::get(arrTy, elems));
    }
    // Pass D: build per-interface itables (itab$C$I), itmaps (itmap$C), and
    // each class's vtable global. New layout (CG35-itable, §13):
    //   [0]=td$C, [1]=itmap$C, [2]=Dispose, [3..]=virtual methods.
    // The dense shared slot region (ifaceMethodSlot_/ifaceSlotCount_) is gone.
    for (const auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (!ts || ts->kind != ast::TypeDeclKind::Class) continue;

        // Collect the interfaces that C (transitively) implements. For each
        // interface I, build itab$C$I: an array of fn-ptrs in I's non-static
        // method declaration order. Then assemble itmap$C.
        std::vector<const sema::TypeSymbol*> supers;
        collectTypeAndBases(ts, supers);

        std::vector<llvm::Constant*> itmapElems;  // [td$I, itab$C$I, ..., null]
        for (const sema::TypeSymbol* iface : supers) {
            if (iface->kind != ast::TypeDeclKind::Interface) continue;

            // Collect iface's non-static methods in declaration order.
            std::vector<sema::MethodSymbol*> ifaceMethods;
            for (sema::Symbol* m : iface->members) {
                auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
                if (ms && !isStaticMethod(ms)) ifaceMethods.push_back(ms);
            }
            // §24.7 (#477 B7): collect iface's sync events in declaration order
            // (after the methods, matching ifaceEventIndex_'s numbering).
            std::vector<sema::EventSymbol*> ifaceEvents;
            for (sema::Symbol* m : iface->members) {
                auto* es = dynamic_cast<sema::EventSymbol*>(m);
                if (!es) continue;
                auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl);
                if (ed && !ed->isAsync) ifaceEvents.push_back(es);
            }
            // #481 / §13 / §23: collect iface's properties in declaration order
            // (after the events, matching ifacePropertyGetIndex_'s numbering).
            std::vector<sema::PropertySymbol*> ifaceProps;
            for (sema::Symbol* m : iface->members) {
                auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
                if (ps && !ps->isIndexer) ifaceProps.push_back(ps);
            }
            // Skip a wholly empty interface (no methods, events, or properties).
            if (ifaceMethods.empty() && ifaceEvents.empty() &&
                ifaceProps.empty())
                continue;

            // Build itab$C$I: array of function pointers (C's impl of each
            // method of I, in I's declaration order). Missing impls → null.
            std::vector<llvm::Constant*> itabElems;
            for (sema::MethodSymbol* im : ifaceMethods) {
                llvm::Constant* fp = llvm::ConstantPointerNull::get(ptrTy());
                if (sema::MethodSymbol* impl = findMethodByName(ts, im->name)) {
                    auto mit = methodFns_.find(impl);
                    if (mit != methodFns_.end())
                        fp = llvm::cast<llvm::Constant>(mit->second);
                }
                itabElems.push_back(fp);
            }
            // §24.7 (#477 B7): append a per-class accessor for each interface
            // event. The accessor returns the implementing object's backing
            // EventSource<T> (the class's own event field). Interface-typed
            // event access dispatches here to reach the real source.
            for (sema::EventSymbol* ie : ifaceEvents) {
                llvm::Constant* fp = llvm::ConstantPointerNull::get(ptrTy());
                if (const sema::EventSymbol* impl =
                        classEventByName(ts, ie->name))
                    if (llvm::Function* acc = emitEventAccessor(ts, impl))
                        fp = llvm::cast<llvm::Constant>(acc);
                itabElems.push_back(fp);
            }
            // #481 / §13 / §23: append a per-class accessor function for each
            // interface property (getter, then setter if the interface property
            // declares one) — the same accessor shape installed in the class's
            // vtable. Interface-typed property access dispatches here to reach
            // the implementing class's get/set body.
            for (sema::PropertySymbol* ip : ifaceProps) {
                sema::PropertySymbol* impl =
                    classPropertyByName(ts, ip->name);
                llvm::Constant* getFp =
                    llvm::ConstantPointerNull::get(ptrTy());
                if (impl)
                    if (llvm::Function* acc =
                            ensurePropertyAccessorFn(ts, impl, /*isSet=*/false))
                        getFp = llvm::cast<llvm::Constant>(acc);
                itabElems.push_back(getFp);
                if (propertyHasSetter(ip)) {
                    llvm::Constant* setFp =
                        llvm::ConstantPointerNull::get(ptrTy());
                    if (impl)
                        if (llvm::Function* acc = ensurePropertyAccessorFn(
                                ts, impl, /*isSet=*/true))
                            setFp = llvm::cast<llvm::Constant>(acc);
                    itabElems.push_back(setFp);
                }
            }
            auto* itabArrTy = llvm::ArrayType::get(ptrTy(), itabElems.size());
            auto* itabGV = new llvm::GlobalVariable(
                *module_, itabArrTy, /*isConstant=*/true,
                llvm::GlobalValue::InternalLinkage,
                llvm::ConstantArray::get(itabArrTy, itabElems),
                "itab$" + std::string(ts->name) + "$" +
                    std::string(iface->name));
            itables_[{ts, iface}] = itabGV;

            // Append td$I and itab$C$I to the itmap pair list.
            auto dit2 = descArray_.find(iface);
            if (dit2 != descArray_.end()) {
                itmapElems.push_back(
                    llvm::cast<llvm::Constant>(dit2->second));
                itmapElems.push_back(
                    llvm::ConstantExpr::getPointerCast(itabGV, ptrTy()));
            }
        }
        // Null terminator.
        itmapElems.push_back(llvm::ConstantPointerNull::get(ptrTy()));
        auto* itmapArrTy = llvm::ArrayType::get(ptrTy(), itmapElems.size());
        auto* itmapGV = new llvm::GlobalVariable(
            *module_, itmapArrTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(itmapArrTy, itmapElems),
            "itmap$" + std::string(ts->name));
        itmaps_[ts] = itmapGV;

        // Build vtable (CG35-itable + §8.6/§8.5 #417): [0]=td$C, [1]=itmap$C,
        // [2]=Dispose, [3]=ToString, [4..]=virtuals.
        const auto& slots = vtableMethods_[ts];
        std::vector<llvm::Constant*> elems(kToStringSlot + 1 + slots.size(),
                                           llvm::ConstantPointerNull::get(
                                               ptrTy()));
        elems[0] = descArray_[ts];
        elems[1] = llvm::ConstantExpr::getPointerCast(itmapGV, ptrTy());
        auto dit = disposerFns_.find(ts);
        if (dit != disposerFns_.end())
            elems[2] = llvm::cast<llvm::Constant>(dit->second);
        // §8.5 / §8.6 (#417): uniform ToString at the fixed slot 3 — the user
        // override (via the class chain) or the type-name default.
        elems[kToStringSlot] = classToStringThunk(ts);
        // Virtual members at [4..] (kToStringSlot + 1 + i). A method slot holds
        // the most-derived method (already resolved in slots[]); a property
        // accessor slot (#481) holds the most-derived accessor FUNCTION for that
        // property, found by name on `ts`'s chain (so a derived override fills a
        // base-introduced slot, and an abstract base's slot is filled by the
        // concrete override).
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const VSlot& vs = slots[i];
            if (vs.method) {
                auto mit = methodFns_.find(vs.method);
                if (mit != methodFns_.end())
                    elems[kToStringSlot + 1 + i] =
                        llvm::cast<llvm::Constant>(mit->second);
            } else if (vs.prop) {
                // #481 / §12.5: resolve the most-derived accessor on `ts`'s
                // chain. classPropertyByName returns the override if present,
                // else the base declaration (abstract → no body → null fn).
                sema::PropertySymbol* impl =
                    classPropertyByName(ts, vs.prop->name);
                if (impl)
                    if (llvm::Function* fn =
                            ensurePropertyAccessorFn(ts, impl, vs.isSet))
                        elems[kToStringSlot + 1 + i] =
                            llvm::cast<llvm::Constant>(fn);
            }
        }
        auto* arrTy = llvm::ArrayType::get(ptrTy(), elems.size());
        auto* init = llvm::ConstantArray::get(arrTy, elems);
        // §2.5 / §2.6 / §12 (issue #328 item 2): a PUBLIC class's vtable is part
        // of its cross-module instance surface. An importer constructing the type
        // (or dispatching a virtual method on an instance of it) must install
        // this exact vtable, so it carries EXTERNAL linkage under the same
        // deterministic mangling (`vt$<Type>`) the imported method/ctor externs
        // already use. A non-public type (or a generic template, whose vtable is
        // per-instantiation and not part of the importable surface) keeps the old
        // INTERNAL linkage, so single-module codegen and private types are
        // unchanged — and only the genuinely-exported types widen the link
        // surface, bounding the duplicate-symbol risk to the same name-collision
        // case the existing method/ctor mangling already has.
        bool exported = ts->visibility == ast::Visibility::Public &&
                        ts->genericParams.empty();
        vtables_[ts] = new llvm::GlobalVariable(
            *module_, arrTy, /*isConstant=*/true,
            exported ? llvm::GlobalValue::ExternalLinkage
                     : llvm::GlobalValue::InternalLinkage,
            init, "vt$" + std::string(ts->name));
        allVtables_.push_back(vtables_[ts]);  // §8.6 #417: string discriminator
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
        // §31.4 / issue #601: DisposedAccessException is under
        // InvalidOperationException per the spec hierarchy.
        {"DisposedAccessException", "InvalidOperationException"},
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
        // §30.15 (#600 phase c): ChannelClosedException — raised when Receive()
        // is called on a drained+closed Channel<T>.
        {"ChannelClosedException", "Exception"},
        // §30.14 (#600 phase c): DomainFaultedException — raised at the join
        // of a spawned domain that threw an exception. Message carries a copy
        // of the original exception's message text.
        {"DomainFaultedException", "Exception"},
        // §9.8 / issue #550: thrown by synthesised Serialize/Deserialize when
        // the binary stream is malformed (unknown fields, truncated data, etc.).
        {"SerializationException", "Exception"},
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
    // Pass 3: vtable per built-in = [descriptor, itmap (null=empty), Dispose,
    // ToString]. Built-in exceptions implement no user interfaces, so the itmap
    // is a single-null array. Layout matches the new CG35-itable + §8.6/§8.5
    // (#417) class vtable prefix:
    //   [0]=td, [1]=itmap (null-terminated, empty), [2]=Dispose, [3]=ToString.
    // A common empty itmap (just a null ptr) is shared across all builtins.
    // Each exception's ToString is the type-name default (§8.5: a class's
    // default ToString is the type's qualified name).
    auto* emptyItmapTy = llvm::ArrayType::get(ptrTy(), 1);
    auto* emptyItmap = new llvm::GlobalVariable(
        *module_, emptyItmapTy, /*isConstant=*/true,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(emptyItmapTy,
                                 {llvm::ConstantPointerNull::get(ptrTy())}),
        "itmap$bz$empty");
    for (const auto& [name, par] : hier) {
        // §8.5 / §8.6 (#417): a per-exception ToString thunk `ptr(ptr this)`
        // returning the type-name literal (the §8.5 class default). Emitted at
        // setup time; lowerStringLiteral only allocates module globals, so it is
        // safe before declareFunctions runs.
        auto* tsFnTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, false);
        auto* tsThunk = llvm::Function::Create(
            tsFnTy, llvm::Function::InternalLinkage,
            std::string("bz$extostr$") + name, module_.get());
        {
            auto* bb = llvm::BasicBlock::Create(llvm_, "entry", tsThunk);
            llvm::IRBuilder<> tb(bb);
            // lowerStringLiteral returns a constant global pointer (no builder
            // use), so we can return it directly.
            llvm::Value* nameStr = lowerStringLiteral(
                std::string("\"") + name + "\"", false);
            tb.CreateRet(nameStr);
        }
        std::vector<llvm::Constant*> v = {
            builtinDesc_[name],
            llvm::ConstantExpr::getPointerCast(emptyItmap, ptrTy()),
            builtinDisposeFn_,
            tsThunk
        };
        auto* arrTy = llvm::ArrayType::get(ptrTy(), v.size());
        builtinVtable_[name] = new llvm::GlobalVariable(
            *module_, arrTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(arrTy, v),
            std::string("bvt$") + name);
        allVtables_.push_back(builtinVtable_[name]);  // §8.6 #417
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


// #473 / §19.3: resolve a NamedPattern's QualifiedName (`Dog`, `M.Dog`) to a
// type descriptor for a type-test pattern (`emitIsA`). A bare type name in a
// pattern carries no `Type` node and thus no `resolvedType` entry, so we
// resolve it by its last name part against the local type symbols (mirroring
// the sema exhaustiveness check, ExprTypecheck.cpp, and the
// DestructuringPattern record-by-name lookup).
//
// Only CLASS and INTERFACE names resolve here: §19.3 type-test patterns
// match a reference scrutinee against a class/interface hierarchy via the
// runtime type tag. Records and enums ALSO have descArray_ entries (for
// boxing, setupBoxDescriptors), but a bare record/enum name is NOT a
// reference type test — emitIsA on an unboxed value-type struct is illegal
// IR (the type-tag walk assumes a pointer). Record matching uses the
// destructuring form `Pt(...)` (DestructuringPattern); enum constants are
// matched as ConstantPattern/NamedPattern equality. So we skip Record/Enum
// here and leave those shapes to their own handlers (returning null →
// deferred). A built-in exception class used as a pattern falls back to
// builtinDesc_.
llvm::GlobalVariable* Emitter::descriptorForPatternName(
        const ast::QualifiedName* name) {
    if (!name || name->parts.empty()) return nullptr;
    std::string_view last = name->parts.back();
    for (const auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (!ts || ts->name != last) continue;
        if (ts->kind != ast::TypeDeclKind::Class &&
            ts->kind != ast::TypeDeclKind::Interface)
            return nullptr;  // record/enum name — not a reference type test
        auto it = descArray_.find(ts);
        if (it != descArray_.end()) return it->second;
    }
    auto bit = builtinDesc_.find(std::string(last));
    if (bit != builtinDesc_.end()) return bit->second;
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


void Emitter::emitThrowValue(llvm::Value* excVal, const ast::Expr* excAst) {
    if (excVal) {
        // #397 / §31.1: a `throw <newExpr>` leaving a catch arm. Relax the
        // former #387 conservative guard now that #430's compile-time
        // diagnostic rejects every aliasing-escape path (throw/return a new
        // object whose constructor stores the catch borrow into a non-owned
        // field that outlives the arm). After #430, the only remaining case
        // where the thrown expression references the catch binding with the
        // ACTUAL EXCEPTION OBJECT (not a value copy) is a direct re-throw
        // `throw ex;` — where the same exception continues in flight and must
        // NOT be freed. All other references (member-access value projections,
        // type tests, etc.) evaluate to a copy before lowerExpr returns, so
        // freeing the caught object after excVal is already computed is safe.
        //
        // Direct-re-throw detection: the AST is a bare IdentifierExpr whose
        // name is the catch-binding name. Bare `throw;` is handled by the
        // else-branch below (excVal is null). The excVal != currentCaughtExc_
        // guard remains for defence-in-depth (the LLVM Values are always
        // distinct even for a re-throw, but belt-and-suspenders).
        bool isDirectRethrow = false;
        if (excAst && !currentCaughtName_.empty()) {
            const auto* id = dynamic_cast<const ast::IdentifierExpr*>(excAst);
            isDirectRethrow = id && id->name == currentCaughtName_;
        }
        if (currentCaughtExc_ && excVal != currentCaughtExc_ &&
            !isDirectRethrow) {
            // #397 / §28.2 / §31.1: the caught object's lifetime is ending
            // here with nothing retaining it — dispatch its idempotent
            // Dispose and reclaim its block once. This is the sole free on
            // this throw path: the caught-exception scope disposal is SKIPPED
            // on unwind (isUnwind). The new exception's expression (excVal)
            // has already been fully evaluated by lowerExpr before this call,
            // so the caught borrow is no longer needed; freeing it here is
            // safe and fixes the #387 / #397 residual leak.
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
    // bz_array_free, cascading into class elements for an owning collection),
    // or — §28.13 / stdlib §2.16 (#620) — a Shared<T>/Weak<T> handle (released
    // via bz_shared_release / bz_weak_release).
    struct OwnedField {
        const sema::FieldSymbol* fs;
        const sema::TypeRef* collTy;  // non-null → collection field
        // §28.13 (#620): true for Shared<T> owned field (bz_shared_release),
        // false for ordinary class/exception/collection field.
        bool sharedField = false;
        // §28.13 (#620): true for Weak<T> owned field (bz_weak_release).
        bool weakField = false;
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
            // (vtable slot 2 is the shared no-op bz$exDispose; the block is
            // reclaimed, the runtime-managed message string is not freed).
            owned.push_back({fs, nullptr});
        } else if (const sema::TypeRef* ct = ownedCollectionFieldType(fs)) {
            owned.push_back({fs, ct});
        } else {
            // §28.13 / stdlib §2.16 (#620): an `owned` field of Shared<T> or
            // Weak<T> type.  These are compiler-known ref-counted heap objects
            // with no user TypeDecl / no vtable; release via the appropriate
            // runtime function instead of the vtable-Dispose path.
            bool isWeak = false;
            if (isOwnedSharedWeakField(fs, &isWeak)) {
                OwnedField of{fs, nullptr};
                if (isWeak) of.weakField = true;
                else        of.sharedField = true;
                owned.push_back(of);
            }
        }
    }
    for (auto it = owned.rbegin(); it != owned.rend(); ++it) {
        llvm::StructType* st = classStruct(ts);
        unsigned idx = fieldIndex_[it->fs];
        llvm::Value* addr = builder_.CreateStructGEP(st, currentThis_, idx);
        llvm::Value* val = builder_.CreateLoad(ptrTy(), addr);
        // #362 / stdlib §4.5: an owned collection field frees its backing and
        // cascades disposal into its class elements (for an owning collection).
        // bz_array_free / bz_llist_free are null-safe, so no field null-check needed.
        if (it->collTy) {
            // #602 / §28.11 v0.135: derive element disposer directly from
            // elementPolicyFor (covering class, interface, Shared/Weak elements).
            // A borrowing Array/Stack/Queue<string> co-owns a +1 per element
            // (retainRelease=true); use the string-release thunk for that row.
            // The two paths are mutually exclusive (string is not a class).
            const sema::TypeRef* collElem =
                (!it->collTy->genericArgs.empty()) ? it->collTy->genericArgs[0]
                                                   : nullptr;
            bool collIsOwning =
                it->collTy->astNode && it->collTy->astNode->name &&
                !it->collTy->astNode->name->parts.empty() &&
                it->collTy->astNode->name->parts[0].rfind("Owned", 0) == 0;
            ElementPolicy fieldCollPol = elementPolicyFor(collElem, collIsOwning);
            llvm::Function* elemDisp = fieldCollPol.disposerThunk;
            if (!elemDisp && fieldCollPol.cat == ElementPolicy::Category::RuntimeManaged &&
                    collElem && collElem->kind == sema::TypeRef::Kind::Primitive &&
                    collElem->primitive == sema::PrimitiveType::String &&
                    !collElem->nullable &&
                    it->collTy->astNode && it->collTy->astNode->name &&
                    !it->collTy->astNode->name->parts.empty() &&
                    [](std::string_view n){ return n == "Array" || n == "Stack" ||
                        n == "Queue"; }(it->collTy->astNode->name->parts[0]))
                elemDisp = stringElementReleaseThunk();
            llvm::Value* disp = elemDisp
                ? llvm::cast<llvm::Value>(elemDisp)
                : llvm::ConstantPointerNull::get(ptrTy());
            // per §4.6/§4.7: LinkedList fields free via bz_llist_free.
            bool isLL = isLinkedListTypeName(it->collTy);
            const char* freeName = isLL ? "bz_llist_free" : "bz_array_free";
            auto* fn = declareC(freeName,
                                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {val, disp});
            continue;
        }
        // §28.13 / stdlib §2.16 (#620): owned Shared<T>/Weak<T> field —
        // release via bz_shared_release / bz_weak_release (null-safe, no vtable).
        // Null the field after so a re-entrant dispose is a no-op.
        if (it->sharedField || it->weakField) {
            const char* relName = it->weakField ? "bz_weak_release"
                                                : "bz_shared_release";
            auto* relFn = declareC(relName, llvm::Type::getVoidTy(llvm_),
                                   {ptrTy()});
            builder_.CreateCall(relFn, {val});
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
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

    // #429 / §4.6 / §28.9: release every STRING field / auto-property of THIS
    // class (own members only; an inherited slot is released by the base CORE
    // disposer via emitBaseDisposeChain — so visiting only ts->members avoids a
    // double-release). A class string field holds an INDEPENDENT +1 (retained at
    // store via lowerStringFieldStore / armed in emitInstanceInit's initializer
    // path); on disposal that +1 is released exactly once, freeing the
    // data+header at rc 0. bz_string_release is null-safe (a never-assigned
    // string? slot is null in the calloc'd object) and a no-op on a sentinel
    // (a field holding a literal is never freed). After releasing, NULL the slot
    // so a re-entrant / double dispose finds null and skips (the #396 slot-null
    // precedent) — the §28.6 run-once flag already guards the public Dispose, and
    // this nulling guards any base-core re-entry. A local that aliases this field
    // value holds its OWN +1 (#429 borrow-retain), so this release never dangles
    // it (no UAF). A RECORD has no disposer (§9.7) and its string fields are
    // borrows (§9) — never reached here.
    {
        llvm::StructType* sst = classStruct(ts);
        for (const sema::Symbol* m : ts->members) {
            bool isField = dynamic_cast<const sema::FieldSymbol*>(m) != nullptr;
            if (!isField && !isAutoProperty(m)) continue;
            if (fieldIndex_.find(m) == fieldIndex_.end()) continue;
            if (!isStringStorageMember(m)) continue;
            unsigned idx = fieldIndex_[m];
            llvm::Value* addr = builder_.CreateStructGEP(sst, currentThis_, idx,
                                                         "strf.addr");
            llvm::Value* val = builder_.CreateLoad(ptrTy(), addr, "strf.val");
            emitStringRelease(val);
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        }
    }

    // #429 (closure FIELD-STORE category) / §27.5 / §28.9: release every CLOSURE
    // field / auto-property of THIS class (own members only; an inherited slot
    // is released by the base CORE disposer via emitBaseDisposeChain — so
    // visiting only ts->members avoids a double-release). A class closure field
    // holds an INDEPENDENT +1 (retained at store via lowerClosureFieldStore /
    // emitInstanceInit's initializer path); on disposal that +1 is released
    // exactly once via bz_closure_release, freeing the cell + env block at rc 0.
    // bz_closure_release is null-safe (a never-assigned closure field slot is
    // null in the calloc'd object) and does NOT touch captured values (they are
    // borrows, §28.9 — no double-dispose of a capture). After releasing, NULL
    // the slot so a re-entrant / double dispose finds null and is a no-op (the
    // #396 slot-null precedent, parallel to the string-field nulling above).
    {
        llvm::StructType* cst = classStruct(ts);
        for (const sema::Symbol* m : ts->members) {
            bool isField = dynamic_cast<const sema::FieldSymbol*>(m) != nullptr;
            if (!isField && !isAutoProperty(m)) continue;
            if (fieldIndex_.find(m) == fieldIndex_.end()) continue;
            if (!isClosureStorageMember(m)) continue;
            unsigned idx = fieldIndex_[m];
            llvm::Value* addr = builder_.CreateStructGEP(cst, currentThis_, idx,
                                                         "clf.addr");
            llvm::Value* val = builder_.CreateLoad(ptrTy(), addr, "clf.val");
            emitClosureRelease(val);
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        }
    }

    // #429 (box FIELD-STORE category) / §8.6 / §28.9: release every Object/
    // interface field / auto-property of THIS class (own members only; an
    // inherited slot is released by the base CORE disposer via
    // emitBaseDisposeChain — visiting only ts->members avoids double-release).
    // A box storage field holds an INDEPENDENT +1 (taken at store via
    // lowerBoxFieldStore / emitInstanceInit); on disposal that +1 is released
    // EXACTLY ONCE via bz_box_release, freeing the box wrapper at rc 0. Safety:
    //   - isBoxStorageMember gates on boxStorageFields_, which is populated ONLY
    //     for fields where collectBoxStorageFields + lowerBoxFieldStore confirmed
    //     EVERY store uses a boxable value source. A field that ever received a
    //     class-reference store is NOT in boxStorageFields_ → NOT released here.
    //     This is the cardinal box-vs-class discriminator: bz_box_release is
    //     NEVER called on a class instance. (Corrupting offset 8 of a class
    //     instance — the disposed-flag word — would cause double-dispose / UAF.)
    //   - bz_box_release is null-safe (a calloc-zeroed null field is a no-op).
    //   - After releasing, NULL the slot so a re-entrant / double dispose finds
    //     null and is a no-op (the #396 slot-null precedent, parallel to the
    //     string-field and closure-field nulling above).
    //   - The boxed value is a value-type leaf (no owned sub-objects): the
    //     release frees ONLY the wrapper — no cascade.
    {
        llvm::StructType* bst = classStruct(ts);
        for (const sema::Symbol* m : ts->members) {
            bool isField = dynamic_cast<const sema::FieldSymbol*>(m) != nullptr;
            if (!isField && !isAutoProperty(m)) continue;
            if (fieldIndex_.find(m) == fieldIndex_.end()) continue;
            if (!isBoxStorageMember(m)) continue;
            unsigned idx = fieldIndex_[m];
            llvm::Value* addr = builder_.CreateStructGEP(bst, currentThis_, idx,
                                                         "boxf.addr");
            llvm::Value* val = builder_.CreateLoad(ptrTy(), addr, "boxf.val");
            emitBoxRelease(val);
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        }
    }

    // #480 / §24.1 / §28: free each instance event's backing EventSource.
    // emitInstanceInit allocated one bz_event_source_new per instance EventSymbol;
    // those allocations are NOT visited by the FieldSymbol loop above because
    // EventSymbol does not inherit from FieldSymbol and the dynamic_cast returns
    // null.  Collect all instance-event symbols that have a fieldIndex_ entry
    // (synchronous non-static events, the same predicate as collectInstanceStorage)
    // and emit a null-guarded bz_event_source_free call for each, then store null
    // back so a re-entrant dispose is a no-op.  Ordering relative to FieldSymbol
    // disposals is not safety-critical: EventSource.free (handler array + struct)
    // does not cascade into any owned sub-objects tracked by this class.
    llvm::StructType* st = classStruct(ts);
    auto* freeFn = declareC("bz_event_source_free",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
    for (const sema::Symbol* m : ts->members) {
        auto* es = dynamic_cast<const sema::EventSymbol*>(m);
        if (!es) continue;
        auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl);
        if (!ed || ed->isStatic || ed->isAsync) continue;  // instance sync only
        auto idxIt = fieldIndex_.find(es);
        if (idxIt == fieldIndex_.end()) continue;
        unsigned idx = idxIt->second;
        llvm::Value* addr = builder_.CreateStructGEP(st, currentThis_, idx,
                                                     "evt.addr");
        llvm::Value* val  = builder_.CreateLoad(ptrTy(), addr, "evt.src");
        // Null-guard: calloc zeroes the slot; if the ctor threw before
        // emitInstanceInit allocated this source the slot is still null.
        llvm::Value* isNull = builder_.CreateICmpEQ(
            val, llvm::ConstantPointerNull::get(ptrTy()));
        auto* freeBB = llvm::BasicBlock::Create(llvm_, "evt.free",  currentFn_);
        auto* contBB = llvm::BasicBlock::Create(llvm_, "evt.cont",  currentFn_);
        builder_.CreateCondBr(isNull, contBB, freeBB);
        builder_.SetInsertPoint(freeBB);
        builder_.CreateCall(freeFn, {val});
        // Null the slot so a second disposal pass (re-entrant or double-dispose)
        // finds null and skips, mirroring the FieldSymbol disposal pattern above.
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
        builder_.CreateBr(contBB);
        builder_.SetInsertPoint(contBB);
    }
}


// §28.5: does `ts` declare at least one `owned` field whose type is one of its
// own generic type parameters (`private owned _slot: T`)? Such a field is
// classified — and so auto-disposed — at INSTANTIATION (the substituted type),
// not in the shared type-erased disposer, so a class with one needs a per-
// instantiation specialized Dispose/vtable.
bool Emitter::classHasOwnedTypeParamField(const sema::TypeSymbol* ts) {
    if (!ts) return false;
    for (const sema::Symbol* m : ts->members) {
        auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
        if (fs && isOwnedTypeParamFieldShape(fs)) return true;
    }
    return false;
}


// §28.5: build (once per instantiation) the specialized core + public disposer
// for the active monomorphization of `ts`. Mirrors emitDisposerCore /
// emitDisposerBody but is emitted with the current genericSubst_ active, so an
// owned type-parameter field is laid out (the spec struct) and classified (the
// substituted concrete type) correctly. `suffix` disambiguates the function
// names across instantiations. Returns the public idempotent Dispose.
//
// State: the caller (lowerNew, mid-body) owns builder_/currentFn_/etc.; we save
// the full disposer-emit state and the builder insert point and restore them so
// the caller can continue emitting where it left off.
llvm::Function* Emitter::emitSpecializedDisposer(const sema::TypeSymbol* ts,
                                                 const std::string& suffix) {
    auto* fnType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(llvm_), {ptrTy()}, /*isVarArg=*/false);
    auto* pubFn = llvm::Function::Create(
        fnType, llvm::Function::InternalLinkage,
        "bz$" + std::string(ts->name) + "$Dispose" + suffix, module_.get());
    auto* coreFn = llvm::Function::Create(
        fnType, llvm::Function::InternalLinkage,
        "bz$" + std::string(ts->name) + "$DisposeCore" + suffix, module_.get());

    // Save all state the disposer-emit clobbers, plus the builder insert point.
    llvm::Function* savedFn = currentFn_;
    llvm::Value* savedThis = currentThis_;
    auto savedLocals = locals_;
    auto savedParamSlots = paramSlots_;
    auto savedRefParams = refParams_;
    auto savedDisposeStack = disposeStack_;
    auto savedLoopStack = loopStack_;
    auto savedFinallyStack = finallyStack_;
    llvm::BasicBlock* savedPropagate = propagateBB_;
    llvm::BasicBlock* savedUnwind = unwindTarget_;
    llvm::Value* savedCaughtExc = currentCaughtExc_;
    std::string_view savedCaughtName = currentCaughtName_;
    std::size_t savedScopeDepth = unwindScopeDepth_;
    llvm::IRBuilder<>::InsertPoint savedIP = builder_.saveIP();

    // The core (destructor body + owned-field disposal + base chain) and the
    // public run-once wrapper — identical to emitDisposerCore / emitDisposerBody
    // but specialized: genericSubst_ stays active so classStruct(ts) resolves to
    // the spec struct and isOwnedReferenceField sees the concrete T.
    emitDisposerCore(ts, coreFn);

    currentFn_ = pubFn;
    currentThis_ = &*pubFn->args().begin();
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
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", pubFn));
    llvm::StructType* st = classStruct(ts);
    unsigned fi = disposedFlagIndex(ts);
    llvm::Value* flagAddr =
        builder_.CreateStructGEP(st, currentThis_, fi, "disposed.flag");
    llvm::Value* already = builder_.CreateICmpNE(
        builder_.CreateLoad(disposedFlagTy(), flagAddr),
        llvm::ConstantInt::get(disposedFlagTy(), 0), "already.disposed");
    auto* doBB = llvm::BasicBlock::Create(llvm_, "dispose.do", pubFn);
    auto* retBB = llvm::BasicBlock::Create(llvm_, "dispose.ret", pubFn);
    builder_.CreateCondBr(already, retBB, doBB);
    builder_.SetInsertPoint(doBB);
    builder_.CreateStore(llvm::ConstantInt::get(disposedFlagTy(), 1), flagAddr);
    builder_.CreateCall(coreFn, {currentThis_});
    builder_.CreateBr(retBB);
    builder_.SetInsertPoint(retBB);
    builder_.CreateRetVoid();

    // Restore the caller's state and insert point.
    currentFn_ = savedFn;
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals);
    paramSlots_ = std::move(savedParamSlots);
    refParams_ = std::move(savedRefParams);
    disposeStack_ = std::move(savedDisposeStack);
    loopStack_ = std::move(savedLoopStack);
    finallyStack_ = std::move(savedFinallyStack);
    propagateBB_ = savedPropagate;
    unwindTarget_ = savedUnwind;
    currentCaughtExc_ = savedCaughtExc;
    currentCaughtName_ = savedCaughtName;
    unwindScopeDepth_ = savedScopeDepth;
    builder_.restoreIP(savedIP);
    return pubFn;
}


// §28.5 / §14.1 / §2.5 (#662 foundation A): build (or return the cached)
// per-instantiation vtable for an IMPORTED generic class using the currently
// active genericSubst_.  Unlike specializedVtableFor (which patches an
// existing shared `vt$<T>`), this builds a FRESH vtable from scratch because
// the exporter emits NO shared `vt$<T>` for a generic class (§2.5: only
// public non-generic types export their vtable).
//
// Layout (matches declareVtables Pass D):
//   [0] per-instantiation descriptor  (td$<Type>$specN  = [self, null])
//   [1] per-instantiation itmap       (null-terminated, no interfaces yet)
//   [2] specialized Dispose           (via emitSpecializedDisposer)
//   [3] ToString thunk                (type-name default or methodFns_ entry)
//   [4+] virtual method slots         (methodFns_ entries where bodies exist)
//
// Also populates disposerCoreFns_[ts] (blocker-4 residue from foundation B:
// emitBaseDisposeChain needs the core fn for any derived class that inherits
// this imported generic base).
//
// Cached in specializedVtables_[ts][specFieldTys] so Box<int> and Box<string>
// get distinct vtables.
llvm::GlobalVariable* Emitter::buildImportedGenericVtable(
    const sema::TypeSymbol* ts) {
    if (!ts || ts->kind != ast::TypeDeclKind::Class) return nullptr;
    if (genericSubst_.empty()) return nullptr;

    // Cache key: same spec-field-types vector as specializedStructs_ /
    // specializedVtables_ so each concrete instantiation gets exactly one vtable.
    std::vector<const sema::Symbol*> storage;
    collectInstanceStorage(ts, storage);
    std::vector<llvm::Type*> specFieldTys;
    specFieldTys.push_back(ptrTy());           // [0] vtable ptr
    if (hasBuiltinExceptionBase(ts))
        specFieldTys.push_back(ptrTy());       // [1] message ptr (rare for generics)
    specFieldTys.push_back(disposedFlagTy());  // [1 or 2] disposed flag
    for (const sema::Symbol* m : storage)
        specFieldTys.push_back(storageType(m));

    auto& specMap = specializedVtables_[ts];
    auto sit = specMap.find(specFieldTys);
    if (sit != specMap.end()) return sit->second;

    // Suffix for de-duplicating names (Box$spec0, Box$spec1, …).
    std::string suffix = "$spec" + std::to_string(specMap.size());
    std::string tsName = std::string(ts->name);

    // --- (a) Per-instantiation descriptor: [self, null] ---
    // A minimal descriptor that lets emitIsA / type tests recognize the object.
    // We use a 2-element array [selfDesc, null] (no base entries — the imported
    // generic class has no known base TypeSymbol here; Foundation C can extend
    // this when base-chain information is available cross-module).
    auto* descArrTy = llvm::ArrayType::get(ptrTy(), 2);
    auto* descGV = new llvm::GlobalVariable(
        *module_, descArrTy, /*isConstant=*/true,
        llvm::GlobalValue::InternalLinkage, /*init=*/nullptr,
        "td$" + tsName + suffix);
    // Self-referential initializer: [self, null].
    std::vector<llvm::Constant*> descElems = {
        llvm::cast<llvm::Constant>(descGV),
        llvm::ConstantPointerNull::get(ptrTy()),
    };
    descGV->setInitializer(
        llvm::ConstantArray::get(descArrTy, descElems));

    // --- (b) Per-instantiation itmap: [null] (no interfaces) ---
    auto* itmapArrTy = llvm::ArrayType::get(ptrTy(), 1);
    auto* itmapGV = new llvm::GlobalVariable(
        *module_, itmapArrTy, /*isConstant=*/true,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(itmapArrTy,
                                 {llvm::ConstantPointerNull::get(ptrTy())}),
        "itmap$" + tsName + suffix);

    // --- (c) Specialized disposer ---
    // emitSpecializedDisposer builds BOTH the public Dispose (run-once flag
    // guard) AND the DisposeCore (destructor body + owned-field disposal +
    // base chain) with genericSubst_ active, so owned T fields are classified
    // by their concrete substituted type.
    llvm::Function* specDispose = emitSpecializedDisposer(ts, suffix);

    // Blocker-4 residue from foundation B: emitBaseDisposeChain looks up
    // disposerCoreFns_[base].  If `ts` is used as a base by a local class,
    // this population makes the chain work. The core fn is the second function
    // created by emitSpecializedDisposer (named bz$<T>$DisposeCore<suffix>).
    // We reach it by walking the module's function list — or, more robustly,
    // by looking it up by name.
    {
        std::string coreName = "bz$" + tsName + "$DisposeCore" + suffix;
        if (llvm::Function* coreFn = module_->getFunction(coreName))
            if (!disposerCoreFns_.count(ts))
                disposerCoreFns_[ts] = coreFn;
    }

    // --- (d) Virtual method slots (from methodFns_ for any imported methods
    //         that were resolved and stored by declareImportedFunctions) ---
    const auto& slots = vtableMethods_[ts];  // filled by declareVtables Pass A
    std::vector<llvm::Constant*> elems(
        kToStringSlot + 1 + slots.size(),
        llvm::ConstantPointerNull::get(ptrTy()));
    elems[0] = llvm::cast<llvm::Constant>(descGV);
    elems[1] = llvm::ConstantExpr::getPointerCast(itmapGV, ptrTy());
    elems[2] = llvm::cast<llvm::Constant>(specDispose);
    elems[kToStringSlot] = classToStringThunk(ts);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const VSlot& vs = slots[i];
        if (vs.method) {
            auto mit = methodFns_.find(vs.method);
            if (mit != methodFns_.end())
                elems[kToStringSlot + 1 + i] =
                    llvm::cast<llvm::Constant>(mit->second);
        }
        // Property accessor slots for imported generics: not yet supported
        // (their bodies are also in the exporter) — slots stay null.
    }

    auto* arrTy = llvm::ArrayType::get(ptrTy(), elems.size());
    auto* init  = llvm::ConstantArray::get(arrTy, elems);
    auto* gv = new llvm::GlobalVariable(
        *module_, arrTy, /*isConstant=*/true,
        llvm::GlobalValue::InternalLinkage, init,
        "vt$" + tsName + suffix);
    allVtables_.push_back(gv);  // §8.6 #417: string discriminator
    specMap[specFieldTys] = gv;
    return gv;
}


// §28.5: the per-instantiation vtable to install in a freshly-constructed
// instance of the active monomorphization of `ts`, in place of the shared
// `vt$<T>`. Identical to the shared vtable except slot 2 (Dispose) is a
// specialized disposer that classifies and disposes the class's owned type-
// parameter field(s) using the active substitution. Falls back to the shared
// vtable when the class needs no specialization. Cached per (class, spec field
// types) so each distinct instantiation builds exactly one.
llvm::GlobalVariable* Emitter::specializedVtableFor(const sema::TypeSymbol* ts) {
    llvm::GlobalVariable* shared = vtableOf(ts);
    // No specialization needed: no active substitution, or no owned type-param
    // field. Use the shared vtable (the unchanged, type-erased disposer).
    if (!shared || genericSubst_.empty() || !classHasOwnedTypeParamField(ts))
        return shared;

    // Key by the same spec field types as the instance struct so Box<Res> and
    // Box<int> get distinct vtables (and distinct disposers).
    std::vector<const sema::Symbol*> storage;
    collectInstanceStorage(ts, storage);
    std::vector<llvm::Type*> specFieldTys;
    specFieldTys.push_back(ptrTy());
    specFieldTys.push_back(disposedFlagTy());
    for (const sema::Symbol* m : storage)
        specFieldTys.push_back(storageType(m));

    auto& specMap = specializedVtables_[ts];
    auto sit = specMap.find(specFieldTys);
    if (sit != specMap.end()) return sit->second;

    // Build the specialized disposer for THIS instantiation, then clone the
    // shared vtable's initializer with slot 2 (Dispose) replaced by it. The
    // shared vtable already has its full initializer (built in declareVtables,
    // before bodies), so every other slot (descriptor, itmap, virtual methods)
    // carries over unchanged.
    std::string suffix = "$spec" + std::to_string(specMap.size());
    llvm::Function* specDispose = emitSpecializedDisposer(ts, suffix);

    auto* arrTy = llvm::cast<llvm::ArrayType>(shared->getValueType());
    auto* sharedInit = llvm::cast<llvm::ConstantArray>(shared->getInitializer());
    std::vector<llvm::Constant*> elems;
    elems.reserve(arrTy->getNumElements());
    for (unsigned i = 0; i < arrTy->getNumElements(); ++i)
        elems.push_back(sharedInit->getAggregateElement(i));
    elems[2] = llvm::cast<llvm::Constant>(specDispose);  // slot 2 = Dispose
    auto* init = llvm::ConstantArray::get(arrTy, elems);
    auto* gv = new llvm::GlobalVariable(
        *module_, arrTy, /*isConstant=*/true,
        llvm::GlobalValue::InternalLinkage, init,
        "vt$" + std::string(ts->name) + suffix);
    specMap[specFieldTys] = gv;
    return gv;
}


llvm::GlobalVariable* Emitter::excGlobal() {
    if (!excGlobal_) {
        // fix #492 / §31.1 / §2.6: emit an ExternalLinkage DECLARATION (no
        // initializer) so all separately-compiled modules reference the SAME
        // "bz_exc" symbol defined once in bezel_runtime.cpp. Before this fix
        // each module defined its own InternalLinkage (BSS) copy under the name
        // "bz$exc"; a cross-module `throw` wrote the library's copy while the
        // importer's pending-check read a still-null copy — the exception
        // silently disappeared.
        //
        // An external declaration (nullptr initializer) in LLVM means "defined
        // elsewhere"; the linker resolves all references to the runtime's single
        // definition. The name "bz_exc" must match the name in bezel_runtime.cpp.
        // (Renamed from "bz$exc" — a `$` in an external symbol name is a
        // clang-only extension that triggers -Wdollar-in-identifier-extension in
        // code that declares it in a header. "bz_exc" is a valid C identifier.)
        excGlobal_ = new llvm::GlobalVariable(
            *module_, ptrTy(), /*isConstant=*/false,
            llvm::GlobalValue::ExternalLinkage,
            /*Initializer=*/nullptr,  // external declaration — defined in runtime
            "bz_exc");
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


void Emitter::emitUnwindDisposalsInRange(std::size_t lo, std::size_t hi) {
    // #467 / §28.3 (lines 4914, 4922): dispose the scopes in disposeStack_
    // indices [lo, hi), innermost (higher index) first — the reverse-declaration,
    // inner-before-outer order the §28.3 exception-path discharge requires. Used
    // when a `try`-with-`finally` re-propagates an exception to its enclosing
    // handler after running its own finally: the throw site only disposed scopes
    // down to the inner try's depth (unwindScopeDepth_), so any scope that lies
    // BETWEEN the inner try and the enclosing handler (e.g. an outer `using var`
    // declared in the same block as, but textually before, the inner try) is
    // still live and must be disposed here — AFTER the explicit finally body, as
    // the scope unwinds further outward (§28.3: "the explicit finally runs first
    // ... then the implicit auto-dispose runs as the scope unwinds further").
    // All entries are disposed on the isUnwind path; caughtException entries are
    // skipped — they stay in flight for the enclosing handler.
    for (std::size_t i = hi; i-- > lo;)
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


void Emitter::emitNewPendingCheck(llvm::Value* obj) {
    // §22.9 / §28.3 (FINDING [4]): same as emitPendingCheck, but on the unwind
    // edge ALSO reclaim the in-flight instance block `obj` whose constructor
    // threw. The ctor's own cleanup disposed the owned fields it had assigned and
    // the base subobject (it does not free `this`); freeing the block here — the
    // allocating site — reclaims it exactly once. This branch is taken only when
    // the ctor set the pending exception, so the free never reaches a normally-
    // constructed object (which is reclaimed later by its owning slot).
    llvm::Value* exc = builder_.CreateLoad(ptrTy(), excGlobal());
    llvm::Value* pending = builder_.CreateICmpNE(
        exc, llvm::ConstantPointerNull::get(ptrTy()));
    auto* unwind =
        llvm::BasicBlock::Create(llvm_, "new.unwind", currentFn_);
    auto* cont = llvm::BasicBlock::Create(llvm_, "new.cont", currentFn_);
    builder_.CreateCondBr(pending, unwind, cont);
    builder_.SetInsertPoint(unwind);
    builder_.CreateCall(freeFn(), {obj});  // reclaim the instance block once
    emitUnwindDisposals();
    builder_.CreateBr(unwindBB());
    builder_.SetInsertPoint(cont);
}


void Emitter::emitDynamicDispose(llvm::Value* obj) {
    // §28.2 / §28.4: dispatch the object's PUBLIC, idempotent Dispose through
    // its vtable (slot 2, CG35-itable) — which runs the destructor body, owned-field
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
    // safe. vtable layout (CG35-itable + §8.6/§8.5 #417): [0]=td$, [1]=itmap$,
    // [2]=Dispose, [3]=ToString, [4+]=virtuals. Dispose stays at the FIXED slot 2.
    llvm::Value* vptr = builder_.CreateLoad(ptrTy(), obj);
    llvm::Value* slot = builder_.CreateGEP(ptrTy(), vptr,
                                           {builder_.getInt32(2)});
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
    // §28.2 v0.134 / issue #601: in checked disposal mode the block is
    // quarantined (not freed) after disposal so that the per-instance disposed
    // flag at slot 1 (or 2) remains readable when a subsequent member-entry
    // guard loads it. This is a deliberate memory leak in checked (debug)
    // builds; the spec explicitly endorses deferred reclamation here. In
    // unchecked (release) mode the block is freed immediately as before.
    if (!checkedDisposal_)
        builder_.CreateCall(freeFn(), {obj});
}


llvm::Function* Emitter::elementDisposerThunk() {
    // #362 / stdlib §4.5: a standalone `void(void*)` thunk that runs an owning
    // collection element's destructor via the element's own vtable (slot 2,
    // CG35-itable), skipping a null element. Built once with its own IRBuilder
    // so the active `builder_` insert point is untouched.
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
    // vtable layout (CG35-itable): [0]=td$, [1]=itmap$, [2]=Dispose.
    llvm::Value* vptr = b.CreateLoad(ptrTy(), obj);
    llvm::Value* slot = b.CreateGEP(ptrTy(), vptr, {b.getInt32(2)});
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


// §28.13 / stdlib §2.16 (#603): void(void*) thunk that calls
// bz_shared_release(ptr), used as the per-element disposal callback for an
// OwnedArray<Shared<T>> (or similar owning collection of shared handles).
// Mirrors elementDisposerThunk but calls the runtime release rather than
// dispatching through a class vtable (Shared<T> has no vtable).
llvm::Function* Emitter::sharedReleaseThunk() {
    if (sharedReleaseThunk_) return sharedReleaseThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$sharedRelease", module_.get());
    llvm::Argument* h = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* relBB  = llvm::BasicBlock::Create(llvm_, "h.rel",  fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "h.ret",  fn);
    llvm::IRBuilder<> b(entry);
    b.CreateCondBr(
        b.CreateICmpEQ(h, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, relBB);
    b.SetInsertPoint(relBB);
    auto* relFn = declareC("bz_shared_release",
                           llvm::Type::getVoidTy(llvm_), {ptrTy()});
    b.CreateCall(relFn, {h});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    sharedReleaseThunk_ = fn;
    return fn;
}

// §28.13 / stdlib §2.16 (#603): void(void*) thunk that calls
// bz_weak_release(ptr), used as the per-element disposal callback for an
// OwnedArray<Weak<T>>.
llvm::Function* Emitter::weakReleaseThunk() {
    if (weakReleaseThunk_) return weakReleaseThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$weakRelease", module_.get());
    llvm::Argument* h = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* relBB  = llvm::BasicBlock::Create(llvm_, "h.rel",  fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "h.ret",  fn);
    llvm::IRBuilder<> b(entry);
    b.CreateCondBr(
        b.CreateICmpEQ(h, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, relBB);
    b.SetInsertPoint(relBB);
    auto* relFn = declareC("bz_weak_release",
                           llvm::Type::getVoidTy(llvm_), {ptrTy()});
    b.CreateCall(relFn, {h});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    weakReleaseThunk_ = fn;
    return fn;
}

// stdlib §5 (#651): void(void*) thunk that calls bz_grouping_free(ptr),
// used as the per-element disposal callback for an Array<IGrouping<TKey,T>>.
// Null-checks the pointer first (same pattern as sharedReleaseThunk).
llvm::Function* Emitter::groupingFreeThunk() {
    if (groupingFreeThunk_) return groupingFreeThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$groupingFree", module_.get());
    llvm::Argument* h = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* freeBB = llvm::BasicBlock::Create(llvm_, "g.free", fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "g.ret",  fn);
    llvm::IRBuilder<> b(entry);
    b.CreateCondBr(
        b.CreateICmpEQ(h, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, freeBB);
    b.SetInsertPoint(freeBB);
    auto* freeFn = declareC("bz_grouping_free",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
    b.CreateCall(freeFn, {h});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    groupingFreeThunk_ = fn;
    return fn;
}

bool Emitter::collectionElemIsClass(const sema::TypeRef* collTy, unsigned idx) {
    // fix #619 / §28.11 v0.135 / §14.4: the §28.11 table's owned/borrow column
    // applies to "class / interface type" — both class AND interface elements are
    // class-like from the collection's perspective (vtable-dispatch, pointer slot).
    // Widening to Class|Interface fixes the Class-only check that caused
    // OwnedDictionary<K,IFoo> indexer-set and move-out to skip disposal for
    // interface-valued slots.
    if (!collTy || collTy->genericArgs.size() <= idx) return false;
    const sema::TypeRef* el = collTy->genericArgs[idx];
    if (!el || el->kind != sema::TypeRef::Kind::Named || !el->declaration)
        return false;
    return el->declaration->kind == ast::TypeDeclKind::Class ||
           el->declaration->kind == ast::TypeDeclKind::Interface;
}


// #602 batch 3: owningCollectionElemDisposer deleted — all call sites now use
// elementPolicyFor(el, owningFamily).disposerThunk directly.
// #602 batch 3: owningDictValueDisposer deleted — all call sites now use
// elementPolicyFor(val, dictIsOwning).disposerThunk directly.
// #602 batch 3: isStringElementSet deleted — all call sites now use
// elementPolicyFor(el, false) + string primitive check directly.
// #602 batch 3: isStringValueDict deleted — all call sites now use
// elementPolicyFor(val, false) + string primitive check directly.
// #602 batch 3: stringElementReleaseDisposer deleted — all disposal call sites
// now use elementPolicyFor directly.
// #602 batch 4: isStringElementArray deleted — its only use was inside
// emitArrayRetainStringsIfStringArray; that helper now calls elementPolicyFor
// directly (the logic is inlined in LlvmBackend_impl.hpp).

llvm::Function* Emitter::stringElementReleaseThunk() {
    // #429 / §4.6 / §28.9: a standalone `void(void*)` thunk that releases one
    // co-owned string element (the slot holds a bz_string*). bz_string_release is
    // null-safe and a no-op on a sentinel/literal, so a null/never-stored slot is
    // safe to pass. Built once with its own IRBuilder so the active `builder_`
    // insert point is untouched (mirrors elementDisposerThunk).
    if (stringElementReleaseThunk_) return stringElementReleaseThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$strElemRelease", module_.get());
    llvm::Argument* s = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    llvm::IRBuilder<> b(entry);
    auto* relFn = declareC("bz_string_release",
                           llvm::Type::getVoidTy(llvm_), {ptrTy()});
    b.CreateCall(relFn, {s});   // null-safe / sentinel-safe in the runtime
    b.CreateRetVoid();
    stringElementReleaseThunk_ = fn;
    return fn;
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
    // a RECORD or a NULLABLE VALUE TYPE stores a heap box per value (the encode in
    // LlvmBackend_{Calls,Assign}.cpp boxes exactly `(valRec || valNullableVal) &&
    // isStructTy()`); its disposal must reclaim each surviving box. An
    // OwnedDictionary<K, class> routes through owningDictValueDisposer instead
    // (#394: dispose+free a class object), and a borrowing Dictionary with a
    // scalar/string value stores no box (the value is inttoptr'd or is the pointer
    // itself) — both return null here, so the value box is never freed twice.
    // The record check mirrors the encode's broad test (any record declaration,
    // equality-bearing or not), NOT the narrower recordTypeOfRef, so it matches
    // every boxed value exactly.
    // issue #539 / §7: nullable value types ({i8,T} tagged structs) are also
    // heap-boxed by encodeValue; their boxes must be freed at Clear/scope-end too.
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
    bool valIsNullableVal = valTy && isNullableValueTy(valTy);
    if (!valIsRecord && !valIsNullableVal) return nullptr;
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

    // §29.3 / §30.7 / #442: detect iterator and async-iterator methods BEFORE
    // creating the entry block (emitIteratorMethod / emitAsyncIteratorMethod
    // handle their own entry block + bindParameters).
    //
    // Routing rules (applied in order):
    //   1. async + yield in body  → emitAsyncIteratorMethod (fused).
    //   2. async + return type is IAsyncEnumerable<T>/IAsyncEnumerator<T>
    //      (even with NO yield — empty or await-only body) →
    //      emitAsyncIteratorMethod. Fix #442: without this check a body with
    //      no yield is misrouted to emitAsyncMethod, which emits a bz_task*
    //      result; an `await for` that loads a vtable from the task pointer
    //      then crashes (SIGSEGV).
    //   3. non-async + yield in body  → emitIteratorMethod (sync).
    //   4. async (non-iterator return type, no rule above matched) →
    //      emitAsyncMethod.
    if (ci.bodyKind == ast::MethodBodyKind::Block && ci.stmts &&
        isAsyncMethod(ms) &&
        (containsYield(*ci.stmts) || returnsAsyncIteratorType(ms))) {
        emitAsyncIteratorMethod(ms, fn, ci);
        return;
    }
    if (ci.bodyKind == ast::MethodBodyKind::Block && ci.stmts &&
        !isAsyncMethod(ms) &&
        containsYield(*ci.stmts)) {
        emitIteratorMethod(ms, fn, ci);
        return;
    }

    // §30.1 / §30.2: detect (non-yield, non-async-iterator) async methods.
    // emitAsyncMethod builds the entry + Resume functions itself (mirrors
    // emitIteratorMethod). Gated to the Task / Task<T> / OwnedTask<T> forms;
    // the async-iterator forms (async + yield, or async + IAsyncEnumerable
    // return type) are handled by the iterator path above.
    if (ci.bodyKind == ast::MethodBodyKind::Block && ci.stmts &&
        isAsyncMethod(ms)) {
        emitAsyncMethod(ms, fn, ci);
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
    pendingArgsDisposal_.reset();
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
    // #429 / §27.5: which captured locals/params have ALL-non-escaping lambdas
    // and may have their boxed cells freed at outer-scope exit.
    cellFreeEligible_.clear();
    cellFreeEligibleFn_ = fn;
    if (ci.bodyKind == ast::MethodBodyKind::Block && ci.stmts) {
        collectUnwindIneligible(*ci.stmts);
        collectStringReleaseEligible(*ci.stmts);
        collectClosureReleaseEligible(*ci.stmts);
        collectBoxReleaseEligible(*ci.stmts);
        // Must run AFTER collectClosureReleaseEligible (uses the result).
        collectCellFreeEligible(*ci.stmts);
    }

    // fix #633 / §33.4: Main(args: Array<string>) has C ABI signature
    // `i32(i32 argc, ptr argv)` — NOT `i32(ptr args)` (the Bezel type).
    // The OS passes argc as an i32 in the first integer register and argv as
    // a ptr in the second; if the function is declared `i32(ptr)`, the CPU
    // puts argc (e.g. 1) in the pointer register and args dereferences it →
    // SIGSEGV.  The fix: declare main with (i32, ptr) and convert to
    // Array<string> in the prolog via bz_args_array (runtime helper).
    // §33.4 rule: "The program's own executable path is not included;
    // args[0] is the first user-supplied argument."  bz_args_array skips
    // argv[0] to enforce exactly that.
    if (isMain(ms) && !ms->params.empty()) {
        // The LLVM function has exactly two args: i32 argc, ptr argv.
        auto argIt = fn->args().begin();
        llvm::Value* argc = &*argIt++;   // i32
        llvm::Value* argv = &*argIt;     // ptr (char**)
        // Call bz_args_array(int32_t argc, char** argv) → void* (bz_array_t*).
        auto* bz_args_fn = declareC("bz_args_array", ptrTy(), {i32(), ptrTy()});
        llvm::AllocaInst* argsSlot = entryAlloca(ptrTy());
        llvm::Value* argsArr = builder_.CreateCall(bz_args_fn, {argc, argv},
                                                    "main.args");
        builder_.CreateStore(argsArr, argsSlot);
        // Wire the param symbol → slot so body reads (IdentifierExpr of args)
        // resolve correctly via paramSlots_.
        paramSlots_[ms->params[0]] = argsSlot;
        // Register the array for collection disposal at scope end (both normal
        // exit and unwind).  No scope is open yet — defer to lowerScopedBlock
        // via pendingArgsDisposal_, mirroring pendingOwnedParams_.
        // bz_array_free(arr, bz_string_release) disposes each string element
        // (rc-decremented to 0 → freed) and then frees the array header.
        pendingArgsDisposal_ = Disposal{argsSlot, nullptr};
        pendingArgsDisposal_->collection = true;
        pendingArgsDisposal_->collectionElemDisposer = stringElementReleaseThunk();
    } else {
        bindParameters(fn, /*hasThis=*/!isStaticMethod(ms), &ms->params);
    }

    // §28.2 v0.134 / issue #601: checked disposal mode guard.
    // For every instance method of a class (not static, not the entry point,
    // not constructors — those go through emitConstructorBody — not the
    // compiler-generated Dispose — that goes through emitDisposerBody): emit
    // a disposed-flag test at method entry. If the flag is set, raise
    // DisposedAccessException and unwind. This is a no-op when checkedDisposal_
    // is false (unchecked mode pays zero cost — the flag test is absent).
    if (checkedDisposal_ && !isStaticMethod(ms) && !isMain(ms) &&
        ms->parent && ms->parent->kind == ast::TypeDeclKind::Class) {
        llvm::StructType* st = classStruct(ms->parent);
        unsigned fi = disposedFlagIndex(ms->parent);
        llvm::Value* flagAddr =
            builder_.CreateStructGEP(st, currentThis_, fi, "cd.flag");
        llvm::Value* disposed = builder_.CreateICmpNE(
            builder_.CreateLoad(disposedFlagTy(), flagAddr),
            llvm::ConstantInt::get(disposedFlagTy(), 0), "cd.disposed");
        emitGuardThrow(disposed, "DisposedAccessException");
    }

    // CG47: run every static constructor once, before the program's
    // body. The entry point Main is the single root, so calling them at
    // its start runs them before any user code (§22.7).
    // fix #529 / §22.7 (part b): after each static-ctor call, check bz_exc.
    // If the ctor threw, the ctor's own propagate block has already wrapped the
    // original exception in TypeInitializationException (see
    // emitStaticConstructorBody). emitPendingCheck then routes the wrapped
    // exception through Main's normal unwind path — if Main's body has an
    // enclosing try/catch (e.g. in a test), the TypeInitializationException
    // reaches the catch arm; otherwise it propagates to Main's propagateBB
    // (bz_unhandled_exception).
    if (isMain(ms)) {
        // §2.15 / §9.8: register the SerializationException vtable with the
        // runtime so bz_ser_throw (called by JSON runtime helpers) produces a
        // well-formed exception that emitIsA's catch dispatch can identify.
        // Must run before any user code (incl. static ctors) can call serializer
        // APIs.  The vtable pointer is always defined here (setupBuiltinExceptions
        // runs in emitModule before any method lowering).
        {
            auto vit = builtinVtable_.find("SerializationException");
            if (vit != builtinVtable_.end()) {
                auto* initFn = declareC("bz_ser_exc_vtable_init",
                                        llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(initFn, {vit->second});
            }
        }
        for (auto& e : staticCtors_) {
            builder_.CreateCall(e.fn, {});
            emitPendingCheck();
        }
        // §24.1 / §22.13 (#477 B6): allocate each static event's backing
        // EventSource<T> at program start (the single static root), so a
        // subscribe/raise on a static event reaches a real shared source.
        // Mirrors the per-instance allocation done during construction
        // (emitInstanceInit), but at the program-wide static-init point.
        // bz_static_event_source_new also registers the source for program-exit
        // disposal (§22.13) via atexit — a static owned field is disposed at
        // program exit, not by any instance disposer.
        if (!staticEventGlobals_.empty()) {
            auto* newFn =
                declareC("bz_static_event_source_new", ptrTy(), {});
            for (auto& [es, gv] : staticEventGlobals_) {
                llvm::Value* esrc = builder_.CreateCall(newFn, {});
                builder_.CreateStore(esrc, gv);
            }
        }
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
        // CG40 / §17.12: an expression body whose whole expression diverges
        // (`=> throw E`, or any expr that branches to the unwind/propagate
        // target) has already terminated the current block — lowerExpr returns
        // a non-null poison placeholder and sets diverged_. Emitting a return
        // into the already-terminated block would give it two terminators
        // (malformed IR that crashes the optimizer). Mirror the ReturnStmt path
        // (LlvmBackend_Stmts.cpp): if diverged_, the block is already correctly
        // ended — skip emitReturn (and boxForTarget, which must not add
        // instructions to a terminated block).
        if (diverged_) {
            terminated = true;
        } else {
            // §8.6: box a value-typed `=> expr` result returned through an
            // Object/interface return type (so the caller's is/as/dispatch works).
            if (v && currentReturnTypeNode_)
                v = boxForTarget(v, ctx_.resolvedType(*currentReturnTypeNode_),
                                 ci.exprBody);
            // #694 residue-1 / §4.6 / §28.9 / §28.3: an arrow-body (`=> expr`)
            // string-returning method must mint EXACTLY +1 just like the explicit
            // ReturnStmt path (LlvmBackend_Stmts.cpp line ~750). Without this,
            // a `=> fieldRef` or `=> param` body returns a raw borrow (rc stays 0
            // above what the caller expects), so the caller's release under-counts.
            // Mirror the ReturnStmt logic exactly: TRANSFER if `ci.exprBody` is
            // already a +1 producer or a returned-local-owns-plus-one; else RETAIN.
            if (v && currentReturnTypeNode_ && v->getType()->isPointerTy()) {
                const sema::TypeRef* declRet =
                    ctx_.resolvedType(*currentReturnTypeNode_);
                if (declRet && !declRet->nullable &&
                    declRet->kind == sema::TypeRef::Kind::Primitive &&
                    declRet->primitive == sema::PrimitiveType::String &&
                    !isPlusOneStringProducer(ci.exprBody) &&
                    !returnedStringLocalOwnsPlusOne(ci.exprBody))
                    emitStringRetain(v);
            }
            terminated = emitReturn(fn, v);
        }
    } else if (ci.bodyKind == ast::MethodBodyKind::Block) {
        terminated = lowerScopedBlock(*ci.stmts);
    }
    if (!terminated) emitImplicitReturn(fn);
    currentEnumMethod_ = savedEnumMethod;
}


void Emitter::emitStaticConstructorBody(sema::ConstructorSymbol* cs,
                               const sema::TypeSymbol* type,
                               llvm::Function* fn) {
    // cs may be null (synthesized field-init-only function); in that case
    // there is no user-written ConstructorDecl body to lower.
    const ast::ConstructorDecl* cd = nullptr;
    if (cs) {
        cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
        if (!cd) return;  // malformed AST — nothing to emit
    }

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

    // fix #529 / §22.7 (part a): establish the entry block FIRST so that
    // builder_ has a valid insert point, then pre-seed propagateBB_ with a
    // custom block that wraps the escaping exception in TypeInitializationException
    // before returning to the caller.
    //
    // Any uncaught exception inside the static-ctor body eventually branches to
    // propagateBB().  The lazy propagateBB() would produce a plain `ret void`
    // for a non-main void function, leaving bz_exc pointing at the ORIGINAL
    // exception.  By pre-seeding propagateBB_ we intercept every uncaught-
    // exception exit and:
    //   1. Load bz_exc (the original exception that escaped the ctor).
    //   2. Allocate a TypeInitializationException (24 bytes):
    //        [offset  0] vtable ptr    — identifies the type at runtime
    //        [offset  8] message ptr   — null (calloc zeroes it; §31.4)
    //        [offset 16] InnerException — the original exception (§22.7)
    //   3. Store the wrapped exception back into bz_exc.
    //   4. Return void — the caller's emitPendingCheck (in Main's prologue,
    //      fix #529 part b) observes the non-null bz_exc and routes the
    //      TypeInitializationException through Main's normal unwind path,
    //      reaching a catch arm if one is in scope.
    //
    // An exception that IS caught inside the static-ctor body (via try/catch)
    // never reaches propagateBB_ — lowerTry handles it before the propagate
    // branch; the ctor then returns normally (ret void with bz_exc == null).
    //
    // NOTE: The TypeInitializationException disposer is the shared no-op
    // bz$exDispose, which does not recurse into InnerException; the inner object
    // is therefore not freed at program exit when the exception is not caught.
    // This is an acceptable simplification for the process-exit path.  A full fix
    // would add a typed disposer for TypeInitializationException; tracked as a
    // follow-on to issue #529.
    auto* entryBB = llvm::BasicBlock::Create(llvm_, "entry", fn);
    builder_.SetInsertPoint(entryBB);

    // Pre-seed propagateBB_ before lowering the body so that every throw inside
    // the body (direct throw or emitPendingCheck on a nested call) reaches the
    // wrapping block.
    propagateBB_ = llvm::BasicBlock::Create(llvm_, "sinit.propagate", fn);
    {
        // Temporarily switch to the propagate block to emit its body.
        auto* savedInsert = builder_.GetInsertBlock();
        builder_.SetInsertPoint(propagateBB_);
        llvm::Value* origExc = builder_.CreateLoad(ptrTy(), excGlobal());
        auto vit = builtinVtable_.find("TypeInitializationException");
        if (vit != builtinVtable_.end()) {
            llvm::Value* tie = builder_.CreateCall(
                callocFn(), {builder_.getInt64(1), builder_.getInt64(24)});
            // vtable at offset 0
            builder_.CreateStore(vit->second, tie);
            // message at offset 8: null (calloc zeroed it)
            // InnerException at offset 16
            auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
            llvm::Value* innerSlot =
                builder_.CreateGEP(i8Ty, tie, builder_.getInt64(16));
            builder_.CreateStore(origExc, innerSlot);
            builder_.CreateStore(tie, excGlobal());
        }
        builder_.CreateRetVoid();
        builder_.SetInsertPoint(savedInsert);
    }

    // fix #582 / §22.13: emit runtime stores for every non-constant static
    // field initializer of `type`, in textual declaration order.  This runs
    // BEFORE the user-written constructor body (if any), satisfying §22.13:
    // "Within a single type, static fields are initialized in textual
    // declaration order" and "the static constructor … runs after all static
    // field initializers complete."
    //
    // For a synthesized sfinit function (cs == null) this is the entire
    // body.  For a user-written cctor (cs != null) this is the preamble
    // before lowerScopedBlock(cd->body).
    //
    // Only non-constant initializers are emitted here; constant initializers
    // (int/bool/float/string literals) are already encoded as the global's
    // LLVM initializer by constInitFor, so they need no runtime store.
    if (type) {
        for (const sema::Symbol* m : type->members) {
            auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
            if (!fs || !isStaticField(fs)) continue;
            auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
            if (!fd || !fd->initializer) continue;
            const ast::Expr* initExpr = fd->initializer.get();
            // Skip initializers that constInitFor already handles as a
            // compile-time constant — no runtime store needed for those.
            if (dynamic_cast<const ast::IntLiteralExpr*>(initExpr))     continue;
            if (dynamic_cast<const ast::BoolLiteralExpr*>(initExpr))    continue;
            if (dynamic_cast<const ast::FloatLiteralExpr*>(initExpr))   continue;
            if (dynamic_cast<const ast::StringLiteralExpr*>(initExpr))  continue;
            if (dynamic_cast<const ast::VerbatimStringExpr*>(initExpr)) continue;
            // Non-constant initializer: evaluate it and store into the global.
            auto gvIt = staticGlobals_.find(fs);
            if (gvIt == staticGlobals_.end()) continue;  // no backing global
            llvm::Value* val = lowerExpr(*initExpr);
            if (!val) continue;
            builder_.CreateStore(val, gvIt->second);
        }
    }

    // Lower the user-written static constructor body (cs != null).
    if (cd) {
        if (!lowerScopedBlock(cd->body)) builder_.CreateRetVoid();
    } else {
        builder_.CreateRetVoid();
    }
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
    // before running the rest of this constructor's body. §22.9 step-1: an
    // exception during the base/chained ctor "leaves no derived state to clean
    // up — the base ctor's own exception-safety logic disposes ... the base
    // subobject." This emitPendingCheck routes such a throw to the BARE
    // propagate block (no derived field/base disposal); the instance block is
    // reclaimed by the `new` site (emitNewPendingCheck). It is intentionally
    // emitted BEFORE installing the body's cleanup propagate (below), so the
    // base-call throw does not run the derived cleanup.
    emitPendingCheck();

    // §22.9 step-3 (FINDING [4]): install a constructor-cleanup block as the
    // propagate target for the BODY. An exception in the ctor body (step 3) must
    // dispose "every owned field assigned during initialization (step 2) and the
    // base subobject" — and owned fields assigned imperatively in the body —
    // before the exception leaves the ctor. emitOwnedFieldDisposals disposes
    // exactly the assigned subset: each owned field is null-guarded, and an
    // unassigned field is calloc-zeroed (null) so its disposal is a no-op; an
    // assigned field is non-null and is disposed (reverse declaration order) and
    // its slot nulled, so it cannot be disposed twice. emitBaseDisposeChain then
    // disposes the (fully-constructed, since the base ctor completed) base
    // subobject. `this` is NOT freed here — only the `new` site frees the block
    // (emitNewPendingCheck), exactly once, avoiding any double-free with an
    // enclosing base-ctor frame in an inheritance chain. This cleanup fires only
    // on the throw edge; normal completion falls through to the RetVoid below and
    // the instance is disposed later by its owning slot (no double-free).
    //
    // Pre-seeding propagateBB_ makes every body exception edge (a direct throw, a
    // call that throws, or a try with no matching catch re-propagating) route
    // here: lowerTry's outerUnwind and emitThrow/emitPendingCheck all fall back
    // to propagateBB() when no enclosing handler is set.
    bool hasOwnedToClean = false;
    for (const sema::Symbol* m : cs->parent->members) {
        // FieldSymbol-based owned fields (class references, collections, etc.)
        auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
        if (fs && (isOwnedReferenceField(fs) ||
                   isOwnedBuiltinExceptionField(fs) ||
                   ownedCollectionFieldType(fs) ||
                   // §28.13 (#620): owned Shared<T>/Weak<T> field also needs
                   // cleanup if the constructor body throws.
                   isOwnedSharedWeakField(fs))) {
            hasOwnedToClean = true;
            break;
        }
        // #480 / §24.1 / §28: EventSymbol backing fields are also owned — each
        // instance event's EventSource is allocated by emitInstanceInit BEFORE the
        // ctor body runs.  If the ctor body throws the source must be freed by the
        // cleanup block.  Including EventSymbol here ensures the cleanup block is
        // emitted even when the class has only events and no other owned fields.
        if (auto* es = dynamic_cast<const sema::EventSymbol*>(m)) {
            if (auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl)) {
                if (!ed->isStatic && !ed->isAsync &&
                    fieldIndex_.find(es) != fieldIndex_.end()) {
                    hasOwnedToClean = true;
                    break;
                }
            }
        }
    }
    if (hasOwnedToClean || baseClassOf(cs->parent)) {
        auto* savedInsert = builder_.GetInsertBlock();
        auto* cleanupBB =
            llvm::BasicBlock::Create(llvm_, "ctor.cleanup", currentFn_);
        builder_.SetInsertPoint(cleanupBB);
        emitOwnedFieldDisposals(cs->parent);   // assigned owned fields (reverse)
        emitBaseDisposeChain(cs->parent);      // the base subobject
        builder_.CreateRetVoid();              // exc stays set; `new` site frees
        if (savedInsert) builder_.SetInsertPoint(savedInsert);
        // Route the body's exception edges to the cleanup block. The bare
        // propagate (used by the base-call check above) keeps its own block.
        propagateBB_ = cleanupBB;
    }

    bool terminated = lowerScopedBlock(cd->body, startIdx);
    if (!terminated) builder_.CreateRetVoid();
}


// §22.8 step 1+2 (issue #328 item 2+3): the shared instance-initialization
// sequence, factored out of lowerNew so it runs identically at the in-module
// `new` site and inside the cross-module `bz$<Type>$init` function. Installs the
// most-derived vtable into slot 0, then runs the full-hierarchy field
// initializers (base-first, own last; calloc already zeroed the rest, so an
// uninitialized field is left zero), the collection-literal initializers, and
// the §24.1 event-source initializers. NOTE: this is the in-module form — it
// reads each field's AST initializer via `m->decl`, so it is NEVER called for an
// imported type (whose symbols carry no decl); the importer instead calls the
// linked init function (declareImportedInit). The old constant-foldable replay
// is therefore gone: a foldable OR non-foldable initializer now crosses the
// boundary by running the exporter's real initializer here.
void Emitter::emitInstanceInit(const sema::TypeSymbol* t, llvm::Value* obj,
                               llvm::StructType* st) {
    // CG17: install the vtable pointer (instance slot 0) before field
    // initializers / the constructor, so virtual dispatch on the new object
    // resolves to its own class.
    //
    // §28.5: for a generic instantiation whose class has an `owned` type-
    // parameter field, install the PER-INSTANTIATION vtable instead of the
    // shared `vt$<T>` (its slot-1 Dispose disposes that field by the concrete
    // substituted type, §28.4 step 2 / §28.5). specializedVtableFor falls back
    // to the shared vtable when no specialization is needed. genericSubst_ is
    // active here when called from lowerNew for a generic instantiation.
    //
    // §14.1 / §2.5 (#662 foundation A): for an IMPORTED generic class the
    // exporter emits no shared vt$<T>, so specializedVtableFor returns null
    // (vtableOf finds nothing). Call buildImportedGenericVtable instead, which
    // builds the per-instantiation vtable from scratch (descriptor + itmap +
    // specialized disposer + virtual slots). The result is cached in
    // specializedVtables_ so repeated `new Box<int>()` sites share one vtable.
    if (ctx_.isImported(t) && !t->genericParams.empty() &&
        !genericSubst_.empty()) {
        if (llvm::GlobalVariable* vt = buildImportedGenericVtable(t))
            builder_.CreateStore(vt, builder_.CreateStructGEP(st, obj, 0));
        // Imported generic fields have no AST initializers — calloc already
        // zeroed them.  No field-init loop is needed.
        return;
    }
    if (llvm::GlobalVariable* vt = specializedVtableFor(t)) {
        builder_.CreateStore(vt, builder_.CreateStructGEP(st, obj, 0));
    }

    // Run field initializers, inherited (base-first) then own, so a
    // `new Derived()` initializes base fields too. CG16's base-ctor chaining
    // runs ctor *bodies*, not these initializers, so there is no double-init.
    std::vector<const sema::Symbol*> storage;
    collectInstanceStorage(t, storage);
    for (const sema::Symbol* m : storage) {
        const ast::Expr* init = nullptr;
        if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m->decl))
            init = fd->initializer.get();
        else if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m->decl))
            init = pd->autoInitializer.get();  // CG33: `property X := v;`
        if (!init) continue;  // no initializer → calloc-zeroed
        auto idxIt = fieldIndex_.find(m);
        if (idxIt == fieldIndex_.end()) continue;
        // §15.1 / §16: collection literal `[e1, e2, …]` initializer.
        // Build the array in-place via GEP into the field slot.
        if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(init);
            cl && !cl->isDict) {
            // #465 / §15.4: a HEAP-collection field (`Array<T>` etc.) lowers to a
            // bare `ptr` slot, not an inline `[N x elem]` ArrayType — the flat GEP
            // below would be malformed (or, when the dyn_cast misses, the field is
            // silently left null → crash on first use). Materialize a real heap
            // collection (same bz_array_alloc + bz_array_add as `new`/`.Add`) and
            // store the pointer into the field slot.
            const sema::TypeRef* fieldRt = nullptr;
            if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m->decl))
                if (fd->type) fieldRt = ctx_.resolvedType(*fd->type);
            if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m->decl))
                if (pd->type) fieldRt = ctx_.resolvedType(*pd->type);
            if (fieldRt) {
                if (llvm::Value* arr =
                        materializeHeapCollectionLiteral(cl, fieldRt)) {
                    builder_.CreateStore(
                        arr, builder_.CreateStructGEP(st, obj, idxIt->second));
                    continue;
                }
                // #559: if the field is a hash-backed Set/OwnedSet and
                // materializeHeapSetLiteral returned null (unrecognised element
                // type), do NOT fall through to the inline-array GEP path below —
                // that path treats the Set `ptr` field as a struct → malformed IR.
                if (isCollectionTypeName(fieldRt)) {
                    std::string_view cn =
                        fieldRt->astNode && fieldRt->astNode->name &&
                        !fieldRt->astNode->name->parts.empty()
                            ? fieldRt->astNode->name->parts[0]
                            : std::string_view{};
                    if (cn == "Set" || cn == "OwnedSet")
                        continue;  // safe no-op; sema already diagnosed the type
                }
            }
            if (cl->elements.empty()) continue;
            llvm::Value* fieldGep =
                builder_.CreateStructGEP(st, obj, idxIt->second);
            llvm::Type* fieldTy = st->getElementType(idxIt->second);
            if (auto* aty = llvm::dyn_cast<llvm::ArrayType>(fieldTy)) {
                llvm::Type* elemTy = aty->getElementType();
                for (std::size_t i = 0; i < cl->elements.size(); ++i) {
                    if (!cl->elements[i]) continue;
                    llvm::Value* ev = lowerExpr(*cl->elements[i]);
                    if (!ev) continue;
                    if (ev->getType() != elemTy &&
                        ev->getType()->isIntegerTy() &&
                        elemTy->isIntegerTy()) {
                        unsigned eb = ev->getType()->getIntegerBitWidth();
                        unsigned db = elemTy->getIntegerBitWidth();
                        ev = db > eb ? builder_.CreateSExt(ev, elemTy)
                                     : builder_.CreateTrunc(ev, elemTy);
                    }
                    builder_.CreateStore(
                        ev, builder_.CreateGEP(fieldTy, fieldGep,
                            {builder_.getInt32(0), builder_.getInt32(i)}));
                }
            }
            continue;
        }
        // #538 / stdlib §4.8: dictionary literal `["k1":v1, "k2":v2, …]` as a
        // field initializer.  Same root cause as #534 (local-decl context): the
        // old `!cl->isDict` guard caused dict literals to fall through to
        // lowerExpr, which returns null for CollectionLiteralExpr → field left
        // null → crash on first dict access.  Route through the same
        // materializeHeapDictLiteral helper that #534 wired for local-decls.
        if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(init);
            cl && cl->isDict && !cl->keys.empty()) {
            const sema::TypeRef* fieldRt = nullptr;
            if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m->decl))
                if (fd->type) fieldRt = ctx_.resolvedType(*fd->type);
            if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m->decl))
                if (pd->type) fieldRt = ctx_.resolvedType(*pd->type);
            if (fieldRt) {
                if (llvm::Value* tbl = materializeHeapDictLiteral(cl, fieldRt)) {
                    builder_.CreateStore(
                        tbl, builder_.CreateStructGEP(st, obj, idxIt->second));
                    continue;
                }
            }
        }
        llvm::Value* v = lowerExpr(*init);
        if (!v) continue;
        // §5.1: an int initializer for a wider numeric field is implicitly
        // widened — convert before the store so the field holds the value, not
        // the int's raw bits (issue #408 / #410).
        v = coerceNumericToType(v, st->getElementType(idxIt->second),
                                isUnsignedTargetExpr(*init));
        // #429 / §4.6 / §28.9: a CLASS string field / auto-property initializer
        // gives the field its own reference, like a later assignment. The field
        // is calloc-zeroed (null) before this, so there is no old value to
        // release. A fresh +1 producer is TRANSFERRED (no retain); anything else
        // (a literal sentinel — a no-op retain; a borrow / call) is RETAINED so
        // the field's count is independent. The disposal release
        // (emitOwnedFieldDisposals) balances it either way (no UAF, no leak).
        // emitInstanceInit runs only for CLASS construction (records use the
        // by-value path in lowerNew, whose string fields are borrows, §9).
        if (v->getType()->isPointerTy() && isStringStorageMember(m) &&
            !isPlusOneStringProducer(init))
            emitStringRetain(v);
        // #429 (closure FIELD-STORE category) / §27.5 / §28.9: a CLASS closure
        // field / auto-property initializer gives the field its own reference,
        // like a later assignment. The field is calloc-zeroed (null) before
        // this, so there is no old value to release. A fresh +1 producer
        // (lambda / method-ref / user closure-returning call) is TRANSFERRED
        // (no retain); anything else is RETAINED so the field's count is
        // independent of the source's. The disposal release
        // (emitOwnedFieldDisposals) balances it either way (no UAF, no leak).
        // emitInstanceInit runs only for CLASS construction (records use the
        // by-value path in lowerNew, whose function fields are borrows, §9).
        if (v->getType()->isPointerTy() && isClosureStorageMember(m)) {
            bool isProducer = isPlusOneClosureProducer(init) ||
                              isUserMethodClosureReturnCall(init);
            if (!isProducer)
                emitClosureRetain(v);
        }
        // #429 (box FIELD-STORE category) / §8.6 / §28.9: a CLASS Object/
        // interface field / auto-property initializer that stores a boxable
        // value gives the field its own reference, like a later assignment.
        // The field is calloc-zeroed (null) before this, so there is no old
        // value to release (no bz_box_release needed here). A fresh +1 producer
        // (a boxable value source: boxForTarget creates a fresh rc=1 box, or a
        // box-return method call whose callee's boxForTarget produced the box)
        // is TRANSFERRED (no extra retain — its single reference flows into the
        // field). Anything else (a borrow — a local / param / field that holds
        // a box but whose +1 belongs to its own slot) is RETAINED via
        // bz_box_retain so the field's count is independent. The disposal
        // release (emitDisposerCore box field loop) balances it. Gated on
        // isBoxStorageMember (populated by collectBoxStorageFields) so ONLY
        // fields confirmed to always hold boxes are touched — a class-reference
        // initializer for a field that sometimes holds class refs is excluded.
        // emitInstanceInit runs only for CLASS construction (records use the
        // by-value path in lowerNew, whose Object fields are borrows, §9).
        if (v->getType()->isPointerTy() && isBoxStorageMember(m)) {
            const sema::TypeRef* srcTy = ctx_.exprType(*init);
            bool isProducer =
                (srcTy && (isBoxableValueType(srcTy) || isNullableValueTy(srcTy))) ||
                isUserMethodBoxReturnCall(init);
            if (!isProducer)
                emitBoxRetain(v);
            // Record in boxStorageFields_ so disposal is armed. The field is
            // already in boxStorageFields_ from collectBoxStorageFields; this
            // re-insert is a no-op but makes it explicit.
            boxStorageFields_.insert(static_cast<const void*>(m));
        }
        builder_.CreateStore(
            v, builder_.CreateStructGEP(st, obj, idxIt->second));
    }

    // per §24.1: initialize each instance event's backing EventSource<T> slot
    // to a fresh, empty source during construction.
    for (const sema::Symbol* m : storage) {
        if (!dynamic_cast<const sema::EventSymbol*>(m)) continue;
        auto idxIt = fieldIndex_.find(m);
        if (idxIt == fieldIndex_.end()) continue;
        auto* newFn = declareC("bz_event_source_new", ptrTy(), {});
        llvm::Value* esrc = builder_.CreateCall(newFn, {});
        builder_.CreateStore(
            esrc, builder_.CreateStructGEP(st, obj, idxIt->second));
    }
}


// §2.5 / §2.6 / §22.8 (issue #328 item 2+3): emit the exported
// `bz$<Type>$init(ptr this)` for a public, non-generic class. Its body just runs
// emitInstanceInit on the incoming `this`, so a cross-module `new` of this type
// performs §22.8 step-1+2 (vtable install + field initializers, foldable or not)
// by linking to this function. Only public non-generic classes are emitted: a
// generic template's initialization is per-instantiation (not a stable linkable
// symbol) and a non-public type is not part of the importable construction
// surface, so neither needs (or gets) an exported init. The in-module `new` site
// keeps inlining emitInstanceInit directly, so this function is dead code within
// a single-module build (no double-init) — it exists only for cross-module
// callers.
void Emitter::emitInstanceInitFunction(const sema::TypeSymbol* ts) {
    auto it = initFns_.find(ts);
    if (it == initFns_.end()) return;  // not an exported init
    llvm::Function* fn = it->second;
    if (!fn->empty()) return;  // already emitted

    // Save/restore the per-function emitter state (mirrors emitDisposerBody).
    llvm::Function* savedFn = currentFn_;
    llvm::Value* savedThis = currentThis_;
    auto savedLocals = locals_;
    auto savedParams = paramSlots_;
    auto* savedInsert = builder_.GetInsertBlock();

    currentFn_ = fn;
    currentThis_ = &*fn->args().begin();
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    disposeStack_.clear();
    loopStack_.clear();
    finallyStack_.clear();
    propagateBB_ = nullptr;
    unwindTarget_ = nullptr;
    currentCaughtExc_ = nullptr;
    currentCaughtName_ = {};
    unwindScopeDepth_ = 0;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));

    emitInstanceInit(ts, currentThis_, classStruct(ts));
    builder_.CreateRetVoid();

    currentFn_ = savedFn;
    currentThis_ = savedThis;
    locals_ = std::move(savedLocals);
    paramSlots_ = std::move(savedParams);
    if (savedInsert) builder_.SetInsertPoint(savedInsert);
}


// §2.5 (issue #328 item 2+3): declare the external `bz$<Type>$init(ptr)` for an
// IMPORTED type so the importer's `new` calls the linked initializer. The
// signature is `void(ptr this)`, matching emitInstanceInitFunction's emitter.
// Cached in initFns_ so repeated `new`s of the same imported type share one
// declaration.
llvm::Function* Emitter::declareImportedInit(const sema::TypeSymbol* ts) {
    auto it = initFns_.find(ts);
    if (it != initFns_.end()) return it->second;
    auto* fnType = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                           {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnType, llvm::Function::ExternalLinkage,
                                      "bz$" + std::string(ts->name) + "$init",
                                      module_.get());
    initFns_[ts] = fn;
    return fn;
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
    if (!targetCls) {
        // #595 / §12 / §31: base() at the root — OR — the AST-declared base is a
        // built-in exception type (which has no TypeSymbol in baseTypes, so
        // baseClassOf returns null).  For a built-in exception base, store the
        // first constructor argument (the message string) to the inherited message-
        // ptr slot at struct slot 1 / byte offset 8 so `e.Message` returns it.
        // Only applies to a `base(...)` chain (isThis==false) on a class whose
        // direct AST base is a built-in exception.
        if (!isThis && !call->args.empty() && call->args[0].value &&
            hasBuiltinExceptionBase(cs->parent) && currentThis_) {
            llvm::Value* msgVal = lowerExpr(*call->args[0].value);
            if (msgVal) {
                // message_ptr is at byte offset 8 — store directly via byte GEP.
                auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                llvm::Value* msgSlot = builder_.CreateGEP(
                    i8Ty, currentThis_, builder_.getInt64(8));
                builder_.CreateStore(msgVal, msgSlot);
            }
        }
        return;
    }
    std::size_t n = call->args.size();
    sema::ConstructorSymbol* target =
        ctorByArity(targetCls, n, isThis ? cs : nullptr);
    if (!target) {
        // Explicit `base()` with no matching base ctor: the base's own
        // base may still need an implicit chain.
        if (!isThis && n == 0) emitImplicitBase(targetCls, currentThis_);
        return;
    }
    // §2.5 / §2.6 / §12 (issue #328 / #103): a LOCAL class's `base(...)` chain to
    // an IMPORTED base runs the dependency's `bz$<Base>$ctor`. The imported ctor
    // has no local ctorFns_ entry until declared, so declare it on demand (as
    // lowerNew does for `new ImportedT(...)`); otherwise the chain call is
    // silently dropped and the base subobject's fields stay calloc-zeroed.
    auto it = ctorFns_.find(target);
    if (it == ctorFns_.end()) {
        if (ctx_.isImported(targetCls)) {
            declareImportedCtor(target);
            it = ctorFns_.find(target);
        }
        if (it == ctorFns_.end()) return;
    }
    std::vector<llvm::Value*> args;
    args.push_back(currentThis_);
    for (const auto& a : call->args) {
        if (!a.value) return;
        llvm::Value* v = lowerExpr(*a.value);
        if (!v) return;
        args.push_back(v);
    }
    // §12 / §14 (issue #617): when the DERIVED class is non-generic but its
    // base is a CONCRETE generic instantiation (e.g. `class StringBox: Box<string>`
    // calling `base(s)`), genericSubst_ is empty and the template ctor
    // `bz$Box$ctor(ptr, i32)` would be called with a `ptr` argument — an ABI
    // mismatch that LLVM rejects.  Build the concrete substitution from the
    // base type's recorded TypeRef (TypeSymbol::baseTypeRefs, parallel to
    // baseTypes, which carries the generic args for each base) and activate it
    // so the inline-ctor path below uses the right field layout and param types.
    auto savedSubstForBase = genericSubst_;
    auto savedSemaTyForBase = genericSubstSemaTy_;
    bool restoredSubstForBase = false;
    if (genericSubst_.empty() && targetCls && !targetCls->genericParams.empty()) {
        const sema::TypeRef* baseRef = nullptr;
        for (std::size_t bi = 0; bi < cs->parent->baseTypes.size(); ++bi) {
            if (bi < cs->parent->baseTypeRefs.size() &&
                cs->parent->baseTypes[bi] == targetCls) {
                baseRef = cs->parent->baseTypeRefs[bi];
                break;
            }
        }
        if (baseRef && !baseRef->genericArgs.empty()) {
            auto* td = dynamic_cast<const ast::TypeDecl*>(targetCls->decl);
            if (td && td->genericParams.size() == baseRef->genericArgs.size()) {
                std::unordered_map<std::string_view, llvm::Type*> newSubst;
                std::unordered_map<std::string_view, const sema::TypeRef*> newSema;
                for (std::size_t gi = 0; gi < td->genericParams.size(); ++gi) {
                    std::string_view pname = td->genericParams[gi].name;
                    newSubst[pname] = lowerTypeRef(baseRef->genericArgs[gi]);
                    newSema[pname] = baseRef->genericArgs[gi];
                }
                genericSubst_ = std::move(newSubst);
                genericSubstSemaTy_ = std::move(newSema);
                restoredSubstForBase = true;
            }
        }
    }
    // §12 / §14 (issue #585): when a generic substitution is active, the target
    // ctor function in ctorFns_ is the T→i32 TEMPLATE — calling it would store
    // the chained ctor's fields against the unspecialized layout and would
    // mismatch a non-int `T` argument (a `ptr` string passed to an `i32`
    // param). Inline the target ctor body instead, with the current
    // substitution active, so its field stores use the concrete specialized
    // layout. The param `T` of the base/sibling generic class shares the name
    // bound in genericSubst_ (both `Container<T>` and `Wrapper<T>` name it `T`),
    // so the active substitution applies directly. The target's own
    // `base(...)`/implicit-base chain runs recursively inside inlineCtorBody.
    if (!genericSubst_.empty() && targetCls && !targetCls->genericParams.empty()) {
        auto* targetCd = dynamic_cast<const ast::ConstructorDecl*>(target->decl);
        if (targetCd && args.size() == target->params.size() + 1) {
            // Save/restore the inlining frame state, then bind the chained
            // ctor's params to the lowered argument values and inline its body
            // onto the SAME object subobject (currentThis_).
            llvm::Value* subObj = currentThis_;
            auto savedLocals = locals_;
            auto savedParams = paramSlots_;
            llvm::BasicBlock* savedUnwindTarget = unwindTarget_;
            std::size_t savedUnwindDepth = unwindScopeDepth_;
            locals_.clear();
            paramSlots_.clear();
            for (std::size_t pi = 0; pi < target->params.size(); ++pi) {
                auto* slot = entryAlloca(args[pi + 1]->getType());
                builder_.CreateStore(args[pi + 1], slot);
                paramSlots_[target->params[pi]] = slot;
            }
            inlineCtorBody(target, subObj);
            unwindTarget_ = savedUnwindTarget;
            unwindScopeDepth_ = savedUnwindDepth;
            currentThis_ = subObj;
            locals_ = std::move(savedLocals);
            paramSlots_ = std::move(savedParams);
            if (restoredSubstForBase) {
                genericSubst_ = std::move(savedSubstForBase);
                genericSubstSemaTy_ = std::move(savedSemaTyForBase);
            }
            return;
        }
    }
    if (restoredSubstForBase) {
        genericSubst_ = std::move(savedSubstForBase);
        genericSubstSemaTy_ = std::move(savedSemaTyForBase);
    }
    if (args.size() == it->second->arg_size()) {
        builder_.CreateCall(it->second, args);
    }
}


// §12 / §14 (issue #585): inline a constructor's body into the current function
// with the active generic substitution. `obj` is `this` (already the correct
// subobject), and the ctor's parameters must already be bound in paramSlots_.
// Runs the leading constructor chain (an explicit `base(...)`/`this(...)`, else
// the implicit base), then the remaining body statements. A constructor-cleanup
// block (assigned owned fields + base subobject + free, routed to the enclosing
// unwind) guards the inlined body's exception edges, mirroring lowerNew's
// inline path and emitConstructorBody.
void Emitter::inlineCtorBody(sema::ConstructorSymbol* cs, llvm::Value* obj) {
    auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
    if (!cd) return;
    currentThis_ = obj;

    // Build the cleanup block first, capturing the enclosing unwind target.
    auto* savedInsert = builder_.GetInsertBlock();
    llvm::BasicBlock* enclosingUnwind = unwindBB();
    auto* cleanupBB =
        llvm::BasicBlock::Create(llvm_, "ctor.inline.cleanup", currentFn_);
    builder_.SetInsertPoint(cleanupBB);
    emitOwnedFieldDisposals(cs->parent);
    emitBaseDisposeChain(cs->parent);
    builder_.CreateCall(freeFn(), {obj});
    builder_.CreateBr(enclosingUnwind);
    if (savedInsert) builder_.SetInsertPoint(savedInsert);
    unwindTarget_ = cleanupBB;
    unwindScopeDepth_ = disposeStack_.size();

    // Run the leading chain (base/this) — recursively inlining a generic base
    // ctor — then lower the rest of the body. With neither written, the
    // implicit base ctor runs (chaining further up).
    bool isThis = false;
    std::size_t startIdx = 0;
    if (!cd->body.empty()) {
        if (const ast::CallExpr* chain = chainCallOf(*cd->body[0], isThis)) {
            emitChainCall(chain, isThis, cs);
            startIdx = 1;
        } else {
            emitImplicitBase(cs->parent, currentThis_);
        }
    } else {
        emitImplicitBase(cs->parent, currentThis_);
    }
    lowerScopedBlock(cd->body, startIdx);
}


void Emitter::emitImplicitBase(const sema::TypeSymbol* cls, llvm::Value* obj) {
    const sema::TypeSymbol* b = baseClassOf(cls);
    if (!b) return;
    if (sema::ConstructorSymbol* target = ctorByArity(b, 0, nullptr)) {
        auto it = ctorFns_.find(target);
        // §2.5 / §12 (issue #328 / #103): an implicit base chain into an IMPORTED
        // base runs the dependency's `bz$<Base>$ctor`; declare it on demand so
        // the base subobject is constructed (mirrors emitChainCall).
        if (it == ctorFns_.end() && ctx_.isImported(b)) {
            declareImportedCtor(target);
            it = ctorFns_.find(target);
        }
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
        // #465 / §15.4: a collection literal whose declared target is a HEAP
        // collection (`Array<T>` / `OwnedArray<T>` / Stack / Queue, §15.2) must
        // materialize a real heap object — NOT the inline-array flat store below.
        // The inline path emits a 2-index GEP into `arrTy`, but a heap collection
        // lowers `arrTy` to a bare `ptr` (a pointer to the runtime bz_array_t),
        // so that GEP is malformed IR ("Invalid indices for GEP pointer type")
        // and the verifier aborts. Materialize via the runtime instead (same
        // bz_array_alloc + bz_array_add the `new`/`.Add` paths use) and store the
        // resulting pointer. Handles the empty literal `[]` too (§15.4: admitted
        // for every collection target — an empty heap array of Count 0).
        if (!cl->isDict && declType) {
            if (const sema::TypeRef* collTy = ctx_.resolvedType(*declType)) {
                if (llvm::Value* arr =
                        materializeHeapCollectionLiteral(cl, collTy)) {
                    llvm::AllocaInst* slot = entryAlloca(ptrTy());
                    locals_[declNode] = slot;
                    builder_.CreateStore(arr, slot);
                    return;
                }
            }
        }
        // #534 / stdlib §4.8: dictionary literal `["k1":v1, "k2":v2, …]` — allocate
        // and populate a Dictionary<K,V> (or OwnedDictionary<K,V>) via the SAME
        // bz_dict_alloc / bz_dict_set_* calls that `new Dictionary()` + `.Add(k,v)`
        // use. The slot is a bare `ptr` (same as for the new-expr path). The resolved
        // dict TypeRef is read from the declared type annotation; without it we fall
        // through and let lowerExpr return null (unrecoverable, same as before).
        if (cl->isDict && !cl->keys.empty() && declType) {
            if (const sema::TypeRef* dictTy = ctx_.resolvedType(*declType)) {
                if (llvm::Value* tbl = materializeHeapDictLiteral(cl, dictTy)) {
                    llvm::AllocaInst* slot = entryAlloca(ptrTy());
                    locals_[declNode] = slot;
                    builder_.CreateStore(tbl, slot);
                    return;
                }
            }
        }
        // #559: if the declared type is a hash-backed Set/OwnedSet and
        // materializeHeapSetLiteral returned null (unrecognised element type —
        // a sema error), do NOT fall through to the inline-array GEP path: that
        // path treats the Set's `ptr` slot as a struct → malformed IR → crash.
        // A missing-materialization here means sema already diagnosed the type
        // mismatch; just skip.
        if (!cl->isDict && declType) {
            if (const sema::TypeRef* collTy = ctx_.resolvedType(*declType)) {
                if (isCollectionTypeName(collTy)) {
                    std::string_view cn =
                        collTy->astNode && collTy->astNode->name &&
                        !collTy->astNode->name->parts.empty()
                            ? collTy->astNode->name->parts[0]
                            : std::string_view{};
                    if (cn == "Set" || cn == "OwnedSet")
                        return;  // materialization failed; safe no-op
                }
            }
        }
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
    // fix #541 / §7.1 / §17.14: when a lambda is assigned to an explicitly-typed
    // local whose declared type is a function type with a NULLABLE VALUE return
    // (e.g. `var f: (string)->int? := (k)=>42`), set expectedFunctionType_ so
    // that lowerLambda derives the correct `{i8,T}` LLVM return type for the
    // closure function — exactly as buildOrderedArgs does for function arguments.
    // Without this, lowerLambda has no context, derives `i32` from the body
    // literal, and emits `ret i32 42` into a function whose call site expects
    // `{i8,i32}` → IR verification failure.
    const sema::TypeRef* savedExpectedForLambdaDecl = expectedFunctionType_;
    if (init && declType && dynamic_cast<const ast::LambdaExpr*>(init)) {
        if (const sema::TypeRef* declTR = ctx_.resolvedType(*declType)) {
            if (declTR->kind == sema::TypeRef::Kind::Function)
                expectedFunctionType_ = declTR;
        }
    }
    llvm::Value* v = init ? lowerExpr(*init) : nullptr;
    expectedFunctionType_ = savedExpectedForLambdaDecl;
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
    // §29.2 (#552): when a user class (implementing IEnumerable<T>) is assigned
    // to a local declared as IEnumerable<T>, wrap it in a thin sm-compatible
    // enumerator exactly as buildOrderedArgs does at call sites.  Without this,
    // `using var items: IEnumerable<T> := new MyRange(...)` followed by
    // `for var x in items do` silently miscompiles (vtable[2] = Dispose, not
    // MoveNext) because lowerIEnumerableBuiltinFor's vtable-dispatch path
    // assumes the IEnumerable<T>-typed value IS already an enumerator.
    if (v && declType && init) {
        const sema::TypeRef* declTR = ctx_.resolvedType(*declType);
        if (declTR && declTR->kind == sema::TypeRef::Kind::Named &&
            !declTR->declaration &&
            declTR->astNode && declTR->astNode->name &&
            !declTR->astNode->name->parts.empty() &&
            declTR->astNode->name->parts[0] == "IEnumerable") {
            const sema::TypeRef* initTR = ctx_.exprType(*init);
            if (initTR && initTR->kind == sema::TypeRef::Kind::Named &&
                initTR->declaration) {
                v = wrapUserClassAsIEnumerable(v, initTR->declaration, declTR);
            }
        }
    }
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
    // issue #490 / §14.2: in an INFERRED-TYPE local (`var n := x.Member`) in a
    // generic method body, the initializer may return nullptr in the
    // UNSPECIALIZED template stub (e.g. a constraint-bound method call on a
    // T-erased i32 receiver returns nullptr per the #484 guard). In that case
    // both v and declaredTy are null, so slotTy defaults to i32 above. When the
    // initializer's sema-typed return is a REFERENCE type (string, class, or
    // interface — all lower to ptr), an i32 slot causes the next use of the
    // local (e.g. n.Length or n.SomeMethod()) to emit a string/class GEP or
    // call with an i32 base → malformed IR → LLVM assertion crash.
    //
    // Fix: when v=null AND declaredTy=null (unannotated inferred-type local)
    // AND init carries a sema exprType that lowers to something other than i32,
    // prefer the sema type for the slot. This keeps the slot correctly typed in
    // the unspecialized stub so subsequent uses emit valid (if undef-valued) IR.
    // The SPECIALIZED body (emitGenericSpecialization) sees a concrete recv ptr
    // and returns a real value — it is unaffected by this guard.
    if (!v && !declaredTy && init) {
        if (const sema::TypeRef* initTy = ctx_.exprType(*init)) {
            llvm::Type* inferredTy = lowerTypeRef(initTy);
            if (inferredTy && inferredTy != i32())
                slotTy = inferredTy;
        }
    }
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
    if (smStateStruct_ && smSelf_) {
        auto fit = smLocalFields_.find(declNode);
        if (fit != smLocalFields_.end()) {
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
            smAllocaToField_[slot] = fit->second;
            locals_[declNode] = slot;
            // Restore from struct at this point (in case we're in state > 0).
            llvm::Value* fptr = builder_.CreateStructGEP(
                smStateStruct_, smSelf_, fit->second);
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
    } else if (slotTy->isPointerTy() && declType &&
               isStringTypeRefNonNull(ctx_.resolvedType(*declType))) {
        // #429 / §4.6: a `var s: string;` (no initializer) lowers to a `ptr`
        // slot. Zero-initialize it to null so a later reassignment's release-old
        // (lowerStringLocalReassign) reads null (a no-op) rather than garbage —
        // the slot starts owning "no string". DA (§7.9) still requires assignment
        // before use, so this null is never observed by user code.
        builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), slot);
    } else if (llvm::dyn_cast<llvm::ArrayType>(slotTy)) {
        // §15.1: inline arrays are zero-initialized when declared
        // without an initializer (like all locals with explicit type).
        builder_.CreateStore(llvm::ConstantAggregateZero::get(slotTy),
                             slot);
    }
    // #422 / #429 / §4.6 / §28.9: reference-count this `var` string local. A +1
    // PRODUCER (string `+` / interpolation / range-slice) transfers its single
    // reference into the slot; a BORROW (any other string initializer) is
    // RETAINED so the slot owns an independent +1 (so a later field disposal /
    // overwrite that releases the same heap string cannot dangle this local — no
    // UAF). Either way, a NON-escaping local (proven by the escape walk) is
    // released at scope end. armStringLocalRefcount centralizes the decision and
    // only acts on the `var` string locals collectStringReleaseEligible
    // classified for THIS function (so it never fires on a non-string pointer
    // local). disposeStack_ is non-empty inside any lowered block body.
    if (v)
        armStringLocalRefcount(static_cast<const void*>(declNode), slot, v,
                               init);
    // #422 / #429 / §27.5 / §28.9: if this `var` closure (function-typed) local
    // was proven non-escaping and initialized by a +1 closure producer (a lambda
    // expression, a method reference, or a call to a user-defined closure-
    // returning method — #429 RETURN category), register a scope-end
    // bz_closure_release of its single producer reference. The slot holds
    // exactly that one reference (the cell `v` just stored); no other reference
    // exists (proven by the escape walk), so releasing it at scope end frees the
    // cell + its env block exactly once — no UAF, no double-free, and (per
    // §28.9) no captured value is touched (captures are borrows). Gated on
    // currentFn_ matching the analyzed function (a lambda/thunk body lowered
    // mid-function is not analyzed → its closure locals just leak, never
    // double-freed).
    if (v && currentFn_ == closureReleaseEligibleFn_ &&
        closureReleaseEligible_.count(static_cast<const void*>(declNode)) &&
        !disposeStack_.empty()) {
        // #429 / §27.5: register boxedCellFree BEFORE closureRelease so that
        // in reverse-iteration order (emitDisposals LIFO), closureRelease fires
        // FIRST, then boxedCellFree.
        //
        // Ordering is critical: closureRelease → bz_closure_release → envdtor
        // reads captured values through the cell.  Only after that is the cell
        // safe to free.  Registering boxedCellFree first in the forward list
        // means it appears earlier (lower index), so reverse iteration reaches
        // closureRelease (higher index) first, then boxedCellFree — correct.
        //
        // Safety rationale for boxedCellFree:
        //   (a) cellFreeEligible_ (pre-scan) guarantees the captured local is
        //       captured ONLY by non-escaping lambdas — no escaping lambda env
        //       holds a pointer to this cell past scope exit.
        //   (b) closureRelease fires FIRST (see ordering above), running the
        //       envdtor which reads all values through the cell; only then does
        //       boxedCellFree free the cell itself — no UAF.
        //   (c) free(null) is a no-op (POSIX); the guarded boxing path null-
        //       initialises the cell-holder alloca in the function entry block
        //       so a scope-exit free on a never-run guard is safe.
        //
        // OVER-DISPOSAL GUARD / fix for the six failing tests:
        //   Some captured variables have their OWN disposal entry in the same
        //   scope — most commonly a `using var` class-object entry (cls != null,
        //   dynamicDispose, etc.) that reads the OBJECT POINTER through the cell
        //   via boxedStorage(slot).  If we free the cell before that disposal
        //   fires, the disposal performs a UAF on freed memory when it dereferences
        //   the cell.  (Example: `using var r := new R()` followed by a non-
        //   escaping lambda that captures `r` — LIFO order makes closureRelease
        //   fire first (correct), then boxedCellFree would free the cell, then
        //   the using-var disposal tries to load r* through the now-freed cell.)
        //
        //   Fix: skip boxedCellFree for any cell whose cell-holder slot already
        //   appears in a "reads-through-the-cell" disposal in the current scope.
        //   A "reads-through-the-cell" disposal is any entry that is NOT purely
        //   a runtime-managed release (closureRelease, stringRelease, boxRelease,
        //   boxedCellFree) — i.e., any entry that calls boxedStorage(slot) to
        //   load the CONTENTS of the cell (using-var class/collection/dict/linked-
        //   list/dynamic/unwindOnly/subscription/...).  For those variables the
        //   cell must remain valid until THEIR disposal fires.
        //
        //   The cell will continue to leak (its prior contained-leak status is
        //   preserved); this is the correct sound choice per the PRIME DIRECTIVE.
        const auto& curScope = disposeStack_.back();
        for (const auto& ce : lastLambdaCapturedCells_) {
            // Check: does this cell's slot have any reads-through-cell disposal?
            bool hasOwnerDisposal = false;
            for (const auto& existing : curScope) {
                if (existing.slot != ce.newSlot) continue;
                // These flags indicate disposals that do NOT read through the
                // cell (they just release a runtime-managed value stored in the
                // slot, not a value reached by dereferencing the slot as a cell
                // pointer).  All other disposals use boxedStorage(slot) which
                // dereferences the cell — unsafe to free beforehand.
                if (existing.stringRelease || existing.closureRelease ||
                    existing.boxRelease    || existing.boxedCellFree)
                    continue;
                // This existing disposal READS THROUGH the cell; freeing the
                // cell via boxedCellFree would be a UAF.
                hasOwnerDisposal = true;
                break;
            }
            if (hasOwnerDisposal) continue;  // leave cell alive for owner disposal
            Disposal dc{ce.newSlot, nullptr};
            dc.boxedCellFree = true;
            disposeStack_.back().push_back(dc);
        }
        Disposal d{slot, nullptr};
        d.closureRelease = true;
        disposeStack_.back().push_back(d);
    }
    // Consume lastLambdaCapturedCells_ after any lambda-initialised local is
    // processed; the vector belongs to the most-recent lowerLambda call and
    // should not be reused by subsequent locals in the same scope.
    lastLambdaCapturedCells_.clear();
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

    // §28.12 / §30: `await using var` — the resource implements IAsyncDisposable.
    // On the normal-exit path: call DisposeAsync() directly, await the returned
    // Task (a proper suspend point), then free the block (physical reclamation).
    // On the unwind path: synchronous Dispose fallback (the class should have a
    // destructor for the synchronous fallback per §28.12; awaiting across an
    // active unwind is a deferred increment).
    //
    // Resolve the DisposeAsync LLVM Function* from the concrete class (via
    // findMethodByName + methodFns_). If the static class is not known
    // (interface-typed local / call returning owned IAsyncDisposable) or the
    // method is not in methodFns_ yet, leave disposeAsyncFn null — emitDisposals
    // will use the sync fallback.
    if (ld->isAwaitUsing) {
        Disposal d{it->second, nullptr};
        d.asyncDispose = true;
        // Attempt to resolve the direct DisposeAsync function pointer.
        const sema::TypeSymbol* cls = classOfLocal(ld);
        if (!cls && ld->initializer) {
            // inferred: try the initializer's resolved type.
            if (auto* rt = ctx_.exprType(*ld->initializer)) {
                if (rt->kind == sema::TypeRef::Kind::Named)
                    cls = rt->declaration;
            }
        }
        if (cls && cls->kind == ast::TypeDeclKind::Class) {
            sema::MethodSymbol* dams = findMethodByName(cls, "DisposeAsync");
            if (dams) {
                auto fnIt = methodFns_.find(dams);
                if (fnIt != methodFns_.end())
                    d.disposeAsyncFn = fnIt->second;
            }
        }
        // disposeAsyncFn == null → sync fallback (emitDisposals handles it).
        disposeStack_.back().push_back(d);
        return;
    }

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
    // §2.15 / issue #550: a `using var` bound to BinarySerializer or
    // BinaryDeserializer — opaque runtime handles with no user TypeSymbol and
    // no vtable.  At scope end they are freed via bz_binser_free /
    // bz_binder_free.  Recognised by declared type name or NewExpr type name.
    {
        auto isBinName = [](const sema::TypeRef* rt, std::string_view nm) {
            return rt && rt->kind == sema::TypeRef::Kind::Named &&
                   !rt->declaration && rt->astNode && rt->astNode->name &&
                   !rt->astNode->name->parts.empty() &&
                   rt->astNode->name->parts[0] == nm;
        };
        bool isBinSer = false, isBinDer = false;
        if (ld->type) {
            auto* rt = ctx_.resolvedType(*ld->type);
            isBinSer = isBinName(rt, "BinarySerializer");
            isBinDer = isBinName(rt, "BinaryDeserializer");
        }
        if (!isBinSer && !isBinDer && ld->initializer) {
            if (auto* ne =
                    dynamic_cast<const ast::NewExpr*>(ld->initializer.get())) {
                isBinSer = ne->type && ne->type->name &&
                           !ne->type->name->parts.empty() &&
                           ne->type->name->parts[0] == "BinarySerializer";
                isBinDer = ne->type && ne->type->name &&
                           !ne->type->name->parts.empty() &&
                           ne->type->name->parts[0] == "BinaryDeserializer";
            }
        }
        if (isBinSer) {
            Disposal d{it->second, nullptr};
            d.binarySerializer = true;
            disposeStack_.back().push_back(d);
            return;
        }
        if (isBinDer) {
            Disposal d{it->second, nullptr};
            d.binaryDeserializer = true;
            disposeStack_.back().push_back(d);
            return;
        }
    }
    // §2.15 / issue #550: a `using var` bound to JsonSerializer or
    // JsonDeserializer — opaque runtime handles freed via bz_binser_free /
    // bz_binder_free (same free functions as Binary variants).
    {
        auto isJsonName = [](const sema::TypeRef* rt, std::string_view nm) {
            return rt && rt->kind == sema::TypeRef::Kind::Named &&
                   !rt->declaration && rt->astNode && rt->astNode->name &&
                   !rt->astNode->name->parts.empty() &&
                   rt->astNode->name->parts[0] == nm;
        };
        bool isJsonSer = false, isJsonDer = false;
        if (ld->type) {
            auto* rt = ctx_.resolvedType(*ld->type);
            isJsonSer = isJsonName(rt, "JsonSerializer");
            isJsonDer = isJsonName(rt, "JsonDeserializer");
        }
        if (!isJsonSer && !isJsonDer && ld->initializer) {
            if (auto* ne =
                    dynamic_cast<const ast::NewExpr*>(ld->initializer.get())) {
                isJsonSer = ne->type && ne->type->name &&
                            !ne->type->name->parts.empty() &&
                            ne->type->name->parts[0] == "JsonSerializer";
                isJsonDer = ne->type && ne->type->name &&
                            !ne->type->name->parts.empty() &&
                            ne->type->name->parts[0] == "JsonDeserializer";
            }
        }
        if (isJsonSer) {
            Disposal d{it->second, nullptr};
            d.jsonSerializer = true;
            disposeStack_.back().push_back(d);
            return;
        }
        if (isJsonDer) {
            Disposal d{it->second, nullptr};
            d.jsonDeserializer = true;
            disposeStack_.back().push_back(d);
            return;
        }
    }
    // §8.1 / issue #646: a `using var` bound to a Stopwatch — a builtin
    // heap-allocated class (bz_stopwatch_t*) with no user TypeSymbol, freed
    // via bz_stopwatch_free. Recognised by two patterns:
    //   1. NewExpr type name is "Stopwatch" (`new Stopwatch()`).
    //   2. Initializer exprType is the builtin "Stopwatch" class
    //      (Stopwatch.StartNew() whose return-type sema sets to a Named TypeRef
    //      with builtinClassSym("Stopwatch")).
    {
        bool isSW = false;
        if (!isSW && ld->initializer) {
            if (auto* ne =
                    dynamic_cast<const ast::NewExpr*>(ld->initializer.get()))
                isSW = ne->type && ne->type->name &&
                       !ne->type->name->parts.empty() &&
                       ne->type->name->parts[0] == "Stopwatch";
        }
        if (!isSW && ld->initializer) {
            if (auto* rt = ctx_.exprType(*ld->initializer))
                isSW = builtinTypeName(rt) == "Stopwatch";
        }
        if (isSW) {
            Disposal d{it->second, nullptr};
            d.stopwatch = true;
            disposeStack_.back().push_back(d);
            return;
        }
    }
    // §6.1 / §30.8: a `using var` bound to a CancellationTokenSource — a
    // builtin owned class with no user TypeSymbol, disposed via bz_cts_dispose
    // (idempotent: cancels surviving token observers, then frees the source).
    // Recognised by three patterns:
    //   1. Explicit type annotation names CancellationTokenSource (astNode path).
    //   2. Initializer is `new CancellationTokenSource()` (NewExpr type name).
    //   3. (#463) Initializer is a CallExpr whose sema exprType is
    //      CancellationTokenSource — e.g. `CancellationTokenSource.WithCancelAfter(d)`.
    //      After the sema fix, the call expression carries a TypeRef with
    //      builtinClassSym("CancellationTokenSource") as declaration (no user
    //      TypeDecl), so builtinTypeName returns "CancellationTokenSource".
    {
        bool isCTS = false;
        if (ld->type) {
            auto* rt = ctx_.resolvedType(*ld->type);
            isCTS = rt && rt->kind == sema::TypeRef::Kind::Named &&
                    !rt->declaration && rt->astNode && rt->astNode->name &&
                    !rt->astNode->name->parts.empty() &&
                    rt->astNode->name->parts[0] == "CancellationTokenSource";
        }
        if (!isCTS && ld->initializer) {
            if (auto* ne =
                    dynamic_cast<const ast::NewExpr*>(ld->initializer.get())) {
                isCTS = ne->type && ne->type->name &&
                        !ne->type->name->parts.empty() &&
                        ne->type->name->parts[0] == "CancellationTokenSource";
            }
        }
        // Pattern 3 (#463): static factory (e.g. WithCancelAfter) whose sema
        // return type is CancellationTokenSource — identified via builtinTypeName.
        if (!isCTS && ld->initializer) {
            if (auto* rt = ctx_.exprType(*ld->initializer))
                isCTS = builtinTypeName(rt) == "CancellationTokenSource";
        }
        if (isCTS) {
            Disposal d{it->second, nullptr};
            d.cancellationTokenSource = true;
            disposeStack_.back().push_back(d);
            return;
        }
    }
    // §6.2 / issue #647: a `using var` bound to a Mutex.
    // No user TypeSymbol; recognised by NewExpr type name or inferred exprType
    // name "Mutex". At scope end, bz_mutex_dispose is called (idempotent:
    // marks disposed, drops the Mutex object's core ref).
    {
        bool isMx = false;
        auto isMutexTypeName = [](const sema::TypeRef* rt) {
            return rt && rt->kind == sema::TypeRef::Kind::Named &&
                   !rt->declaration && rt->astNode && rt->astNode->name &&
                   !rt->astNode->name->parts.empty() &&
                   rt->astNode->name->parts[0] == "Mutex";
        };
        if (ld->type)
            isMx = isMutexTypeName(ctx_.resolvedType(*ld->type));
        if (!isMx && ld->initializer) {
            if (auto* ne =
                    dynamic_cast<const ast::NewExpr*>(ld->initializer.get()))
                isMx = ne->type && ne->type->name &&
                       !ne->type->name->parts.empty() &&
                       ne->type->name->parts[0] == "Mutex";
        }
        if (!isMx && ld->initializer)
            isMx = isMutexTypeName(ctx_.exprType(*ld->initializer));
        if (isMx) {
            Disposal d{it->second, nullptr};
            d.mutex = true;
            disposeStack_.back().push_back(d);
            return;
        }
    }
    // §6.2 / issue #647: a `using var` bound to a Lock (owned guard returned
    // by Mutex.Acquire() / TryAcquire()). No user TypeSymbol; recognised by the
    // inferred expression type name "Lock". At scope end, bz_lock_release is
    // called (idempotent per §28.2: the released flag prevents double-release).
    // bz_lock_release is null-safe (handles a null Lock? on the timeout path).
    {
        bool isLk = false;
        auto isLockTypeName = [](const sema::TypeRef* rt) {
            return rt && rt->kind == sema::TypeRef::Kind::Named &&
                   !rt->declaration && rt->astNode && rt->astNode->name &&
                   !rt->astNode->name->parts.empty() &&
                   rt->astNode->name->parts[0] == "Lock";
        };
        if (ld->type)
            isLk = isLockTypeName(ctx_.resolvedType(*ld->type));
        // Lock can only be obtained from a method call, so check the initializer
        // expression type (the return type of Acquire / TryAcquire).
        if (!isLk && ld->initializer)
            isLk = isLockTypeName(ctx_.exprType(*ld->initializer));
        if (isLk) {
            Disposal d{it->second, nullptr};
            d.lock = true;
            disposeStack_.back().push_back(d);
            return;
        }
    }
    // §6.3 / issue #647: a `using var` bound to a Semaphore.
    // No user TypeSymbol; recognised by type name "Semaphore". At scope end,
    // bz_semaphore_dispose is called (idempotent: marks disposed, drops the
    // Semaphore object's core ref).
    {
        bool isSem = false;
        auto isSemaphoreTypeName = [](const sema::TypeRef* rt) {
            return rt && rt->kind == sema::TypeRef::Kind::Named &&
                   !rt->declaration && rt->astNode && rt->astNode->name &&
                   !rt->astNode->name->parts.empty() &&
                   rt->astNode->name->parts[0] == "Semaphore";
        };
        if (ld->type)
            isSem = isSemaphoreTypeName(ctx_.resolvedType(*ld->type));
        if (!isSem && ld->initializer) {
            if (auto* ne =
                    dynamic_cast<const ast::NewExpr*>(ld->initializer.get()))
                isSem = ne->type && ne->type->name &&
                        !ne->type->name->parts.empty() &&
                        ne->type->name->parts[0] == "Semaphore";
        }
        if (!isSem && ld->initializer)
            isSem = isSemaphoreTypeName(ctx_.exprType(*ld->initializer));
        if (isSem) {
            Disposal d{it->second, nullptr};
            d.semaphore = true;
            disposeStack_.back().push_back(d);
            return;
        }
    }
    // §6.3 / issue #647: a `using var` bound to a SemaphoreToken (owned
    // permit guard returned by Semaphore.Acquire() / TryAcquire()). No user
    // TypeSymbol; recognised by the inferred expression type name "SemaphoreToken".
    // At scope end, bz_semaphore_token_release is called (idempotent per §28.2).
    // bz_semaphore_token_release is null-safe (handles null SemaphoreToken?).
    {
        bool isTok = false;
        auto isSemTokenTypeName = [](const sema::TypeRef* rt) {
            return rt && rt->kind == sema::TypeRef::Kind::Named &&
                   !rt->declaration && rt->astNode && rt->astNode->name &&
                   !rt->astNode->name->parts.empty() &&
                   rt->astNode->name->parts[0] == "SemaphoreToken";
        };
        if (ld->type)
            isTok = isSemTokenTypeName(ctx_.resolvedType(*ld->type));
        // SemaphoreToken can only be obtained from a method call (Acquire /
        // TryAcquire), so check the initializer expression type.
        if (!isTok && ld->initializer)
            isTok = isSemTokenTypeName(ctx_.exprType(*ld->initializer));
        if (isTok) {
            Disposal d{it->second, nullptr};
            d.semaphoreToken = true;
            disposeStack_.back().push_back(d);
            return;
        }
    }
    // §30.15 (#600 phase c): a `using var` bound to a Channel<T>.
    // No user TypeSymbol; recognised by type name "Channel". At scope end,
    // bz_channel_dispose is called (closes channel, faults pending receivers with
    // ChannelClosedException, disposes undelivered items in reverse insertion
    // order per §28.11).
    // Detection: NewExpr type name OR inferred exprType name "Channel".
    {
        bool isCh = false;
        auto isChannelTypeName = [](const sema::TypeRef* rt) {
            return rt && rt->kind == sema::TypeRef::Kind::Named &&
                   !rt->declaration && rt->astNode && rt->astNode->name &&
                   !rt->astNode->name->parts.empty() &&
                   rt->astNode->name->parts[0] == "Channel";
        };
        if (ld->type)
            isCh = isChannelTypeName(ctx_.resolvedType(*ld->type));
        if (!isCh && ld->initializer) {
            if (auto* ne =
                    dynamic_cast<const ast::NewExpr*>(ld->initializer.get()))
                isCh = ne->type && ne->type->name &&
                       !ne->type->name->parts.empty() &&
                       ne->type->name->parts[0] == "Channel";
        }
        if (!isCh && ld->initializer)
            isCh = isChannelTypeName(ctx_.exprType(*ld->initializer));
        if (isCh) {
            Disposal d{it->second, nullptr};
            d.channel = true;
            disposeStack_.back().push_back(d);
            return;
        }
    }
    // §30.15 (#600 phase c): a `using var` bound to a ChannelSender<T>.
    // No user TypeSymbol; recognised by type name "ChannelSender". At scope end,
    // bz_channel_sender_release is called (decrements sender count; may close
    // the channel if this was the last sender).
    {
        bool isTx = false;
        auto isSenderTypeName = [](const sema::TypeRef* rt) {
            return rt && rt->kind == sema::TypeRef::Kind::Named &&
                   !rt->declaration && rt->astNode && rt->astNode->name &&
                   !rt->astNode->name->parts.empty() &&
                   rt->astNode->name->parts[0] == "ChannelSender";
        };
        if (ld->type)
            isTx = isSenderTypeName(ctx_.resolvedType(*ld->type));
        if (!isTx && ld->initializer)
            isTx = isSenderTypeName(ctx_.exprType(*ld->initializer));
        if (isTx) {
            Disposal d{it->second, nullptr};
            d.channelSender = true;
            disposeStack_.back().push_back(d);
            return;
        }
    }
    // §28.13 / stdlib §2.16 (#603): a `using var` bound to a Shared<T> handle.
    // No user TypeSymbol; recognised by type name "Shared". At scope end,
    // bz_shared_release decrements the strong count and disposes the inner
    // value exactly once when the last strong handle is released.
    // Detection patterns mirror CTS (declared type OR NewExpr type name):
    //   1. Explicit type annotation names Shared<T>.
    //   2. Initializer is `new Shared<T>(...)` (NewExpr type name).
    //   3. Initializer is a call returning Shared<T> (Clone()).
    {
        bool isShared = false;
        auto isSharedTypeName = [](const sema::TypeRef* rt) {
            return rt && rt->kind == sema::TypeRef::Kind::Named &&
                   !rt->declaration && rt->astNode && rt->astNode->name &&
                   !rt->astNode->name->parts.empty() &&
                   rt->astNode->name->parts[0] == "Shared";
        };
        if (ld->type)
            isShared = isSharedTypeName(ctx_.resolvedType(*ld->type));
        if (!isShared && ld->initializer) {
            if (auto* ne =
                    dynamic_cast<const ast::NewExpr*>(ld->initializer.get()))
                isShared = ne->type && ne->type->name &&
                           !ne->type->name->parts.empty() &&
                           ne->type->name->parts[0] == "Shared";
        }
        if (!isShared && ld->initializer)
            isShared = isSharedTypeName(ctx_.exprType(*ld->initializer));
        if (isShared) {
            Disposal d{it->second, nullptr};
            d.sharedHandle = true;
            disposeStack_.back().push_back(d);
            return;
        }
    }
    // §28.13 / stdlib §2.16 (#603): a `using var` bound to a Weak<T> handle.
    // No user TypeSymbol; recognised by type name "Weak". At scope end,
    // bz_weak_release decrements the weak count and frees the control block
    // when both strong and weak counts are 0.
    // The `out using var` slot created by TryUpgrade also ends up here (its
    // declared type is Weak<T> if the upgrade failed, but actually the SUCCESS
    // path binds a Shared<T> handle — handled by sharedHandle above because
    // TryUpgrade stores a Shared<T> into the out-slot and that slot's type is
    // Shared<T> not Weak<T>).
    {
        bool isWeak = false;
        auto isWeakTypeName = [](const sema::TypeRef* rt) {
            return rt && rt->kind == sema::TypeRef::Kind::Named &&
                   !rt->declaration && rt->astNode && rt->astNode->name &&
                   !rt->astNode->name->parts.empty() &&
                   rt->astNode->name->parts[0] == "Weak";
        };
        if (ld->type)
            isWeak = isWeakTypeName(ctx_.resolvedType(*ld->type));
        if (!isWeak && ld->initializer) {
            if (auto* ne =
                    dynamic_cast<const ast::NewExpr*>(ld->initializer.get()))
                isWeak = ne->type && ne->type->name &&
                         !ne->type->name->parts.empty() &&
                         ne->type->name->parts[0] == "Weak";
        }
        if (!isWeak && ld->initializer)
            isWeak = isWeakTypeName(ctx_.exprType(*ld->initializer));
        if (isWeak) {
            Disposal d{it->second, nullptr};
            d.weakHandle = true;
            disposeStack_.back().push_back(d);
            return;
        }
    }
    // issue #545: a `using var` bound to a Buffer<T>.
    // No user TypeSymbol; recognised by type name "Buffer" in the declared type
    // or the NewExpr type name.  At scope end, bz_buffer_free is called (null-safe
    // plain free — value elements carry no disposal obligation).
    {
        bool isBuf = false;
        auto isBufferTypeName = [](const sema::TypeRef* rt) {
            return rt && rt->kind == sema::TypeRef::Kind::Named &&
                   !rt->declaration && rt->astNode && rt->astNode->name &&
                   !rt->astNode->name->parts.empty() &&
                   rt->astNode->name->parts[0] == "Buffer";
        };
        if (ld->type)
            isBuf = isBufferTypeName(ctx_.resolvedType(*ld->type));
        if (!isBuf && ld->initializer) {
            if (auto* ne =
                    dynamic_cast<const ast::NewExpr*>(ld->initializer.get()))
                isBuf = ne->type && ne->type->name &&
                        !ne->type->name->parts.empty() &&
                        ne->type->name->parts[0] == "Buffer";
        }
        if (!isBuf && ld->initializer)
            isBuf = isBufferTypeName(ctx_.exprType(*ld->initializer));
        if (isBuf) {
            Disposal d{it->second, nullptr};
            d.buffer = true;
            disposeStack_.back().push_back(d);
            return;
        }
    }
    // stdlib §6.7 / §30.5: a `using var` bound to a Task / Task<T> / OwnedTask<T>.
    // Tasks have no user TypeSymbol; at scope end the task is freed via bz_task_free.
    // Double-free safety: lowerAwait only frees tasks whose operand is an anonymous
    // CallExpr (e.g. `await Producer()`), never a named local (`await t`) — so a
    // using-var-bound task is freed exactly once here at scope end.
    // Recognised by the initializer's inferred expression type name "Task" or
    // "OwnedTask", or by the declared type name.
    if (taskTypeOfLocal(ld)) {
        Disposal d{it->second, nullptr};
        d.task = true;
        disposeStack_.back().push_back(d);
        return;
    }
    // #362 / stdlib §4.5: a `using var` bound to a heap collection
    // (Array<T>/OwnedArray<T>/Dictionary/Set/…). These are builtins with no
    // user TypeSymbol, so classOfLocal returns null; recognize them by name and
    // register a collection disposal. For an OWNING collection of class
    // elements the disposal cascades into the elements (the leak fixed here);
    // a borrowing collection or value elements free only the backing.
    // per §4.6/§4.7: a `using var` LinkedList<T>/OwnedLinkedList<T>. Check BEFORE
    // the array-backed collection path because isCollectionTypeName now excludes
    // LinkedList (it is bz_llist_t-backed, not bz_array_t-backed).
    if (const sema::TypeRef* llTy = linkedListTypeOfLocal(ld)) {
        Disposal d{it->second, nullptr};
        d.linkedList = true;
        // #602 / §28.11 v0.135: derive element disposer directly from
        // elementPolicyFor (covering class/interface elements).
        {
            const sema::TypeRef* llEl =
                (!llTy->genericArgs.empty()) ? llTy->genericArgs[0] : nullptr;
            bool llIsOwning =
                llTy->astNode && llTy->astNode->name &&
                !llTy->astNode->name->parts.empty() &&
                llTy->astNode->name->parts[0].rfind("Owned", 0) == 0;
            d.collectionElemDisposer =
                elementPolicyFor(llEl, llIsOwning).disposerThunk;
        }
        disposeStack_.back().push_back(d);
        return;
    }
    if (const sema::TypeRef* collTy = collectionTypeOfLocal(ld)) {
        if (isCollectionTypeName(collTy)) {
            // #429 / §28.9 / §4.10: Set<string> is dict-backed (bz_dict_t*), NOT
            // array-backed — intercept BEFORE the d.collection = true branch so we
            // route it through bz_dict_free (with bz_set_string_keys_release) instead
            // of the erroneous bz_array_free path that every Set<T> currently falls
            // into (pre-existing safe-but-leaky behavior for non-string sets). This
            // is the minimal targeted fix for the #429 Set<string> sub-slice; fixing
            // all Set<T> disposal (replacing bz_array_free with bz_dict_free) is a
            // broader correctness fix deferred to a separate lane.
            // #602 / §28.11 v0.135: derive element category from elementPolicyFor.
            auto isStrElem = [&](const sema::TypeRef* ty, bool owning) {
                if (!ty || ty->genericArgs.empty() || !ty->genericArgs[0]) return false;
                const sema::TypeRef* el = ty->genericArgs[0];
                if (el->nullable) return false;
                ElementPolicy p = elementPolicyFor(el, owning);
                return p.cat == ElementPolicy::Category::RuntimeManaged &&
                       el->kind == sema::TypeRef::Kind::Primitive &&
                       el->primitive == sema::PrimitiveType::String;
            };
            bool collIsOwning =
                collTy->astNode && collTy->astNode->name &&
                !collTy->astNode->name->parts.empty() &&
                collTy->astNode->name->parts[0].rfind("Owned", 0) == 0;
            bool isSetName =
                collTy->astNode && collTy->astNode->name &&
                !collTy->astNode->name->parts.empty() &&
                collTy->astNode->name->parts[0] == "Set";
            if (isSetName && isStrElem(collTy, /*owning=*/false)) {
                Disposal d{it->second, nullptr};
                d.dict = true;
                d.dictStringKeys = true;  // signal pre-free string key release
                disposeStack_.back().push_back(d);
                return;
            }
            Disposal d{it->second, nullptr};
            d.collection = true;
            // #602 / §28.11 v0.135: element disposer from policy (class/interface
            // owned → vtable thunk; Shared/Weak owned → shared/weak release thunk).
            {
                const sema::TypeRef* collEl =
                    (!collTy->genericArgs.empty()) ? collTy->genericArgs[0] : nullptr;
                ElementPolicy cp = elementPolicyFor(collEl, collIsOwning);
                d.collectionElemDisposer = cp.disposerThunk;
                // #429 / §4.6 / §28.9: a borrowing Array/Stack/Queue<string>
                // co-owns a +1 per element — use the string-release thunk.
                // Mutually exclusive with the class-disposer above.
                if (!d.collectionElemDisposer &&
                        cp.cat == ElementPolicy::Category::RuntimeManaged &&
                        collEl &&
                        collEl->kind == sema::TypeRef::Kind::Primitive &&
                        collEl->primitive == sema::PrimitiveType::String &&
                        !collEl->nullable) {
                    d.collectionElemDisposer = stringElementReleaseThunk();
                }
            }
            disposeStack_.back().push_back(d);
            return;
        }
    }
    // #394 / stdlib §4.9 + #401 / stdlib §4.8: a `using var` bound to a
    // hash-backed dictionary (Dictionary<K,V>/OwnedDictionary<K,V>). Dicts are
    // excluded from isCollectionTypeName (they free via bz_dict_free, not
    // bz_array_free), so they get their own disposal kind here. The per-value
    // callback is one of exactly three mutually exclusive things (or null):
    //   * #394: an OwnedDictionary<K, class/interface> → cascade disposal into
    //     each surviving value (dispose + free via vtable) — #602: covers
    //     Interface too via elementPolicyFor.
    //   * #401: a borrowing Dictionary<K, record> → free each surviving record
    //     value BOX (plain free; a record is a value type with nothing to
    //     dispose, §3.2 / §9).
    //   * #429 / §28.9: a borrowing Dictionary<K, string> → release each
    //     surviving string value's +1 (bz_string_release via the
    //     stringElementReleaseThunk). The dict co-owns a +1 per stored value.
    // Keys are never disposed via dictValueDisposer (§4.9: keys are value-like;
    // the dictionary owns its values only). Exception: Set<string> stores strings
    // as KEYS — that case sets dictStringKeys = true (see the collection block
    // above) and leaves dictValueDisposer null.
    if (const sema::TypeRef* dictTy = dictTypeOfLocal(ld)) {
        Disposal d{it->second, nullptr};
        d.dict = true;
        // Set<string>: elements are KEYS — pre-free key release, not a val disposer.
        // #602 / §28.11 v0.135: use elementPolicyFor for the is-string-element test.
        bool dictIsOwning =
            dictTy->astNode && dictTy->astNode->name &&
            !dictTy->astNode->name->parts.empty() &&
            dictTy->astNode->name->parts[0] == "OwnedDictionary";
        bool isSetNameD =
            dictTy->astNode && dictTy->astNode->name &&
            !dictTy->astNode->name->parts.empty() &&
            dictTy->astNode->name->parts[0] == "Set";
        auto isStrKey = [&]() {
            if (dictTy->genericArgs.empty() || !dictTy->genericArgs[0]) return false;
            const sema::TypeRef* k = dictTy->genericArgs[0];
            if (k->nullable) return false;
            ElementPolicy p = elementPolicyFor(k, /*owning=*/false);
            return p.cat == ElementPolicy::Category::RuntimeManaged &&
                   k->kind == sema::TypeRef::Kind::Primitive &&
                   k->primitive == sema::PrimitiveType::String;
        };
        if (isSetNameD && isStrKey()) {
            d.dictStringKeys = true;
        } else {
            // #602 / §28.11 v0.135: owning dict value disposer from policy.
            {
                const sema::TypeRef* dictVal =
                    (dictTy->genericArgs.size() > 1) ? dictTy->genericArgs[1]
                                                     : nullptr;
                d.dictValueDisposer =
                    elementPolicyFor(dictVal, dictIsOwning).disposerThunk;
            }
            if (!d.dictValueDisposer)
                d.dictValueDisposer = dictValueBoxFreer(dictTy);
            // #429 / §28.9: a borrowing Dictionary<K, string> co-owns a +1 per
            // stored value — release each at scope-end free via the string-release
            // thunk. Mutually exclusive with the owning-class and record-box-free
            // disposers checked above.
            if (!d.dictValueDisposer && !dictIsOwning) {
                const sema::TypeRef* dictVal =
                    (dictTy->genericArgs.size() > 1) ? dictTy->genericArgs[1]
                                                     : nullptr;
                if (dictVal && !dictVal->nullable) {
                    ElementPolicy dvp = elementPolicyFor(dictVal, false);
                    if (dvp.cat == ElementPolicy::Category::RuntimeManaged &&
                            dictVal->kind == sema::TypeRef::Kind::Primitive &&
                            dictVal->primitive == sema::PrimitiveType::String)
                        d.dictValueDisposer = stringElementReleaseThunk();
                }
            }
        }
        disposeStack_.back().push_back(d);
        return;
    }
    // #628 / stdlib §4.9 v0.135: a `using var` bound to a DrainedEntry<K,V>
    // obtained from `for var entry in d.Drain() do … end`. The entry struct
    // holds {K key; V* value}; ownership of the V instance must be discharged
    // at scope end. Additionally, if K is string the bz_string_t* at field[0]
    // must be released (the drain runtime transferred its ownership to the
    // loop variable). Recognition: initializer's exprType has importedName
    // "DrainedEntry" (synthetic TypeRef set by sema for OwnedDictionary.Drain
    // in ExprTypecheck_Calls.cpp).
    {
        const sema::TypeRef* iniTy = ld->initializer
            ? ctx_.exprType(*ld->initializer) : nullptr;
        bool isDrainedEntry =
            iniTy && iniTy->kind == sema::TypeRef::Kind::Named &&
            !iniTy->declaration && iniTy->importedName == "DrainedEntry" &&
            iniTy->genericArgs.size() >= 2 &&
            iniTy->genericArgs[0] && iniTy->genericArgs[1];
        if (isDrainedEntry) {
            const sema::TypeRef* kTy = iniTy->genericArgs[0];
            const sema::TypeRef* vTy = iniTy->genericArgs[1];
            Disposal d{it->second, nullptr};
            d.drainedEntry = true;
            d.drainedEntryKeyIsString =
                kTy->kind == sema::TypeRef::Kind::Primitive &&
                kTy->primitive == sema::PrimitiveType::String;
            // Build LLVM types for field GEPs (K and V*).
            d.drainedEntryKeyType = lowerTypeRef(kTy);
            d.drainedEntryValType = lowerTypeRef(vTy);  // ptr for class
            // Value disposer thunk (for owning class V). elementPolicyFor
            // returns the vtable-based thunk when V is a user class; null
            // for builtins (use emitDisposeAndReclaim directly in emitDisposals).
            d.drainedEntryValDisposer =
                elementPolicyFor(vTy, /*owning=*/true).disposerThunk;
            disposeStack_.back().push_back(d);
            return;
        }
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
    // dynamically through the object's own vtable (slot 2 = no-op bz$exDispose;
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
    if (collectionTypeOfLocal(ld) || dictTypeOfLocal(ld) ||
        linkedListTypeOfLocal(ld)) return nullptr;
    // stdlib §6.7 / §30.5: Task / Task<T> — disposed via bz_task_free, not
    // class vtable dispatch. Exclude from the owned-class unwind path.
    if (taskTypeOfLocal(ld)) return nullptr;
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
                // §28.10 / #707: for imported methods ms->decl is null (the
                // ModuleLoader attaches no AST MethodDecl); the owned-return
                // flag lives in SemaContext::importedReturnOwned_.  Fall back
                // to that when md is absent — but ONLY when importedReturnOwned
                // is true (a borrow return must NOT be disposed).
                bool isOwned = (md && md->returnOwned) ||
                               (!md && ctx_.importedReturnOwned(ms));
                if (isOwned &&
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
    // §3.4 / §4.6 / issue #474: a string Range slice `s[a..b]` is a fresh +1
    // runtime-managed string (emitStringRangeSlice allocates with rc := 1), so a
    // `var sub := s[a..b]` local owns the producer reference and must release it
    // at scope end. Recognize it by the IndexExpr's target being a string and
    // the index being a RangeExpr (the same shape codegen lowers to a slice).
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(init)) {
        if (ix->target && !ix->indices.empty() && ix->indices[0].value &&
            dynamic_cast<const ast::RangeExpr*>(ix->indices[0].value.get())) {
            auto* tt = ctx_.exprType(*ix->target);
            return tt && tt->kind == sema::TypeRef::Kind::Primitive &&
                   tt->primitive == sema::PrimitiveType::String;
        }
    }
    // #429 (OPERATOR-OPERAND category) / §4.6 / §28.9: a CALL to the inline
    // CG27c ToString() path on a NUMERIC or CHAR primitive (emitPrimitiveToString
    // allocates a fresh heap string with rc := 1). Recognised by a CallExpr with
    // an empty argument list, member == "ToString", and a numeric/char/decimal
    // receiver type. Bool.ToString() is EXCLUDED (returns a "True"/"False"
    // sentinel with the immortal rc — not heap-allocated); String.ToString() is
    // EXCLUDED (identity, no allocation); Object.ToString() is EXCLUDED (goes
    // through lowerObjectToString which may or may not allocate — excluded for
    // safety). This recognition makes releaseConcatIntermediate release the
    // emitPrimitiveToString result when it feeds directly into a `+` operand,
    // comparison, or other consumer — fixing the `"ab" + i.ToString()` shape.
    if (auto* call = dynamic_cast<const ast::CallExpr*>(init)) {
        if (call->args.empty() && call->callee) {
            if (auto* ma =
                    dynamic_cast<const ast::MemberAccessExpr*>(call->callee.get())) {
                if (ma->member == "ToString" && ma->target) {
                    auto* tt = ctx_.exprType(*ma->target);
                    if (tt && tt->kind == sema::TypeRef::Kind::Primitive) {
                        using P = sema::PrimitiveType;
                        switch (tt->primitive) {
                            case P::Int: case P::Long: case P::Short:
                            case P::Byte: case P::SByte: case P::UInt:
                            case P::ULong: case P::UShort:
                            case P::Float: case P::Double: case P::Decimal:
                            case P::Char:
                                return true;  // emitPrimitiveToString → rc=1 heap string
                            default:
                                break;  // Bool → sentinel; String → identity; Object → excluded
                        }
                    }
                }
            }
        }
    }
    // #429 (RETURN-escape category) / §4.6 / §28.9: a CALL to a user-defined,
    // locally-compiled method whose declared return type is `string` hands the
    // caller EXACTLY +1 (the callee's ReturnStmt lowering mints it). So such a
    // call is a +1 producer at the call site: a bound local TRANSFERS the single
    // reference (no extra retain) and releases it at scope end if non-escaping,
    // and a string-field store TRANSFERS it into the field. Builtin stdlib string
    // methods and cross-module imported methods are EXCLUDED (see the helper).
    if (isUserMethodStringReturnCall(init)) return true;
    // #429 (closure CALL-RETURN category) / §28.9 / §27.5: a CALL to a
    // function-typed LOCAL or PARAMETER whose declared funcReturn is non-nullable
    // `string`. After the lambda expr-body minting fix (lowerLambda Part 1) ALL
    // `() -> string` closures hand their caller exactly +1 — the expr-body path
    // now mints +1 for borrow returns (mirroring ReturnStmt), and the block-body
    // path always went through ReturnStmt which already minted. So calling any
    // `() -> string` local/param is a +1 string producer at the call site.
    // Recognised by: CallExpr whose callee resolves to a LocalSymbol or
    // ParameterSymbol, AND whose call-expression sema type is non-nullable
    // string (the callee's funcReturn is string). MethodSymbol callees are
    // covered by isUserMethodStringReturnCall above; this catches the
    // closure-local-call shape (`var f := C.Make(...); f()` → `f().Length`
    // must release the +1 string returned by calling f).
    if (auto* call = dynamic_cast<const ast::CallExpr*>(init)) {
        if (call->callee) {
            sema::Symbol* sym = ctx_.exprSymbol(*call->callee);
            if (dynamic_cast<sema::LocalSymbol*>(sym) ||
                dynamic_cast<sema::ParameterSymbol*>(sym)) {
                // The result type of the call is the funcReturn of the callee.
                // If that result is non-nullable string, this call is a +1 producer.
                const sema::TypeRef* resultTy = ctx_.exprType(*init);
                if (resultTy && !resultTy->nullable &&
                    resultTy->kind == sema::TypeRef::Kind::Primitive &&
                    resultTy->primitive == sema::PrimitiveType::String)
                    return true;
            }
        }
    }
    return false;
}


// #429 (RETURN-escape category) / §4.6 / §28.9: see the declaration in
// LlvmBackend_impl.hpp. Recognize a CallExpr whose resolved callee is a
// USER-DEFINED, LOCALLY-COMPILED MethodSymbol (backed by an ast::MethodDecl, not
// imported, not an operator) and whose declared return type is (non-nullable)
// `string`. Such a call goes through THIS module's ReturnStmt lowering, which
// mints exactly +1 for the returned string — so the caller owns that single
// reference. Builtin stdlib string methods (no resolved MethodSymbol — matched
// structurally in tryLowerStringMethod) are excluded. Cross-module imported
// methods are now recognized as +1 producers when they declare a non-nullable
// string return type: their ReturnStmt in the exporter module ran the same
// string-retain minting logic, so the caller receives exactly +1 and must
// release at scope end without an extra borrow-retain. (#694 residue-1 / §4.6
// / §28.9.)
bool Emitter::isUserMethodStringReturnCall(const ast::Expr* init) const {
    auto* call = dynamic_cast<const ast::CallExpr*>(init);
    if (!call || !call->callee) return false;
    auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(*call->callee));
    if (!ms) return false;                         // builtin string method / not a method
    if (ms->isOperator) return false;              // operator overloads excluded
    if (ctx_.isImported(ms)) {
        // §4.6 / §28.9 / #694 residue-1: an imported method whose declared
        // return type is non-nullable string already ran ReturnStmt string-retain
        // minting in the exporter. The caller receives +1 and must release at
        // scope end, not borrow-retain. Treat it as a +1 producer.
        const sema::TypeRef* rt = ctx_.importedReturnType(ms);
        return rt && !rt->nullable &&
               rt->kind == sema::TypeRef::Kind::Primitive &&
               rt->primitive == sema::PrimitiveType::String;
    }
    // Must be a locally-compiled body — backed by an ast::MethodDecl whose body
    // codegen lowers (so its ReturnStmt runs the +1 mint). An abstract/interface
    // method's runtime target is some override that IS compiled here and also
    // mints +1 on return, so a declared-string virtual/abstract call is fine; we
    // only require a MethodDecl with a `string` return type.
    auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
    if (!md || !md->returnType) return false;
    const sema::TypeRef* rt = ctx_.resolvedType(*md->returnType);
    return rt && !rt->nullable &&
           rt->kind == sema::TypeRef::Kind::Primitive &&
           rt->primitive == sema::PrimitiveType::String;
}


// #429 (RETURN-escape category) / §4.6 / §28.9: see the declaration in
// LlvmBackend_impl.hpp. A returned IDENTIFIER names a non-nullable string LOCAL
// that owns a +1 in its slot when EITHER it was retained at bind (a borrow local
// in stringBorrowAll_) OR its initializer is a +1 producer (the producer's +1
// was transferred into the slot — armStringLocalRefcount left it un-retained but
// the slot holds the producer's single reference). In both cases the return must
// TRANSFER that +1 to the caller rather than mint a second one (which would leak,
// since the escaped local is not released at scope end). Gated on the analyzed
// function so the eligibility sets describe THIS body (a lambda/thunk body whose
// locals were not walked returns false here → its returns fall back to the
// retain path, which is safe — at worst a leak, never a UAF/double-free).
bool Emitter::returnedStringLocalOwnsPlusOne(const ast::Expr* e) const {
    if (currentFn_ != stringReleaseEligibleFn_) return false;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(e);
    if (!id) return false;
    auto* loc = dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*id));
    if (!loc || !loc->decl) return false;
    const void* declNode = static_cast<const void*>(loc->decl);
    // A borrow local was retained at bind (owns a +1).
    if (stringBorrowAll_.count(declNode)) return true;
    // A producer-initialized non-nullable string local transferred the
    // producer's +1 into its slot. (Borrow locals are exactly stringBorrowAll_;
    // a non-using string LocalDecl whose initializer is a +1 producer is the
    // producer case — its +1 lives in the slot whether or not it escapes.)
    auto* ld = dynamic_cast<const ast::LocalDecl*>(loc->decl);
    if (ld && !ld->isUsing && ld->initializer &&
        isPlusOneStringProducer(ld->initializer.get())) {
        const sema::TypeRef* lt = nullptr;
        if (ld->type) lt = ctx_.resolvedType(*ld->type);
        if (!lt) lt = ctx_.exprType(*ld->initializer);
        return lt && !lt->nullable &&
               lt->kind == sema::TypeRef::Kind::Primitive &&
               lt->primitive == sema::PrimitiveType::String;
    }
    return false;
}


// #429 / §4.6 / §28.9: is `target` a string-typed CLASS instance field /
// auto-property lvalue — `obj.f`, `this.f`, the implicit `f`, or an auto-
// property of a class — as opposed to a local / parameter / ref-out param /
// array element / static field / RECORD field? Only such a location outlives the
// function and is released on the owner's disposal, so only it takes an
// independent +1 on store. Classification mirrors lvalueAddr's order: NOT a
// ref-param, NOT a local slot, NOT a static field — then the resolved symbol is
// a class FieldSymbol or a class auto-property PropertySymbol. A RECORD's
// reference fields are BORROWS (§9 "the record does not own what they point to")
// and a record has no disposer (§9.7), so record string fields are excluded
// (they keep the plain-store / leak behaviour).
bool Emitter::isStringFieldLvalue(const ast::Expr& target) {
    if (!isStringExprType(target)) return false;
    if (localSlot(target)) return false;       // a plain local / parameter
    {
        llvm::Type* dummy = nullptr;
        if (refParamAddr(target, dummy)) return false;  // a ref/out alias
    }
    if (staticFieldGlobal(target)) return false;  // program-lifetime storage
    sema::Symbol* sym = ctx_.exprSymbol(target);
    if (!sym) return false;
    if (auto* fs = dynamic_cast<sema::FieldSymbol*>(sym))
        return fs->parent && fs->parent->kind == ast::TypeDeclKind::Class;
    if (isAutoProperty(sym)) {
        auto* ms = dynamic_cast<sema::MemberSymbol*>(sym);
        return ms && ms->parent && ms->parent->kind == ast::TypeDeclKind::Class;
    }
    return false;
}


// #429 / §4.6 / §28.9: store `newVal` into the class string field at `fieldAddr`
// with retain-new / release-old, giving the field an INDEPENDENT +1. Order:
// load-old → retain-or-transfer-new → store-new → release-old. Retaining the new
// value BEFORE releasing the old makes a self-assignment safe (rc dips +1 then
// back, never transiently 0). A fresh +1 producer is TRANSFERRED (no retain — its
// single reference becomes the field's), anything else is RETAINED (the field's
// count is independent of the source's). bz_string_retain/release are null-safe
// and no-ops on a sentinel/literal.
bool Emitter::lowerStringFieldStore(const ast::Expr& target,
                                    llvm::Value* fieldAddr,
                                    llvm::Value* newVal,
                                    const ast::Expr* valueExpr) {
    if (!isStringFieldLvalue(target)) return false;
    if (!fieldAddr || !newVal) return false;
    if (!newVal->getType()->isPointerTy()) return false;
    llvm::Value* oldVal = builder_.CreateLoad(ptrTy(), fieldAddr, "strf.old");
    if (!isPlusOneStringProducer(valueExpr))
        emitStringRetain(newVal);
    builder_.CreateStore(newVal, fieldAddr);
    emitStringRelease(oldVal);
    return true;
}


// #429 / §4.6 / §28.9: reassign a string LOCAL/parameter slot with retain-new /
// release-old, keeping the slot's independent +1 across the overwrite. Without
// this, `x := h.Name` would make `x` alias the field's heap string WITHOUT a +1,
// so the field's disposal release (or a later overwrite) would free a value `x`
// still reads — a use-after-free. Order: load-old → retain-or-transfer-new →
// store-new → release-old (retain-before-release keeps `x := x` and `x := <alias
// of x>` safe). A +1 producer is transferred (no retain); a borrow is retained.
// Restricted to a string-typed LOCAL/parameter slot (NOT a field — that is
// lowerStringFieldStore; NOT an array element / ref-out param / static field).
// The reassigned slot keeps owning its value; the escape walk marks a reassigned
// local as escaped, so its final value is NOT auto-released at scope end and
// simply leaks (safe — never a UAF, never a double-free).
bool Emitter::lowerStringLocalReassign(const ast::Expr& target,
                                       llvm::Value* slotAddr, llvm::Type* elemTy,
                                       llvm::Value* newVal,
                                       const ast::Expr* valueExpr) {
    if (!slotAddr || !newVal) return false;
    if (!newVal->getType()->isPointerTy()) return false;
    // Must be an actual LOCAL/parameter slot, string-typed, a bare `ptr` slot.
    if (!localSlot(target)) return false;          // not a local/param
    if (!elemTy || !elemTy->isPointerTy()) return false;  // not a `ptr` slot
    if (!isStringExprType(target)) return false;   // not string-typed
    // A nullable string? target lowers to a `ptr` too; retain/release are
    // null-safe, so including it is sound — but we keep scope to the same
    // non-nullable string surface the bind-time arming uses, to avoid surprising
    // interactions. A string? local is left as a plain store (it just leaks, as
    // before — safe). Detect nullability via the resolved target type.
    if (const sema::TypeRef* tt = ctx_.exprType(target))
        if (tt->nullable) return false;
    llvm::Value* oldVal = builder_.CreateLoad(ptrTy(), slotAddr, "strl.old");
    if (!isPlusOneStringProducer(valueExpr))
        emitStringRetain(newVal);
    builder_.CreateStore(newVal, slotAddr);
    emitStringRelease(oldVal);
    return true;
}


// #429 / §4.6 / §28.9: emit the reference-count bookkeeping for a `var` STRING
// local at bind time and arm its scope-end release. The slot store itself is
// performed by the caller (lowerLocalDecl). The model gives EVERY string local
// an independent +1 for its current value, so a later field disposal / field
// overwrite / collection free that releases the same heap string never dangles a
// local that still aliases it (no UAF):
//   * A +1 PRODUCER initializer (`+` / interpolation / range-slice) TRANSFERS its
//     single reference into the slot (no retain). Released at scope end iff the
//     local was proven non-escaping (stringReleaseEligible_).
//   * A BORROW initializer (a field/param/call/identifier read) is RETAINED so
//     the slot owns a fresh +1. Released at scope end iff non-escaping
//     (stringBorrowRetainEligible_). An ESCAPING borrow local is retained but NOT
//     released — its +1 leaks (safe: never dangling, never double-freed).
// Only fires for an actual `string`-typed `var` local; a non-string / non-pointer
// value is left to the caller's plain store.
void Emitter::armStringLocalRefcount(const void* declNode,
                                     llvm::AllocaInst* slot, llvm::Value* v,
                                     const ast::Expr* init) {
    (void)init;
    if (!v || !v->getType()->isPointerTy() || disposeStack_.empty()) return;
    // Gated on the analyzed function: a lambda/thunk body lowered mid-function is
    // not walked, so the eligibility sets are empty for its locals — they just
    // leak (never double-freed). collectStringReleaseEligible classified THIS
    // function's `var` string locals into the three sets below; this function
    // only acts on members of those sets, so it never fires on a non-string
    // pointer local (a class/interface/box).
    if (currentFn_ != stringReleaseEligibleFn_) return;
    // BORROW string local (escaping or not): retain so the slot owns an
    // independent +1. A literal initializer is a sentinel — the retain is a
    // no-op. A producer-bound local is NOT in stringBorrowAll_ and so is NOT
    // retained (its single producer reference is transferred into the slot).
    if (stringBorrowAll_.count(declNode))
        emitStringRetain(v);
    // Release at scope end iff the local was proven NON-escaping — producers via
    // stringReleaseEligible_ (transfer + release), borrows via
    // stringBorrowRetainEligible_ (retain + release). An escaping local is in
    // neither set, so its +1 (the producer's, or the just-emitted borrow retain)
    // simply leaks — safe (never dangling, never double-freed).
    if (stringReleaseEligible_.count(declNode) ||
        stringBorrowRetainEligible_.count(declNode)) {
        Disposal d{slot, nullptr};
        d.stringRelease = true;
        disposeStack_.back().push_back(d);
    }
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


// #429 (closure RETURN category) / §28.9 / §28.3: see the declaration in
// LlvmBackend_impl.hpp. Recognize a CallExpr whose resolved callee is a
// USER-DEFINED, LOCALLY-COMPILED MethodSymbol (backed by an ast::MethodDecl,
// not imported, not an operator) whose declared return type is a function type
// (Kind::Function). Such a call goes through THIS module's ReturnStmt
// lowering, which mints exactly +1 for the returned closure cell — so the
// caller owns that single reference. Cross-module imported methods (their
// callee does NOT run our ReturnStmt minting) are excluded. Parallel to
// isUserMethodStringReturnCall for strings.
bool Emitter::isUserMethodClosureReturnCall(const ast::Expr* init) const {
    auto* call = dynamic_cast<const ast::CallExpr*>(init);
    if (!call || !call->callee) return false;
    auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(*call->callee));
    if (!ms) return false;
    if (ms->isOperator) return false;
    if (ctx_.isImported(ms)) return false;
    auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
    if (!md || !md->returnType) return false;
    const sema::TypeRef* rt = ctx_.resolvedType(*md->returnType);
    return rt && rt->kind == sema::TypeRef::Kind::Function;
}


// #429 (closure RETURN category) / §28.9 / §28.3: see the declaration in
// LlvmBackend_impl.hpp. A returned IDENTIFIER names a closure-typed LOCAL
// that owns a +1 in its slot when EITHER its initializer is a +1 closure
// producer (lambda / method-ref, from isPlusOneClosureProducer — the
// producer's +1 was transferred into the slot) OR it is in
// closureReleaseEligible_ (proven non-escaping at bind, will be released at
// scope end — the slot still holds the producer's single reference). In both
// cases the return must TRANSFER that +1 to the caller rather than mint a
// second one (which would leak, since the escaped local is not released at
// scope end). Gated on the analyzed function so the eligibility sets describe
// THIS body (a lambda/thunk body whose locals were not walked returns false
// here → its returns fall back to the retain path, which is safe — at worst
// a leak, never a UAF/double-free). Parallel to returnedStringLocalOwnsPlusOne.
bool Emitter::returnedClosureLocalOwnsPlusOne(const ast::Expr* e) const {
    if (currentFn_ != closureReleaseEligibleFn_) return false;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(e);
    if (!id) return false;
    auto* loc = dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*id));
    if (!loc || !loc->decl) return false;
    const void* declNode = static_cast<const void*>(loc->decl);
    // A producer-initialized closure local: the lambda / method-ref +1 was
    // transferred into the slot (closureReleaseEligible_ or not — the slot
    // always holds the producer's +1).
    auto* ld = dynamic_cast<const ast::LocalDecl*>(loc->decl);
    if (ld && !ld->isUsing && ld->initializer &&
        isPlusOneClosureProducer(ld->initializer.get())) {
        const sema::TypeRef* lt = nullptr;
        if (ld->type) lt = ctx_.resolvedType(*ld->type);
        if (!lt) lt = ctx_.exprType(*ld->initializer);
        return lt && lt->kind == sema::TypeRef::Kind::Function;
    }
    // A non-escaping closure local also in closureReleaseEligible_ was
    // proven to own its single +1 (codegen registered its scope-end release).
    if (closureReleaseEligible_.count(declNode)) return true;
    return false;
}


// #429 (closure FIELD-STORE) / §28.9: see the declaration in
// LlvmBackend_impl.hpp. A target expression is a closure-typed class
// instance field / auto-property lvalue iff:
//   - its resolved type is a function type (Kind::Function),
//   - it is NOT a plain local / parameter slot,
//   - it is NOT a ref/out alias,
//   - it is NOT a program-lifetime static field,
//   - its symbol is a FieldSymbol or auto-property of a CLASS (not a record).
// Parallel to isStringFieldLvalue for strings.
bool Emitter::isClosureFieldLvalue(const ast::Expr& target) {
    const sema::TypeRef* rt = ctx_.exprType(target);
    if (!rt || rt->kind != sema::TypeRef::Kind::Function) return false;
    if (localSlot(target)) return false;
    {
        llvm::Type* dummy = nullptr;
        if (refParamAddr(target, dummy)) return false;
    }
    if (staticFieldGlobal(target)) return false;
    sema::Symbol* sym = ctx_.exprSymbol(target);
    if (!sym) return false;
    if (auto* fs = dynamic_cast<sema::FieldSymbol*>(sym))
        return fs->parent && fs->parent->kind == ast::TypeDeclKind::Class;
    if (isAutoProperty(sym)) {
        auto* ms = dynamic_cast<sema::MemberSymbol*>(sym);
        return ms && ms->parent && ms->parent->kind == ast::TypeDeclKind::Class;
    }
    return false;
}


// #429 (closure FIELD-STORE) / §28.9: see the declaration in
// LlvmBackend_impl.hpp. Store `newVal` into the class closure field at
// `fieldAddr` with retain-new / release-old, giving the field an INDEPENDENT
// +1. Order: load-old → retain-or-transfer-new → store-new → release-old.
// Retaining the new value BEFORE releasing the old makes a self-assignment
// safe (rc dips +1 then back, never transiently 0). A +1 producer (a lambda,
// a method-ref, or a user-defined closure-returning call) is TRANSFERRED (no
// retain — its single reference becomes the field's); anything else is
// RETAINED. bz_closure_retain/release are null-safe and no-ops on null.
// Parallel to lowerStringFieldStore for strings.
bool Emitter::lowerClosureFieldStore(const ast::Expr& target,
                                     llvm::Value* fieldAddr,
                                     llvm::Value* newVal,
                                     const ast::Expr* valueExpr) {
    if (!isClosureFieldLvalue(target)) return false;
    if (!fieldAddr || !newVal) return false;
    if (!newVal->getType()->isPointerTy()) return false;
    llvm::Value* oldVal = builder_.CreateLoad(ptrTy(), fieldAddr, "clf.old");
    // A +1 producer (lambda / method-ref / user closure-returning call) is
    // TRANSFERRED (no retain) — its single reference flows into the field.
    // A borrow (a local / param / field read / identifier) is RETAINED so
    // the field's count is independent of the source's. The transfer vs retain
    // decision mirrors lowerStringFieldStore.isPlusOneStringProducer exactly,
    // but for closure-valued expressions.
    bool isProducer = isPlusOneClosureProducer(valueExpr) ||
                      isUserMethodClosureReturnCall(valueExpr);
    if (!isProducer)
        emitClosureRetain(newVal);
    builder_.CreateStore(newVal, fieldAddr);
    emitClosureRelease(oldVal);
    return true;
}


// #429 (box RETURN category) / §8.6 / §28.9: see the declaration in
// LlvmBackend_impl.hpp. Recognize a CallExpr whose resolved callee is a
// USER-DEFINED, LOCALLY-COMPILED MethodSymbol (backed by an ast::MethodDecl,
// not imported, not an operator) that appears in boxReturnMethods_ — i.e. a
// method whose declared return type is Object/interface AND whose body provably
// always returns a boxable value source (every return stmt carries a value of
// boxable type, so boxForTarget at the callee's return site always creates a
// fresh rc=1 box). Such a call hands back EXACTLY +1: the fresh box the callee
// created. The caller is the +1 owner. Cross-module imported methods are
// excluded (their callee does NOT run THIS module's boxForTarget). Parallel to
// isUserMethodClosureReturnCall for closures.
bool Emitter::isUserMethodBoxReturnCall(const ast::Expr* init) const {
    auto* call = dynamic_cast<const ast::CallExpr*>(init);
    if (!call || !call->callee) return false;
    auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(*call->callee));
    if (!ms) return false;
    if (ms->isOperator) return false;
    if (ctx_.isImported(ms)) return false;
    return boxReturnMethods_.count(ms) != 0;
}


// #429 (box RETURN + FIELD-STORE) / §8.6 / §28.9: see the declaration in
// LlvmBackend_impl.hpp. A target expression is a box-storage-eligible
// Object/interface-typed class instance field / auto-property lvalue iff:
//   - its resolved type is Object or an interface (isObjectOrInterfaceTarget),
//   - the valueExpr's static type is a boxable VALUE type or nullable value
//     (isBoxableValueType / isNullableValueTy) — i.e. boxForTarget will/has
//     created a fresh box for it; a class-reference source type is excluded,
//   - it is NOT a plain local / parameter slot (localSlot returns non-null),
//   - it is NOT a ref/out alias (refParamAddr returns non-null),
//   - it is NOT a program-lifetime static field (staticFieldGlobal non-null),
//   - its symbol is a FieldSymbol or auto-property of a CLASS (not a record).
// Parallel to isClosureFieldLvalue for closures.
bool Emitter::isBoxFieldLvalue(const ast::Expr& target,
                               const ast::Expr* valueExpr) {
    // The target type must be Object or an interface.
    const sema::TypeRef* rt = ctx_.exprType(target);
    if (!rt || !isObjectOrInterfaceTarget(rt)) return false;
    // The source expression must be a boxable value type (not a class ref).
    // A class reference assigned to an Object field has a reference source type
    // → isBoxableValueType / isNullableValueTy returns false → excluded here,
    // preventing bz_box_retain/release on a class instance.
    if (!valueExpr) return false;
    const sema::TypeRef* srcTy = ctx_.exprType(*valueExpr);
    if (!srcTy) return false;
    if (!isBoxableValueType(srcTy) && !isNullableValueTy(srcTy)) {
        // Also accept a call to a box-return method: its callee always produces
        // a fresh box (see isUserMethodBoxReturnCall). The source type is
        // Object/interface (the callee's return type), not itself boxable, but
        // the value IS a box pointer from the callee's boxForTarget.
        if (!isUserMethodBoxReturnCall(valueExpr)) return false;
    }
    // Exclude locals, ref params, and static fields.
    if (localSlot(target)) return false;
    {
        llvm::Type* dummy = nullptr;
        if (refParamAddr(target, dummy)) return false;
    }
    if (staticFieldGlobal(target)) return false;
    // Must be a class instance field or auto-property (not record, not static).
    sema::Symbol* sym = ctx_.exprSymbol(target);
    if (!sym) return false;
    if (auto* fs = dynamic_cast<sema::FieldSymbol*>(sym))
        return fs->parent && fs->parent->kind == ast::TypeDeclKind::Class;
    if (isAutoProperty(sym)) {
        auto* ms = dynamic_cast<sema::MemberSymbol*>(sym);
        return ms && ms->parent && ms->parent->kind == ast::TypeDeclKind::Class;
    }
    return false;
}


// #429 (box FIELD-STORE) / §8.6 / §28.9: see the declaration in
// LlvmBackend_impl.hpp. Store `newVal` (a box pointer after boxForTarget or
// a box from a box-return method call) into the class Object/interface field
// at `fieldAddr` with retain-new / release-old, giving the field an INDEPENDENT
// +1. Order: load-old → retain-or-transfer-new → store-new → release-old.
// Retaining the new value BEFORE releasing the old makes a self-assignment
// safe (rc dips +1 then back, never transiently 0). A +1 producer (a freshly-
// boxed value, i.e. the source had a boxable value type, or a call to a box-
// return method) is TRANSFERRED (no retain — its single reference becomes the
// field's); anything else is RETAINED via bz_box_retain. bz_box_retain/release
// are null-safe (no-op on null). Also records the field in boxStorageFields_ so
// emitDisposerCore knows to release it. Returns true when handled.
// Parallel to lowerClosureFieldStore for closures.
bool Emitter::lowerBoxFieldStore(const ast::Expr& target,
                                 llvm::Value* fieldAddr,
                                 llvm::Value* newVal,
                                 const ast::Expr* valueExpr) {
    if (!isBoxFieldLvalue(target, valueExpr)) return false;
    if (!fieldAddr || !newVal) return false;
    if (!newVal->getType()->isPointerTy()) return false;
    llvm::Value* oldVal = builder_.CreateLoad(ptrTy(), fieldAddr, "boxf.old");
    // A +1 producer is TRANSFERRED (no retain) — its single reference flows
    // into the field. A borrow is RETAINED so the field's count is independent.
    // Producer: a freshly-boxed value source (boxable value type → boxForTarget
    // just created the box) OR a call to a box-return method (callee's box).
    const sema::TypeRef* srcTy = valueExpr ? ctx_.exprType(*valueExpr) : nullptr;
    bool isProducer =
        (srcTy && (isBoxableValueType(srcTy) || isNullableValueTy(srcTy))) ||
        isUserMethodBoxReturnCall(valueExpr);
    if (!isProducer)
        emitBoxRetain(newVal);
    builder_.CreateStore(newVal, fieldAddr);
    emitBoxRelease(oldVal);
    // Record this field as a box storage field so the disposer releases it.
    // Safety: isBoxFieldLvalue already verified the source is boxable; a class-
    // reference source cannot reach here. If the field was previously added via
    // another store (same field, same class, same box-only pattern), it stays
    // in the set — the pre-pass collectBoxStorageFields ensures no class-ref
    // stores reach this path.
    sema::Symbol* sym = ctx_.exprSymbol(target);
    if (sym) boxStorageFields_.insert(static_cast<const void*>(sym));
    return true;
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
    // +1 closure producer: either a codegen-internal producer (a lambda /
    // method reference — isPlusOneClosureProducer) or a call to a
    // user-defined, locally-compiled method whose declared return type is a
    // function type (#429 RETURN category — isUserMethodClosureReturnCall;
    // the callee mints exactly +1 on return, so the caller's local transfers
    // that single reference and must release it at scope end iff non-escaping).
    // The escape walk decides which candidates are non-escaping. (Function-typed
    // locals never appear in `using var` — §28.9 — but the isUsing guard
    // mirrors the string path defensively.)
    std::vector<const ast::LocalDecl*> candidates;
    std::function<void(const ast::Stmt*)> scan = [&](const ast::Stmt* s) {
        if (!s) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            if (!ld->isUsing &&
                (isPlusOneClosureProducer(ld->initializer.get()) ||
                 isUserMethodClosureReturnCall(ld->initializer.get())))
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
    stringBorrowRetainEligible_.clear();
    stringBorrowAll_.clear();
    // Find every `var` (non-using) string LocalDecl, classified as a +1 PRODUCER
    // (`+`/interpolation/range-slice) or a BORROW (any other string initializer:
    // a field/param/call/identifier read). Run the escape walk for each; a decl
    // that the walk proves non-escaping is eligible for a scope-end release —
    // producers TRANSFER (no retain), borrows RETAIN at bind (#429). The two
    // eligibility sets are disjoint by construction (producer vs borrow).
    std::vector<const ast::LocalDecl*> producerCandidates;
    std::vector<const ast::LocalDecl*> borrowCandidates;
    std::function<void(const ast::Stmt*)> scan = [&](const ast::Stmt* s) {
        if (!s) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            if (!ld->isUsing && ld->initializer) {
                if (isPlusOneStringProducer(ld->initializer.get())) {
                    producerCandidates.push_back(ld);
                } else {
                    // #429: a BORROW-initialized string local. Recognized by the
                    // local's resolved type being `string` (non-nullable; a
                    // string? borrow is left alone — its null/sentinel handling
                    // is unchanged and retain/release are null-safe anyway, but
                    // we keep the conservative scope to non-nullable string
                    // borrows for this increment). The symbol type comes from the
                    // initializer's exprType (inference) or the declared type.
                    const sema::TypeRef* lt = nullptr;
                    if (ld->type) lt = ctx_.resolvedType(*ld->type);
                    if (!lt && ld->initializer)
                        lt = ctx_.exprType(*ld->initializer);
                    if (lt && !lt->nullable &&
                        lt->kind == sema::TypeRef::Kind::Primitive &&
                        lt->primitive == sema::PrimitiveType::String) {
                        borrowCandidates.push_back(ld);
                        stringBorrowAll_.insert(static_cast<const void*>(ld));
                    }
                }
            }
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

    for (const ast::LocalDecl* ld : producerCandidates) {
        StringEscapeWalker w{ctx_, static_cast<const void*>(ld)};
        for (auto& s : body) { w.stmt(s.get()); if (w.escaped) break; }
        if (!w.escaped)
            stringReleaseEligible_.insert(static_cast<const void*>(ld));
    }
    // #429: a BORROW-bound string local is eligible for retain-at-bind +
    // scope-end release ONLY if the SAME conservative escape walk proves it never
    // escapes. The walk is the identical over-approximation used for producers
    // (any non-safe-read use ⇒ escaped ⇒ excluded ⇒ no release), so releasing an
    // eligible borrow local's retained +1 at scope end is provably free of UAF
    // (no escaping alias exists past the scope) and of double-free (once per
    // exit). An ESCAPING borrow local is still RETAINED (in armStringLocalRefcount)
    // but NOT released — its +1 leaks, which is safe.
    for (const ast::LocalDecl* ld : borrowCandidates) {
        StringEscapeWalker w{ctx_, static_cast<const void*>(ld)};
        for (auto& s : body) { w.stmt(s.get()); if (w.escaped) break; }
        if (!w.escaped)
            stringBorrowRetainEligible_.insert(static_cast<const void*>(ld));
    }
}


// #422 / #429 (box RETURN) / §8.6 / §28.9: is local `ld` a codegen-internal
// +1 box producer? It is when the declared target type is Object or an
// interface AND either:
//   (a) the initializer's static type is a boxable VALUE type
//       (record/enum/primitive) or a nullable value type — i.e. exactly the
//       cases boxForTarget/boxValue allocate a fresh rc=1 box wrapper for; OR
//   (b) the initializer is a call to a locally-compiled method in
//       boxReturnMethods_ (a method that ALWAYS returns a boxable value and
//       therefore ALWAYS triggers boxForTarget at its callee return site),
//       so the caller receives a fresh rc=1 box from the callee.
// A class instance coerced to Object (the #399 path) has a REFERENCE
// initializer (class/interface/string/Object) for which boxForTarget is a
// no-op — NOT a producer, never released, so the class model is untouched.
// An INFERRED local (no `: Type`) is not a producer either: without a declared
// Object/interface target boxForTarget never boxes (the local takes the value
// type directly). Mirrors the boxForTarget gate in lowerLocalDecl exactly.
bool Emitter::isPlusOneBoxProducer(const ast::LocalDecl* ld) {
    if (!ld || !ld->type || !ld->initializer) return false;
    const sema::TypeRef* declTy = ctx_.resolvedType(*ld->type);
    if (!isObjectOrInterfaceTarget(declTy)) return false;
    const sema::TypeRef* srcTy = ctx_.exprType(*ld->initializer);
    if (!srcTy) return false;
    // Case (a): the initializer itself is a boxable value source.
    if (isBoxableValueType(srcTy) || isNullableValueTy(srcTy)) return true;
    // Case (b): a call to a box-return method (always returns a fresh box).
    if (isUserMethodBoxReturnCall(ld->initializer.get())) return true;
    return false;
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


// #429 / §27.5: compute cellFreeEligible_ for the function body.
//
// A captured local/param's CELL is safe to free at outer-scope exit iff
// EVERY lambda in the function body that captures it is proven non-escaping
// (in closureReleaseEligible_).  An escaping lambda's env outlives the scope
// and holds a raw pointer into the cell; freeing the cell while that env
// lives is a UAF.  A non-escaping lambda's env is guaranteed to be released
// BEFORE scope exit (via the closureRelease disposal), so the cell is dead.
//
// Algorithm:
//   1. Walk the entire function body and collect ALL lambda expressions,
//      classifying each as "non-escaping" or "escaping".
//      A lambda is non-escaping iff its direct owner `var` LocalDecl is in
//      closureReleaseEligible_.  Any lambda NOT directly assigned to a `var`
//      local (e.g. passed as an argument, stored in a collection, returned
//      inline) is CONSERVATIVELY treated as escaping.
//   2. For each lambda, collect its captured outer locals/params.
//   3. A captured entity is in cellFreeEligible_ iff:
//        (a) at least one lambda captures it, AND
//        (b) NO escaping lambda captures it.
//
// This is a whole-body pre-pass; it runs after collectClosureReleaseEligible
// so closureReleaseEligible_ is fully populated.
void Emitter::collectCellFreeEligible(
        const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    cellFreeEligible_.clear();

    // Sets to accumulate captured entities per classification.
    std::unordered_set<const void*> capturedByNonEscaping; // LocalDecl* / Symbol*
    std::unordered_set<const void*> capturedByEscaping;    // same keys

    // Walk ALL expressions in the function body looking for LambdaExpr nodes.
    // We use two walkers:
    //   (a) scanStmt: descends through statements to find lambda initializers
    //       of `var` LocalDecls (whose owner is in closureReleaseEligible_).
    //   (b) For any LambdaExpr found elsewhere (as an argument, collection
    //       element, etc.), treat it as escaping.
    //
    // To find ALL lambdas (not just top-level ones), we need a full recursive
    // expression walk. We reuse the CaptureWalker pattern.
    struct LambdaFinder {
        const sema::SemaContext& ctx;
        Emitter& em;
        const std::unordered_set<const void*>& closureNonEscaping; // closureReleaseEligible_
        std::unordered_set<const void*>& capturedByNonEscaping;
        std::unordered_set<const void*>& capturedByEscaping;

        // Track whether we're inside a "direct initializer" position for a
        // `var` local that is in closureReleaseEligible_. Only at the TOP
        // level (safePos=true) can a lambda be non-escaping.
        bool safePos = false;

        // Collect symbols referenced inside a lambda body via a lightweight
        // walk (mirrors CaptureWalker from LlvmBackend_Layout.cpp; duplicated
        // here because CaptureWalker lives in an anonymous namespace there and
        // is not visible in this translation unit).
        static void walkExpr(const sema::SemaContext& ctx,
                             const ast::Expr* e,
                             std::unordered_set<const sema::Symbol*>& out) {
            if (!e) return;
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e)) {
                if (auto* s = ctx.exprSymbol(*id)) out.insert(s);
                return;
            }
            if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
                walkExpr(ctx, b->left.get(), out);
                walkExpr(ctx, b->right.get(), out); return; }
            if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
                walkExpr(ctx, u->operand.get(), out); return; }
            if (auto* p = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
                walkExpr(ctx, p->operand.get(), out); return; }
            if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
                walkExpr(ctx, m->target.get(), out); return; }
            if (auto* m = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
                walkExpr(ctx, m->target.get(), out); return; }
            if (auto* c = dynamic_cast<const ast::CallExpr*>(e)) {
                walkExpr(ctx, c->callee.get(), out);
                for (auto& a : c->args) walkExpr(ctx, a.value.get(), out);
                return; }
            if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
                walkExpr(ctx, c->callee.get(), out);
                for (auto& a : c->args) walkExpr(ctx, a.value.get(), out);
                return; }
            if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
                walkExpr(ctx, ix->target.get(), out);
                for (auto& a : ix->indices) walkExpr(ctx, a.value.get(), out);
                return; }
            if (auto* t = dynamic_cast<const ast::TypeTestExpr*>(e)) {
                walkExpr(ctx, t->operand.get(), out); return; }
            if (auto* co = dynamic_cast<const ast::ConditionalExpr*>(e)) {
                walkExpr(ctx, co->condition.get(), out);
                walkExpr(ctx, co->thenValue.get(), out);
                walkExpr(ctx, co->elseValue.get(), out); return; }
            if (auto* th = dynamic_cast<const ast::ThrowExpr*>(e)) {
                walkExpr(ctx, th->operand.get(), out); return; }
            if (auto* r = dynamic_cast<const ast::RangeExpr*>(e)) {
                walkExpr(ctx, r->lo.get(), out);
                walkExpr(ctx, r->hi.get(), out);
                walkExpr(ctx, r->step.get(), out); return; }
            if (auto* n = dynamic_cast<const ast::NewExpr*>(e)) {
                for (auto& a : n->args) walkExpr(ctx, a.value.get(), out);
                return; }
            if (auto* w = dynamic_cast<const ast::WithExpr*>(e)) {
                walkExpr(ctx, w->source.get(), out);
                for (auto& a : w->assigns) walkExpr(ctx, a.value.get(), out);
                return; }
            if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
                for (auto& el : cl->elements) walkExpr(ctx, el.get(), out);
                for (auto& k : cl->keys) walkExpr(ctx, k.get(), out);
                return; }
            if (auto* is2 = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
                for (auto& part : is2->parts)
                    walkExpr(ctx, part.expr.get(), out); return; }
            if (auto* lam2 = dynamic_cast<const ast::LambdaExpr*>(e)) {
                // Recurse into nested lambdas to capture outer symbols.
                if (lam2->exprBody) walkExpr(ctx, lam2->exprBody.get(), out);
                for (auto& s : lam2->stmts) walkStmt(ctx, s.get(), out);
                return; }
            if (auto* ce2 = dynamic_cast<const ast::CaseExpr*>(e)) {
                walkExpr(ctx, ce2->scrutinee.get(), out);
                for (auto& arm : ce2->arms) {
                    walkExpr(ctx, arm.guard.get(), out);
                    walkExpr(ctx, arm.exprBody.get(), out); }
                return; }
        }
        static void walkStmt(const sema::SemaContext& ctx,
                             const ast::Stmt* s,
                             std::unordered_set<const sema::Symbol*>& out) {
            if (!s) return;
            if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
                if (ld->initializer) walkExpr(ctx, ld->initializer.get(), out);
                return; }
            if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
                if (as->target) walkExpr(ctx, as->target.get(), out);
                if (as->value) walkExpr(ctx, as->value.get(), out); return; }
            if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s)) {
                if (rs->value) walkExpr(ctx, rs->value.get(), out); return; }
            if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(s)) {
                if (ts->value) walkExpr(ctx, ts->value.get(), out); return; }
            if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s)) {
                if (es->expr) walkExpr(ctx, es->expr.get(), out); return; }
            if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
                walkExpr(ctx, is->ifBranch.condition.get(), out);
                for (auto& b : is->ifBranch.body) walkStmt(ctx, b.get(), out);
                for (auto& br : is->elifBranches) {
                    walkExpr(ctx, br.condition.get(), out);
                    for (auto& b : br.body) walkStmt(ctx, b.get(), out); }
                for (auto& b : is->elseBody) walkStmt(ctx, b.get(), out);
                return; }
            if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
                walkExpr(ctx, ws->condition.get(), out);
                for (auto& b : ws->body) walkStmt(ctx, b.get(), out); return; }
            if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
                if (fs->iterable) walkExpr(ctx, fs->iterable.get(), out);
                for (auto& b : fs->body) walkStmt(ctx, b.get(), out); return; }
            if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
                for (auto& b : d->body) walkStmt(ctx, b.get(), out); return; }
            if (auto* ts2 = dynamic_cast<const ast::TryStmt*>(s)) {
                for (auto& b : ts2->body) walkStmt(ctx, b.get(), out);
                for (auto& c : ts2->catches) {
                    walkExpr(ctx, c.guard.get(), out);
                    if (c.body) walkStmt(ctx, c.body.get(), out); }
                for (auto& b : ts2->finallyBody) walkStmt(ctx, b.get(), out);
                return; }
            if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
                walkExpr(ctx, cs->scrutinee.get(), out);
                for (auto& arm : cs->arms) {
                    walkExpr(ctx, arm.guard.get(), out);
                    walkExpr(ctx, arm.exprBody.get(), out);
                    for (auto& b : arm.stmts) walkStmt(ctx, b.get(), out); }
                return; }
        }

        void collectLambdaCaptures(const ast::LambdaExpr* lam,
                                   Emitter& /*em*/, bool isNonEscaping) {
            std::unordered_set<const sema::Symbol*> syms;
            if (lam->exprBody) walkExpr(ctx, lam->exprBody.get(), syms);
            for (auto& s : lam->stmts) walkStmt(ctx, s.get(), syms);
            if (isNonEscaping) {
                for (const sema::Symbol* s : syms) {
                    if (auto* loc = dynamic_cast<const sema::LocalSymbol*>(s)) {
                        if (loc->decl)
                            capturedByNonEscaping.insert(
                                static_cast<const void*>(loc->decl));
                    } else if (dynamic_cast<const sema::ParameterSymbol*>(s)) {
                        capturedByNonEscaping.insert(
                            static_cast<const void*>(s));
                    }
                }
            } else {
                for (const sema::Symbol* s : syms) {
                    if (auto* loc = dynamic_cast<const sema::LocalSymbol*>(s)) {
                        if (loc->decl)
                            capturedByEscaping.insert(
                                static_cast<const void*>(loc->decl));
                    } else if (dynamic_cast<const sema::ParameterSymbol*>(s)) {
                        capturedByEscaping.insert(
                            static_cast<const void*>(s));
                    }
                }
            }
            // Recursively scan the BODY of the lambda for nested lambdas —
            // they are all in an escaping context (from the outer scope's
            // perspective, any lambda inside another lambda's body captures
            // the outer local through the outer lambda's env, so the inner
            // lambda does NOT have a direct fresh cell from the outer scope).
            // We can safely treat nested lambdas as escaping from the
            // outer scope's perspective (they do not box the outer scope's
            // locals directly — the outer lambda already boxed them).
            // Nothing to do: the outer lambda's CaptureWalker already
            // collected all referenced outer symbols transitively.
        }

        void expr(const ast::Expr* e, bool topPos) {
            if (!e) return;
            if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(e)) {
                collectLambdaCaptures(lam, em, topPos);
                // Don't recurse into the body — captured syms already handled.
                return;
            }
            // For all other expressions, recurse without top-level position.
            if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
                expr(b->left.get(), false); expr(b->right.get(), false); return; }
            if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
                expr(u->operand.get(), false); return; }
            if (auto* p = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
                expr(p->operand.get(), false); return; }
            if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
                expr(m->target.get(), false); return; }
            if (auto* m = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
                expr(m->target.get(), false); return; }
            if (auto* c = dynamic_cast<const ast::CallExpr*>(e)) {
                expr(c->callee.get(), false);
                for (auto& a : c->args) expr(a.value.get(), false); return; }
            if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
                expr(c->callee.get(), false);
                for (auto& a : c->args) expr(a.value.get(), false); return; }
            if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
                expr(ix->target.get(), false);
                for (auto& a : ix->indices) expr(a.value.get(), false); return; }
            if (auto* t = dynamic_cast<const ast::TypeTestExpr*>(e)) {
                expr(t->operand.get(), false); return; }
            if (auto* co = dynamic_cast<const ast::ConditionalExpr*>(e)) {
                expr(co->condition.get(), false);
                expr(co->thenValue.get(), false);
                expr(co->elseValue.get(), false); return; }
            if (auto* th = dynamic_cast<const ast::ThrowExpr*>(e)) {
                expr(th->operand.get(), false); return; }
            if (auto* r = dynamic_cast<const ast::RangeExpr*>(e)) {
                expr(r->lo.get(), false); expr(r->hi.get(), false);
                expr(r->step.get(), false); return; }
            if (auto* n = dynamic_cast<const ast::NewExpr*>(e)) {
                for (auto& a : n->args) expr(a.value.get(), false); return; }
            if (auto* w = dynamic_cast<const ast::WithExpr*>(e)) {
                expr(w->source.get(), false);
                for (auto& a : w->assigns) expr(a.value.get(), false); return; }
            if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
                for (auto& el : cl->elements) expr(el.get(), false);
                for (auto& k : cl->keys) expr(k.get(), false); return; }
            if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
                for (auto& part : is->parts) expr(part.expr.get(), false);
                return; }
            if (auto* ce = dynamic_cast<const ast::CaseExpr*>(e)) {
                expr(ce->scrutinee.get(), false);
                for (auto& arm : ce->arms) {
                    expr(arm.guard.get(), false);
                    expr(arm.exprBody.get(), false); } return; }
            // Literals, identifiers, null: no lambdas.
        }

        void stmt(const ast::Stmt* s) {
            if (!s) return;
            // A `var f := lambda` where f is in closureReleaseEligible_:
            // the lambda is in "non-escaping" top-level position.
            if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
                bool nonEsc = !ld->isUsing &&
                    closureNonEscaping.count(static_cast<const void*>(ld));
                if (ld->initializer)
                    expr(ld->initializer.get(), nonEsc);
                return;
            }
            if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
                if (as->value) expr(as->value.get(), false);
                if (as->target) expr(as->target.get(), false); return; }
            if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s)) {
                if (rs->value) expr(rs->value.get(), false); return; }
            if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(s)) {
                if (ts->value) expr(ts->value.get(), false); return; }
            if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s)) {
                if (es->expr) expr(es->expr.get(), false); return; }
            if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
                if (is->ifBranch.condition)
                    expr(is->ifBranch.condition.get(), false);
                for (auto& b : is->ifBranch.body) stmt(b.get());
                for (auto& br : is->elifBranches) {
                    if (br.condition) expr(br.condition.get(), false);
                    for (auto& b : br.body) stmt(b.get()); }
                for (auto& b : is->elseBody) stmt(b.get()); return; }
            if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
                if (ws->condition) expr(ws->condition.get(), false);
                for (auto& b : ws->body) stmt(b.get()); return; }
            if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
                if (fs->iterable) expr(fs->iterable.get(), false);
                for (auto& b : fs->body) stmt(b.get()); return; }
            if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
                for (auto& b : d->body) stmt(b.get()); return; }
            if (auto* ts2 = dynamic_cast<const ast::TryStmt*>(s)) {
                for (auto& b : ts2->body) stmt(b.get());
                for (auto& c : ts2->catches) {
                    if (c.guard) expr(c.guard.get(), false);
                    if (c.body) stmt(c.body.get()); }
                for (auto& b : ts2->finallyBody) stmt(b.get()); return; }
            if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
                if (cs->scrutinee) expr(cs->scrutinee.get(), false);
                for (auto& arm : cs->arms) {
                    if (arm.guard) expr(arm.guard.get(), false);
                    if (arm.exprBody) expr(arm.exprBody.get(), false);
                    for (auto& b : arm.stmts) stmt(b.get()); }
                return; }
        }
    };

    LambdaFinder finder{ctx_, *this, closureReleaseEligible_,
                        capturedByNonEscaping, capturedByEscaping};
    for (auto& s : body) finder.stmt(s.get());

    // A captured entity qualifies iff:
    //   - it appears in capturedByNonEscaping (at least one non-escaping lambda
    //     captures it → it was boxed and should be freed), AND
    //   - it does NOT appear in capturedByEscaping (no escaping lambda holds
    //     a cell pointer that would outlive the scope).
    for (const void* key : capturedByNonEscaping) {
        if (!capturedByEscaping.count(key))
            cellFreeEligible_.insert(key);
    }
}


// #429 (box RETURN category) / §8.6 / §28.9: see the declaration in
// LlvmBackend_impl.hpp. Scan all locally-compiled method bodies in methodFns_.
// A MethodSymbol is added to boxReturnMethods_ iff its declared return type is
// Object or an interface, it is not imported, and every ReturnStmt in the body
// carries a value whose STATIC TYPE (ctx_.exprType) is a boxable value type or
// nullable value type. An arrow-body method counts iff its expression body has
// a boxable type. A method with no ReturnStmt (empty body or fall-off) is not
// added (it has no returns to count). This pre-pass runs BEFORE any method body
// is lowered, so all isUserMethodBoxReturnCall queries during per-body
// collectBoxReleaseEligible see a fully-populated set.
void Emitter::collectBoxReturnMethods() {
    // Recursive statement scanner: sets `allBox` to false if any ReturnStmt
    // returns a non-boxable value, and sets `hasReturn` to true when any
    // ReturnStmt with a value is found.
    struct Scanner {
        const sema::SemaContext& ctx;
        Emitter& em;
        bool allBox = true;
        bool hasReturn = false;

        void scanStmts(const std::vector<std::unique_ptr<ast::Stmt>>& stmts) {
            for (auto& s : stmts) scanStmt(s.get());
        }
        void scanStmt(const ast::Stmt* s) {
            if (!s || !allBox) return;
            if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s)) {
                if (rs->value) {
                    hasReturn = true;
                    const sema::TypeRef* srcTy = ctx.exprType(*rs->value);
                    if (!srcTy ||
                        (!em.isBoxableValueType(srcTy) &&
                         !em.isNullableValueTy(srcTy))) {
                        allBox = false;
                    }
                }
                return;
            }
            if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
                scanStmts(is->ifBranch.body);
                for (auto& br : is->elifBranches) scanStmts(br.body);
                scanStmts(is->elseBody);
            } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
                scanStmts(ws->body);
            } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
                scanStmts(fs->body);
            } else if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
                scanStmts(d->body);
            } else if (auto* ts = dynamic_cast<const ast::TryStmt*>(s)) {
                scanStmts(ts->body);
                for (auto& c : ts->catches)
                    if (c.body) scanStmt(c.body.get());
                scanStmts(ts->finallyBody);
            } else if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
                for (auto& arm : cs->arms)
                    scanStmts(arm.stmts);
            }
        }
    };

    for (auto& [sym, fn] : methodFns_) {
        if (ctx_.isImported(sym)) continue;
        auto* md = dynamic_cast<const ast::MethodDecl*>(sym->decl);
        if (!md || !md->returnType) continue;
        const sema::TypeRef* rt = ctx_.resolvedType(*md->returnType);
        if (!rt || !isObjectOrInterfaceTarget(rt)) continue;

        if (md->bodyKind == ast::MethodBodyKind::Expression && md->exprBody) {
            // Arrow body: the body expression itself is the returned value.
            const sema::TypeRef* srcTy = ctx_.exprType(*md->exprBody);
            if (srcTy && (isBoxableValueType(srcTy) || isNullableValueTy(srcTy)))
                boxReturnMethods_.insert(sym);
        } else if (md->bodyKind == ast::MethodBodyKind::Block && !md->stmts.empty()) {
            Scanner sc{ctx_, *this};
            sc.scanStmts(md->stmts);
            if (sc.allBox && sc.hasReturn)
                boxReturnMethods_.insert(sym);
        }
        // Abstract or empty block body: not a box return method.
    }
}


// #429 (box FIELD-STORE / disposal) / §8.6 / §28.9: see the declaration in
// LlvmBackend_impl.hpp. For every class TypeSymbol, find all Object/interface-
// typed instance fields and auto-properties. Then scan EVERY method and
// constructor body of that class for AssignmentStmt stores (:=) to those
// fields. Additionally, check each field's own AST initializer (FieldDecl /
// PropertyDecl autoInitializer). A field is added to boxStorageFields_ ONLY if:
//   (a) every such store has a BOXABLE VALUE source type (isBoxableValueType or
//       isNullableValueTy on ctx_.exprType(*valueExpr)); OR
//   (b) the store is a call to a box-return method (isUserMethodBoxReturnCall) —
//       this is treated as a boxable source (the callee produced a fresh box).
// A field that receives ANY class-reference store (a reference source type) is
// excluded. This guarantees that bz_box_release in emitDisposerCore never fires
// for an Object field that might hold a class instance.
// NOTE: this pre-pass only runs the STATIC analysis. lowerBoxFieldStore also
// adds to boxStorageFields_ at emission time (for fields that pass isBoxFieldLvalue
// during lowering), but the pre-pass is the authoritative gate: a field is only
// a box storage field if the pre-pass confirmed all stores are box-valued.
void Emitter::collectBoxStorageFields() {
    // For each class, collect its Object/interface instance field symbols.
    for (auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (!ts || ts->kind != ast::TypeDeclKind::Class) continue;

        // Collect candidate fields: Object/interface typed instance fields.
        std::vector<const sema::Symbol*> candidates;
        for (const sema::Symbol* m : ts->members) {
            bool isField = dynamic_cast<const sema::FieldSymbol*>(m) != nullptr;
            if (!isField && !isAutoProperty(m)) continue;
            if (fieldIndex_.find(m) == fieldIndex_.end()) continue;
            const sema::TypeRef* ft = recordMemberType(m);
            if (!ft || !isObjectOrInterfaceTarget(ft)) continue;
            candidates.push_back(m);
        }
        if (candidates.empty()) continue;

        // Track which candidates are still "box only" (start as true).
        std::unordered_map<const void*, bool> ok;
        for (const sema::Symbol* m : candidates)
            ok[static_cast<const void*>(m)] = true;

        // Helper: check a store to an Object/interface field — if the source
        // type is NOT boxable (and not a box-return call), mark it non-box.
        auto checkStore = [&](const ast::Expr* target, const ast::Expr* val) {
            if (!target || !val) return;
            sema::Symbol* fsym = ctx_.exprSymbol(*target);
            if (!fsym) return;
            auto it = ok.find(static_cast<const void*>(fsym));
            if (it == ok.end()) return;  // not a candidate field
            if (!it->second) return;     // already excluded
            const sema::TypeRef* srcTy = ctx_.exprType(*val);
            if (!srcTy) { it->second = false; return; }
            bool isBoxableSource =
                isBoxableValueType(srcTy) || isNullableValueTy(srcTy);
            bool isBoxReturnCall = isUserMethodBoxReturnCall(val);
            if (!isBoxableSource && !isBoxReturnCall)
                it->second = false;
        };

        // Recursive statement scanner for stores.
        std::function<void(const ast::Stmt*)> scanStmt;
        scanStmt = [&](const ast::Stmt* s) {
            if (!s) return;
            if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
                if (as->op == ":=" && as->target && as->value)
                    checkStore(as->target.get(), as->value.get());
            }
            if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
                for (auto& b : is->ifBranch.body) scanStmt(b.get());
                for (auto& br : is->elifBranches)
                    for (auto& b : br.body) scanStmt(b.get());
                for (auto& b : is->elseBody) scanStmt(b.get());
            } else if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
                for (auto& b : ws->body) scanStmt(b.get());
            } else if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
                for (auto& b : fs->body) scanStmt(b.get());
            } else if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
                for (auto& b : d->body) scanStmt(b.get());
            } else if (auto* ts2 = dynamic_cast<const ast::TryStmt*>(s)) {
                for (auto& b : ts2->body) scanStmt(b.get());
                for (auto& c : ts2->catches)
                    if (c.body) scanStmt(c.body.get());
                for (auto& b : ts2->finallyBody) scanStmt(b.get());
            } else if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
                for (auto& arm : cs->arms)
                    for (auto& a : arm.stmts) scanStmt(a.get());
            }
        };

        // Check field initializers (AST FieldDecl / PropertyDecl initializer).
        for (const sema::Symbol* m : candidates) {
            const ast::Expr* init = nullptr;
            if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m->decl))
                init = fd->initializer.get();
            else if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m->decl))
                init = pd->autoInitializer.get();
            if (!init) continue;  // no initializer → calloc-zeroed null → OK
            // Synthesize a check: the "target" symbol is m, the "source" is
            // init. We check the init directly.
            auto it = ok.find(static_cast<const void*>(m));
            if (!it->second) continue;
            const sema::TypeRef* srcTy = ctx_.exprType(*init);
            if (!srcTy) { it->second = false; continue; }
            bool isBoxableSource =
                isBoxableValueType(srcTy) || isNullableValueTy(srcTy);
            bool isBoxReturnCall = isUserMethodBoxReturnCall(init);
            if (!isBoxableSource && !isBoxReturnCall)
                it->second = false;
        }

        // Check all method bodies for stores to these fields.
        for (auto& [msym, fn] : methodFns_) {
            if (ctx_.isImported(msym)) continue;
            auto* md = dynamic_cast<const ast::MethodDecl*>(msym->decl);
            if (!md) continue;
            // Only scan methods that belong to THIS class (ts).
            if (msym->parent != ts) continue;
            for (auto& s : md->stmts) scanStmt(s.get());
        }

        // Check all constructor bodies for stores to these fields.
        for (auto& [csym, fn] : ctorFns_) {
            if (ctx_.isImported(csym)) continue;
            auto* cd = dynamic_cast<const ast::ConstructorDecl*>(csym->decl);
            if (!cd) continue;
            if (csym->parent != ts) continue;
            for (auto& s : cd->body) scanStmt(s.get());
        }

        // Also scan the class's destructor if any.
        for (const sema::Symbol* m : ts->members) {
            auto* ds = dynamic_cast<const sema::MethodSymbol*>(m);
            if (!ds) continue;
            auto* dd = dynamic_cast<const ast::DestructorDecl*>(ds->decl);
            if (!dd) continue;
            for (auto& s : dd->body) scanStmt(s.get());
        }

        // Add fields that survived as box-only to boxStorageFields_.
        for (auto& [voidPtr, isOk] : ok) {
            if (isOk) boxStorageFields_.insert(voidPtr);
        }
    }
}


// #383 / §28: a reference TypeRef carrying a vtable — a user class or
// interface (Named, with a class/interface declaration). Such objects are
// disposed dynamically through their own vtable slot 2 (CG35-itable).
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
                // (b1) User method with returnOwned: the MethodSymbol exists and
                // its MethodDecl declares `owned` on the return.
                // §28.10 / #707: for imported methods ms->decl is null (the
                // ModuleLoader attaches no AST MethodDecl); the owned-return
                // flag lives in SemaContext::importedReturnOwned_.  Fall back
                // to that when md is absent — but ONLY when importedReturnOwned
                // is true (a borrow return must NOT be disposed).
                auto* ms = dynamic_cast<sema::MethodSymbol*>(
                    ctx_.exprSymbol(*call->callee));
                if (ms) {
                    auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
                    bool isOwned = (md && md->returnOwned) ||
                                   (!md && ctx_.importedReturnOwned(ms));
                    if (isOwned &&
                        isClassOrInterfaceRef(ctx_.exprType(*ld->initializer)))
                        return true;
                }
                // (b2) fix #642/#643: builtin collection Pop* methods (PopLast,
                // PopFirst, PopAt, PopValue) transfer ownership of a class element
                // to the caller (§4.5/§4.7/§4.9).  These are BUILTIN methods —
                // sema never sets an exprSymbol on their MemberAccessExpr callee
                // (ctx_.exprSymbol returns null), so (b1) above misses them.
                // Detect by the method name on the callee MemberAccessExpr when
                // the sema result type is a class/interface with a registered
                // disposer (all local classes + imported public classes are in
                // disposerFns_; the guard catches any gap in e.g. a compiler
                // extension that synthesises a class without the normal
                // declaration pipeline).  The dynamicDispose path in emitDisposals
                // dispatches through vtable slot 2 (Dispose) — declareVtables Pass
                // D installs the compiler-generated `bz$T$Dispose` for EVERY local
                // class (including those with no user destructor), so slot 2 is
                // always non-null for a class that's in disposerFns_.
                if (isClassOrInterfaceRef(ctx_.exprType(*ld->initializer))) {
                    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                        call->callee.get());
                    if (ma) {
                        auto nm = ma->member;
                        // §2.14 / #550: T.Deserialize(d) is the synthesized
                        // (or hand-written conforming) factory returning
                        // `owned T` by contract — sema sets no exprSymbol on
                        // the synthesized form, so (b1) misses it and the
                        // deserialized tree leaked under `using var` (found by
                        // the t2/t3 ASan round-trip probes, 2026-06-12).
                        if (nm == "PopLast" || nm == "PopFirst" ||
                            nm == "PopAt"   || nm == "PopValue" ||
                            nm == "Deserialize") {
                            // Only register if the element class has a disposer
                            // (vtable slot 2 non-null).  Interface-typed results
                            // always dispatch dynamically and always have a vtable,
                            // so treat them as having a disposer too.
                            const sema::TypeRef* eTy =
                                ctx_.exprType(*ld->initializer);
                            if (eTy && eTy->declaration) {
                                bool hasDisposer =
                                    disposerFns_.find(eTy->declaration) !=
                                    disposerFns_.end();
                                bool isIface =
                                    eTy->declaration->kind ==
                                    ast::TypeDeclKind::Interface;
                                if (hasDisposer || isIface)
                                    return true;
                            }
                        }
                    }
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
    // be reclaimed at scope end (dispatch is dynamic, vtable slot 2, CG35-itable).
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
    const sema::TypeRef* rt = ctx_.exprType(*ld->initializer);
    if (isClassOrInterfaceRef(rt)) return true;
    // #572 / §28.3 / §14.1: `using var x := move item` where `item` has a
    // generic-type-parameter type (declaration == nullptr, single-part name).
    // The raw TypeRef has no declaration so isClassOrInterfaceRef returns false.
    // Resolve through genericSubstSemaTy_ to the concrete substituted TypeRef
    // and test it instead.  Ownership has already transferred from `item` to
    // `x` via move (§28.8.1); registering disposal on `x` only, not on `item`,
    // avoids double-dispose.
    if (rt && rt->kind == sema::TypeRef::Kind::Named &&
        rt->declaration == nullptr && !genericSubstSemaTy_.empty() &&
        rt->astNode && rt->astNode->name &&
        rt->astNode->name->parts.size() == 1) {
        auto it = genericSubstSemaTy_.find(rt->astNode->name->parts[0]);
        if (it != genericSubstSemaTy_.end())
            return isClassOrInterfaceRef(it->second);
    }
    return false;
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


// #465 / §15.4 / stdlib §4.4–§4.5: materialize a sequence collection literal
// `[e1, …, en]` into a fresh heap collection whose target type is an array-backed
// heap collection (Array<T> / OwnedArray<T> / Stack / OwnedStack / Queue /
// OwnedQueue). Reuses the SAME runtime construction the `new Array<T>()` path
// emits (bz_array_alloc) and the SAME element store the `.Add(...)` path emits
// (bz_array_add) — so a literal-built array is byte-for-byte identical to one
// built by `new` + `.Add`. Textual element order maps to the array's natural
// construction order (§15.4): Array/OwnedArray index 0 = first element; Stack
// last element on top; Queue first element at the front — all achieved by adding
// in textual order to the bz_array_t (which append-stores), matching each type's
// runtime Pop/Dequeue contract. Set/OwnedSet are NOT handled here (they are
// hash-backed with dedup, allocated via bz_dict_alloc_generic — a separate
// path); this helper returns null for them so the caller can fall back.
llvm::Value* Emitter::materializeHeapCollectionLiteral(
        const ast::CollectionLiteralExpr* cl, const sema::TypeRef* collTy) {
    if (!cl || cl->isDict || !collTy) return nullptr;
    if (!isCollectionTypeName(collTy)) return nullptr;
    // Dispatch Set/OwnedSet to the dedicated hash-backed materializer (they use
    // bz_dict_alloc_generic + bz_dict_set_gen, not the array path below).
    std::string_view name = collTy->astNode && collTy->astNode->name &&
                            !collTy->astNode->name->parts.empty()
                                ? collTy->astNode->name->parts[0]
                                : std::string_view{};
    if (name == "Set" || name == "OwnedSet")
        return materializeHeapSetLiteral(cl, collTy);

    // Element type + byte size, computed exactly as the `new` / `.Add` paths do
    // (collectionElemSize reads genericArgs[0]); both sides agree on the layout.
    const sema::TypeRef* elemRef =
        (!collTy->genericArgs.empty()) ? collTy->genericArgs[0] : nullptr;
    llvm::Type* elemLlvmTy = elemRef ? lowerTypeRef(elemRef) : i32();
    int64_t esz = collectionElemSize(collTy);

    // Allocate the bz_array_t (same call lowerNew emits for `new Array<T>()`).
    // Capacity is sized to the literal length (min 16, matching the default).
    int64_t cap = std::max<int64_t>(16, (int64_t)cl->elements.size());
    auto* allocFn = declareC("bz_array_alloc", ptrTy(), {i64(), i64()});
    llvm::Value* arr = builder_.CreateCall(
        allocFn, {builder_.getInt64(esz), builder_.getInt64(cap)}, "clit.arr");

    // Store each element in textual order via bz_array_add (same as `.Add`).
    auto* addFn = declareC("bz_array_add", llvm::Type::getVoidTy(llvm_),
                           {ptrTy(), ptrTy(), i64()});
    for (const auto& elemPtr : cl->elements) {
        if (!elemPtr) continue;
        llvm::Value* ev = nullptr;
        // #465 / §15.4: a NESTED sequence literal element (`Array<Array<int>>`
        // := [[1,2],[3,4]]) — lowerExpr returns null for a CollectionLiteralExpr,
        // so materialize the inner heap collection here (recursively) against the
        // outer element type (genericArgs[0]). The inner array is a fresh owned
        // heap object stored (as a ptr) into the outer's slot, exactly as an
        // `inner` named local added via `.Add(inner)` would be.
        if (auto* nested =
                dynamic_cast<const ast::CollectionLiteralExpr*>(elemPtr.get())) {
            if (!nested->isDict && elemRef)
                ev = materializeHeapCollectionLiteral(nested, elemRef);
        }
        if (!ev) ev = lowerExpr(*elemPtr);
        if (!ev) continue;
        // fix #455 parity: a nullable VALUE element (int?/bool?/enum?/record?)
        // may lower to its inner value; lift it to the tagged {hasValue, T}
        // struct the runtime stores (matching esz).
        if (elemRef && isNullableValueTy(elemRef)) {
            llvm::StructType* expected = nullableValueStructTy(elemRef);
            if (expected && ev->getType() != expected) {
                ev = ev->getType()->isPointerTy() ? nullableValueNull(elemRef)
                                                  : wrapNullableValue(elemRef, ev);
            }
        } else if (ev->getType() != elemLlvmTy) {
            // §5.1: widen/narrow a numeric element to the declared element type
            // (e.g. `Array<long> := [1, 2]` — i32 literals → i64 slots), honoring
            // the element expression's signedness. Mirrors lowerLocalDecl's
            // inline-array element coercion; a no-op for already-matching types.
            ev = coerceNumericToType(ev, elemLlvmTy,
                                     isUnsignedTargetExpr(*elemPtr));
        }
        // #429 / §4.6 / §28.9: an Array<string> built from a literal co-owns a +1
        // per element, exactly as the `.Add(...)` path does (transfer a +1
        // producer, retain anything else) — so a literal-built array disposed at
        // scope end releases each element symmetrically (no leak, no premature
        // free of a borrowed element).
        // #602 / §28.11 v0.135: derive string-element classification from
        // elementPolicyFor (RuntimeManaged + string primitive + non-owning
        // array-backed collection).
        {
            bool litIsStrArr =
                elemRef && !elemRef->nullable &&
                elemRef->kind == sema::TypeRef::Kind::Primitive &&
                elemRef->primitive == sema::PrimitiveType::String &&
                elementPolicyFor(elemRef, /*owning=*/false).cat ==
                    ElementPolicy::Category::RuntimeManaged &&
                (name == "Array" || name == "Stack" || name == "Queue");
            if (litIsStrArr && !isPlusOneStringProducer(elemPtr.get()))
                emitStringRetain(ev);
        }
        llvm::AllocaInst* slot = entryAlloca(ev->getType());
        builder_.CreateStore(ev, slot);
        builder_.CreateCall(addFn, {arr, slot, builder_.getInt64(esz)});
    }
    return arr;
}


// #559 / stdlib §4.10: materialize a Set<T>/OwnedSet<T> sequence literal
// `[e1, e2, …]` into a freshly-allocated Set<T>.
//
// Strategy: reuse EXACTLY the same allocation and per-element insertion codegen
// as the working `new Set<T>()` + `.Add(e)` paths (lowerNew + the Set branch
// of tryLowerCollectionInstanceCall) so the resulting object is bit-for-bit
// identical to one built with explicit Add calls.
//
//   1. Allocate via bz_dict_alloc_generic (same as lowerNew Set<T> branch) —
//      parameterised by key byte-size + hash/eq trampolines for T.
//   2. For each element in textual order: lower the sub-expression, spill it to
//      a stack slot, and call bz_dict_set_gen with sentinel value 1 — mirroring
//      the Set.Add branch of tryLowerCollectionInstanceCall.  Duplicate keys are
//      silently collapsed (the dict stores the first occurrence; a subsequent
//      bz_dict_set_gen on an equal key is a no-op), giving the dedup semantics
//      §4.10 / stdlib §4.10 requires.
//
// #429 / §28.9 ownership: for Set<string>, each element is co-owned (retained
// at insert); a +1 producer is transferred, a borrow/literal is retained —
// exactly as Set.Add handles it in tryLowerCollectionInstanceCall.
//
// Returns a `ptr` to the bz_dict_t.  Returns null when `setTy` is not a
// Set/OwnedSet TypeRef with a recognised element type (caller then falls
// through to the inline-array path, which is incorrect for Sets — but the
// lowerLocalDecl guard already prevents reaching this function for non-Set
// targets, so null here means "unrecognized element", which is a sema error).
llvm::Value* Emitter::materializeHeapSetLiteral(
        const ast::CollectionLiteralExpr* cl, const sema::TypeRef* setTy) {
    if (!cl || cl->isDict || !setTy) return nullptr;
    // Require a known element type; genericBackingType returns non-null for every
    // element type the generic-dict runtime supports (all primitives, enums,
    // records, strings — §4.10 / isSet=true path).
    uint64_t ksz = 0;
    const sema::TypeRef* elemTy =
        genericBackingType(setTy, 0, /*isSet=*/true, &ksz);
    if (!elemTy) return nullptr;

    // --- 1. Allocate the backing hash table (same call as lowerNew Set<T>). ---
    llvm::Function* hashT = buildValueHashTrampoline(elemTy);
    llvm::Function* eqT   = buildValueEqTrampoline(elemTy);
    auto* allocFn = declareC("bz_dict_alloc_generic", ptrTy(),
                             {i64(), ptrTy(), ptrTy()});
    llvm::Value* tbl = builder_.CreateCall(
        allocFn, {builder_.getInt64((int64_t)ksz), hashT, eqT}, "slit.tbl");

    // Sentinel non-null value stored for present elements (same as Set.Add).
    llvm::Value* sentinel = builder_.CreateIntToPtr(builder_.getInt64(1), ptrTy());

    bool elemIsRecord = recordTypeOfRef(elemTy) != nullptr;
    // #429 / §28.9: Set<string> co-owns a +1 per element.
    // #602 / §28.11 v0.135: derive string-element classification from
    // elementPolicyFor (RuntimeManaged + string primitive + non-owning Set).
    bool isStrSet = !elemTy->nullable &&
        elemTy->kind == sema::TypeRef::Kind::Primitive &&
        elemTy->primitive == sema::PrimitiveType::String &&
        elementPolicyFor(elemTy, /*owning=*/false).cat ==
            ElementPolicy::Category::RuntimeManaged;

    auto* setFn = declareC("bz_dict_set_gen", llvm::Type::getVoidTy(llvm_),
                           {ptrTy(), ptrTy(), ptrTy()});

    // --- 2. Insert each element in textual order (duplicates collapsed). -----
    for (const auto& elemPtr : cl->elements) {
        if (!elemPtr) continue;
        llvm::Value* ev = lowerExpr(*elemPtr);
        if (!ev) continue;
        // #429 / §28.9: retain the string element before inserting — the set
        // takes an INDEPENDENT +1.  A +1 producer is transferred (no retain);
        // a borrow / literal / identifier is retained.
        if (isStrSet && !isPlusOneStringProducer(elemPtr.get()))
            emitStringRetain(ev);
        // Spill to a stack slot and pass its byte address to the generic runtime
        // (mirrors the elemPtr lambda in the Set.Add branch of
        // tryLowerCollectionInstanceCall).
        llvm::Value* keyPtr;
        if (elemIsRecord && ev->getType()->isPointerTy()) {
            keyPtr = ev;  // record already spilled to an address
        } else {
            llvm::AllocaInst* slot = entryAlloca(ev->getType());
            builder_.CreateStore(ev, slot);
            keyPtr = slot;
        }
        builder_.CreateCall(setFn, {tbl, keyPtr, sentinel});
    }
    return tbl;
}


// #534 / stdlib §4.8: materialize a dictionary literal `["k1":v1, "k2":v2, …]`
// into a freshly-allocated Dictionary<K,V> (or OwnedDictionary<K,V>).
//
// Strategy: reuse EXACTLY the same allocation and per-entry insertion codegen
// as the working `new Dictionary<K,V>()` + `.Add(k, v)` paths (lowerNew +
// tryLowerCollectionInstanceCall), so the resulting object is bit-for-bit
// identical to one built with explicit Add calls.
//
//   1. Allocate via bz_dict_alloc (string keys) or bz_dict_alloc_generic
//      (generic keys) — mirroring lowerNew's Dictionary branch.
//   2. For each (key, value) pair: lower both sub-expressions, encode the value
//      via the same inttoptr/box logic, then call bz_dict_set_str or
//      bz_dict_set_gen — mirroring the .Add(k, v) branch.
//
// Returns a `ptr` to the bz_dict_t.  Returns null when `dictTy` is not a
// recognized Dictionary/OwnedDictionary TypeRef.
llvm::Value* Emitter::materializeHeapDictLiteral(
        const ast::CollectionLiteralExpr* cl, const sema::TypeRef* dictTy) {
    if (!cl || !cl->isDict || !dictTy) return nullptr;
    if (!isDictTypeName(dictTy)) return nullptr;
    if (cl->keys.size() != cl->elements.size()) return nullptr;  // malformed AST

    // Key type analysis (mirrors lowerNew Dictionary branch and .Add branch).
    // genericBackingType returns non-null for every non-string key (int/long/
    // bool/char/enum/record/float): use the generic-key runtime.  Null → string.
    const sema::TypeRef* keyTy =
        genericBackingType(dictTy, 0, /*isSet=*/false, nullptr);
    bool keyIsRecord = keyTy && recordTypeOfRef(keyTy) != nullptr;

    // Value type analysis (only valTy is needed now; encodeVal boxes all structs).
    const sema::TypeRef* valTy =
        (dictTy->genericArgs.size() > 1) ? dictTy->genericArgs[1] : nullptr;

    // --- 1. Allocate the backing hash table (same as lowerNew Dictionary). ---
    llvm::Value* dict = nullptr;
    if (keyTy) {
        // Generic-key table: parameterised by key byte-size + hash/eq trampolines.
        uint64_t ksz = 0;
        genericBackingType(dictTy, 0, /*isSet=*/false, &ksz);
        llvm::Function* hashT = buildValueHashTrampoline(keyTy);
        llvm::Function* eqT   = buildValueEqTrampoline(keyTy);
        auto* allocFn = declareC("bz_dict_alloc_generic", ptrTy(),
                                 {i64(), ptrTy(), ptrTy()});
        dict = builder_.CreateCall(allocFn,
            {builder_.getInt64((int64_t)ksz), hashT, eqT}, "dlit.tbl");
    } else {
        // String-key fast-path table.
        auto* allocFn = declareC("bz_dict_alloc", ptrTy(), {});
        dict = builder_.CreateCall(allocFn, {}, "dlit.tbl");
    }
    if (!dict) return nullptr;

    // --- 2. Insert each (key, value) pair (same as .Add(k, v)). -----------

    // Encode a scalar/float/ptr/record value into the void* slot the runtime
    // stores (mirrors the encodeValue lambda in tryLowerCollectionInstanceCall).
    auto encodeVal = [&](llvm::Value* v) -> llvm::Value* {
        // fix #685: any struct-typed value (record, nullable, or primitive struct
        // e.g. decimal) must be heap-boxed — too wide for the scalar inttoptr slot.
        if (v->getType()->isStructTy()) {
            uint64_t vsz = module_->getDataLayout().getTypeAllocSize(v->getType());
            auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
            llvm::Value* box =
                builder_.CreateCall(mallocFn, {builder_.getInt64((int64_t)vsz)});
            builder_.CreateStore(v, box);
            return box;
        }
        if (v->getType()->isPointerTy()) return v;
        // Scalar / float: widen to i64 then inttoptr so the value is
        // self-contained (no shared-alloca aliasing across loop iterations).
        llvm::Value* asI64 = v;
        if (v->getType()->isFloatingPointTy()) {
            if (v->getType()->isDoubleTy())
                asI64 = builder_.CreateBitCast(v, i64());
            else
                asI64 = builder_.CreateZExt(builder_.CreateBitCast(v, i32()), i64());
        } else if (!v->getType()->isIntegerTy(64)) {
            asI64 = builder_.CreateZExt(v, i64());
        }
        return builder_.CreateIntToPtr(asI64, ptrTy());
    };

    // Spill a generic key to a stack slot and return its byte pointer
    // (mirrors the genKeyPtr lambda in tryLowerCollectionInstanceCall).
    auto spillKey = [&](llvm::Value* k) -> llvm::Value* {
        if (keyIsRecord && k->getType()->isPointerTy())
            return k;  // record is already spilled to an address
        llvm::AllocaInst* slot = entryAlloca(k->getType());
        builder_.CreateStore(k, slot);
        return slot;
    };

    for (std::size_t i = 0; i < cl->keys.size(); ++i) {
        if (!cl->keys[i] || !cl->elements[i]) continue;
        llvm::Value* k = lowerExpr(*cl->keys[i]);
        llvm::Value* v = lowerExpr(*cl->elements[i]);
        if (!k || !v) continue;
        llvm::Value* vPtr = encodeVal(v);
        if (keyTy) {
            auto* fn = declareC("bz_dict_set_gen", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {dict, spillKey(k), vPtr});
        } else {
            // String key: extract data/len from the bz_string struct.
            llvm::Value* kd = loadStringData(k);
            llvm::Value* kl = loadStringLen(k);
            auto* fn = declareC("bz_dict_set_str", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy(), i64(), ptrTy()});
            builder_.CreateCall(fn, {dict, kd, kl, vPtr});
        }
    }
    return dict;
}


// per §4.6/§4.7: recover a local's LinkedList<T>/OwnedLinkedList<T> TypeRef.
const sema::TypeRef* Emitter::linkedListTypeOfLocal(const ast::LocalDecl* ld) {
    if (ld->type) {
        if (auto* rt = ctx_.resolvedType(*ld->type))
            if (isLinkedListTypeName(rt)) return rt;
    }
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(ld->initializer.get())) {
        if (ne->type) {
            if (auto* rt = ctx_.resolvedType(*ne->type))
                if (isLinkedListTypeName(rt)) return rt;
        }
    }
    if (ld->initializer) {
        if (auto* rt = ctx_.exprType(*ld->initializer))
            if (isLinkedListTypeName(rt)) return rt;
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


// stdlib §6.7 / §30.5: true when `rt` is a Task / Task<T> or OwnedTask<T> —
// a builtin runtime-managed async task type with no user TypeDecl. Freed via
// bz_task_free. Recognises the names "Task" and "OwnedTask" in a TypeRef that
// has no user declaration (declaration == null or declaration->decl == null
// with name "Task"/"OwnedTask") and no user TypeDecl entry.
bool Emitter::isTaskTypeName(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
    // Path A: no declaration at all — name is in astNode->name->parts[0].
    if (!rt->declaration) {
        if (rt->astNode && rt->astNode->name &&
            !rt->astNode->name->parts.empty()) {
            auto& n = rt->astNode->name->parts[0];
            return n == "Task" || n == "OwnedTask";
        }
        return false;
    }
    // Path B: builtinClassSym — a declaration with no user-defined decl and
    // whose name is "Task" or "OwnedTask".
    if (!rt->declaration->decl) {
        return rt->declaration->name == "Task" ||
               rt->declaration->name == "OwnedTask";
    }
    return false;
}

// stdlib §6.7 / §30.5: recover the Task/Task<T>/OwnedTask<T> TypeRef for a
// local's initializer or declared type. Prefers the inferred expression type
// (which is set for static calls like `Task.FromResult(42)`, `Task.Delay(10)`)
// over the declared type (rarely annotated explicitly for task locals). Returns
// null if the local does not hold a task.
const sema::TypeRef* Emitter::taskTypeOfLocal(const ast::LocalDecl* ld) {
    // Prefer the inferred expression type from the initializer — Task.FromResult,
    // Task.Delay, Task.WhenAll, Task.WhenAny, and async-method calls all set
    // the type on the initializer expression.
    if (ld->initializer) {
        if (auto* rt = ctx_.exprType(*ld->initializer))
            if (isTaskTypeName(rt)) return rt;
    }
    // Fall back to the declared type annotation.
    if (ld->type) {
        if (auto* rt = ctx_.resolvedType(*ld->type))
            if (isTaskTypeName(rt)) return rt;
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
                if (rt->kind == sema::TypeRef::Kind::Named) {
                    // Concrete class: return its declaration directly.
                    if (rt->declaration) return rt->declaration;
                    // §28.3 / §14.2: `new T()` where T is a generic type
                    // parameter — declaration is null. Resolve T through the
                    // active instantiation's genericSubstSemaTy_ to the
                    // concrete class so that registerUsingDisposal can push a
                    // disposal entry. Without this the using-var local is never
                    // disposed (destructor never runs, block leaks). This
                    // mirrors how ownedTypeParamConcreteType resolves type-param
                    // fields for per-instantiation specialized disposers.
                    if (!genericSubstSemaTy_.empty() && rt->astNode &&
                        rt->astNode->name &&
                        rt->astNode->name->parts.size() == 1) {
                        auto it = genericSubstSemaTy_.find(
                            rt->astNode->name->parts[0]);
                        if (it != genericSubstSemaTy_.end() && it->second &&
                            it->second->kind == sema::TypeRef::Kind::Named)
                            return it->second->declaration;
                    }
                }
            }
        }
    }
    // §22.11 / §28: `using var h := new C() with f := v end` — the initializer
    // is a WithExpr whose source is the NewExpr. classOfLocal must look through
    // the WithExpr to find the class so that registerUsingDisposal pushes a
    // disposal entry. Without this, the WithExpr fell through every disposal-
    // registration path and the object leaked (destructor never ran).
    if (auto* we =
            dynamic_cast<const ast::WithExpr*>(ld->initializer.get())) {
        if (we->source) {
            if (auto* ne2 =
                    dynamic_cast<const ast::NewExpr*>(we->source.get())) {
                if (ne2->type) {
                    if (auto* rt = ctx_.resolvedType(*ne2->type)) {
                        if (rt->kind == sema::TypeRef::Kind::Named)
                            return rt->declaration;
                    }
                }
            }
        }
    }
    // #639 / §28.11: `using var u := item` where `item` is a drain-loop
    // variable of user class type (an IdentifierExpr with no explicit
    // type annotation). The ownership checker confirms the transfer; here
    // we just need to recover the concrete class from the identifier's
    // sema-resolved exprType so that registerUsingDisposal can push a
    // static disposal entry (destructor runs at scope end).
    if (!ld->type &&
        dynamic_cast<const ast::IdentifierExpr*>(ld->initializer.get())) {
        if (auto* rt = ctx_.exprType(*ld->initializer)) {
            if (rt->kind == sema::TypeRef::Kind::Named && rt->declaration)
                return rt->declaration;
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
    // fix #633 / §33.4: Main(args: Array<string>) prolog built the args array
    // before any scope opened.  Consume the pending Disposal here (ROOT scope
    // only) so emitDisposals runs bz_array_free(arr, bz_string_release) on both
    // the normal-exit and unwind-exit paths, exactly once per program run.
    if (disposeStack_.size() == 1 && pendingArgsDisposal_) {
        disposeStack_.back().push_back(std::move(*pendingArgsDisposal_));
        pendingArgsDisposal_.reset();
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
        // #429 / §27.5: a boxed capture cell (shared heap allocation from
        // boxLocalSlot) whose owning lambda is proven non-escaping.  `slot`
        // is the ptr-alloca (newSlot) holding the cell pointer.  At scope end
        // we load the cell pointer and call free() on it.
        // ORDER: this entry is registered for the captured-local slot, which
        // was declared BEFORE the lambda local.  In reverse-decl order the
        // lambda local's closureRelease fires FIRST (LIFO), running the envdtor
        // which reads all values through the cell.  Only then does this entry
        // fire and free the cell itself — no UAF.
        // NULL SAFETY: the guarded boxing path null-initialises the cell-holder
        // in the function entry block; free(null) is a no-op (POSIX).
        // BOTH normal and unwind paths: the cell has no live external references
        // at this point (all lambdas referencing it are non-escaping and have
        // been released), so freeing on either path is safe.
        if (it->boxedCellFree) {
            llvm::Value* cell =
                builder_.CreateLoad(ptrTy(), it->slot, "cell.ptr");
            builder_.CreateCall(freeFn(), {cell});
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
        // #429 / §28.9 / §4.10: a Set<string> stores its string elements as
        // KEYS (not values) in the generic-key table. Before bz_dict_free frees
        // each key copy, emit bz_set_string_keys_release to call
        // bz_string_release on every live key's string pointer. This discharges
        // the set's co-owned +1 per element (retained at Add). bz_dict_free is
        // then called with a null value disposer (sentinels, nothing to dispose).
        if (it->dict) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* tbl = builder_.CreateLoad(ptrTy(), addr);
            if (it->dictStringKeys) {
                // Set<string>: release each string element's +1 before freeing.
                auto* relFn = declareC("bz_set_string_keys_release",
                                       llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(relFn, {tbl});
            }
            llvm::Value* disp = it->dictValueDisposer
                ? llvm::cast<llvm::Value>(it->dictValueDisposer)
                : llvm::ConstantPointerNull::get(ptrTy());
            auto* fn = declareC("bz_dict_free",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {tbl, disp});
            continue;
        }
        // per §4.6/§4.7: a `using var` LinkedList<T> / OwnedLinkedList<T>.
        // Free the node chain via bz_llist_free; for an OwnedLinkedList<T> of
        // class/interface elements the element-disposer thunk cascades disposal
        // into each (non-null) element exactly once (same policy as OwnedArray —
        // but on the node value pointer, not an array slot).
        // #602 batch 3/4: owningCollectionElemDisposer deleted; d.collectionElemDisposer
        // was set from elementPolicyFor at registerLocal time (covers class, interface,
        // Shared<T>, Weak<T> elements per §28.11 v0.135).
        if (it->linkedList) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* list = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* disp = it->collectionElemDisposer
                ? llvm::cast<llvm::Value>(it->collectionElemDisposer)
                : llvm::ConstantPointerNull::get(ptrTy());
            auto* fn = declareC("bz_llist_free",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {list, disp});
            continue;
        }
        // §2.15 / issue #550: a `using var` BinarySerializer — free the
        // serializer buffer + state via bz_binser_free.
        if (it->binarySerializer) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* h = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                h, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "binser.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "binser.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_binser_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {h});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §2.15 / issue #550: a `using var` BinaryDeserializer — free the
        // deserializer cursor state via bz_binder_free.
        if (it->binaryDeserializer) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* h = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                h, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "binder.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "binder.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_binder_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {h});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §2.15 / issue #550: a `using var` JsonSerializer — free via
        // bz_binser_free (same as BinarySerializer; kind field differentiates).
        if (it->jsonSerializer) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* h = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                h, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "jser.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "jser.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_binser_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {h});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §2.15 / issue #550: a `using var` JsonDeserializer — free via
        // bz_binder_free (same as BinaryDeserializer; kind field differentiates).
        if (it->jsonDeserializer) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* h = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                h, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "jder.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "jder.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_binder_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {h});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §8.1 / issue #646: a `using var` Stopwatch. Free the timing struct
        // via bz_stopwatch_free. Null-guarded for consistency with other
        // builtin disposal sites.
        if (it->stopwatch) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* sw = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                sw, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "sw.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "sw.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_stopwatch_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {sw});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.1 / §30.8: a `using var` CancellationTokenSource. Dispose via
        // bz_cts_dispose (idempotent: signals all tokens, then frees the source).
        // null-guarded: bz_cts_dispose handles null internally (its disposed flag
        // prevents double-free), but we guard anyway for consistency with other
        // builtin disposal sites.
        if (it->cancellationTokenSource) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* cts = builder_.CreateLoad(ptrTy(), addr);
            auto* fn = declareC("bz_cts_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {cts});
            continue;
        }
        // §28.13 / stdlib §2.16 (#603): a `using var` Shared<T> handle.
        // Null-guarded: bz_shared_release handles null gracefully, but we load
        // and call only when non-null for consistency with other disposal sites.
        if (it->sharedHandle) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* h = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                h, llvm::ConstantPointerNull::get(ptrTy()));
            auto* relBB  = llvm::BasicBlock::Create(llvm_, "shared.rel",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "shared.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, relBB);
            builder_.SetInsertPoint(relBB);
            auto* fn = declareC("bz_shared_release",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {h});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §28.13 / stdlib §2.16 (#603): a `using var` Weak<T> handle.
        if (it->weakHandle) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* h = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                h, llvm::ConstantPointerNull::get(ptrTy()));
            auto* relBB  = llvm::BasicBlock::Create(llvm_, "weak.rel",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "weak.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, relBB);
            builder_.SetInsertPoint(relBB);
            auto* fn = declareC("bz_weak_release",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {h});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // stdlib §6.7 / §30.5: a `using var` Task / Task<T>. At scope end, the
        // task's Dispose() runs "but does not block waiting for completion" (§30.5).
        // We implement this via bz_task_fire_and_forget, which registers a
        // continuation that calls bz_task_free when the task eventually completes.
        // This is safe for both already-complete tasks (the continuation fires
        // immediately via bz_task_then's fast path) and still-running tasks
        // (the task struct remains live until the continuation fires). The
        // alternative of calling bz_task_free directly at scope end would be
        // unsafe if the task is still running on the thread pool (UAF). Null-
        // guarded: bz_task_fire_and_forget tolerates a null task gracefully (no
        // continuation is registered for null). Double-free safety: lowerAwait only
        // frees tasks whose operand is an anonymous CallExpr (`await Producer()`),
        // never a named local (`await t`), so a using-var-bound task is fired-
        // and-forgotten exactly once here at scope end.
        if (it->task) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* t = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                t, llvm::ConstantPointerNull::get(ptrTy()));
            auto* fafBB = llvm::BasicBlock::Create(llvm_, "task.faf", currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "task.cont", currentFn_);
            builder_.CreateCondBr(isNull, contBB, fafBB);
            builder_.SetInsertPoint(fafBB);
            auto* fn = declareC("bz_task_fire_and_forget",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {t});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.2 / issue #647: a `using var` Mutex. Dispose via bz_mutex_dispose
        // (marks disposed, drops the Mutex object's core ref; destroys the
        // underlying pthread_mutex when the last Lock is also released).
        // Null-guarded: bz_mutex_dispose is null-safe, but we guard for
        // consistency with other builtin disposal sites.
        if (it->mutex) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* mx = builder_.CreateLoad(ptrTy(), addr, "mx.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                mx, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB  = llvm::BasicBlock::Create(llvm_, "mx.dispose",  currentFn_);
            auto* contBB  = llvm::BasicBlock::Create(llvm_, "mx.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            auto* fn = declareC("bz_mutex_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {mx});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.2 / issue #647: a `using var` Lock. Release via bz_lock_release
        // (idempotent per §28.2: bz_lock_release checks the released flag).
        // Null-guarded: bz_lock_release handles null (null Lock? on timeout path).
        if (it->lock) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* lk = builder_.CreateLoad(ptrTy(), addr, "lk.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                lk, llvm::ConstantPointerNull::get(ptrTy()));
            auto* relBB  = llvm::BasicBlock::Create(llvm_, "lk.release",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "lk.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, relBB);
            builder_.SetInsertPoint(relBB);
            auto* fn = declareC("bz_lock_release",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {lk});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.3 / issue #647: a `using var` Semaphore. Dispose via bz_semaphore_dispose
        // (marks disposed, drops the Semaphore object's core ref; destroys the
        // underlying sem_t when the last SemaphoreToken is also released).
        if (it->semaphore) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* sem = builder_.CreateLoad(ptrTy(), addr, "sem.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                sem, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB  = llvm::BasicBlock::Create(llvm_, "sem.dispose",  currentFn_);
            auto* contBB  = llvm::BasicBlock::Create(llvm_, "sem.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            auto* fn = declareC("bz_semaphore_dispose",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {sem});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §6.3 / issue #647: a `using var` SemaphoreToken. Release via
        // bz_semaphore_token_release (idempotent per §28.2: released flag
        // prevents double-release).  Null-safe (handles null SemaphoreToken?
        // on the timeout path).
        if (it->semaphoreToken) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* tok = builder_.CreateLoad(ptrTy(), addr, "stok.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                tok, llvm::ConstantPointerNull::get(ptrTy()));
            auto* relBB  = llvm::BasicBlock::Create(llvm_, "stok.release",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "stok.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, relBB);
            builder_.SetInsertPoint(relBB);
            auto* fn = declareC("bz_semaphore_token_release",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {tok});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // issue #545: a `using var` Buffer<T>. Free via bz_buffer_free (null-safe
        // plain free — value elements carry no disposal obligation, so no per-element
        // callback is needed). Null-guarded for consistency with other builtin disposal.
        if (it->buffer) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* buf = builder_.CreateLoad(ptrTy(), addr, "buf.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                buf, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "buf.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "buf.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            auto* fn = declareC("bz_buffer_free",
                                llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(fn, {buf});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §30.15 (#600 phase c): a `using var` Channel<T>. Dispose via
        // bz_channel_dispose (closes, faults pending receivers with
        // ChannelClosedException, disposes undelivered items in reverse order
        // per §28.11, frees the struct). The ChannelClosedException vtable is
        // retrieved from builtinVtable_ (set up by setupBuiltinExceptions).
        if (it->channel) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* ch = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                ch, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB  = llvm::BasicBlock::Create(llvm_, "ch.dispose",  currentFn_);
            auto* contBB  = llvm::BasicBlock::Create(llvm_, "ch.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            // Get ChannelClosedException vtable (may be null before setup).
            llvm::Value* cceVt = llvm::ConstantPointerNull::get(ptrTy());
            {   auto vit = builtinVtable_.find("ChannelClosedException");
                if (vit != builtinVtable_.end()) cceVt = vit->second;
            }
            auto* fn = declareC("bz_channel_dispose",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {ch, cceVt});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // #628 / stdlib §4.9 v0.135: a `using var` DrainedEntry<K,V> — the
        // loop variable from `for var entry in d.Drain() do … end`. The entry
        // struct is {K key; V* value} allocated on the stack. At scope end:
        //   1. Load V* from field[1] and dispose+reclaim the owned class value.
        //   2. If K is string: load the bz_string_t* from field[0] and release
        //      it (bz_string_release) — the drain runtime transferred ownership
        //      of the heap-allocated key string to the loop entry struct.
        // The slot is a struct AllocaInst, not a pointer-to-heap-object; no
        // boxedStorage detour — load fields directly via GEP.
        if (it->drainedEntry) {
            // drainedEntryKeyType / drainedEntryValType carry the LLVM types for
            // fields 0 and 1 of the entry struct (same layout as the alloca in
            // lowerDictDrainFor: { keyLlvm, valLlvm }). They may be null if
            // lowerTypeRef failed at registration time; skip the whole disposal
            // in that case (should not happen in well-formed code).
            llvm::Type* kTy = it->drainedEntryKeyType;
            llvm::Type* vTy = it->drainedEntryValType;
            if (kTy && vTy) {
                llvm::StructType* entStTy =
                    llvm::StructType::get(llvm_, {kTy, vTy});
                // Step 1: dispose the owned V class value (field[1]).
                // Guard: V must be a pointer type (class). OwnedDictionary<K,V>
                // requires V: class per §4.9, so in well-formed code this always
                // holds; the guard prevents codegen crashes on ill-formed programs.
                if (vTy->isPointerTy()) {
                    llvm::Value* valFldPtr =
                        builder_.CreateStructGEP(entStTy, it->slot, 1, "de.val.ptr");
                    llvm::Value* valPtr =
                        builder_.CreateLoad(ptrTy(), valFldPtr, "de.val");
                    llvm::Value* isNullVal = builder_.CreateICmpEQ(
                        valPtr, llvm::ConstantPointerNull::get(ptrTy()));
                    auto* dispBB = llvm::BasicBlock::Create(
                        llvm_, "de.dispose", currentFn_);
                    auto* afterValBB = llvm::BasicBlock::Create(
                        llvm_, "de.after_val", currentFn_);
                    builder_.CreateCondBr(isNullVal, afterValBB, dispBB);
                    builder_.SetInsertPoint(dispBB);
                    // Dispose via dynamic dispatch (emitDisposeAndReclaim handles
                    // the vtable call + bz_free; dynamic dispatch is correct for
                    // both user and builtin V class types).
                    emitDisposeAndReclaim(valPtr);
                    builder_.CreateStore(
                        llvm::ConstantPointerNull::get(ptrTy()), valFldPtr);
                    builder_.CreateBr(afterValBB);
                    builder_.SetInsertPoint(afterValBB);
                }
                // Step 2: if K is string, release the owned key string.
                if (it->drainedEntryKeyIsString) {
                    llvm::Value* keyFldPtr =
                        builder_.CreateStructGEP(entStTy, it->slot, 0, "de.key.ptr");
                    llvm::Value* keyPtr =
                        builder_.CreateLoad(ptrTy(), keyFldPtr, "de.key");
                    emitStringRelease(keyPtr);
                }
            }
            continue;
        }
        // §30.15 (#600 phase c): a `using var` ChannelSender<T>. Release via
        // bz_channel_sender_release (decrements sender count; may close the
        // channel if this was the last sender).
        if (it->channelSender) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* tx = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                tx, llvm::ConstantPointerNull::get(ptrTy()));
            auto* relBB  = llvm::BasicBlock::Create(llvm_, "tx.release",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "tx.cont",     currentFn_);
            builder_.CreateCondBr(isNull, contBB, relBB);
            builder_.SetInsertPoint(relBB);
            llvm::Value* cceVt = llvm::ConstantPointerNull::get(ptrTy());
            {   auto vit = builtinVtable_.find("ChannelClosedException");
                if (vit != builtinVtable_.end()) cceVt = vit->second;
            }
            auto* fn = declareC("bz_channel_sender_release",
                                llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {tx, cceVt});
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §28.12 / §30 (`await using var`): async disposal path. On the NORMAL
        // exit (not isUnwind): call DisposeAsync() directly, await the returned
        // Task (a proper suspend point), then free the block (physical reclamation).
        // On the UNWIND path (exception in flight): synchronous Dispose() fallback —
        // the resource is reclaimed on every exit path.
        if (it->asyncDispose) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), addr);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                obj, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB = llvm::BasicBlock::Create(llvm_, "awaitusing.dispose",
                                                    currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "awaitusing.cont",
                                                    currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            // Pass the direct function pointer; null means sync fallback.
            // On the unwind path, always use sync fallback (pass null).
            emitAsyncDisposeAwaitAndFree(
                obj, isUnwind ? nullptr : it->disposeAsyncFn);
            builder_.CreateStore(
                llvm::ConstantPointerNull::get(ptrTy()), addr);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §29.7 (#623): an iterator enumerator whose Dispose is at vtable slot 1
        // (iterator format: [0]=td, [1]=Dispose, [2]=MoveNext, [3]=Current).
        // Call Dispose(self) via slot 1 (which runs pending finally blocks inside
        // the state machine and sets state=-1) then free the heap frame.
        if (it->iterEnumDispose) {
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), it->slot);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                obj, llvm::ConstantPointerNull::get(ptrTy()));
            auto* dispBB = llvm::BasicBlock::Create(llvm_, "iter.dispose",
                                                    currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "iter.cont",
                                                    currentFn_);
            builder_.CreateCondBr(isNull, contBB, dispBB);
            builder_.SetInsertPoint(dispBB);
            // Dispatch vtable slot 1 (Dispose).
            llvm::Value* vt = builder_.CreateLoad(ptrTy(), obj);
            llvm::Value* dispSlot =
                builder_.CreateGEP(ptrTy(), vt, {builder_.getInt32(1)});
            llvm::Value* dispFnPtr = builder_.CreateLoad(ptrTy(), dispSlot);
            auto* dispTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {ptrTy()}, false);
            builder_.CreateCall(dispTy, dispFnPtr, {obj});
            // Dispose now freed the frame — null the slot.
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()),
                                 it->slot);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
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
    // Pass C: build each record's box vtable using the new itable layout
    // (CG35-itable, §9.6 / §8.6 / §13). New layout:
    //   [0]=td, [1]=itmap$box$C (null-terminated [td$I, itab$box$C$I, …]),
    //   [2]=Dispose (no-op: boxed value types are runtime-managed, §28.9)
    // Interface dispatch on a boxed record goes through bz_itable_lookup,
    // just like a class. Enums get the same shape (no interface methods).
    //
    // Box thunks: the real record method takes `this` = pointer to the bare
    // record bytes. A boxed record's receiver is the BOX pointer
    // { vtable*, i64 rc, record-bytes } (#422); the record bytes start TWO
    // words in. Each itable entry is a small thunk that advances `this` past
    // both words then tail-calls the real method.
    for (auto& [ts, supers] : supersOf) {
        std::unordered_set<const sema::TypeSymbol*> superSet(supers.begin(),
                                                             supers.end());

        // Build itab$box$C$I for each interface I that this record implements.
        std::vector<llvm::Constant*> itmapElems;
        for (const sema::TypeSymbol* iface : supers) {
            if (iface->kind != ast::TypeDeclKind::Interface) continue;

            // Collect iface's non-static methods in declaration order.
            std::vector<sema::MethodSymbol*> ifaceMethods;
            for (sema::Symbol* m : iface->members) {
                auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
                if (ms && !isStaticMethod(ms)) ifaceMethods.push_back(ms);
            }
            // §24.7 (#477 B7): collect iface's sync events in declaration order
            // (after the methods, matching ifaceEventIndex_'s numbering). Even
            // though records cannot declare events themselves, an interface that
            // a record implements may declare events; their slots must be
            // present (as null entries) to keep the property-slot offsets
            // consistent with ifacePropertyGetIndex_.
            std::vector<sema::EventSymbol*> ifaceEvents;
            for (sema::Symbol* m : iface->members) {
                auto* es = dynamic_cast<sema::EventSymbol*>(m);
                if (!es) continue;
                auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl);
                if (ed && !ed->isAsync) ifaceEvents.push_back(es);
            }
            // #481 / §13 / §23: collect iface's properties in declaration order
            // (after the events, matching ifacePropertyGetIndex_'s numbering).
            // This was the root cause of the block-form-getter SIGSEGV: Pass C
            // only handled methods, so property slots were absent from the
            // record's box itable and bz_itable_lookup + GEP reached null.
            std::vector<sema::PropertySymbol*> ifaceProps;
            for (sema::Symbol* m : iface->members) {
                auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
                if (ps && !ps->isIndexer) ifaceProps.push_back(ps);
            }
            // Skip a wholly empty interface (no methods, events, or properties).
            if (ifaceMethods.empty() && ifaceEvents.empty() &&
                ifaceProps.empty())
                continue;

            // Helper lambda: build a box→record thunk that GEPs past the two
            // header words (vtable*, rc) in the box and tail-calls `realFn`.
            // Signature: (box ptr, extra params…) → return type of realFn.
            // Records are never enums, so only the GEP path is needed here.
            auto makeBoxThunk = [&](llvm::Function* realFn,
                                    const std::string& thunkName) -> llvm::Function* {
                llvm::FunctionType* realFnTy = realFn->getFunctionType();
                // Replace the first param (record ptr) with ptrTy() (box ptr).
                std::vector<llvm::Type*> thunkParamTys;
                thunkParamTys.push_back(ptrTy());  // box ptr
                for (unsigned pi = 1; pi < realFnTy->getNumParams(); ++pi)
                    thunkParamTys.push_back(realFnTy->getParamType(pi));
                auto* thunkFnTy = llvm::FunctionType::get(
                    realFnTy->getReturnType(), thunkParamTys, /*isVarArg=*/false);
                auto* thunk = llvm::Function::Create(
                    thunkFnTy, llvm::Function::InternalLinkage,
                    thunkName, module_.get());
                auto* bb = llvm::BasicBlock::Create(llvm_, "entry", thunk);
                llvm::IRBuilder<> tb(bb);
                // GEP to the record bytes: box + 2 * sizeof(ptr) = +16.
                auto ai = thunk->arg_begin();
                llvm::Value* valSlot = tb.CreateGEP(
                    ptrTy(), &*ai,
                    {llvm::ConstantInt::get(i32(), 2)}, "box.valptr");
                std::vector<llvm::Value*> callArgs{valSlot};
                ++ai;
                for (; ai != thunk->arg_end(); ++ai) callArgs.push_back(&*ai);
                llvm::Value* r = tb.CreateCall(realFnTy, realFn, callArgs);
                if (realFnTy->getReturnType()->isVoidTy())
                    tb.CreateRetVoid();
                else
                    tb.CreateRet(r);
                return thunk;
            };

            std::vector<llvm::Constant*> itabElems;
            for (sema::MethodSymbol* im : ifaceMethods) {
                llvm::Constant* fp = llvm::ConstantPointerNull::get(ptrTy());
                sema::MethodSymbol* impl = findMethodByName(ts, im->name);
                if (impl) {
                    auto mit = methodFns_.find(impl);
                    if (mit != methodFns_.end()) {
                        llvm::Function* real = mit->second;
                        llvm::FunctionType* realFnTy = real->getFunctionType();
                        // §10 / §8.6 / §13 / #435: box thunks adapt
                        // interface dispatch (box ptr as `this`) to the
                        // underlying method's calling convention.
                        //
                        // For RECORDS: `this` is a pointer to the bare
                        // record bytes. The box is { vtable*, i64 rc,
                        // record-bytes }; record bytes start two words in
                        // (offset 16). The thunk GEPs past the two header
                        // words and passes the resulting pointer.
                        //
                        // For ENUMS: `this` is the i32 enum value itself
                        // (§10, CG30). The box stores the i32 at offset 16
                        // (two words from the box base, same layout).
                        // The thunk must LOAD the i32 from that slot and
                        // pass it by value — a GEP with an i32 base is
                        // malformed IR (IR verifier failure, #435).
                        //
                        // In both cases the thunk's external signature
                        // (seen by the interface caller) has ptrTy() as the
                        // first parameter, matching the interface method ABI.
                        bool isEnumImpl =
                            ts->kind == ast::TypeDeclKind::Enum;
                        // Build the thunk's own function type: replace the
                        // first param with ptrTy() (box pointer). Param
                        // types 1..N are unchanged.
                        std::vector<llvm::Type*> thunkParamTys;
                        thunkParamTys.push_back(ptrTy());  // box ptr
                        for (unsigned pi = 1;
                             pi < realFnTy->getNumParams(); ++pi)
                            thunkParamTys.push_back(
                                realFnTy->getParamType(pi));
                        auto* thunkFnTy = llvm::FunctionType::get(
                            realFnTy->getReturnType(), thunkParamTys,
                            /*isVarArg=*/false);
                        auto* thunk = llvm::Function::Create(
                            thunkFnTy, llvm::Function::InternalLinkage,
                            "bz$boxthunk$" + std::string(ts->name) + "$" +
                                std::string(im->name),
                            module_.get());
                        auto* bb = llvm::BasicBlock::Create(llvm_, "entry",
                                                            thunk);
                        llvm::IRBuilder<> tb(bb);
                        std::vector<llvm::Value*> callArgs;
                        auto ai = thunk->arg_begin();
                        // GEP to the value slot: box + 2 * sizeof(ptr) = +16.
                        llvm::Value* valSlot = tb.CreateGEP(
                            ptrTy(), &*ai,
                            {llvm::ConstantInt::get(i32(), 2)}, "box.valptr");
                        if (isEnumImpl) {
                            // §10 / #435: enum `this` is i32 by value —
                            // load it from the box's value slot.
                            llvm::Value* enumVal = tb.CreateLoad(
                                i32(), valSlot, "box.enumval");
                            callArgs.push_back(enumVal);
                        } else {
                            // Record: pass the pointer to the value slot
                            // as the record `this`.
                            callArgs.push_back(valSlot);
                        }
                        ++ai;
                        for (; ai != thunk->arg_end(); ++ai)
                            callArgs.push_back(&*ai);
                        llvm::Value* r = tb.CreateCall(realFnTy, real,
                                                       callArgs);
                        if (realFnTy->getReturnType()->isVoidTy())
                            tb.CreateRetVoid();
                        else
                            tb.CreateRet(r);
                        fp = thunk;
                    }
                }
                itabElems.push_back(fp);
            }
            // §24.7 (#477 B7): append null slots for interface events. Records
            // cannot declare or back interface events; the slots must be present
            // so property GEP offsets (ifacePropertyGetIndex_) stay correct.
            for (sema::EventSymbol* ie : ifaceEvents) {
                (void)ie;  // slot present for index consistency; record has no event backing
                itabElems.push_back(llvm::ConstantPointerNull::get(ptrTy()));
            }
            // #481 / §13 / §23 (block-form getter fix): append getter (and
            // setter) box-thunks for each interface property. The thunk GEPs
            // past the box header to reach the record bytes, then calls the
            // uniform accessor function (ensurePropertyAccessorFn) which handles
            // arrow, block, and auto-property bodies. This mirrors Pass D (class
            // itable) but adapted for the record-box receiver shape.
            //
            // Ensure the record struct layout (and fieldIndex_) is populated
            // before calling ensurePropertyAccessorFn: block/arrow getter
            // bodies access fields via fieldIndex_, which recordStruct()
            // populates lazily. Pass C runs before method bodies are emitted,
            // so fieldIndex_ may not yet exist for this record type.
            // classStruct routes to recordStruct for records (§9.4).
            classStruct(ts);
            for (sema::PropertySymbol* ip : ifaceProps) {
                sema::PropertySymbol* impl =
                    classPropertyByName(ts, ip->name);
                llvm::Constant* getFp =
                    llvm::ConstantPointerNull::get(ptrTy());
                if (impl) {
                    if (llvm::Function* acc =
                            ensurePropertyAccessorFn(ts, impl, /*isSet=*/false)) {
                        std::string thunkName =
                            "bz$boxpropget$" + std::string(ts->name) + "$" +
                            std::string(ip->name);
                        getFp = makeBoxThunk(acc, thunkName);
                    }
                }
                itabElems.push_back(getFp);
                if (propertyHasSetter(ip)) {
                    llvm::Constant* setFp =
                        llvm::ConstantPointerNull::get(ptrTy());
                    if (impl) {
                        if (llvm::Function* acc = ensurePropertyAccessorFn(
                                ts, impl, /*isSet=*/true)) {
                            std::string thunkName =
                                "bz$boxpropset$" + std::string(ts->name) +
                                "$" + std::string(ip->name);
                            setFp = makeBoxThunk(acc, thunkName);
                        }
                    }
                    itabElems.push_back(setFp);
                }
            }
            auto* itabArrTy = llvm::ArrayType::get(ptrTy(), itabElems.size());
            auto* itabGV = new llvm::GlobalVariable(
                *module_, itabArrTy, /*isConstant=*/true,
                llvm::GlobalValue::InternalLinkage,
                llvm::ConstantArray::get(itabArrTy, itabElems),
                "itab$box$" + std::string(ts->name) + "$" +
                    std::string(iface->name));

            auto dit2 = descArray_.find(iface);
            if (dit2 != descArray_.end()) {
                itmapElems.push_back(
                    llvm::cast<llvm::Constant>(dit2->second));
                itmapElems.push_back(
                    llvm::ConstantExpr::getPointerCast(itabGV, ptrTy()));
            }
        }
        // Null terminator.
        itmapElems.push_back(llvm::ConstantPointerNull::get(ptrTy()));
        auto* itmapArrTy = llvm::ArrayType::get(ptrTy(), itmapElems.size());
        auto* boxItmapGV = new llvm::GlobalVariable(
            *module_, itmapArrTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(itmapArrTy, itmapElems),
            "itmap$box$" + std::string(ts->name));

        // Vtable (§8.6/§8.5 #417): [0]=td, [1]=itmap$box$C, [2]=Dispose (no-op),
        // [3]=ToString (synthesized record/enum form, or user override).
        std::vector<llvm::Constant*> elems(kToStringSlot + 1,
                                           llvm::ConstantPointerNull::get(
                                               ptrTy()));
        elems[0] = descArray_[ts];
        elems[1] = llvm::ConstantExpr::getPointerCast(boxItmapGV, ptrTy());
        if (builtinDisposeFn_) elems[2] = builtinDisposeFn_;
        elems[kToStringSlot] = boxToStringThunk(ts);
        auto* arrTy = llvm::ArrayType::get(ptrTy(), elems.size());
        boxVtable_[ts] = new llvm::GlobalVariable(
            *module_, arrTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(arrTy, elems),
            "bvt$box$" + std::string(ts->name));
        allVtables_.push_back(boxVtable_[ts]);  // §8.6 #417
    }

    // --- Primitives: descriptor + box vtable ------------------------------
    // Each boxable value primitive gets a 2-element descriptor [self, null] and
    // a vtable matching the uniform class/box prefix (§8.6/§8.5 #417):
    //   [0]=descriptor (read by emitIsA for `o is int`),
    //   [1]=itmap (null — primitives implement no user interface),
    //   [2]=Dispose (no-op — boxed primitives are runtime-managed, §28.9),
    //   [3]=ToString (loads the box payload, formats it).
    // (string is included so the descriptor exists for `o is string`'s target,
    // but a string is NOT boxed — it stays a bare bz_string* in an Object slot,
    // recognized via bz_obj_is_string; see lowerTypeTest / lowerObjectToString.)
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
        // ToString thunk for every boxable primitive (not string — string has
        // no box payload; its ToString-through-Object is identity, handled
        // separately). The null at slot 3 for string keeps the prefix uniform.
        llvm::Constant* tostr =
            (p == P::String)
                ? static_cast<llvm::Constant*>(
                      llvm::ConstantPointerNull::get(ptrTy()))
                : primBoxToStringThunk(p, nm);
        auto* vtTy = llvm::ArrayType::get(ptrTy(), kToStringSlot + 1);
        primBoxVtable_[nm] = new llvm::GlobalVariable(
            *module_, vtTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(
                vtTy, {desc,
                       llvm::ConstantPointerNull::get(ptrTy()),  // itmap
                       builtinDisposeFn_
                           ? llvm::cast<llvm::Constant>(builtinDisposeFn_)
                           : llvm::ConstantPointerNull::get(ptrTy()),  // Dispose
                       tostr}),
            std::string("pvt$") + nm);
        allVtables_.push_back(primBoxVtable_[nm]);  // §8.6 #417
    }
}


// §8.5 / §8.6 (#417): RAII helper to emit a function body at SETUP time (no
// active Bezel function). Saves/restores currentFn_ + the builder's insert
// point, and positions the builder at a fresh "entry" block of `fn`. The body
// emitters (emitRecordToString, emitPrimitiveToString, …) use the member
// builder_ and currentFn_ (e.g. via entryAlloca), so a thunk must set them up.
namespace {
struct ThunkScope {
    llvm::Function*& curFn;
    llvm::IRBuilder<>& b;
    llvm::Function* saved;
    llvm::BasicBlock* savedBB;
    llvm::BasicBlock::iterator savedIt;
    bool hadInsert;
    ThunkScope(llvm::Function*& cf, llvm::IRBuilder<>& bb, llvm::Function* fn,
               llvm::BasicBlock* entry)
        : curFn(cf), b(bb), saved(cf), hadInsert(b.GetInsertBlock() != nullptr) {
        if (hadInsert) { savedBB = b.GetInsertBlock(); savedIt = b.GetInsertPoint(); }
        curFn = fn;
        b.SetInsertPoint(entry);
    }
    ~ThunkScope() {
        curFn = saved;
        if (hadInsert) b.SetInsertPoint(savedBB, savedIt);
        else b.ClearInsertionPoint();
    }
};
}  // namespace


// §8.5 / §8.6 (#417): the uniform ToString thunk for a CLASS. Returns the user
// `ToString()` override if one exists anywhere on the class chain (§8.5 "via
// virtual dispatch"); otherwise a default that returns the class's name (§8.5
// "For classes: the type's qualified name"). The thunk's signature is the
// vtable-uniform `ptr(ptr this)`.
llvm::Constant* Emitter::classToStringThunk(const sema::TypeSymbol* ts) {
    auto it = classToStringFns_.find(ts);
    if (it != classToStringFns_.end()) return it->second;

    // Walk the class chain for a user `ToString(): string` (no params). The
    // most-derived override wins; vtableSlot_ already routes virtual overrides,
    // but the thunk is what slot 3 points at for THIS exact class's vtable, so
    // resolve the override visible from `ts`.
    sema::MethodSymbol* userToStr = nullptr;
    for (const sema::TypeSymbol* cur = ts; cur && !userToStr;) {
        for (sema::Symbol* sym : cur->members) {
            auto* ms = dynamic_cast<sema::MethodSymbol*>(sym);
            if (ms && !isStaticMethod(ms) && ms->name == "ToString" &&
                ms->params.empty()) { userToStr = ms; break; }
        }
        const sema::TypeSymbol* nxt = nullptr;
        for (const sema::TypeSymbol* b : cur->baseTypes)
            if (b && b->kind == ast::TypeDeclKind::Class) { nxt = b; break; }
        cur = nxt;
    }

    auto* fnTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, /*isVarArg=*/false);
    auto* thunk = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage,
        "bz$tostr$" + std::string(ts->name), module_.get());
    classToStringFns_[ts] = thunk;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", thunk);
    {
        ThunkScope scope(currentFn_, builder_, thunk, entry);
        llvm::Value* result = nullptr;
        if (userToStr) {
            auto fit = methodFns_.find(userToStr);
            if (fit != methodFns_.end()) {
                // Call the real user ToString non-virtually (the thunk IS the
                // dispatch target; calling it directly avoids re-entry).
                result = builder_.CreateCall(fit->second->getFunctionType(),
                                             fit->second, {thunk->getArg(0)});
            }
        }
        if (!result)
            result = lowerStringLiteral(
                std::string("\"") + std::string(ts->name) + "\"", false);
        builder_.CreateRet(result);
    }
    return thunk;
}


// §8.5 / §8.6 (#417): the uniform ToString thunk for a boxed RECORD or boxed
// ENUM. The receiver is the BOX pointer { vtable*, i64 rc, payload }; the
// payload starts two words in (offset 16, BZ_BOX layout). For a record the
// thunk prefers a user `ToString()` override and otherwise emits the
// synthesized §8.5/§9.1/§9.2 property-listing form. For an enum the thunk
// returns the type name (§8.5).
llvm::Constant* Emitter::boxToStringThunk(const sema::TypeSymbol* ts) {
    auto it = boxToStringFns_.find(ts);
    if (it != boxToStringFns_.end()) return it->second;

    auto* fnTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, /*isVarArg=*/false);
    auto* thunk = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage,
        "bz$boxtostr$" + std::string(ts->name), module_.get());
    boxToStringFns_[ts] = thunk;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", thunk);
    {
        ThunkScope scope(currentFn_, builder_, thunk, entry);
        llvm::Value* result = nullptr;
        if (ts->kind == ast::TypeDeclKind::Enum) {
            // §8.5: enum default ToString is the type's qualified name.
            result = lowerStringLiteral(
                std::string("\"") + std::string(ts->name) + "\"", false);
        } else {
            // Record: payload is the bare record struct two words into the box.
            llvm::Value* boxPtr = thunk->getArg(0);
            llvm::Value* payload = builder_.CreateGEP(
                ptrTy(), boxPtr, {builder_.getInt32(2)}, "box.valptr");
            // Prefer a user `ToString()` override (§8.5 via dispatch); the
            // synthesized property-listing form is the default (§9.1/§9.2).
            sema::MethodSymbol* userToStr = nullptr;
            for (sema::Symbol* sym : ts->members) {
                auto* ms = dynamic_cast<sema::MethodSymbol*>(sym);
                if (ms && !isStaticMethod(ms) && ms->name == "ToString" &&
                    ms->params.empty()) { userToStr = ms; break; }
            }
            if (userToStr) {
                auto fit = methodFns_.find(userToStr);
                if (fit != methodFns_.end())
                    result = builder_.CreateCall(fit->second->getFunctionType(),
                                                 fit->second, {payload});
            }
            if (!result) {
                // Load the record value and build the §9.1/§9.2 form.
                llvm::StructType* recTy = recordStruct(ts);
                llvm::Value* recVal = builder_.CreateLoad(recTy, payload, "rec");
                result = emitRecordToString(recVal, ts);
            }
        }
        builder_.CreateRet(result);
    }
    return thunk;
}


// §8.5 / §8.6 (#417): the uniform ToString thunk for a boxed PRIMITIVE. The
// receiver is the BOX pointer; the scalar value lives two words in. The thunk
// loads the value and formats it via emitPrimitiveToString (int → decimal,
// bool → "True"/"False" per stdlib bool.ToString(), etc.).
llvm::Constant* Emitter::primBoxToStringThunk(sema::PrimitiveType prim,
                                              const char* name) {
    auto it = primBoxToStringFns_.find(name);
    if (it != primBoxToStringFns_.end()) return it->second;

    auto* fnTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, /*isVarArg=*/false);
    auto* thunk = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage,
        std::string("bz$primtostr$") + name, module_.get());
    primBoxToStringFns_[name] = thunk;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", thunk);
    {
        ThunkScope scope(currentFn_, builder_, thunk, entry);
        llvm::Value* boxPtr = thunk->getArg(0);
        llvm::Value* payload = builder_.CreateGEP(
            ptrTy(), boxPtr, {builder_.getInt32(2)}, "box.valptr");
        llvm::Type* valTy = lowerPrimitive(prim);
        llvm::Value* val = builder_.CreateLoad(valTy, payload, "boxval");
        llvm::Value* result = emitPrimitiveToString(val, prim);
        builder_.CreateRet(result);
    }
    return thunk;
}


// §8.6 / §12.7 (#417): publish (once) the null-terminated `bz$vtable$table`
// listing every vtable / box-vtable / exception-vtable global codegen emitted.
// bz_obj_is_string scans it: an Object holding a string has `char* data` (not a
// listed vtable) at word 0, so it is not found → recognized as a string. The
// table is built lazily on the first `is/as/ToString string-through-Object`
// site; all vtable globals already exist by the time any function body emits.
llvm::Value* Emitter::vtableTablePtr() {
    if (vtableTableGV_) return vtableTableGV_;
    std::vector<llvm::Constant*> entries;
    for (llvm::GlobalVariable* gv : allVtables_)
        entries.push_back(llvm::ConstantExpr::getPointerCast(gv, ptrTy()));
    entries.push_back(llvm::ConstantPointerNull::get(ptrTy()));  // terminator
    auto* arrTy = llvm::ArrayType::get(ptrTy(), entries.size());
    vtableTableGV_ = new llvm::GlobalVariable(
        *module_, arrTy, /*isConstant=*/true,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(arrTy, entries), "bz$vtable$table");
    return vtableTableGV_;
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
