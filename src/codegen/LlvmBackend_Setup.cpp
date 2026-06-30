#include "codegen/LlvmBackend_impl.hpp"

#include "sema/ConstFold.hpp"

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
            // §10.2 (#739): an explicit value is a compile-time integer constant
            // — a literal, unary `+ - ~`, arithmetic, or a reference to an
            // earlier constant of this enum. The old IntLiteralExpr-only check
            // dropped a NEGATIVE value (`:= -5` is a UnaryExpr) and fell back to
            // auto-increment. foldIntConst handles all those forms; the resolver
            // looks up an already-computed sibling constant by name.
            auto resolveSibling =
                [&](std::string_view name, int) -> std::optional<long long> {
                for (const sema::Symbol* m : ts->members)
                    if (dynamic_cast<const sema::EnumConstantSymbol*>(m) &&
                        m->name == name) {
                        auto vit = enumValue_.find(m);
                        if (vit != enumValue_.end()) return vit->second;
                    }
                return std::nullopt;
            };
            if (auto folded = sema::foldIntConst(*ec.value, resolveSibling))
                val = *folded;
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
    // §10.3 (#740): materialize at the enum's underlying width (default i32),
    // so a `: long` enum constant keeps all 64 bits instead of truncating.
    return llvm::ConstantInt::get(
        enumLlvmType(enumOfConstant(sym)),
        static_cast<std::uint64_t>(it->second), /*isSigned=*/true);
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
        // §10.3 (#740): materialize at the enum's underlying width.
        return llvm::ConstantInt::get(
            enumLlvmType(enumTy), static_cast<std::uint64_t>(it->second),
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
            // quotes by renderConstExpr (e.g. "hello" → text = `"hello"`).
            // lowerStringLiteralConst calls processStringCodePoints which
            // itself strips the surrounding delimiter pair (§2.5/§22.12).
            // Do NOT pre-strip the quotes here — doing so causes
            // processStringCodePoints to then strip the first/last VALUE
            // characters, truncating "hello"→"ell" and "ab"→"" (fix #1225).
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

    // §23.1 (#743): a STATIC auto-property's backing storage is a module-level
    // global (instance auto-properties use field storage). Without it the
    // implicit get/set had nowhere to read/write — the getter returned 0 and
    // the setter was discarded. Allocate one global per static auto-property
    // (keyed on the PropertySymbol); the implicit get/set route to it via
    // staticFieldGlobal, exactly like a static field.
    for (const auto& sym : ctx_.symbols()) {
        auto* ps = dynamic_cast<const sema::PropertySymbol*>(sym.get());
        if (!ps || !isStaticAutoProperty(ps)) continue;
        auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
        llvm::Type* ty = (pd && pd->type) ? lowerType(pd->type.get()) : i32();
        std::string cls = ps->parent ? std::string(ps->parent->name) : "";
        // §23.1 (#1146): a static auto-property may carry an inline initializer
        // (`static readonly property Total: int := 42`). Encode a CONSTANT
        // initializer as the global's LLVM initializer (mirroring a static
        // field's constInitFor) — without this the global was always
        // getNullValue and the property read 0. A NON-constant initializer is
        // emitted as a runtime store during static init (the loop below).
        llvm::Constant* initC = (pd && pd->autoInitializer)
            ? constInitForExpr(pd->autoInitializer.get(), ty)
            : llvm::Constant::getNullValue(ty);
        auto* gv = new llvm::GlobalVariable(
            *module_, ty, /*isConstant=*/false,
            llvm::GlobalValue::ExternalLinkage,
            initC,
            "bz$" + cls + "$" + std::string(ps->name) + "$prop");
        staticGlobals_[ps] = gv;
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
    return constInitForExpr(fd ? fd->initializer.get() : nullptr, ty);
}

llvm::Constant* Emitter::constInitForExpr(const ast::Expr* e, llvm::Type* ty) {
    if (e) {
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
    // §23.1 (#743): a static auto-property reads/writes its backing global
    // (keyed on the PropertySymbol), exactly like a static field. No generic
    // per-instantiation handling here (a generic static auto-property is a
    // separate, rarer case — the FieldSymbol path below covers generic fields).
    if (isStaticAutoProperty(sym_raw)) {
        auto it = staticGlobals_.find(sym_raw);
        return it == staticGlobals_.end() ? nullptr : it->second;
    }
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
        // #1116: also collect the sema TypeRef* for each arg so we can
        // recompute the concrete LLVM type for the field below.
        std::vector<const sema::TypeRef*> substSemaTys;
        std::string typeArgMangle;

        // §22.12 (#738): mangle one type argument to a name fragment. A Named
        // generic argument must encode its OWN type arguments recursively —
        // otherwise `Counter<Box<int>>` and `Counter<Box<string>>` both mangle
        // their arg to "Box" and collide on a single `bz$Counter$Box$n` global,
        // so writes to one instantiation clobber the other (§22.12: "each
        // distinct instantiation has its own static state").
        std::function<std::string(const sema::TypeRef*)> mangleTy =
            [&](const sema::TypeRef* t) -> std::string {
            if (!t) return "t";
            if (t->kind == sema::TypeRef::Kind::Named && t->declaration) {
                std::string s(t->declaration->name);
                for (const sema::TypeRef* inner : t->genericArgs)
                    s += "_" + mangleTy(inner);
                return s;
            }
            if (t->kind == sema::TypeRef::Kind::Primitive) {
                switch (t->primitive) {
                    case sema::PrimitiveType::Bool:    return "bool";
                    case sema::PrimitiveType::Char:    return "char";
                    case sema::PrimitiveType::String:  return "string";
                    case sema::PrimitiveType::Byte:    return "byte";
                    case sema::PrimitiveType::SByte:   return "sbyte";
                    case sema::PrimitiveType::Short:   return "short";
                    case sema::PrimitiveType::UShort:  return "ushort";
                    case sema::PrimitiveType::Int:     return "int";
                    case sema::PrimitiveType::UInt:    return "uint";
                    case sema::PrimitiveType::Long:    return "long";
                    case sema::PrimitiveType::ULong:   return "ulong";
                    case sema::PrimitiveType::Float:   return "float";
                    case sema::PrimitiveType::Double:  return "double";
                    case sema::PrimitiveType::Decimal: return "decimal";
                    default:                           return "prim";
                }
            }
            return "t";
        };

        // Helper: append one type argument (semaTy) to the four key vectors.
        auto appendTypeArg = [&](const sema::TypeRef* semaTy) {
            llvm::Type* llTy = semaTy ? lowerTypeRef(semaTy) : i32();
            substTypes.push_back(llTy);
            substIds.push_back(typeRefIdentityToken(semaTy));
            substSemaTys.push_back(semaTy);  // #1116: track sema type for instTy
            if (!typeArgMangle.empty()) typeArgMangle += "_";
            typeArgMangle += mangleTy(semaTy);
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
            // #723: a qualified self-type access `Box<T>.n` inside the
            // monomorphized body carries the UNRESOLVED param `T` (Named,
            // declaration==nullptr) as the target's genericArg, so appendTypeArg
            // would see a no-token type and Path (B) would fall through to the
            // template-level global — distinct from the external `Box<int>.n`'s
            // per-instantiation global. Resolve a bare param through the active
            // substitution (genericSubstSemaTy_) first, mirroring
            // recordMemberTypeResolved; a genuinely unbound param (no active
            // substitution, e.g. the #715 sfinit case) still falls through.
            auto resolveArg = [&](const sema::TypeRef* arg) -> const sema::TypeRef* {
                if (!arg || genericSubstSemaTy_.empty()) return arg;
                if (arg->kind != sema::TypeRef::Kind::Named || arg->declaration)
                    return arg;
                if (arg->astNode && arg->astNode->name &&
                    arg->astNode->name->parts.size() == 1) {
                    auto it = genericSubstSemaTy_.find(
                        arg->astNode->name->parts[0]);
                    if (it != genericSubstSemaTy_.end() && it->second)
                        return it->second;
                }
                if (!arg->importedName.empty()) {
                    auto it = genericSubstSemaTy_.find(arg->importedName);
                    if (it != genericSubstSemaTy_.end() && it->second)
                        return it->second;
                }
                return arg;
            };
            if (targetTR && targetTR->kind == sema::TypeRef::Kind::Named &&
                    targetTR->declaration == fs->parent &&
                    targetTR->genericArgs.size() ==
                        fs->parent->genericParams.size()) {
                for (std::size_t i = 0; i < fs->parent->genericParams.size(); ++i)
                    appendTypeArg(resolveArg(targetTR->genericArgs[i]));
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
                substSemaTys.push_back(semaTy);  // #1116: track sema type for instTy
                if (!typeArgMangle.empty()) typeArgMangle += "_";
                typeArgMangle += mangleTy(semaTy);  // #738: recursive
            }
        }

        // Only redirect if we found at least one bound parent type param AND
        // every type argument is a concrete (resolved) type — not an unresolved
        // generic type parameter.  An unresolved parameter produces a null
        // substId (typeRefIdentityToken returns nullptr when the TypeRef has
        // kind==Named but declaration==nullptr, which is exactly how a
        // GenericParamSymbol is encoded: GenericParamSymbol is not a TypeSymbol,
        // so TypeRef::declaration is left null).
        //
        // This guards against the #715 bug: inside a generic class's sfinit
        // body (emitStaticConstructorBody), a self-qualified access such as
        // `Registry<T>.seed` reaches Path (B) above with T still an unresolved
        // param.  Without this guard, a spurious per-instantiation global
        // `bz$Registry$t$seed` is created with zero-init and is never stored
        // into, producing the stale-zero read.  The template-level global
        // (returned by the fall-through below) IS correctly initialised first
        // in the sfinit sequence, so reading it is safe.  Per §22.12 / #715.
        bool allConcrete = std::all_of(substIds.begin(), substIds.end(),
                                       [](const void* id) { return id != nullptr; });
        if (!substTypes.empty() && allConcrete) {
            // #738: include the recursive type-arg mangle in the key so nested
            // instantiations (Counter<Box<int>> vs Counter<Box<string>>), which
            // share llvm type `ptr` AND the per-declaration `Box` identity token,
            // resolve to DISTINCT per-instantiation globals (§22.12).
            auto key = std::make_pair(
                static_cast<const sema::Symbol*>(fs),
                std::make_pair(std::make_pair(substTypes, substIds),
                               typeArgMangle));
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

            // Create the per-instantiation global with the correct concrete type.
            //
            // §22.12 / #1116: the template-level global's LLVM type was lowered
            // from the field's UNSPECIALIZED type in declareStatics (no active
            // substitution there), so e.g. `owned T?` (T class-constrained) maps
            // to the placeholder {i8,i32} nullable-value-struct instead of the
            // concrete `ptr` for T=Cell.  Storing a class reference (ptr) into a
            // {i8,i32} global then loading it as {i8,i32} silently miscompiles
            // the pointer — the null-check reads the pointer's low byte (zero for
            // any heap-aligned alloc) as the hasValue tag, and the unwrapped
            // "value" is a 32-bit fragment of the address.  Recompute the
            // concrete LLVM type by temporarily installing the type-arg
            // substitution and calling lowerType, mirroring how a monomorphized
            // method body would see the field.  Per §22.12 / §28.5.
            llvm::Type* instTy = base->getValueType();
            auto* fieldDecl = dynamic_cast<const ast::FieldDecl*>(fs->decl);
            if (fieldDecl &&
                substSemaTys.size() == fs->parent->genericParams.size()) {
                auto savedSubst     = std::move(genericSubst_);
                auto savedSubstSema = std::move(genericSubstSemaTy_);
                for (std::size_t i = 0; i < fs->parent->genericParams.size(); ++i) {
                    genericSubst_[fs->parent->genericParams[i]->name] =
                        substTypes[i];
                    if (substSemaTys[i])
                        genericSubstSemaTy_[fs->parent->genericParams[i]->name] =
                            substSemaTys[i];
                }
                instTy = lowerType(fieldDecl->type.get());
                genericSubst_     = std::move(savedSubst);
                genericSubstSemaTy_ = std::move(savedSubstSema);
            }
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
            // When instTy differs from base->getValueType() (e.g. ptr vs
            // {i8,i32}), the base's typed initializer cannot be reused — use a
            // fresh null of the concrete type instead.
            llvm::Constant* instInit =
                (instTy == base->getValueType() && base->hasInitializer())
                    ? base->getInitializer()
                    : llvm::Constant::getNullValue(instTy);
            auto* gv = new llvm::GlobalVariable(
                *module_, instTy, /*isConstant=*/false,
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
        // §14.1 / §2.5 / #783: an IMPORTED method must NOT be emitted as a
        // defined function here. Its template body lives (compiled) in the
        // defining module — emitting `bz$<Class>$<M>` again would multiply-define
        // that symbol at link time. Generic-class imported methods now carry a
        // re-parsed ast::MethodDecl (schema v5 methodBodyText) so callableInfo
        // succeeds, but that decl is for PER-INSTANTIATION inline monomorphization
        // at the call site (emitInlineGenericMethod), not for a standalone
        // definition. Skip every imported method (matching the pre-#783 behaviour,
        // which relied on callableInfo failing for the no-decl case).
        if (ctx_.isImported(ms)) continue;
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
                // CG30 / §10.3 (#740): enum instance methods take the enum value
                // (its underlying integer, default i32) as `this`, not a pointer
                // — enums are value types.
                bool isEnumMethod = ms->parent &&
                    ms->parent->kind == ast::TypeDeclKind::Enum;
                paramTys.push_back(
                    isEnumMethod ? enumLlvmType(ms->parent) : ptrTy());
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
            // CG50 / §22.3 (#967): a `ref`/`out` constructor parameter is passed
            // by reference — the caller (lowerNew) passes the storage address, so
            // its slot type is a pointer, exactly as for a method ref/out param
            // (above). Without this the signature declared the value type while
            // lowerNew passed a pointer and bindParameters read through it as a
            // ref param → malformed IR.
            paramTys.push_back(isByRefMod(p.modifier)
                                   ? ptrTy()
                                   : lowerType(p.type.get()));
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
                sfinitFnOf_[ts] = fn;  // #720: index for ensureinit
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
            if (cs->parent) cctorFnOf_[cs->parent] = fn;  // #720
        }
    }

    // #720 / §22.13 / §22.7: build the lazy `bz$<T>$ensureinit()` functions for
    // every non-generic type with static state. Emitted here (after the sfinit/
    // cctor declarations exist) so static-access sites can call them.
    emitEnsureInitFns();

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
        // #735: an imported ENUM instance method's `this` is the enum's
        // underlying integer (i32), passed BY VALUE — matching the exporter's
        // CG30 lowering — NOT a pointer. Declaring it as ptr made the importer's
        // call pass a ptr where the exported `bz$Enum$M(i32)` expects an i32, so
        // the receiver was read as garbage. Mirror enumTypeOf/CG30.
        if (!isStaticMethod(ms)) {
            // §10.3 (#740): the enum's underlying width (default i32, and i32 for
            // an imported enum whose metadata does not carry the underlying type).
            bool isEnumMethod =
                ms->parent && ms->parent->kind == ast::TypeDeclKind::Enum;
            paramTys.push_back(
                isEnumMethod ? enumLlvmType(ms->parent) : ptrTy());
        }
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
    // Vtable (CG35-itable + §8.6 #417 + §9.6 #946): [0]=td$C, [1]=itmap$C,
    // [2]=Dispose, [3]=ToString, [4]=Equals(Object?), [5]=GetHashCode,
    // [6..]=virtual members. So a virtual member's slot is its position in
    // `slots` + kVirtualMethodsBase (the three Object methods ToString/Equals/
    // GetHashCode occupy fixed slots 3/4/5, like Dispose occupies fixed slot 2).
    // See declareVtables Pass D for the slot fill.
    //
    // #481 / §12.1 / §12.5 / §23: virtual METHODS and virtual PROPERTY accessors
    // are enumerated together in member declaration order, each getting its own
    // slot. An `override` (method or property accessor) reuses the base member's
    // slot, matched by name (a setter uses "name$set" so get/set are distinct).
    // For a method-only class the layout is exactly the pre-#481 one (a property
    // just appends/interleaves additional slots); the IR verifier + the existing
    // virtual/interface dispatch tests guard against a perturbed method index.
    const unsigned base = kVirtualMethodsBase;
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
            // #1085: key the vtable slot by name + ARITY, not the bare name.
            // Two virtual OVERLOADS sharing a name (`F(x)` / `F(x, y)`) were
            // keyed identically, so the second collapsed onto the first's slot
            // (last-writer-wins) and every call to the earlier overload
            // dispatched to the wrong body. Arity is the disambiguator the rest
            // of the vtable/itable machinery already uses (interface-impl and
            // abstract-impl matching are arity-based), and it keeps the SAME key
            // for a base virtual and its derived override of the same arity so
            // the override correctly reuses the base slot. The `#` separator
            // can't appear in an identifier or the property `$set` suffix, so a
            // method `F#0` never collides with a property `F` / setter `F$set`.
            // Single-overload classes keep their exact slot layout (distinct
            // names → distinct keys either way; only the override-reuse lookup
            // is affected). Dispatch is unchanged — it is keyed by MethodSymbol*
            // (vtableSlot_[ms]), which now receives a distinct slot per overload.
            vtableSlot_[ms] = assignSlot(
                std::string(ms->name) + "#" +
                    std::to_string(ms->params.size()),
                VSlot{ms, nullptr, false});
            continue;
        }
        if (auto* ps = dynamic_cast<sema::PropertySymbol*>(m)) {
            // #1080: a virtual indexer also gets a slot. Its key is the
            // index-signature key (so overloaded indexers `[i:int]`/`[s:bool]`
            // get distinct slots while a base/derived override of the same
            // signature aligns). The slot fill (declareVtables Pass D) uses the
            // INDEXER accessor builder (its signature carries the index params),
            // and lowerIndexerGet/Set dispatch through it.
            bool vIdx = isVirtualIndexer(ps);
            if (!isVirtualProperty(ps) && !vIdx) continue;
            std::string key = vIdx ? indexerSlotKey(ps) : std::string(ps->name);
            propGetSlot_[ps] = assignSlot(key, VSlot{nullptr, ps, false});
            if (propertyHasSetter(ps))
                propSetSlot_[ps] = assignSlot(key + "$set",
                                              VSlot{nullptr, ps, true});
        }
    }
    vtableMethods_[t] = std::move(slots);
    vtableNameSlot_[t] = std::move(nameSlot);
}


// fix #1139 / §11.2 nullability-widening: generate an adapter thunk for an
// itable slot where the implementing method declares T? for a parameter that
// the interface declares as T.  The thunk has the interface's ABI and wraps
// each widened argument (T → {i8 1, T}) before forwarding to the impl body.
// Cached; returns nullptr when no ABI mismatch exists (direct fn ptr suffices).
llvm::Function* Emitter::ensureNullableWideningThunk(
    const sema::MethodSymbol* ifaceMs,
    const sema::MethodSymbol* implMs)
{
    // Fetch the implementation's compiled LLVM function.
    auto fIt = methodFns_.find(const_cast<sema::MethodSymbol*>(implMs));
    if (fIt == methodFns_.end()) return nullptr;
    llvm::Function* implFn = fIt->second;
    if (!implFn) return nullptr;

    // Get both function types and compare.
    llvm::FunctionType* ifaceTy = methodFnType(ifaceMs);
    if (!ifaceTy) return nullptr;
    llvm::FunctionType* implTy = implFn->getFunctionType();

    // No mismatch → use the direct function pointer (no thunk needed).
    if (ifaceTy == implTy) return nullptr;
    if (ifaceTy->getNumParams() != implTy->getNumParams()) return nullptr;
    bool needsThunk = false;
    for (unsigned i = 0; i < ifaceTy->getNumParams(); ++i)
        if (ifaceTy->getParamType(i) != implTy->getParamType(i))
            { needsThunk = true; break; }
    if (!needsThunk) return nullptr;

    // #1183 / §11.2 / §12.1: only emit a thunk for NULLABLE-WIDENING
    // mismatches (impl declares T? where iface declares T, e.g. {i8,T}
    // vs T). For GENERIC-T erasure mismatches (iface has i32 as the T
    // placeholder, impl has the substituted concrete T — e.g. a record
    // struct or ptr for string) the call-site fix in emitMethodCall
    // rebuilds the concrete call type from the actual argument types.
    // A thunk here would emit a malformed forwarding call (passing an
    // i32 thunk-arg to a concrete-T parameter). Skip the thunk whenever
    // any mismatch is not a nullable-value widening.
    for (unsigned ci = 1; ci < ifaceTy->getNumParams(); ++ci) {
        if (ifaceTy->getParamType(ci) == implTy->getParamType(ci)) continue;
        unsigned pIdx = ci - 1;
        sema::TypeRef* pr = nullptr;
        if (pIdx < implMs->params.size()) {
            auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                           implMs->params[pIdx]);
            if (ps) pr = ps->resolvedDeclType
                          ? ps->resolvedDeclType
                          : (ps->declType ? ctx_.resolvedType(*ps->declType)
                                          : nullptr);
        }
        if (!pr) {
            auto* md = dynamic_cast<const ast::MethodDecl*>(implMs->decl);
            if (md && pIdx < md->params.size() && md->params[pIdx].type)
                pr = ctx_.resolvedType(*md->params[pIdx].type);
        }
        // If the mismatch is not a nullable-value widening (impl param is
        // not T?), a thunk cannot be correctly generated — return nullptr
        // so the itable slot holds the direct concrete function.
        if (!pr || !isNullableValueTy(pr)) return nullptr;
    }

    // Cache lookup.
    auto key = std::make_pair(ifaceMs, implMs);
    if (auto it = nullableWideningThunks_.find(key);
        it != nullableWideningThunks_.end())
        return it->second;

    // Emit the thunk with the interface's signature.
    std::string name = "bz$nw$" +
                       std::string(implMs->parent ? implMs->parent->name : "") +
                       "$" + std::string(implMs->name);
    auto* thunk = llvm::Function::Create(
        ifaceTy, llvm::GlobalValue::InternalLinkage, name, module_.get());
    nullableWideningThunks_[key] = thunk;

    // Save/restore builder state (same pattern as emitEventAccessor).
    llvm::Function* savedFn = currentFn_;
    llvm::BasicBlock* savedBB = builder_.GetInsertBlock();
    currentFn_ = thunk;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", thunk);
    builder_.SetInsertPoint(entry);

    // Build the argument list for the impl call, wrapping widened params.
    std::vector<llvm::Value*> implArgs;
    unsigned paramIdx = 0;
    for (llvm::Argument& arg : thunk->args()) {
        llvm::Type* ifaceArgTy = ifaceTy->getParamType(paramIdx);
        llvm::Type* implArgTy  = implTy ->getParamType(paramIdx);
        if (implArgTy == ifaceArgTy || paramIdx == 0) {
            // Receiver (ptr) or type-identical param: pass through unchanged.
            implArgs.push_back(&arg);
        } else {
            // §11.2 rule-1 widening: impl expects T? ({i8,T}), iface passed T.
            // Build {hasValue=1, value=arg} using the impl param's resolved type.
            unsigned pIdx = paramIdx - 1;  // 0-based, excluding receiver
            sema::TypeRef* implParamRef = nullptr;
            if (pIdx < implMs->params.size()) {
                auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                               implMs->params[pIdx]);
                if (ps) {
                    // Prefer the pre-resolved TypeRef; fall back to resolving
                    // from the AST type when resolvedDeclType was not populated
                    // (which can happen for method-symbol params at itable-build
                    // time — sema populates resolvedDeclType lazily per call-site).
                    if (ps->resolvedDeclType)
                        implParamRef = ps->resolvedDeclType;
                    else if (ps->declType)
                        implParamRef = ctx_.resolvedType(*ps->declType);
                }
            }
            // If still null, fall back via the impl method's AST MethodDecl.
            if (!implParamRef) {
                auto* md = dynamic_cast<const ast::MethodDecl*>(implMs->decl);
                if (md && pIdx < md->params.size() && md->params[pIdx].type)
                    implParamRef = ctx_.resolvedType(*md->params[pIdx].type);
            }
            llvm::Value* wrapped = nullptr;
            if (implParamRef && isNullableValueTy(implParamRef))
                wrapped = wrapNullableValue(implParamRef, &arg);
            else
                wrapped = &arg;  // fallback (§11.2-valid programs never reach here)
            implArgs.push_back(wrapped);
        }
        ++paramIdx;
    }

    llvm::Value* result = builder_.CreateCall(implTy, implFn, implArgs,
                                              "nw.ret");
    if (ifaceTy->getReturnType()->isVoidTy())
        builder_.CreateRetVoid();
    else
        builder_.CreateRet(result);

    currentFn_ = savedFn;
    if (savedBB) builder_.SetInsertPoint(savedBB);
    return thunk;
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
        // #730 / §23.4: interface INDEXERS (`property [i: T]: U;`) participate
        // too, interleaved with named properties in declaration order. The fill
        // in Pass D walks the same member order, so positions stay consistent.
        for (sema::Symbol* m : ts->members) {
            auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
            if (!ps) continue;
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
            // #730 / §23.4: include indexers — same member order as Pass 0.
            std::vector<sema::PropertySymbol*> ifaceProps;
            for (sema::Symbol* m : iface->members) {
                auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
                if (ps) ifaceProps.push_back(ps);
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
                if (sema::MethodSymbol* impl = findInterfaceImpl(ts, im)) {
                    auto mit = methodFns_.find(impl);
                    if (mit != methodFns_.end()) {
                        // fix #1139 / §11.2: if the impl widens a parameter
                        // to T? where the interface declares T, their LLVM
                        // ABIs differ ({i8,T} vs T).  Install an adapter thunk
                        // that wraps each widened arg before forwarding.
                        if (llvm::Function* thunk =
                                ensureNullableWideningThunk(im, impl))
                            fp = llvm::cast<llvm::Constant>(thunk);
                        else
                            fp = llvm::cast<llvm::Constant>(mit->second);
                    }
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
                // #730 / §23.4: an interface INDEXER's slot holds the class's
                // concrete indexer accessor (`T get(ptr this, idx...)` /
                // `void set(ptr this, idx..., value)`), matched by index-param
                // type rather than name (all indexers are anonymous ".this").
                if (ip->isIndexer) {
                    const sema::PropertySymbol* iimpl =
                        classIndexerImplementing(ts, ip);
                    llvm::Constant* getFp =
                        llvm::ConstantPointerNull::get(ptrTy());
                    if (iimpl)
                        if (llvm::Function* acc = ensureIndexerGetterFn(ts, iimpl))
                            getFp = llvm::cast<llvm::Constant>(acc);
                    itabElems.push_back(getFp);
                    if (propertyHasSetter(ip)) {
                        llvm::Constant* setFp =
                            llvm::ConstantPointerNull::get(ptrTy());
                        if (iimpl)
                            if (llvm::Function* acc =
                                    ensureIndexerSetterFn(ts, iimpl))
                                setFp = llvm::cast<llvm::Constant>(acc);
                        itabElems.push_back(setFp);
                    }
                    continue;
                }
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

        // Build vtable (CG35-itable + §8.6/§8.5 #417 + §9.6 #946): [0]=td$C,
        // [1]=itmap$C, [2]=Dispose, [3]=ToString, [4]=Equals, [5]=GetHashCode,
        // [6..]=virtuals.
        const auto& slots = vtableMethods_[ts];
        std::vector<llvm::Constant*> elems(kVirtualMethodsBase + slots.size(),
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
        // §9.6 (#946/#947/#948): uniform Equals(Object?) at fixed slot 4 and
        // GetHashCode at fixed slot 5 — the user override (via the class chain)
        // or the §8.3 reference-identity / identity-hash default. This makes an
        // override reachable through an Object-typed receiver.
        elems[kEqualsSlot] = classEqualsThunk(ts);
        elems[kGetHashCodeSlot] = classHashCodeThunk(ts);
        // Virtual members at [6..] (kVirtualMethodsBase + i). A method slot holds
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
                    elems[kVirtualMethodsBase + i] =
                        llvm::cast<llvm::Constant>(mit->second);
            } else if (vs.prop) {
                // #481 / §12.5: resolve the most-derived accessor on `ts`'s
                // chain. classPropertyByName returns the override if present,
                // else the base declaration (abstract → no body → null fn).
                // #1080: classPropertyByName skips indexers, so an indexer slot
                // resolves its implementation via classIndexerMatching (by index
                // signature, so an overloaded indexer fills its own slot).
                sema::PropertySymbol* impl =
                    vs.prop->isIndexer
                        ? classIndexerMatching(ts, vs.prop)
                        : classPropertyByName(ts, vs.prop->name);
                if (impl) {
                    // #1080: an indexer slot carries the INDEXER accessor (its
                    // signature has the index params), not the plain
                    // `T get(ptr this)` property accessor.
                    llvm::Function* fn =
                        vs.prop->isIndexer
                            ? (vs.isSet ? ensureIndexerSetterFn(ts, impl)
                                        : ensureIndexerGetterFn(ts, impl))
                            : ensurePropertyAccessorFn(ts, impl, vs.isSet);
                    if (fn)
                        elems[kVirtualMethodsBase + i] =
                            llvm::cast<llvm::Constant>(fn);
                }
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
        // issue #976 / stdlib §7.4: System.Threading TimeoutException.
        {"TimeoutException", "Exception"},
        // issue #976 / stdlib §7.5: the System.IO exception family. All derive
        // from IOException, which derives from Exception. Registered so
        // `catch var ex: IOException` matches a thrown FileNotFoundException
        // (etc.) via the descriptor base-chain, and `throw new IOException(...)`
        // lowers like any other built-in exception.
        {"IOException", "Exception"},
        {"FileNotFoundException", "IOException"},
        {"DirectoryNotFoundException", "IOException"},
        {"EndOfStreamException", "IOException"},
        {"PathTooLongException", "IOException"},
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
        // #805 / §2.5 / §31: the built-in exception type descriptor is emitted
        // with LinkOnceODR (not Internal) linkage so all modules share ONE
        // descriptor symbol per built-in exception type. emitIsA matches by
        // descriptor-POINTER equality; with per-module (internal) descriptors a
        // `catch Exception` in an importer could never match an exception thrown
        // by an imported module (distinct addresses). The built-in exception
        // hierarchy is fixed, so every module's `btd$<name>` is the same chain —
        // ODR-mergeable. The address stays significant (no unnamed_addr): isA
        // depends on a single canonical address per type.
        builtinDesc_[name] = new llvm::GlobalVariable(
            *module_, arrTy, /*isConstant=*/true,
            llvm::GlobalValue::LinkOnceODRLinkage, nullptr,
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


// §19 / §A.8 (#957): a NamedPattern whose qualified name resolves to a named
// CONST (`C.K`, or a bare own-class/module const) is a constant-equality
// pattern, not a type test. The pattern name carries no resolved-symbol entry
// (a QualifiedName, not an Expr), so resolve it by name against the const
// symbols and fold its compile-time value (the same fold constMemberValue uses
// for a const READ). Returns null when the name is not a const, leaving the
// type-test / enum-constant handlers (or the deferred-null fall-through) to
// take over.
llvm::Value* Emitter::constValueForPatternName(
        const ast::QualifiedName* name) {
    if (!name || name->parts.empty()) return nullptr;
    std::string_view last = name->parts.back();
    auto fold = [&](sema::ConstMemberSymbol* cs) -> llvm::Value* {
        auto* cd = dynamic_cast<const ast::ConstDecl*>(cs->decl);
        if (cd && cd->value) return lowerExpr(*cd->value);
        return nullptr;  // imported const w/o AST decl — decline (loud)
    };
    // Qualified `C.K`: `K` is a const member of the type named just before it.
    if (name->parts.size() >= 2) {
        std::string_view typeName = name->parts[name->parts.size() - 2];
        for (const auto& sym : ctx_.symbols()) {
            auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
            if (!ts || ts->name != typeName) continue;
            for (sema::Symbol* m : ts->members)
                if (m && m->name == last)
                    if (auto* cs = dynamic_cast<sema::ConstMemberSymbol*>(m))
                        return fold(cs);
        }
        return nullptr;
    }
    // Bare `K`: accept only a UNIQUE const of that name (an ambiguous bare name
    // is left deferred — never mis-resolved to an unrelated class's const). The
    // same symbol can appear both flat in ctx_.symbols() and under its owning
    // type's `members`, so dedup by pointer before counting.
    std::unordered_set<const sema::ConstMemberSymbol*> seen;
    auto consider = [&](sema::Symbol* m) {
        if (auto* cs = dynamic_cast<sema::ConstMemberSymbol*>(m))
            if (cs->name == last) seen.insert(cs);
    };
    for (const auto& sym : ctx_.symbols()) {
        consider(sym.get());
        if (auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get()))
            for (sema::Symbol* m : ts->members) consider(m);
    }
    return seen.size() == 1
               ? fold(const_cast<sema::ConstMemberSymbol*>(*seen.begin()))
               : nullptr;
}


void Emitter::emitThrowBuiltin(const char* name) {
    auto vit = builtinVtable_.find(name);
    if (vit == builtinVtable_.end()) return;
    // 16-byte object: slot0=vtable, slot1=empty message ptr (calloc zeros it).
    llvm::Value* obj = builder_.CreateCall(
        callocFn(), {builder_.getInt64(1), builder_.getInt64(16)});
    builder_.CreateStore(vit->second, obj);
    builder_.CreateStore(obj, emitExcSlot());
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
        // #866: free the caught exception ONLY when this throw actually LEAVES
        // the enclosing catch arm — i.e. its unwind target is the arm-leaving
        // target. A throw caught by an INNER try/catch within the arm targets
        // that inner dispatch instead; the arm continues afterwards and the
        // caught exception is still in flight, so freeing it here would
        // double-free it at the arm's own exit (crash).
        bool throwLeavesArm =
            caughtExcArmTarget_ && unwindBB() == caughtExcArmTarget_;
        if (currentCaughtExc_ && excVal != currentCaughtExc_ &&
            !isDirectRethrow && throwLeavesArm) {
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
        builder_.CreateStore(excVal, emitExcSlot());
    } else if (currentCaughtExc_) {
        // Bare `throw` re-raises the arm's caught exception (the global
        // was cleared when the arm was entered).
        builder_.CreateStore(currentCaughtExc_, emitExcSlot());
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
    // via bz_shared_release / bz_weak_release),
    // or — §28.5/§24.4 (#879) — a Subscription handle (unsubscribed via
    // bz_subscription_dispose, which is null-safe).
    struct OwnedField {
        const sema::FieldSymbol* fs;
        const sema::TypeRef* collTy;  // non-null → collection field
        // §28.13 (#620): true for Shared<T> owned field (bz_shared_release),
        // false for ordinary class/exception/collection field.
        bool sharedField = false;
        // §28.13 (#620): true for Weak<T> owned field (bz_weak_release).
        bool weakField = false;
        // §28.5/§24.4 (#879): true for Subscription (or Subscription?) owned
        // field (bz_subscription_dispose — null-safe, so no guard needed).
        bool subscriptionField = false;
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
        } else if (isOwnedSubscriptionField(fs)) {
            // §28.5/§24.4 (#879): an `owned` field of Subscription (or
            // Subscription?) type.  Subscription is a compiler-known opaque
            // handle with no vtable; unsubscribe via bz_subscription_dispose
            // (null-safe: `if (!subp) return;` — so Subscription? is safe too).
            OwnedField of{fs, nullptr};
            of.subscriptionField = true;
            owned.push_back(of);
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
            //
            // §28.5/§28.11, #893 #894: OwnedDictionary<K,V> owns its V values
            // (genericArgs[1]), NOT its K keys (genericArgs[0]).  Free via
            // bz_dict_free(ptr, valueDisposer) so class V elements are disposed.
            // Array/Stack/Queue/LinkedList/Buffer own their single generic element
            // (genericArgs[0]) and use bz_array_free / bz_llist_free as before.
            bool isDict = isDictTypeName(it->collTy);
            const sema::TypeRef* collElem =
                isDict
                    ? ((it->collTy->genericArgs.size() > 1)
                           ? it->collTy->genericArgs[1]
                           : nullptr)
                    : ((!it->collTy->genericArgs.empty())
                           ? it->collTy->genericArgs[0]
                           : nullptr);
            // #816: in a per-instantiation (specialized) disposer the element may
            // be a generic type PARAMETER (`owned _arr: OwnedArray<T>`); resolve it
            // to the concrete substituted type so elementPolicyFor classifies it
            // (and builds the element disposer thunk) — otherwise a bare `T`
            // element has a null declaration → Value policy → elements leak. A
            // no-op outside a substitution (the shared disposer keeps the bare T).
            collElem = resolveTypeParamRef(collElem);
            bool collIsOwning =
                it->collTy->astNode && it->collTy->astNode->name &&
                !it->collTy->astNode->name->parts.empty() &&
                it->collTy->astNode->name->parts[0].rfind("Owned", 0) == 0;
            ElementPolicy fieldCollPol = elementPolicyFor(collElem, collIsOwning);
            llvm::Function* elemDisp = fieldCollPol.disposerThunk;
            if (!elemDisp && !isDict &&
                    fieldCollPol.cat == ElementPolicy::Category::RuntimeManaged &&
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
            // §28.5/§28.11, #893: OwnedDictionary fields free via bz_dict_free.
            bool isLL = isLinkedListTypeName(it->collTy);
            const char* freeName = isLL   ? "bz_llist_free"
                                  : isDict ? "bz_dict_free"
                                  :          "bz_array_free";
            auto* fn = declareC(freeName,
                                llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
            builder_.CreateCall(fn, {val, disp});
            continue;
        }
        // §28.5/§24.4 (#879): owned Subscription (or Subscription?) field —
        // unsubscribe via bz_subscription_dispose (null-safe: first thing it
        // does is `if (!subp) return;`, so Subscription? left null is safe).
        // Null the field after so a re-entrant dispose is a no-op.
        if (it->subscriptionField) {
            auto* dispFn = declareC("bz_subscription_dispose",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(dispFn, {val});
            builder_.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), addr);
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

    // §15.1.1 line 1957 / §15.3 (#747): a move-only OWNING inline-array FIELD
    // ([N owned]T? / [N owned]T) — "for class fields, the enclosing class's
    // destructor invokes the disposal." Each non-null slot's object is disposed
    // via its own vtable, then nulled (the #396 slot-null precedent guards a
    // re-entrant pass). A slot left moved-from via `move this.field[i]` was
    // nulled at the move (lowerMoveFromCollectionSlot), so the null-guard skips
    // it — no double-free. Unlike the owned-reference fields above there is no
    // single object to dispatch on; the field is a [N x ptr] sub-aggregate.
    {
        llvm::StructType* st = classStruct(ts);
        for (const sema::Symbol* m : ts->members) {
            auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
            if (!fs) continue;
            if (fieldIndex_.find(fs) == fieldIndex_.end()) continue;
            const sema::TypeRef* ft = nullptr;
            if (auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl)) {
                if (fd->type) ft = ctx_.resolvedType(*fd->type);
            }
            if (!ft || ft->kind != sema::TypeRef::Kind::InlineArray ||
                !ft->arrayOwned)
                continue;
            unsigned n = ft->arraySizeValue >= 0
                ? (unsigned)ft->arraySizeValue : 0;
            if (n == 0) continue;
            unsigned idx = fieldIndex_[fs];
            llvm::Value* fieldAddr =
                builder_.CreateStructGEP(st, currentThis_, idx, "ioaf.addr");
            auto* arrTy = llvm::ArrayType::get(ptrTy(), n);
            for (unsigned i = 0; i < n; ++i) {
                llvm::Value* slotAddr = builder_.CreateInBoundsGEP(
                    arrTy, fieldAddr,
                    {builder_.getInt32(0), builder_.getInt32(i)}, "ioaf.slot");
                llvm::Value* obj =
                    builder_.CreateLoad(ptrTy(), slotAddr, "ioaf.val");
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    obj, llvm::ConstantPointerNull::get(ptrTy()));
                auto* dispBB = llvm::BasicBlock::Create(llvm_, "ioaf.dispose",
                                                        currentFn_);
                auto* contBB = llvm::BasicBlock::Create(llvm_, "ioaf.cont",
                                                        currentFn_);
                builder_.CreateCondBr(isNull, contBB, dispBB);
                builder_.SetInsertPoint(dispBB);
                emitDisposeAndReclaim(obj);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), slotAddr);
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
            }
        }
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

    // #480 / §24.1 / §24.5 / §28 (#1176): free each instance event's backing
    // EventSource. emitInstanceInit allocated one bz_event_source_new per
    // instance EventSymbol (sync and async); those allocations are NOT visited
    // by the FieldSymbol loop above because EventSymbol does not inherit from
    // FieldSymbol and the dynamic_cast returns null.  Collect all instance-event
    // symbols that have a fieldIndex_ entry (non-static events, the same
    // predicate as collectInstanceStorage) and emit a null-guarded
    // bz_event_source_free call for each, then store null back so a re-entrant
    // dispose is a no-op.  Ordering relative to FieldSymbol disposals is not
    // safety-critical: EventSource.free (handler array + struct) does not
    // cascade into any owned sub-objects tracked by this class.
    llvm::StructType* st = classStruct(ts);
    auto* freeFn = declareC("bz_event_source_free",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
    for (const sema::Symbol* m : ts->members) {
        auto* es = dynamic_cast<const sema::EventSymbol*>(m);
        if (!es) continue;
        auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl);
        if (!ed || ed->isStatic) continue;  // instance events (sync + async)
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
        if (!fs) continue;
        // (a) a direct owned type-parameter field (`owned _f: T`).
        if (isOwnedTypeParamFieldShape(fs)) return true;
        // #816 (b): a direct owned generic OWNING-collection field whose element
        // is a type parameter (`owned _arr: OwnedArray<T>`) — its elements are
        // only disposed by a per-instantiation disposer.
        if (isOwnedTypeParamCollectionField(fs)) return true;
    }
    // #816 (c): a class whose (transitive) BASE has an owned type-parameter field
    // (`class Child<T> : Base<T>` where Base has `owned _f: T`) must ALSO get a
    // specialized disposer, so its emitBaseDisposeChain can invoke the base's
    // SUBSTITUTED disposer (the shared base disposer is type-erased and skips the
    // base's owned-T field — the leak in #816 repro (a)).
    for (const sema::TypeSymbol* base : ts->baseTypes)
        if (base && base->kind == ast::TypeDeclKind::Class &&
            classHasOwnedTypeParamField(base))
            return true;
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
        kVirtualMethodsBase + slots.size(),
        llvm::ConstantPointerNull::get(ptrTy()));
    elems[0] = llvm::cast<llvm::Constant>(descGV);
    elems[1] = llvm::ConstantExpr::getPointerCast(itmapGV, ptrTy());
    elems[2] = llvm::cast<llvm::Constant>(specDispose);
    elems[kToStringSlot] = classToStringThunk(ts);
    // §9.6 (#946/#947/#948): fixed Equals/GetHashCode slots, same as a local
    // class vtable, so an Object-typed receiver of this specialization dispatches.
    elems[kEqualsSlot] = classEqualsThunk(ts);
    elems[kGetHashCodeSlot] = classHashCodeThunk(ts);
    for (std::size_t i = 0; i < slots.size(); ++i) {
        const VSlot& vs = slots[i];
        if (vs.method) {
            auto mit = methodFns_.find(vs.method);
            if (mit != methodFns_.end())
                elems[kVirtualMethodsBase + i] =
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


// §28.5 / §14 / #1143 / #1240: the per-instantiation vtable to install in a
// freshly-constructed instance of the active monomorphization of `ts`, in
// place of the shared `vt$<T>`. Specialized when:
//
// (a) The class has an owned type-parameter field: the shared (type-erased)
//     disposer cannot classify the owned field — a per-instantiation Dispose
//     (slot 2) must use the concrete substituted type. This is the §28.5 case.
//
// (b) The class implements an interface and either:
//     (b1) an interface method has a class-typed return or param (T bound to a
//          reference type): the shared itable stores the i32-placeholder template
//          function which truncates the pointer to 32 bits (SIGSEGV on any
//          vtable access through the returned value). §12/§13/§14 (#1143).
//     (b2) the concrete struct layout differs from the template layout — any
//          field's LLVM type changes under the substitution (e.g. T→ptr vs
//          T→i32): template methods have GEP indices compiled against the
//          template struct, so they land at the wrong byte offsets in the
//          concrete object's memory even when the method's own signature
//          doesn't involve T at all. §12/§14 (#1240).
//
// Falls back to the shared vtable when neither condition holds. Cached per
// (class, spec-field-type vector) so each distinct instantiation builds once.
llvm::GlobalVariable* Emitter::specializedVtableFor(const sema::TypeSymbol* ts) {
    llvm::GlobalVariable* shared = vtableOf(ts);
    if (!shared || genericSubst_.empty()) return shared;

    bool needsSpecDispose = classHasOwnedTypeParamField(ts);

    // #1240 (§12/§14): detect when the concrete struct layout under the active
    // genericSubst_ differs from the template layout (built with T→i32 erasing).
    // structs_[ts] is the template struct created without a substitution active;
    // classStruct(ts) with genericSubst_ active returns the concrete specialised
    // struct. If they are different struct types, at least one field's LLVM type
    // changed — every itable method slot must be re-emitted so its body uses GEP
    // offsets against the correct (concrete) struct type, not the template struct.
    bool layoutDiffers = false;
    {
        auto sit = structs_.find(ts);
        if (sit != structs_.end() && classStruct(ts) != sit->second)
            layoutDiffers = true;
    }

    // Condition (b): scan the class's interfaces for any method whose concrete
    // function type (under the active genericSubst_) differs from the compiled
    // template function's type, OR when the struct layout itself changed (#1240).
    bool needsSpecItable = false;
    {
        std::vector<const sema::TypeSymbol*> supers;
        collectTypeAndBases(ts, supers);
        for (const sema::TypeSymbol* iface : supers) {
            if (iface->kind != ast::TypeDeclKind::Interface) continue;
            // #1240: if the struct layout changed, every itable slot needs
            // re-emitting — trigger rebuild on the first interface we find.
            if (layoutDiffers) { needsSpecItable = true; break; }
            for (sema::Symbol* m : iface->members) {
                auto* im = dynamic_cast<sema::MethodSymbol*>(m);
                if (!im || isStaticMethod(im)) continue;
                sema::MethodSymbol* impl = findInterfaceImpl(ts, im);
                if (!impl) continue;
                auto mit = methodFns_.find(impl);
                if (mit == methodFns_.end()) continue;
                llvm::FunctionType* concFnTy = methodFnType(impl);
                if (concFnTy && concFnTy != mit->second->getFunctionType()) {
                    needsSpecItable = true;
                    break;
                }
            }
            if (needsSpecItable) break;
        }
    }

    if (!needsSpecDispose && !needsSpecItable) return shared;

    // Key by the spec field types so Box<Elem> and Box<int> get distinct vtables.
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

    std::string suffix = "$spec" + std::to_string(specMap.size());

    // (a) Specialized disposer — only when the class has an owned T field.
    llvm::Function* specDispose = nullptr;
    if (needsSpecDispose)
        specDispose = emitSpecializedDisposer(ts, suffix);

    // (b) Specialized itable(s) + itmap — only when interface method types differ.
    // per §12/§13/§14 (#1143): for each interface the class implements, take the
    // existing itable (already has correct event/property slots from Pass D) and
    // replace each method slot whose concrete type differs from the template with
    // a buildSpecializedMethodFn result under the active genericSubst_.
    llvm::GlobalVariable* specItmapGV = nullptr;
    if (needsSpecItable) {
        std::vector<const sema::TypeSymbol*> supers;
        collectTypeAndBases(ts, supers);
        std::vector<llvm::Constant*> newItmapElems;
        for (const sema::TypeSymbol* iface : supers) {
            if (iface->kind != ast::TypeDeclKind::Interface) continue;
            std::vector<sema::MethodSymbol*> ifaceMethods;
            for (sema::Symbol* m : iface->members) {
                auto* ms = dynamic_cast<sema::MethodSymbol*>(m);
                if (ms && !isStaticMethod(ms)) ifaceMethods.push_back(ms);
            }
            auto itabIt = itables_.find({ts, iface});
            if (itabIt == itables_.end()) continue;
            llvm::GlobalVariable* existingItab = itabIt->second;
            auto* existingInit =
                llvm::cast<llvm::ConstantArray>(existingItab->getInitializer());
            unsigned numSlots = existingInit->getType()->getNumElements();
            std::vector<llvm::Constant*> newItabElems;
            newItabElems.reserve(numSlots);
            for (unsigned i = 0; i < numSlots; ++i)
                newItabElems.push_back(existingInit->getAggregateElement(i));
            for (unsigned mi = 0; mi < ifaceMethods.size() && mi < numSlots; ++mi) {
                sema::MethodSymbol* impl = findInterfaceImpl(ts, ifaceMethods[mi]);
                if (!impl) continue;
                auto mit = methodFns_.find(impl);
                if (mit == methodFns_.end()) continue;
                llvm::FunctionType* concFnTy = methodFnType(impl);
                if (!concFnTy) continue;
                // #1240: when the struct layout differs from the template,
                // rebuild every slot — not only those whose function type
                // changed — because the template body has GEP indices computed
                // against the template struct type (wrong byte offsets for the
                // concrete instantiation). buildSpecializedMethodFn re-emits
                // the body under the active genericSubst_, using classStruct(ts)
                // which returns the concrete struct type with correct offsets.
                if (!layoutDiffers && concFnTy == mit->second->getFunctionType())
                    continue;
                CallableInfo ci;
                if (!callableInfo(impl, ci)) continue;
                std::vector<llvm::Type*> paramTys;
                for (unsigned pi = 0; pi < concFnTy->getNumParams(); ++pi)
                    paramTys.push_back(concFnTy->getParamType(pi));
                if (llvm::Function* specFn =
                        buildSpecializedMethodFn(impl, ci, paramTys))
                    newItabElems[mi] = llvm::cast<llvm::Constant>(specFn);
            }
            auto* newItabArrTy = llvm::ArrayType::get(ptrTy(), newItabElems.size());
            auto* newItabGV = new llvm::GlobalVariable(
                *module_, newItabArrTy, /*isConstant=*/true,
                llvm::GlobalValue::InternalLinkage,
                llvm::ConstantArray::get(newItabArrTy, newItabElems),
                "itab$" + std::string(ts->name) + "$" +
                    std::string(iface->name) + suffix);
            auto dit = descArray_.find(iface);
            if (dit != descArray_.end()) {
                newItmapElems.push_back(llvm::cast<llvm::Constant>(dit->second));
                newItmapElems.push_back(
                    llvm::ConstantExpr::getPointerCast(newItabGV, ptrTy()));
            }
        }
        newItmapElems.push_back(llvm::ConstantPointerNull::get(ptrTy()));
        auto* newItmapArrTy =
            llvm::ArrayType::get(ptrTy(), newItmapElems.size());
        specItmapGV = new llvm::GlobalVariable(
            *module_, newItmapArrTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(newItmapArrTy, newItmapElems),
            "itmap$" + std::string(ts->name) + suffix);
    }

    // Clone the shared vtable initializer and patch the needed slots.
    auto* arrTy = llvm::cast<llvm::ArrayType>(shared->getValueType());
    auto* sharedInit = llvm::cast<llvm::ConstantArray>(shared->getInitializer());
    std::vector<llvm::Constant*> elems;
    elems.reserve(arrTy->getNumElements());
    for (unsigned i = 0; i < arrTy->getNumElements(); ++i)
        elems.push_back(sharedInit->getAggregateElement(i));
    if (specDispose)
        elems[2] = llvm::cast<llvm::Constant>(specDispose);  // slot 2 = Dispose
    if (specItmapGV)
        elems[1] = llvm::ConstantExpr::getPointerCast(specItmapGV, ptrTy());  // slot 1 = itmap
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
        //
        // fix #704: bz_exc is now thread_local in the runtime, but generated
        // code accesses it via emitExcSlot() → call bz_exc_addr() → load/store
        // through the returned pointer.  This GlobalVariable is kept only as an
        // anchor (it is no longer referenced by any load/store in generated IR);
        // emitExcSlot() is the sole code path that touches the slot at runtime.
        excGlobal_ = new llvm::GlobalVariable(
            *module_, ptrTy(), /*isConstant=*/false,
            llvm::GlobalValue::ExternalLinkage,
            /*Initializer=*/nullptr,  // external declaration — defined in runtime
            "bz_exc");
    }
    return excGlobal_;
}


llvm::Value* Emitter::emitExcSlot() {
    // fix #704 / §30.14: return a ptr to the calling thread's bz_exc slot by
    // calling bz_exc_addr().  Using a plain extern "C" function (not a TLS
    // GlobalVariable) lets the LLVM ORC JIT resolve the symbol via
    // DynamicLibrarySearchGenerator — the host process exports "bz_exc_addr"
    // as a regular function, whereas a TLS GlobalVariable would require the
    // JIT to emit __emutls_v.bz_exc references which are NOT exported by the
    // host process (the host uses real, not emulated, TLS).
    // Per-call (not cached): each load/store gets a fresh call so the slot
    // address is always current (required when called from a thread-pool
    // worker that may switch stacks).
    if (!excAddrFn_) {
        // Declare: ptr bz_exc_addr() — returns &bz_exc for the current thread
        auto* fnTy = llvm::FunctionType::get(ptrTy(),
                                             llvm::ArrayRef<llvm::Type*>{},
                                             /*isVarArg=*/false);
        excAddrFn_ = llvm::Function::Create(
            fnTy, llvm::Function::ExternalLinkage, "bz_exc_addr", module_.get());
    }
    return builder_.CreateCall(excAddrFn_, {}, "exc.slot");
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
    llvm::Value* exc = builder_.CreateLoad(ptrTy(), emitExcSlot());
    llvm::Value* pending = builder_.CreateICmpNE(
        exc, llvm::ConstantPointerNull::get(ptrTy()));
    auto* unwind =
        llvm::BasicBlock::Create(llvm_, "call.unwind", currentFn_);
    auto* cont = llvm::BasicBlock::Create(llvm_, "call.cont", currentFn_);
    builder_.CreateCondBr(pending, unwind, cont);
    builder_.SetInsertPoint(unwind);
    // #1216 (sibling of #1149 / §31.2): if a call inside a finally body (or
    // catch arm) propagates an exception E2 via this pending-check, and there
    // is a captured in-flight exception E1 exposed as currentCaughtExc_ (set by
    // the #1149 / #1108 finally / catch-guard supersede fix), E1 is SUPERSEDED
    // by E2 and must be freed exactly once HERE — the #1149 emitThrowValue path
    // handles direct `throw` but is never reached on a call-propagation edge.
    // Guard mirrors emitThrowValue's #397/#866 logic: throwLeavesArm ensures we
    // only free when this unwind target is the arm/finally's own outer boundary
    // (a throw caught by an INNER try within the finally must not free E1 —
    // the finally continues and E1 stays in flight). No isDirectRethrow guard
    // is needed: a call-propagated exception (bz$exc set by the callee) is
    // never the same object as currentCaughtExc_; they are distinct heap objects.
    bool throwLeavesArm =
        caughtExcArmTarget_ && unwindBB() == caughtExcArmTarget_;
    if (currentCaughtExc_ && throwLeavesArm) {
        emitDisposeAndReclaim(currentCaughtExc_);
    }
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
    llvm::Value* exc = builder_.CreateLoad(ptrTy(), emitExcSlot());
    llvm::Value* pending = builder_.CreateICmpNE(
        exc, llvm::ConstantPointerNull::get(ptrTy()));
    auto* unwind =
        llvm::BasicBlock::Create(llvm_, "new.unwind", currentFn_);
    auto* cont = llvm::BasicBlock::Create(llvm_, "new.cont", currentFn_);
    builder_.CreateCondBr(pending, unwind, cont);
    builder_.SetInsertPoint(unwind);
    builder_.CreateCall(freeFn(), {obj});  // reclaim the instance block once
    // #1216: same as emitPendingCheck — if a constructor inside a finally body
    // (or catch arm) throws and supersedes E1, free E1 exactly once here.
    // See emitPendingCheck for the full rationale and double-free guards.
    bool throwLeavesArm =
        caughtExcArmTarget_ && unwindBB() == caughtExcArmTarget_;
    if (currentCaughtExc_ && throwLeavesArm) {
        emitDisposeAndReclaim(currentCaughtExc_);
    }
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


bool Emitter::isFreshOwnedClassProducer(const ast::Expr* e) const {
    // #1039 / #1053: a literal `new T(...)` or a call to a method declared to
    // return `owned T` — both transfer a fresh +1 the caller solely owns. A
    // call WITHOUT an `owned` return transfers no ownership: its result is a
    // BORROW (e.g. `return this.field`), and reclaiming it would free a value
    // owned elsewhere (use-after-free). `returnOwned` (ast::MethodDecl) is the
    // sound discriminator. A variable / field / `this` / property receiver is
    // neither a NewExpr nor a call, so it is never a fresh producer.
    if (!e) return false;
    bool producer = false;
    if (dynamic_cast<const ast::NewExpr*>(e)) {
        producer = true;
    } else if (auto* call = dynamic_cast<const ast::CallExpr*>(e)) {
        if (call->callee)
            if (auto* ms = dynamic_cast<sema::MethodSymbol*>(
                    ctx_.exprSymbol(*call->callee)))
                if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl))
                    producer = md->returnOwned;
    }
    if (!producer) return false;
    // The receiver type must be a heap CLASS: records/structs are value types
    // with no heap block or vtable disposer; interfaces/enums are not `new`-of-
    // class temporaries. A null declaration (builtin not exported as a TypeDecl)
    // is conservatively excluded — leak, but sound.
    const sema::TypeRef* rt = ctx_.exprType(*e);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return false;
    const auto* rtd = dynamic_cast<const ast::TypeDecl*>(rt->declaration->decl);
    return rtd && rtd->kind == ast::TypeDeclKind::Class;
}


void Emitter::disposeFreshNewReceiverTemp(const ast::Expr* recvExpr,
                                          llvm::Value* recvPtr,
                                          const sema::TypeRef* resultTy) {
    // #1039 / #1053 / §28.3: a fresh owned-class producer used ONLY as the
    // receiver of a member access or method call — never bound to a variable —
    // is owned by nothing and leaks. Dispose it after the value result has been
    // extracted, when the receiver is a fresh `new`/`owned`-returning producer
    // (#1039/#1053-case-2) and the result cannot alias into it.
    if (!recvExpr || !recvPtr || !resultTy) return;
    if (!isFreshOwnedClassProducer(recvExpr)) return;
    // The extracted result must be a NON-REFERENCE value — provably free of any
    // reference or borrow into the receiver: a numeric/bool/char/decimal
    // primitive, an enum, or unit. EXCLUDED, because each may alias into the
    // receiver: `string` and `object` (reference-typed primitives — a field /
    // method result borrows the receiver's refcounted string or sub-object); a
    // struct (could embed a borrow of a receiver sub-object); any class /
    // interface result (a reference). Excluding these frees the temporary ONLY
    // when nothing aliases it. (Bezel's borrow checker guarantees an instance
    // method cannot escape `this` past the call when its result is such a
    // value, so post-call disposal is sound — §15.3.)
    bool valueResult = false;
    if (resultTy->kind == sema::TypeRef::Kind::Unit) {
        valueResult = true;
    } else if (resultTy->kind == sema::TypeRef::Kind::Primitive &&
               resultTy->primitive != sema::PrimitiveType::None &&
               resultTy->primitive != sema::PrimitiveType::String &&
               resultTy->primitive != sema::PrimitiveType::Object) {
        valueResult = true;
    } else if (resultTy->kind == sema::TypeRef::Kind::Named &&
               resultTy->declaration) {
        const auto* td =
            dynamic_cast<const ast::TypeDecl*>(resultTy->declaration->decl);
        if (td && td->kind == ast::TypeDeclKind::Enum) valueResult = true;
    }
    if (!valueResult) return;
    // Dispose (idempotent vtable Dispose + reclaim exactly once). A fresh `new`
    // is non-null, but null-guard for safety/uniformity (folded away for a
    // provably-non-null pointer).
    llvm::Value* isNull = builder_.CreateICmpEQ(
        recvPtr, llvm::ConstantPointerNull::get(ptrTy()));
    auto* dispBB =
        llvm::BasicBlock::Create(llvm_, "freshrecv.dispose", currentFn_);
    auto* contBB =
        llvm::BasicBlock::Create(llvm_, "freshrecv.cont", currentFn_);
    builder_.CreateCondBr(isNull, contBB, dispBB);
    builder_.SetInsertPoint(dispBB);
    emitDisposeAndReclaim(recvPtr);
    builder_.CreateBr(contBB);
    builder_.SetInsertPoint(contBB);
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

// §2.11 (#838): the per-element disposer thunk for a Subscription element of an
// owning collection (OwnedArray<Subscription>, etc.). A Subscription is a
// runtime-managed opaque handle (bz_subscription_t*) disposed by
// bz_subscription_dispose (which unsubscribes), not via a vtable — so an owning
// collection of Subscriptions needs this thunk to unsubscribe each element at
// scope-end/clear/remove. Null-safe + idempotent (mirrors the `using var`
// Subscription auto-dispose). Same shape as groupingFreeThunk.
llvm::Function* Emitter::subscriptionDisposeThunk() {
    if (subscriptionDisposeThunk_) return subscriptionDisposeThunk_;
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      "bz$subscriptionDispose", module_.get());
    llvm::Argument* h = &*fn->args().begin();
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", fn);
    auto* dispBB = llvm::BasicBlock::Create(llvm_, "s.disp", fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "s.ret",  fn);
    llvm::IRBuilder<> b(entry);
    b.CreateCondBr(
        b.CreateICmpEQ(h, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, dispBB);
    b.SetInsertPoint(dispBB);
    auto* dispFn = declareC("bz_subscription_dispose",
                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
    b.CreateCall(dispFn, {h});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();
    subscriptionDisposeThunk_ = fn;
    return fn;
}

// §28.11 (#868): void(void*) thunk that frees a nested builtin owning collection
// element (e.g. the OwnedArray<R> inside an OwnedArray<OwnedArray<R>>) by calling
// the right runtime free fn with the recursive inner-element disposer.
// Mirrors sharedReleaseThunk / groupingFreeThunk in shape: own IRBuilder, null
// check, call free fn, return void. Cached per inner element-collection TypeRef*.
//
// Free fn selection (mirrors emitOwnedFieldDisposals / emitDisposals):
//   OwnedArray / OwnedStack / OwnedQueue  → bz_array_free(ptr, innerDisposer)
//   OwnedLinkedList                       → bz_llist_free(ptr, innerDisposer)
//   OwnedDictionary                       → bz_dict_free(ptr, innerValueDisposer)
//     (OwnedDictionary<K,V> owns its V elements; genericArgs[1] is V)
//   OwnedSet / OwnedBuffer                → bz_array_free(ptr, elementDisposer)
//     (#888: an OwnedSet<class>'s elements are disposed by bz_array_free with the
//     element disposer, the same call the top-level/field OwnedSet disposal uses.)
//
// The recursion terminates: each level peels one collection layer; eventually the
// inner element is a class R → elementDisposerThunk, or a plain value → null.
llvm::Function* Emitter::nestedCollectionDisposerThunk(const sema::TypeRef* elemCollTy) {
    // Cache hit.
    auto it = nestedCollectionDisposerThunks_.find(elemCollTy);
    if (it != nestedCollectionDisposerThunks_.end()) return it->second;

    // Determine the inner element type and free fn name from the collection's
    // kind (using the same name-detection as isCollectionTypeName /
    // isLinkedListTypeName / isDictTypeName, per §28.11 #868).
    std::string_view collName;
    if (elemCollTy && elemCollTy->kind == sema::TypeRef::Kind::Named &&
        !elemCollTy->declaration &&
        elemCollTy->astNode && elemCollTy->astNode->name &&
        !elemCollTy->astNode->name->parts.empty())
        collName = elemCollTy->astNode->name->parts[0];

    bool isLL   = (collName == "OwnedLinkedList");
    bool isDict = (collName == "OwnedDictionary");

    const char* freeName = isLL ? "bz_llist_free"
                         : isDict ? "bz_dict_free"
                         : "bz_array_free";

    // Resolve the inner element type (the thing the inner collection owns):
    //   OwnedArray/Stack/Queue/LinkedList/Buffer/Set → genericArgs[0]
    //   OwnedDictionary                              → genericArgs[1] (the value)
    // §28.11 (#888): OwnedSet's class elements are freed via bz_array_free with
    // the element disposer — the SAME call the top-level/field OwnedSet disposal
    // uses (it is not a dict-type-name, so the field path at emitOwnedField-
    // Disposals selects bz_array_free + the element disposer). Earlier this thunk
    // passed a null disposer for a nested OwnedSet, so an OwnedSet<class> inside
    // another owning collection leaked its elements; routing genericArgs[0]
    // through elementPolicyFor disposes them, mirroring the top-level path.
    const sema::TypeRef* innerElem = nullptr;
    if (elemCollTy && !isDict) {
        innerElem = (!elemCollTy->genericArgs.empty()) ? elemCollTy->genericArgs[0]
                                                       : nullptr;
    } else if (isDict) {
        innerElem = (elemCollTy->genericArgs.size() > 1) ? elemCollTy->genericArgs[1]
                                                         : nullptr;
    }

    // Recursively get the inner element's disposer (elementPolicyFor does its
    // own caching / base cases, including further nested-collection detection).
    llvm::Function* innerDisposer =
        elementPolicyFor(innerElem, /*owningFamily=*/true).disposerThunk;

    // Build the thunk: void(void* innerCollPtr) { if(!ptr) return;
    //                                              freeFn(ptr, innerDisposer); }
    // Uses its own IRBuilder so `builder_` insert point is untouched.
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    // Name the thunk after the collection kind for debuggability.
    std::string thunkName = std::string("bz$nestedFree$") + std::string(collName);
    // Make unique if same collection name appears multiple times (shouldn't, but
    // guard against symbol clash if two OwnedArray<R1> and OwnedArray<R2> both
    // need a thunk — give each its own symbol via the cache key pointer).
    if (module_->getFunction(thunkName)) {
        thunkName += "$" + std::to_string(
            reinterpret_cast<std::uintptr_t>(elemCollTy));
    }
    auto* fn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                      thunkName, module_.get());
    llvm::Argument* ptr = &*fn->args().begin();
    auto* entry  = llvm::BasicBlock::Create(llvm_, "entry",    fn);
    auto* freeBB = llvm::BasicBlock::Create(llvm_, "coll.free", fn);
    auto* retBB  = llvm::BasicBlock::Create(llvm_, "coll.ret",  fn);
    llvm::IRBuilder<> b(entry);
    // Null-guard: bz_array_free/bz_llist_free/bz_dict_free are null-safe but
    // check here anyway (mirrors sharedReleaseThunk pattern).
    b.CreateCondBr(
        b.CreateICmpEQ(ptr, llvm::ConstantPointerNull::get(ptrTy())),
        retBB, freeBB);
    b.SetInsertPoint(freeBB);
    llvm::Value* innerDispArg = innerDisposer
        ? llvm::cast<llvm::Value>(innerDisposer)
        : llvm::ConstantPointerNull::get(ptrTy());
    auto* freeFn = declareC(freeName,
                            llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy()});
    b.CreateCall(freeFn, {ptr, innerDispArg});
    b.CreateBr(retBB);
    b.SetInsertPoint(retBB);
    b.CreateRetVoid();

    nestedCollectionDisposerThunks_[elemCollTy] = fn;
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


// #816 / §28.5: build (once per instantiation, cached) the specialized CORE
// disposer for a class `ts` reached as the BASE of a specialized derived class,
// with the current genericSubst_ active. Mirrors the core half of
// emitSpecializedDisposer (which builds a public run-once wrapper + core for the
// most-derived class) but returns just the core, since a base chain calls the
// base CORE (unguarded — the run-once flag is checked once at the most-derived
// public Dispose). Saves/restores the full emit state + insert point because it
// is invoked mid-emission of the derived class's disposer (from
// emitBaseDisposeChain).
llvm::Function* Emitter::specializedDisposerCoreFor(const sema::TypeSymbol* ts) {
    std::vector<const sema::Symbol*> storage;
    collectInstanceStorage(ts, storage);
    std::vector<llvm::Type*> specFieldTys;
    specFieldTys.push_back(ptrTy());
    specFieldTys.push_back(disposedFlagTy());
    for (const sema::Symbol* m : storage)
        specFieldTys.push_back(storageType(m));
    auto& specMap = specializedDisposerCores_[ts];
    auto sit = specMap.find(specFieldTys);
    if (sit != specMap.end()) return sit->second;

    std::string suffix = "$baseSpec" + std::to_string(specMap.size());
    auto* fnType = llvm::FunctionType::get(
        llvm::Type::getVoidTy(llvm_), {ptrTy()}, /*isVarArg=*/false);
    auto* coreFn = llvm::Function::Create(
        fnType, llvm::Function::InternalLinkage,
        "bz$" + std::string(ts->name) + "$DisposeCore" + suffix, module_.get());
    specMap[specFieldTys] = coreFn;  // register before emit (self/diamond safety)

    // Save the caller's (derived-disposer) emit state + insert point.
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

    emitDisposerCore(ts, coreFn);  // genericSubst_ stays active

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
    return coreFn;
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
    for (std::size_t bi = 0; bi < ts->baseTypes.size(); ++bi) {
        const sema::TypeSymbol* base = ts->baseTypes[bi];
        if (!base || base->kind != ast::TypeDeclKind::Class) continue;
        // #816: if the base (transitively) has an owned type-parameter field, the
        // shared, type-erased base core disposer skips it (the concrete T is
        // unknown there) → leak. Bind the base's concrete type arguments — from
        // ts->baseTypeRefs[bi], resolved under any active OUTER substitution — and
        // call a SPECIALIZED base core emitted with that substitution active. The
        // baseTypeRefs derivation covers BOTH `class Child<T> : Base<T>` (T already
        // bound to e.g. Cell in the active subst) and the non-generic
        // `class ConcreteChild : Base<Cell>` (Cell is concrete in baseTypeRefs) —
        // the same per-base-arg map classStruct builds for inherited fields.
        const ast::TypeDecl* btd =
            base->decl ? dynamic_cast<const ast::TypeDecl*>(base->decl) : nullptr;
        if (classHasOwnedTypeParamField(base) && btd &&
            !btd->genericParams.empty() && bi < ts->baseTypeRefs.size() &&
            ts->baseTypeRefs[bi] &&
            btd->genericParams.size() == ts->baseTypeRefs[bi]->genericArgs.size()) {
            const sema::TypeRef* baseRef = ts->baseTypeRefs[bi];
            std::unordered_map<std::string_view, llvm::Type*> newSubst;
            std::unordered_map<std::string_view, const sema::TypeRef*> newSema;
            for (std::size_t gi = 0; gi < btd->genericParams.size(); ++gi) {
                // Resolve each base arg under the OUTER subst BEFORE installing
                // the new one (so `Base<T>` with the outer T=Cell maps to Cell).
                newSubst[btd->genericParams[gi].name] =
                    lowerTypeRef(baseRef->genericArgs[gi]);
                newSema[btd->genericParams[gi].name] =
                    resolveTypeParamRef(baseRef->genericArgs[gi]);
            }
            auto savedSubst = genericSubst_;
            auto savedSema = genericSubstSemaTy_;
            genericSubst_ = std::move(newSubst);
            genericSubstSemaTy_ = std::move(newSema);
            llvm::Function* specCore = specializedDisposerCoreFor(base);
            genericSubst_ = std::move(savedSubst);
            genericSubstSemaTy_ = std::move(savedSema);
            if (specCore) {
                builder_.CreateCall(specCore, {currentThis_});
                return;
            }
        }
        auto it = disposerCoreFns_.find(base);
        if (it != disposerCoreFns_.end()) builder_.CreateCall(it->second,
                                                          {currentThis_});
        return;  // single base class
    }
}


void Emitter::emitFunctionBody(sema::MethodSymbol* ms, llvm::Function* fn) {
    // §2.5 / §14.1 / #783: an IMPORTED method's `methodFns_` entry is an EXTERNAL
    // declaration (the exporter holds the compiled body); never emit a body for
    // it here — doing so would multiply-define `bz$<Class>$<M>` at link time.
    // This previously held implicitly because callableInfo failed for imported
    // methods (no AST decl); a generic-class imported method now carries a
    // re-parsed ast::MethodDecl for per-instantiation inline monomorphization
    // (#783, schema v5 methodBodyText), so the guard must be explicit — mirrors
    // the disposer loop's `ctx_.isImported` skip.
    if (ctx_.isImported(ms)) return;
    CallableInfo ci;
    if (!callableInfo(ms, ci)) return;

    // #817 / §14.2: mark this as an unspecialized generic TEMPLATE body when the
    // method belongs to a generic class or is itself a generic method. emitReturn
    // consults this to neutralize an irreconcilable placeholder-vs-concrete
    // return-type mismatch in the (dead) template — see emittingGenericTemplateBody_.
    {
        bool genericTemplate = false;
        if (ms->parent && ms->parent->decl) {
            if (auto* ptd = dynamic_cast<const ast::TypeDecl*>(ms->parent->decl))
                if (!ptd->genericParams.empty()) genericTemplate = true;
        }
        if (!genericTemplate && ms->decl) {
            if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl))
                if (!md->genericParams.empty()) genericTemplate = true;
        }
        emittingGenericTemplateBody_ = genericTemplate;
    }

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

    // §30.1: an EXPRESSION-bodied async method (`async method M(): Task<T> =>
    // expr`) must also flow through the async state-machine lowering. The async
    // detection above requires a Block body, so an expression body fell through
    // to the regular expression path below, which emitted a raw `ret <value>`
    // whose type is the UNWRAPPED result, not the Task<T> the function returns —
    // malformed IR ("return type does not match operand"). Synthesize a single
    // `return <exprBody>` and route it exactly like a block body. The synthesized
    // ReturnStmt BORROWS the AST-owned exprBody; release it after lowering so the
    // synthesized body's destructor does not free the AST node.
    if (ci.bodyKind == ast::MethodBodyKind::Expression && ci.exprBody &&
        isAsyncMethod(ms)) {
        std::vector<std::unique_ptr<ast::Stmt>> synthBody;
        auto ret = std::make_unique<ast::ReturnStmt>();
        ret->span = ci.exprBody->span;
        ret->value.reset(const_cast<ast::Expr*>(ci.exprBody));
        synthBody.push_back(std::move(ret));
        CallableInfo aci = ci;
        aci.bodyKind = ast::MethodBodyKind::Block;
        aci.stmts = &synthBody;
        aci.exprBody = nullptr;
        if (returnsAsyncIteratorType(ms))
            emitAsyncIteratorMethod(ms, fn, aci);
        else
            emitAsyncMethod(ms, fn, aci);
        // Release the borrowed exprBody so synthBody's destructor does not free
        // the AST-owned expression node.
        static_cast<ast::ReturnStmt*>(synthBody[0].get())->value.release();
        return;
    }

    currentFn_ = fn;
    currentThis_ = nullptr;
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    outOwnedRecipients_.clear();  // §28.11 / #889: per-body out-owned recipients
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
    // §15.3 (#747): which move-only owning inline-array locals are TRANSFERRED
    // out (so the rest stay eligible for scope-end slot disposal).
    moveOnlyArrayTransferred_.clear();
    moveOnlyArrayTransferredFn_ = fn;
    currentReturnTypeNode_ = ci.returnType;  // §8.6: for return-value boxing
    // #422 / §4.6 / §28.9: which `var` string locals may be released at scope
    // end (mirrors collectUnwindIneligible; consulted in lowerLocalDecl).
    stringReleaseEligible_.clear();
    stringReleaseEligibleFn_ = fn;
    // #1003 / stdlib §5 / §28: which plain `var` borrowing-Array locals bound to a
    // fresh materializer (ToArray/Sorted/Reversed) may have their backing reclaimed
    // at scope end (mirrors stringReleaseEligible_; consulted in lowerLocalDecl).
    borrowingArrayFreeEligible_.clear();
    borrowingArrayFreeEligibleFn_ = fn;
    // #1028: dict-materializer eligibility, populated by the same pass.
    dictMaterializerFreeEligible_.clear();
    dictMaterializerFreeEligibleFn_ = fn;
    // #422 / §27.5 / §28.9: which `var` closure (function-typed) locals may be
    // released at scope end (mirrors stringReleaseEligible_).
    closureReleaseEligible_.clear();
    closureReleaseEligibleFn_ = fn;
    // #422 / §8.6 / §28.9: which `var` Object/interface locals holding a boxed
    // VALUE may be released at scope end (mirrors stringReleaseEligible_).
    boxReleaseEligible_.clear();
    boxReleaseEligibleFn_ = fn;
    // §27.5 / §28.9 / #672: which +1-box-producer locals are captured by exactly
    // one lambda env, so the owning lambda's envdtor may perform the deferred
    // (escape-suppressed) scope-end box release.
    boxCaptureLocals_.clear();
    singleCaptureLocals_.clear();          // #694
    definitelyEscapingLambdas_.clear();    // #694
    boxCaptureLocalsFn_ = fn;
    // #429 / §27.5: which captured locals/params have ALL-non-escaping lambdas
    // and may have their boxed cells freed at outer-scope exit.
    cellFreeEligible_.clear();
    cellFreeEligibleFn_ = fn;
    if (ci.bodyKind == ast::MethodBodyKind::Block && ci.stmts) {
        collectUnwindIneligible(*ci.stmts);
        collectMoveOnlyArrayTransferred(*ci.stmts);
        collectStringReleaseEligible(*ci.stmts);
        collectBorrowingArrayFreeEligible(*ci.stmts);  // #1003
        collectClosureReleaseEligible(*ci.stmts);
        collectBoxReleaseEligible(*ci.stmts);
        collectBoxCaptureLocals(*ci.stmts);
        collectDefinitelyEscapingLambdas(*ci.stmts);  // #694
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
        // #720 / #796 / §22.13: every type with static state now initializes
        // LAZILY on first use (via bz$<T>$ensureinit at each static-access site)
        // — non-generic since #720, generic since #796. Skip any type that has a
        // lazy ensureinit; only a type WITHOUT one (none today) would run here.
        for (auto& e : staticCtors_) {
            if (e.type && ensureInitFns_.count(e.type)) continue;  // lazy
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
    // R36: an extension method ON an enum (`extension Color ... method ToInt()`)
    // needs the same wiring so a `case this` over the enum's constants resolves.
    // An extension method has NO parent type (functionName names it after its
    // target), so the check above misses it; consult the resolved target type.
    // Without this, enumTypeOf(this) returns null, the enum-constant arms never
    // match, patternMatches bails, and lowerCaseExpr leaves `case.end`
    // unterminated → an IR-verify failure in `bz$ext$<Enum>$<method>`.
    else if (isExtensionMethod(ms) && ms->extensionDecl &&
             ms->extensionDecl->targetType) {
        if (auto* rt = ctx_.resolvedType(*ms->extensionDecl->targetType))
            if (rt->declaration &&
                rt->declaration->kind == ast::TypeDeclKind::Enum)
                currentEnumMethod_ = rt->declaration;
    }

    // #720 / §22.13: a static method call triggers its declaring type's lazy
    // initialization before the body runs ("once initialization begins for a
    // type, all of its static fields are initialized before any of its methods
    // can run"). No-op for a type without lazy static state.
    // #917 / #918: this fires for Main too. Invoking the entry point IS the
    // first use of its declaring class, so §22.13 requires that class's static
    // fields to be initialized before Main's body runs — otherwise a static
    // field initializer with observable side effects (e.g. `static w := new
    // Widget()` whose ctor mutates another type's static state, or a `static
    // owned` field) never runs unless Main's body happens to read that class's
    // own static state. Placed at Main's body entry: an own-class static-init
    // failure wraps as TypeInitializationException and routes through Main's
    // normal unwind chain to bz_unhandled_exception, which is the spec-correct
    // outcome for an unhandled static-init failure at the entry point. (The
    // prior "skip Main so its body can catch its own init failure" behaviour
    // itself violated §22.13, since init must complete before the body runs.)
    if (isStaticMethod(ms))
        emitEnsureStaticInit(ms->parent);

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
            // #843 / §7.1: an arrow body returning a nullable VALUE type must
            // wrap (and widen) the bare value into the `{i8, T}` struct, exactly
            // as the ReturnStmt path does. Without this `=> 42` for a `long?`
            // method `ret`'d a bare i32 against the nullable-struct return type
            // (IR_FAIL).
            if (v && currentReturnTypeNode_) {
                if (const sema::TypeRef* declRet =
                        ctx_.resolvedType(*currentReturnTypeNode_))
                    v = coerceReturnToNullableValue(
                        v, declRet, isUnsignedTargetExpr(*ci.exprBody));
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
    outOwnedRecipients_.clear();  // §28.11 / #889: per-body out-owned recipients
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
        llvm::Value* origExc = builder_.CreateLoad(ptrTy(), emitExcSlot());
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
            builder_.CreateStore(tie, emitExcSlot());
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
            // NOTE (#1146 residual): a static auto-property with a NON-constant
            // inline initializer (`static readonly property X: int := calc()`) is
            // NOT yet emitted here — the "type has static state" detection that
            // gates sfinit generation only inspects static FIELDS, so no static
            // initializer runs for a property-only type. The CONSTANT case (the
            // common form) is handled by constInitForExpr in declareStatics. The
            // non-constant case is tracked separately.
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
            llvm::Value* val = nullptr;
            // R40 / #743 / §15.4: a COLLECTION-LITERAL static initializer
            // (`static All: Array<T> := [a, b, c]`, or a dict literal) — lowerExpr
            // returns null for a CollectionLiteralExpr, so without this the store
            // was skipped and the static global stayed null → null deref on first
            // access (`Type.All.Length` → SIGSEGV). Materialize the heap collection
            // against the field's declared type, exactly as the instance-field and
            // local-decl init paths do (materializeHeapCollectionLiteral also
            // delegates an empty `[]` with a dict target to the dict materializer).
            if (auto* cl =
                    dynamic_cast<const ast::CollectionLiteralExpr*>(initExpr)) {
                const sema::TypeRef* fty =
                    fd->type ? ctx_.resolvedType(*fd->type) : nullptr;
                if (fty)
                    val = cl->isDict ? materializeHeapDictLiteral(cl, fty)
                                     : materializeHeapCollectionLiteral(cl, fty);
            }
            // §15.1 (#1098): an INLINE-ARRAY static field
            // (`static _arr: [N]T := [a, b, c]`). The backing global is an
            // `[N x T]` aggregate; the heap-materialize above returns null for a
            // non-heap inline array and lowerExpr returns null for a
            // CollectionLiteralExpr, so the literal was dropped and the static
            // array stayed zero. Store each element into the global, mirroring
            // the local inline-array init path (lowerLocalDecl §15.1).
            if (!val) {
                auto* cl =
                    dynamic_cast<const ast::CollectionLiteralExpr*>(initExpr);
                auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(
                    gvIt->second->getValueType());
                if (cl && !cl->isDict && !cl->elements.empty() && arrTy) {
                    llvm::Type* elemTy = arrTy->getElementType();
                    // §22.13 (#1212): detect [N owned]T fields so each stored
                    // slot can be registered for atexit disposal below.
                    const sema::TypeRef* ft =
                        fd->type ? ctx_.resolvedType(*fd->type) : nullptr;
                    bool isOwnedInlineArr =
                        ft && ft->kind == sema::TypeRef::Kind::InlineArray &&
                        ft->arrayOwned;
                    llvm::Function* regFn = nullptr;
                    if (isOwnedInlineArr)
                        regFn = declareC("bz_register_static_owned",
                                         llvm::Type::getVoidTy(llvm_), {ptrTy()});
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
                        llvm::Value* gep = builder_.CreateGEP(
                            arrTy, gvIt->second,
                            {builder_.getInt32(0),
                             builder_.getInt32(static_cast<unsigned>(i))});
                        builder_.CreateStore(coerceToNullableSlot(elemTy, ev),
                                             gep);
                        // §22.13 (#1212): register each owned slot for atexit
                        // disposal (reverse init order). Only class-typed slots
                        // have a vtable and a bz_register_static_owned call.
                        if (regFn && ev->getType()->isPointerTy())
                            builder_.CreateCall(regFn, {ev});
                    }
                    continue;  // inline-array literal stored element-wise
                }
            }
            if (!val) val = lowerExpr(*initExpr);
            if (!val) continue;
            builder_.CreateStore(val, gvIt->second);
            // §22.13 (#1212): a static OWNED class/interface field is disposed
            // at program exit in reverse init order (bz_dispose_static_owned,
            // installed via atexit on first registration).  Register the value
            // right after the store so the atexit handler holds a fully-
            // initialized pointer.  Only owned class/interface fields have a
            // vtable Dispose; collections, Shared/Weak, and Subscription fields
            // have separate runtime free paths and are excluded by the predicate.
            if (isOwnedReferenceField(fs) || isOwnedBuiltinExceptionField(fs)) {
                auto* regFn = declareC("bz_register_static_owned",
                                       llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(regFn, {val});
            }
            // fix #709 / §22.12: also store into every per-instantiation global
            // for this field that was created lazily by staticFieldGlobal().
            // Per-instantiation globals are created on FIRST ACCESS (method body
            // emission, which precedes static-ctor body emission in emitModule),
            // so by the time we arrive here, genericInstStaticGlobals_ already
            // contains all instantiations referenced in this compilation unit.
            // The template-level store above handles the (unreachable) template
            // path; every concrete instantiation needs its own runtime store.
            // Note: this only covers T-independent initializers — expressions
            // that depend on T would need per-instantiation emit, which requires
            // larger machinery tracked separately.  The getValueType() guard
            // keeps that "unchanged behavior" literally true: if a field's TYPE
            // depends on T (e.g. `static x: T`), its per-instantiation globals
            // have differing LLVM types, so storing this single (placeholder-
            // typed) val would emit malformed IR — skip those, leaving them as
            // before rather than miscompiling.
            for (auto& [key, instGV] : genericInstStaticGlobals_) {
                if (key.first == static_cast<const sema::Symbol*>(fs) &&
                    instGV->getValueType() == val->getType())
                    builder_.CreateStore(val, instGV);
            }
        }
    }

    // Lower the user-written static constructor body (cs != null).
    if (cd) {
        if (!lowerScopedBlock(cd->body)) builder_.CreateRetVoid();
    } else {
        builder_.CreateRetVoid();
    }
}


// #720 / #796 / §22.13 / §22.7: build the per-type lazy-init function
// `bz$<T>$ensureinit` for every type that has static state (an sfinit and/or a
// cctor) — non-generic (#720) and generic (#796).
// The function runs that type's initializers exactly once, on first use, guarded
// by a per-type state global (0=uninit, 1=in-progress, 2=done, 3=failed):
//   * done      → return immediately.
//   * in-progress → return immediately (a re-entrant access during this type's own
//                   init sees its partially-initialized state, per §22.13 textual
//                   order; this also prevents unbounded recursion).
//   * failed    → re-raise the saved TypeInitializationException (§22.7).
//   * uninit    → mark in-progress, run sfinit then cctor; if either leaves a
//                 pending exception (already wrapped in TypeInitializationException
//                 by emitStaticConstructorBody), save it, mark failed, and leave it
//                 pending for the caller's emitPendingCheck to route. Otherwise mark
//                 done.
// For a GENERIC type the guard is per-TYPE: the single sfinit initializes every
// instantiation at once, so first use of ANY instantiation runs it once (§22.13
// timing fixed; per-instantiation INIT-timing independence for T-dependent
// initializers stays a follow-up — see #796).
void Emitter::emitEnsureInitFns() {
    // Collect every type that has static state — non-generic (#720) AND generic
    // (#796). For a generic type the single per-type sfinit initializes ALL of
    // its instantiations at once (it stores each T-independent initializer into
    // every per-instantiation global — emitStaticConstructorBody / #709), so a
    // per-TYPE lazy guard defers that all-at-once init to first use, exactly as
    // for a non-generic type. (Per-INSTANTIATION init *timing* independence —
    // only observable for T-DEPENDENT initializers, which are not yet supported,
    // emitStaticConstructorBody L~3647 — remains a tracked follow-up.)
    std::vector<const sema::TypeSymbol*> types;
    std::unordered_set<const sema::TypeSymbol*> seen;
    auto consider = [&](const sema::TypeSymbol* t) {
        if (!t) return;
        if (seen.insert(t).second) types.push_back(t);
    };
    for (auto& [t, fn] : sfinitFnOf_) consider(t);
    for (auto& [t, fn] : cctorFnOf_) consider(t);
    // #798: also emit an (exported) ensureinit for every LOCAL public non-generic
    // type that has a static MEMBER but no static STATE (const-only static fields
    // / static methods). The importer calls bz$<T>$ensureinit at each cross-module
    // static-member access; since all imported types are public, the symbol must
    // exist even when the type's init is a no-op — otherwise the importer would
    // reference a missing ensureinit and the link would fail. A type with no
    // static member is never init-triggered, so it needs none.
    auto isStaticMod = [](const sema::MemberSymbol* m) {
        return std::find(m->modifiers.begin(), m->modifiers.end(),
                         std::string_view("static")) != m->modifiers.end();
    };
    auto hasAnyStaticMember = [&](const sema::TypeSymbol* t) {
        for (const sema::Symbol* m : t->members) {
            if (auto* fs = dynamic_cast<const sema::FieldSymbol*>(m)) {
                if (isStaticMod(fs)) return true;
            } else if (auto* ms = dynamic_cast<const sema::MethodSymbol*>(m)) {
                if (isStaticMod(ms)) return true;
            } else if (auto* ps = dynamic_cast<const sema::PropertySymbol*>(m)) {
                if (isStaticMod(ps)) return true;
            } else if (auto* cs = dynamic_cast<const sema::ConstructorSymbol*>(m)) {
                if (cs->isStatic) return true;
            }
        }
        return false;
    };
    for (const auto& sym : ctx_.symbols()) {
        auto* t = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (!t || ctx_.isImported(t)) continue;
        if (!t->genericParams.empty()) continue;   // generics: cross-module deferred
        if (t->visibility != ast::Visibility::Public) continue;
        if (hasAnyStaticMember(t)) consider(t);
    }

    auto* voidFnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(llvm_), {}, /*isVarArg=*/false);
    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);

    // Save the per-function builder state (emitEnsureInitFns runs during setup,
    // before/around other function emission).
    llvm::Function* savedFn = currentFn_;
    auto* savedInsert = builder_.GetInsertBlock();

    for (const sema::TypeSymbol* t : types) {
        std::string cls(t->name);
        // Per-type state guard + saved-exception globals.
        auto* stateGV = new llvm::GlobalVariable(
            *module_, i8Ty, /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantInt::get(i8Ty, 0), "bz$" + cls + "$initstate");
        auto* excGV = new llvm::GlobalVariable(
            *module_, ptrTy(), /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantPointerNull::get(ptrTy()), "bz$" + cls + "$initexc");

        // #798: a public non-generic type's ensureinit is EXPORTED so an importer
        // can trigger its static init on first cross-module use (§22.13). The
        // per-type state/exc globals stay internal — only this module's ensureinit
        // body touches them, so the single guard is shared via the one exported
        // function. Non-public / generic types keep internal linkage.
        bool exportInit = t->visibility == ast::Visibility::Public &&
                          t->genericParams.empty();
        auto* fn = llvm::Function::Create(
            voidFnTy,
            exportInit ? llvm::GlobalValue::ExternalLinkage
                       : llvm::GlobalValue::InternalLinkage,
            "bz$" + cls + "$ensureinit", module_.get());
        ensureInitFns_[t] = fn;

        currentFn_ = fn;
        auto* entry   = llvm::BasicBlock::Create(llvm_, "entry",   fn);
        auto* failedB = llvm::BasicBlock::Create(llvm_, "reraise", fn);
        auto* initB   = llvm::BasicBlock::Create(llvm_, "doinit",  fn);
        auto* failB   = llvm::BasicBlock::Create(llvm_, "setfail", fn);
        auto* doneB   = llvm::BasicBlock::Create(llvm_, "setdone", fn);
        auto* retB    = llvm::BasicBlock::Create(llvm_, "ret",     fn);

        builder_.SetInsertPoint(entry);
        llvm::Value* s = builder_.CreateLoad(i8Ty, stateGV, "state");
        llvm::Value* isDone =
            builder_.CreateICmpEQ(s, llvm::ConstantInt::get(i8Ty, 2));
        auto* chkFailed = llvm::BasicBlock::Create(llvm_, "chkfail", fn);
        builder_.CreateCondBr(isDone, retB, chkFailed);

        builder_.SetInsertPoint(chkFailed);
        llvm::Value* isFailed =
            builder_.CreateICmpEQ(s, llvm::ConstantInt::get(i8Ty, 3));
        auto* chkProg = llvm::BasicBlock::Create(llvm_, "chkprog", fn);
        builder_.CreateCondBr(isFailed, failedB, chkProg);

        builder_.SetInsertPoint(chkProg);
        llvm::Value* isProg =
            builder_.CreateICmpEQ(s, llvm::ConstantInt::get(i8Ty, 1));
        builder_.CreateCondBr(isProg, retB, initB);

        // doinit: mark in-progress, run sfinit then cctor with pending-exc checks.
        builder_.SetInsertPoint(initB);
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 1), stateGV);
        auto sfIt = sfinitFnOf_.find(t);
        auto ccIt = cctorFnOf_.find(t);
        auto* afterSf = llvm::BasicBlock::Create(llvm_, "after_sf", fn);
        if (sfIt != sfinitFnOf_.end()) {
            builder_.CreateCall(sfIt->second, {});
            llvm::Value* e = builder_.CreateLoad(ptrTy(), emitExcSlot());
            llvm::Value* thrown = builder_.CreateICmpNE(
                e, llvm::ConstantPointerNull::get(ptrTy()));
            builder_.CreateCondBr(thrown, failB, afterSf);
        } else {
            builder_.CreateBr(afterSf);
        }
        builder_.SetInsertPoint(afterSf);
        if (ccIt != cctorFnOf_.end()) {
            builder_.CreateCall(ccIt->second, {});
            llvm::Value* e = builder_.CreateLoad(ptrTy(), emitExcSlot());
            llvm::Value* thrown = builder_.CreateICmpNE(
                e, llvm::ConstantPointerNull::get(ptrTy()));
            builder_.CreateCondBr(thrown, failB, doneB);
        } else {
            builder_.CreateBr(doneB);
        }

        builder_.SetInsertPoint(doneB);
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 2), stateGV);
        builder_.CreateBr(retB);

        // setfail: bz_exc holds the TypeInitializationException (inner=original;
        // sfinit/cctor self-wrap in emitStaticConstructorBody). Save the INNER
        // (the ORIGINAL) — NOT the TIE — because once the first raised TIE is
        // caught its heap block is reclaimed by the handler, so re-storing that
        // pointer on a later access would be a use-after-free (the TIE's disposer
        // is a no-op, so the inner is never freed and safely survives). Mark
        // failed; leave bz_exc = the TIE for this first raise.
        builder_.SetInsertPoint(failB);
        llvm::Value* tie0 = builder_.CreateLoad(ptrTy(), emitExcSlot());
        llvm::Value* inner0 = builder_.CreateLoad(
            ptrTy(), builder_.CreateGEP(i8Ty, tie0, builder_.getInt64(16)));
        builder_.CreateStore(inner0, excGV);
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 3), stateGV);
        builder_.CreateBr(retB);

        // reraise: failed state — wrap a FRESH TypeInitializationException around
        // the saved original (§22.7) and raise it. A fresh TIE each time avoids
        // re-raising a reclaimed block.
        builder_.SetInsertPoint(failedB);
        llvm::Value* savedInner = builder_.CreateLoad(ptrTy(), excGV);
        auto tieVit = builtinVtable_.find("TypeInitializationException");
        if (tieVit != builtinVtable_.end()) {
            llvm::Value* tie = builder_.CreateCall(
                callocFn(), {builder_.getInt64(1), builder_.getInt64(24)});
            builder_.CreateStore(tieVit->second, tie);  // vtable at offset 0
            builder_.CreateStore(savedInner,
                builder_.CreateGEP(i8Ty, tie, builder_.getInt64(16)));  // inner@16
            builder_.CreateStore(tie, emitExcSlot());
        } else {
            builder_.CreateStore(savedInner, emitExcSlot());  // fallback: raw
        }
        builder_.CreateBr(retB);

        builder_.SetInsertPoint(retB);
        builder_.CreateRetVoid();
    }

    currentFn_ = savedFn;
    if (savedInsert) builder_.SetInsertPoint(savedInsert);
}


// #720 / #796 / #798: at a static-access site, ensure `owner`'s static state is
// initialized (lazy, §22.13) before the access, routing a possible
// TypeInitializationException through the current unwind target. No-op for a type
// with no lazy ensureinit (a type with no static initializer at all).
void Emitter::emitEnsureStaticInit(const sema::TypeSymbol* owner) {
    if (!owner) return;
    auto it = ensureInitFns_.find(owner);
    if (it != ensureInitFns_.end()) {
        builder_.CreateCall(it->second, {});
        emitPendingCheck();
        return;
    }
    // #798 / §2.5 / §22.13: an IMPORTED type's static init runs in its DEFINING
    // module. The importer triggers it on first cross-module static-member access
    // by calling the exported `bz$<T>$ensureinit` (declared external here). Every
    // imported type is public, and the defining module exports an ensureinit for
    // each public non-generic type with a static member — so the symbol resolves
    // at link. Generic imported types are excluded (cross-module generic static
    // init is the #428 family).
    if (ctx_.isImported(owner) && owner->genericParams.empty()) {
        std::string name = "bz$" + std::string(owner->name) + "$ensureinit";
        llvm::Function* fn = module_->getFunction(name);
        if (!fn) {
            auto* voidFnTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(llvm_), {}, /*isVarArg=*/false);
            fn = llvm::Function::Create(
                voidFnTy, llvm::GlobalValue::ExternalLinkage, name,
                module_.get());
        }
        builder_.CreateCall(fn, {});
        emitPendingCheck();
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
    outOwnedRecipients_.clear();  // §28.11 / #889: per-body out-owned recipients
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
    moveOnlyArrayTransferred_.clear();
    moveOnlyArrayTransferredFn_ = fn;
    collectMoveOnlyArrayTransferred(cd->body);  // §15.3 (#747)
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
                   isOwnedSharedWeakField(fs) ||
                   // §28.5/§24.4 (#879): owned Subscription field also needs
                   // cleanup if the constructor body throws.
                   isOwnedSubscriptionField(fs))) {
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
                if (!ed->isStatic &&
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

    // §22.8 step 2 (#929/#930): run this class's OWN field initializers AFTER the
    // base chain (step 1) and BEFORE the body (step 3). Emitted after the
    // cleanup-block install so a throwing field initializer routes to it and the
    // already-assigned owned fields + the constructed base subobject are disposed
    // (§22.9). emitInstanceInit at the `new` site now installs only the vtable.
    // SKIP for a `this(...)` delegation: §22.8 step 1 says the chained sibling
    // constructor runs its own steps 0-3 (including field initializers) to
    // completion, so re-running them here would double-initialize.
    if (!isThis)
        emitOwnFieldInit(cs->parent, currentThis_, classStruct(cs->parent));

    bool terminated = lowerScopedBlock(cd->body, startIdx);
    if (!terminated) builder_.CreateRetVoid();
}


// §22.8 (issue #328 item 2+3; #929/#930): install the most-derived vtable into
// instance slot 0. Factored out of lowerNew so it runs identically at the
// in-module `new` site and inside the cross-module `bz$<Type>$init` function.
//
// Field initializers are NO LONGER run here. Per §22.8 (the v0.73 order
// base-ctor → field-inits → ctor body) each class's OWN field initializers run
// in its constructor body AFTER the base chain (emitConstructorBody /
// inlineCtorBody, via emitOwnFieldInit), or — for a no-explicit-ctor class — in
// the construction sequence after the base sequence (the `new` site's no-ctor
// branch, emitChainCall's primary-base branch, and emitInstanceInitFunction for
// a no-ctor public class). Running them here, at the `new` site before any base
// ctor, was the pre-v0.73 ordering bug (#930). The vtable install stays here so
// virtual dispatch during construction resolves to the most-derived class.
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
}


// §22.8 step 2 (#929/#930): run class `t`'s OWN field initializers and own
// EventSource allocations — NOT inherited ones. Per §22.8 the v0.73 order is
// base/chained ctor (step 1) → field initializers (step 2) → ctor body (step 3).
// This runs INSIDE the constructor AFTER the base chain (emitConstructorBody /
// inlineCtorBody), and at the no-explicit-ctor construction site after the base
// sequence, so a derived field initializer observes the fully-constructed base
// subobject (fixing the pre-v0.73 ordering where emitInstanceInit ran the whole
// hierarchy's field initializers at the `new` site, before any base ctor). Only
// `t`'s own storage is initialized here; each base level's own fields are
// initialized by that base's own constructor (or no-ctor construction sequence)
// reached through the base chain — so there is no double-init. `st` is
// classStruct(t) and `obj` points at the instance (base subobject at offset 0).
void Emitter::emitOwnFieldInit(const sema::TypeSymbol* t, llvm::Value* obj,
                               llvm::StructType* st) {
    // Own (non-inherited) instance storage only: `t`'s own non-static fields,
    // auto-properties, and instance event backing fields (sync + async per
    // §24.5 / #1176) — in declaration order (the same per-member shape
    // collectInstanceStorage records, minus the base-first recursion).
    std::vector<const sema::Symbol*> storage;
    for (const sema::Symbol* m : t->members) {
        auto* fs = dynamic_cast<const sema::FieldSymbol*>(m);
        if (fs && !isStaticField(fs)) { storage.push_back(m); continue; }
        if (isAutoProperty(m)) { storage.push_back(m); continue; }
        if (auto* es = dynamic_cast<const sema::EventSymbol*>(m)) {
            if (auto* ed = dynamic_cast<const ast::EventDecl*>(es->decl)) {
                if (!ed->isStatic) storage.push_back(m);
            }
        }
    }
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
        // per §23.1 (#1169) / §7: a field or auto-property of nullable VALUE type
        // (`int? := 42`, `bool? := true`, a nullable record `Pt? := new Pt(3,4)`)
        // lowers to a tagged {i8 hasValue; T value} slot, but the initializer is
        // the bare inner T (or already T?). Without wrapping, the bare scalar is
        // stored over the {i8,T} slot — clobbering the hasValue byte and leaving
        // the value half stale (present-but-zero reads). Wrap/widen to the tagged
        // struct exactly as a constructor-body assignment and the method-return
        // path (coerceReturnToNullableValue) do. A reference-typed nullable
        // (`string? := "hi"`) is NOT a value type, so isNullableValueTy is false
        // and this is skipped; an initializer already of type T? round-trips
        // unchanged (coerceReturnToNullableValue returns it as-is).
        {
            const sema::TypeRef* fieldRt = nullptr;
            if (auto* fd = dynamic_cast<const ast::FieldDecl*>(m->decl))
                if (fd->type) fieldRt = ctx_.resolvedType(*fd->type);
            if (auto* pd = dynamic_cast<const ast::PropertyDecl*>(m->decl))
                if (pd->type) fieldRt = ctx_.resolvedType(*pd->type);
            if (fieldRt && isNullableValueTy(fieldRt))
                v = coerceReturnToNullableValue(v, fieldRt,
                                                isUnsignedTargetExpr(*init));
        }
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

    // §22.8 (#929/#930): `bz$<T>$init` installs the vtable (step pre-1). For a
    // class WITH a constructor the importer calls `bz$<T>$ctor` after this, which
    // runs the base chain + field initializers + body — so init stays vtable-only
    // to avoid double-init. For a NO-explicit-ctor class there is no `bz$<T>$ctor`
    // for the importer to call, so init must itself run the construction sequence
    // (base sequence then this class's own field initializers, §22.8 step 1+2).
    emitInstanceInit(ts, currentThis_, classStruct(ts));
    if (!hasAnyConstructor(ts)) {
        emitImplicitBase(ts, currentThis_);
        emitOwnFieldInit(ts, currentThis_, classStruct(ts));
    }
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
        // Explicit `base(...)` with no matching base CONSTRUCTOR symbol. This is
        // the primary-ctor / no-explicit-ctor base case (e.g. `class ED: Base`
        // with `base(v)` where `Base(v: int)` is a primary-ctor class — it has no
        // ConstructorSymbol). §22.8 (#929/#930): run the base's construction
        // sequence — its own base chain (step 1) then its OWN field initializers
        // (step 2) — so a base field initializer like `_field := v` runs. The
        // base's positional-param fields were already stored by
        // prePopulateBasePositionalFields at the `new` site, so the initializer
        // reads them. currentThis_ is the constructing object (base subobject at
        // offset 0), which emitOwnFieldInit's implicitFieldAddr resolves against.
        if (!isThis && !hasAnyConstructor(targetCls)) {
            emitImplicitBase(targetCls, currentThis_);
            emitOwnFieldInit(targetCls, currentThis_, classStruct(targetCls));
        } else if (!isThis && n == 0) {
            // base() into a class whose own base still needs an implicit chain.
            emitImplicitBase(targetCls, currentThis_);
        } else if (isThis && currentThis_) {
            // #1087: `this(args)` chaining to the PRIMARY constructor of THIS
            // class. A primary-ctor class (`class C(x: int)`) has no explicit
            // ConstructorSymbol for its primary ctor, so ctorByArity returned
            // null and the `base(...)` branches above don't apply. Reproduce the
            // primary ctor's construction sequence: store each chained positional
            // arg into its param-field (mirroring `new C(args)`,
            // LlvmBackend_Exprs.cpp), run the base chain, then run the field
            // initializers. The additional ctor body SKIPS emitOwnFieldInit for a
            // `this(...)` delegation (expecting the chained ctor to run it), so
            // without this nothing initialized the primary-ctor class — its
            // positional params and field initializers all stayed calloc-zero.
            llvm::StructType* st = classStruct(targetCls);
            if (auto* td =
                    dynamic_cast<const ast::TypeDecl*>(targetCls->decl)) {
                bool allPositional = true;
                for (const auto& a : call->args)
                    if (!a.name.empty()) { allPositional = false; break; }
                if (td->kind == ast::TypeDeclKind::Class &&
                    td->hasPositionalFields && allPositional &&
                    n == td->positionalFields.size()) {
                    for (std::size_t pi = 0;
                         pi < td->positionalFields.size(); ++pi) {
                        const ast::Param& pf = td->positionalFields[pi];
                        for (sema::Symbol* mm : targetCls->members) {
                            auto* fs = dynamic_cast<sema::FieldSymbol*>(mm);
                            if (!fs || fs->posParam != &pf) continue;
                            auto pit = fieldIndex_.find(fs);
                            if (pit == fieldIndex_.end()) break;
                            if (!call->args[pi].value) break;
                            llvm::Value* v = lowerExpr(*call->args[pi].value);
                            if (!v) break;
                            v = coerceNumericToType(
                                v, st->getElementType(pit->second),
                                isUnsignedTargetExpr(*call->args[pi].value));
                            builder_.CreateStore(
                                v, builder_.CreateStructGEP(st, currentThis_,
                                                            pit->second));
                            break;
                        }
                    }
                }
            }
            emitImplicitBase(targetCls, currentThis_);
            emitOwnFieldInit(targetCls, currentThis_, st);
        }
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
    // §22.8 step 2 (#929/#930): own field initializers after the base chain,
    // before the body (mirrors emitConstructorBody). The inline cleanup block
    // installed above routes a throwing field initializer to owned-field + base
    // disposal. Skipped for a `this(...)` delegation (the chained sibling ctor
    // already ran them, §22.8 step 1).
    if (!isThis)
        emitOwnFieldInit(cs->parent, obj, classStruct(cs->parent));
    bool bodyTerminated = lowerScopedBlock(cd->body, startIdx);
    // fix #1239 / §14/§31: if the inlined constructor body diverged — e.g.
    // because it ends with an unconditional `throw` — the current basic block
    // already has a terminator (the `br` to ctor.inline.cleanup).  The
    // non-generic standalone path (emitConstructorBody) handles divergence by
    // omitting the trailing RetVoid; but here the body is inlined INTO the
    // caller's function, so after we return the caller continues emitting
    // instructions into whatever block the builder points at.  If that block
    // already has a terminator, those instructions land AFTER the terminator
    // and produce "Terminator found in the middle of a basic block" (IR_FAIL).
    // Fix: when the body terminated the current block, move the builder to a
    // fresh dead block so the caller's continuation code stays IR-valid
    // (unreachable but well-formed — LLVM's DCE will prune it).
    if (bodyTerminated) {
        auto* deadBB = llvm::BasicBlock::Create(
            llvm_, "ctor.inline.unreachable", currentFn_);
        builder_.SetInsertPoint(deadBB);
    }
}


void Emitter::emitImplicitBase(const sema::TypeSymbol* cls, llvm::Value* obj) {
    const sema::TypeSymbol* b = baseClassOf(cls);
    if (!b) return;
    // §11.1 / §22.8 (#929): a primary-ctor class `cls(...): b(baseArgs)` chains to
    // b's constructor. If b has an EXPLICIT constructor of matching arity, call it
    // with the evaluated inheritance-clause arguments (running its body), so a
    // primary-ctor derived class actually invokes a PARAMETERIZED base ctor
    // (previously only base positional FIELDS were pre-populated and only an
    // arity-0 base ctor was ever called, so `Derived(v): Base(v)` with an explicit
    // `Base.constructor(x)` left the base unconstructed). The args reference cls's
    // primary-ctor params, which are stored as positional backing fields at the
    // `new` site (read here via implicitFieldAddr against currentThis_ == obj). A
    // primary / no-explicit-ctor base has no ConstructorSymbol, so ctorByArity
    // returns null and control falls through to prePopulateBasePositionalFields +
    // the no-ctor field-init below — keeping the two mechanisms mutually exclusive.
    if (auto* td = dynamic_cast<const ast::TypeDecl*>(cls->decl)) {
        if (!td->primaryBaseCtorArgs.empty()) {
            std::size_t n = td->primaryBaseCtorArgs.size();
            if (sema::ConstructorSymbol* bt = ctorByArity(b, n, nullptr)) {
                auto bit = ctorFns_.find(bt);
                if (bit == ctorFns_.end() && ctx_.isImported(b)) {
                    declareImportedCtor(bt);
                    bit = ctorFns_.find(bt);
                }
                if (bit != ctorFns_.end() &&
                    bit->second->arg_size() == n + 1) {
                    std::vector<llvm::Value*> args;
                    args.push_back(obj);
                    bool ok = true;
                    for (std::size_t i = 0; i < td->primaryBaseCtorArgs.size();
                         ++i) {
                        const auto& a = td->primaryBaseCtorArgs[i];
                        if (!a.value) { ok = false; break; }
                        llvm::Value* v = lowerExpr(*a.value);
                        if (!v) { ok = false; break; }
                        // Coerce a numeric arg to the base ctor's param type
                        // (a no-op for pointer/struct args, per §5.1).
                        v = coerceNumericToType(
                            v, bit->second->getFunctionType()->getParamType(i + 1),
                            /*srcUnsigned=*/false);
                        args.push_back(v);
                    }
                    if (ok && args.size() == n + 1) {
                        builder_.CreateCall(bit->second, args);
                        return;
                    }
                }
            }
        }
    }
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
        // §22.8 (#930): `b` has no parameterless ctor. If `b` is a
        // no-explicit-ctor class, run its own base sequence then its own field
        // initializers (step 2) — there is no `b` ctor to do so. (If `b` had
        // only parameterized ctors, an implicit `base()` is a sema error and
        // never reaches codegen.)
        emitImplicitBase(b, obj);
        if (!hasAnyConstructor(b))
            emitOwnFieldInit(b, obj, classStruct(b));
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
        // #914: returning a narrowed value-nullable (sema proved it non-null) as
        // a non-nullable return type — unwrap the {flag, value} struct to its
        // value (the inverse of the non-nullable→nullable wrap below).
        v = unwrapNarrowedNullable(v, retTy);
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
        // #817 / §14.2: in an UNSPECIALIZED generic TEMPLATE body, a `T`-typed
        // field/local read takes the i32/`{i8,i32}` placeholder layout, which can
        // be irreconcilable with a CONCRETE signature return type (e.g.
        // `GetConcrete(): Counter?` returning a `T?` field → `{i8,i32}` value vs a
        // `ptr` return). CreateRet requires an exact type match, so this would be
        // malformed IR rejected by the verifier — even though the template is dead
        // code (every call binds the substitution and routes to a specialization,
        // where this flag is false and the layouts agree). Substitute a type-safe
        // `ret null` for the dead template. Gated on the flag so a genuine
        // return-coercion bug in a non-generic method (or a live specialization)
        // still fails loudly at the verifier rather than being silently nulled.
        if (emittingGenericTemplateBody_ && v && v->getType() != retTy)
            v = llvm::Constant::getNullValue(retTy);
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
    // §27.5 / §28.9 (#969): record this local's block-nesting depth so the
    // boxedCellFree registration can tell whether a captured local lives in an
    // enclosing scope (and must not have its capture cell freed at an inner
    // lambda's scope). disposeStack_ already holds this local's scope.
    if (declNode && !disposeStack_.empty())
        declScopeDepth_[declNode] = disposeStack_.size();
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
                // #813 / §15.1: wrap into the nullable-value slot struct
                // `{i8, T}` for an `[N]T?` literal initializer.
                builder_.CreateStore(coerceToNullableSlot(elemTy, ev), gep);
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
            // fix #747 R43 / §29.2: an Array<T> initializer assigned to an
            // IEnumerable<T> local must ALSO be wrapped in a vtable-backed
            // enumerator — only the user-class case was handled here, so
            // `var seq: IEnumerable<int> := someArray; for var x in seq do`
            // left `seq` holding the raw bz_array_t* and the for-loop's
            // IEnumerable vtable dispatch (lowerIEnumerableBuiltinFor) read the
            // array layout as a vtable → SIGSEGV. Mirrors the call-site wrapping
            // in buildOrderedArgs. Richer interfaces (IList/IReadOnlyList) keep
            // the array-backed protocol and are not wrapped.
            //
            // fix #1177 / §4.5 / §12.1: extend from Array-only to ALL
            // bz_array_t-backed owning collections — OwnedArray, OwnedBuffer,
            // Stack/OwnedStack, Queue/OwnedQueue. A local declared as
            // `IEnumerable<T>` and initialized from any of these previously
            // stored the raw bz_array_t* rather than a vtable-backed wrapper,
            // so lowerIEnumerableBuiltinFor's vtable-dispatch path read a
            // non-existent vtable slot 4 → SIGSEGV even on an empty collection.
            // Also handles (Owned)LinkedList<T> via the node-walking wrapper,
            // mirroring the same split in buildOrderedArgs. The fix is here
            // (at the local-variable initializer site) rather than in
            // lowerIEnumerableBuiltinFor because by the time for-in lowering
            // runs the slot already holds the unwrapped value; wrapping here
            // keeps every IEnumerable<T> local holding a proper vtable object.
            bool initIsArray = initTR &&
                initTR->kind == sema::TypeRef::Kind::Named &&
                initTR->astNode && initTR->astNode->name &&
                !initTR->astNode->name->parts.empty() &&
                (initTR->astNode->name->parts[0] == "Array" ||
                 initTR->astNode->name->parts[0] == "OwnedArray" ||
                 initTR->astNode->name->parts[0] == "OwnedBuffer" ||
                 initTR->astNode->name->parts[0] == "Stack" ||
                 initTR->astNode->name->parts[0] == "OwnedStack" ||
                 initTR->astNode->name->parts[0] == "Queue" ||
                 initTR->astNode->name->parts[0] == "OwnedQueue");
            if (initIsArray) {
                v = wrapArrayAsIEnumerable(v, declTR);
            } else if (initTR && isLinkedListTypeName(initTR)) {
                // #1177: (Owned)LinkedList<T> is node-based (bz_llist) — wrap
                // with the node-walking enumerator, mirroring buildOrderedArgs.
                v = wrapLinkedListAsIEnumerable(v, declTR);
            } else if (initTR && initTR->kind == sema::TypeRef::Kind::Named &&
                       initTR->declaration) {
                v = wrapUserClassAsIEnumerable(v, initTR->declaration, declTR);
            }
        }
    }
    llvm::Type* declaredTy = declType ? lowerType(declType) : nullptr;
    // #914: a narrowed value-nullable initializer (sema accepted the assignment
    // because null-flow proved it non-null) lowered to the {flag, value} struct,
    // but the local is declared NON-nullable — unwrap to its value.
    if (v && declaredTy) v = unwrapNarrowedNullable(v, declaredTy);
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
                    auto* vNullSt = llvm::dyn_cast<llvm::StructType>(v->getType());
                    bool vIsNullableStruct =
                        vNullSt && vNullSt->getNumElements() == 2 &&
                        vNullSt->getElementType(0) == nullablePrimHasValTy();
                    if (v->getType()->isPointerTy() &&
                        llvm::isa<llvm::ConstantPointerNull>(v)) {
                        // null → {false, 0}
                        v = llvm::ConstantAggregateZero::get(declaredTy);
                    } else if (vIsNullableStruct) {
                        // #840 / §5.1 / §7: a nullable→nullable LIFTED widening
                        // (`var d: double? := n` with `n: int?`). Keep hasValue;
                        // convert the inner value to the target inner type
                        // (int→double SIToFP, int→long SExt/ZExt, …). Before this
                        // the int? struct was stored as-is into the double? slot,
                        // losing the value. A same-inner-type source never reaches
                        // here (v->getType() == declaredTy short-circuits above).
                        llvm::Value* hasVal =
                            builder_.CreateExtractValue(v, {0u}, "lift.has");
                        llvm::Value* srcInner =
                            builder_.CreateExtractValue(v, {1u}, "lift.in");
                        llvm::Value* conv = coerceNumericToType(
                            srcInner, st->getElementType(1),
                            init && isUnsignedTargetExpr(*init));
                        if (conv->getType() == st->getElementType(1)) {
                            llvm::Value* res = builder_.CreateInsertValue(
                                llvm::UndefValue::get(declaredTy), hasVal, {0u});
                            v = builder_.CreateInsertValue(res, conv, {1u});
                        }
                    } else {
                        // #839 / §5.1 / §7: widen/convert a narrower numeric
                        // initializer to the nullable's inner type BEFORE wrapping
                        // — `var x: long? := 42` needs the int literal widened
                        // i32→i64; `var d: double? := someInt` needs SIToFP.
                        // Without this the exact-type-match check below failed and
                        // the value was stored UNWIDENED into the {i8, T} slot
                        // (slotTy was still set to the nullable struct) — a layout
                        // / value corruption (e.g. long? read back as garbage).
                        llvm::Value* inner = coerceNumericToType(
                            v, st->getElementType(1),
                            init && isUnsignedTargetExpr(*init));
                        if (inner->getType() == st->getElementType(1)) {
                            // plain prim → {true, value}
                            llvm::Value* res = builder_.CreateInsertValue(
                                llvm::UndefValue::get(declaredTy),
                                llvm::ConstantInt::get(nullablePrimHasValTy(), 1),
                                {0u});
                            v = builder_.CreateInsertValue(res, inner, {1u});
                        }
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
        for (const auto& ce : lastLambdaCapturedCells_) {
            // Check: does this cell's slot have any reads-through-cell disposal?
            // #966: the cell must outlive ANY disposal that reads the captured
            // value THROUGH it — INCLUDING one registered in an ENCLOSING scope
            // (a `using var` declared in an outer block, captured by a lambda in
            // an inner block). Searching only the current scope (disposeStack_
            // .back()) freed the cell at the inner-block exit while the outer
            // local still read + disposed through it → use-after-free (the #966
            // SIGSEGV on `do var g := () => r.V; end; return r.V;`). Search EVERY
            // live scope, not just the innermost one.
            bool hasOwnerDisposal = false;
            std::size_t ownerScopeIdx = 0;
            for (std::size_t si = 0; si < disposeStack_.size(); ++si) {
                for (const auto& existing : disposeStack_[si]) {
                    if (existing.slot != ce.newSlot) continue;
                    // These flags indicate disposals that do NOT read through the
                    // cell (they just release a runtime-managed value stored in
                    // the slot, not a value reached by dereferencing the slot as a
                    // cell pointer). All other disposals use boxedStorage(slot)
                    // which dereferences the cell — unsafe to free beforehand.
                    if (existing.stringRelease || existing.closureRelease ||
                        existing.boxRelease    || existing.boxedCellFree ||
                        existing.boxedCellFreeDeferred)
                        continue;
                    // This existing disposal READS THROUGH the cell; freeing the
                    // cell via boxedCellFree would be a UAF.
                    hasOwnerDisposal = true;
                    ownerScopeIdx = si;
                    break;
                }
                if (hasOwnerDisposal) break;
            }
            if (hasOwnerDisposal) {
                // #1017: the plain boxedCellFree is unsafe (the owner disposal in
                // disposeStack_[ownerScopeIdx] reads the object through the cell,
                // and LIFO order would free it first → UAF). Instead register a
                // DEFERRED cell-free in the OWNER's scope — emitDisposals frees it
                // in a second pass, AFTER every main-loop disposal in that scope
                // (so after the owner reads through it and every capturing env is
                // released). Without this the cell leaked (8 bytes). Only for a
                // plain cell (no inline-array elements — that path stays the
                // documented #747/#782 skip). Dedup: a second closure capturing the
                // same local hits this branch too, but its cell is alreadyBoxed so
                // it is never in lastLambdaCapturedCells_; still guard against a
                // duplicate deferred entry on the same slot in the owner scope.
                if (ce.inlineArrayN == 0) {
                    bool alreadyDeferred = false;
                    for (const auto& existing : disposeStack_[ownerScopeIdx])
                        if (existing.slot == ce.newSlot &&
                            existing.boxedCellFreeDeferred) {
                            alreadyDeferred = true;
                            break;
                        }
                    if (!alreadyDeferred) {
                        Disposal dcd{ce.newSlot, nullptr};
                        dcd.boxedCellFreeDeferred = true;
                        disposeStack_[ownerScopeIdx].push_back(dcd);
                    }
                }
                continue;  // leave cell alive for owner disposal (freed deferred)
            }
            // §27.5 / §28.9 (#969): even a VALUE-typed captured local (which has
            // no reads-through-cell disposal, so the guard above misses it) is
            // read through the cell after the inner block exits when it was
            // declared in an ENCLOSING scope (`var x := 55; do var g := () => x;
            // end; return x;`). Freeing the cell at this inner lambda's scope
            // would corrupt that later read. Skip boxedCellFree when the captured
            // local's declaration depth is shallower than the current scope (the
            // cell then leaks until the local's own scope exits — the sound
            // trade-off). Only LOCALS are in declScopeDepth_; params are left to
            // the disposal guard above (a noted follow-up).
            auto dd = declScopeDepth_.find(ce.key);
            if (dd != declScopeDepth_.end() && dd->second < disposeStack_.size())
                continue;
            Disposal dc{ce.newSlot, nullptr};
            dc.boxedCellFree = true;
            disposeStack_.back().push_back(dc);
            // #782: a captured move-only OWNING inline-array's elements live in the
            // boxed cell. Dispose each non-null element THROUGH the cell, then null
            // the slot — pushed AFTER boxedCellFree so it fires BEFORE it (reverse
            // iteration): dispose elements, then free the cell. The original
            // [N x ptr] alloca disposal is skipped (the array is marked
            // "transferred" by the capture), so each element is disposed exactly
            // once. Non-escaping-gated via lastLambdaCapturedCells_ (only cells of
            // cellFreeEligible_ locals are recorded), so the escaped-closure UAF
            // case never reaches here.
            if (ce.inlineArrayN > 0) {
                Disposal da{ce.newSlot, nullptr};
                da.inlineOwnedArrayCell = true;
                da.inlineSlotCount = ce.inlineArrayN;
                disposeStack_.back().push_back(da);
            }
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
    // #1003 / stdlib §5 / §28: if this `var` borrowing-`Array<T>` local was bound
    // to a fresh materializer (ToArray/Sorted/Reversed) and the escape walk proved
    // it never escapes its scope, register a scope-end bz_array_free of its single
    // producer reference (the array `v` just stored). The slot holds exactly that
    // one reference — the local is the sole owner of the fresh backing — so the
    // free runs exactly once per scope exit (on BOTH the normal and unwind path;
    // a given exit goes one way only) with no UAF (no escaping alias exists) and
    // no double-free (a `using var`/returned/moved/aliased local is excluded). The
    // backing is freed via the existing `collection` Disposal (bz_array_free); the
    // per-element disposer is null for value/class element borrows (only the
    // backing is reclaimed, §28.11) and the string-release thunk for
    // `Array<string>` (whose ToArray retained a +1/element — released symmetrically
    // here, no leak). Gated on currentFn_ matching the analyzed function (a lambda/
    // thunk body lowered mid-function is not analyzed → its array locals just leak,
    // never double-freed). A `using var` never reaches here — it is in neither
    // eligibility set and its disposal comes from registerUsingDisposal.
    if (v && currentFn_ == borrowingArrayFreeEligibleFn_ &&
        borrowingArrayFreeEligible_.count(static_cast<const void*>(declNode)) &&
        !disposeStack_.empty()) {
        Disposal d{slot, nullptr};
        d.collection = true;
        // Derive the per-element disposer from the collection's element type —
        // null for a borrowing value/class element (only the backing is freed),
        // the string-release thunk for the `Array<string>` co-owned-+1 case.
        // Mirror the using-var collection path (LlvmBackend_Setup.cpp ~6477).
        const sema::TypeRef* rt = nullptr;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(declNode)) {
            if (ld->type) rt = ctx_.resolvedType(*ld->type);
            if (!rt && ld->initializer) rt = ctx_.exprType(*ld->initializer);
        }
        const sema::TypeRef* el =
            (rt && !rt->genericArgs.empty()) ? rt->genericArgs[0] : nullptr;
        ElementPolicy ep = elementPolicyFor(el, /*owningFamily=*/false);
        d.collectionElemDisposer = ep.disposerThunk;
        if (!d.collectionElemDisposer &&
                ep.cat == ElementPolicy::Category::RuntimeManaged && el &&
                el->kind == sema::TypeRef::Kind::Primitive &&
                el->primitive == sema::PrimitiveType::String && !el->nullable) {
            d.collectionElemDisposer = stringElementReleaseThunk();
        }
        disposeStack_.back().push_back(d);
    }
    // #1028: a plain `var d := src.ToDictionary(...)` proven non-escaping holds the
    // sole reference to a fresh `owned Dictionary<K,V>`. Reclaim it at scope end via
    // bz_dict_free — the dict-backed analog of the #1003 array reclaim above. Per
    // stdlib §5 the borrowing chain owns the dict (the table, its key bytes, and the
    // value-box storage), NOT the value objects, so the per-value disposer is null —
    // matching the #1013 ownerless-temp dict reclaim (freeUnboundOwnedBorrowingArray
    // Temp's dict branch). A `using var` is excluded at collection time (its disposal
    // comes from registerUsingDisposal) so this never doubles with it; the escape
    // walk excludes any returned/moved/aliased/captured local so the free is
    // exactly-once (no UAF, no double-free). Gated on currentFn_ matching the
    // analyzed function (a lambda/thunk body inherits no eligibility → just leaks).
    if (v && currentFn_ == dictMaterializerFreeEligibleFn_ &&
        dictMaterializerFreeEligible_.count(static_cast<const void*>(declNode)) &&
        !disposeStack_.empty()) {
        Disposal d{slot, nullptr};
        d.dict = true;
        disposeStack_.back().push_back(d);
    }
}


bool Emitter::registerIterableSourceDisposal(const ast::LocalDecl* ld) {
    // §29.11 (#943): only a plain `var` (using-vars are handled by
    // registerUsingDisposal) initialised from an expression.
    if (!ld || ld->isUsing || !ld->initializer || disposeStack_.empty())
        return false;
    auto it = locals_.find(ld);
    if (it == locals_.end()) return false;

    // The local's effective type: the explicit annotation if present, else the
    // initializer's inferred type. A re-iterable, vtable-backed source has a
    // declaration-less Named type whose name is exactly "IEnumerable" or
    // "OwnedEnumerable" — the only declaration-less iteration types the for-in
    // builtin path drives through the iterator vtable (slot-1 Dispose). Richer
    // interfaces (IList/IReadOnlyList/...) keep the array-backed protocol and are
    // not vtable objects, so they are excluded by the exact name match.
    const sema::TypeRef* tr = nullptr;
    if (ld->type) tr = ctx_.resolvedType(*ld->type);
    if (!tr) tr = ctx_.exprType(*ld->initializer);
    if (!tr || tr->kind != sema::TypeRef::Kind::Named)
        return false;
    if (tr->declaration)
        return false;
    std::string_view name;
    if (tr->astNode && tr->astNode->name && !tr->astNode->name->parts.empty())
        name = tr->astNode->name->parts[0];
    else if (!tr->importedName.empty())
        name = tr->importedName;
    if (name != "IEnumerable" && name != "OwnedEnumerable")
        return false;

    // Only the local that OWNS the freshly produced source object is disposed
    // here. The owning initializer is a producer expression: a CallExpr (a yield
    // iterator method, `P.Seq()`), a NewExpr (a user class wrapped into the
    // IEnumerable form), or an identifier/member whose value is WRAPPED at this
    // decl site (`var seq: IEnumerable<int> := someArray;` — wrapArrayAs/
    // wrapUserClassAsIEnumerable allocate a fresh wrapper struct that this local
    // owns). A bare identifier/member initializer whose value is ALREADY an
    // IEnumerable (`var b := a;`) is an ALIAS — the aliased local already owns and
    // disposes the object, so registering disposal here too would double-free.
    bool wrappedHere = false;
    if (ld->type && ld->initializer) {
        const sema::TypeRef* declTR = ctx_.resolvedType(*ld->type);
        const sema::TypeRef* initTR = ctx_.exprType(*ld->initializer);
        bool declIsPureIEnum = declTR && declTR->kind == sema::TypeRef::Kind::Named &&
            !declTR->declaration && declTR->astNode && declTR->astNode->name &&
            !declTR->astNode->name->parts.empty() &&
            declTR->astNode->name->parts[0] == "IEnumerable";
        if (declIsPureIEnum && initTR &&
            initTR->kind == sema::TypeRef::Kind::Named) {
            bool initIsArray = initTR->astNode && initTR->astNode->name &&
                !initTR->astNode->name->parts.empty() &&
                initTR->astNode->name->parts[0] == "Array";
            // A user-class (declaration-carrying) init is wrapped (line 5413), as
            // is an Array init (line 5410). A declaration-less IEnumerable init is
            // NOT wrapped — it is a pre-existing IEnumerable value (an alias).
            wrappedHere = initIsArray || initTR->declaration != nullptr;
        }
    }
    bool isCall = dynamic_cast<const ast::CallExpr*>(ld->initializer.get()) != nullptr;
    bool isNew = dynamic_cast<const ast::NewExpr*>(ld->initializer.get()) != nullptr;
    bool ownsSource = isCall || isNew || wrappedHere;
    if (!ownsSource) return false;

    // The source value is the IEnumerABLE: dispose it once at scope end (the
    // for-in clones a fresh enumerator per loop and never disposes the source).
    Disposal d{it->second, nullptr};
    d.iterEnumDispose = true;
    disposeStack_.back().push_back(d);
    return true;
}

void Emitter::registerUsingDisposal(const ast::LocalDecl* ld) {
    if (disposeStack_.empty()) return;
    auto it = locals_.find(ld);
    if (it == locals_.end()) return;

    // §29.11 (#943): a sync `using var source := P.Seq();` whose value is a
    // vtable-backed re-iterable IEnumerable<T>/OwnedEnumerable<T> (yield-iterator
    // result, or an array/user-class wrapped into one) is disposed via the
    // iterator-format slot-1 Dispose (iterEnumDispose) — the for-in clones a fresh
    // enumerator per loop and never disposes the named source. Without this the
    // source frame leaks (the generic dispose paths below do not recognise the
    // declaration-less iterator type). Mirrors the IAsyncEnumerator using-var
    // handling in the await-using branch below. Only fires for the owning
    // initializer kinds (a CallExpr/NewExpr producer, or an init wrapped at the
    // decl site) — a bare alias (`using var b := a;`) is excluded so the aliased
    // owner remains the sole disposer.
    if (!ld->isAwaitUsing && ld->initializer) {
        const sema::TypeRef* lt = ld->type ? ctx_.resolvedType(*ld->type)
                                           : ctx_.exprType(*ld->initializer);
        // issue #1002 / §29.11: also match a `using var e := arr.GetEnumerator()`
        // whose value is the builtin IEnumerator<T>/OwnedEnumerator<T> sentinel
        // (an iterator-vtable enumerator, slot-1 Dispose). Without this the
        // enumerator struct leaks at scope end (the generic dispose paths below
        // do not recognise the declaration-less iterator type). Its slot-1
        // Dispose frees only the wrapper struct (it borrows the array), so this
        // is exactly-once and never double-frees the separately owned array.
        bool isSyncIterEnum =
            lt && lt->kind == sema::TypeRef::Kind::Named && !lt->declaration &&
            lt->astNode && lt->astNode->name &&
            !lt->astNode->name->parts.empty() &&
            (lt->astNode->name->parts[0] == "IEnumerable" ||
             lt->astNode->name->parts[0] == "OwnedEnumerable" ||
             lt->astNode->name->parts[0] == "IEnumerator" ||
             lt->astNode->name->parts[0] == "OwnedEnumerator");
        if (isSyncIterEnum) {
            const sema::TypeRef* initTR = ctx_.exprType(*ld->initializer);
            bool wrappedHere = initTR && initTR->kind == sema::TypeRef::Kind::Named &&
                ((initTR->astNode && initTR->astNode->name &&
                  !initTR->astNode->name->parts.empty() &&
                  initTR->astNode->name->parts[0] == "Array") ||
                 initTR->declaration != nullptr);
            bool ownsSource =
                dynamic_cast<const ast::CallExpr*>(ld->initializer.get()) ||
                dynamic_cast<const ast::NewExpr*>(ld->initializer.get()) ||
                wrappedHere;
            if (ownsSource) {
                Disposal d{it->second, nullptr};
                d.iterEnumDispose = true;
                disposeStack_.back().push_back(d);
                return;
            }
            // A bare-alias using-var of an iterator source: do not register a
            // second disposal (the aliased owner disposes it once). Fall through.
            return;
        }
    }

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
        // issue #759 (UNMASK): an async-iterator enumerator frame
        // (IAsyncEnumerator<T> / OwnedAsyncEnumerator<T>) has the iterator vtable
        // layout [0]=td,[1]=Dispose,[2]=MoveNextAsync,[3]=Current. The generic
        // asyncDispose path below reclaims via emitDynamicDispose → vtable SLOT 2,
        // which for THIS frame is MoveNextAsync (not Dispose) — calling it then
        // freeing the frame corrupts the heap. Route scope-end disposal through
        // the iterator-format slot-1 Dispose (iterEnumDispose), exactly as the
        // sync `for` / `await for` loops do (§29.7); it runs the state machine's
        // pending finally blocks (state → -1) then frees the frame.
        if (ld->initializer) {
            if (const sema::TypeRef* lt = ctx_.exprType(*ld->initializer)) {
                bool isAsyncIterEnum =
                    lt->kind == sema::TypeRef::Kind::Named && !lt->declaration &&
                    lt->astNode && lt->astNode->name &&
                    !lt->astNode->name->parts.empty() &&
                    (lt->astNode->name->parts[0] == "IAsyncEnumerator" ||
                     lt->astNode->name->parts[0] == "OwnedAsyncEnumerator");
                if (isAsyncIterEnum) {
                    Disposal d{it->second, nullptr};
                    d.iterEnumDispose = true;
                    disposeStack_.back().push_back(d);
                    return;
                }
            }
        }
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
    // stdlib §10.1/§10.2/§10.3 / §32.6 / fix #977: a `using var` bound to a
    // CString, WString, or RawBytes produced by a static From/FromBytes factory.
    // All three are malloc'd buffers; disposal is a plain free() (null-guarded).
    // Recognised by the initializer's sema exprType having builtinTypeName equal
    // to "CString", "WString", or "RawBytes" (set by the ExprTypecheck fix above).
    // No explicit type annotation pattern is needed — the only way to produce an
    // owned CString/WString/RawBytes without an extern return type is via these
    // static factories; extern-returning CString? lives a different path.
    {
        bool isFfiBuf = false;
        bool isRawBytes = false;   // fix #985: RawBytes needs the two-level free
        if (!isFfiBuf && ld->initializer) {
            if (auto* rt = ctx_.exprType(*ld->initializer)) {
                std::string_view n = builtinTypeName(rt);
                isFfiBuf = (n == "CString" || n == "WString" || n == "RawBytes");
                isRawBytes = (n == "RawBytes");
            }
        }
        if (isFfiBuf) {
            Disposal d{it->second, nullptr};
            d.ffiBuffer = true;
            // fix #985: a RawBytes is a { ptr, len } struct — free both the
            // owned buffer and the struct (bz_rawbytes_free), not a single
            // free() of a bare buffer (CString/WString).
            d.rawBytes = isRawBytes;
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
                (collTy->astNode->name->parts[0] == "Set" ||
                 collTy->astNode->name->parts[0] == "OwnedSet");
            // #1012 / §4.10: a Set/OwnedSet is DICT-BACKED whenever its element is
            // generic-dict-eligible (records, non-nullable primitives/enums, and —
            // for sets — strings; see genericBackingType / lowerNew). Such a Set
            // MUST be freed via bz_dict_free (the struct AND its entries), not
            // bz_array_free — the latter leaks the bz_dict struct (#1012: Set<int>
            // leaked 64 B + entries; only Set<string> was previously intercepted).
            // String elements are KEYS carrying a co-owned +1, released by
            // dictStringKeys before the free; value-type/record/enum keys carry no
            // per-element obligation (null disposer — value types, nothing to
            // dispose). A class/interface/nullable element is NOT dict-eligible
            // (genericBackingType returns null) → that Set stays array-backed and
            // falls through to the bz_array_free path below with its #888 element
            // disposer, which is correct for that (array-backed) representation.
            bool setDictBacked = isSetName &&
                genericBackingType(collTy, 0, /*isSet=*/true, nullptr) != nullptr;
            if (setDictBacked) {
                Disposal d{it->second, nullptr};
                d.dict = true;
                if (isStrElem(collTy, /*owning=*/false))
                    d.dictStringKeys = true;  // string keys: release +1 before free
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
    // #990 / §4.5 / §14.2: `using var _ := item` where `item` is the loop var of
    // a `for var item in coll.Drain()` over an owning sequence collection whose
    // element type is an unresolved generic class type parameter `T`.  In the
    // generic TEMPLATE body classOfLocal cannot resolve T to a concrete class, so
    // the static-disposal branch above missed it; dispose the drained class
    // element dynamically through its own vtable (exactly once).  Concrete
    // (non-generic) drains resolved to a class above and never reach here.
    if (usingVarIsDrainedOwningElement(ld)) {
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
        // #1024 / §28.3 / §28.5: an INTERFACE-typed local initialized with a
        // concrete `new C(...)` — `var f: IThing := new Factory()`. classOfLocal
        // short-circuits on the declared type and returns the INTERFACE symbol
        // (kind != Class), so the concrete class above is missed and this local
        // fell through to the unwind path's no-op AND the explicit-dispose reclaim
        // gate (explicitDisposeOwnedReceiverSlot). An explicit `f.Dispose()` then
        // ran the destructor via vtable but never RECLAIMED the block (a normal-
        // path leak — the concrete-typed `var f := new Factory(); f.Dispose()`
        // does reclaim, so this was an interface/concrete parity gap). Scope-end
        // disposal is correct on its own (autoDropAtScopeEnd routes through
        // registerUsingDisposal). Recover the concrete class from the NewExpr so
        // the explicit-dispose reclaim+null and the §28.3 unwind disposal both
        // fire; dispatch through emitDisposeAndReclaim is dynamic regardless, so
        // returning the concrete class (with a disposer) is sufficient.
        if (ne->type) {
            if (auto* rt = ctx_.resolvedType(*ne->type)) {
                if (rt->kind == sema::TypeRef::Kind::Named && rt->declaration &&
                    rt->declaration->kind == ast::TypeDeclKind::Class &&
                    disposerFns_.find(rt->declaration) != disposerFns_.end())
                    return rt->declaration;
            }
        }
        return nullptr;
    }
    // (a2) #1018 / #911 / §25: a user-defined binary operator whose `operator OP`
    // returns `owned T` — `var r := a + b`. The initializer is a BinaryExpr (not a
    // NewExpr or CallExpr), so (a)/(b) miss it: the operator-result block was
    // neither reclaimed on an explicit `r.Dispose()` (the #1018 leak — this slot
    // never reached explicitDisposeOwnedReceiverSlot's reclaim) nor disposed on an
    // exception unwind. Sema attaches the operator MethodSymbol to the BinaryExpr;
    // OperatorDecl::returnOwned is the authority that the operator owns its result.
    // Recognize it here so the operator result behaves exactly like a `new`/owned-
    // call result for both the explicit-dispose reclaim+null and the §28.3 unwind
    // disposal — mirroring usingVarOwnsClassObjectDynamically (b0b). The normal-
    // path affine auto-drop (#965, ld->autoDropAtScopeEnd) is unaffected: it routes
    // through registerUsingDisposal, and a local that is explicitly discharged is
    // NOT flagged autoDropAtScopeEnd, so the unwind-only registration added here
    // does not double with it (the `else if`/`else` in lowerLocalDecl is exclusive).
    if (auto* be = dynamic_cast<const ast::BinaryExpr*>(ld->initializer.get())) {
        if (auto* ms = dynamic_cast<sema::MethodSymbol*>(
                ctx_.exprSymbol(*ld->initializer))) {
            auto* od = dynamic_cast<const ast::OperatorDecl*>(ms->decl);
            const sema::TypeRef* initTy = ctx_.exprType(*ld->initializer);
            if (od && od->returnOwned &&
                (isClassOrInterfaceRef(initTy) ||
                 isClassOrInterfaceRef(resolveTypeParamRef(initTy)))) {
                if (initTy && initTy->kind == sema::TypeRef::Kind::Named &&
                    initTy->declaration &&
                    initTy->declaration->kind == ast::TypeDeclKind::Class)
                    return initTy->declaration;
                return reinterpret_cast<const sema::TypeSymbol*>(1);
            }
        }
        // #1029 / §28.3: built-in `??` (NullCoalescing) with a fresh owned-class
        // RHS — mirrors the producerState counterpart in Ownership.cpp. Needed for
        // the unwind-only disposal path (everDischarged > 0 → autoDropAtScopeEnd
        // false → registerUnwindOwnedLocal called here). The disposal is dynamic
        // (vtable) and null-guarded; the concrete class is returned when available
        // so the disposerFns_ gate (emitDisposals) finds it; otherwise a sentinel
        // triggers dynamic dispatch. // per §28.3
        if (be->op == "??" && be->right &&
            isFreshOwnedClassProducer(be->right.get())) {
            const sema::TypeRef* initTy = ctx_.exprType(*ld->initializer);
            if (isClassOrInterfaceRef(initTy)) {
                if (initTy->kind == sema::TypeRef::Kind::Named &&
                    initTy->declaration &&
                    initTy->declaration->kind == ast::TypeDeclKind::Class)
                    return initTy->declaration;
                return reinterpret_cast<const sema::TypeSymbol*>(1);
            }
        }
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


bool Emitter::registerInlineOwnedArrayDisposal(const ast::LocalDecl* ld) {
    // §15.3 / §15.1.1 (#747): a move-only OWNING inline-array local
    // ([N owned]T? / [N owned]T) auto-disposes its non-null slots at scope end.
    const sema::TypeRef* rt =
        ld->type ? ctx_.resolvedType(*ld->type)
                 : (ld->initializer ? ctx_.exprType(*ld->initializer) : nullptr);
    if (!rt || rt->kind != sema::TypeRef::Kind::InlineArray || !rt->arrayOwned)
        return false;  // not a move-only owning inline array — caller continues.
    // From here it IS a move-only owning inline array: handle it (return true)
    // so the using/task/unwind chain is bypassed (§28.10 line 2025: `using var`
    // has no disposal effect on a move-only array; the auto-dispose below runs
    // regardless). Whether we actually register disposal depends on transfer.
    if (emittingDisposerBody_) return true;
    // Only register when the transfer analysis covers the current body; a
    // lambda/thunk body lowered mid-function inherits a stale set, so skip
    // (a safe leak, never a double-free).
    if (currentFn_ != moveOnlyArrayTransferredFn_) return true;
    // Transferred out of the local somewhere → the receiver / callee owns the
    // slots; disposing here would double-free. Skip (the conservative default).
    if (moveOnlyArrayTransferred_.count(static_cast<const void*>(ld))) return true;
    if (disposeStack_.empty()) return true;
    auto it = locals_.find(ld);
    if (it == locals_.end() || !it->second) return true;
    llvm::AllocaInst* slot = it->second;
    auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(slot->getAllocatedType());
    if (!arrTy) return true;  // defensive: a non-aggregate alloca → skip.
    Disposal d{slot, nullptr};
    d.inlineOwnedArray = true;
    d.inlineSlotCount = static_cast<unsigned>(arrTy->getNumElements());
    disposeStack_.back().push_back(d);
    return true;
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
    if (ps->autoDropAtScopeEnd) {
        // §28.3 v0.152 (#1008 Part B): the ownership pass flagged this dropped
        // owned parameter for affine auto-drop — dispose it at method end on
        // BOTH the normal and unwind edges (the null-guarded dynamic dispatch a
        // dropped owned local gets, dynamicDispose). It is a never-moved owner
        // (everDischarged == 0, never reassigned), so the slot holds a live value
        // at method end and is disposed exactly once.
        d.dynamicDispose = true;
    } else {
        // Otherwise an `owned` parameter is auto-disposed only on the exception-
        // unwind path (the param is discharged on the normal path).
        d.unwindOnly = true;
    }
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

// §28.3 (#900): an `out` / `out var` / `out using var` / `out owned` argument is
// an OUTPUT — the callee WRITES the local (an `out owned` recipient receives
// OWNERSHIP and must be unwind-disposed), it does not consume it. So such an
// argument must NOT taint the local as unwind-ineligible. `ref` is conservatively
// left consuming (the callee may move out through it).
inline bool isOutArg(std::string_view m) {
    return m == "out" || m == "out var" || m == "out using var" ||
           m == "out owned";
}

// §28.3 (#994): the callee's parameter list backing a CallExpr/NullCondCallExpr,
// recovered from the resolved callee symbol — a MethodSymbol (ordinary method,
// free function, extension method, or operator), a ConstructorSymbol, or an
// ExternDeclSymbol. The receiver of an instance/extension call is the member-
// access target (walked separately as a borrow) and is NOT in this list, so the
// AST argument list aligns one-to-one with these ParameterSymbols by position.
// Returns nullptr when the callee cannot be resolved to a parameter list (a
// callee through a function-typed local/field, a generic/builtin intrinsic, an
// imported symbol without params, etc.) — the caller then conservatively treats
// every argument as consuming (a sound over-approximation: at worst a leak on the
// unwind path, never a double-free).
inline const std::vector<sema::Symbol*>* calleeParamList(
        const sema::SemaContext& ctx, const ast::Expr* callee) {
    if (!callee) return nullptr;
    sema::Symbol* s = ctx.exprSymbol(*callee);
    if (!s) return nullptr;
    if (auto* ms = dynamic_cast<sema::MethodSymbol*>(s)) return &ms->params;
    if (auto* cs = dynamic_cast<sema::ConstructorSymbol*>(s)) return &cs->params;
    if (auto* es = dynamic_cast<sema::ExternDeclSymbol*>(s)) return &es->params;
    return nullptr;
}

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

    // §28.3 (#994): walk a call's arguments, taking the callee's parameter modes
    // into account. An argument is CONSUMING (taints its local as unwind-
    // ineligible) only when ownership actually leaves the caller:
    //   - an explicit `owned` argument modifier (caller-stated transfer), or
    //   - the callee's parameter is `owned` / `out owned` (the callee takes over
    //     the disposal obligation), or
    //   - the callee's parameter is `ref` (the callee may move out through it —
    //     conservatively consuming), or
    //   - the callee parameter list is unavailable / the argument cannot be
    //     mapped (default to consuming — sound over-approximation).
    // A plain BORROW argument (callee param has no modifier) RETAINS the caller's
    // ownership (§28, line ~4899: a read in a non-`owned` argument position
    // borrows; `x` keeps ownership), so it is NOT consuming and the owned
    // local/param must remain eligible for unwind disposal. `out`/`out var`
    // arguments are outputs, never consuming (isOutArg). Previously every non-out
    // argument was treated as consuming, which dropped a borrow-passed owned
    // param from the unwind set → leak on a later throw.
    void walkCallArgs(const ast::Expr* callee,
                      const std::vector<ast::CallArgument>& args) {
        const std::vector<sema::Symbol*>* params = calleeParamList(ctx, callee);
        std::size_t pos = 0;
        for (const auto& a : args) {
            std::size_t positionalIdx = a.name.empty() ? pos++ : 0;
            // Resolve the parameter the argument binds to (positional or named).
            std::size_t pidx = params ? params->size() : 0;
            if (params) {
                if (a.name.empty()) {
                    pidx = positionalIdx;
                } else {
                    for (std::size_t j = 0; j < params->size(); ++j)
                        if ((*params)[j] && (*params)[j]->name == a.name) {
                            pidx = j;
                            break;
                        }
                }
            }
            bool mapped = params && pidx < params->size();
            std::string_view pm;
            if (mapped)
                if (auto* ps =
                        dynamic_cast<sema::ParameterSymbol*>((*params)[pidx]))
                    pm = ps->modifier;

            bool consuming;
            if (isOutArg(a.modifier)) {
                consuming = false;  // output, not consumed
            } else if (a.modifier == "owned") {
                consuming = true;   // explicit caller-side transfer
            } else if (!mapped) {
                consuming = true;   // unknown callee/unmappable arg → conservative
            } else {
                // Borrow param (no modifier) RETAINS caller ownership ⇒ not
                // consuming; `owned`/`out owned`/`ref` transfer ⇒ consuming.
                consuming = (pm == "owned" || pm == "out owned" || pm == "ref");
            }
            expr(a.value.get(), consuming);
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
            // §28.3 (#865): an `x.Method(...)` BORROWS the receiver — including
            // `x.Dispose()`. An explicit `x.Dispose()` is NOT a transfer that the
            // unwind disposal must skip: on the owned-local/param dispose path it
            // frees the block AND NULLS the slot (explicitDisposeOwnedReceiverSlot
            // → CreateStore null), and the unwind disposal is null-guarded — so a
            // local that was already disposed before a throw is skipped (no
            // double-free), while one whose `Dispose()` is UNREACHABLE or
            // CONDITIONALLY-not-taken keeps a non-null slot and is correctly
            // unwind-disposed (no leak). Marking the receiver ineligible here was
            // a flow-INSENSITIVE over-approximation: it saw `x.Dispose()` anywhere
            // (even after a `throw`, or guarded by a never-true `if`) and
            // suppressed the unwind disposal, leaking `x` on the throw path.
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                    c->callee.get())) {
                expr(ma->target.get(), /*consuming=*/false);
            } else {
                expr(c->callee.get(), /*consuming=*/false);
            }
            // §28.3 (#994): a borrow argument retains the caller's ownership; only
            // an `owned`/`ref` parameter (or explicit `owned` arg) transfers.
            walkCallArgs(c->callee.get(), c->args);
            return;
        }
        if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
            // §28.3 (#865): same as CallExpr — `x?.Dispose()` borrows + nulls the
            // slot; the null-guarded unwind disposal makes it safe to leave `x`
            // eligible (no double-free), and not marking it fixes the leak.
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                    c->callee.get())) {
                expr(ma->target.get(), /*consuming=*/false);
            } else {
                expr(c->callee.get(), /*consuming=*/false);
            }
            // §28.3 (#994): borrow arguments retain ownership (see walkCallArgs).
            walkCallArgs(c->callee.get(), c->args);
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


namespace {
// §15.3 (#747): a DEFAULT-TRANSFER walk that records every local whose ownership
// LEAVES the local — used to decide which move-only owning inline-array locals
// are eligible for scope-end slot disposal. The bias is the inverse of
// UnwindIneligibleWalker: there, an unrecognized use is a BORROW (eligible);
// here, an unrecognized use is a TRANSFER (ineligible → skip disposal → at worst
// a leak, NEVER a double-free). The ONLY positions that keep a local eligible
// (borrow=true, identifier NOT recorded) are: an index receiver (`a[i]`,
// `a[i] := x`, `move a[i]`), a member-access receiver (`a.Length`), and a for-in
// iterable. Crucially, unlike UnwindIneligibleWalker, `return arr` is a TRANSFER
// (a move-only array returned hands ownership to the caller — disposing it at the
// callee's scope end would double-free). A lambda body is walked as transfer
// (conservative): an inline-owned-array captured by a closure is left ineligible
// (a safe leak) rather than risk disposing it while an escaping closure still
// references it. `markIdent` records the local by its LocalDecl node.
struct MoveOnlyArrayTransferWalker {
    const sema::SemaContext& ctx;
    std::unordered_set<const void*>& out;
    // #747 verify (LENS 1): inside a lambda body EVERY referenced local is
    // treated as transferred — a captured move-only inline array is capture-boxed
    // (its [N x ptr] alloca is rewritten to a ptr-sized cell holder, §27.5), so
    // the slot-disposal GEP would read out of bounds and crash. Marking captured
    // arrays ineligible (a safe leak) avoids that entirely.
    bool inLambda = false;

    // #747 verify (LENS 2): a store into an owned slot `a[i] := rhs` whose rhs is
    // NOT a fresh-owned value (`new`), a `move`, or `null` may ALIAS another owner
    // into the slot (e.g. `a[i] := a[j]` copies a slot pointer), and the scope-end
    // disposal would then double-free the shared object. Such a store marks the
    // array ineligible (skip disposal = leak, never a double-free). new/move/null
    // stores never alias, so they keep the array eligible.
    static bool isSafeOwnedSlotRhs(const ast::Expr* rhs) {
        if (!rhs) return true;
        if (dynamic_cast<const ast::NewExpr*>(rhs)) return true;
        if (dynamic_cast<const ast::NullLiteralExpr*>(rhs)) return true;
        if (auto* u = dynamic_cast<const ast::UnaryExpr*>(rhs))
            if (u->op == "move") return true;
        return false;
    }

    void markIdent(const ast::Expr* e) {
        if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e)) {
            if (auto* loc = dynamic_cast<sema::LocalSymbol*>(ctx.exprSymbol(*id)))
                if (loc->decl) out.insert(static_cast<const void*>(loc->decl));
            return;
        }
        expr(e, /*borrow=*/false);
    }

    // borrow=true: this position only BORROWS its operand — an inline-array
    // identifier appearing here does NOT transfer ownership and stays eligible.
    void expr(const ast::Expr* e, bool borrow) {
        if (!e) return;
        if (dynamic_cast<const ast::IdentifierExpr*>(e)) {
            if (!borrow || inLambda) markIdent(e);  // capture ⇒ transfer (LENS 1)
            return;
        }
        if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
            expr(ix->target.get(), /*borrow=*/true);   // a[i] borrows a
            for (auto& a : ix->indices) expr(a.value.get(), /*borrow=*/false);
            return;
        }
        if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
            expr(m->target.get(), /*borrow=*/true);     // a.Length borrows a
            return;
        }
        if (auto* m = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
            expr(m->target.get(), /*borrow=*/true);
            return;
        }
        if (auto* p = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
            expr(p->operand.get(), borrow);
            return;
        }
        if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
            // `move a` transfers the whole array; `move a[i]` is a slot-move whose
            // receiver `a` is a borrow (the IndexExpr branch handles it, and the
            // slot is nulled at lowering so the array stays disposal-eligible).
            if (u->op == "move") {
                if (dynamic_cast<const ast::IndexExpr*>(u->operand.get()))
                    expr(u->operand.get(), /*borrow=*/false);  // walks a[i]: a borrow
                else
                    markIdent(u->operand.get());               // move a → transfer
            } else {
                expr(u->operand.get(), /*borrow=*/false);
            }
            return;
        }
        if (auto* c = dynamic_cast<const ast::CallExpr*>(e)) {
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                    c->callee.get()))
                expr(ma->target.get(), /*borrow=*/true);  // a.M(...) borrows a
            else
                expr(c->callee.get(), /*borrow=*/false);
            for (auto& a : c->args) expr(a.value.get(), /*borrow=*/false);
            return;
        }
        if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                    c->callee.get()))
                expr(ma->target.get(), /*borrow=*/true);
            else
                expr(c->callee.get(), /*borrow=*/false);
            for (auto& a : c->args) expr(a.value.get(), /*borrow=*/false);
            return;
        }
        if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
            expr(b->left.get(), /*borrow=*/false);
            expr(b->right.get(), /*borrow=*/false);
            return;
        }
        if (auto* c = dynamic_cast<const ast::ConditionalExpr*>(e)) {
            expr(c->condition.get(), /*borrow=*/false);
            expr(c->thenValue.get(), borrow);
            expr(c->elseValue.get(), borrow);
            return;
        }
        if (auto* t = dynamic_cast<const ast::TypeTestExpr*>(e)) {
            expr(t->operand.get(), /*borrow=*/true);  // `is`/`as` borrows
            return;
        }
        if (auto* t = dynamic_cast<const ast::ThrowExpr*>(e)) {
            expr(t->operand.get(), /*borrow=*/false);
            return;
        }
        if (auto* n = dynamic_cast<const ast::NewExpr*>(e)) {
            for (auto& a : n->args) expr(a.value.get(), /*borrow=*/false);
            return;
        }
        if (auto* w = dynamic_cast<const ast::WithExpr*>(e)) {
            expr(w->source.get(), /*borrow=*/false);
            for (auto& a : w->assigns) expr(a.value.get(), /*borrow=*/false);
            return;
        }
        if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
            for (auto& el : cl->elements) expr(el.get(), /*borrow=*/false);
            for (auto& k : cl->keys) expr(k.get(), /*borrow=*/false);
            return;
        }
        if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
            for (auto& part : is->parts) expr(part.expr.get(), /*borrow=*/false);
            return;
        }
        if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(e)) {
            // Every local referenced in a lambda body is treated as transferred
            // (captured ⇒ ineligible) — the `inLambda` flag forces marking even at
            // borrow positions, so a captured capture-boxed array is never the
            // target of the slot-disposal GEP (LENS 1: that GEP would crash).
            bool saved = inLambda;
            inLambda = true;
            if (lam->exprBody) expr(lam->exprBody.get(), /*borrow=*/false);
            for (auto& s : lam->stmts) stmt(s.get());
            inLambda = saved;
            return;
        }
        if (auto* ce = dynamic_cast<const ast::CaseExpr*>(e)) {
            expr(ce->scrutinee.get(), /*borrow=*/true);
            for (auto& arm : ce->arms) {
                expr(arm.guard.get(), /*borrow=*/false);
                expr(arm.exprBody.get(), borrow);
                for (auto& s : arm.stmts) stmt(s.get());
            }
            return;
        }
        // Any other expression: recurse conservatively as transfer.
    }

    void stmt(const ast::Stmt* s) {
        if (!s) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            expr(ld->initializer.get(), /*borrow=*/false);   // `var b := a` xfers a
            return;
        }
        if (auto* lc = dynamic_cast<const ast::LocalConstDecl*>(s)) {
            expr(lc->value.get(), /*borrow=*/false); return;
        }
        if (auto* r = dynamic_cast<const ast::ReturnStmt*>(s)) {
            // KEY: `return arr` TRANSFERS the move-only array to the caller.
            expr(r->value.get(), /*borrow=*/false); return;
        }
        if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s)) {
            expr(es->expr.get(), /*borrow=*/false); return;
        }
        if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(s)) {
            expr(ts->value.get(), /*borrow=*/false); return;
        }
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
            // The target is a WRITE (not a transfer of the target's old value);
            // `a[i] := x` borrows a, `a := x` overwrites a (its final value is
            // still disposed at scope end). The RHS, if an array identifier, is a
            // transfer.
            // LENS 2: a slot store `a[i] := rhs` whose rhs is NOT new/move/null may
            // alias another owner into the slot (`a[i] := a[j]`), which the
            // scope-end disposal would then double-free — mark the array (the index
            // receiver) ineligible so it is skipped (leak, never a double-free).
            if (auto* ixt = dynamic_cast<const ast::IndexExpr*>(as->target.get())) {
                if (!isSafeOwnedSlotRhs(as->value.get()))
                    markIdent(ixt->target.get());
            }
            expr(as->target.get(), /*borrow=*/true);
            expr(as->value.get(), /*borrow=*/false);
            return;
        }
        if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(s)) {
            expr(inc->target.get(), /*borrow=*/true); return;
        }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(s)) {
            // `_ := arr` is a §17.13 compile error for a move-only array, so this
            // never carries a real transfer; walk conservatively anyway.
            expr(ds->value.get(), /*borrow=*/false); return;
        }
        if (auto* ys = dynamic_cast<const ast::YieldStmt*>(s)) {
            expr(ys->value.get(), /*borrow=*/false); return;
        }
        if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
            expr(is->ifBranch.condition.get(), /*borrow=*/false);
            for (auto& b : is->ifBranch.body) stmt(b.get());
            for (auto& br : is->elifBranches) {
                expr(br.condition.get(), /*borrow=*/false);
                for (auto& b : br.body) stmt(b.get());
            }
            for (auto& b : is->elseBody) stmt(b.get());
            return;
        }
        if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
            expr(ws->condition.get(), /*borrow=*/false);
            for (auto& b : ws->body) stmt(b.get());
            return;
        }
        if (auto* fs = dynamic_cast<const ast::ForStmt*>(s)) {
            expr(fs->iterable.get(), /*borrow=*/true);   // `for x in a` borrows a
            expr(fs->withExpr.get(), /*borrow=*/false);
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
                expr(c.guard.get(), /*borrow=*/false);
                stmt(c.body.get());
            }
            for (auto& b : ts->finallyBody) stmt(b.get());
            return;
        }
        if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
            expr(cs->scrutinee.get(), /*borrow=*/true);
            for (auto& arm : cs->arms) {
                expr(arm.guard.get(), /*borrow=*/false);
                expr(arm.exprBody.get(), /*borrow=*/false);
                for (auto& a : arm.stmts) stmt(a.get());
            }
            return;
        }
        // BreakStmt / ContinueStmt: no sub-expressions.
    }
};
}  // namespace


void Emitter::collectMoveOnlyArrayTransferred(
        const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    moveOnlyArrayTransferred_.clear();
    MoveOnlyArrayTransferWalker w{ctx_, moveOnlyArrayTransferred_};
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
    // §9.6 / §28.3 (#1184): `string.Join(separator, items)` lowers (in
    // LlvmBackend_Calls.cpp) to a freshly malloc'd buffer wrapped in a heap
    // `bz.string` whose rc := 1 (initStringRc) — exactly the +1 single-reference
    // shape of `+`/interpolation/range-slice/primitive-ToString. (The borrowed
    // item snapshot is freed inside the lowering; only the RESULT string is handed
    // to the caller at +1, and nothing else reclaims it.) Recognise it as a +1
    // producer so a `var s := string.Join(...)` local TRANSFERS that single
    // reference into its slot (no borrow-retain) and the scope-end release — when
    // the escape walk proves the local non-escaping — drops rc 1→0 and frees it
    // exactly once. No double-free: no extra retain is emitted, and an escaping
    // local is excluded from the release by the escape walk (its +1 then leaks,
    // the safe direction). Matched structurally — the callee is a MemberAccess on
    // the `string`/`String` type keyword with member `Join` and two args — to
    // mirror the static-call recognition in lowerStaticCall (it is a builtin: no
    // resolved MethodSymbol, so isUserMethodStringReturnCall below never sees it).
    if (auto* call = dynamic_cast<const ast::CallExpr*>(init)) {
        if (call->args.size() == 2 && call->callee) {
            if (auto* ma =
                    dynamic_cast<const ast::MemberAccessExpr*>(call->callee.get())) {
                if (ma->member == "Join" && ma->target) {
                    if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                            ma->target.get())) {
                        if (tid->name == "string" || tid->name == "String")
                            return true;  // string.Join → rc=1 heap string
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
// USER-DEFINED MethodSymbol (backed by an ast::MethodDecl, not an operator)
// whose declared return type is a function type (Kind::Function). Such a call
// mints exactly +1 for the returned closure cell — so the caller owns that
// single reference. Cross-module imported methods are now also recognized as
// +1 producers when they declare a non-nullable function return type: their
// ReturnStmt in the exporter module ran the same closure-retain minting logic,
// so the caller receives exactly +1 and must release at scope end without an
// extra borrow-retain. (#712 / §28.9 — mirror of #694 for strings.) Parallel
// to isUserMethodStringReturnCall for strings.
bool Emitter::isUserMethodClosureReturnCall(const ast::Expr* init) const {
    auto* call = dynamic_cast<const ast::CallExpr*>(init);
    if (!call || !call->callee) return false;
    auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(*call->callee));
    if (!ms) return false;
    if (ms->isOperator) return false;
    if (ctx_.isImported(ms)) {
        // §28.9 / #712: an imported method whose declared return type is a
        // function type already ran ReturnStmt closure-retain minting in the
        // exporter. The caller receives +1 and must release at scope end, not
        // borrow-retain. Treat it as a +1 producer. (Mirror of #694 for strings.)
        const sema::TypeRef* rt = ctx_.importedReturnType(ms);
        return rt && rt->kind == sema::TypeRef::Kind::Function;
    }
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


// #824 / §28.5 / §28.9: is `target` an `owned` heap-collection CLASS instance
// field lvalue (OwnedArray<T>/OwnedStack<T>/OwnedQueue<T>/OwnedSet<T>/
// OwnedLinkedList<T>/OwnedBuffer<T>/OwnedDictionary<T> etc.)? Classification
// mirrors isClosureFieldLvalue: exclude locals, ref params, static fields;
// require symbol to be a FieldSymbol of a Class. Only fields whose type passes
// ownedCollectionFieldType (i.e. `owned` modifier + a collection builtin name)
// are matched, so borrowed Array<T> fields and value-type slots are excluded.
bool Emitter::isOwnedCollectionFieldLvalue(const ast::Expr& target) {
    // Must be a pointer-typed location (collection pointer is a ptr).
    if (localSlot(target)) return false;       // a plain local / parameter
    {
        llvm::Type* dummy = nullptr;
        if (refParamAddr(target, dummy)) return false;  // a ref/out alias
    }
    if (staticFieldGlobal(target)) return false;  // program-lifetime storage
    sema::Symbol* sym = ctx_.exprSymbol(target);
    if (!sym) return false;
    auto* fs = dynamic_cast<sema::FieldSymbol*>(sym);
    if (!fs) return false;
    // Must be a class field (not a record field — records have no destructor
    // that would auto-dispose it, §9.7).
    if (!fs->parent || fs->parent->kind != ast::TypeDeclKind::Class) return false;
    // Must be an `owned` heap collection field per §28.5.
    return ownedCollectionFieldType(fs) != nullptr;
}


// #824 / §28.5 / §28.9: dispose the prior value of an owned collection CLASS
// field before storing the new pointer. Mirrors emitOwnedFieldDisposals for
// the collection path: build the element disposer from elementPolicyFor, then
// call bz_array_free or bz_llist_free on the old pointer.
// bz_array_free / bz_llist_free are null-safe (no-op on null), so the first
// assignment in a constructor (where the slot holds an uninitialized null
// pointer) is correctly handled without an explicit null-guard — matching the
// emitOwnedFieldDisposals comment at §2184.
// Store-new BEFORE dispose-old (like lowerStringFieldStore retain-before-
// release): the field points to the new value from the moment of the store, so
// even if the disposal path is re-entrant (unlikely for collections, but
// consistent), the field is never observed holding the old value after dispose.
bool Emitter::lowerOwnedCollectionFieldStore(const ast::Expr& target,
                                             llvm::Value* fieldAddr,
                                             llvm::Value* newVal) {
    if (!isOwnedCollectionFieldLvalue(target)) return false;
    if (!fieldAddr || !newVal) return false;
    if (!newVal->getType()->isPointerTy()) return false;

    // Load the old collection pointer before overwriting it.
    llvm::Value* oldVal = builder_.CreateLoad(ptrTy(), fieldAddr, "ocf.old");

    // Store the new pointer — field now owns the new collection.
    builder_.CreateStore(newVal, fieldAddr);

    // Identify the collection element type and whether this is an owning family.
    // Reuse the same logic as emitOwnedFieldDisposals (§2185-§2218).
    sema::Symbol* sym = ctx_.exprSymbol(target);
    auto* fs = dynamic_cast<sema::FieldSymbol*>(sym);
    const sema::TypeRef* collTy = fs ? ownedCollectionFieldType(fs) : nullptr;

    // §28.5/§28.11, #894: OwnedDictionary<K,V> owns its V values (genericArgs[1]),
    // NOT its K keys (genericArgs[0]).  Free the OLD dict via bz_dict_free so that
    // class V elements of the displaced dict are disposed on field reassignment.
    bool isDict = collTy && isDictTypeName(collTy);
    const sema::TypeRef* collElem =
        isDict
            ? ((collTy && collTy->genericArgs.size() > 1)
                   ? collTy->genericArgs[1]
                   : nullptr)
            : ((collTy && !collTy->genericArgs.empty())
                   ? collTy->genericArgs[0]
                   : nullptr);
    bool collIsOwning =
        collTy && collTy->astNode && collTy->astNode->name &&
        !collTy->astNode->name->parts.empty() &&
        collTy->astNode->name->parts[0].rfind("Owned", 0) == 0;

    ElementPolicy pol = elementPolicyFor(collElem, collIsOwning);
    llvm::Function* elemDisp = pol.disposerThunk;
    // §28.11: borrowing Array/Stack/Queue<string> co-owns a +1 per element
    // (retainRelease path); use the string-release thunk. Mirrors §2200-§2208.
    if (!elemDisp && !isDict &&
            pol.cat == ElementPolicy::Category::RuntimeManaged &&
            collElem &&
            collElem->kind == sema::TypeRef::Kind::Primitive &&
            collElem->primitive == sema::PrimitiveType::String &&
            !collElem->nullable &&
            collTy && collTy->astNode && collTy->astNode->name &&
            !collTy->astNode->name->parts.empty() &&
            [](std::string_view n){ return n == "Array" || n == "Stack" ||
                n == "Queue"; }(collTy->astNode->name->parts[0]))
        elemDisp = stringElementReleaseThunk();

    llvm::Value* disp = elemDisp
        ? llvm::cast<llvm::Value>(elemDisp)
        : llvm::ConstantPointerNull::get(ptrTy());

    // per §4.6/§4.7: LinkedList/OwnedLinkedList free via bz_llist_free.
    // §28.5/§28.11, #894: OwnedDictionary fields free via bz_dict_free.
    bool isLL = collTy && isLinkedListTypeName(collTy);
    const char* freeName = isLL   ? "bz_llist_free"
                          : isDict ? "bz_dict_free"
                          :          "bz_array_free";
    auto* freeFn = declareC(freeName,
                            llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
    builder_.CreateCall(freeFn, {oldVal, disp});
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


// #1003 / stdlib §5 / §28: is `ld` a plain `var` binding a FRESHLY-MATERIALIZED
// borrowing `Array<T>` from a LINQ/copy materializer call (`ToArray` / `Sorted`
// / `Reversed`)? These are the exact shapes codegen lowers to a fresh
// `bz_array_t` whose single reference the local solely owns. Per §28 a borrowing
// `Array<T>` is trivial-dispose — its backing must be reclaimed at scope end but
// the sema/ownership pass imposes no disposal obligation (the result is "Borrowed"
// — name-head "Array", not "OwnedArray", and the materializer has no `owned`-
// returning user MethodDecl), so codegen alone must emit the implicit reclaim.
//   Excluded by construction (so the producer/consumer balance stays inside
//   codegen, never a UAF on a borrowed/aliased value):
//     * a `using var` (already disposed via registerUsingDisposal's collection
//       path — would double-free);
//     * a `new Array<T>(...)` (a NewExpr carries a sema obligation → rejected as
//       an undisposed plain `var`, never reaches this discardable path);
//     * an identifier/member/`move` alias (`var b := a;`) — not a fresh producer;
//     * a non-materializer call (a method returning a BORROWED field-array) —
//       freeing it would be a UAF, so only the whitelisted fresh-producers count;
//     * any result type other than the borrowing `Array<T>` (Owned*, Set/
//       Dictionary/LinkedList are dict-/llist-backed and freed differently).
bool Emitter::isFreshBorrowingArrayMaterializerLocal(
        const ast::LocalDecl* ld) const {
    if (!ld || ld->isUsing || !ld->initializer) return false;
    // The result type must be a borrowing `Array<T>` (name-head exactly "Array").
    // ToArray/Sorted/Reversed all return `Array<T>`; restrict to that to avoid the
    // dict-backed (Set) / llist-backed (LinkedList) / owning (OwnedArray) families.
    const sema::TypeRef* rt = nullptr;
    if (ld->type) rt = ctx_.resolvedType(*ld->type);
    if (!rt) rt = ctx_.exprType(*ld->initializer);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (builtinTypeName(rt) != "Array") return false;
    // The initializer must be a fresh-array MATERIALIZER call: `recv.ToArray()`,
    // `recv.Sorted(...)`, or `recv.Reversed()` — a CallExpr whose callee is a
    // member access naming one of those methods. (These are stdlib builtins with
    // no resolved user MethodSymbol; recognized structurally by member name.)
    auto* call = dynamic_cast<const ast::CallExpr*>(ld->initializer.get());
    if (!call || !call->callee) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call->callee.get());
    if (!ma) return false;
    std::string_view mm = ma->member;
    // #1027 / stdlib §9.6: `string.Split(...)` returns a fresh `owned Array<string>`
    // whose substrings are created at rc=1 (the array is their sole owner). A plain
    // `var parts := s.Split(...)` was neither flagged for the §28.3 affine auto-drop
    // nor reclaimed here, so the backing + every substring leaked. It is recognized
    // alongside the ToArray/Sorted/Reversed materializers: the scope-end reclaim
    // (bz_array_free with the Array<string> string-release per-element disposer,
    // selected above by element type) frees the backing AND releases each substring
    // exactly once — the same disposal the `using var` form already performs, so
    // plain `var` and `using var` reach parity. No double-free: the substrings are
    // freshly created (no source alias retains them, unlike ToArray's +1 co-owned
    // borrow), so the single release frees each exactly once.
    return mm == "ToArray" || mm == "Sorted" || mm == "Reversed" ||
           mm == "Split";
}


// #1004 / stdlib §5 / §3 / §28: is `e` an EXPRESSION codegen lowers to a FRESH,
// OWNERLESS `Array<T>` temporary — i.e. a fresh-array materializer call
// (`recv.ToArray()`, `recv.Sorted(...)`, or `recv.Reversed()`) whose result is
// a `bz_array_t` no named local/field/param owns? These three stdlib builtins
// always return a fresh borrowing `Array<T>` (codegen allocates it via
// `bz_lazy_to_array` / `bz_array_copy`); when such a call appears as the
// RECEIVER of a copy-form `.Sorted()`/`.Reversed()`, its result is consumed
// only as the copy source and is otherwise unreferenced, so the input must be
// reclaimed after the copy (else it leaks — issue #1004).
//   Soundness — this is intentionally NARROW (a CallExpr to a whitelisted
//   materializer is the ONLY shape recognised), so a receiver that is NOT a
//   fresh ownerless temporary is never freed:
//     * a named-local / field / param identifier receiver (`t.Sorted()`,
//       `myArr.Sorted()`) is an IdentifierExpr/MemberAccessExpr, NOT a CallExpr
//       → not matched → not freed (would be a UAF/double-free; `t` is freed by
//       its own owner / #1003);
//     * a `move`/alias/`new Array<T>(...)` receiver is likewise not a
//       materializer CallExpr → not matched;
//     * the result type is required to be a builtin borrowing `Array` (name-head
//       exactly "Array", declaration-less) — excluding Owned* families and any
//       user type with a same-named method.
//   When matched, the input array carries no disposal obligation on its
//   ELEMENTS except the `Array<string>` co-owned +1 (ToArray/Sorted/Reversed all
//   `bz_array_retain_strings` their result); the caller passes the matching
//   per-element disposer (the string-release thunk for `Array<string>`, null
//   otherwise) to `bz_array_free`, releasing that +1 symmetrically.
bool Emitter::isFreshOwnerlessArrayMaterializerExpr(const ast::Expr* e) const {
    if (!e) return false;
    // Result type must be a builtin borrowing `Array<T>` (name-head "Array",
    // declaration-less) — not Owned*, not a dict-/llist-backed family.
    const sema::TypeRef* rt = ctx_.exprType(*e);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (builtinTypeName(rt) != "Array") return false;
    // The expression itself must be a fresh-array MATERIALIZER call:
    // `recv.ToArray()`, `recv.Sorted(...)`, `recv.Reversed()`, or `s.Split(...)`.
    auto* call = dynamic_cast<const ast::CallExpr*>(e);
    if (!call || !call->callee) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call->callee.get());
    if (!ma) return false;
    std::string_view mm = ma->member;
    // #1027: `s.Split(...)` is a fresh `owned Array<string>` materializer too — a
    // bare `s.Split(...).Length` (the result consumed as a receiver, then dropped)
    // leaked its backing + substrings without this. Reclaimed exactly once via
    // bz_array_free with the Array<string> string-release disposer (selected by
    // element type at the call site), mirroring the ToArray/Sorted/Reversed temps.
    return mm == "ToArray" || mm == "Sorted" || mm == "Reversed" ||
           mm == "Split";
}


// #1013 / §28.3 / stdlib §5: is `e` a fresh-DICTIONARY materializer call
// (`recv.ToDictionary(...)`) whose result is a builtin borrowing `Dictionary<K,V>`
// temporary that no named local/field/param owns? Like the Array materializers
// above, when such a call appears UNBOUND as the receiver of a chained borrowing
// operation (`a.ToDictionary(k).Count()`) the intermediate Dictionary is owned by
// nothing and must be reclaimed at end-of-full-expression, via bz_dict_free (it is
// dict-backed: a bz_dict_t*, not a bz_array_t*). Scoped to the builtin `Dictionary`
// name-head (declaration-less) and the `ToDictionary` method, mirroring the Array
// recognizer's narrowness — a bound/`move`d/aliased receiver is not a `ToDictionary`
// CallExpr → never matched → never freed (no UAF / double-free; that dict is freed
// by its own owner / using-var disposal). `ToSet()` is intentionally NOT matched:
// its temporary is already reclaimed on the existing path, so adding a second free
// would double-free.
bool Emitter::isFreshOwnerlessDictMaterializerExpr(const ast::Expr* e) const {
    if (!e) return false;
    const sema::TypeRef* rt = ctx_.exprType(*e);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (builtinTypeName(rt) != "Dictionary") return false;
    auto* call = dynamic_cast<const ast::CallExpr*>(e);
    if (!call || !call->callee) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call->callee.get());
    if (!ma) return false;
    return ma->member == "ToDictionary";
}


// #1006(b) / §28.3 / §28.8 / stdlib §3 / §5: is `e` a CALL to a USER method whose
// declared return type carries `owned` (`method M(): owned Array<T>`) and whose
// RESULT is a builtin borrowing `Array<T>` temporary that no named local/field/
// param owns? Such a call produces a fresh, must-consume (affine, §28.3) owned
// array; when it appears UNBOUND as the receiver of a chained borrowing operation
// (`P.Fold(s).Sorted()`), nothing else takes ownership of the intermediate, so it
// must be reclaimed at end-of-full-expression or it leaks (#1006b). This is the
// generalization of #1004's name-whitelisted materializer free along the `owned`
// axis: instead of matching the builtin ToArray/Sorted/Reversed name-head, we
// recognise the producing method's `owned` return marker (the authority that the
// call OWNS its result and the caller must dispose it).
//   Soundness — kept exactly as narrow as #1004, distinguishing transfer from
//   the borrowing chained-receiver use:
//     * the result type must be a builtin borrowing `Array<T>` (name-head exactly
//       "Array", declaration-less) — NOT an Owned* / Set / Dictionary / LinkedList
//       family, whose backing is freed differently and whose elements are owned
//       (an owning collection would need an element cascade, not a backing free);
//     * the EXPRESSION must be a CallExpr to a MethodSymbol whose ast::MethodDecl
//       has `returnOwned` — a named-local / field / param / `move` / alias / `new`
//       receiver is NOT a CallExpr (or its callee is not an owned-returning method)
//       → never matched → never freed (no UAF / double-free; that array is freed
//       by its own owner / using-var disposal);
//     * because this only ever fires for a receiver CallExpr evaluated INLINE as a
//       chained operation's receiver, the temporary is genuinely unbound — a
//       `using var f := P.Fold(s); f.Sorted()` reads `f` via an IdentifierExpr
//       receiver (not a CallExpr) and is NOT matched (so the using-var disposal is
//       the sole free — no double-free).
//   Per stdlib §3/§5 ("the result is a borrowing array … the caller is responsible
//   for disposing the array itself, not its elements") the per-element disposer is
//   exactly #1004's: null for value/class borrows, the string-release thunk for an
//   `Array<string>` whose materializer retained a +1/element.
bool Emitter::isUnboundOwnedBorrowingArrayCallExpr(const ast::Expr* e) const {
    if (!e) return false;
    // Result type must be a builtin borrowing `Array<T>` (name-head "Array",
    // declaration-less) — not Owned*, not a dict-/llist-backed family.
    const sema::TypeRef* rt = ctx_.exprType(*e);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (builtinTypeName(rt) != "Array") return false;
    // The expression must be a CALL whose resolved callee is a user MethodSymbol
    // declared with an `owned` return (the authority that the call owns its
    // result). The callee symbol is keyed on the CallExpr's `callee` node.
    auto* call = dynamic_cast<const ast::CallExpr*>(e);
    if (!call || !call->callee) return false;
    auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(*call->callee));
    if (!ms) return false;
    auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl);
    return md && md->returnOwned;
}


// #1006(b) / #1004 / §28.3 / stdlib §3 / §5: if `recvExpr` is an UNBOUND fresh
// borrowing-`Array<T>` temporary — either a builtin materializer call
// (`X.ToArray()`/`X.Sorted()`/`X.Reversed()`, #1004) OR an `owned`-returning user
// method call (`P.Fold(s)` with `Fold(): owned Array<T>`, #1006b) — emit the
// end-of-full-expression `bz_array_free` of its backing `recvVal`, with the
// per-element disposer the borrowing snapshot requires (null for value/class
// borrows; the string-release thunk for an `Array<string>` whose materializer
// retained a +1/element). A no-op for any other receiver shape, so a bound /
// `move`d / returned / owned-arg / aliased receiver is never freed → no UAF, no
// double-free. The single place this free logic lives, shared by the chained-
// method-receiver sites (`freeFreshInputTemp`) and the `.Length`/`.Count` borrow-
// position property read; the CALLER must already have fully read the result it
// derives from `recvVal` BEFORE calling this (the backing is released here).
void Emitter::freeUnboundOwnedBorrowingArrayTemp(
        const ast::Expr* recvExpr, llvm::Value* recvVal,
        const sema::TypeRef* recvTy) {
    if (!recvExpr || !recvVal) return;
    // #1013: a fresh unbound `Dictionary<K,V>` materializer temp (`a.ToDictionary
    // (k).Count()`) is dict-backed — reclaim it with bz_dict_free (the struct AND
    // its entries), NOT bz_array_free which would leak the bz_dict struct. The
    // borrowing chain owns the dict, not its keys/values (stdlib §5), so the
    // per-value disposer is null: bz_dict_free frees the table, its key bytes, and
    // the value-box storage (value types carry nothing to dispose).
    if (isFreshOwnerlessDictMaterializerExpr(recvExpr)) {
        auto* fn = declareC("bz_dict_free", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy()});
        builder_.CreateCall(
            fn, {recvVal, llvm::ConstantPointerNull::get(ptrTy())});
        return;
    }
    if (!isFreshOwnerlessArrayMaterializerExpr(recvExpr) &&
        !isUnboundOwnedBorrowingArrayCallExpr(recvExpr)) return;
    // Per-element disposer: release the `Array<string>` co-owned +1 that the
    // materializer/snapshot added; null for value/class element borrows (stdlib
    // §3/§5: the caller owns the array, not its elements).
    llvm::Value* elemDisp = llvm::ConstantPointerNull::get(ptrTy());
    const sema::TypeRef* el = (recvTy && !recvTy->genericArgs.empty())
        ? recvTy->genericArgs[0] : nullptr;
    if (el && !el->nullable &&
            el->kind == sema::TypeRef::Kind::Primitive &&
            el->primitive == sema::PrimitiveType::String) {
        ElementPolicy ep = elementPolicyFor(el, /*owningFamily=*/false);
        if (ep.cat == ElementPolicy::Category::RuntimeManaged)
            elemDisp = stringElementReleaseThunk();
    }
    auto* fn = declareC("bz_array_free", llvm::Type::getVoidTy(llvm_),
                        {ptrTy(), ptrTy()});
    builder_.CreateCall(fn, {recvVal, elemDisp});
}


// #1003 / stdlib §5 / §28: compute borrowingArrayFreeEligible_ for a block body.
// Mirrors collectStringReleaseEligible: find every plain `var` borrowing-Array
// local bound to a fresh materializer, then run the conservative (type-agnostic)
// StringEscapeWalker over the whole body; a local the walk proves NEVER escapes
// is eligible for a scope-end bz_array_free of its single producer reference. The
// walk over-approximates escape: a `return a;` / `move a` / owned-arg / store-to-
// field / alias-binding / lambda-capture all taint the local → excluded → no
// reclaim (a leak, the safe direction). Only a local whose every use is a borrow-
// position read (`a.Length`, `a[i]`, `for var x in a`, a method/property receiver)
// stays eligible — for those, the local is provably the sole owner of the backing,
// so freeing it once at scope end is free of UAF and of double-free.
void Emitter::collectBorrowingArrayFreeEligible(
        const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    borrowingArrayFreeEligible_.clear();
    dictMaterializerFreeEligible_.clear();  // #1028
    std::vector<const ast::LocalDecl*> candidates;
    std::vector<const ast::LocalDecl*> dictCandidates;  // #1028
    std::function<void(const ast::Stmt*)> scan = [&](const ast::Stmt* s) {
        if (!s) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            if (isFreshBorrowingArrayMaterializerLocal(ld))
                candidates.push_back(ld);
            // #1028: a plain `var d := src.ToDictionary(...)` — a fresh
            // `owned Dictionary<K,V>` materializer the local solely owns. Excluded:
            // a `using var` (disposed via registerUsingDisposal → would double-free).
            else if (ld && !ld->isUsing && ld->initializer &&
                     isFreshOwnerlessDictMaterializerExpr(ld->initializer.get()))
                dictCandidates.push_back(ld);
        }
        // Recurse into nested blocks (mirrors collectStringReleaseEligible).
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
            borrowingArrayFreeEligible_.insert(static_cast<const void*>(ld));
    }
    // #1028: same conservative type-agnostic escape walk for dict-materializer
    // locals — a local proven never to escape is the sole owner of the fresh
    // dict, so a scope-end bz_dict_free runs exactly once (no UAF, no double-free).
    for (const ast::LocalDecl* ld : dictCandidates) {
        StringEscapeWalker w{ctx_, static_cast<const void*>(ld)};
        for (auto& s : body) { w.stmt(s.get()); if (w.escaped) break; }
        if (!w.escaped)
            dictMaterializerFreeEligible_.insert(static_cast<const void*>(ld));
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


// §27.5 / §28.9 / #672 / #802: compute boxCaptureLocals_ for the function body.
//
// A +1-box-producer local that is CAPTURED by a lambda has its scope-end
// bz_box_release suppressed by BoxEscapeWalker (the capture is an escape, so the
// local is NOT in boxReleaseEligible_). The owning lambda's per-lambda envdtor
// performs that deferred release instead. With ONE capturing env this is a pure
// transfer (the env releases the producer's +1). With N>1 envs (#802) each env
// holds the same box pointer; the rc-retain model keeps it sound: the FIRST
// capture transfers the producer's +1, every ADDITIONAL capture bz_box_retains an
// independent +1, and EVERY env's envdtor releases exactly one — so the rc
// reaches 0 after the last env dies (no leak, no double-free). This set therefore
// admits a box-producer local captured by AT LEAST ONE lambda; the per-capture
// retain (in lowerLambda) and the per-envdtor release balance the count.
//
// Counting uses FULL-DESCENT lambda collection (collectAllLambdas) + per-lambda
// collectCapturedSyms (which itself descends): a box captured by a nested lambda
// is counted at BOTH the enclosing and the nested lambda (each env holds it), so
// any multi-env case yields count >= 2 and is conservatively excluded.
void Emitter::collectBoxCaptureLocals(
        const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    boxCaptureLocals_.clear();
    singleCaptureLocals_.clear();  // #694

    // Candidate box-producer locals (same predicate as collectBoxReleaseEligible).
    std::unordered_set<const void*> candidates;
    std::function<void(const ast::Stmt*)> scan = [&](const ast::Stmt* s) {
        if (!s) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            if (!ld->isUsing && isPlusOneBoxProducer(ld))
                candidates.insert(static_cast<const void*>(ld));
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

    // Tally how many distinct lambda envs capture each LOCAL — ALL captured
    // locals (#694 needs the general count for singleCaptureLocals_), not just the
    // box-producer candidates.
    std::vector<const ast::LambdaExpr*> lambdas;
    collectAllLambdas(body, lambdas);
    std::unordered_map<const void*, int> captureCount;
    for (const ast::LambdaExpr* lam : lambdas) {
        std::unordered_set<const sema::Symbol*> caps;
        collectCapturedSyms(*lam, caps);
        for (const sema::Symbol* s : caps) {
            if (auto* loc = dynamic_cast<const sema::LocalSymbol*>(s)) {
                if (loc->decl)
                    captureCount[static_cast<const void*>(loc->decl)]++;
            }
        }
    }
    for (const auto& [d, n] : captureCount) {
        if (n >= 1 && candidates.count(d))
            boxCaptureLocals_.insert(d);      // #802: any captured box producer
        if (n == 1)
            singleCaptureLocals_.insert(d);   // #694: captured by exactly one env
    }
}


// §27.5 / #694: collect the LambdaExpr nodes that DEFINITELY escape the function
// — those directly returned (`return () => …`). For such a closure the defining
// scope is gone before the closure can be dropped, so its envdtor (run at the
// closure's last-drop) can safely free a capture cell it solely holds. This is a
// DEFINITE-escape under-approximation (the inverse of the conservative
// capturedByEscaping): a lambda only enters this set when it is syntactically the
// returned value, never speculatively — required for the cell-free in lowerLambda
// to be sound (a non-escaping closure's envdtor runs at scope end, where freeing
// the cell would UAF the outer scope's later access).
void Emitter::collectDefinitelyEscapingLambdas(
        const std::vector<std::unique_ptr<ast::Stmt>>& body) {
    definitelyEscapingLambdas_.clear();
    // One pass collects: (a) a DIRECT `return () => …` lambda → escapes; (b) the
    // set of locals that are REASSIGNED anywhere; (c) `return <ident>` candidates.
    // After the pass, a candidate `return f` adds f's INITIALIZER lambda iff f is
    // never reassigned (#694 extend) — otherwise f's value at the return is not
    // its init lambda, so freeing the init lambda's cell (dropped at the
    // reassignment) could UAF a later read of the captured local.
    std::unordered_set<const void*> reassigned;
    std::vector<const ast::LocalDecl*> returnedVars;
    // #694 field-store: `this.f := () => …` stores the closure on the receiver,
    // which outlives the method → a definite escape. Counted per field so a field
    // assigned more than once (its earlier lambda dropped mid-method) is excluded
    // (the same reassignment hazard as a returned var). Only `this.f`/implicit `f`
    // (the receiver's own field); `other.f` is excluded (other may be scope-bound).
    std::unordered_map<const sema::Symbol*, int> fieldAssignCount;
    std::unordered_map<const sema::Symbol*, const ast::LambdaExpr*> fieldLambda;
    std::function<void(const ast::Stmt*)> scan = [&](const ast::Stmt* s) {
        if (!s) return;
        if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
            sema::Symbol* tsym =
                as->target ? ctx_.exprSymbol(*as->target) : nullptr;
            if (auto* loc = dynamic_cast<sema::LocalSymbol*>(tsym)) {
                if (loc->decl)
                    reassigned.insert(static_cast<const void*>(loc->decl));
            } else if (auto* fld = dynamic_cast<sema::FieldSymbol*>(tsym)) {
                bool thisField =
                    dynamic_cast<const ast::IdentifierExpr*>(as->target.get())
                        != nullptr;
                if (!thisField)
                    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                            as->target.get()))
                        if (auto* rid = dynamic_cast<const ast::IdentifierExpr*>(
                                ma->target.get()))
                            thisField = (rid->name == "this");
                if (thisField) {
                    fieldAssignCount[fld]++;
                    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(
                            as->value.get()))
                        fieldLambda[fld] = lam;
                }
            }
            return;
        }
        if (auto* r = dynamic_cast<const ast::ReturnStmt*>(s)) {
            if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(r->value.get()))
                definitelyEscapingLambdas_.insert(
                    static_cast<const void*>(lam));
            else if (auto* id =
                         dynamic_cast<const ast::IdentifierExpr*>(r->value.get()))
                if (auto* loc =
                        dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*id)))
                    if (auto* ld =
                            dynamic_cast<const ast::LocalDecl*>(loc->decl))
                        returnedVars.push_back(ld);
            return;
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
    for (const ast::LocalDecl* ld : returnedVars) {
        if (reassigned.count(static_cast<const void*>(ld))) continue;
        if (auto* lam =
                dynamic_cast<const ast::LambdaExpr*>(ld->initializer.get()))
            definitelyEscapingLambdas_.insert(static_cast<const void*>(lam));
    }
    // #694 field-store: a `this.f := () => …` whose field is assigned EXACTLY ONCE
    // in the method (no within-method reassignment that would drop the lambda
    // mid-scope) escapes via the receiver — add it.
    for (const auto& [fld, n] : fieldAssignCount) {
        if (n != 1) continue;
        auto it = fieldLambda.find(fld);
        if (it != fieldLambda.end())
            definitelyEscapingLambdas_.insert(
                static_cast<const void*>(it->second));
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
        // (b0) #910 / §30.5 Pattern B: `using var c := await <expr>`. Awaiting an
        // OwnedTask<T> yields an owned T (§30.5); the CallExpr branch below does
        // not match the `await` UnaryExpr, so the owned result leaked. A `using
        // var` of a non-owned value is a sema error, so a class/interface await
        // result here is owned and must be disposed at scope end (synchronously,
        // per the `using` form — `await using` handles its own async disposal).
        if (auto* un = dynamic_cast<const ast::UnaryExpr*>(
                ld->initializer.get())) {
            if (un->op == "await") {
                const sema::TypeRef* initTy = ctx_.exprType(*ld->initializer);
                if (isClassOrInterfaceRef(initTy) ||
                    isClassOrInterfaceRef(resolveTypeParamRef(initTy)))
                    return true;
            }
        }
        // (b0b) #911 / §25: a user-defined binary operator whose `operator OP`
        // returns `owned T` — `using var c := a + b`. The initializer is a
        // BinaryExpr, not a CallExpr, so the call branch below missed it and the
        // owned operator result leaked. Sema sets the BinaryExpr's exprSymbol to
        // the operator MethodSymbol; its OperatorDecl carries returnOwned.
        if (dynamic_cast<const ast::BinaryExpr*>(ld->initializer.get())) {
            if (auto* ms = dynamic_cast<sema::MethodSymbol*>(
                    ctx_.exprSymbol(*ld->initializer))) {
                auto* od = dynamic_cast<const ast::OperatorDecl*>(ms->decl);
                const sema::TypeRef* initTy = ctx_.exprType(*ld->initializer);
                if (od && od->returnOwned &&
                    (isClassOrInterfaceRef(initTy) ||
                     isClassOrInterfaceRef(resolveTypeParamRef(initTy))))
                    return true;
            }
        }
        // (b0c) #1029 / §28.3: built-in `??` (NullCoalescing) with a fresh
        // owned-class RHS — mirrors producerState/unwindOwnedLocalClass. The
        // BinaryExpr has no sema MethodSymbol (built-in `??`) so (b0b) missed it.
        // Gated on isFreshOwnedClassProducer(RHS) so a borrow-RHS `??` (no owned
        // result) never registers a disposal. // per §28.3
        if (auto* be = dynamic_cast<const ast::BinaryExpr*>(
                ld->initializer.get())) {
            if (be->op == "??" && be->right &&
                isFreshOwnedClassProducer(be->right.get())) {
                const sema::TypeRef* initTy = ctx_.exprType(*ld->initializer);
                if (isClassOrInterfaceRef(initTy) ||
                    isClassOrInterfaceRef(resolveTypeParamRef(initTy)))
                    return true;
            }
        }
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
                    // §28.10 / §14.2 (sweep #6): the return type may be a bare
                    // generic type PARAMETER `T` (declaration==null), e.g.
                    // `Make(): owned T` in a generic class. isClassOrInterfaceRef
                    // rejects it, so `using var x := Make()` leaked the owned T.
                    // In the per-instantiation specialization (where this disposal
                    // is registered) the active substitution resolves T to its
                    // concrete type — accept it when that resolves to a class /
                    // interface (a value-typed T resolves to a non-reference and
                    // correctly stays unregistered).
                    const sema::TypeRef* initTy =
                        ctx_.exprType(*ld->initializer);
                    if (isOwned &&
                        (isClassOrInterfaceRef(initTy) ||
                         isClassOrInterfaceRef(resolveTypeParamRef(initTy))))
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
                        // §4.11 (#746 R41-F): OwnedStack.Pop() /
                        // OwnedQueue.Dequeue() transfer the owned element to the
                        // caller, exactly like PopLast/PopFirst. Gate on the
                        // OWNING receiver — a borrowing Stack/Queue returns a
                        // BORROW, and disposing that would double-free.
                        bool ownedPopDequeue = false;
                        if ((nm == "Pop" || nm == "Dequeue") && ma->target) {
                            const sema::TypeRef* rt =
                                ctx_.exprType(*ma->target);
                            std::string_view rcn =
                                (rt && rt->astNode && rt->astNode->name &&
                                 !rt->astNode->name->parts.empty())
                                    ? rt->astNode->name->parts[0]
                                    : std::string_view{};
                            ownedPopDequeue =
                                rcn == "OwnedStack" || rcn == "OwnedQueue";
                        }
                        if (nm == "PopLast" || nm == "PopFirst" ||
                            nm == "PopAt"   || nm == "PopValue" ||
                            nm == "Deserialize" || ownedPopDequeue) {
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


bool Emitter::usingVarIsDrainedOwningElement(const ast::LocalDecl* ld) {
    // #990 / §4.5 / §14.2: `using var _ := item` where `item` is the loop
    // variable of `for var item in coll.Drain()` over an OWNING sequence
    // collection (OwnedArray/Stack/Queue/LinkedList).  Such an element is a class
    // object with a vtable by the stdlib invariant (owning collections hold only
    // class elements).  In the GENERIC TEMPLATE body the element's static type is
    // the unresolved type parameter `T` (declaration == null, genericSubstSemaTy_
    // empty), so classOfLocal cannot recover a concrete class and the static-
    // disposal path misses it — the drained element would leak.  Recognise this
    // shape and route it to the dynamic (vtable-slot-2) dispose so it discharges
    // exactly once.  The concrete (non-generic) case is already handled by
    // classOfLocal returning the element's declaration before this is reached.
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(ld->initializer.get());
    if (!id) return false;
    auto* loc = dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*id));
    if (!loc) return false;
    auto* fs = dynamic_cast<const ast::ForStmt*>(loc->decl);
    if (!fs || !fs->iterable) return false;
    // The iterable must be a `.Drain()` call on one of the four owning sequence
    // collections (the same shape lowerOwnedSeqDrainFor accepts).  Excludes the
    // OwnedDictionary drain (its loop var is a DrainedEntry, handled separately)
    // and the borrowing collections (which yield borrows, not owned elements).
    auto* ce = dynamic_cast<const ast::CallExpr*>(fs->iterable.get());
    if (!ce) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(ce->callee.get());
    if (!ma || ma->member != "Drain" || !ma->target) return false;
    const sema::TypeRef* ct = ctx_.exprType(*ma->target);
    if (!ct || ct->kind != sema::TypeRef::Kind::Named) return false;
    std::string_view cn;
    if (ct->astNode && ct->astNode->name && !ct->astNode->name->parts.empty())
        cn = ct->astNode->name->parts[0];
    else if (!ct->importedName.empty())
        cn = ct->importedName;
    return cn == "OwnedArray" || cn == "OwnedStack" ||
           cn == "OwnedQueue" || cn == "OwnedLinkedList";
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

    // §15.4 (R39): the empty literal `[]` parses as a NON-dict
    // CollectionLiteralExpr (the empty form is syntactically ambiguous), but
    // with a Dictionary<K,V>/OwnedDictionary<K,V> target it must construct an
    // empty dictionary. Delegate to the dict materializer. Without this the
    // ordered-collection path below returns null for a dict target (it is not
    // an isCollectionTypeName), leaving the local/field slot uninitialized — a
    // `using var d: Dictionary<…> := []` then free()s a garbage pointer
    // (SIGABRT). Every empty-`[]` site calls this helper first, so this single
    // delegation also covers field initializers and `return []`.
    if (cl->elements.empty() && cl->keys.empty() && isDictTypeName(collTy))
        return materializeHeapDictLiteral(cl, collTy);

    // §4.6/§4.7 (#738): LinkedList<T>/OwnedLinkedList<T> are bz_llist_t-backed,
    // not bz_array_t — and isCollectionTypeName deliberately EXCLUDES them, so
    // handle the literal here (before that guard) via bz_llist_alloc +
    // bz_llist_add_last, exactly what `new LinkedList<T>()` + `.AddLast(e)` emit.
    // Otherwise the literal fell through to the inline-array GEP path and the
    // LinkedList consumers (.Count, scope-end disposal) misread a bz_array_t* as
    // a bz_llist_t* — a runtime free() of an invalid pointer (empty `[]`) or a
    // malformed element GEP (non-empty). Like `.AddLast`, no string-retain.
    if (isLinkedListTypeName(collTy)) {
        const sema::TypeRef* er =
            (!collTy->genericArgs.empty()) ? collTy->genericArgs[0] : nullptr;
        llvm::Type* elTy = er ? lowerTypeRef(er) : i32();
        int64_t es = collectionElemSize(collTy);
        auto* allocFn = declareC("bz_llist_alloc", ptrTy(), {i64()});
        llvm::Value* ll =
            builder_.CreateCall(allocFn, {builder_.getInt64(es)}, "clit.ll");
        auto* addFn = declareC("bz_llist_add_last", ptrTy(),
                               {ptrTy(), ptrTy(), i64()});
        for (const auto& elemPtr : cl->elements) {
            if (!elemPtr) continue;
            llvm::Value* ev = nullptr;
            if (auto* nested = dynamic_cast<const ast::CollectionLiteralExpr*>(
                    elemPtr.get())) {
                if (!nested->isDict && er)
                    ev = materializeHeapCollectionLiteral(nested, er);
            }
            if (!ev) ev = lowerExpr(*elemPtr);
            if (!ev) continue;
            if (er && isNullableValueTy(er)) {
                llvm::StructType* expected = nullableValueStructTy(er);
                if (expected && ev->getType() != expected)
                    ev = ev->getType()->isPointerTy() ? nullableValueNull(er)
                                                       : wrapNullableValue(er, ev);
            } else if (ev->getType() != elTy) {
                ev = coerceNumericToType(ev, elTy, isUnsignedTargetExpr(*elemPtr));
            }
            llvm::AllocaInst* slot = entryAlloca(ev->getType());
            builder_.CreateStore(ev, slot);
            builder_.CreateCall(addFn, {ll, slot, builder_.getInt64(es)});
        }
        return ll;
    }

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


// #999 / §15.4: materialize a collection literal in CALL-ARGUMENT position into
// a fresh owned heap collection of the parameter's target type. §15.4 admits a
// collection literal "via the parameter type of a method call", so `foo([1,2,3])`
// where foo's parameter is Array<int> must CONSTRUCT the collection from the
// literal — it is not a general expression value (lowerExpr returns null for a
// CollectionLiteralExpr), so the local-decl / return / field-init paths each
// materialize it via a dedicated path; the call-argument path had none, so the
// literal lowered to null/garbage (an empty collection for ordinary methods; a
// dereferenced garbage pointer → crash for the byte-array FFI factories).
//
// This is the single entry the argument path uses. It reuses the EXACT same
// construction machinery the local-decl / field-init paths use (no per-element
// reimplementation): the dictionary literal form routes to
// materializeHeapDictLiteral; every sequence form (Array/OwnedArray, Set/OwnedSet,
// Stack/Queue and owned, LinkedList/OwnedLinkedList) routes to
// materializeHeapCollectionLiteral (which itself dispatches Set and LinkedList).
// Returns a `ptr` to the constructed heap collection, or null when `collTy` is
// not a recognised heap-collection target (caller declines without miscompiling).
llvm::Value* Emitter::materializeCollectionLiteralForArg(
        const ast::CollectionLiteralExpr* cl, const sema::TypeRef* collTy) {
    if (!cl || !collTy) return nullptr;
    // Dictionary literal `[k: v, …]` (or empty `[]` with a dict target).
    if (cl->isDict || isDictTypeName(collTy))
        return materializeHeapDictLiteral(cl, collTy);
    // Every sequence-literal form (Array/OwnedArray/Set/OwnedSet/Stack/Queue and
    // owned forms, LinkedList/OwnedLinkedList). materializeHeapCollectionLiteral
    // routes Set→materializeHeapSetLiteral and LinkedList internally.
    return materializeHeapCollectionLiteral(cl, collTy);
}


// #999 / stdlib §10: build (once) a concrete `Array<byte>` sema::TypeRef for the
// byte-array FFI factory arguments (RawBytes.From / CString.FromBytes). Shaped
// exactly like makeConcreteCollectionResult's Array<byte> (Named, no declaration,
// a static "Array" astNode, genericArgs[0] = byte) so materializeHeapCollection-
// Literal's name/element-size/element-coercion logic produces the same bz_array_t
// a `using var b: Array<byte> := […]` would.
const sema::TypeRef* Emitter::builtinByteArrayTypeRef() {
    static const ast::Type* kArrayAstNode = []() -> ast::Type* {
        auto* qn = new ast::QualifiedName;
        qn->parts.push_back("Array");
        auto* t = new ast::Type;
        t->kind = ast::TypeKind::Named;
        t->name.reset(qn);
        return t;
    }();
    auto* tr = ctx_.makeTypeRef();
    tr->kind = sema::TypeRef::Kind::Named;
    tr->declaration = nullptr;
    tr->astNode = kArrayAstNode;
    tr->genericArgs.push_back(
        ctx_.primitiveTypeRef(sema::PrimitiveType::Byte, /*nullable=*/false));
    return tr;
}


// #999 / stdlib §10 / §15.4: lower the single `Array<byte>` BORROW argument of a
// byte-array FFI factory (RawBytes.From / CString.FromBytes). These intrinsic
// sites have a dedicated call-lowering path (not buildOrderedArgs) that lowered
// the argument via lowerExpr — which returns null for a CollectionLiteralExpr, so
// `RawBytes.From([10,20])` dereferenced a garbage pointer and crashed (#999). The
// factory copies the bytes (stdlib §10: independent buffer) and only borrows the
// source, so materialize the literal into an owned Array<byte> temporary and
// register its scope-end disposal (disposed exactly once on the normal AND unwind
// paths). A non-literal argument: lower via lowerExpr and — for a plain `var`
// (non-using) local — register its scope-end collection disposal exactly once
// (#998 / §28.3), since the non-using lowerStmt path calls only
// registerUnwindOwnedLocal, which skips collection types.
llvm::Value* Emitter::lowerByteArrayFactoryArg(const ast::Expr& argExpr) {
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(&argExpr)) {
        const sema::TypeRef* byteArr = builtinByteArrayTypeRef();
        llvm::Value* coll = materializeCollectionLiteralForArg(cl, byteArr);
        if (!coll) return nullptr;
        llvm::AllocaInst* slot = entryAlloca(coll->getType());
        builder_.CreateStore(coll, slot);
        if (!disposeStack_.empty())
            disposeStack_.back().push_back(
                buildCollectionTempDisposal(slot, byteArr));
        return coll;
    }
    // #998 / §28.3: when the argument is a plain `var` (non-using) local of
    // collection type (e.g. `var b: Array<byte>`), no scope-end disposal is
    // registered at declaration time — the non-using lowerStmt path calls only
    // registerUnwindOwnedLocal, which skips collections (§28.3 line 4910 covers
    // class instances, not collection builtins). Register the caller's array for
    // scope-end disposal here, on the NORMAL AND UNWIND paths, so the factory
    // merely borrows the bytes while the caller disposes exactly once at scope end.
    //
    // Guards:
    //   isUsing — a `using var b` already has disposal from registerUsingDisposal;
    //             skip to prevent double-free.
    //   alreadyReg — if the same local is passed to multiple FFI calls in the same
    //             scope (e.g. RawBytes.From(b); RawBytes.From(b)), only register
    //             disposal once to prevent double-free at scope exit.
    //   scope depth — register in the scope where the local was declared
    //             (declScopeDepth_), not necessarily the innermost scope; this is
    //             correct whether the call is in the same or a nested scope.
    if (!disposeStack_.empty()) {
        if (sema::Symbol* rawSym = ctx_.exprSymbol(argExpr)) {
            if (auto* loc = dynamic_cast<sema::LocalSymbol*>(rawSym)) {
                if (!loc->isUsing && loc->decl) {
                    auto slotIt = locals_.find(loc->decl);
                    if (slotIt != locals_.end()) {
                        llvm::AllocaInst* argSlot = slotIt->second;
                        // Determine the scope in which this local was declared.
                        std::size_t depth = disposeStack_.size() - 1;
                        auto dd = declScopeDepth_.find(loc->decl);
                        if (dd != declScopeDepth_.end() && dd->second >= 1 &&
                                dd->second - 1 < disposeStack_.size())
                            depth = dd->second - 1;
                        // Check if already registered (guard against double-registration).
                        bool alreadyReg = false;
                        for (const auto& d : disposeStack_[depth])
                            if (d.slot == argSlot &&
                                    (d.collection || d.dict || d.linkedList)) {
                                alreadyReg = true;
                                break;
                            }
                        if (!alreadyReg) {
                            const sema::TypeRef* byteArr = builtinByteArrayTypeRef();
                            disposeStack_[depth].push_back(
                                buildCollectionTempDisposal(argSlot, byteArr));
                        }
                    }
                }
            }
        }
    }
    return lowerExpr(argExpr);
}


// #999 / §15.4 / §28.3: build the scope-end Disposal entry for an owned temporary
// heap collection materialized for a BORROW call-argument. The temp is an owned
// local: the callee borrows it for the call duration, then it is disposed exactly
// once at scope exit on the normal AND unwind paths (disposeStack_ fires on both).
// The selection mirrors the `using var` collection / linked-list / dict
// registration in lowerLocalDecl (LlvmBackend_Setup.cpp ~6281–6430): the right
// runtime free (bz_array_free / bz_llist_free / bz_dict_free) and the right
// per-element disposer (class/interface owning thunk, owning Shared/Weak release,
// or a borrowing-string +1 release). `slot` is an alloca holding the collection
// pointer. NOT called for an `owned` parameter — ownership transfers into the
// callee, so the caller registers no disposal (§28.3).
Emitter::Disposal Emitter::buildCollectionTempDisposal(
        llvm::Value* slot, const sema::TypeRef* collTy) {
    auto* alloca = llvm::cast<llvm::AllocaInst>(slot);
    bool isOwning =
        collTy->astNode && collTy->astNode->name &&
        !collTy->astNode->name->parts.empty() &&
        collTy->astNode->name->parts[0].rfind("Owned", 0) == 0;
    auto isStrElem = [&](const sema::TypeRef* el, bool owning) {
        if (!el || el->nullable) return false;
        ElementPolicy p = elementPolicyFor(el, owning);
        return p.cat == ElementPolicy::Category::RuntimeManaged &&
               el->kind == sema::TypeRef::Kind::Primitive &&
               el->primitive == sema::PrimitiveType::String;
    };

    // LinkedList / OwnedLinkedList → bz_llist_free.
    if (isLinkedListTypeName(collTy)) {
        Disposal d{alloca, nullptr};
        d.linkedList = true;
        const sema::TypeRef* el =
            (!collTy->genericArgs.empty()) ? collTy->genericArgs[0] : nullptr;
        d.collectionElemDisposer = elementPolicyFor(el, isOwning).disposerThunk;
        if (!d.collectionElemDisposer && isStrElem(el, /*owning=*/false))
            d.collectionElemDisposer = stringElementReleaseThunk();
        return d;
    }

    // Dictionary / OwnedDictionary → bz_dict_free.
    if (isDictTypeName(collTy)) {
        Disposal d{alloca, nullptr};
        d.dict = true;
        const sema::TypeRef* vTy =
            (collTy->genericArgs.size() > 1) ? collTy->genericArgs[1] : nullptr;
        d.dictValueDisposer = elementPolicyFor(vTy, isOwning).disposerThunk;
        if (!d.dictValueDisposer)
            d.dictValueDisposer = dictValueBoxFreer(collTy);
        if (!d.dictValueDisposer && !isOwning && isStrElem(vTy, /*owning=*/false))
            d.dictValueDisposer = stringElementReleaseThunk();
        return d;
    }

    // #1012 / §4.10: a dict-backed Set/OwnedSet (a generic-dict-eligible element —
    // record, non-nullable primitive/enum, or string) frees via bz_dict_free (the
    // struct AND its entries), not bz_array_free — the latter leaks the bz_dict
    // struct. String elements are KEYS released via dictStringKeys before the free;
    // value-type/record/enum keys carry no per-element obligation. A class/
    // interface/nullable element is NOT dict-eligible (genericBackingType returns
    // null) → that Set is array-backed and falls through to bz_array_free below.
    const sema::TypeRef* el =
        (!collTy->genericArgs.empty()) ? collTy->genericArgs[0] : nullptr;
    bool isSetName =
        collTy->astNode && collTy->astNode->name &&
        !collTy->astNode->name->parts.empty() &&
        (collTy->astNode->name->parts[0] == "Set" ||
         collTy->astNode->name->parts[0] == "OwnedSet");
    bool setDictBacked = isSetName &&
        genericBackingType(collTy, 0, /*isSet=*/true, nullptr) != nullptr;
    if (setDictBacked) {
        Disposal d{alloca, nullptr};
        d.dict = true;
        if (isStrElem(el, /*owning=*/false)) d.dictStringKeys = true;
        return d;
    }

    // Array-backed: Array/OwnedArray/Set/OwnedSet/Stack/Queue and owned forms
    // → bz_array_free with the per-element disposer.
    Disposal d{alloca, nullptr};
    d.collection = true;
    d.collectionElemDisposer = elementPolicyFor(el, isOwning).disposerThunk;
    if (!d.collectionElemDisposer && isStrElem(el, /*owning=*/false))
        d.collectionElemDisposer = stringElementReleaseThunk();
    return d;
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
    if (!cl || !dictTy) return nullptr;
    if (!isDictTypeName(dictTy)) return nullptr;
    // §15.4 (R39): the empty literal `[]` parses with isDict=false; with a
    // Dictionary target it still constructs an empty dictionary (the populate
    // loop below is then a no-op). A NON-empty non-dict literal is a list, not
    // a dict — reject it so a list literal never lands on a dict target.
    bool emptyLiteral = cl->keys.empty() && cl->elements.empty();
    if (!cl->isDict && !emptyLiteral) return nullptr;
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


void Emitter::disposeDisplacedOwnedClassLocal(const ast::Expr* target,
                                              llvm::Value* addr,
                                              llvm::Type* elemTy,
                                              llvm::Value* newVal) {
    // §28.7 (#913/#1054): reassigning an owned class LOCAL must dispose the
    // displaced value before overwriting (the class analog of the string/
    // closure/box/owned-collection field-store helpers). Sound owned-detection:
    // the target must be a plain local variable that `unwindOwnedLocalClass`
    // classifies as a definitely-owned plain class/interface local (`new C()` /
    // `owned`-returning initializer). That EXCLUDES a borrow-initialized local,
    // a collection/task/subscription local, and any param (borrow OR owned — a
    // ParameterSymbol is not a LocalDecl), so a borrowed value owned elsewhere
    // is never disposed here (no double-free). The scope-end / explicit-dispose
    // path re-loads the slot and disposes the NEW value; here we dispose the OLD
    // one — exactly once each.
    if (!addr) return;
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(target);
    if (!id) return;  // field / index / static target — not a plain local
    auto* loc = dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*id));
    if (!loc) return;  // a parameter (borrow or owned) — excluded
    auto* ld = dynamic_cast<const ast::LocalDecl*>(loc->decl);
    if (!ld || !unwindOwnedLocalClass(ld)) return;  // not an owned plain class local
    llvm::Value* old = builder_.CreateLoad(elemTy, addr, "reassign.old");
    if (!old->getType()->isPointerTy()) return;
    // Null-guard (a moved-out slot holds null) AND identity-guard (a self-
    // assignment `c := c` would otherwise dispose the value being stored → UAF).
    llvm::Value* nullp = llvm::ConstantPointerNull::get(ptrTy());
    llvm::Value* live = builder_.CreateICmpNE(old, nullp, "reassign.live");
    llvm::Value* doDispose = live;
    if (newVal && newVal->getType()->isPointerTy())
        doDispose = builder_.CreateAnd(
            live, builder_.CreateICmpNE(old, newVal, "reassign.diff"));
    auto* dispBB = llvm::BasicBlock::Create(llvm_, "reassign.dispose", currentFn_);
    auto* contBB = llvm::BasicBlock::Create(llvm_, "reassign.cont", currentFn_);
    builder_.CreateCondBr(doDispose, dispBB, contBB);
    builder_.SetInsertPoint(dispBB);
    emitDisposeAndReclaim(old);
    builder_.CreateBr(contBB);
    builder_.SetInsertPoint(contBB);
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
            // #991 / §28.5 / §14.2: `using var x := item` where `item` is an
            // `owned T` PARAMETER (or local) whose type is a generic type
            // PARAMETER — `rt->declaration` is null (T is not a real user
            // type) and the name is single-part.  The non-generic equivalent
            // (`item: Cell`) resolves to a concrete declaration above and is
            // disposed; the generic case fell through every disposal-
            // registration path and leaked (x never disposed, the #572 fix
            // only covered the `move item` initializer).  A generic method /
            // class body is lowered per-instantiation, so the active
            // substitution resolves T to its concrete class — register a
            // static disposal on that class.  Ownership transferred from the
            // source to x (the UnwindIneligibleWalker marks the consumed
            // source ineligible for its own unwind disposal), so disposing x
            // alone discharges the value exactly once (no double-free).
            if (rt->kind == sema::TypeRef::Kind::Named &&
                rt->declaration == nullptr) {
                const sema::TypeRef* concr = resolveTypeParamRef(rt);
                if (concr && concr != rt &&
                    concr->kind == sema::TypeRef::Kind::Named &&
                    concr->declaration &&
                    concr->declaration->kind == ast::TypeDeclKind::Class)
                    return concr->declaration;
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
        // #1017: a DEFERRED boxed-cell free is handled in the SECOND pass below
        // (after every main-loop disposal in this scope), not here — the owner
        // disposal reads the object through the cell and must run first.
        if (it->boxedCellFreeDeferred) continue;
        // #383 / §31.1: the caught exception object. On a normal handled exit
        // it is freed (destructor + base chain + block, via its vtable); on the
        // unwind path it is SKIPPED — a re-raise/throw leaving the arm keeps it
        // in flight for the enclosing handler. It is non-null here (it was the
        // in-flight exception that matched this arm).
        if (it->caughtException) {
            if (isUnwind) continue;
            llvm::Value* obj = builder_.CreateLoad(ptrTy(), it->slot);
            // §30.4 / §30.9 (#854): a task-fault exception re-raised by `await t`
            // of a BOUND task is BORROWED from the task (codegen marked it via
            // bz_mark_exc_borrowed) — the awaiter does NOT own it, so a SECOND
            // `await t` of the same faulted task must still find a LIVE error_obj.
            // Skip the reclaim + unmark for a borrowed object; a normal (owned)
            // caught exception is reclaimed as before (§28.2/#380: idempotent
            // Dispose + base chain + block reclaim). Branch at runtime on the
            // per-object borrowed-set membership.
            llvm::Function* fn = builder_.GetInsertBlock()->getParent();
            auto* isBorrowedFn = declareC("bz_is_exc_borrowed",
                                          llvm::Type::getInt8Ty(llvm_), {ptrTy()});
            llvm::Value* bv =
                builder_.CreateCall(isBorrowedFn, {obj}, "exc.borrowed");
            llvm::Value* isB = builder_.CreateICmpNE(
                bv, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0));
            auto* borrowBB = llvm::BasicBlock::Create(llvm_, "exc.borrow", fn);
            auto* ownedBB  = llvm::BasicBlock::Create(llvm_, "exc.owned", fn);
            auto* contBB   = llvm::BasicBlock::Create(llvm_, "exc.dispcont", fn);
            builder_.CreateCondBr(isB, borrowBB, ownedBB);
            // Borrowed: unmark, do NOT reclaim (the task local's scope-end / the
            // owning awaiter frees error_obj exactly once).
            builder_.SetInsertPoint(borrowBB);
            auto* unmarkFn = declareC("bz_unmark_exc_borrowed",
                                      llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(unmarkFn, {obj});
            builder_.CreateBr(contBB);
            // Owned: normal reclaim.
            builder_.SetInsertPoint(ownedBB);
            emitDisposeAndReclaim(obj);
            builder_.CreateBr(contBB);
            builder_.SetInsertPoint(contBB);
            continue;
        }
        // §15.3 / §15.1.1 (#747): a move-only owning inline-array local. Dispose
        // each non-null slot of the [N x ptr] alloca via the slot object's own
        // vtable (dynamic dispatch), then null the slot. Runs on BOTH normal and
        // unwind exit — the array as a whole carries no class Dispose (it is a
        // value type, not IDisposable), so unlike a plain owned class local
        // (unwindOnly below) it auto-disposes on normal exit too (§15.3 line 2021:
        // "a move-only local's owned slots are auto-disposed at scope end").
        // Only registered for locals proven never transferred out, so this is the
        // single disposal of each slot's object (no double-free).
        if (it->inlineOwnedArray) {
            auto* arrTy = llvm::ArrayType::get(ptrTy(), it->inlineSlotCount);
            for (unsigned i = 0; i < it->inlineSlotCount; ++i) {
                llvm::Value* slotAddr = builder_.CreateInBoundsGEP(
                    arrTy, it->slot,
                    {builder_.getInt32(0), builder_.getInt32(i)}, "ioa.slot");
                llvm::Value* obj = builder_.CreateLoad(ptrTy(), slotAddr, "ioa.val");
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    obj, llvm::ConstantPointerNull::get(ptrTy()));
                auto* dispBB = llvm::BasicBlock::Create(llvm_, "ioa.dispose",
                                                        currentFn_);
                auto* contBB = llvm::BasicBlock::Create(llvm_, "ioa.cont",
                                                        currentFn_);
                builder_.CreateCondBr(isNull, contBB, dispBB);
                builder_.SetInsertPoint(dispBB);
                emitDisposeAndReclaim(obj);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), slotAddr);
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
            }
            continue;
        }
        // §15.3 / §27.5 (#782): a CAPTURED move-only owning inline-array — its
        // [N x ptr] value lives in the boxed cell (boxLocalSlot copied it there).
        // `slot` is the cell-HOLDER ptr-alloca; load the cell pointer, then dispose
        // each non-null element by GEPing the [N x ptr] over the CELL (heap), and
        // null the slot. The cell itself is freed separately by the boxedCellFree
        // that fires AFTER this. Non-escaping-gated at registration so the cell is
        // alive here and no escaping closure still references the elements.
        if (it->inlineOwnedArrayCell) {
            llvm::Value* cell =
                builder_.CreateLoad(ptrTy(), it->slot, "ioac.cell");
            auto* cellNull = builder_.CreateICmpEQ(
                cell, llvm::ConstantPointerNull::get(ptrTy()));
            auto* liveBB = llvm::BasicBlock::Create(llvm_, "ioac.live", currentFn_);
            auto* afterBB = llvm::BasicBlock::Create(llvm_, "ioac.after", currentFn_);
            builder_.CreateCondBr(cellNull, afterBB, liveBB);
            builder_.SetInsertPoint(liveBB);
            auto* arrTy = llvm::ArrayType::get(ptrTy(), it->inlineSlotCount);
            for (unsigned i = 0; i < it->inlineSlotCount; ++i) {
                llvm::Value* slotAddr = builder_.CreateInBoundsGEP(
                    arrTy, cell,
                    {builder_.getInt32(0), builder_.getInt32(i)}, "ioac.slot");
                llvm::Value* obj =
                    builder_.CreateLoad(ptrTy(), slotAddr, "ioac.val");
                llvm::Value* isNull = builder_.CreateICmpEQ(
                    obj, llvm::ConstantPointerNull::get(ptrTy()));
                auto* dispBB = llvm::BasicBlock::Create(llvm_, "ioac.dispose",
                                                        currentFn_);
                auto* contBB = llvm::BasicBlock::Create(llvm_, "ioac.cont",
                                                        currentFn_);
                builder_.CreateCondBr(isNull, contBB, dispBB);
                builder_.SetInsertPoint(dispBB);
                emitDisposeAndReclaim(obj);
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), slotAddr);
                builder_.CreateBr(contBB);
                builder_.SetInsertPoint(contBB);
            }
            builder_.CreateBr(afterBB);
            builder_.SetInsertPoint(afterBB);
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
            // §30.4 / §30.9 (#854): a BOUND faulted task's error_obj is BORROWED by
            // any awaiter (the catch arm skips its reclaim, #854 sites 2-3) — or is
            // unobserved (never awaited) — so the task LOCAL's scope-end is the
            // single Bezel-level free of error_obj. bz_task_free / fire-and-forget
            // frees the task struct + its buffers but NOT the Bezel exception object
            // (only Bezel codegen can run its destructor). Dispose it here when the
            // task has faulted; unmark first so the borrowed-set never holds a freed
            // pointer. (A still-running async task is not yet errored here → skipped;
            // its unobserved-fault error_obj is a separate residual.) A non-faulted
            // task has a null error_obj → skipped. bz_task_free below ignores
            // error_obj, so the now-dangling task->error_obj is never re-read.
            {
                auto* isErrFn = declareC("bz_task_is_error", i1(), {ptrTy()});
                llvm::Value* err = builder_.CreateCall(isErrFn, {t}, "task.err");
                auto* edBB = llvm::BasicBlock::Create(llvm_, "task.errdisp",
                                                      currentFn_);
                auto* eaBB = llvm::BasicBlock::Create(llvm_, "task.errcont",
                                                      currentFn_);
                builder_.CreateCondBr(err, edBB, eaBB);
                builder_.SetInsertPoint(edBB);
                auto* eoFn = declareC("bz_task_error_obj", ptrTy(), {ptrTy()});
                llvm::Value* eo = builder_.CreateCall(eoFn, {t}, "task.errobj");
                llvm::Value* eoNull = builder_.CreateICmpEQ(
                    eo, llvm::ConstantPointerNull::get(ptrTy()));
                auto* doBB = llvm::BasicBlock::Create(llvm_, "task.eodisp",
                                                      currentFn_);
                builder_.CreateCondBr(eoNull, eaBB, doBB);
                builder_.SetInsertPoint(doBB);
                auto* unmarkFn = declareC("bz_unmark_exc_borrowed",
                                          llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(unmarkFn, {eo});
                emitDisposeAndReclaim(eo);
                builder_.CreateBr(eaBB);
                builder_.SetInsertPoint(eaBB);
            }
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
        // stdlib §10.1/§10.2/§10.3 / §32.6 / fix #977 / #985: a `using var`
        // CString, WString, or RawBytes. All are produced by static From /
        // FromBytes factories. CString/WString are bare malloc'd buffers
        // (bz_string_to_utf8z / bz_string_to_utf16z) released by a single
        // null-guarded free(). RawBytes (fix #985) is a TWO-LEVEL allocation —
        // a { ptr, len } struct whose `ptr` owns a separate byte buffer — so it
        // is released via bz_rawbytes_free, which frees the buffer THEN the
        // struct, exactly once. The disposal contract (stdlib §10.x destructor)
        // says "releases the underlying buffer when this instance allocated it".
        if (it->ffiBuffer) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(it->slot, ety);
            llvm::Value* fbuf = builder_.CreateLoad(ptrTy(), addr, "ffibuf.load");
            llvm::Value* isNull = builder_.CreateICmpEQ(
                fbuf, llvm::ConstantPointerNull::get(ptrTy()));
            auto* frBB   = llvm::BasicBlock::Create(llvm_, "ffibuf.free",  currentFn_);
            auto* contBB = llvm::BasicBlock::Create(llvm_, "ffibuf.cont",  currentFn_);
            builder_.CreateCondBr(isNull, contBB, frBB);
            builder_.SetInsertPoint(frBB);
            if (it->rawBytes) {
                // fix #985: free the owned buffer AND the { ptr, len } struct.
                auto* fn = declareC("bz_rawbytes_free",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
                builder_.CreateCall(fn, {fbuf});
            } else {
                builder_.CreateCall(freeFn(), {fbuf});
            }
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
    // #1017 SECOND PASS: free any boxed capture cells whose plain boxedCellFree
    // was suppressed (boxedCellFreeDeferred). This runs AFTER every main-loop
    // disposal in this scope — including the owner's reads-through-cell disposal
    // (using var / collection / dict / subscription / ... — it loads the object
    // THROUGH the cell) and every capturing closure's release — so the cell has
    // no live reader or env reference left. Freeing here (instead of the
    // suppressed boxedCellFree, which LIFO order would have fired BEFORE the owner
    // disposal → UAF) closes the 8-byte capture-cell leak. free(null) is a no-op
    // (guarded boxing may leave the cell-holder null). Exactly-once: a deferred
    // entry is registered ONLY where boxedCellFree was suppressed (mutually
    // exclusive with boxedCellFree) and deduped per slot, so the cell is freed
    // exactly once across both the normal and the unwind exit (a given exit goes
    // one way only). Fires on both paths, symmetric with boxedCellFree.
    for (const auto& d : scope) {
        if (!d.boxedCellFreeDeferred) continue;
        llvm::Value* cell =
            builder_.CreateLoad(ptrTy(), d.slot, "cell.deferred.ptr");
        builder_.CreateCall(freeFn(), {cell});
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
            // #730 / §23.4: include indexers — same member order as Pass 0, so
            // record-box itable slot offsets stay consistent with the class
            // path's ifacePropertyGetIndex_ numbering.
            std::vector<sema::PropertySymbol*> ifaceProps;
            for (sema::Symbol* m : iface->members) {
                auto* ps = dynamic_cast<sema::PropertySymbol*>(m);
                if (ps) ifaceProps.push_back(ps);
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
                sema::MethodSymbol* impl = findInterfaceImpl(ts, im);
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
                            // §10 / §10.3 / #435 (#740): enum `this` is its
                            // underlying integer by value (default i32) — load it
                            // from the box's value slot at that width.
                            llvm::Value* enumVal = tb.CreateLoad(
                                enumLlvmType(ts), valSlot, "box.enumval");
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
                // #730 / §23.4: a record's interface-indexer slot holds a box
                // thunk over the record's concrete indexer accessor (the thunk
                // GEPs past the box header to the record bytes, then forwards
                // the index params / value).
                if (ip->isIndexer) {
                    const sema::PropertySymbol* iimpl =
                        classIndexerImplementing(ts, ip);
                    llvm::Constant* getFp =
                        llvm::ConstantPointerNull::get(ptrTy());
                    if (iimpl)
                        if (llvm::Function* acc = ensureIndexerGetterFn(ts, iimpl))
                            getFp = makeBoxThunk(acc,
                                "bz$boxidxget$" + std::string(ts->name) + "$" +
                                std::string(iface->name));
                    itabElems.push_back(getFp);
                    if (propertyHasSetter(ip)) {
                        llvm::Constant* setFp =
                            llvm::ConstantPointerNull::get(ptrTy());
                        if (iimpl)
                            if (llvm::Function* acc =
                                    ensureIndexerSetterFn(ts, iimpl))
                                setFp = makeBoxThunk(acc,
                                    "bz$boxidxset$" + std::string(ts->name) +
                                    "$" + std::string(iface->name));
                        itabElems.push_back(setFp);
                    }
                    continue;
                }
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
                } else if (sema::FieldSymbol* pfs =
                               classPositionalFieldProperty(ts, ip->name)) {
                    // #815 / §9.1: a POSITIONAL record field backs the interface
                    // property (it generates a public read-only property but is a
                    // FieldSymbol, not a PropertySymbol). Emit a struct-GEP+load
                    // getter and box-thunk it so interface dispatch reaches the
                    // field value instead of a null itable slot (which SIGSEGV'd).
                    if (llvm::Function* acc =
                            ensurePositionalFieldGetterFn(ts, pfs)) {
                        getFp = makeBoxThunk(
                            acc, "bz$boxposfieldget$" + std::string(ts->name) +
                                     "$" + std::string(ip->name));
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

        // Vtable (§8.6/§8.5 #417 + §9.6 #946): [0]=td, [1]=itmap$box$C,
        // [2]=Dispose (no-op), [3]=ToString (synthesized record/enum form, or
        // user override), [4]=Equals, [5]=GetHashCode (box defaults — see below).
        std::vector<llvm::Constant*> elems(kVirtualMethodsBase,
                                           llvm::ConstantPointerNull::get(
                                               ptrTy()));
        elems[0] = descArray_[ts];
        elems[1] = llvm::ConstantExpr::getPointerCast(boxItmapGV, ptrTy());
        if (builtinDisposeFn_) elems[2] = builtinDisposeFn_;
        elems[kToStringSlot] = boxToStringThunk(ts);
        // §9.1 / §9.6 (#961): structural Equals / GetHashCode for boxed
        // records and enums so an Object-typed receiver gets value-semantics
        // equality (§9.1: field-by-field for records; underlying-integer for
        // enums) rather than reference identity (§8.3 default).
        elems[kEqualsSlot]      = boxRecordEqualsThunk(ts);
        elems[kGetHashCodeSlot] = boxRecordHashThunk(ts);
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
        // §9.1 / §9.6 (#961): primitive box vtable with structural Equals /
        // GetHashCode so an Object-typed receiver gets value-semantics equality
        // and hash rather than reference identity. String has no box payload
        // (it stays a bare bz_string* in an Object slot) so it keeps the
        // default reference/identity thunks (the box vtable exists only for
        // the descriptor but the vtable is never dispatched through for a
        // string operand — lowerObjectEquals/GetHashCode's bz_obj_is_string
        // branch intercepts it first).
        llvm::Constant* eqThunk  = (p == P::String)
            ? boxDefaultEqualsThunk()
            : primBoxEqualsThunk(p, nm);
        llvm::Constant* hashThunk = (p == P::String)
            ? boxDefaultHashCodeThunk()
            : primBoxHashThunk(p, nm);
        auto* vtTy = llvm::ArrayType::get(ptrTy(), kVirtualMethodsBase);
        primBoxVtable_[nm] = new llvm::GlobalVariable(
            *module_, vtTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(
                vtTy, {desc,
                       llvm::ConstantPointerNull::get(ptrTy()),  // itmap
                       builtinDisposeFn_
                           ? llvm::cast<llvm::Constant>(builtinDisposeFn_)
                           : llvm::ConstantPointerNull::get(ptrTy()),  // Dispose
                       tostr,
                       eqThunk,    // [4]=Equals
                       hashThunk}),  // [5]=GetHashCode
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


// §9.6 (#946/#947/#948): the uniform Equals thunk for a CLASS. Returns the user
// `Equals(other: Object?)` override if one exists anywhere on the class chain
// (§8.4 "via virtual dispatch"); otherwise the §8.3 reference-identity default
// (`this == other`). The thunk's signature is the vtable-uniform
// `i1(ptr this, ptr other)`, so an Object-typed receiver reaches the override.
llvm::Constant* Emitter::classEqualsThunk(const sema::TypeSymbol* ts) {
    auto it = classEqualsFns_.find(ts);
    if (it != classEqualsFns_.end()) return it->second;

    // Walk the class chain for a user `Equals` (single parameter, non-static).
    // The most-derived override wins (same lookup as classEqualsOverride).
    sema::MethodSymbol* userEq = nullptr;
    for (const sema::TypeSymbol* cur = ts; cur && !userEq;) {
        for (sema::Symbol* sym : cur->members) {
            auto* ms = dynamic_cast<sema::MethodSymbol*>(sym);
            if (!(ms && !ms->isOperator && !isStaticMethod(ms) &&
                  ms->name == "Equals" && ms->params.size() == 1))
                continue;
            // #1115/#1151: only an `Equals` whose parameter is a REFERENCE —
            // its compiled param[1] lowers to a pointer (e.g. `Equals(Object?)`)
            // — is the §8.4 Object.Equals override bound into the uniform
            // `i1(ptr, ptr)` slot-4 thunk. A VALUE-typed `Equals(other: int)`
            // (the IEquatable<int> contract impl) has an i32 param[1]; binding
            // it here would pass the thunk's ptr argument to an i32 parameter →
            // malformed IR ("Call parameter type does not match"). Such an
            // Equals fills only its IEquatable itable slot; the class keeps the
            // §8.3 reference-identity default for the Object.Equals thunk. (Only
            // skip when we can prove the param is non-pointer; an uncompiled
            // method conservatively keeps the prior accept behavior.)
            auto fit = methodFns_.find(ms);
            if (fit != methodFns_.end() &&
                fit->second->getFunctionType()->getNumParams() >= 2 &&
                !fit->second->getFunctionType()->getParamType(1)->isPointerTy())
                continue;
            userEq = ms; break;
        }
        const sema::TypeSymbol* nxt = nullptr;
        for (const sema::TypeSymbol* b : cur->baseTypes)
            if (b && b->kind == ast::TypeDeclKind::Class) { nxt = b; break; }
        cur = nxt;
    }

    auto* fnTy = llvm::FunctionType::get(i1(), {ptrTy(), ptrTy()},
                                         /*isVarArg=*/false);
    auto* thunk = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage,
        "bz$eq$" + std::string(ts->name), module_.get());
    classEqualsFns_[ts] = thunk;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", thunk);
    {
        ThunkScope scope(currentFn_, builder_, thunk, entry);
        llvm::Value* result = nullptr;
        if (userEq) {
            auto fit = methodFns_.find(userEq);
            if (fit != methodFns_.end()) {
                // Call the real user Equals non-virtually (the thunk IS the
                // dispatch target; calling it directly avoids re-entry).
                llvm::Value* r = builder_.CreateCall(
                    fit->second->getFunctionType(), fit->second,
                    {thunk->getArg(0), thunk->getArg(1)});
                // Coerce to i1 (a bool result may be a wider integer).
                if (r->getType()->isIntegerTy() && !r->getType()->isIntegerTy(1))
                    r = builder_.CreateICmpNE(
                        r, llvm::ConstantInt::get(r->getType(), 0));
                if (r->getType()->isIntegerTy(1)) result = r;
            }
        }
        if (!result)
            // §8.3 default: reference identity.
            result = builder_.CreateICmpEQ(thunk->getArg(0), thunk->getArg(1));
        builder_.CreateRet(result);
    }
    return thunk;
}


// §9.6 (#946/#947/#948): the uniform GetHashCode thunk for a CLASS. Returns the
// user `GetHashCode(): int` override if one exists on the class chain; otherwise
// the §8.3 identity-hash default (the receiver pointer bits truncated to i32).
// Signature: the vtable-uniform `i32(ptr this)`.
llvm::Constant* Emitter::classHashCodeThunk(const sema::TypeSymbol* ts) {
    auto it = classHashCodeFns_.find(ts);
    if (it != classHashCodeFns_.end()) return it->second;

    sema::MethodSymbol* userHash = nullptr;
    for (const sema::TypeSymbol* cur = ts; cur && !userHash;) {
        for (sema::Symbol* sym : cur->members) {
            auto* ms = dynamic_cast<sema::MethodSymbol*>(sym);
            if (ms && !ms->isOperator && !isStaticMethod(ms) &&
                ms->name == "GetHashCode" && ms->params.empty()) {
                userHash = ms; break;
            }
        }
        const sema::TypeSymbol* nxt = nullptr;
        for (const sema::TypeSymbol* b : cur->baseTypes)
            if (b && b->kind == ast::TypeDeclKind::Class) { nxt = b; break; }
        cur = nxt;
    }

    auto* fnTy = llvm::FunctionType::get(i32(), {ptrTy()}, /*isVarArg=*/false);
    auto* thunk = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage,
        "bz$hash$" + std::string(ts->name), module_.get());
    classHashCodeFns_[ts] = thunk;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", thunk);
    {
        ThunkScope scope(currentFn_, builder_, thunk, entry);
        llvm::Value* result = nullptr;
        if (userHash) {
            auto fit = methodFns_.find(userHash);
            if (fit != methodFns_.end()) {
                llvm::Value* r = builder_.CreateCall(
                    fit->second->getFunctionType(), fit->second,
                    {thunk->getArg(0)});
                if (r->getType()->isIntegerTy())
                    result = builder_.CreateZExtOrTrunc(r, i32());
            }
        }
        if (!result)
            // §8.3 default: identity hash (pointer bits → i32).
            result = builder_.CreateTrunc(
                builder_.CreatePtrToInt(thunk->getArg(0), i64()), i32());
        builder_.CreateRet(result);
    }
    return thunk;
}


// §9.6 (#946/#947/#948): the shared default Equals thunk `i1(ptr this, ptr other)`
// installed in BOX vtables. A boxed value reached through an Object receiver uses
// reference identity (the §8.3 default) — keeping the uniform slot read in bounds
// and never crashing. Proper structural/value equality for a boxed value type
// through Object is a separate follow-up (see the filed issue).
llvm::Constant* Emitter::boxDefaultEqualsThunk() {
    if (boxDefaultEqualsFn_) return boxDefaultEqualsFn_;
    auto* fnTy = llvm::FunctionType::get(i1(), {ptrTy(), ptrTy()},
                                         /*isVarArg=*/false);
    auto* thunk = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage, "bz$box$eq", module_.get());
    boxDefaultEqualsFn_ = thunk;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", thunk);
    {
        ThunkScope scope(currentFn_, builder_, thunk, entry);
        builder_.CreateRet(
            builder_.CreateICmpEQ(thunk->getArg(0), thunk->getArg(1)));
    }
    return thunk;
}


// §9.6 (#946/#947/#948): the shared default GetHashCode thunk `i32(ptr this)`
// installed in BOX vtables — identity hash (the box pointer bits → i32).
llvm::Constant* Emitter::boxDefaultHashCodeThunk() {
    if (boxDefaultHashCodeFn_) return boxDefaultHashCodeFn_;
    auto* fnTy = llvm::FunctionType::get(i32(), {ptrTy()}, /*isVarArg=*/false);
    auto* thunk = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage, "bz$box$hash", module_.get());
    boxDefaultHashCodeFn_ = thunk;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", thunk);
    {
        ThunkScope scope(currentFn_, builder_, thunk, entry);
        builder_.CreateRet(builder_.CreateTrunc(
            builder_.CreatePtrToInt(thunk->getArg(0), i64()), i32()));
    }
    return thunk;
}


// §9.1 / §9.6 (#961): per-type structural Equals thunk `i1(ptr this, ptr other)`
// for a boxed RECORD or boxed ENUM. `this` is the box pointer (the receiver);
// `other` is the Object? argument. The thunk:
//   1. Returns false if `other` is null.
//   2. Loads `other`'s vtable[0] (its type descriptor) and compares it to the
//      known descriptor for `ts`. A mismatch returns false (different type or
//      a non-box class object in the other slot).
//   3. Loads both payloads (two words past the box header, BZ_BOX layout) and
//      runs emitStructEquality (record) or an integer ICmpEQ (enum), mirroring
//      the statically-typed §8.3 value-equality path.
llvm::Constant* Emitter::boxRecordEqualsThunk(const sema::TypeSymbol* ts) {
    auto it = boxRecordEqualsFns_.find(ts);
    if (it != boxRecordEqualsFns_.end()) return it->second;

    auto* fnTy = llvm::FunctionType::get(i1(), {ptrTy(), ptrTy()},
                                         /*isVarArg=*/false);
    auto* thunk = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage,
        "bz$boxeq$" + std::string(ts->name), module_.get());
    boxRecordEqualsFns_[ts] = thunk;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", thunk);
    {
        ThunkScope scope(currentFn_, builder_, thunk, entry);
        llvm::Value* self  = thunk->getArg(0);
        llvm::Value* other = thunk->getArg(1);
        auto* nullP = llvm::ConstantPointerNull::get(ptrTy());

        // Block 1: check other != null.
        auto* notNullBB = llvm::BasicBlock::Create(llvm_, "beq.nonnull", thunk);
        auto* checkBB   = llvm::BasicBlock::Create(llvm_, "beq.check",   thunk);
        auto* endBB     = llvm::BasicBlock::Create(llvm_, "beq.end",     thunk);

        builder_.CreateCondBr(
            builder_.CreateICmpEQ(other, nullP), endBB, notNullBB);

        // other is non-null: load its vtable[0] (the type descriptor pointer).
        builder_.SetInsertPoint(notNullBB);
        llvm::Value* otherVtbl = builder_.CreateLoad(ptrTy(), other, "beq.ovtbl");
        llvm::Value* otherDesc = builder_.CreateLoad(ptrTy(), otherVtbl, "beq.odesc");
        // Compare with descArray_[ts] — the descriptor for this exact type.
        llvm::Value* myDesc = llvm::ConstantExpr::getPointerCast(
            llvm::cast<llvm::Constant>(descArray_.at(ts)), ptrTy());
        builder_.CreateCondBr(
            builder_.CreateICmpEQ(otherDesc, myDesc), checkBB, endBB);

        // Types match: load both payloads (offset 2 words) and compare.
        builder_.SetInsertPoint(checkBB);
        llvm::Value* selfPayload  = builder_.CreateGEP(
            ptrTy(), self,  {builder_.getInt32(2)}, "beq.spay");
        llvm::Value* otherPayload = builder_.CreateGEP(
            ptrTy(), other, {builder_.getInt32(2)}, "beq.opay");
        llvm::Value* eq;
        if (ts->kind == ast::TypeDeclKind::Record) {
            llvm::StructType* st = recordStruct(ts);
            llvm::Value* sv = builder_.CreateLoad(st, selfPayload,  "beq.sv");
            llvm::Value* ov = builder_.CreateLoad(st, otherPayload, "beq.ov");
            eq = emitStructEquality(sv, ov, st, ts);
        } else {
            // Enum: underlying integer comparison.
            llvm::Type* ety = enumLlvmType(ts);
            llvm::Value* sv = builder_.CreateLoad(ety, selfPayload,  "beq.sv");
            llvm::Value* ov = builder_.CreateLoad(ety, otherPayload, "beq.ov");
            eq = builder_.CreateICmpEQ(sv, ov);
        }
        llvm::BasicBlock* checkEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);

        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i1(), 3, "beq.res");
        phi->addIncoming(builder_.getFalse(), entry);     // other was null
        phi->addIncoming(builder_.getFalse(), notNullBB); // descriptor mismatch
        phi->addIncoming(eq, checkEnd);                   // structural result
        builder_.CreateRet(phi);
    }
    return thunk;
}


// §9.1 / §9.6 (#961): per-type structural GetHashCode thunk `i32(ptr this)` for
// a boxed RECORD or boxed ENUM. Loads the payload (two words past the box
// header) and runs emitRecordHashCode (record) or emitValueHashCode (enum).
llvm::Constant* Emitter::boxRecordHashThunk(const sema::TypeSymbol* ts) {
    auto it = boxRecordHashFns_.find(ts);
    if (it != boxRecordHashFns_.end()) return it->second;

    auto* fnTy = llvm::FunctionType::get(i32(), {ptrTy()}, /*isVarArg=*/false);
    auto* thunk = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage,
        "bz$boxhash$" + std::string(ts->name), module_.get());
    boxRecordHashFns_[ts] = thunk;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", thunk);
    {
        ThunkScope scope(currentFn_, builder_, thunk, entry);
        llvm::Value* self = thunk->getArg(0);
        llvm::Value* payload = builder_.CreateGEP(
            ptrTy(), self, {builder_.getInt32(2)}, "bhash.pay");
        llvm::Value* h;
        if (ts->kind == ast::TypeDeclKind::Record) {
            llvm::StructType* st = recordStruct(ts);
            llvm::Value* rv = builder_.CreateLoad(st, payload, "bhash.rv");
            h = emitRecordHashCode(rv, ts);
        } else {
            // Enum: hash by underlying integer value.
            llvm::Type* ety = enumLlvmType(ts);
            llvm::Value* ev = builder_.CreateLoad(ety, payload, "bhash.ev");
            sema::TypeRef enumRef;
            enumRef.kind      = sema::TypeRef::Kind::Named;
            enumRef.declaration = const_cast<sema::TypeSymbol*>(ts);
            h = emitValueHashCode(ev, &enumRef);
        }
        if (h->getType() != i32())
            h = builder_.CreateZExtOrTrunc(h, i32());
        builder_.CreateRet(h);
    }
    return thunk;
}


// §9.1 / §9.6 (#961): per-primitive structural Equals thunk `i1(ptr this, ptr other)`
// for a boxed PRIMITIVE (int, bool, char, …). Guards `other` is a box of the
// same primitive (same vtable[0] descriptor), then loads both scalar payloads
// and compares by value (ICmpEQ for integers, FCmpOEQ for floats).
llvm::Constant* Emitter::primBoxEqualsThunk(sema::PrimitiveType prim,
                                            const char* nm) {
    auto it = primBoxEqualsFns_.find(nm);
    if (it != primBoxEqualsFns_.end()) return it->second;

    auto* fnTy = llvm::FunctionType::get(i1(), {ptrTy(), ptrTy()},
                                         /*isVarArg=*/false);
    auto* thunk = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage,
        std::string("bz$primeq$") + nm, module_.get());
    primBoxEqualsFns_[nm] = thunk;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", thunk);
    {
        ThunkScope scope(currentFn_, builder_, thunk, entry);
        llvm::Value* self  = thunk->getArg(0);
        llvm::Value* other = thunk->getArg(1);
        auto* nullP = llvm::ConstantPointerNull::get(ptrTy());

        auto* notNullBB = llvm::BasicBlock::Create(llvm_, "peq.nonnull", thunk);
        auto* checkBB   = llvm::BasicBlock::Create(llvm_, "peq.check",   thunk);
        auto* endBB     = llvm::BasicBlock::Create(llvm_, "peq.end",     thunk);

        builder_.CreateCondBr(
            builder_.CreateICmpEQ(other, nullP), endBB, notNullBB);

        // Load other's vtable[0] and compare with this primitive's descriptor.
        builder_.SetInsertPoint(notNullBB);
        llvm::Value* otherVtbl = builder_.CreateLoad(ptrTy(), other, "peq.ovtbl");
        llvm::Value* otherDesc = builder_.CreateLoad(ptrTy(), otherVtbl, "peq.odesc");
        llvm::Value* myDesc = llvm::ConstantExpr::getPointerCast(
            primBoxDesc_.at(nm), ptrTy());
        builder_.CreateCondBr(
            builder_.CreateICmpEQ(otherDesc, myDesc), checkBB, endBB);

        // Same primitive type: load both payloads and compare.
        builder_.SetInsertPoint(checkBB);
        llvm::Type* valTy = lowerPrimitive(prim);
        llvm::Value* selfPayload  = builder_.CreateGEP(
            ptrTy(), self,  {builder_.getInt32(2)}, "peq.spay");
        llvm::Value* otherPayload = builder_.CreateGEP(
            ptrTy(), other, {builder_.getInt32(2)}, "peq.opay");
        llvm::Value* sv = builder_.CreateLoad(valTy, selfPayload,  "peq.sv");
        llvm::Value* ov = builder_.CreateLoad(valTy, otherPayload, "peq.ov");
        llvm::Value* eq;
        if (valTy->isFloatingPointTy())
            eq = builder_.CreateFCmpOEQ(sv, ov);
        else
            eq = builder_.CreateICmpEQ(sv, ov);
        llvm::BasicBlock* checkEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);

        builder_.SetInsertPoint(endBB);
        auto* phi = builder_.CreatePHI(i1(), 3, "peq.res");
        phi->addIncoming(builder_.getFalse(), entry);     // other was null
        phi->addIncoming(builder_.getFalse(), notNullBB); // descriptor mismatch
        phi->addIncoming(eq, checkEnd);                   // value equality
        builder_.CreateRet(phi);
    }
    return thunk;
}


// §9.1 / §9.6 (#961): per-primitive GetHashCode thunk `i32(ptr this)` for a
// boxed PRIMITIVE. Loads the scalar payload and runs emitValueHashCode with
// the primitive's TypeRef so equal values produce equal hashes.
llvm::Constant* Emitter::primBoxHashThunk(sema::PrimitiveType prim,
                                          const char* nm) {
    auto it = primBoxHashFns_.find(nm);
    if (it != primBoxHashFns_.end()) return it->second;

    auto* fnTy = llvm::FunctionType::get(i32(), {ptrTy()}, /*isVarArg=*/false);
    auto* thunk = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage,
        std::string("bz$primhash$") + nm, module_.get());
    primBoxHashFns_[nm] = thunk;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", thunk);
    {
        ThunkScope scope(currentFn_, builder_, thunk, entry);
        llvm::Value* self = thunk->getArg(0);
        llvm::Value* payload = builder_.CreateGEP(
            ptrTy(), self, {builder_.getInt32(2)}, "phash.pay");
        llvm::Type* valTy = lowerPrimitive(prim);
        llvm::Value* val = builder_.CreateLoad(valTy, payload, "phash.val");
        sema::TypeRef primRef;
        primRef.kind      = sema::TypeRef::Kind::Primitive;
        primRef.primitive = prim;
        llvm::Value* h = emitValueHashCode(val, &primRef);
        if (h->getType() != i32())
            h = builder_.CreateZExtOrTrunc(h, i32());
        builder_.CreateRet(h);
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
