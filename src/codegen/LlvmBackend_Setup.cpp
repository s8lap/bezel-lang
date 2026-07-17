#include "codegen/LlvmBackend_impl.hpp"

#include "common/OwnershipFacts.hpp"  // #1414 stage 4: shared ownership-facts core
#include "sema/ConstFold.hpp"

// §4.4 fix #521: decimal compile-time literal parser (defined in bezel_runtime.cpp).
extern "C" void bz_decimal_parse_cstr(const char* s, void* out);

namespace bezel::codegen {

// #383 / §28: forward declaration — defined later in this file. A reference
// TypeRef carrying a vtable (a Named class/interface). Used by the §28.3
// unwind-disposal eligibility helpers above its definition.
static bool isClassOrInterfaceRef(const sema::TypeRef* rt);

// #1945 / §8.6: forward declaration — defined later in this file. The box-key
// name for a boxable value-builtin (DateTime/…) by spelling; used by
// descriptorForPatternName above its definition.
static const char* valueBuiltinBoxNameStr(std::string_view n);

// #1309 / §22.13: the constant-literal initializer shapes that constInitForExpr
// can fold into an LLVM Constant for a static field's backing global. Factored
// out of the (previously triplicated) inline dynamic_cast checklist so the
// mutable-vs-readonly bake decision stays consistent across every call site
// (declareStatics, needsRuntimeFieldInit, the sfinit field loop, and
// isSfinitStoredStaticField).
static bool isConstantLiteralStaticInit(const ast::Expr* e) {
    if (!e) return false;
    return dynamic_cast<const ast::IntLiteralExpr*>(e) ||
           dynamic_cast<const ast::BoolLiteralExpr*>(e) ||
           dynamic_cast<const ast::FloatLiteralExpr*>(e) ||
           dynamic_cast<const ast::StringLiteralExpr*>(e) ||
           dynamic_cast<const ast::VerbatimStringExpr*>(e);
}

// #1309 / §22.13: a static field is MUTABLE unless its modifiers include
// `readonly` (a `const` member is a different symbol kind — ConstMemberSymbol —
// and never reaches this path). Only a `readonly` static may have its literal
// initializer baked into the global at compile time: a forward reference from an
// earlier `static readonly` initializer is a compile error (§22.13), so baking
// is sound there. A MUTABLE static must initialize at runtime in textual
// declaration order — otherwise an earlier initializer that reads it would
// silently observe the baked value instead of the not-yet-initialized state the
// partial-state guard exists to protect (§22.13: "Partially-initialized statics
// are never observable").
static bool isReadonlyStaticField(const sema::FieldSymbol* fs) {
    if (!fs) return false;
    for (auto mod : fs->modifiers)
        if (mod == "readonly") return true;
    return false;
}

// #1309 / §22.13: true iff a static field's initializer is baked at compile time
// into its backing global (constInitForExpr) and so needs NO runtime store in the
// textual-order sfinit. Baked iff the initializer is a constant literal AND the
// field is readonly; a mutable static is always runtime-initialized.
static bool isBakedStaticFieldInit(const sema::FieldSymbol* fs,
                                   const ast::Expr* e) {
    return isConstantLiteralStaticInit(e) && isReadonlyStaticField(fs);
}

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
            for (const auto& p : ed->params) {
                // fix #1720 (§32.5, v0.186): a `RawBytes` parameter — of an
                // extern method or FFI function type — occupies TWO consecutive
                // C argument slots: the buffer pointer (const void*) then the
                // length (size_t). Lower it as (ptr, i64) so the extern's
                // C-ABI signature matches the two-argument marshalling emitted
                // at the call site (lowerExternCall). `RawBytes?` is the same
                // two slots (NULL/0 for null). Kept in step with the call site:
                // an inconsistent 1-slot-here/2-slot-there lowering would
                // corrupt the ABI for every following argument.
                if (isRawBytesFfiType(p.type.get())) {
                    paramTys.push_back(ptrTy());
                    paramTys.push_back(i64());
                    continue;
                }
                paramTys.push_back(lowerType(p.type.get()));
            }
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
        // #1309 / §22.13: a MUTABLE static is NOT compile-time-baked — it is
        // initialized at runtime in textual declaration order (its initializer is
        // stored by the synthesized sfinit), so a forward reference to it from an
        // earlier initializer observes the not-yet-stored state and the
        // partial-state guard fires rather than silently reading a baked value.
        // Only a `readonly` static keeps its literal baked in the global (forward
        // refs among readonly are a compile error per §22.13, so baking is sound).
        llvm::Constant* fieldInitC = isReadonlyStaticField(fs)
            ? constInitFor(fd, ty)
            : llvm::Constant::getNullValue(ty);
        auto* gv = new llvm::GlobalVariable(
            *module_, ty, /*isConstant=*/false,
            llvm::GlobalValue::ExternalLinkage,
            fieldInitC,
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
                        arg->headName());
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
            // #1309 / §22.13: only a readonly static's literal initializer is
            // baked (constInitFor); a mutable static's literal initializer needs
            // a runtime store in textual order, so it does NOT skip here.
            if (isBakedStaticFieldInit(fs, e)) continue;
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

    // per §11.2 (#1613): a void-returning interface method still reaches
    // this thunk when a param is nullability-widened — CreateCall must not
    // name a void-typed Value (the LLVM verifier rejects "Instruction has a
    // name, but provides a void value!"). Mirrors ensureVtableWideningThunk's
    // isVoidRet guard (#1592, §12.1).
    bool isVoidRet = ifaceTy->getReturnType()->isVoidTy();
    llvm::Value* result = builder_.CreateCall(
        implTy, implFn, implArgs, isVoidRet ? llvm::Twine() : "nw.ret");
    if (isVoidRet)
        builder_.CreateRetVoid();
    else
        builder_.CreateRet(result);

    currentFn_ = savedFn;
    if (savedBB) builder_.SetInsertPoint(savedBB);
    return thunk;
}


// #1592 / §12.1: find the INTRODUCING (non-override) virtual declaration for
// `ms` by walking the base-class chain while the same vtable slot continues
// to be present on the ancestor. buildVtable copies + overwrites the base's
// slot array for every subclass, so a slot is present at index i in every
// class from its introducing declarer downward — walking up stops at the
// first ancestor missing it. Returns `ms` itself when no slot info is
// available (e.g. a non-virtual/static method, which emitMethodCall never
// reaches this from anyway) or `ms` already is the introducing declaration.
const sema::MethodSymbol* Emitter::introducingVirtualMethod(
        const sema::MethodSymbol* ms) {
    if (!ms || !ms->parent) return ms;
    auto slotIt = vtableSlot_.find(ms);
    if (slotIt == vtableSlot_.end() || slotIt->second < kVirtualMethodsBase)
        return ms;
    unsigned idx = slotIt->second - kVirtualMethodsBase;
    const sema::TypeSymbol* introClass = ms->parent;
    for (const sema::TypeSymbol* cur = ms->parent; cur; ) {
        const sema::TypeSymbol* base = baseClassOf(cur);
        if (!base) break;
        auto vmIt = vtableMethods_.find(base);
        if (vmIt == vtableMethods_.end() || idx >= vmIt->second.size() ||
            !vmIt->second[idx].method)
            break;
        introClass = base;
        cur = base;
    }
    auto vmIt = vtableMethods_.find(introClass);
    if (vmIt == vtableMethods_.end() || idx >= vmIt->second.size() ||
        !vmIt->second[idx].method)
        return ms;
    return vmIt->second[idx].method;
}

// #1592: which 0-based parameter positions does ANY contributor anywhere in
// rootMs's whole virtual-slot family widen (base `T` → some override's `T?`,
// a VALUE type)? Scans every class in the program that is rootMs->parent or
// a (transitive) subclass of it — the same whole-program subclass scan
// emitVirtualGenericDispatch already performs (LlvmBackend_Calls2.cpp) — and
// for each finds that class's own installed contributor for this slot (by
// name+arity, via vtableNameSlot_/vtableMethods_), comparing its declared
// parameter types against rootMs's own. Memoized per rootMs: a hierarchy scan
// runs at most once per distinct virtual-slot family.
const std::vector<bool>& Emitter::widenedParamPositions(
        const sema::MethodSymbol* rootMs) {
    static const std::vector<bool> kEmpty;
    if (!rootMs || !rootMs->parent) return kEmpty;
    auto cached = vtableFamilyWidenedParams_.find(rootMs);
    if (cached != vtableFamilyWidenedParams_.end()) return cached->second;
    std::vector<bool>& result = vtableFamilyWidenedParams_[rootMs];
    result.assign(rootMs->params.size(), false);

    auto isSubclassOf = [&](const sema::TypeSymbol* c,
                            const sema::TypeSymbol* anc) {
        for (const sema::TypeSymbol* t = c; t; t = baseClassOf(t))
            if (t == anc) return true;
        return false;
    };
    std::string key = std::string(rootMs->name) + "#" +
                      std::to_string(rootMs->params.size());
    for (const auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (!ts || ts->kind != ast::TypeDeclKind::Class) continue;
        if (!isSubclassOf(ts, rootMs->parent)) continue;
        auto nsIt = vtableNameSlot_.find(ts);
        if (nsIt == vtableNameSlot_.end()) continue;
        auto slotIt = nsIt->second.find(key);
        if (slotIt == nsIt->second.end() ||
            slotIt->second < kVirtualMethodsBase)
            continue;
        unsigned idx = slotIt->second - kVirtualMethodsBase;
        auto vmIt = vtableMethods_.find(ts);
        if (vmIt == vtableMethods_.end() || idx >= vmIt->second.size())
            continue;
        const sema::MethodSymbol* leaf = vmIt->second[idx].method;
        if (!leaf) continue;
        for (std::size_t pi = 0; pi < leaf->params.size() &&
                                  pi < result.size(); ++pi) {
            sema::TypeRef* lpt = paramTypeOf(leaf->params[pi]);
            sema::TypeRef* rpt = paramTypeOf(rootMs->params[pi]);
            if (lpt && isNullableValueTy(lpt) &&
                !(rpt && isNullableValueTy(rpt)))
                result[pi] = true;
        }
    }
    return result;
}

// #1592: rootMs's own declared function type with each widened position's
// parameter type replaced by the tagged nullable-value struct. Identical to
// methodFnType(rootMs) when `widened` is all-false (or empty).
llvm::FunctionType* Emitter::uniformVtableFnType(
        const sema::MethodSymbol* rootMs, const std::vector<bool>& widened) {
    llvm::FunctionType* base = methodFnType(rootMs);
    if (!base) return nullptr;
    std::vector<llvm::Type*> paramTys(base->param_begin(), base->param_end());
    for (std::size_t pi = 0; pi < widened.size(); ++pi) {
        if (!widened[pi]) continue;
        std::size_t ai = pi + 1;  // skip the receiver slot
        if (ai >= paramTys.size()) continue;
        sema::TypeRef* pt = paramTypeOf(rootMs->params[pi]);
        if (!pt) continue;
        sema::TypeRef nn = *pt;
        nn.nullable = true;
        paramTys[ai] = nullableValueStructTy(&nn);
    }
    return llvm::FunctionType::get(base->getReturnType(), paramTys,
                                   /*isVarArg=*/false);
}

// #1592 / §12.1: adapter thunk that presents rootMs's whole family's uniform
// (possibly-widened) ABI for a contributor (`leafMs`) whose OWN compiled body
// is still the narrow, un-widened form — the un-overridden base body, or a
// sibling override that never widened. Mirrors ensureNullableWideningThunk
// above (adapts an ABI mismatch by boxing/unboxing at a thunk boundary) but
// for the vtable side and in the OPPOSITE direction: here the thunk's OWN
// signature is the WIDE uniform ABI, and it UNBOXES each widened argument
// before forwarding to the leaf's real narrow-ABI body.
//
// Unboxing needs no runtime null check because of WHERE this thunk is
// reachable from. This thunk is only ever installed for a contributor whose
// OWN declared parameter is still narrow at the widened position — i.e. a
// class that is NOT at or below the point in the hierarchy where widening
// was introduced. By Liskov substitution, no variable whose STATIC type
// already sees the widened `T?` parameter can ever hold a runtime object
// that dispatches to this contributor (such a static type's own class is
// necessarily at or below the widening point, hence so is any object it can
// reference, and that object's OWN contributor — installed directly, not via
// this thunk — is the one reached instead). So every call that reaches this
// thunk resolved through a narrow (`T`-declaring) static type, which
// `emitMethodCall`'s uniform-ABI dispatch (LlvmBackend_Calls2.cpp) can only
// ever have populated with a definite (non-null) boxed value — the boxed
// struct this thunk unboxes always has hasValue=true.
llvm::Function* Emitter::ensureVtableWideningThunk(
        const sema::MethodSymbol* rootMs, const sema::MethodSymbol* leafMs,
        const std::vector<bool>& widened) {
    if (!rootMs || !leafMs) return nullptr;
    // #1766 / §12.1 / §14: this thunk exists SOLELY to unbox widened
    // nullable-value parameters (#1592) — its body forwards every argument,
    // unboxing only the positions this slot's virtual family actually widened.
    // If NO position is widened it has nothing to adapt, and the only way it
    // could still be reached is an UNRELATED signature difference it cannot
    // legitimately bridge: a generic base method returning the type parameter
    // `T` lowers its vtable-slot return type to the erased `i32` placeholder
    // (methodFnType over the un-instantiated `Base<T>`), while a
    // reference-typed override (`T` = string, a class, ...) returns `ptr`.
    // Emitting a pass-through thunk there produced malformed IR (`ret ptr`
    // from an `i32`-typed function): the AOT verifier rejected it; the JIT,
    // which skips the verifier, silently ran it. Bail so the vtable slot holds
    // the override fn directly (the slot is an opaque `ptr`; the virtual call
    // site substitutes `T` and types the call on its own).
    bool anyWidened = false;
    for (bool w : widened)
        if (w) { anyWidened = true; break; }
    if (!anyWidened) return nullptr;
    auto cacheIt = vtableWideningThunks_.find(leafMs);
    if (cacheIt != vtableWideningThunks_.end()) return cacheIt->second;

    auto fIt = methodFns_.find(const_cast<sema::MethodSymbol*>(leafMs));
    if (fIt == methodFns_.end() || !fIt->second) return nullptr;
    llvm::Function* implFn = fIt->second;
    llvm::FunctionType* implTy = implFn->getFunctionType();
    llvm::FunctionType* uniformTy = uniformVtableFnType(rootMs, widened);
    if (!uniformTy || uniformTy == implTy) return nullptr;  // no thunk needed
    if (uniformTy->getNumParams() != implTy->getNumParams()) return nullptr;

    std::string name = "bz$vw$" +
                       std::string(leafMs->parent ? leafMs->parent->name : "") +
                       "$" + std::string(leafMs->name);
    auto* thunk = llvm::Function::Create(
        uniformTy, llvm::GlobalValue::InternalLinkage, name, module_.get());
    vtableWideningThunks_[leafMs] = thunk;

    // Save/restore builder state (same pattern as ensureNullableWideningThunk
    // / emitEventAccessor — this may run during declareVtables, before method
    // bodies are emitted).
    llvm::Function* savedFn = currentFn_;
    llvm::BasicBlock* savedBB = builder_.GetInsertBlock();
    currentFn_ = thunk;
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", thunk);
    builder_.SetInsertPoint(entry);

    std::vector<llvm::Value*> implArgs;
    unsigned paramIdx = 0;
    for (llvm::Argument& arg : thunk->args()) {
        llvm::Type* uniformArgTy = uniformTy->getParamType(paramIdx);
        llvm::Type* implArgTy = implTy->getParamType(paramIdx);
        if (uniformArgTy == implArgTy || paramIdx == 0) {
            // Receiver (ptr), or a position not widened for this leaf: pass
            // through unchanged.
            implArgs.push_back(&arg);
        } else {
            // Unbox: the uniform (family-wide) ABI passes the tagged
            // {i8 hasValue, T} struct here; leafMs's own narrow body wants
            // the bare T. Always safe — see the function comment above.
            implArgs.push_back(
                builder_.CreateExtractValue(&arg, {1u}, "vw.val"));
        }
        ++paramIdx;
    }
    // A void-returning call must not be given a name (the IR verifier
    // rejects a named void value), so only name it when there's a real
    // return value to forward.
    bool isVoidRet = implTy->getReturnType()->isVoidTy();
    llvm::Value* result = builder_.CreateCall(
        implTy, implFn, implArgs, isVoidRet ? llvm::Twine() : "vw.ret");
    if (isVoidRet)
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
                if (mit != methodFns_.end()) {
                    llvm::Constant* fp = llvm::cast<llvm::Constant>(mit->second);
                    // #1592 / §12.1: if ANY contributor anywhere in this
                    // slot's whole virtual hierarchy widened a value-typed
                    // parameter, the slot's ABI must be uniformly the WIDE
                    // (boxed) form so a narrow-static-typed caller and a
                    // wide-static-typed caller (emitMethodCall's uniform-ABI
                    // dispatch, LlvmBackend_Calls2.cpp) agree on one calling
                    // convention. Install an unboxing adapter thunk when
                    // `ts`'s own contribution (`vs.method`) is still narrow;
                    // returns nullptr (direct fn ptr, unchanged) when there is
                    // no widening anywhere, or `vs.method` already is the
                    // wide-ABI contributor.
                    const sema::MethodSymbol* rootMs =
                        introducingVirtualMethod(vs.method);
                    const std::vector<bool>& widened =
                        widenedParamPositions(rootMs);
                    if (llvm::Function* thunk =
                            ensureVtableWideningThunk(rootMs, vs.method, widened))
                        fp = llvm::cast<llvm::Constant>(thunk);
                    elems[kVirtualMethodsBase + i] = fp;
                }
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


// #1621: sema keeps an INDEPENDENT copy of this table —
// `bezel::common::builtinExceptionParentOf` (src/common/BuiltinRegistry.cpp)
// — consulted by `ExprTC::catchArmNcb`'s ancestor walk (§31.2.1,
// src/sema/ExprTypecheck.cpp) so a multi-type catch-arm binding of two
// SIBLING built-in exceptions (e.g. `OverflowException | ArgumentException`)
// resolves to their shared ancestor. Keep both in sync by hand when editing
// either (they aren't merged — see the cross-reference comment on
// `builtinExceptionParentOf` in BuiltinRegistry.hpp for why).
const std::vector<std::pair<const char*, const char*>>&
Emitter::builtinExceptionHierarchy() {
    static const std::vector<std::pair<const char*, const char*>> h = {
        {"Exception", ""},
        {"ArgumentException", "Exception"},
        {"ArgumentNullException", "ArgumentException"},
        // stdlib §2.5/§4.4 (v0.183, #1396): ArgumentOutOfRangeException — raised
        // by DateTime's component validation and by the negative-magnitude
        // constructor guards (capacity/size/length). Derives from
        // ArgumentException so a broad `catch var e: ArgumentException` still
        // covers it (stdlib doc, declared alongside DateTime in §2.5).
        {"ArgumentOutOfRangeException", "ArgumentException"},
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
        // §6.9 / §30.15 v0.174 (#809): ChannelFullException — raised by the
        // synchronous Send on a FULL bounded channel (the never-blocks contract;
        // a producer that wants to wait uses SendAsync instead).
        {"ChannelFullException", "Exception"},
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
        // The SHARED no-op disposer for every VALUE-TYPE box (record / primitive
        // / span box, §8.6) — a box owns nothing its Dispose must free. Built-in
        // EXCEPTIONS used to share this no-op too, but #1033 (§31.1 / §28.9)
        // gave them their own disposer that releases the owned Message string
        // (see builtinExceptionDisposeFn_ below); boxes still use this no-op
        // because they carry no string at offset 8.
        //
        // §31.4/#811 note: the CauseChain snapshot a `(message, cause)` exception
        // holds is NOT freed (in either disposer). An exception object has
        // several disposal / reclaim paths (catch-arm dispose, scope-end dispose,
        // and the async fault-propagation free in bz$async$*$Resume), and they
        // are not a single idempotent site — a Dispose that read/free'd
        // CauseChain@24 double-freed it (and read freed memory) on the async
        // path. So the CauseChain is left to be reclaimed by process exit, like
        // the runtime-owned TypeInitializationException.InnerException (§22.13).
        // This is a small, bounded leak confined to a caught-and-disposed
        // exception constructed WITH a cause; tracked as a follow-up. (The
        // Message/ParamName strings DO get the slot-null idempotency guard
        // below, so they are safe to release where CauseChain is not.)
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry",
                                            builtinDisposeFn_);
        llvm::IRBuilder<> b(bb);
        b.CreateRetVoid();
    }

    // #1033 / §31.1 / §28.9: the built-in exception's OWN disposer. A built-in
    // exception OWNS its Message string (§31.4 `readonly property Message:
    // string`), and ArgumentException owns its ParamName. Per §28.9 a class
    // string field holds an INDEPENDENT +1 (retained/transferred at the store
    // site in the construction path, LlvmBackend_Exprs2.cpp), released exactly
    // once on disposal — mirroring the class string-field disposal loop
    // (LlvmBackend_Disposal.cpp). Before this, `new Exception($"...")` stored
    // the +1 interpolation result into the slot but nothing ever released it,
    // so every `throw new Exception(<fresh string>)` leaked the message (#1033).
    //
    // The release is null-safe and a no-op on a sentinel/literal
    // (bz_string_release), so a guard-thrown exception (calloc'd, null slots)
    // and a literal-message exception (sentinel) dispose cleanly. After
    // releasing, NULL the slot so a repeated dispatch (explicit .Dispose() +
    // scope-end, the §28.6 split, or the DomainFaulted wrap that disposes the
    // original then re-uses its retained message copy) finds null and skips —
    // the same #396 slot-null precedent the class string-field loop uses.
    builtinExceptionDisposeFn_ = llvm::Function::Create(
        dispTy, llvm::Function::InternalLinkage, "bz$exDisposeMsg",
        module_.get());
    {
        auto* bb = llvm::BasicBlock::Create(
            llvm_, "entry", builtinExceptionDisposeFn_);
        llvm::IRBuilder<> b(bb);
        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
        llvm::Value* excObj = builtinExceptionDisposeFn_->arg_begin();
        auto* relFn = declareC("bz_string_release",
                               llvm::Type::getVoidTy(llvm_), {ptrTy()});
        // Release Message@kExcMsgOffset, then null the slot.
        llvm::Value* msgSlot = b.CreateGEP(
            i8Ty, excObj, b.getInt64(kExcMsgOffset), "ex.msg.slot");
        llvm::Value* msg = b.CreateLoad(ptrTy(), msgSlot, "ex.msg");
        b.CreateCall(relFn, {msg});
        b.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), msgSlot);
        // Release ParamName@kExcParamOffset (ArgumentException; null for every
        // other built-in → no-op), then null the slot.
        llvm::Value* pnSlot = b.CreateGEP(
            i8Ty, excObj, b.getInt64(kExcParamOffset), "ex.param.slot");
        llvm::Value* pn = b.CreateLoad(ptrTy(), pnSlot, "ex.param");
        b.CreateCall(relFn, {pn});
        b.CreateStore(llvm::ConstantPointerNull::get(ptrTy()), pnSlot);
        // CauseChain@kExcCauseOffset is intentionally NOT touched — see the
        // #811 note in the no-op disposer's comment above.
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
            builtinExceptionDisposeFn_,
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
    // §19.3 / §8.6 (#1945): a value-builtin type-test target (DateTime/TimeSpan/
    // DateOnly/TimeOnly/Guid) resolves to its box descriptor (primBoxDesc_), so a
    // concrete `case o of DateTime` / `o is DateTime` runs a real is-a against a
    // boxed value-builtin. Value-builtin names are reserved, so no user type name
    // collides; checked after the user-symbol / builtinDesc_ lookups above.
    if (const char* vb = valueBuiltinBoxNameStr(last)) {
        auto vit = primBoxDesc_.find(vb);
        if (vit != primBoxDesc_.end()) return vit->second;
    }
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


// §19.3 (v0.182, #1852): lower a type-test pattern whose type is a bare type
// parameter T, resolved to the concrete substituted type `ct` at THIS
// monomorphized instantiation. `case x of T` and `var x: T` share one §19.3
// rule — "test against the substituted type argument, per instantiation" — so
// both route here after resolving T. The emission mirrors the concrete-type
// handlers in patternMatches: a class/interface → runtime is-a against the
// PER-MONOMORPHIZATION descriptor (#1843 perInstDescriptorForTarget, so
// `T=Wrap<int>` does not match a `Wrap<string>` scrutinee — the shared erased
// descriptor would over-match); a built-in exception → the btd$ descriptor is-a
// (like #1854); a value-builtin (DateTime/Guid/… — no reference descriptor) → a
// well-formed never-match; a record/enum → static decidability; a primitive
// incl. string → the boxed / same-primitive test. Returns null ONLY for a kind
// it cannot lower; the caller then emits a TERMINATED never-match (never a
// return-nullptr, which would leave `case.end` unterminated → malformed IR).
llvm::Value* Emitter::emitTypeParamPatternTest(const sema::TypeRef* ct,
                                               llvm::Value* scrut,
                                               const sema::TypeRef* scrutTy) {
    if (!ct) return nullptr;

    // Is `scrut` a heap object that carries a vtable/descriptor emitIsA can
    // walk? A runtime is-a only makes sense on a class/interface/Object/built-in-
    // exception scrutinee; against a value primitive or an unboxed value type the
    // test can never match (return false, do not fault emitIsA on a non-object).
    auto scrutObjectLike = [&]() -> bool {
        return !scrutTy ||
            (scrutTy->kind == sema::TypeRef::Kind::Named && scrutTy->declaration &&
             (scrutTy->declaration->kind == ast::TypeDeclKind::Class ||
              scrutTy->declaration->kind == ast::TypeDeclKind::Interface)) ||
            (scrutTy->kind == sema::TypeRef::Kind::Primitive &&
             scrutTy->primitive == sema::PrimitiveType::Object) ||
            isBuiltinExceptionTypeRef(scrutTy);
    };
    // Could the scrutinee's runtime value be a bare bz_string* (a vtable-less
    // string in an Object or interface)? A string has no leading descriptor word,
    // so emitIsA would fault walking it — guard on it. Mirrors lowerTypeTest's
    // `mightHoldString` (Object or interface static type).
    auto scrutMayBeString = [&]() -> bool {
        if (!scrutTy) return true;
        if (scrutTy->kind == sema::TypeRef::Kind::Primitive &&
            scrutTy->primitive == sema::PrimitiveType::Object)
            return true;
        if (scrutTy->kind == sema::TypeRef::Kind::Named && scrutTy->declaration &&
            scrutTy->declaration->kind == ast::TypeDeclKind::Interface)
            return true;
        return false;
    };
    // Emit the runtime is-a against `desc`, short-circuiting a runtime string to
    // false when the scrutinee could hold one (emitIsA would fault walking a
    // vtable-less bz_string*). Mirrors lowerTypeTest's / patternMatches' guard.
    auto guardedIsA = [&](llvm::GlobalVariable* desc) -> llvm::Value* {
        if (!scrutMayBeString()) return emitIsA(scrut, desc);
        llvm::BasicBlock* preBB = builder_.GetInsertBlock();
        auto* isStrFn = declareC("bz_obj_is_string", i32(), {ptrTy(), ptrTy()});
        llvm::Value* isStr = builder_.CreateICmpNE(
            builder_.CreateCall(isStrFn, {scrut, vtableTablePtr()}),
            builder_.getInt32(0));
        auto* doIsaBB = llvm::BasicBlock::Create(llvm_, "tp.isa", currentFn_);
        auto* joinBB = llvm::BasicBlock::Create(llvm_, "tp.isa.join", currentFn_);
        builder_.CreateCondBr(isStr, joinBB, doIsaBB);
        builder_.SetInsertPoint(doIsaBB);
        llvm::Value* isa = emitIsA(scrut, desc);
        llvm::BasicBlock* isaEnd = builder_.GetInsertBlock();
        builder_.CreateBr(joinBB);
        builder_.SetInsertPoint(joinBB);
        auto* phi = builder_.CreatePHI(i1(), 2);
        phi->addIncoming(builder_.getInt1(false), preBB);
        phi->addIncoming(isa, isaEnd);
        return phi;
    };

    // A VALUE-BUILTIN substitution (DateTime/TimeSpan/Guid/… — isValueBuiltin, or
    // declaration=null with a value-builtin name). Its declaration kind is Class
    // but it carries NO reference descriptor (descriptorForPatternName /
    // boxDescriptorFor both miss it), exactly as the CONCRETE `case o of DateTime`
    // form does; §19.3 makes the type-param form behave identically to that
    // concrete form. Emit a well-formed never-match (the honest, IR-valid result:
    // a value-builtin boxed into Object is not yet a walkable box either — a
    // separate lowering gap, filed as #1945). Checked BEFORE the class/interface
    // branch so it does not enter emitIsA with a descArray_ miss — the #1852
    // blocker-1 malformed-IR crash. (If a box descriptor ever exists, is-a it.)
    bool isValueBuiltin =
        (ct->declaration && ct->declaration->isValueBuiltin) ||
        (ct->kind == sema::TypeRef::Kind::Named && !ct->declaration &&
         sema::isValueBuiltinByName(ct->headName()));
    if (isValueBuiltin) {
        if (scrut->getType()->isPointerTy() && scrutObjectLike())
            if (llvm::GlobalVariable* desc = boxDescriptorFor(ct))
                return guardedIsA(desc);
        return builder_.getInt1(false);
    }

    // A class or interface substitution: runtime is-a. #1843: for a CONSTRUCTED
    // generic target (`T=Box<int>`) resolve the per-monomorphization descriptor
    // so it does not match a sibling instantiation (`Box<string>`); a non-generic
    // or erased target keeps its shared descArray_ descriptor.
    if (ct->kind == sema::TypeRef::Kind::Named && ct->declaration &&
        (ct->declaration->kind == ast::TypeDeclKind::Class ||
         ct->declaration->kind == ast::TypeDeclKind::Interface)) {
        if (!scrut->getType()->isPointerTy() || !scrutObjectLike())
            return builder_.getInt1(false);
        llvm::GlobalVariable* desc = perInstDescriptorForTarget(ct);
        if (!desc) {
            auto it = descArray_.find(ct->declaration);
            if (it == descArray_.end()) return nullptr;
            desc = it->second;
        }
        return guardedIsA(desc);
    }

    // A built-in exception substitution (`Box<IOException>`): no user TypeSymbol
    // (ct->declaration null) but a heap object whose vtable[0] is the btd$<name>
    // descriptor emitIsA walks (#1854). §19.3: "the same rule as catch arms".
    // The is-a is correct for the descriptor handed to it; note that two distinct
    // built-in-exception type ARGUMENTS to one generic currently collapse to a
    // single monomorphization (#1944), so per-instantiation distinctness for
    // built-in-exception args is limited at the monomorphization layer, not here.
    if (isBuiltinExceptionTypeRef(ct) && ct->astNode && ct->astNode->name &&
        !ct->astNode->name->parts.empty()) {
        auto bit =
            builtinDesc_.find(std::string(ct->astNode->name->parts.back()));
        if (bit != builtinDesc_.end()) {
            if (!scrut->getType()->isPointerTy() || !scrutObjectLike())
                return builder_.getInt1(false);
            return guardedIsA(bit->second);
        }
    }

    // A record or enum substitution: a sealed value type with no subtyping, so
    // the test is statically decidable (mirrors the NamedPattern record/enum
    // branch). A boxed record/enum inside an Object (a pointer scrutinee) is not
    // matched by a bare value-type name here — the concrete form returns false
    // for a pointer scrutinee too.
    if (ct->kind == sema::TypeRef::Kind::Named && ct->declaration &&
        (ct->declaration->kind == ast::TypeDeclKind::Record ||
         ct->declaration->kind == ast::TypeDeclKind::Enum)) {
        if (scrut->getType()->isPointerTy())
            return builder_.getInt1(false);
        const sema::TypeRef* rScrutTy = resolveTypeParamRef(scrutTy);
        bool sameType = rScrutTy && rScrutTy->declaration == ct->declaration;
        return builder_.getInt1(sameType);
    }

    // A primitive substitution.
    if (ct->kind == sema::TypeRef::Kind::Primitive) {
        const sema::TypeRef* rScrutTy = resolveTypeParamRef(scrutTy);
        bool scrutSamePrim = rScrutTy &&
                             rScrutTy->kind == sema::TypeRef::Kind::Primitive &&
                             rScrutTy->primitive == ct->primitive &&
                             !rScrutTy->nullable;

        // T = Object: matches any non-null reference (§19.5 — a type pattern
        // never matches null). A non-pointer scrutinee cannot be an Object.
        if (ct->primitive == sema::PrimitiveType::Object) {
            if (scrut->getType()->isPointerTy())
                return builder_.CreateICmpNE(
                    scrut, llvm::ConstantPointerNull::get(ptrTy()),
                    "tp.obj.nonnull");
            return builder_.getInt1(false);
        }

        // T = string against a pointer scrutinee (a `string`/`string?` or a
        // boxed Object): match a non-null string value (mirrors the `var n:
        // string` branch in patternMatches).
        if (ct->primitive == sema::PrimitiveType::String &&
            scrut->getType()->isPointerTy()) {
            llvm::Value* nonNull = builder_.CreateICmpNE(
                scrut, llvm::ConstantPointerNull::get(ptrTy()),
                "tp.str.nonnull");
            // A statically `string`/`string?` scrutinee: non-null suffices.
            if (rScrutTy && rScrutTy->kind == sema::TypeRef::Kind::Primitive &&
                rScrutTy->primitive == sema::PrimitiveType::String)
                return nonNull;
            // An Object/interface scrutinee: a runtime is-string test, guarded so
            // bz_obj_is_string is never called on null.
            llvm::BasicBlock* preBB = builder_.GetInsertBlock();
            auto* isStrFn = declareC("bz_obj_is_string", i32(), {ptrTy(), ptrTy()});
            auto* chkBB = llvm::BasicBlock::Create(llvm_, "tp.str.chk", currentFn_);
            auto* joinBB =
                llvm::BasicBlock::Create(llvm_, "tp.str.join", currentFn_);
            builder_.CreateCondBr(nonNull, chkBB, joinBB);
            builder_.SetInsertPoint(chkBB);
            llvm::Value* isStr = builder_.CreateICmpNE(
                builder_.CreateCall(isStrFn, {scrut, vtableTablePtr()}),
                builder_.getInt32(0));
            llvm::BasicBlock* chkEnd = builder_.GetInsertBlock();
            builder_.CreateBr(joinBB);
            builder_.SetInsertPoint(joinBB);
            auto* phi = builder_.CreatePHI(i1(), 2);
            phi->addIncoming(builder_.getInt1(false), preBB);
            phi->addIncoming(isStr, chkEnd);
            return phi;
        }

        // T = a value primitive (int, bool, …) boxed in an Object (a pointer
        // scrutinee): a runtime is-a against the boxed primitive's descriptor,
        // short-circuiting a string. Mirrors the boxed-primitive branch in
        // patternMatches.
        if (!scrutSamePrim && scrut->getType()->isPointerTy())
            if (llvm::GlobalVariable* desc = boxDescriptorFor(ct))
                return guardedIsA(desc);

        // A non-pointer scrutinee already the same primitive: static match; a
        // differing value primitive can never match.
        return builder_.getInt1(scrutSamePrim);
    }

    return nullptr;  // not a lowerable type-test kind — caller falls back
}


void Emitter::emitThrowBuiltin(const char* name) {
    auto vit = builtinVtable_.find(name);
    if (vit == builtinVtable_.end()) return;
    // §31.4: kExcObjSize object; slot0=vtable, message/causechain/paramname
    // slots left null by calloc (a guard-thrown exception carries none).
    llvm::Value* obj = builder_.CreateCall(
        callocFn(), {builder_.getInt64(1), builder_.getInt64(kExcObjSize)});
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
    if (hasBuiltinExceptionBase(ts)) {
        // §31.4: full built-in exception header (see classStruct) — rare for
        // generics, but the key must match specializedStructs_ exactly.
        specFieldTys.push_back(ptrTy());       // [1] Message
        specFieldTys.push_back(ptrTy());       // [2] InnerException
        specFieldTys.push_back(ptrTy());       // [3] CauseChain
        specFieldTys.push_back(ptrTy());       // [4] ParamName
    }
    specFieldTys.push_back(disposedFlagTy());  // [1 or 5] disposed flag
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

    // #1843 (§12.7/§12.9): a constructed generic instantiation needs its OWN
    // runtime descriptor (vtable slot 0) so `x is Box<int>` distinguishes it
    // from `Box<string>`.  Derive the concrete type arguments from the active
    // substitution (genericSubstSemaTy_, set up by lowerNew), mirroring the
    // static-field per-instantiation Path A (§22.12/#536).  A null/unresolved
    // arg leaves instDesc == the shared descriptor and no descriptor
    // specialization is forced.
    llvm::GlobalVariable* instDesc = nullptr;
    {
        std::vector<const sema::TypeRef*> args;
        args.reserve(ts->genericParams.size());
        for (const sema::Symbol* gp : ts->genericParams) {
            auto it = genericSubstSemaTy_.find(gp->name);
            args.push_back(it != genericSubstSemaTy_.end() ? it->second
                                                           : nullptr);
        }
        if (!args.empty()) {
            llvm::GlobalVariable* pid = instanceDescriptorFor(ts, args);
            auto dit = descArray_.find(ts);
            llvm::GlobalVariable* sharedDesc =
                dit != descArray_.end() ? dit->second : nullptr;
            if (pid && pid != sharedDesc) instDesc = pid;
        }
    }
    bool needsSpecDescriptor = instDesc != nullptr;

    // #1247 (§28.3/§28.4): a generic class whose user-defined destructor body
    // references instantiation-dependent symbols (e.g. a static field of
    // GBox<T>: `GBox<T>._m := GBox<T>._m - 1`) must also get a specialized
    // disposer.  The shared type-erased disposer runs WITHOUT an active
    // genericSubst_, so staticFieldGlobal() resolves the access to the
    // TEMPLATE-LEVEL global (bz$GBox$_m) instead of the per-instantiation one
    // (bz$GBox$int$_m).  Emitting a specialized disposer (with genericSubst_
    // active) ensures the correct per-instantiation symbol is referenced.
    bool needsSpecDispose = classHasOwnedTypeParamField(ts)
                         || findDestructor(ts) != nullptr;

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

    if (!needsSpecDispose && !needsSpecItable && !needsSpecDescriptor)
        return shared;

    // #1843: when the instantiation needs a per-monomorphization descriptor,
    // cache by the descriptor global (unique per type-argument identity) — the
    // layout key below conflates Box<string> and Box<Dog> (both `ptr`), but they
    // must carry DISTINCT slot-0 descriptors.  Otherwise keep the layout key.
    if (needsSpecDescriptor) {
        auto& m = specVtablesByDesc_[ts];
        auto it = m.find(instDesc);
        if (it != m.end()) return it->second;
    }

    // Key by the spec field types so Box<Elem> and Box<int> get distinct vtables.
    std::vector<const sema::Symbol*> storage;
    collectInstanceStorage(ts, storage);
    std::vector<llvm::Type*> specFieldTys;
    specFieldTys.push_back(ptrTy());
    specFieldTys.push_back(disposedFlagTy());
    for (const sema::Symbol* m : storage)
        specFieldTys.push_back(storageType(m));

    auto& specMap = specializedVtables_[ts];
    if (!needsSpecDescriptor) {
        auto sit = specMap.find(specFieldTys);
        if (sit != specMap.end()) return sit->second;
    }

    std::string suffix = "$spec" + std::to_string(
        specMap.size() + specVtablesByDesc_[ts].size());

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
    if (instDesc)
        elems[0] = instDesc;  // #1843: slot 0 = per-monomorphization descriptor
    auto* init = llvm::ConstantArray::get(arrTy, elems);
    auto* gv = new llvm::GlobalVariable(
        *module_, arrTy, /*isConstant=*/true,
        llvm::GlobalValue::InternalLinkage, init,
        "vt$" + std::string(ts->name) + suffix);
    // §8.6 #417: a specialized vtable is a word-0 vtable an object can carry, so
    // register it in the string discriminator's whitelist (bz$vtable$table) —
    // otherwise bz_obj_is_string misreads such an instance as a string and
    // `is`/`as` on it silently fails (#1843). finalizeVtableTable emits the table
    // after this push.
    allVtables_.push_back(gv);
    if (needsSpecDescriptor) specVtablesByDesc_[ts][instDesc] = gv;
    else specMap[specFieldTys] = gv;
    return gv;
}


// #1843 (§12.7/§12.9/§19.6): per-monomorphization runtime descriptors. See the
// declaration comment in LlvmBackend_impl.hpp for the rationale; the three
// helpers below (descReadableMangle, descIdentityKey, descArgsConcrete,
// substituteTypeRefLocal) support the instanceDescriptorFor factory.

// A readable name fragment for a resolved type — used only for the LLVM global
// name (`td$Box$int`); LLVM uniquifies on collision, so this is cosmetic. It
// still recurses through type arguments and marks nullability so a reader can
// tell instantiations apart at a glance.
std::string Emitter::descReadableMangle(const sema::TypeRef* t) {
    if (!t) return "t";
    std::string s;
    switch (t->kind) {
        case sema::TypeRef::Kind::Named:
            if (t->declaration) {
                s = std::string(t->declaration->name);
                for (const sema::TypeRef* a : t->genericArgs)
                    s += "_" + descReadableMangle(a);
            } else {
                s = std::string(t->headName());
            }
            break;
        case sema::TypeRef::Kind::Primitive: {
            static const char* names[] = {
                "none", "bool", "char", "string", "byte", "sbyte", "short",
                "ushort", "int", "uint", "long", "ulong", "float", "double",
                "decimal", "object"};
            unsigned i = static_cast<unsigned>(t->primitive);
            s = i < (sizeof(names) / sizeof(names[0])) ? names[i] : "prim";
            break;
        }
        case sema::TypeRef::Kind::InlineArray:
            s = "arr_" + descReadableMangle(t->arrayElement);
            break;
        case sema::TypeRef::Kind::Function:
            s = "fn";
            break;
        default:
            s = "t";
            break;
    }
    if (t->nullable) s += "$n";
    return s;
}


// A structural, pointer-based cache key for a constructed generic type. Unlike
// the readable name, this disambiguates distinct declarations sharing a name
// (declaration pointer) and primitives by enum value, and recurses through the
// argument list — so two type-arg lists produce the same key iff they denote the
// same type (pointer identity + nullability), which is exactly the runtime
// identity `is`/`as` must honor.
static void appendDescKey(const sema::TypeRef* t, std::string& out) {
    if (!t) { out += "0;"; return; }
    switch (t->kind) {
        case sema::TypeRef::Kind::Named:
            if (t->declaration) {
                out += 'C';
                out += std::to_string(reinterpret_cast<std::uintptr_t>(
                    static_cast<const void*>(t->declaration)));
                out += '<';
                for (const sema::TypeRef* a : t->genericArgs) appendDescKey(a, out);
                out += '>';
            } else {
                out += "U";
                out += std::string(t->headName());
            }
            break;
        case sema::TypeRef::Kind::Primitive:
            out += "P";
            out += std::to_string(static_cast<int>(t->primitive));
            break;
        case sema::TypeRef::Kind::InlineArray:
            out += "A";
            appendDescKey(t->arrayElement, out);
            break;
        case sema::TypeRef::Kind::Function:
            out += "F";
            break;
        default:
            out += "X";
            break;
    }
    if (t->nullable) out += "?";
    out += ';';
}

std::string Emitter::descIdentityKey(
    const sema::TypeSymbol* ts,
    const std::vector<const sema::TypeRef*>& args) {
    std::string key = "C";
    key += std::to_string(
        reinterpret_cast<std::uintptr_t>(static_cast<const void*>(ts)));
    key += '<';
    for (const sema::TypeRef* a : args) appendDescKey(a, key);
    key += '>';
    return key;
}

// #1947: recursive structural key of one constructed type argument. Mirrors the
// per-argument encoding descIdentityKey applies to a type's arg list, so it
// distinguishes a self-nested instantiation (Wrap<int> vs Wrap<Wrap<int>>) —
// exactly the discrimination the runtime `is`/`as` descriptor honors and the
// monomorphization cache must too.
std::string Emitter::typeRefIdentityKey(const sema::TypeRef* t) {
    std::string key;
    appendDescKey(t, key);
    return key;
}

bool Emitter::descArgsConcrete(const std::vector<const sema::TypeRef*>& args) {
    for (const sema::TypeRef* a : args) {
        // A concrete arg carries a real identity token (a TypeSymbol* or a
        // primitive tag). An unbound generic-parameter placeholder is Named
        // with declaration == nullptr — typeRefIdentityToken returns null.
        if (typeRefIdentityToken(a) == nullptr) return false;
    }
    return true;
}


// Substitute the param-name → concrete-TypeRef `map` through `t`. A bare
// generic-parameter reference (Named, declaration == nullptr, single-part name)
// is replaced by its bound argument; a constructed type recurses through its
// arguments, allocating a fresh TypeRef into descArena_ only when substitution
// actually changes something (so the common identity case allocates nothing).
const sema::TypeRef* Emitter::substituteTypeRefLocal(
    const sema::TypeRef* t,
    const std::unordered_map<std::string_view, const sema::TypeRef*>& map) {
    if (!t || map.empty()) return t;
    if (t->kind == sema::TypeRef::Kind::Named) {
        if (!t->declaration) {
            // A bare parameter placeholder — resolve by head name.
            std::string_view head = t->headName();
            if (!head.empty()) {
                auto it = map.find(head);
                if (it != map.end() && it->second) {
                    // Preserve `T?` nullability of the placeholder occurrence.
                    if (t->nullable && !it->second->nullable) {
                        auto copy = std::make_unique<sema::TypeRef>(*it->second);
                        copy->nullable = true;
                        const sema::TypeRef* raw = copy.get();
                        descArena_.push_back(std::move(copy));
                        return raw;
                    }
                    return it->second;
                }
            }
            return t;
        }
        // A constructed type: substitute inside its arguments.
        bool changed = false;
        std::vector<sema::TypeRef*> newArgs;
        newArgs.reserve(t->genericArgs.size());
        for (const sema::TypeRef* a : t->genericArgs) {
            const sema::TypeRef* sub = substituteTypeRefLocal(a, map);
            if (sub != a) changed = true;
            newArgs.push_back(const_cast<sema::TypeRef*>(sub));
        }
        if (!changed) return t;
        auto copy = std::make_unique<sema::TypeRef>(*t);
        copy->genericArgs = std::move(newArgs);
        const sema::TypeRef* raw = copy.get();
        descArena_.push_back(std::move(copy));
        return raw;
    }
    if (t->kind == sema::TypeRef::Kind::InlineArray && t->arrayElement) {
        const sema::TypeRef* sub = substituteTypeRefLocal(t->arrayElement, map);
        if (sub == t->arrayElement) return t;
        auto copy = std::make_unique<sema::TypeRef>(*t);
        copy->arrayElement = const_cast<sema::TypeRef*>(sub);
        const sema::TypeRef* raw = copy.get();
        descArena_.push_back(std::move(copy));
        return raw;
    }
    return t;
}


// The per-monomorphization descriptor factory. Returns a global whose ADDRESS is
// the runtime identity of `ts<args>` and whose CONTENTS are the flat array of
// ancestor descriptors `emitIsA` walks — each ancestor reified under the
// substitution `ts<args>` induces, so a generic base (`Ok<int>: Result<int>`)
// contributes its OWN per-instantiation descriptor, not the shared erased one.
llvm::GlobalVariable* Emitter::instanceDescriptorFor(
    const sema::TypeSymbol* ts,
    const std::vector<const sema::TypeRef*>& args) {
    // Fallback: a non-generic type, an arity mismatch, or a non-concrete arg
    // keeps the shared descriptor — correct for non-generics and the only sound
    // option for a genuinely-unbound template (which never reaches a `new`/`is`
    // site with concrete operands).
    auto sharedOf = [&](const sema::TypeSymbol* s) -> llvm::GlobalVariable* {
        auto it = descArray_.find(s);
        return it != descArray_.end() ? it->second : nullptr;
    };
    if (!ts || ts->genericParams.empty() ||
        args.size() != ts->genericParams.size() || !descArgsConcrete(args))
        return sharedOf(ts);

    std::string key = descIdentityKey(ts, args);
    auto cached = instanceDesc_.find(key);
    if (cached != instanceDesc_.end()) return cached->second;

    // Phase 1: BFS over the (substituted) super-chain, mirroring
    // collectTypeAndBases so the flat array lists ts and every transitive
    // base/interface exactly once. Each node records its own concrete args (for
    // its own genericParams); this phase mints nothing, so it terminates on the
    // finite (dedup-by-symbol) super set with no reentrancy.
    struct Node {
        const sema::TypeSymbol* sym;
        std::vector<const sema::TypeRef*> symArgs;  // for sym's own params
        bool concrete;                              // all symArgs concrete
    };
    struct Pending {
        const sema::TypeSymbol* sym;
        std::unordered_map<std::string_view, const sema::TypeRef*> subst;
    };
    std::vector<Node> nodes;
    std::vector<Pending> frontier;
    std::unordered_set<const sema::TypeSymbol*> seen;
    {
        std::unordered_map<std::string_view, const sema::TypeRef*> m0;
        for (std::size_t i = 0; i < ts->genericParams.size(); ++i)
            m0[ts->genericParams[i]->name] = args[i];
        frontier.push_back(Pending{ts, std::move(m0)});
        seen.insert(ts);
    }
    for (std::size_t i = 0; i < frontier.size(); ++i) {
        const sema::TypeSymbol* sym = frontier[i].sym;
        auto subst = frontier[i].subst;  // copy — frontier may reallocate below
        std::vector<const sema::TypeRef*> symArgs;
        bool symConcrete = !sym->genericParams.empty();
        for (const sema::Symbol* gp : sym->genericParams) {
            auto it = subst.find(gp->name);
            const sema::TypeRef* a = it != subst.end() ? it->second : nullptr;
            symArgs.push_back(a);
            if (typeRefIdentityToken(a) == nullptr) symConcrete = false;
        }
        nodes.push_back(Node{sym, std::move(symArgs), symConcrete});
        for (std::size_t bi = 0; bi < sym->baseTypes.size(); ++bi) {
            const sema::TypeSymbol* b = sym->baseTypes[bi];
            if (!b || !seen.insert(b).second) continue;
            std::unordered_map<std::string_view, const sema::TypeRef*> bm;
            const sema::TypeRef* bref =
                bi < sym->baseTypeRefs.size() ? sym->baseTypeRefs[bi] : nullptr;
            if (bref && b->genericParams.size() == bref->genericArgs.size()) {
                for (std::size_t gi = 0; gi < b->genericParams.size(); ++gi)
                    bm[b->genericParams[gi]->name] =
                        substituteTypeRefLocal(bref->genericArgs[gi], subst);
            }
            frontier.push_back(Pending{b, std::move(bm)});
        }
    }

    // Compute the array size (nodes + built-in exception chain + null) and mint
    // the global with NO initializer, then CACHE it — so the recursive
    // instanceDescriptorFor calls in phase 2 resolve a self/diamond reference to
    // this same global instead of re-minting or looping.
    std::vector<std::string> builtinNames;
    {
        std::string bn = directBuiltinExceptionBase(ts);
        std::unordered_map<std::string, std::string> builtinParent;
        for (const auto& [name, par] : builtinExceptionHierarchy())
            builtinParent[name] = par;
        while (!bn.empty()) {
            builtinNames.push_back(bn);
            auto pit = builtinParent.find(bn);
            bn = pit != builtinParent.end() ? pit->second : std::string{};
        }
    }
    std::size_t size = nodes.size() + builtinNames.size() + 1;
    auto* arrTy = llvm::ArrayType::get(ptrTy(), size);
    // Readable (cosmetic) global name from ts + args.
    std::string readable;
    {
        auto tmp = std::make_unique<sema::TypeRef>();
        tmp->kind = sema::TypeRef::Kind::Named;
        tmp->declaration = const_cast<sema::TypeSymbol*>(ts);
        for (const sema::TypeRef* a : args)
            tmp->genericArgs.push_back(const_cast<sema::TypeRef*>(a));
        readable = descReadableMangle(tmp.get());
        descArena_.push_back(std::move(tmp));
    }
    auto* gv = new llvm::GlobalVariable(
        *module_, arrTy, /*isConstant=*/true,
        llvm::GlobalValue::InternalLinkage, /*init=*/nullptr, "td$" + readable);
    instanceDesc_[key] = gv;  // cache BEFORE phase 2 (self / diamond / cycle)

    // Phase 2: fill the elements. Element for ts itself is the global (self);
    // a generic super with concrete args contributes its per-instantiation
    // descriptor; every other super its shared descriptor.
    std::vector<llvm::Constant*> elems;
    elems.reserve(size);
    for (const Node& n : nodes) {
        if (n.sym == ts) {
            elems.push_back(gv);
        } else if (n.concrete) {
            elems.push_back(instanceDescriptorFor(n.sym, n.symArgs));
        } else {
            llvm::GlobalVariable* sd = sharedOf(n.sym);
            elems.push_back(sd ? llvm::cast<llvm::Constant>(sd)
                               : llvm::ConstantPointerNull::get(ptrTy()));
        }
    }
    for (const std::string& bn : builtinNames) {
        auto bit = builtinDesc_.find(bn);
        elems.push_back(bit != builtinDesc_.end()
                            ? llvm::cast<llvm::Constant>(bit->second)
                            : llvm::ConstantPointerNull::get(ptrTy()));
    }
    elems.push_back(llvm::ConstantPointerNull::get(ptrTy()));
    gv->setInitializer(llvm::ConstantArray::get(arrTy, elems));
    return gv;
}


// #1843: resolve an `is`/`as`/case-pattern TARGET to its per-monomorphization
// descriptor when it is a constructed generic class or interface with concrete
// arguments (`is Box<int>`). Returns nullptr for a non-generic target, a bare
// template, or when the descriptor would fall back to the shared one — the
// caller then keeps its existing shared-descriptor resolution. Type-parameter
// arguments are resolved through the active substitution first, so `is Box<T>`
// inside a monomorphized body tests against the concrete instantiation (§19.3
// v0.182 / §19.6 v0.196).
llvm::GlobalVariable* Emitter::perInstDescriptorForTarget(
    const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || !rt->declaration)
        return nullptr;
    const sema::TypeSymbol* ts = rt->declaration;
    if ((ts->kind != ast::TypeDeclKind::Class &&
         ts->kind != ast::TypeDeclKind::Interface) ||
        ts->genericParams.empty() ||
        rt->genericArgs.size() != ts->genericParams.size())
        return nullptr;
    // #1843: resolve each argument through the active substitution
    // RECURSIVELY. `resolveTypeParamRef` only rewrites a TOP-LEVEL bare
    // parameter, so inside a monomorphized `Checker<int>` body a nested target
    // `Box<Box<T>>` kept its inner `T` unbound — the resulting descriptor was
    // keyed on the placeholder and never equalled the object's fully-resolved
    // `td$Box$Box_int`, making the test constant-false (a correct→wrong
    // regression on a legal program). substituteTypeRefLocal rewrites the bare
    // top-level param AND recurses into constructed-type arguments, arena-ing
    // the rewritten TypeRef into descArena_ (Emitter-lifetime), so `Box<Box<T>>`
    // with T=int resolves to the same per-instantiation global the `new` site
    // minted. Outside a generic body genericSubstSemaTy_ is empty and this is a
    // no-op, so a concrete literal target (`is Box<int>`) is unaffected.
    std::vector<const sema::TypeRef*> args;
    args.reserve(rt->genericArgs.size());
    for (const sema::TypeRef* a : rt->genericArgs)
        args.push_back(substituteTypeRefLocal(a, genericSubstSemaTy_));
    if (!descArgsConcrete(args)) return nullptr;
    llvm::GlobalVariable* pid = instanceDescriptorFor(ts, args);
    auto dit = descArray_.find(ts);
    llvm::GlobalVariable* sharedDesc =
        dit != descArray_.end() ? dit->second : nullptr;
    return (pid && pid != sharedDesc) ? pid : nullptr;
}


bool Emitter::isFreshOwnedClassProducer(const ast::Expr* e) const {
    // #1414 stage 4 phase A: the codegen-side consumer of the shared ownership-
    // facts core. The expression-shape classification (#1039/#1053) now lives in
    // common::isFreshOwnedClassProducer; this wrapper injects codegen's
    // `resolveTypeParamRef` (backed by the active `genericSubstSemaTy_`
    // monomorphization binding) as the TypeParamResolver hook. Behaviour is
    // byte-identical to the pre-extraction predicate.
    return common::isFreshOwnedClassProducer(
        e, ctx_,
        [this](const sema::TypeRef* t) { return resolveTypeParamRef(t); });
}


// §22.7 / §22.13 / §24.1: the entry-point (Main) static-init prologue. Runs
// the program-wide static initialization that must complete before any user
// code: the SerializationException runtime vtable, every non-lazy static
// constructor, and the allocation of each static event's backing EventSource<T>
// so a subscribe/raise on a static event reaches a real shared source.
//
// Requires builder_ positioned in the entry function (currentFn_ is the C
// `main`, whose propagateBB routes an uncaught static-init fault to
// bz_unhandled_exception — §33.3) and emitPendingCheck's unwind chain set up
// (propagateBB_/unwindTarget_ as at method-body entry). Called from
// emitFunctionBody for a synchronous Main, and from emitAsyncMethod's entry for
// an async Main — the latter returns from emitFunctionBody before the inline
// prologue site, so without this call the static event sources (and every other
// static root) were never allocated (#1502).
void Emitter::emitMainStaticInitPrologue() {
    // §2.15 / §9.8: register the SerializationException vtable with the runtime
    // so bz_ser_throw (called by JSON runtime helpers) produces a well-formed
    // exception that emitIsA's catch dispatch can identify. Must run before any
    // user code (incl. static ctors) can call serializer APIs. The vtable
    // pointer is always defined here (setupBuiltinExceptions runs in emitModule
    // before any method lowering).
    {
        auto vit = builtinVtable_.find("SerializationException");
        if (vit != builtinVtable_.end()) {
            auto* initFn = declareC("bz_ser_exc_vtable_init",
                                    llvm::Type::getVoidTy(llvm_), {ptrTy()});
            builder_.CreateCall(initFn, {vit->second});
        }
    }
    // #720 / #796 / §22.13: every type with static state now initializes LAZILY
    // on first use (via bz$<T>$ensureinit at each static-access site) — non-
    // generic since #720, generic since #796. Skip any type that has a lazy
    // ensureinit; only a type WITHOUT one (none today) would run here.
    // fix #529 / §22.7 (part b): after each static-ctor call, check bz_exc. If
    // the ctor threw, its own propagate block already wrapped the original
    // exception in TypeInitializationException; emitPendingCheck routes the
    // wrapped exception through Main's normal unwind path.
    for (auto& e : staticCtors_) {
        if (e.type && ensureInitFns_.count(e.type)) continue;  // lazy
        builder_.CreateCall(e.fn, {});
        emitPendingCheck();
    }
    // §24.1 / §22.13 (#477 B6): allocate each static event's backing
    // EventSource<T> at program start (the single static root), so a
    // subscribe/raise on a static event reaches a real shared source. Mirrors
    // the per-instance allocation done during construction (emitInstanceInit),
    // but at the program-wide static-init point. bz_static_event_source_new also
    // registers the source for program-exit disposal (§22.13) via atexit — a
    // static owned field is disposed at program exit, not by any instance
    // disposer.
    if (!staticEventGlobals_.empty()) {
        auto* newFn =
            declareC("bz_static_event_source_new", ptrTy(), {});
        for (auto& [es, gv] : staticEventGlobals_) {
            llvm::Value* esrc = builder_.CreateCall(newFn, {});
            builder_.CreateStore(esrc, gv);
        }
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
        // §19.3 (v0.182, #1852): collect the generic parameter names in scope so
        // a type-parameter pattern in the (dead) template body is recognizable
        // even with no active substitution (genericSubstSemaTy_ empty there).
        templateGenericParamNames_.clear();
        if (ms->parent && ms->parent->decl) {
            if (auto* ptd = dynamic_cast<const ast::TypeDecl*>(ms->parent->decl))
                if (!ptd->genericParams.empty()) {
                    genericTemplate = true;
                    for (const auto& gp : ptd->genericParams)
                        templateGenericParamNames_.insert(gp.name);
                }
        }
        if (ms->decl) {
            if (auto* md = dynamic_cast<const ast::MethodDecl*>(ms->decl))
                if (!md->genericParams.empty()) {
                    genericTemplate = true;
                    for (const auto& gp : md->genericParams)
                        templateGenericParamNames_.insert(gp.name);
                }
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
    // emitIteratorMethod). Gated to the Task / Task<T> / OwningTask<T> forms;
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
    // #1930: which plain `var g := <src>.GroupBy(sel).First()/…` locals (a bare-
    // GroupBy element-returning terminal) may have the returned grouping freed at
    // scope end. Populated by collectBorrowingArrayFreeEligible; consulted in
    // lowerLocalDecl (mirrors borrowingArrayFreeEligible_).
    groupingFreeEligible_.clear();
    groupingFreeEligibleFn_ = fn;
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
    // §22.7 / §22.13 / §24.1: the entry-point (Main) static-init prologue —
    // SerializationException vtable, static ctors, and static-event backing
    // sources. Factored into emitMainStaticInitPrologue so the async-Main entry
    // (emitAsyncMethod, which returns from emitFunctionBody before this point)
    // can run the same prologue before driving its state machine (#1502).
    if (isMain(ms))
        emitMainStaticInitPrologue();

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
            // §29.2 / §4.3 (#1310): wrap an array/user-class `=> expr` result
            // returned as IEnumerable<T> into the vtable-backed enumerator (a raw
            // array/instance returned as IEnumerable → SIGSEGV on the caller's
            // for-in / scope-end iterEnumDispose). Mirrors the ReturnStmt path.
            // #1916: ownLazyPipeline=true — a lazy pipeline `=> xs.Select(…)` is
            // drained into an OWNING enumerator the caller disposes (leak-free).
            if (v && currentReturnTypeNode_)
                v = wrapAsIEnumerableForTarget(
                    v, ctx_.resolvedType(*currentReturnTypeNode_), ci.exprBody,
                    /*ownLazyPipeline=*/true);
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
            // §5.1 (#1778): a numeric arrow-body result narrower than the
            // declared return type is implicitly WIDENED (`=> 42` for a `long`
            // method, `=> 1.5f` for a `double`, `=> byte(5)` for `int`, …).
            // The ReturnStmt path (LlvmBackend_Stmts.cpp ~line 1383) applies the
            // same coercion; the arrow path emitted the bare narrow value as the
            // `ret` operand (e.g. `ret i32` into an `i64` function) — malformed
            // IR that fails the verifier for VALID code. coerceNumericToType is a
            // no-op for ptr/struct returns and for an already-matching width, so
            // the nullable-value case coerced just above is unaffected.
            if (v && ci.exprBody && !fn->getReturnType()->isVoidTy())
                v = coerceNumericToType(v, fn->getReturnType(),
                                        isUnsignedTargetExpr(*ci.exprBody));
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
    //   2. Allocate a TypeInitializationException (kExcObjSize / 40 bytes —
    //      the uniform built-in exception block, §31.4; #1033 requires the
    //      full size so the disposer's ParamName@32 read stays in bounds):
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
    // NOTE (#1033): the TypeInitializationException disposer is bz$exDisposeMsg
    // (built-in exceptions share it), which releases Message@8 / ParamName@32
    // (both null for a TIE → no-op) and does NOT recurse into InnerException@16;
    // the inner object is therefore not freed at program exit when the exception
    // is not caught. This is an acceptable simplification for the process-exit
    // path; tracked as a follow-on to issue #529.
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
            auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
            // #1553 / §22.7 (v0.181): if the escaping exception is ITSELF a
            // TypeInitializationException — the §22.13 cycle backstop synthesizes
            // one, and a nested static init may have already wrapped — it is
            // cached DIRECTLY, not wrapped again. Compare its vtable (offset 0)
            // with the TIE vtable; on a match, leave bz_exc unchanged. A null
            // exc (defensive; propagate is only reached with a pending exc) also
            // needs no wrapping.
            auto* retBB     = llvm::BasicBlock::Create(llvm_, "sinit.prop.ret",  fn);
            auto* notNullBB = llvm::BasicBlock::Create(llvm_, "sinit.prop.nn",   fn);
            auto* wrapBB    = llvm::BasicBlock::Create(llvm_, "sinit.prop.wrap", fn);
            llvm::Value* isNull = builder_.CreateICmpEQ(
                origExc, llvm::ConstantPointerNull::get(ptrTy()));
            builder_.CreateCondBr(isNull, retBB, notNullBB);

            builder_.SetInsertPoint(notNullBB);
            llvm::Value* vt = builder_.CreateLoad(ptrTy(), origExc);  // vtable@0
            llvm::Value* isTie = builder_.CreateICmpEQ(vt, vit->second);
            builder_.CreateCondBr(isTie, retBB, wrapBB);

            builder_.SetInsertPoint(wrapBB);
            llvm::Value* tie = builder_.CreateCall(
                callocFn(),
                {builder_.getInt64(1), builder_.getInt64(kExcObjSize)});
            // vtable at offset 0
            builder_.CreateStore(vit->second, tie);
            // message at offset 8: null (calloc zeroed it)
            // InnerException at offset 16 (kExcInnerOffset)
            llvm::Value* innerSlot =
                builder_.CreateGEP(i8Ty, tie, builder_.getInt64(kExcInnerOffset));
            builder_.CreateStore(origExc, innerSlot);
            builder_.CreateStore(tie, emitExcSlot());
            builder_.CreateBr(retBB);

            builder_.SetInsertPoint(retBB);
            builder_.CreateRetVoid();
        } else {
            builder_.CreateRetVoid();
        }
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
            // #1309 / §22.13: skip only the BAKED initializers (readonly
            // literal) — constInitFor already encoded those as the global's LLVM
            // initializer, so they need no runtime store. A MUTABLE static's
            // literal initializer is NOT baked; it is stored here in textual
            // order so a forward reference to it from an earlier initializer hits
            // the partial-state guard instead of silently reading a compile-time
            // value.
            if (isBakedStaticFieldInit(fs, initExpr)) continue;
            // Non-constant initializer: evaluate it and store into the global.
            auto gvIt = staticGlobals_.find(fs);
            if (gvIt == staticGlobals_.end()) continue;  // no backing global
            // #1553 / §22.13 (v0.181): after this field's initializer store lands,
            // advance the owning type's init-progress to (this field's index + 1),
            // so a re-entrant read of a later (not-yet-stored) static observes it
            // as unassigned and the partial-state guard raises. `bumpProgress` is
            // emitted on every store path (inline-array early-continue included).
            auto pgIt = initProgressGV_.find(type);
            int fieldIdx = staticFieldInitIndex(fs);
            auto bumpProgress = [&]() {
                if (pgIt != initProgressGV_.end() && fieldIdx >= 0)
                    builder_.CreateStore(
                        llvm::ConstantInt::get(i32(),
                            static_cast<uint64_t>(fieldIdx) + 1),
                        pgIt->second);
            };
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
                        if (!ev) {
                            // #1386: ICE — lowerExpr returned null for a
                            // static field inline-array initializer element.
                            ctx_.diagnostics().report(
                                Severity::Error,
                                cl->elements[i]->span.begin,
                                "internal: lowerExpr returned null for static "
                                "field inline-array initializer element — #1386");
                            continue;
                        }
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
                    bumpProgress();  // #1553: inline-array field now assigned
                    continue;  // inline-array literal stored element-wise
                }
            }
            if (!val) val = lowerExpr(*initExpr);
            if (!val) {
                // #1386: ICE — lowerExpr returned null for a static field
                // initializer; the expression kind is not yet handled by codegen.
                ctx_.diagnostics().report(
                    Severity::Error, initExpr->span.begin,
                    "internal: lowerExpr returned null for static field "
                    "initializer — #1386");
                continue;
            }
            builder_.CreateStore(val, gvIt->second);
            bumpProgress();  // #1553 / §22.13: this field is now assigned
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
    // #1553 / §22.13 (v0.181): the same-thread re-entrancy backstop needs the
    // id of the thread currently touching a type, to tell a SAME-thread re-entrant
    // touch of an in-progress initializer (→ synthesize TypeInitializationException)
    // apart from a cross-domain touch on another pool thread (→ §22.12 rule-1
    // blocking; not synthesized here, #1391).
    llvm::Function* tidFn = declareC("bz_current_thread_id", i64(), {});

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
        // #1553 / §22.13 (v0.181): owner-thread tracking. Set to the id of the
        // thread that moved the state to in-progress (1); read back when a touch
        // finds the state still in-progress, to classify it as SAME-thread
        // re-entrancy (the cycle backstop) vs. a cross-domain touch. 0 = no owner.
        auto* ownerTidGV = new llvm::GlobalVariable(
            *module_, i64(), /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantInt::get(i64(), 0), "bz$" + cls + "$initowner");
        // #1553 / §22.13 (v0.181): initialization progress — the count of this
        // type's non-constant static-field initializers (sfinit-stored, textual
        // order) that have completed. sfinit bumps it after each store; the
        // partial-state read guard compares a field's declaration index against
        // it to tell an already-assigned static from a not-yet-assigned one.
        auto* progressGV = new llvm::GlobalVariable(
            *module_, i32(), /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantInt::get(i32(), 0), "bz$" + cls + "$initprogress");
        initStateGV_[t]    = stateGV;
        initOwnerGV_[t]    = ownerTidGV;
        initProgressGV_[t] = progressGV;

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

        // #1553 / §22.13 (v0.181): in-progress → return (proceed), NOT raise. A
        // re-entrant touch here is the ordinary self-reference case — a same-type
        // static METHOD call during the type's own init (e.g. `_v := Seed()`)
        // legitimately re-enters and must run. Partial-state OBSERVATION is caught
        // precisely at static-field READ sites (emitStaticPartialReadGuard): a
        // re-entrant read, on the OWNER thread, of a static that the in-progress
        // initializer has not assigned yet raises the backstop TIE. This keeps the
        // backstop strictly same-thread-only (owner-thread tracking below) while
        // not falsely raising for same-type helper calls or reads of already-
        // assigned fields; a cross-domain in-progress touch proceeds here (the
        // §22.12 rule-1 blocking case is unimplemented — #1391).
        builder_.SetInsertPoint(chkProg);
        llvm::Value* isProg =
            builder_.CreateICmpEQ(s, llvm::ConstantInt::get(i8Ty, 1));
        builder_.CreateCondBr(isProg, retB, initB);

        // doinit: mark in-progress, record the owner thread (§22.13 owner-thread
        // tracking), run sfinit then cctor with pending-exc checks.
        builder_.SetInsertPoint(initB);
        builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 1), stateGV);
        builder_.CreateStore(builder_.CreateCall(tidFn, {}, "owntid"), ownerTidGV);
        // #792 / §22.13: snapshot any PRE-EXISTING in-flight exception before
        // running sfinit/cctor. The lazy init can be triggered while an exception
        // is already propagating — most notably an async-iterator `finally`
        // disposing its frame during a body-throw unwind touches a static member,
        // which lazily runs the owner's sfinit with bz_exc already set. Without
        // this snapshot the post-sfinit "did it throw?" check (`bz_exc != null`)
        // misreads the pre-existing exception as "sfinit threw", spuriously
        // marking the type failed and raising a TypeInitializationException.
        // Compare the FRESH load against the snapshot: sfinit/cctor "threw" iff
        // bz_exc CHANGED (its propagate-block wraps the original in a fresh TIE,
        // so the pointer differs). When no exception was in flight the snapshot
        // is null and this reduces exactly to the old `e != null` test.
        llvm::Value* initPreExc =
            builder_.CreateLoad(ptrTy(), emitExcSlot(), "init.preexc");
        auto sfIt = sfinitFnOf_.find(t);
        auto ccIt = cctorFnOf_.find(t);
        auto* afterSf = llvm::BasicBlock::Create(llvm_, "after_sf", fn);
        if (sfIt != sfinitFnOf_.end()) {
            builder_.CreateCall(sfIt->second, {});
            llvm::Value* e = builder_.CreateLoad(ptrTy(), emitExcSlot());
            llvm::Value* thrown = builder_.CreateICmpNE(e, initPreExc);
            builder_.CreateCondBr(thrown, failB, afterSf);
        } else {
            builder_.CreateBr(afterSf);
        }
        builder_.SetInsertPoint(afterSf);
        if (ccIt != cctorFnOf_.end()) {
            builder_.CreateCall(ccIt->second, {});
            llvm::Value* e = builder_.CreateLoad(ptrTy(), emitExcSlot());
            llvm::Value* thrown = builder_.CreateICmpNE(e, initPreExc);
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
        // pointer on a later access would be a use-after-free. The TIE's disposer
        // (bz$exDisposeMsg, #1033) releases only the owned Message@8 / ParamName@32
        // (both null for a TIE → no-op) and intentionally does NOT touch
        // InnerException@16, so the inner survives the first TIE's catch-arm
        // disposal and stays valid for the reraise block to re-wrap. Mark failed;
        // leave bz_exc = the TIE for this first raise.
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
        //
        // #1033 / §31.4: the block MUST be the uniform kExcObjSize (40), NOT the
        // 24 bytes this once hard-coded. The TIE vtable's slot 2 is the built-in
        // exception disposer bz$exDisposeMsg (it shares the disposer of every
        // built-in exception), which reads ParamName@32 (and Message@8) on
        // catch-arm-exit disposal. A 24-byte block put offset 32 OUT OF BOUNDS —
        // the disposer read/wrote past the allocation, corrupting the heap and
        // crashing the static-init failed-state-reraise paths (#720 / #796 /
        // #1553). Message@8 stays null (calloc) so its release is a no-op;
        // kExcObjSize keeps the ParamName@32 read in-bounds (also null → no-op),
        // matching emitStaticConstructorBody's propagate-block TIE (which already
        // uses kExcObjSize) and the §31.4 layout note in LlvmBackend_Iterators /
        // LlvmBackend_Async ("a smaller alloc would make CauseChain@24 /
        // ParamName@32 reads read out of bounds").
        builder_.SetInsertPoint(failedB);
        llvm::Value* savedInner = builder_.CreateLoad(ptrTy(), excGV);
        auto tieVit = builtinVtable_.find("TypeInitializationException");
        if (tieVit != builtinVtable_.end()) {
            llvm::Value* tie = builder_.CreateCall(
                callocFn(), {builder_.getInt64(1), builder_.getInt64(kExcObjSize)});
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
        // #792 / §22.13: snapshot bz_exc around the ensureinit call and route
        // ONLY an exception the init RAISED (sfinit/cctor failed → TIE), not a
        // PRE-EXISTING in-flight exception. The static-init guard can fire while
        // an exception is already propagating — e.g. an async-iterator `finally`
        // disposing its frame during a body-throw unwind touches a static member,
        // running the owner's lazy init with bz_exc already set. Unconditionally
        // re-checking bz_exc here would route the pre-existing exception at the
        // access site, skipping the field access (e.g. the `flag := true` store
        // that records frame disposal). The ensureinit itself is guarded against
        // misattributing the pre-existing exception (doinit block, above), so if
        // bz_exc is UNCHANGED the init succeeded and the access should proceed;
        // the pre-existing exception stays in bz_exc and routes at the next
        // natural pending check. Mirrors the #1468 disposer-body "is-new" gate.
        llvm::Value* preExc =
            builder_.CreateLoad(ptrTy(), emitExcSlot(), "si.preexc");
        builder_.CreateCall(it->second, {});
        llvm::Value* postExc =
            builder_.CreateLoad(ptrTy(), emitExcSlot(), "si.postexc");
        emitPendingCheckIf(builder_.CreateICmpNE(preExc, postExc, "si.raised"));
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
        // #792: same pre/post snapshot as the local ensureinit above — route
        // only an exception the cross-module init raised, not a pre-existing one.
        llvm::Value* preExc =
            builder_.CreateLoad(ptrTy(), emitExcSlot(), "si.preexc");
        builder_.CreateCall(fn, {});
        llvm::Value* postExc =
            builder_.CreateLoad(ptrTy(), emitExcSlot(), "si.postexc");
        emitPendingCheckIf(builder_.CreateICmpNE(preExc, postExc, "si.raised"));
    }
}


// #1553 / §22.13 (v0.181); #1309: true iff sfinit stores `f` at RUNTIME in
// textual order — a static-field initializer with a backing global that is NOT
// compile-time-baked. A readonly static's constant-literal initializer is
// already in the global (constInitFor), so it is always assigned and contributes
// no init-progress step. A MUTABLE static's literal initializer is NOT baked
// (#1309): it is runtime-stored in textual order, so it DOES contribute an
// init-progress step — and the partial-state read guard then fires on a forward
// reference to it from an earlier initializer (§22.13: "Partially-initialized
// statics are never observable"), instead of silently observing a baked value.
static bool isSfinitStoredStaticField(const sema::FieldSymbol* f,
    const std::unordered_map<const sema::Symbol*, llvm::GlobalVariable*>& globals) {
    if (!f) return false;
    bool isStatic = false;
    for (auto mod : f->modifiers) if (mod == "static") { isStatic = true; break; }
    if (!isStatic) return false;
    auto* fd = dynamic_cast<const ast::FieldDecl*>(f->decl);
    if (!fd || !fd->initializer) return false;
    const ast::Expr* e = fd->initializer.get();
    if (isBakedStaticFieldInit(f, e)) return false;  // readonly literal: baked
    return globals.count(f) != 0;
}

int Emitter::staticFieldInitIndex(const sema::FieldSymbol* fs) {
    if (!fs || !fs->parent) return -1;
    if (!isSfinitStoredStaticField(fs, staticGlobals_)) return -1;
    int idx = 0;
    for (const sema::Symbol* m : fs->parent->members) {
        auto* f = dynamic_cast<const sema::FieldSymbol*>(m);
        if (!f || !isSfinitStoredStaticField(f, staticGlobals_)) continue;
        if (f == fs) return idx;
        ++idx;
    }
    return -1;
}

void Emitter::emitSynthesizeCycleTIE(const std::string& typeName) {
    auto vit = builtinVtable_.find("TypeInitializationException");
    if (vit == builtinVtable_.end()) return;
    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
    llvm::Value* tie = builder_.CreateCall(
        callocFn(), {builder_.getInt64(1), builder_.getInt64(kExcObjSize)});
    builder_.CreateStore(vit->second, tie);  // vtable at offset 0
    // ASCII-only message (width-1 bz_string): names the re-entrant type per
    // §22.13 and prints cleanly on the uncaught-exception path.
    llvm::Value* msg = lowerStringLiteral(
        "\"The static initializer for type '" + typeName +
            "' was re-entered during its own initialization on the same "
            "thread (static-initialization cycle)\"",
        /*verbatim=*/false);
    builder_.CreateStore(
        msg, builder_.CreateGEP(i8Ty, tie, builder_.getInt64(kExcMsgOffset)));
    // InnerException at offset 16 stays null (calloc): no original exception.
    builder_.CreateStore(tie, emitExcSlot());
}

void Emitter::emitStaticPartialReadGuard(const sema::TypeSymbol* owner,
                                         const sema::FieldSymbol* fs) {
    if (!owner || !fs) return;
    auto stIt = initStateGV_.find(owner);
    if (stIt == initStateGV_.end()) return;   // no static state → no guard
    int idx = staticFieldInitIndex(fs);
    if (idx < 0) return;                        // always-assigned field → no guard
    auto ownIt = initOwnerGV_.find(owner);
    auto prIt  = initProgressGV_.find(owner);
    if (ownIt == initOwnerGV_.end() || prIt == initProgressGV_.end()) return;
    if (!currentFn_) return;

    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
    // Fast path: only an IN-PROGRESS type can expose a partially-initialized
    // static; a completed / failed / uninitialized state cannot, so the common
    // post-init read falls straight through.
    llvm::Value* st = builder_.CreateLoad(i8Ty, stIt->second, "pg.state");
    llvm::Value* isProg =
        builder_.CreateICmpEQ(st, llvm::ConstantInt::get(i8Ty, 1));
    auto* chkBB  = llvm::BasicBlock::Create(llvm_, "pg.chk",   currentFn_);
    auto* raiseBB = llvm::BasicBlock::Create(llvm_, "pg.raise", currentFn_);
    auto* contBB = llvm::BasicBlock::Create(llvm_, "pg.cont",  currentFn_);
    builder_.CreateCondBr(isProg, chkBB, contBB);

    // chk: raise only if this is the OWNER thread (same-thread re-entrancy; a
    // cross-domain in-progress touch is the §22.12 rule-1 blocking case, #1391 —
    // it proceeds here) AND the field has not been assigned yet (its index is at
    // or beyond how far sfinit has progressed).
    builder_.SetInsertPoint(chkBB);
    llvm::Value* ownerTid = builder_.CreateLoad(i64(), ownIt->second, "pg.owner");
    llvm::Value* self =
        builder_.CreateCall(declareC("bz_current_thread_id", i64(), {}), {},
                            "pg.self");
    llvm::Value* sameThread = builder_.CreateICmpEQ(ownerTid, self);
    llvm::Value* progress = builder_.CreateLoad(i32(), prIt->second, "pg.progress");
    llvm::Value* notAssigned = builder_.CreateICmpUGE(
        llvm::ConstantInt::get(i32(), static_cast<uint64_t>(idx)), progress);
    llvm::Value* shouldRaise = builder_.CreateAnd(sameThread, notAssigned);
    builder_.CreateCondBr(shouldRaise, raiseBB, contBB);

    // raise: synthesize the backstop TIE (naming the owner). It escapes via the
    // emitPendingCheck below, unwinds out of the in-progress initializer, and the
    // ordinary §22.7 failed-state machinery caches it — partial state never read.
    builder_.SetInsertPoint(raiseBB);
    emitSynthesizeCycleTIE(std::string(owner->name));
    builder_.CreateBr(contBB);

    builder_.SetInsertPoint(contBB);
    emitPendingCheck();  // route a raised TIE through the current unwind target
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
                   isOwnedSubscriptionField(fs) ||
                   // #1643 / §29.7: owned IEnumerable<T>/IOwningEnumerable<T>
                   // field's iterator frame needs cleanup (slot-1 dispose) if
                   // the constructor body throws after it was assigned.
                   isOwnedIteratorInterfaceField(fs) ||
                   // §6.5 / §24.3 (#1646): owned EventSource<T>/AsyncEventSource<T>
                   // field (user-constructed) also needs freeing if the ctor body
                   // throws after it was assigned.
                   isOwnedEventSourceField(fs))) {
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
    // §22.9 (v0.181): a `this(...)`-chaining constructor's BODY throw is a
    // different case from the ordinary base-anchored one this block otherwise
    // handles. By the time control can reach here for a this(...)-chaining
    // ctor, the chained-to constructor (this class's own base(...)-anchored
    // steps 0-2, including field initializers) has already run to completion
    // — "the instance is at that point fully initialized" — so the spec calls
    // for disposing it through the normal generated-Dispose() path (destructor
    // body + owned fields + base chain), not the narrower assigned-fields-only
    // cleanup an ordinary constructor body throw uses (the `else` branch
    // below, unchanged, per bullet 3, which does not run the destructor). We
    // call this class's own CORE disposer (disposerCoreFns_, NOT the public,
    // vtable-dispatched Dispose): a dynamic dispatch would resolve through the
    // actual most-derived vtable, which — when this constructor is itself
    // reached as a `base(...)` target from a more-derived class's constructor
    // (step 1 above) — would incorrectly run the more-derived class's
    // not-yet-initialized destructor over still-zeroed fields. The
    // statically-resolved core, scoped to exactly this class and its own real
    // base chain, is safe at every chain depth (mirrors emitBaseDisposeChain's
    // own use of the base's core for the identical reason).
    if (hasOwnedToClean || baseClassOf(cs->parent) ||
        (isThis && findDestructor(cs->parent))) {
        auto* savedInsert = builder_.GetInsertBlock();
        auto* cleanupBB =
            llvm::BasicBlock::Create(llvm_, "ctor.cleanup", currentFn_);
        builder_.SetInsertPoint(cleanupBB);
        if (isThis) {
            builder_.CreateCall(disposerCoreFns_[cs->parent], {currentThis_});
        } else {
            emitOwnedFieldDisposals(cs->parent);   // assigned owned fields (reverse)
            emitBaseDisposeChain(cs->parent);      // the base subobject
        }
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
                // #559: if the field is a hash-backed Set/OwningSet and
                // materializeHeapSetLiteral returned null (unrecognised element
                // type), do NOT fall through to the inline-array GEP path below —
                // that path treats the Set `ptr` field as a struct → malformed IR.
                if (isCollectionTypeName(fieldRt)) {
                    std::string_view cn =
                        fieldRt->astNode && fieldRt->astNode->name &&
                        !fieldRt->astNode->name->parts.empty()
                            ? fieldRt->headName()
                            : std::string_view{};
                    if (cn == "Set" || cn == "OwningSet")
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
                    if (!ev) {
                        // #1386: ICE — lowerExpr returned null for an instance
                        // field inline-array initializer element.
                        ctx_.diagnostics().report(
                            Severity::Error, cl->elements[i]->span.begin,
                            "internal: lowerExpr returned null for instance "
                            "field inline-array initializer element — #1386");
                        continue;
                    }
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
        if (!v) {
            // #1386: ICE — lowerExpr returned null for an instance
            // field/property initializer; the expression kind is unhandled.
            ctx_.diagnostics().report(
                Severity::Error, init->span.begin,
                "internal: lowerExpr returned null for instance "
                "field/property initializer — #1386");
            continue;
        }
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
            auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
            llvm::Value* msgVal = lowerExpr(*call->args[0].value);
            if (msgVal) {
                // message_ptr is at byte offset 8 — store directly via byte GEP.
                llvm::Value* msgSlot = builder_.CreateGEP(
                    i8Ty, currentThis_, builder_.getInt64(kExcMsgOffset));
                builder_.CreateStore(msgVal, msgSlot);
            }
            // §31.4 / #811: the two-argument base overloads, discriminated by
            // the second argument's static type exactly like the built-in
            // `new` path (LlvmBackend_Exprs.cpp): a `string` second argument
            // is ArgumentException's paramName (→ ParamName@32); an exception
            // second argument is the (message, cause) overload — snapshot the
            // borrowed cause into CauseChain@24. The user-subclass object
            // mirrors the full built-in header (classStruct), so these byte
            // offsets are in-bounds. The cause here is normally the user
            // ctor's own borrowed parameter — never disposed by this path.
            if (call->args.size() >= 2 && call->args[1].value) {
                const sema::TypeRef* a1ty = ctx_.exprType(*call->args[1].value);
                bool argIsString = a1ty &&
                    a1ty->kind == sema::TypeRef::Kind::Primitive &&
                    a1ty->primitive == sema::PrimitiveType::String;
                if (argIsString) {
                    llvm::Value* pn = lowerExpr(*call->args[1].value);
                    if (pn) {
                        llvm::Value* pnSlot = builder_.CreateGEP(
                            i8Ty, currentThis_,
                            builder_.getInt64(kExcParamOffset));
                        builder_.CreateStore(pn, pnSlot);
                    }
                } else {
                    llvm::Value* cause = lowerExpr(*call->args[1].value);
                    if (cause) {
                        bool hasHdr = isBuiltinExceptionType(a1ty) ||
                            (a1ty && a1ty->declaration &&
                             hasBuiltinExceptionBase(a1ty->declaration));
                        llvm::Value* chain = emitExcCauseSnapshot(
                            cause,
                            /*causeIsBuiltin=*/hasHdr);
                        llvm::Value* ccSlot = builder_.CreateGEP(
                            i8Ty, currentThis_,
                            builder_.getInt64(kExcCauseOffset));
                        builder_.CreateStore(chain, ccSlot);
                    }
                }
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
                            // #1605 / §4.6 / §28.9: same STRING positional-ctor-
                            // param field gap as the `new C(args)` site
                            // (LlvmBackend_Exprs2.cpp) — this `this(args)`
                            // delegation to the primary ctor stores straight into
                            // the same disposed FieldSymbol, so it needs the same
                            // independent +1 (retain unless the arg is a fresh
                            // +1 producer, transferred instead). Field is
                            // calloc-zeroed — no old value to release here.
                            if (v->getType()->isPointerTy() &&
                                isStringStorageMember(fs) &&
                                !isPlusOneStringProducer(call->args[pi].value.get()))
                                emitStringRetain(v);
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
        if (!v) {
            // #1386: ICE — lowerExpr returned null for a base-ctor argument;
            // the expression kind is not yet handled by codegen.
            ctx_.diagnostics().report(
                Severity::Error, a.value->span.begin,
                "internal: lowerExpr returned null for base-constructor "
                "argument — #1386");
            return;
        }
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
// the implicit base) FIRST, with no derived cleanup installed (§22.9 bullet 1:
// a chain-call throw is entirely the chained-to/base ctor's own responsibility
// — see #1555), then installs a constructor-cleanup block — routed to the
// enclosing unwind — as the propagate target for the remaining body/field-init
// steps only. For an ordinary (base-anchored) ctor the cleanup disposes
// assigned owned fields + the base subobject + frees `obj` (mirroring
// lowerNew's inline path and emitConstructorBody); for a `this(...)`-chaining
// ctor (§22.9 v0.181) it instead runs the fully-initialized instance's own
// generated-Dispose() core (destructor + owned fields + base chain) before
// freeing `obj`.
void Emitter::inlineCtorBody(sema::ConstructorSymbol* cs, llvm::Value* obj) {
    auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
    if (!cd) return;
    currentThis_ = obj;

    // §22.9 step 1 (v0.181): run the leading chain (base/this) — recursively
    // inlining a generic base/sibling ctor — BEFORE installing this class's
    // own cleanup block below. An exception during the chain call itself
    // "leaves no derived state to clean up" (§22.9 bullet 1): the chained-
    // to/base ctor's own exception-safety logic (its own inlineCtorBody frame,
    // or its compiled ctor's own cleanup) has already disposed and freed
    // whatever IT constructed, so this class must not repeat that disposal.
    // (Previously the cleanup block below was installed as unwindTarget_
    // BEFORE this call, so a `this(...)`-chain-call throw ALSO ran this
    // class's cleanup over the same object a second time — confirmed via a
    // `this()`-chaining generic ctor whose target threw during its own field
    // initializer: double free, since the target's own cleanup had already
    // freed `obj` and this class's cleanup unconditionally freed it again.)
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

    // Build the cleanup block now (after the chain call has completed),
    // capturing the enclosing unwind target, and install it as the propagate
    // target for the REST of construction — this class's own field
    // initializers (if any) and its body (§22.9 steps 2/3). `unwindTarget_` is
    // unchanged across the chain call above (any nested inlineCtorBody frame
    // saves/restores it), so capturing `enclosingUnwind` here is equivalent to
    // capturing it before the call.
    auto* savedInsert = builder_.GetInsertBlock();
    llvm::BasicBlock* enclosingUnwind = unwindBB();
    auto* cleanupBB =
        llvm::BasicBlock::Create(llvm_, "ctor.inline.cleanup", currentFn_);
    builder_.SetInsertPoint(cleanupBB);
    if (isThis) {
        // §22.8/§22.9 (v0.181): a `this(...)`-chaining constructor skips its
        // own field initializers (the chained-to ctor already ran them as its
        // step 2) — by the time ITS OWN body can throw, "the instance is at
        // that point fully initialized," so it is disposed through the normal
        // generated-Dispose() core (destructor body + owned fields + base
        // chain), not the narrower assigned-fields-only cleanup the `else`
        // branch below uses for an ordinary (base-anchored) ctor's field-init
        // or body throw. specializedDisposerCoreFor builds (cached) a core
        // disposer under the ACTIVE generic substitution: the shared,
        // unspecialized disposerCoreFns_[cs->parent] template has the wrong
        // field layout for a non-template-compatible T — the same reason
        // ctorFns_ isn't called directly here (see the #585 comment above) —
        // and mirrors emitBaseDisposeChain's own specialized-core use.
        builder_.CreateCall(specializedDisposerCoreFor(cs->parent), {obj});
    } else {
        emitOwnedFieldDisposals(cs->parent);
        emitBaseDisposeChain(cs->parent);
    }
    builder_.CreateCall(freeFn(), {obj});
    builder_.CreateBr(enclosingUnwind);
    if (savedInsert) builder_.SetInsertPoint(savedInsert);
    unwindTarget_ = cleanupBB;
    unwindScopeDepth_ = disposeStack_.size();

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
                        if (!v) {
                            // #1386: ICE — lowerExpr returned null for a
                            // positional-base-ctor argument.
                            ctx_.diagnostics().report(
                                Severity::Error, a.value->span.begin,
                                "internal: lowerExpr returned null for "
                                "positional base-constructor argument — #1386");
                            ok = false;
                            break;
                        }
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
        // §9 / §9.1 (#1759): `return this` in a RECORD method — `this` lowered
        // to the receiver POINTER (CG29 / §9.2), but a record method returns the
        // record STRUCT by value. Load the struct so the value-semantics copy is
        // returned, not the 8-byte pointer (which fails IR verification: `ret
        // ptr` vs `ret %record.T`). A no-op for every other returned value.
        v = coerceThisRecordValue(v, retTy);
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
        // collection (`Array<T>` / `OwningArray<T>` / Stack / Queue, §15.2) must
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
                    // #1871 / §28.9 / §28.11: reclaim the backing of an eligible
                    // borrowing-`Array<T>` LITERAL local at scope end
                    // (bz_array_free(backing, null)). `Array<T>` is trivial-dispose
                    // (no-op Dispose) but its backing storage is "reclaimed with the
                    // instance" (§28.9/§28.11) — here a compiler-emitted scope-end
                    // free. This literal path early-returns, bypassing the shared
                    // #1003 reclaim block at the end of lowerLocalDecl, so register
                    // it directly. Gated on borrowingArrayFreeEligible_
                    // (collectBorrowingArrayFreeEligible proved the local never
                    // escapes AND its element policy is backing-only — a null
                    // per-element disposer), so the free is exactly-once (no UAF, no
                    // double-free). OwningArray/Stack/Queue literals are NOT in the
                    // set (only name-head "Array" with a null element disposer is
                    // collected), so their owned disposal path is untouched.
                    if (currentFn_ == borrowingArrayFreeEligibleFn_ &&
                        borrowingArrayFreeEligible_.count(
                            static_cast<const void*>(declNode)) &&
                        !disposeStack_.empty()) {
                        Disposal d{slot, nullptr};
                        d.collection = true;  // per-element disposer null
                        disposeStack_.back().push_back(d);
                    }
                    return;
                }
            }
        }
        // #534 / stdlib §4.8: dictionary literal `["k1":v1, "k2":v2, …]` — allocate
        // and populate a Dictionary<K,V> (or OwningDictionary<K,V>) via the SAME
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
        // #559: if the declared type is a hash-backed Set/OwningSet and
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
                            ? collTy->headName()
                            : std::string_view{};
                    if (cn == "Set" || cn == "OwningSet")
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
                if (!ev) {
                    // #1386: ICE — lowerExpr returned null for a local-decl
                    // inline-array initializer element.
                    ctx_.diagnostics().report(
                        Severity::Error, cl->elements[i]->span.begin,
                        "internal: lowerExpr returned null for local-decl "
                        "inline-array initializer element — #1386");
                    continue;
                }
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
    // §9 / §9.1 (#1759): `var q := this` in a record method copies the record
    // value. `this` lowered to the receiver POINTER (CG29 / §9.2); load the
    // struct so the local holds a value-semantics copy, not the pointer bits — a
    // `var` local would otherwise infer a `ptr` slot from currentThis_'s type
    // and read `q.field` as garbage. A no-op for every initializer that is not
    // the receiver pointer (coerceThisRecordValue's own guards).
    if (v)
        if (const sema::TypeSymbol* rec = currentReceiverRecordType())
            v = coerceThisRecordValue(v, recordStruct(rec));
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
            declTR->headName() == "IEnumerable") {
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
            // bz_array_t-backed owning collections — OwningArray, OwningBuffer,
            // Stack/OwningStack, Queue/OwningQueue. A local declared as
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
                (initTR->headName() == "Array" ||
                 initTR->headName() == "OwningArray" ||
                 initTR->headName() == "OwningBuffer" ||
                 initTR->headName() == "Stack" ||
                 initTR->headName() == "OwningStack" ||
                 initTR->headName() == "Queue" ||
                 initTR->headName() == "OwningQueue");
            if (initIsArray) {
                // §29 / §12.9 (#1850): a nested covariant view whose elements
                // are themselves array-backed collections viewed as pure
                // IEnumerable<T> (Array<Array<Dog>> → IEnumerable<IEnumerable<Dog>>)
                // needs the per-element re-wrapping nested enumerator — the plain
                // wrapper hands out raw bz_array_t* elements the inner for-in then
                // vtable-dispatches on → SIGSEGV.
                if (nestedIEnumViewNeeded(initTR, declTR))
                    v = wrapNestedArrayAsIEnumerable(v, initTR, declTR);
                else
                    v = wrapArrayAsIEnumerable(v, declTR);
            } else if (initTR && isLinkedListTypeName(initTR)) {
                // #1177: (Owned)LinkedList<T> is node-based (bz_llist) — wrap
                // with the node-walking enumerator, mirroring buildOrderedArgs.
                v = wrapLinkedListAsIEnumerable(v, declTR);
            } else if (initTR && isLazyEnumerableType(initTR)) {
                // #1911 / stdlib §5 / §29.11: a lazy LINQ pipeline (Select/Where/…
                // → a bz_lazy_t*) bound to an EXPLICIT `IEnumerable<T>` local. The
                // lazy vtable has only 4 slots (td/Dispose/MoveNext/Current) and
                // its Current returns a POINTER to the element, whereas the for-in
                // over an IEnumerable<T> local drives the §29.11 iterator vtable —
                // it reads slot 4 (GetEnumerator, OUT OF BOUNDS for the 4-slot lazy
                // vtable → a garbage function-pointer call → SIGSEGV) and expects
                // Current BY VALUE. Drain the pipeline once into a fresh owned
                // Array<T> (bz_lazy_to_array also disposes the pipeline, releasing
                // its inline-lambda closures) and wrap that array in an OWNING
                // vtable-backed enumerator: the for-in's slot-4/Current protocol is
                // then satisfied, and scope-end iterEnumDispose
                // (registerIterableSourceDisposal) frees the drained array exactly
                // once (leak-free). An INFERRED local (`var xs := xs.Select(…)`)
                // keeps its concrete lazy type and iterates via lowerLazyLinqFor —
                // only the interface-widened binding needs this bridge. The
                // drain+owning-wrap is shared with the RETURN variant (#1916).
                v = drainLazyPipelineToOwnedIEnumerable(v, declTR);
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
    // #1619 / §4.7: the unit type `()` has exactly one value and lowers to
    // LLVM `void` (lowerTypeRef's Kind::Unit case, LlvmBackend_Layout.cpp) —
    // correct when `void` names a function's RETURN type, but `void` is not
    // a valid `alloca` operand: LLVM's DataLayout has no size/alignment case
    // for it, so IRBuilder::CreateAlloca(void) hits an internal
    // `llvm_unreachable` that this libLLVM build compiles to a hardware trap
    // (SIGILL) rather than an assertion message. `slotTy` reaches here as
    // `void` whenever a local is declared with unit type — explicitly
    // (`var u: () := ();`) or via type inference from a unit-typed
    // initializer (the `!v && !declaredTy` branch above re-derives `void`
    // from the initializer's sema type). A unit-typed local has no data to
    // store — substitute a one-byte placeholder slot; its contents are never
    // meaningfully read (there is only one unit value, so nothing
    // distinguishes one placeholder byte pattern from another).
    if (slotTy->isVoidTy()) slotTy = i8();
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
    // §28.3 / §28.10 (v0.180, #1978): a `var y := owned x;` / `using var y :=
    // owned x;` binding marker (#1962) MOVES a named owned source into this new
    // binding. NULL the source slot at the transfer — exactly as the #1918
    // user-method / #1955 ctor arg-lowering transfers do — so the null-guarded
    // unwindOnly disposal is exactly-once for the source `x` (which
    // UnwindIneligibleWalker now leaves ELIGIBLE, #1978): a throw that PRECEDES
    // this binding still finds `x` live and disposes it once (fixes the #1978
    // leak), while a throw AFTER the transfer finds the null source slot and
    // skips it (no double-free with the new binding's own unwind disposal).
    // nullTransferredOwnedLocalSlot self-gates to exactly the slots the walker
    // keeps eligible (a plain `var` owned local or an `owned` parameter), so the
    // `eligible ⟹ nulled` invariant stays EXACT; every other initializer is a
    // no-op here. // per §28.3 v0.180 #1978
    if (v && init)
        if (auto* ldNode = dynamic_cast<const ast::LocalDecl*>(declNode))
            if (ldNode->initializerHasOwnedMarker)
                nullTransferredOwnedLocalSlot(init);
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
    // #1930 / stdlib §5 / §28.9: a plain `var g := <src>.GroupBy(sel).First()/…`
    // (a bare-GroupBy element-returning terminal) proven non-escaping holds the SOLE
    // reference to the returned grouping — the terminal already reclaimed the sibling
    // groups + the outer array (bz_groupby_free_groups_except), keeping only this
    // one. Reclaim it at scope end via bz_grouping_free (the grouping analog of the
    // #1003 array reclaim / #1028 dict reclaim above). The escape walk excludes any
    // returned (statically a borrow → rejected)/captured/added-to-collection/aliased
    // local, so the free is exactly-once (no UAF, no double-free with an owner that
    // took the grouping). bz_grouping_free is null-safe (an *OrNull empty result is
    // null). Gated on currentFn_ matching the analyzed function (a lambda/thunk body
    // inherits no eligibility → the grouping just leaks, never double-freed).
    if (v && currentFn_ == groupingFreeEligibleFn_ &&
        groupingFreeEligible_.count(static_cast<const void*>(declNode)) &&
        !disposeStack_.empty()) {
        Disposal d{slot, nullptr};
        d.grouping = true;
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
    // "IOwningEnumerable" — the only declaration-less iteration types the for-in
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
        name = tr->headName();
    else if (!tr->importedName.empty())
        name = tr->importedName;
    if (name != "IEnumerable" && name != "IOwningEnumerable")
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
            declTR->headName() == "IEnumerable";
        if (declIsPureIEnum && initTR &&
            initTR->kind == sema::TypeRef::Kind::Named) {
            bool initIsArray = initTR->astNode && initTR->astNode->name &&
                !initTR->astNode->name->parts.empty() &&
                initTR->headName() == "Array";
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

// §30.5 / #1258 / #1868: an OwningTask<T:class> owns its produced result. When
// the task is dropped at scope end without an intervening await taking the
// result (bz_task_take_result zeros it on consume), the produced owned class
// result would leak. Attach the per-T disposer thunk (the caller knows the
// static OwningTask<T> type) so bz_task_free disposes it — whether bz_task_free
// runs synchronously (already-done task) or later via the fire-and-forget
// completion continuation. Shared by BOTH the `using var` task registration and
// the plain-`var` auto-drop task registration (lowerStmt LocalDecl, SB01-C1/D2
// #1786/#1868) so the two paths cannot drift — the drift that leaked a plain
// `var t := Producer()` OwningTask<T:class> auto-dropped across a suspend. The
// `taskName` builtin-identity check (declaration null, or a null-`decl` builtin
// TypeSymbol) means a USER class named `OwningTask` — which has a declaration
// WITH a decl — yields no disposer, exactly as it must. No-op for a
// channel-receive task (the abandon path disposes no produced item) and for a
// value-typed / non-OwningTask result. // per §30.5, §28.3 (v0.190)
void Emitter::attachOwningTaskResultDisposer(Disposal& d,
                                             const sema::TypeRef* taskTy) {
    if (d.channelReceiveTask || !taskTy) return;
    auto taskName = [](const sema::TypeRef* rt) -> std::string_view {
        if (!rt->declaration) {
            if (rt->astNode && rt->astNode->name &&
                !rt->astNode->name->parts.empty())
                return rt->headName();
        } else if (!rt->declaration->decl) {
            return rt->declaration->name;
        }
        return {};
    };
    if (taskName(taskTy) == "OwningTask" && !taskTy->genericArgs.empty() &&
        isClassOrInterfaceRef(taskTy->genericArgs[0])) {
        d.taskOwnedResultDisposer =
            elementPolicyFor(taskTy->genericArgs[0], /*owningFamily=*/true)
                .disposerThunk;
    }
}

void Emitter::registerUsingDisposal(const ast::LocalDecl* ld) {
    if (disposeStack_.empty()) return;
    auto it = locals_.find(ld);
    if (it == locals_.end()) return;

    // §28.10 / §28.3 (v0.184, #1661): a `using var` the ownership pass flagged a
    // BORROW-PIN registers NO disposal — its real owner disposes it. This holds
    // for a borrow-local alias, a borrowing-loop element, and (v0.184) a
    // borrowed-meet if/case expression (`using var x: Res := b1 if c else b2;`).
    // A TYPED borrow-pin otherwise reaches classOfLocal below, which returns the
    // declared class regardless of ownership — a double-free of the borrow at
    // scope end. Honor the flag uniformly here so no disposal path can dispose a
    // pin. (The untyped bare-identifier pin was already handled inside
    // classOfLocal; this generalizes it to the typed and if/case forms.)
    if (ld->usingVarSourceIsBorrow) return;

    // §29.11 (#943): a sync `using var source := P.Seq();` whose value is a
    // vtable-backed re-iterable IEnumerable<T>/IOwningEnumerable<T> (yield-iterator
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
        // whose value is the builtin IEnumerator<T>/IOwningEnumerator<T> sentinel
        // (an iterator-vtable enumerator, slot-1 Dispose). Without this the
        // enumerator struct leaks at scope end (the generic dispose paths below
        // do not recognise the declaration-less iterator type). Its slot-1
        // Dispose frees only the wrapper struct (it borrows the array), so this
        // is exactly-once and never double-frees the separately owned array.
        bool isSyncIterEnum =
            lt && lt->kind == sema::TypeRef::Kind::Named && !lt->declaration &&
            lt->astNode && lt->astNode->name &&
            !lt->astNode->name->parts.empty() &&
            (lt->headName() == "IEnumerable" ||
             lt->headName() == "IOwningEnumerable" ||
             lt->headName() == "IEnumerator" ||
             lt->headName() == "IOwningEnumerator");
        if (isSyncIterEnum) {
            const sema::TypeRef* initTR = ctx_.exprType(*ld->initializer);
            bool wrappedHere = initTR && initTR->kind == sema::TypeRef::Kind::Named &&
                ((initTR->astNode && initTR->astNode->name &&
                  !initTR->astNode->name->parts.empty() &&
                  initTR->headName() == "Array") ||
                 initTR->declaration != nullptr);
            bool ownsSource =
                dynamic_cast<const ast::CallExpr*>(ld->initializer.get()) ||
                dynamic_cast<const ast::NewExpr*>(ld->initializer.get()) ||
                wrappedHere;
            // §29.11 (#1294): `using var e := src.GetEnumerator()` where `src` is a
            // NAMED value (IdentifierExpr) and GetEnumerator on a vtable-backed
            // IEnumerable returns the RECEIVER IDENTITY (LlvmBackend_Calls.cpp
            // #1102C) — so `e` ALIASES `src`. If `src` is a separately-disposed
            // owned local, registering a second disposal here double-frees the
            // same struct (SIGSEGV). Treat it as a bare alias — the named receiver
            // is the sole disposer. The receiver-is-a-TEMPORARY form
            // `Gen().GetEnumerator()` has no named owner, so `e` still owns it (its
            // receiver is a CallExpr, not an IdentifierExpr) and is unaffected.
            if (ownsSource) {
                if (auto* ce = dynamic_cast<const ast::CallExpr*>(
                        ld->initializer.get())) {
                    if (auto* gma = dynamic_cast<const ast::MemberAccessExpr*>(
                            ce->callee.get())) {
                        if (gma->member == "GetEnumerator" && gma->target &&
                            dynamic_cast<const ast::IdentifierExpr*>(
                                gma->target.get()) &&
                            isVtableBackedIEnumerableType(
                                ctx_.exprType(*gma->target)))
                            ownsSource = false;
                    }
                }
            }
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

    // stdlib §6.10 (v0.175, #808): an `await using var` (or, defensively, a
    // `using var` that slipped past the ownership §6.10 reject) bound to a
    // TaskScope. Recognised BEFORE the generic isAwaitUsing branch below —
    // TaskScope has no user TypeSymbol/DisposeAsync MethodSymbol, so the
    // generic branch's sync fallback would vtable-dispose a bz_task_scope_t
    // (garbage). Disposal is emitted by the dedicated d.taskScope arm:
    // normal exit → bz_task_scope_dispose_async + await (cancel, drain,
    // swallow; IOE when Join was never awaited); unwind → the non-blocking
    // bz_task_scope_dispose_unwind zombie drain.
    {
        auto isTaskScopeName = [](const sema::TypeRef* rt) {
            if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
            if (rt->declaration)
                return !rt->declaration->decl &&
                       rt->declaration->name == "TaskScope";
            if (rt->astNode && rt->astNode->name &&
                !rt->astNode->name->parts.empty())
                return rt->headName() == "TaskScope";
            return rt->importedName == "TaskScope";
        };
        bool isTS = false;
        if (ld->type) isTS = isTaskScopeName(ctx_.resolvedType(*ld->type));
        if (!isTS && ld->initializer) {
            isTS = isTaskScopeName(ctx_.exprType(*ld->initializer));
            if (!isTS) {
                if (auto* ne = dynamic_cast<const ast::NewExpr*>(
                        ld->initializer.get()))
                    isTS = ne->type && ne->type->name &&
                           !ne->type->name->parts.empty() &&
                           ne->type->name->parts[0] == "TaskScope";
            }
        }
        if (isTS) {
            Disposal d{it->second, nullptr};
            d.taskScope = true;
            disposeStack_.back().push_back(d);
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
        // (IAsyncEnumerator<T> / IOwningAsyncEnumerator<T>) has the iterator vtable
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
                    (lt->headName() == "IAsyncEnumerator" ||
                     lt->headName() == "IOwningAsyncEnumerator");
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
                   rt->headName() == nm;
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
                   rt->headName() == nm;
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
                    rt->headName() == "CancellationTokenSource";
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
                   rt->headName() == "Mutex";
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
                   rt->headName() == "Lock";
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
                   rt->headName() == "Semaphore";
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
                   rt->headName() == "SemaphoreToken";
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
                   rt->headName() == "Channel";
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
                   rt->headName() == "ChannelSender";
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
                   rt->headName() == "Shared";
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
                   rt->headName() == "Weak";
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
                   rt->headName() == "Buffer";
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
    // §6.6 / issue #974: a `using var` bound to a TaskCompletionSource<T>,
    // OwningTaskCompletionSource<T>, or TaskCompletionSource (non-generic).
    // No user TypeSymbol; recognised by builtin type name. At scope end:
    //   1. bz_tcs_fault_if_pending(tcs, ioe_vtable) — faults the inner task with
    //      InvalidOperationException if not yet completed (so no awaiter hangs).
    //   2. Error-object cleanup on the inner task (mirrors d.task path).
    //   3. bz_task_fire_and_forget(inner_task) — releases the task when done.
    //   4. bz_tcs_free(tcs) — frees the TCS struct itself.
    // Detection: declared type OR NewExpr type name (both forms appear in practice).
    {
        bool isTcs = false;
        auto isTcsTypeName = [](const sema::TypeRef* rt) {
            if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
            if (rt->declaration) return false;
            if (!rt->astNode || !rt->astNode->name ||
                rt->astNode->name->parts.empty()) return false;
            std::string_view n = rt->headName();
            return n == "TaskCompletionSource" ||
                   n == "OwningTaskCompletionSource";
        };
        if (ld->type)
            isTcs = isTcsTypeName(ctx_.resolvedType(*ld->type));
        if (!isTcs && ld->initializer) {
            if (auto* ne =
                    dynamic_cast<const ast::NewExpr*>(ld->initializer.get()))
                isTcs = ne->type && ne->type->name &&
                        !ne->type->name->parts.empty() &&
                        (ne->type->name->parts[0] == "TaskCompletionSource" ||
                         ne->type->name->parts[0] == "OwningTaskCompletionSource");
        }
        if (!isTcs && ld->initializer)
            isTcs = isTcsTypeName(ctx_.exprType(*ld->initializer));
        if (isTcs) {
            Disposal d{it->second, nullptr};
            d.tcs = true;
            disposeStack_.back().push_back(d);
            return;
        }
    }
    // stdlib §6.7 / §30.5: a `using var` bound to a Task / Task<T> / OwningTask<T>.
    // Tasks have no user TypeSymbol; at scope end the task is freed via bz_task_free.
    // Double-free safety: lowerAwait only frees tasks whose operand is an anonymous
    // CallExpr (e.g. `await Producer()`), never a named local (`await t`) — so a
    // using-var-bound task is freed exactly once here at scope end.
    // Recognised by the initializer's inferred expression type name "Task" or
    // "OwningTask", or by the declared type name.
    if (const sema::TypeRef* tt = taskTypeOfLocal(ld)) {
        Disposal d{it->second, nullptr};
        d.task = true;
        // §30.15 (v0.179, #1524): a Receive() task overrides the ordinary
        // disposal below with the channel abandonment give-back protocol — see
        // the field's doc comment (LlvmBackend_impl.hpp) and
        // bz_channel_receive_abandon. `taskOwnedResultDisposer` is deliberately
        // left unset in this branch: the abandon path never disposes a
        // produced item, so no per-T disposer thunk is needed.
        d.channelReceiveTask = isChannelReceiveInitializer(ld);
        // §30.5 / #1258: an OwningTask<T:class> owns its produced result. If the
        // task is dropped at scope end without an intervening await taking the
        // result (bz_task_take_result zeros it on consume), the produced owned
        // class result would leak. Attach the per-T disposer (shared with the
        // plain-`var` auto-drop path so the two cannot drift, #1868).
        attachOwningTaskResultDisposer(d, tt);
        disposeStack_.back().push_back(d);
        return;
    }
    // #362 / stdlib §4.5: a `using var` bound to a heap collection
    // (Array<T>/OwningArray<T>/Dictionary/Set/…). These are builtins with no
    // user TypeSymbol, so classOfLocal returns null; recognize them by name and
    // register a collection disposal. For an OWNING collection of class
    // elements the disposal cascades into the elements (the leak fixed here);
    // a borrowing collection or value elements free only the backing.
    // per §4.6/§4.7: a `using var` LinkedList<T>/OwningLinkedList<T>. Check BEFORE
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
                common::isOwningPrefixedName(llTy->headName());
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
                common::isOwningPrefixedName(collTy->headName());
            bool isSetName =
                collTy->astNode && collTy->astNode->name &&
                !collTy->astNode->name->parts.empty() &&
                (collTy->headName() == "Set" ||
                 collTy->headName() == "OwningSet" ||
                 // stdlib §4.18 (#1426): SortedSet shares the bz_dict_t* rep.
                 collTy->headName() == "SortedSet");
            // #1012 / §4.10: a Set/OwningSet is DICT-BACKED whenever its element is
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
    // hash-backed dictionary (Dictionary<K,V>/OwningDictionary<K,V>). Dicts are
    // excluded from isCollectionTypeName (they free via bz_dict_free, not
    // bz_array_free), so they get their own disposal kind here. The per-value
    // callback is one of exactly three mutually exclusive things (or null):
    //   * #394: an OwningDictionary<K, class/interface> → cascade disposal into
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
            dictTy->headName() == "OwningDictionary";
        bool isSetNameD =
            dictTy->astNode && dictTy->astNode->name &&
            !dictTy->astNode->name->parts.empty() &&
            dictTy->headName() == "Set";
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
    // "DrainedEntry" (synthetic TypeRef set by sema for OwningDictionary.Drain
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
    // #1185 / stdlib §3.1: a `using var` bound to a StringBuilder. The builder is
    // a bz_array_t* of string parts and OWNS a +1 per part (Append/ctor retain or
    // transfer it). Its value is the array pointer itself (a heap-pointer builtin
    // with no user TypeSymbol, so classOfLocal returns null below and every other
    // special case misses it — without this it leaked the whole parts array).
    // Disposal reuses the borrowing-Array<string> path: bz_array_free with the
    // string-element release thunk, which bz_string_release's each part (null-/
    // sentinel-safe) and frees the backing array. Mutators that flatten-and-reset
    // (Insert/Remove/Replace) release the old parts themselves (bz_sb_reset_to).
    {
        auto isSbName = [](const sema::TypeRef* rt) {
            return rt && rt->kind == sema::TypeRef::Kind::Named &&
                   !rt->declaration && rt->astNode && rt->astNode->name &&
                   !rt->astNode->name->parts.empty() &&
                   rt->headName() == "StringBuilder";
        };
        bool isSB = false;
        if (ld->type) isSB = isSbName(ctx_.resolvedType(*ld->type));
        if (!isSB && ld->initializer) {
            if (auto* ne = dynamic_cast<const ast::NewExpr*>(
                    ld->initializer.get()))
                isSB = ne->type && ne->type->name &&
                       !ne->type->name->parts.empty() &&
                       ne->type->name->parts[0] == "StringBuilder";
            if (!isSB) isSB = isSbName(ctx_.exprType(*ld->initializer));
        }
        if (isSB) {
            Disposal d{it->second, nullptr};
            d.collection = true;
            d.collectionElemDisposer = stringElementReleaseThunk();
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


bool Emitter::isBuiltinOwnedTransferCall(const ast::CallExpr& call) {
    // #1414 stage 4 phase A: the codegen-side consumer of the shared ownership-
    // facts core. The builtin owned-TRANSFER expression-shape classification
    // (#1349/#642) now lives in common::isBuiltinOwnedTransferCall; behaviour is
    // byte-identical to the pre-extraction predicate.
    return common::isBuiltinOwnedTransferCall(call, ctx_);
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
    // (a0) §28.3 arm-meet (v0.184, #1661): a plain `var x := new T() if c else
    // Make();` (owned-meet if/case). The initializer is neither a NewExpr nor a
    // call, so (a)/(b) below miss it — the owned value was never reclaimed on an
    // explicit `x.Dispose()` (the destructor ran, the block leaked — the LSan
    // finding) nor disposed on an exception unwind. Recognize the owned-meet via
    // the SAME shared predicate the ownership pass (producerState) consults and
    // route it through the dynamic dispose+reclaim, exactly like an owned `new`/
    // call result — exactly one arm runs, so disposal is exactly-once. A
    // borrowed-meet (predicate false) carries no obligation and a mixed set is
    // rejected in sema, so neither registers here. // per §28.3
    if ((dynamic_cast<const ast::ConditionalExpr*>(ld->initializer.get()) ||
         dynamic_cast<const ast::CaseExpr*>(ld->initializer.get())) &&
        common::conditionalCaseArmMeetIsOwned(
            *ld->initializer, ctx_,
            [this](const sema::TypeRef* t) { return resolveTypeParamRef(t); })) {
        const sema::TypeRef* initTy = ctx_.exprType(*ld->initializer);
        if (isClassOrInterfaceRef(initTy) ||
            isClassOrInterfaceRef(resolveTypeParamRef(initTy))) {
            if (initTy && initTy->kind == sema::TypeRef::Kind::Named &&
                initTy->declaration &&
                initTy->declaration->kind == ast::TypeDeclKind::Class)
                return initTy->declaration;
            return reinterpret_cast<const sema::TypeSymbol*>(1);
        }
        return nullptr;
    }
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
                // (b2u) §28.3 "Factory methods and sourceless returns" /
                // Appendix C.3 (fix #1499): an unannotated STATIC method whose
                // EVERY return is a fresh owned producer is treated as OWNED by
                // the caller — mirrors the sema producerState classification
                // and the usingVarOwnsClassObjectDynamically (b2u) mirror below.
                // Covers the exception-unwind edge for a plain `var` bound to
                // such a call and discharged on only SOME paths. A borrow-
                // returning static method (`Get() => _shared`) fails the
                // every-return-fresh check and is NOT disposed (§28.10).
                if (md && !isOwned) {
                    const sema::TypeRef* initTy =
                        ctx_.exprType(*ld->initializer);
                    if (common::staticFactoryReturnsFreshOwned(
                            *md, initTy, ctx_,
                            [this](const sema::TypeRef* t) {
                                return resolveTypeParamRef(t);
                            })) {
                        if (initTy &&
                            initTy->kind == sema::TypeRef::Kind::Named &&
                            initTy->declaration &&
                            initTy->declaration->kind ==
                                ast::TypeDeclKind::Class)
                            return initTy->declaration;
                        return reinterpret_cast<const sema::TypeSymbol*>(1);
                    }
                }
            }
            // (b3) #1349 / #642 / §28.3 / §28.11: builtin owned-TRANSFER calls
            // (Pop/Dequeue on owning stacks/queues, PopLast/PopFirst/PopAt/
            // PopValue, T.Deserialize, TakeCurrent on owning enumerators) have
            // no MethodSymbol, so (b) above missed them: a plain
            // `var x := s.Pop()` was neither reclaimed by an explicit
            // `x.Dispose()` (destructor dispatched, block leaked — found by the
            // #1349 merge-gate ASan probe) nor disposed on an exception unwind,
            // and §28.7 drop-on-overwrite skipped it. Mirrors the using-var
            // allowlist (usingVarOwnsClassObjectDynamically b2).
            if (!ms && isBuiltinOwnedTransferCall(*call)) {
                const sema::TypeRef* initTy = ctx_.exprType(*ld->initializer);
                if (isClassOrInterfaceRef(initTy)) {
                    if (initTy->kind == sema::TypeRef::Kind::Named &&
                        initTy->declaration &&
                        initTy->declaration->kind == ast::TypeDeclKind::Class)
                        return initTy->declaration;
                    return reinterpret_cast<const sema::TypeSymbol*>(1);
                }
            }
        }
    }
    // §28.7 / §28.3 (#1625): a null-INITIALIZED plain local (`var x: T? := null;`)
    // that BECOMES owned via reassignment is an owned class/interface local for
    // disposal purposes, even though its initializer (`null`) is not itself an
    // owned producer. The ownership pass sets becomesOwnedViaReassign ONLY for a
    // null-initialized local (handleAssignment) on its first Owned reassignment.
    // The restriction is load-bearing: a BORROW-initialized local must NOT be
    // classified as owned here — disposeDisplacedOwnedClassLocal would otherwise
    // dispose the displaced BORROW on the first owned assignment (a double-free;
    // the exclusion that helper documents) — and a no-initializer local returns
    // null at the top of this function (the `!ld->initializer` gate). A null-
    // initialized slot starts null, so treating it as owned for drop-on-overwrite
    // (§28.7) and scope-end/unwind disposal (§28.3) is sound: the disposal is
    // dynamic (vtable) and null-guarded, so a slot still null or moved-from at
    // the dispose site disposes nothing. The state-match rule (§28.3) guarantees
    // such a local is never reassigned a borrow. Recover the class from the
    // declared type (a nullable `T?` resolves to T with nullable=true).
    // Collections/wrappers/tasks/subscriptions were excluded above.
    if (ld->becomesOwnedViaReassign) {
        const sema::TypeSymbol* cls = classOfLocal(ld);
        if (cls && cls->kind == ast::TypeDeclKind::Class &&
            disposerFns_.find(cls) != disposerFns_.end())
            return cls;
        // An INTERFACE-typed nullable local (`var x: IThing? := null;
        // x := new C();`): classOfLocal returns the interface symbol (no
        // disposerFns_ entry), but the concrete object behind it has a vtable,
        // so dispose dynamically. Mirrors the interface handling in the `new`
        // branch above and usingVarOwnsClassObjectDynamically (a).
        if (cls && cls->kind == ast::TypeDeclKind::Interface)
            return reinterpret_cast<const sema::TypeSymbol*>(1);
        // A declared class/interface type whose declaration did not resolve
        // (a generic type parameter under the active substitution): resolve
        // through genericSubstSemaTy_, else dispose dynamically when the
        // declared type is recognisably a class/interface.
        if (ld->type) {
            const sema::TypeRef* rt = ctx_.resolvedType(*ld->type);
            const sema::TypeRef* rrt = resolveTypeParamRef(rt);
            if (isClassOrInterfaceRef(rt) || isClassOrInterfaceRef(rrt))
                return reinterpret_cast<const sema::TypeSymbol*>(1);
        }
    }
    // §28.3 / §28.10 (v0.180, #1962): a plain `var y := owned x;` — the binding-
    // position transfer marker. The initializer is a bare IdentifierExpr (the
    // named owned source), so the (a)/(b)/(a2) shape checks above miss it, exactly
    // as they miss `var y := move x;`. When the binding is discharged on some path
    // (so the ownership pass did NOT flag autoDropAtScopeEnd and the auto-drop
    // registerUsingDisposal is not used) it must still be disposed on the
    // exception-unwind edge that PRECEDES the discharge — otherwise the moved-in
    // value leaks on a throw. The source `x` is left moved-from (marked ineligible
    // by UnwindIneligibleWalker, which walks `var y := x` as a consume of x), so
    // `y` is the sole owner: an unwindOnly dynamic dispose here is exactly-once.
    // Recover the class from the initializer's inferred type (the source's type);
    // dispatch is dynamic (vtable) and null-guarded regardless. // per §28.3 v0.180
    if (ld->initializerHasOwnedMarker &&
        dynamic_cast<const ast::IdentifierExpr*>(ld->initializer.get())) {
        const sema::TypeRef* initTy = ctx_.exprType(*ld->initializer);
        const sema::TypeRef* rInit = resolveTypeParamRef(initTy);
        if (isClassOrInterfaceRef(initTy) || isClassOrInterfaceRef(rInit)) {
            if (initTy && initTy->kind == sema::TypeRef::Kind::Named &&
                initTy->declaration &&
                initTy->declaration->kind == ast::TypeDeclKind::Class &&
                disposerFns_.find(initTy->declaration) != disposerFns_.end())
                return initTy->declaration;
            return reinterpret_cast<const sema::TypeSymbol*>(1);
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
    // Must be a class/interface type to have a Dispose to call. fix #1483 /
    // §28.3: `rt` alone misses an `owned t: T` parameter of a generic method,
    // where T is a bare (erased) type parameter — rt->declaration is null and
    // isClassOrInterfaceRef(rt) is false even though the ACTIVE substitution
    // (genericSubst_/genericSubstSemaTy_) binds T to a concrete class/interface
    // for this specialization. Fall back through resolveTypeParamRef, mirroring
    // the other disposal-registration sites that already handle this case
    // (usingVarOwnsClassObjectMoveInit and the local-variable tracking near
    // line 6640). The Disposal below carries `cls = nullptr` regardless (the
    // `dynamicDispose`/`unwindOnly` path already dispatches through the live
    // object's own vtable, exactly as it does for an interface-typed slot), so
    // no further plumbing of the resolved type is needed here.
    const sema::TypeRef* rt =
        ps->declType ? ctx_.resolvedType(*ps->declType) : ps->resolvedDeclType;
    // §30.15 / §28.13 (#1957): a builtin ChannelSender<T> / Channel<T> /
    // Shared<T> owned parameter has NO user TypeSymbol (rt->declaration == null),
    // so isClassOrInterfaceRef below rejects it and the disposal registration is
    // skipped entirely — the auto-dropped owned sender is never released, its
    // channel stays open (it was the last sender, §30.15), the graph leaks, and a
    // cross-domain producer deadlocks. Detect these compiler-known builtins by
    // head name — exactly the using-var path (usingVarOwns…) — and register their
    // TYPE-SPECIFIC runtime release (bz_channel_sender_release / bz_channel_dispose
    // / bz_shared_release) instead of the generic vtable dispatch, which does not
    // touch the sender/channel/handle refcount and cannot close the channel. This
    // must run BEFORE the class/interface gate. The auto-drop case (§28.3, a
    // never-discharged owner) releases on BOTH exits (the sole release); a
    // discharged owned param (moved/returned/disposed on the normal path)
    // releases only on unwind (releaseOnUnwindOnly), mirroring the generic
    // unwindOnly owned-param path — the discharge site owns the normal exit.
    {
        auto builtinHead = [](const sema::TypeRef* r) -> std::string_view {
            if (!r || r->kind != sema::TypeRef::Kind::Named || r->declaration ||
                !r->astNode || !r->astNode->name || r->astNode->name->parts.empty())
                return std::string_view{};
            return r->headName();
        };
        std::string_view bhead = builtinHead(rt);
        bool isTx = bhead == "ChannelSender";
        bool isCh = bhead == "Channel";
        bool isSh = bhead == "Shared";
        if (isTx || isCh || isSh) {
            Disposal d{slot, nullptr};
            d.channelSender = isTx;
            d.channel       = isCh;
            d.sharedHandle  = isSh;
            // Never-discharged owner (auto-drop) → release on both exits; a
            // discharged owned param → release only on the unwind exit.
            d.releaseOnUnwindOnly = !ps->autoDropAtScopeEnd;
            disposeStack_.front().push_back(d);
            return;
        }
    }
    if (!isClassOrInterfaceRef(rt) && !isClassOrInterfaceRef(resolveTypeParamRef(rt)))
        return;
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
           m == "out owned" || m == "out owned var";
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

// #1918 / §28.3: true iff the CallExpr callee resolves to an ORDINARY user
// method — a MethodSymbol backed by an ast::MethodDecl. That is precisely the
// call path lowered through `buildOrderedArgs(md->params, …,
// nullTransferredOwned=true)` (LlvmBackend_Calls2.cpp), which nulls the caller's
// slot for a bare owned local/param transferred to an `owned` parameter. Only
// for such a callee may the walker leave that argument's local eligible for
// unwind disposal (the null keeps it exactly-once). An imported method
// (ms->decl not a MethodDecl → the non-buildOrderedArgs call path), a
// constructor, an extern, or a call through a function-typed value does NOT null
// the slot, so the argument stays conservatively unwind-INELIGIBLE (a safe leak,
// never a double-free).
inline bool calleeIsUserMethod(const sema::SemaContext& ctx,
                               const ast::Expr* callee) {
    if (!callee) return false;
    sema::Symbol* s = ctx.exprSymbol(*callee);
    auto* ms = dynamic_cast<sema::MethodSymbol*>(s);
    return ms && dynamic_cast<const ast::MethodDecl*>(ms->decl) != nullptr;
}

// #1955 / §28.3: true iff `callee` is an ARRAY-BACKED owning-collection element-
// transfer method — `OwningArray.Add` / `.Insert`, `OwningStack.Push`,
// `OwningQueue.Enqueue`. For these the element parameter is definitionally
// `owned` (the collection takes over the element's disposal), and the element
// lowers through bz_array_add / bz_array_insert_at (LlvmBackend_Calls.cpp) —
// EXACTLY the paths that now NULL the caller's slot for a bare owned local/param
// element argument (nullTransferredOwnedLocalSlot). So the walker may leave that
// argument's local eligible for unwind disposal (the null-guarded unwindOnly
// disposal stays exactly-once), as it does for an ordinary user method.
//
// This is a deliberate ALLOWLIST, not a broad `isOwningPrefixedName` check,
// because the invariant the two sites must jointly preserve is `eligible ⟹
// nulled`. The other owning collections take insert paths that do NOT null:
// OwningSet.Add dedup-inserts (emitSetDedupInsert), OwningLinkedList uses the
// linked-list runtime, OwningDictionary the hash runtime. Admitting them here
// would leave their transferred local eligible with a NON-null slot → a double-
// free on the transfer-window throw edge. They stay tainted (a safe residual
// leak on a preceding-throw edge, never a double-free) until their own paths
// null too. A BORROWING collection (`Array<T>.Add`) is likewise excluded — no
// ownership transfer, the caller keeps ownership.
inline bool calleeIsOwningCollectionTransfer(const sema::SemaContext& ctx,
                                             const ast::Expr* callee) {
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(callee);
    if (!ma || !ma->target) return false;
    const auto& m = ma->member;
    if (m != "Add" && m != "Push" && m != "Enqueue" && m != "Insert")
        return false;
    const sema::TypeRef* recvTy = ctx.exprType(*ma->target);
    if (!recvTy) return false;
    std::string_view h = recvTy->headName();
    return h == "OwningArray" || h == "OwningStack" || h == "OwningQueue";
}

// #1955 / §28.3: true iff `new <type>(args)` resolves to an EXPLICIT user
// constructor — a non-static ConstructorSymbol backed by an ast::ConstructorDecl
// whose arity matches. That is EXACTLY the `new` shape whose lowerNew arg-
// lowering routes through `buildOrderedArgs(nullTransferredOwned=true)` (via
// `ctorForArgs`, LlvmBackend_Exprs2.cpp) and therefore NULLS the caller's slot
// for a bare owned local/param `owned x` argument — so the walker may leave that
// local eligible for unwind disposal (the null-guarded unwindOnly disposal stays
// exactly-once).
//
// It is NOT enough to key on `TypeDeclKind::Class`/`Record`: a PRIMARY-ctor class
// (`class Box(owned r: Resource)`) has NO ConstructorSymbol — sema stores its
// params on the TypeDecl (TypeResolution_Checks.cpp), so `ctorForArgs` returns
// null and NO buildOrderedArgs runs; the captured owned param is stored via the
// positional-field path (LlvmBackend_Exprs2.cpp), which does NOT null the slot.
// Untainting there would leave a NON-null slot eligible → the class's field
// disposer frees the value on unwind, then the caller's unwind disposal frees it
// AGAIN (double-free). A builtin owned-consuming ctor (Shared<T>/Weak<T> — null
// `declaration`) and an imported ctor likewise lower manually without nulling.
// All of those stay TAINTED (a safe residual leak on a throw-BEFORE-transfer
// edge, a tracked #1955 follow-up, never a double-free). This predicate keeps
// the walker's `eligible ⟹ nulled` invariant EXACT.
inline bool newExprRoutesThroughNullingCtor(const sema::SemaContext& ctx,
                                            const ast::NewExpr* n) {
    if (!n || !n->type) return false;
    const sema::TypeRef* nt = ctx.resolvedType(*n->type);
    if (!nt || !nt->declaration) return false;          // builtin (null decl)
    if (ctx.isImported(nt->declaration)) return false;  // imported → manual lower
    for (sema::Symbol* m : nt->declaration->members) {
        auto* cs = dynamic_cast<sema::ConstructorSymbol*>(m);
        if (!cs || cs->isStatic) continue;
        if (cs->params.size() != n->args.size()) continue;
        if (dynamic_cast<const ast::ConstructorDecl*>(cs->decl)) return true;
    }
    return false;
}

// §28.3 / §28.10 (v0.180, #1978): does the bare-identifier SOURCE of an
// `owned`-marked transfer — `var y := owned x;` / `using var y := owned x;`
// (#1962) and `y := owned x;` (#1965) — name a slot the transfer codegen NULLS
// (nullTransferredOwnedLocalSlot)? True iff the source is a plain `var` owned
// LOCAL (non-`using`) or an `owned` PARAMETER — the EXACT slots that helper
// nulls. When true, UnwindIneligibleWalker leaves the source unwind-ELIGIBLE
// (mirroring the #1918 user-method / #1955 ctor / #1956 reassignment transfers):
// the null-guarded unwindOnly disposal is exactly-once — it disposes the source
// on a throw edge that PRECEDES the transfer (fixes the #1978 leak) and is
// skipped (null slot) on the transfer / post-transfer edge (no double-free with
// the target's own disposal). Any OTHER source shape (a borrow, a `using` local,
// a fresh producer, a non-identifier) stays TAINTED — a safe residual leak, never
// a double-free. This predicate keeps the walker's `eligible ⟹ nulled` invariant
// EXACT (it mirrors nullTransferredOwnedLocalSlot's own eligibility test).
inline bool markedOwnedSourceEligible(const sema::SemaContext& ctx,
                                      const ast::Expr* src) {
    auto* id = dynamic_cast<const ast::IdentifierExpr*>(src);
    if (!id) return false;
    sema::Symbol* asym = ctx.exprSymbol(*id);
    if (auto* loc = dynamic_cast<sema::LocalSymbol*>(asym))
        return loc->decl && !loc->isUsing;
    if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(asym))
        return ps->modifier == "owned";
    return false;
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
                      const std::vector<ast::CallArgument>& args,
                      bool untaintOwnedLocalTransfer = false) {
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
            // #1918 / §28.3: a bare owned LOCAL / owned PARAMETER passed with an
            // ownership TRANSFER (explicit `owned` arg, or a mapped `owned`
            // callee param) to an ORDINARY user method does NOT taint the local
            // as unwind-INELIGIBLE. The codegen for that exact call path
            // (buildOrderedArgs nullTransferredOwned) nulls the caller's slot at
            // the transfer, so the null-guarded unwindOnly disposal is exactly-
            // once: it skips the transferred value on the transfer / post-transfer
            // throw edge (no double-free with the callee's owned-param disposal),
            // and disposes a SIBLING-branch value still live at a throw (fixes the
            // #1918 leak). Only for this shape — `ref`/`out owned`/an unmapped or
            // non-user-method callee still taints (the slot is NOT nulled there,
            // so eligibility would risk a double-free). Restricted to a plain
            // `var` owned local or an `owned` parameter, mirroring the slots
            // buildOrderedArgs actually nulls. // per §28.3
            bool branchSensitiveOwnedTransfer = false;
            if (untaintOwnedLocalTransfer && a.value &&
                (a.modifier == "owned" || pm == "owned")) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        a.value.get())) {
                    sema::Symbol* asym = ctx.exprSymbol(*id);
                    if (auto* loc = dynamic_cast<sema::LocalSymbol*>(asym))
                        branchSensitiveOwnedTransfer = loc->decl && !loc->isUsing;
                    else if (auto* ps =
                                 dynamic_cast<sema::ParameterSymbol*>(asym))
                        branchSensitiveOwnedTransfer = (ps->modifier == "owned");
                }
            }
            expr(a.value.get(),
                 branchSensitiveOwnedTransfer ? /*consuming=*/false : consuming);
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
            // #1918: for an ordinary user-method callee (the buildOrderedArgs
            // nullTransferredOwned path) a transferred owned local/param is left
            // eligible (branch-sensitive) — see walkCallArgs.
            // #1955: an owning-collection element-transfer method
            // (`OwningArray.Add(owned x)` / Push / Enqueue / Insert) NULLS the
            // caller's slot at its lowering site too, so its transferred owned
            // local/param is likewise left eligible.
            walkCallArgs(c->callee.get(), c->args,
                         /*untaintOwnedLocalTransfer=*/
                         calleeIsUserMethod(ctx, c->callee.get()) ||
                         calleeIsOwningCollectionTransfer(ctx, c->callee.get()));
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
            // #1955 / §28.3: a constructor argument transferred via an explicit
            // `owned x` (a bare owned local / owned parameter) is NULLED at the
            // ctor's arg-lowering site (lowerNew's buildOrderedArgs, now
            // nullTransferredOwned=true), so the walker leaves that local
            // ELIGIBLE for unwind disposal — exactly as it does for a user-method
            // `owned` argument (#1918): the null-guarded unwindOnly disposal skips
            // the value on the transfer / post-transfer throw edge (no double-free
            // with the constructor's own owned-field cleanup) and disposes it on a
            // throw edge that PRECEDES the transfer (fixes the #1955 leak). Every
            // OTHER constructor argument is a consuming position (an owned
            // temporary, a `move`, a field init) and stays tainted. Restricted to
            // the EXPLICIT `owned` modifier + a plain `var` owned local or an
            // `owned` parameter — exactly the slots buildOrderedArgs nulls for
            // `a.modifier == "owned"`. An IMPLICIT bare-to-owned-param ctor
            // argument stays tainted (a safe leak, never a double-free): the
            // walker cannot resolve the exact overloaded ctor here to confirm the
            // param is `owned`, so it does not claim eligibility the nulling might
            // not cover.
            //
            // CRITICAL — the untaint fires ONLY when `new` routes through the
            // NULLING arg-lowering path: an EXPLICIT user constructor (a matching
            // ConstructorSymbol backed by a ConstructorDecl → ctorForArgs →
            // buildOrderedArgs, nullTransferredOwned=true). It is NOT enough to be
            // a class/record: a PRIMARY-ctor class (`class Box(owned r)`) has NO
            // ConstructorSymbol, so its captured owned arg is stored via the
            // positional-field path (LlvmBackend_Exprs2.cpp) which does NOT null
            // the slot; a COMPILER-KNOWN BUILTIN owned-consuming ctor (Shared<T>/
            // Weak<T> via bz_shared_new — null `declaration`) and an IMPORTED ctor
            // likewise lower manually without nulling. Untainting any of those
            // would leave a NON-null slot eligible → a DOUBLE-FREE on the
            // transfer-window throw edge (the ctor/field disposer frees the value
            // on unwind, then the caller's unwind disposal frees it AGAIN). All of
            // them stay TAINTED — a safe residual leak on a throw-BEFORE-transfer
            // edge (tracked #1970 follow-up), never a double-free.
            // newExprRoutesThroughNullingCtor keeps `eligible ⟹ nulled` EXACT.
            // per §28.3
            bool nullingCtor = newExprRoutesThroughNullingCtor(ctx, n);
            for (auto& a : n->args) {
                bool untaint = false;
                if (nullingCtor && a.modifier == "owned" && a.value) {
                    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                            a.value.get())) {
                        sema::Symbol* asym = ctx.exprSymbol(*id);
                        if (auto* loc =
                                dynamic_cast<sema::LocalSymbol*>(asym))
                            untaint = loc->decl && !loc->isUsing;
                        else if (auto* ps =
                                     dynamic_cast<sema::ParameterSymbol*>(asym))
                            untaint = (ps->modifier == "owned");
                    }
                }
                expr(a.value.get(), /*consuming=*/!untaint);
            }
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
            // §28.3 / §28.10 (v0.180, #1978): a `var y := owned x;` / `using var
            // y := owned x;` binding marker (#1962) MOVES a named owned source.
            // The transfer codegen (lowerLocalDecl) NULLS the source slot, so —
            // exactly like the #1918 user-method / #1955 ctor / #1956
            // reassignment transfers — leave the source unwind-ELIGIBLE: disposed
            // once on a throw that PRECEDES the binding (fixes the #1978 leak),
            // skipped (null slot) on the post-transfer edge (no double-free with
            // the new binding's own disposal). Restricted to the slots codegen
            // nulls (markedOwnedSourceEligible), keeping `eligible ⟹ nulled`
            // exact. // per §28.3 v0.180 #1978
            if (ld->initializerHasOwnedMarker &&
                markedOwnedSourceEligible(ctx, ld->initializer.get())) {
                expr(ld->initializer.get(), /*consuming=*/false);
                return;
            }
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
            // fix #1956 / §28.3: a bare-identifier reassignment `x := …` does
            // NOT taint x as unwind-INELIGIBLE. The slot holds exactly ONE live
            // owned value between declaration and discharge: on the normal path
            // the reassignment lowering evaluates the RHS, disposes the displaced
            // OLD value (disposeDisplacedOwnedClassLocal — null+identity guarded),
            // then stores the new one; and the unwindOnly disposal reloads the
            // slot null-guarded (dynamic dispatch). So the slot always holds
            // exactly the currently-live value at any throw edge:
            //   - a throw BEFORE the reassignment (or while evaluating its RHS)
            //     leaves the OLD value live in the slot → unwind-disposed once
            //     (the reassignment never ran to dispose it) — fixes the #1956
            //     leak;
            //   - a throw AFTER the reassignment finds the NEW value in the slot
            //     → unwind-disposed once (the old was disposed at the reassign).
            // Tainting x here was a flow-INSENSITIVE over-approximation: it saw
            // `x := …` anywhere in the body and dropped the unwind disposal for
            // the throw edge that PRECEDES the reassignment, leaking the pre-
            // reassignment value. A NON-identifier target (`obj.f := …`,
            // `a[i] := …`) is NOT an unwind-eligible owned LOCAL/param key at
            // all, so it is still walked (markIdent → expr, borrow rules) exactly
            // as before. The RHS is walked as consuming (`x := y` moves y).
            // per §28.3
            if (!dynamic_cast<const ast::IdentifierExpr*>(as->target.get()))
                markIdent(as->target.get());
            // §28.3 / §28.10 (v0.180, #1978): a bare-identifier-target
            // `y := owned x;` assignment marker (#1965) MOVES a named owned
            // source. handleMarkedOwnedAssignment routes ONLY a bare-id tracked-
            // local target (a member/index target is self-announcing — the marker
            // is ignored and the general path consumes the source WITHOUT nulling,
            // so it must stay tainted); for the routed shape the assignment
            // codegen NULLS the source slot, so leave the source unwind-ELIGIBLE
            // — same reasoning as the binding marker above and #1918/#1955/#1956.
            // per §28.3 v0.180 #1978
            bool markedEligibleSource =
                as->valueHasOwnedMarker &&
                dynamic_cast<const ast::IdentifierExpr*>(as->target.get()) &&
                markedOwnedSourceEligible(ctx, as->value.get());
            expr(as->value.get(), /*consuming=*/!markedEligibleSource);
            return;
        }
        if (auto* inc = dynamic_cast<const ast::IncrementStmt*>(s)) {
            expr(inc->target.get(), /*consuming=*/false); return;
        }
        if (auto* ds = dynamic_cast<const ast::DiscardStmt*>(s)) {
            // fix #1893 / §28.3 (v0.184/v0.194): a `_ := x;` DISCARD of a bare-
            // identifier named owned binding (the "announced consume") must NOT
            // taint `x` as unwind-ineligible — the exact same reasoning as an
            // explicit `x.Dispose()` (walked as a borrow above). On the normal
            // path the discard's announced-consume codegen (discardOutOwnedVar /
            // the owning-collection discard path) disposes `x` AND nulls its slot,
            // and every scope-end / exception-path disposal reloads that slot
            // null-guarded (or via the null-safe collection free) — so a discard
            // that is UNREACHABLE or PRECEDED BY A THROW keeps a live slot and
            // must still be unwind-disposed (no leak), while a discard that ran
            // leaves a null slot and is skipped (no double-free). Marking `x`
            // ineligible here was a flow-INSENSITIVE over-approximation: it saw
            // `_ := x` anywhere (even AFTER a throwing operation, as in #1893) and
            // dropped the unwind disposal for the edge that PRECEDES the discard,
            // leaking `x` on that throw path. A non-identifier discard value
            // (`_ := move x`, `_ := producer()`, `_ := x.Field`) is handled by the
            // expr() walk with its own consuming rules and is unaffected. // per §28.3
            if (dynamic_cast<const ast::IdentifierExpr*>(ds->value.get()))
                expr(ds->value.get(), /*consuming=*/false);
            else
                expr(ds->value.get(), /*consuming=*/true);
            return;
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
            // §17.13 / §15.3 (#1016): `_ := arr` is now LEGAL for a move-only
            // owning array — it disposes the slots at statement end. The source
            // identifier `arr` is walked as a transfer (borrow=false) so it is
            // marked in moveOnlyArrayTransferred_, causing
            // registerInlineOwnedArrayDisposal to skip the scope-end disposal
            // (the discard takes ownership; no double-free).
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
    // fix #1934 / §19.2 / §28.3 (producers arm-meet, v0.184) / §28.9: an
    // expression-position `case`/`when` whose result is a non-nullable owned
    // `string` is a single +1 producer. lowerCaseExpr normalizes every arm to
    // own exactly +1 (a producer arm transfers its reference; a borrow arm is
    // retained), so the case-expression result carries exactly one reference —
    // the consumer (return / assign / bind) must TRANSFER it, not mint a second
    // borrow-retain. Without recognizing it here, the consumer's borrow-retain
    // double-counted the arm's +1 and the arm's original reference leaked
    // (#1934). Recognized for the same non-nullable `string` result the per-arm
    // mint in lowerCaseExpr is keyed on, so the two decisions stay in lockstep.
    if (auto* ce = dynamic_cast<const ast::CaseExpr*>(init)) {
        const sema::TypeRef* rt = ctx_.exprType(*ce);
        return rt && !rt->nullable &&
               rt->kind == sema::TypeRef::Kind::Primitive &&
               rt->primitive == sema::PrimitiveType::String;
    }
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
    // §3.1 / §28.3 / §28.9 (#1637): StringBuilder.ToString() is a BUILTIN lowered
    // in tryLowerCollectionInstanceCall (no resolved MethodSymbol, so
    // isUserMethodStringReturnCall below never sees it). Its lowering concatenates
    // the parts into a freshly malloc'd buffer (bz_strbuilder_to_string) wrapped in
    // a heap `bz.string` whose rc := 1 (initStringRc) — exactly the +1 single-
    // reference shape of `+`/interpolation/range-slice/primitive-ToString/
    // string.Join. Before this recognition the result was treated as a borrow and
    // never released: `sb.ToString().Length` (and comparison/concat) read the
    // string without dropping its single reference, leaking the 32-byte header +
    // data buffer. Recognize it structurally — a CallExpr with an empty arg list,
    // member == "ToString", and a StringBuilder-typed receiver (headName()
    // "StringBuilder", alias-transparent per #1462) — so the borrowing read
    // releases it after the read, and `var s := sb.ToString()` transfers the +1
    // into the local (released at scope end when non-escaping). No double-free: the
    // release site drops the SINGLE minted reference exactly once; an escaping
    // local is excluded from the scope-end release by the escape walk (its +1 then
    // leaks — the safe direction). This is a distinct check from the primitive-
    // ToString block above, which requires a Primitive receiver and returns false
    // for a Named StringBuilder.
    if (auto* call = dynamic_cast<const ast::CallExpr*>(init)) {
        if (call->args.empty() && call->callee) {
            if (auto* ma =
                    dynamic_cast<const ast::MemberAccessExpr*>(call->callee.get())) {
                if (ma->member == "ToString" && ma->target) {
                    auto* tt = ctx_.exprType(*ma->target);
                    if (tt && tt->kind == sema::TypeRef::Kind::Named &&
                        tt->headName() == "StringBuilder")
                        return true;  // bz_strbuilder_to_string → rc=1 heap string
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

    // #1871 / §28.9 / §28.11: enable the borrowing-`Array<T>`-ONLY safe-read
    // relaxations below (a for-in over the target array, and a binding of the
    // target into a proven-non-escaping pure-`IEnumerable<T>` VIEW local, are
    // borrows — not escapes). Off by default so the string-release reuse of this
    // walk keeps its stricter conservative treatment. Only the borrowing-array
    // eligibility analysis (collectBorrowingArrayFreeEligible) sets it.
    bool borrowingArrayMode = false;
    // #1871: the set of `var v: IEnumerable<T> := <target-array>` VIEW LocalDecls
    // that were independently proven never to escape. A binding of the target into
    // one of these is a borrow (the view wrapper borrows the array's backing; its
    // Dispose frees only the wrapper struct), so it does NOT taint the array.
    // Non-null only in borrowingArrayMode; when null every into-local binding stays
    // an escape.
    const std::unordered_set<const void*>* safeViewAliases = nullptr;

    bool isTargetIdent(const ast::Expr* e) {
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(e);
        if (!id) return false;
        auto* loc = dynamic_cast<sema::LocalSymbol*>(ctx.exprSymbol(*id));
        return loc && loc->decl &&
               static_cast<const void*>(loc->decl) == target;
    }

    // #1185 / stdlib §3.1: StringBuilder.Append/AppendLine(string) is a RETAINING
    // builtin consumer — Append retains (or, for a +1 producer, transfers) the
    // string into its owned parts list, so the builder holds an INDEPENDENT +1
    // that keeps the value alive for the builder's lifetime. The builder is
    // disposed at scope end, so passing a string LOCAL to Append does NOT make
    // the local escape: a scope-end release of the caller's +1 is safe (the
    // builder's own retain outlives the call; no UAF). This mirrors the #1430
    // plain-method relaxation, extended to this retaining builtin (which has no
    // resolved MethodSymbol — it is matched structurally in
    // tryLowerCollectionInstanceCall). Only the STRING arg position (index 0) of
    // Append/AppendLine qualifies; other overloads/positions stay escaped.
    bool isStringBuilderRetainingStringArg(const ast::Expr* callee, size_t i) {
        if (i != 0) return false;
        auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(callee);
        if (!ma || !ma->target) return false;
        if (ma->member != "Append" && ma->member != "AppendLine") return false;
        const sema::TypeRef* rt = ctx.exprType(*ma->target);
        return rt && rt->kind == sema::TypeRef::Kind::Named &&
               !rt->declaration && rt->astNode && rt->astNode->name &&
               !rt->astNode->name->parts.empty() &&
               rt->headName() == "StringBuilder";
    }

    // Call arguments (#1430): a bare read of the target passed in a PLAIN
    // (no-modifier) parameter position of a resolved user/imported method is a
    // CALL-BOUNDED read, not an escape (§28.3 borrow-for-the-call / §28.9).
    // The callee is compiled under the same string refcount discipline, so
    // every path by which it could extend the argument's lifetime mints its
    // own +1: a field store retains (the #429 argpass cardinal canary), a
    // return of the param mints (#429/#694 return logic), a closure capture
    // retains (capture_retain_429), and an onward pass repeats this rule one
    // level down. The caller's scope-end release therefore never strands the
    // callee. Everything else — ref/out/owned positions, named args
    // (positional mapping unknown), operators, extension methods
    // (receiver-shifted param mapping), generic methods, unresolved/builtin
    // callees (no compiled body to hold the discipline), non-identifier args —
    // stays ESCAPED, leak-over-UAF as before. String-walker only: closure envs
    // and boxed Objects have no equivalent callee-side retain discipline.
    void walkCallArgs(const ast::Expr* callee,
                      const std::vector<ast::CallArgument>& args) {
        const sema::MethodSymbol* ms = nullptr;
        if (callee) {
            ms = dynamic_cast<sema::MethodSymbol*>(ctx.exprSymbol(*callee));
            if (ms && (ms->isOperator || ms->extensionDecl ||
                       !ms->genericParams.empty() ||
                       (ms->decl == nullptr && !ctx.isImported(ms))))
                ms = nullptr;  // not a plain compiled method — no relaxation
        }
        for (size_t i = 0; i < args.size(); ++i) {
            const ast::CallArgument& a = args[i];
            bool safeArg = false;
            if (ms && a.modifier.empty() && a.name.empty() &&
                i < ms->params.size() && isTargetIdent(a.value.get())) {
                auto* ps =
                    dynamic_cast<sema::ParameterSymbol*>(ms->params[i]);
                if (ps && ps->modifier.empty()) safeArg = true;
            }
            // #1185: a string local passed to StringBuilder.Append/AppendLine's
            // string slot is non-escaping (the retaining builder owns a +1).
            if (!safeArg && a.modifier.empty() && a.name.empty() &&
                isStringBuilderRetainingStringArg(callee, i) &&
                isTargetIdent(a.value.get()))
                safeArg = true;
            expr(a.value.get(), safeArg);
        }
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
            walkCallArgs(c->callee.get(), c->args);
            return;
        }
        if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
            expr(c->callee.get(), /*safeRead=*/true);
            walkCallArgs(c->callee.get(), c->args);
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
            // #1871 / §28.9 / §28.11: binding the target array directly into a
            // pure-`IEnumerable<T>` VIEW local that was independently proven never
            // to escape (safeViewAliases) is a BORROW: the view wrapper borrows the
            // array's backing (wrapArrayAsIEnumerable) and its scope-end Dispose
            // frees only the wrapper struct — never the backing (§28.11). So the
            // binding does NOT taint the array; the array remains the sole owner of
            // its backing and reclaims it at ITS OWN scope end (which the ownership
            // pass proves outlives the view — the array is usable after the view's
            // scope). Only in borrowingArrayMode, and only for the exact
            // `var v := <array-identifier>` shape (isTargetIdent of the whole
            // initializer); a sub-expression use falls through to the escape walk.
            if (borrowingArrayMode && safeViewAliases &&
                safeViewAliases->count(static_cast<const void*>(ld)) &&
                isTargetIdent(ld->initializer.get()))
                return;
            // Otherwise: binding the target into ANOTHER local escapes it (the new
            // local may itself be returned/stored). Walk the initializer as an
            // escape position — but its sub-reads (e.g. `s.ToUpper()` receiver) are
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
            // §4.6 / §28.9 (#1486): a string compound-assign `X += Y` (the only
            // valid compound on a `string`) is a CONCAT. It borrow-reads Y into a
            // FRESH buffer — it never retains Y into a slot — and maintains the
            // target slot's independent +1 (the `+=` lowering releases the old
            // value and stores a fresh +1 concat result; a +1-producer RHS is
            // released after the concat borrows it). So neither operand becomes a
            // lasting alias:
            //   * the TARGET (if it is our local) is a reassignment, NOT an escape
            //     — the slot still owns exactly one reference at scope end, which
            //     the scope-end release drops soundly (no other alias exists once
            //     the escape walk is otherwise clean).
            //   * a bare-identifier RHS OPERAND (if it is our local) is a SAFE READ
            //     (borrow-read by the concat), so being a `+=` operand does not
            //     taint it. Nested-call args in the RHS still ESCAPE via the
            //     CallExpr case (walkCallArgs), so `X += f(s)` still taints s.
            // Both directions were previously over-approximated as escapes, so a
            // non-escaping accumulator / operand leaked its final +1 at scope end
            // (bounded 1-block leak). Restricted to `+=`; a plain `:=` keeps the
            // conservative treatment below (its RHS RETAINS a borrow into the slot,
            // so scope-end eligibility there needs a separate soundness argument).
            if (as->op == "+=") {
                if (!isTargetIdent(as->target.get()))
                    expr(as->target.get(), /*safeRead=*/false);
                expr(as->value.get(), /*safeRead=*/true);
                return;
            }
            // Plain `:=` (or any other assignment): the target being the local
            // itself is a reassignment → escape (overwrite handling for `:=` is
            // out of this fix). The RHS escapes.
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
            // #1871 / §28.9 / §28.11: EXCEPT in borrowingArrayMode, where the
            // target is a borrowing `Array<T>`: a `for var x in a` clones a
            // per-loop enumerator that BORROWS the backing (bz_array_ptr_at) and
            // whose Dispose frees only its own struct — it never retains or frees
            // the backing. So iterating the array is a borrow-read, not an escape,
            // and the array stays reclaim-eligible. (A read inside a lambda body
            // still escapes: expr() forces it via inLambda regardless of safeRead.)
            if (borrowingArrayMode && isTargetIdent(fs->iterable.get()))
                expr(fs->iterable.get(), /*safeRead=*/true);
            else
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
    // §27.6 (v0.184, #1758): a bare reference to a primitive-intrinsic static
    // method (`int.Parse`, `char.IsDigit`, …) ALSO lowers to a fresh calloc'd
    // rc=1 closure cell — via makePrimIntrinsicThunk (the CG60 path,
    // LlvmBackend_Exprs.cpp) — but has NO MethodSymbol (intrinsics are
    // name-recognized), so the MethodSymbol branch above misses it and the cell
    // leaked (40 bytes/reference). The shared predicate keys eligibility EXACTLY
    // to the CG60 gate that produced the cell, so binding the reference to a
    // local releases its single reference at scope end. A call is a CallExpr
    // node, never this shape, so this cannot misfire.
    if (isPrimIntrinsicMethodRef(init)) return true;
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
// field lvalue (OwningArray<T>/OwningStack<T>/OwningQueue<T>/OwningSet<T>/
// OwningLinkedList<T>/OwningBuffer<T>/OwningDictionary<T> etc.)? Classification
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

    // §28.5/§28.11, #894: OwningDictionary<K,V> owns its V values (genericArgs[1]),
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
        common::isOwningPrefixedName(collTy->headName());

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
                n == "Queue"; }(collTy->headName()))
        elemDisp = stringElementReleaseThunk();

    llvm::Value* disp = elemDisp
        ? llvm::cast<llvm::Value>(elemDisp)
        : llvm::ConstantPointerNull::get(ptrTy());

    // per §4.6/§4.7: LinkedList/OwningLinkedList free via bz_llist_free.
    // §28.5/§28.11, #894: OwningDictionary fields free via bz_dict_free.
    bool isLL = collTy && isLinkedListTypeName(collTy);
    const char* freeName = isLL   ? "bz_llist_free"
                          : isDict ? "bz_dict_free"
                          :          "bz_array_free";
    auto* freeFn = declareC(freeName,
                            llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
    builder_.CreateCall(freeFn, {oldVal, disp});
    return true;
}


// §28.7 / #1328: is `target` an `owned` plain class/interface CLASS instance
// field lvalue that needs drop-on-overwrite? Classification mirrors
// isOwnedCollectionFieldLvalue: exclude locals, ref params, static fields, and
// any field that a sibling helper already handles (string, closure, box, and
// collection fields are excluded because isOwnedReferenceField returns false for
// collection-typed fields and the other categories never reach this helper).
// Only class- or interface-typed `owned` fields on CLASS instances remain.
bool Emitter::isOwnedClassFieldLvalue(const ast::Expr& target) {
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
    // Must be a CLASS instance field (not a record field — records have no
    // §28.4-step-2 auto-dispose, §9.7). Mirrors isOwnedCollectionFieldLvalue.
    if (!fs->parent || fs->parent->kind != ast::TypeDeclKind::Class) return false;
    // isOwnedReferenceField covers exactly class- and interface-typed `owned`
    // fields (with generic-type-param substitution) while excluding collection
    // types (which are handled by lowerOwnedCollectionFieldStore) and
    // Shared<T>/Weak<T> (handled by their own refcount release paths).
    //
    // #1644 / §29.7: builtin iterator-interface fields (owned IEnumerable<T> /
    // IOwningEnumerable<T>) also need drop-on-overwrite, but isOwnedReferenceField
    // rejects them (they have no user `declaration`) — the same root cause #1643
    // fixed for the SCOPE-END path. Accept them here so lowerOwnedClassFieldStore
    // disposes the displaced iterator FRAME on reassignment (via the iterator-
    // vtable slot-1 protocol, mirroring emitOwnedFieldDisposals' iterField branch).
    return isOwnedReferenceField(fs) || isOwnedIteratorInterfaceField(fs);
}


// §28.7 / #1328: dispose the prior value of an owned class/interface CLASS
// field before storing the new owner. Parallel to lowerOwnedCollectionFieldStore
// for collections.
//
// Order (per spec §28.7 / the disposeDisplacedOwnedClassLocal local analog):
//   load-old → null-guard (moved-from or never-assigned → null) →
//   identity-guard (self-assign `this.f := this.f` must NOT dispose) →
//   dispose-old → store-new.
//
// Constructor first-time assignments are safe: the object is calloc-zeroed
// (field holds null → null-guard skips disposal). The §28.4-step-2 auto-dispose
// (emitOwnedFieldDisposals) runs after our store and disposes the new value at
// scope end — no double-free. Inside a destructor body, a reassignment disposes
// the old value here; the auto-dispose after the body disposes the new value —
// correct. emittingDisposerBody_ does NOT need to gate this: the auto-dispose's
// null-guard (and the §28.6 isDisposed flag) protect against double-dispose.
bool Emitter::lowerOwnedClassFieldStore(const ast::Expr& target,
                                        llvm::Value* fieldAddr,
                                        llvm::Value* newVal) {
    if (!isOwnedClassFieldLvalue(target)) return false;
    if (!fieldAddr || !newVal) return false;
    if (!newVal->getType()->isPointerTy()) return false;
    // Load the old object pointer.
    llvm::Value* oldVal = builder_.CreateLoad(ptrTy(), fieldAddr, "ocls.old");

    // Null-guard (a moved-from slot holds null) AND identity-guard (a self-
    // assignment `this.f := this.f` would otherwise dispose the value that is
    // being stored → UAF). Mirrors disposeDisplacedOwnedClassLocal exactly.
    llvm::Value* nullp = llvm::ConstantPointerNull::get(ptrTy());
    llvm::Value* live = builder_.CreateICmpNE(oldVal, nullp, "ocls.live");
    llvm::Value* doDispose = builder_.CreateAnd(
        live, builder_.CreateICmpNE(oldVal, newVal, "ocls.diff"));

    auto* dispBB = llvm::BasicBlock::Create(llvm_, "ocls.dispose", currentFn_);
    auto* contBB = llvm::BasicBlock::Create(llvm_, "ocls.cont", currentFn_);
    builder_.CreateCondBr(doDispose, dispBB, contBB);
    builder_.SetInsertPoint(dispBB);
    // #1644 / §29.7: an owned iterator-interface field (IEnumerable<T> /
    // IOwningEnumerable<T>) holds an iterator-vtable object whose Dispose is at
    // slot 1 (iterator format [0]=td,[1]=Dispose,[2]=MoveNext,[3]=Current) — and
    // that Dispose FREES THE FRAME ITSELF (§29.7). Dispatch slot 1 directly; do
    // NOT use the class-vtable slot-2 emitDisposeAndReclaim, which would invoke
    // MoveNext and never free the frame. The doDispose guard above already
    // excluded null / self-assign, so no per-object null-guard is needed here.
    // Mirrors emitOwnedFieldDisposals' iterField branch (the scope-end analog).
    sema::Symbol* sym = ctx_.exprSymbol(target);
    auto* fs = dynamic_cast<sema::FieldSymbol*>(sym);
    if (fs && isOwnedIteratorInterfaceField(fs)) {
        llvm::Value* vt = builder_.CreateLoad(ptrTy(), oldVal);
        llvm::Value* dispSlot =
            builder_.CreateGEP(ptrTy(), vt, {builder_.getInt32(1)});
        llvm::Value* dispFnPtr = builder_.CreateLoad(ptrTy(), dispSlot);
        auto* dispTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(llvm_), {ptrTy()}, false);
        builder_.CreateCall(dispTy, dispFnPtr, {oldVal});
    } else {
        emitDisposeAndReclaim(oldVal);
    }
    builder_.CreateBr(contBB);
    builder_.SetInsertPoint(contBB);

    // Store new value after disposing old (spec §28.7: RHS first, then dispose
    // displaced, then store). The field holds null between dispose and store; the
    // §28.4-step-2 emitOwnedFieldDisposals null-guard handles a re-entrant
    // dispose that catches the field in that window (null → skips it).
    builder_.CreateStore(newVal, fieldAddr);
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
// — name-head "Array", not "OwningArray", and the materializer has no `owned`-
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
    // dict-backed (Set) / llist-backed (LinkedList) / owning (OwningArray) families.
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


// #1930 / stdlib §5 / §28.9: is `ld` a plain `var g := <expr>` whose initializer is
// a bare-GroupBy element-RETURNING terminal — a call `<src>.GroupBy(sel).<Term>(…)`
// (Term ∈ First/Single/Last/ElementAt/*OrNull/no-seed Aggregate) whose result type
// is an IGrouping<TKey,T> pointer? The terminal returns one of the freshly-
// materialized groups; when the local never escapes (proven separately by the
// escape walk) the returned grouping is freed at scope end (bz_grouping_free). The
// gate is the RESULT TYPE (IGrouping, possibly nullable for the *OrNull forms) plus
// the receiver being a bare GroupBy — matching exactly the terminals the codegen
// keep-path (LlvmBackend_Calls.cpp) reclaims siblings for.
bool Emitter::isFreshGroupingTerminalLocal(const ast::LocalDecl* ld) const {
    if (!ld || ld->isUsing || !ld->initializer) return false;
    // Result type must be a synthetic declaration-less IGrouping<…> (the element a
    // bare-GroupBy element-returning terminal yields; nullable for *OrNull).
    const sema::TypeRef* rt = ctx_.exprType(*ld->initializer);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (!rt->astNode || !rt->astNode->name || rt->astNode->name->parts.empty())
        return false;
    if (rt->headName() != "IGrouping") return false;
    // The initializer must be a terminal CALL whose RECEIVER is a bare `GroupBy(sel)`.
    auto* call = dynamic_cast<const ast::CallExpr*>(ld->initializer.get());
    if (!call || !call->callee) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call->callee.get());
    if (!ma || !ma->target) return false;
    return isGroupByCall(ma->target.get());
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
    groupingFreeEligible_.clear();          // #1930
    std::vector<const ast::LocalDecl*> candidates;
    std::vector<const ast::LocalDecl*> dictCandidates;  // #1028
    std::vector<const ast::LocalDecl*> groupingCandidates;  // #1930
    std::function<void(const ast::Stmt*)> scan = [&](const ast::Stmt* s) {
        if (!s) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            // #1930: a plain `var g := <src>.GroupBy(sel).First()/…` — a bare-GroupBy
            // element-returning terminal binding the returned IGrouping. A `using var`
            // never reaches here (IGrouping has no user Dispose; it is a plain `var`).
            if (isFreshGroupingTerminalLocal(ld))
                groupingCandidates.push_back(ld);
            if (isFreshBorrowingArrayMaterializerLocal(ld))
                candidates.push_back(ld);
            // #1871 / §28.9 / §28.11: a plain `var xs: Array<T> := [ ... ]` — a
            // collection literal whose backing the local solely owns. `Array<T>`
            // is trivial-dispose (its Dispose is a no-op) but its backing storage
            // is "reclaimed with the instance" by an implementation-defined
            // mechanism (§28.9/§28.11) — here, a compiler-emitted scope-end
            // bz_array_free. Without it the backing (spine + data) leaks. Scoped to
            // backing-only element policies (value / class-or-interface borrows,
            // null disposer) via isBorrowingArrayLiteralLocal; string/fn-element
            // literals (their +1/element release differs) are excluded.
            else if (isBorrowingArrayLiteralLocal(ld))
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
        // #1871: gather this array's pure-`IEnumerable<T>` VIEW aliases that are
        // themselves proven non-escaping — a binding into such a view is a borrow,
        // not an escape (the view wrapper borrows the backing; its Dispose frees
        // only the wrapper struct). Then run the escape walk in borrowingArrayMode
        // so a for-in over the array and a binding into a safe view are borrows.
        std::unordered_set<const void*> safeViews;
        collectSafeViewAliases(body, ld, safeViews);
        StringEscapeWalker w{ctx_, static_cast<const void*>(ld)};
        w.borrowingArrayMode = true;
        w.safeViewAliases = &safeViews;
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
    // #1930 / stdlib §5 / §28.9: the same conservative type-agnostic escape walk
    // for bare-GroupBy element-returning-terminal locals. A local proven never to
    // escape holds the SOLE reference to the returned grouping (the terminal freed
    // its siblings + the array), so a scope-end bz_grouping_free runs exactly once
    // (no UAF, no double-free). The walk runs in the STRICT (non-borrowingArray)
    // mode: a `for var x in g` iteration or any binding/capture/argument use of the
    // grouping is treated as an escape — the returned grouping then just leaks (the
    // safe direction), never dangles. CRITICALLY this excludes the two escape routes
    // that would otherwise turn the free into a fault: capture by a closure that
    // outlives the scope (UAF), and `arr.Add(g)` into an owning Array<IGrouping>
    // that also frees it (double-free) — both surface as a non-safe-read occurrence
    // and taint the local out of eligibility.
    for (const ast::LocalDecl* ld : groupingCandidates) {
        StringEscapeWalker w{ctx_, static_cast<const void*>(ld)};
        for (auto& s : body) { w.stmt(s.get()); if (w.escaped) break; }
        if (!w.escaped)
            groupingFreeEligible_.insert(static_cast<const void*>(ld));
    }
}


// #1871 / §28.9 / §28.11: is `ld` a plain `var xs: Array<T> := [ ... ]` whose
// backing the local solely owns — a borrowing `Array<T>` (name-head exactly
// "Array") initialised by a COLLECTION LITERAL, whose element policy is
// backing-only (a null per-element disposer: value types, and class/interface
// BORROWS)? Those are exactly the literals codegen lowers to a fresh bz_array_t
// the local owns and whose scope-end reclaim is a single bz_array_free(backing,
// null) — no element +1 to release, so no double-free. `Array<string>` and other
// runtime-managed-element literals (retain/release-per-element) are excluded here
// (their +1/element semantics differ from the materializer path; left to a
// separate rule). A `new Array<T>(...)` / alias / materializer initializer is not
// a CollectionLiteralExpr, so it is not matched (those are handled elsewhere, or
// carry their own disposal obligation).
bool Emitter::isBorrowingArrayLiteralLocal(const ast::LocalDecl* ld) {
    if (!ld || ld->isUsing || !ld->initializer) return false;
    const sema::TypeRef* rt = nullptr;
    if (ld->type) rt = ctx_.resolvedType(*ld->type);
    if (!rt) rt = ctx_.exprType(*ld->initializer);
    if (!rt || rt->kind != sema::TypeRef::Kind::Named || rt->declaration)
        return false;
    if (builtinTypeName(rt) != "Array") return false;
    auto* lit = dynamic_cast<const ast::CollectionLiteralExpr*>(
        ld->initializer.get());
    if (!lit || lit->isDict) return false;
    // Backing-only element policy gate (same disposer the reclaim block derives):
    // a null per-element disposer AND not a runtime-managed (string/fn) element.
    const sema::TypeRef* el =
        (!rt->genericArgs.empty()) ? rt->genericArgs[0] : nullptr;
    ElementPolicy ep = elementPolicyFor(el, /*owningFamily=*/false);
    if (ep.disposerThunk != nullptr) return false;
    if (ep.cat == ElementPolicy::Category::RuntimeManaged) return false;
    return true;
}


// #1871 / §28.9 / §28.11: collect, into `out`, every `var v: IEnumerable<T> :=
// <arrLd>` VIEW LocalDecl in `body` — a pure declaration-less IEnumerable /
// IOwningEnumerable local whose initializer is EXACTLY the array identifier
// `arrLd` — that is itself proven NEVER to escape. Such a view borrows the
// array's backing (wrapArrayAsIEnumerable) and its scope-end Dispose frees only
// the wrapper struct (§28.11), so binding the array into it does not extend the
// backing's lifetime past the view's scope. The array remains the sole owner of
// its backing and reclaims it at ITS scope end (the ownership pass permits the
// array to be used after the view's scope — it is a borrow). A view that ESCAPES
// (returned / stored / captured / passed to an owned sink) could outlive the
// array, so it is excluded — the binding then stays an escape and the array is
// not reclaimed (the safe direction, a leak not a UAF).
void Emitter::collectSafeViewAliases(
        const std::vector<std::unique_ptr<ast::Stmt>>& body,
        const ast::LocalDecl* arrLd,
        std::unordered_set<const void*>& out) {
    std::vector<const ast::LocalDecl*> views;
    std::function<void(const ast::Stmt*)> scan = [&](const ast::Stmt* s) {
        if (!s) return;
        if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
            if (!ld->isUsing && ld->initializer && ld->type) {
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                        ld->initializer.get())) {
                    auto* loc =
                        dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*id));
                    if (loc && loc->decl &&
                        static_cast<const void*>(loc->decl) ==
                            static_cast<const void*>(arrLd)) {
                        const sema::TypeRef* dt = ctx_.resolvedType(*ld->type);
                        if (dt && dt->kind == sema::TypeRef::Kind::Named &&
                            !dt->declaration && dt->astNode && dt->astNode->name &&
                            !dt->astNode->name->parts.empty() &&
                            (dt->headName() == "IEnumerable" ||
                             dt->headName() == "IOwningEnumerable"))
                            views.push_back(ld);
                    }
                }
            }
        }
        // Recurse into nested blocks (mirrors the collect scan above).
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

    // A view qualifies only if it never escapes. Run the escape walk in
    // borrowingArrayMode so a for-in over the view (the common consumer) is a
    // borrow-read; everything else (return/store/capture/owned-arg) still escapes.
    for (const ast::LocalDecl* v : views) {
        StringEscapeWalker w{ctx_, static_cast<const void*>(v)};
        w.borrowingArrayMode = true;
        for (auto& s : body) { w.stmt(s.get()); if (w.escaped) break; }
        if (!w.escaped) out.insert(static_cast<const void*>(v));
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
            // §27.5 / #1135: any outer local referenced through a NESTED lambda
            // inside this body must also be classified as ESCAPING, even when this
            // outer lambda is non-escaping.  A nested lambda may itself escape the
            // outer lambda (e.g. returned from it); outer and inner lambdas share
            // the SAME heap cell for the grandparent local.  If cellFreeEligible_
            // were to free that cell when the outer lambda's env is released, any
            // escaped inner lambda would hold a dangling pointer → use-after-free.
            //
            // Fix: walk the outer lambda's body to find direct nested lambdas
            // (stopping at their boundaries), then collect ALL symbols inside
            // each nested lambda (full descent via walkExpr/walkStmt) and add
            // them to capturedByEscaping.  This prevents cellFreeEligible_ from
            // including such locals, so the cell is never freed at outer-scope exit.
            // Trade-off: a conservative leak (8 bytes/cell) when the inner lambda
            // is provably non-escaping — accepted per the PRIME DIRECTIVE.
            {
                std::unordered_set<const sema::Symbol*> nestedBodySyms;
                std::function<void(const ast::Expr*)> findLamsE;
                std::function<void(const ast::Stmt*)> findLamsS;
                findLamsE = [&](const ast::Expr* e) {
                    if (!e) return;
                    if (auto* nl = dynamic_cast<const ast::LambdaExpr*>(e)) {
                        // Found a nested lambda: collect all symbols in its body.
                        if (nl->exprBody)
                            walkExpr(ctx, nl->exprBody.get(), nestedBodySyms);
                        for (auto& ns : nl->stmts)
                            walkStmt(ctx, ns.get(), nestedBodySyms);
                        return;  // walkExpr descends into deeper nesting levels
                    }
                    if (auto* b = dynamic_cast<const ast::BinaryExpr*>(e)) {
                        findLamsE(b->left.get()); findLamsE(b->right.get()); return; }
                    if (auto* u = dynamic_cast<const ast::UnaryExpr*>(e)) {
                        findLamsE(u->operand.get()); return; }
                    if (auto* p = dynamic_cast<const ast::PostfixBangExpr*>(e)) {
                        findLamsE(p->operand.get()); return; }
                    if (auto* m = dynamic_cast<const ast::MemberAccessExpr*>(e)) {
                        findLamsE(m->target.get()); return; }
                    if (auto* m = dynamic_cast<const ast::NullCondMemberExpr*>(e)) {
                        findLamsE(m->target.get()); return; }
                    if (auto* c = dynamic_cast<const ast::CallExpr*>(e)) {
                        findLamsE(c->callee.get());
                        for (auto& a : c->args) findLamsE(a.value.get()); return; }
                    if (auto* c = dynamic_cast<const ast::NullCondCallExpr*>(e)) {
                        findLamsE(c->callee.get());
                        for (auto& a : c->args) findLamsE(a.value.get()); return; }
                    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(e)) {
                        findLamsE(ix->target.get());
                        for (auto& a : ix->indices) findLamsE(a.value.get()); return; }
                    if (auto* t = dynamic_cast<const ast::TypeTestExpr*>(e)) {
                        findLamsE(t->operand.get()); return; }
                    if (auto* co = dynamic_cast<const ast::ConditionalExpr*>(e)) {
                        findLamsE(co->condition.get()); findLamsE(co->thenValue.get());
                        findLamsE(co->elseValue.get()); return; }
                    if (auto* th = dynamic_cast<const ast::ThrowExpr*>(e)) {
                        findLamsE(th->operand.get()); return; }
                    if (auto* r = dynamic_cast<const ast::RangeExpr*>(e)) {
                        findLamsE(r->lo.get()); findLamsE(r->hi.get());
                        findLamsE(r->step.get()); return; }
                    if (auto* n = dynamic_cast<const ast::NewExpr*>(e)) {
                        for (auto& a : n->args) findLamsE(a.value.get()); return; }
                    if (auto* w = dynamic_cast<const ast::WithExpr*>(e)) {
                        findLamsE(w->source.get());
                        for (auto& a : w->assigns) findLamsE(a.value.get()); return; }
                    if (auto* cl =
                            dynamic_cast<const ast::CollectionLiteralExpr*>(e)) {
                        for (auto& el : cl->elements) findLamsE(el.get());
                        for (auto& k : cl->keys) findLamsE(k.get()); return; }
                    if (auto* is2 =
                            dynamic_cast<const ast::InterpolatedStringExpr*>(e)) {
                        for (auto& part : is2->parts) findLamsE(part.expr.get());
                        return; }
                    if (auto* ce2 = dynamic_cast<const ast::CaseExpr*>(e)) {
                        findLamsE(ce2->scrutinee.get());
                        for (auto& arm : ce2->arms) {
                            findLamsE(arm.guard.get());
                            findLamsE(arm.exprBody.get()); }
                        return; }
                };
                findLamsS = [&](const ast::Stmt* s) {
                    if (!s) return;
                    if (auto* ld = dynamic_cast<const ast::LocalDecl*>(s)) {
                        findLamsE(ld->initializer.get()); return; }
                    if (auto* as = dynamic_cast<const ast::AssignmentStmt*>(s)) {
                        findLamsE(as->target.get()); findLamsE(as->value.get());
                        return; }
                    if (auto* rs = dynamic_cast<const ast::ReturnStmt*>(s)) {
                        findLamsE(rs->value.get()); return; }
                    if (auto* ts = dynamic_cast<const ast::ThrowStmt*>(s)) {
                        findLamsE(ts->value.get()); return; }
                    if (auto* es = dynamic_cast<const ast::ExpressionStmt*>(s)) {
                        findLamsE(es->expr.get()); return; }
                    if (auto* is = dynamic_cast<const ast::IfStmt*>(s)) {
                        findLamsE(is->ifBranch.condition.get());
                        for (auto& b : is->ifBranch.body) findLamsS(b.get());
                        for (auto& br : is->elifBranches) {
                            findLamsE(br.condition.get());
                            for (auto& b : br.body) findLamsS(b.get()); }
                        for (auto& b : is->elseBody) findLamsS(b.get()); return; }
                    if (auto* ws = dynamic_cast<const ast::WhileStmt*>(s)) {
                        findLamsE(ws->condition.get());
                        for (auto& b : ws->body) findLamsS(b.get()); return; }
                    if (auto* frs = dynamic_cast<const ast::ForStmt*>(s)) {
                        if (frs->iterable) findLamsE(frs->iterable.get());
                        for (auto& b : frs->body) findLamsS(b.get()); return; }
                    if (auto* d = dynamic_cast<const ast::DoEndBlock*>(s)) {
                        for (auto& b : d->body) findLamsS(b.get()); return; }
                    if (auto* ts2 = dynamic_cast<const ast::TryStmt*>(s)) {
                        for (auto& b : ts2->body) findLamsS(b.get());
                        for (auto& c : ts2->catches) {
                            findLamsE(c.guard.get());
                            if (c.body) findLamsS(c.body.get()); }
                        for (auto& b : ts2->finallyBody) findLamsS(b.get());
                        return; }
                    if (auto* cs = dynamic_cast<const ast::CaseStmt*>(s)) {
                        findLamsE(cs->scrutinee.get());
                        for (auto& arm : cs->arms) {
                            findLamsE(arm.guard.get());
                            findLamsE(arm.exprBody.get());
                            for (auto& b : arm.stmts) findLamsS(b.get()); }
                        return; }
                };
                if (lam->exprBody) findLamsE(lam->exprBody.get());
                for (auto& s : lam->stmts) findLamsS(s.get());
                // Classify all symbols found in nested lambda bodies as escaping.
                for (const sema::Symbol* s : nestedBodySyms) {
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
    // (a1) §28.3 / §28.10 (v0.180, #1962): a bare-identifier `owned`-marked binding
    // whose (possibly INFERRED) type is a class or interface — `var y := owned x;`
    // / `using var y := owned x;` where the source is interface-typed and the
    // binding is UNANNOTATED. classOfLocal recovers the interface symbol (which has
    // no disposerFns_ entry), and the initializer is neither a `new`/call/operator/
    // `??`/arm-meet shape (a2/b0..b2 below) nor type-annotated (a above), so every
    // branch missed it and the adopted value LEAKED on the NORMAL path. (The throw
    // edge is covered by unwindOwnedLocalClass's mirror branch.) The marker MOVED
    // the sole owner into this binding (the source is left moved-from and marked
    // unwind-ineligible), so a single dynamic (vtable-slot-2) dispose at scope end
    // is exactly-once. A CLASS-typed source is already disposed by the classOfLocal
    // static path (which returns before this function is reached), so this fires
    // only for the interface / erased-generic case — no double-registration.
    // // per §28.3 v0.180 #1962
    if (ld->initializerHasOwnedMarker && ld->initializer &&
        dynamic_cast<const ast::IdentifierExpr*>(ld->initializer.get())) {
        const sema::TypeRef* initTy = ctx_.exprType(*ld->initializer);
        if (isClassOrInterfaceRef(initTy) ||
            isClassOrInterfaceRef(resolveTypeParamRef(initTy)))
            return true;
    }
    // (a2) §28.3 arm-meet (v0.184, #1661): an inferred `using var x := new T()
    // if c else Make();` (both arms fresh owned producers) — an owned-meet
    // if/case initializer. classOfLocal captures no class for an untyped
    // conditional/case initializer (it is neither a NewExpr nor a type
    // annotation), so without this it fell through every branch and leaked.
    // Exactly one arm runs and yields a +1 the caller owns; dispose it once
    // dynamically at scope end. The shared arm-meet predicate is the SAME one
    // the ownership pass consults (producerState), so a borrowed-meet (which
    // pins, usingVarSourceIsBorrow) and a mixed set (rejected in sema) never
    // reach here. A typed owned-meet is disposed by classOfLocal earlier and
    // never reaches this function. // per §28.3
    if (ld->initializer &&
        (dynamic_cast<const ast::ConditionalExpr*>(ld->initializer.get()) ||
         dynamic_cast<const ast::CaseExpr*>(ld->initializer.get())) &&
        common::conditionalCaseArmMeetIsOwned(
            *ld->initializer, ctx_,
            [this](const sema::TypeRef* t) { return resolveTypeParamRef(t); })) {
        const sema::TypeRef* initTy = ctx_.exprType(*ld->initializer);
        if (isClassOrInterfaceRef(initTy) ||
            isClassOrInterfaceRef(resolveTypeParamRef(initTy)))
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
        // OwningTask<T> yields an owned T (§30.5); the CallExpr branch below does
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
                    // (b2u) §28.3 "Factory methods and sourceless returns" /
                    // Appendix C.3 (fix #1499): an unannotated STATIC method
                    // whose EVERY return is a fresh owned producer is treated as
                    // OWNED by the caller — so a `using var x := Make();` (or a
                    // plain `var` the ownership pass flagged autoDropAtScopeEnd)
                    // actually disposes at codegen. Mirrors the sema
                    // producerState classification (Ownership.cpp) exactly (same
                    // callee AST, same expr-type map — only the ignored resolver
                    // differs), so sema and codegen never diverge. Without this,
                    // sema would classify the value Owned (auto-drop warning
                    // fires) while codegen registered no disposal — a leak
                    // surviving a sema-only fix. A borrow-returning static
                    // method (`Get() => _shared`) fails the every-return-fresh
                    // check → returns false → `using var r := Get()` disposes
                    // NOTHING (§28.10; the prior fix's double-free regression).
                    if (md && !isOwned &&
                        common::staticFactoryReturnsFreshOwned(
                            *md, initTy, ctx_,
                            [this](const sema::TypeRef* t) {
                                return resolveTypeParamRef(t);
                            }))
                        return true;
                }
                // (b1f) #1290 / §27.1: the callee may be a FUNCTION-TYPED VALUE
                // (a local or field) whose declared return slot carries `owned`
                // — `mk: () -> owned Res; using var x := mk()`. Its callee is an
                // ordinary value, not a MethodSymbol, so (b1) missed it and the
                // owned result leaked even under `using var`. Detect it by the
                // callee's function type's funcReturnOwned flag; register a
                // dynamic dispose when the result is a class/interface (a
                // value-typed owned return has no heap disposal obligation and
                // stays unregistered).
                if (const sema::TypeRef* cty = ctx_.exprType(*call->callee)) {
                    if (cty->kind == sema::TypeRef::Kind::Function &&
                        cty->funcReturnOwned) {
                        const sema::TypeRef* initTy =
                            ctx_.exprType(*ld->initializer);
                        if (isClassOrInterfaceRef(initTy) ||
                            isClassOrInterfaceRef(resolveTypeParamRef(initTy)))
                            return true;
                    }
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
                    // §2.14 / §4.5 / §4.11 / §29.1 (#550/#642/#746/#1349): the
                    // builtin owned-TRANSFER calls (Pop/Dequeue on owning
                    // stacks/queues, PopLast/PopFirst/PopAt/PopValue,
                    // T.Deserialize, TakeCurrent on owning enumerators) —
                    // recognized by name + owning receiver in the shared
                    // isBuiltinOwnedTransferCall (also used by
                    // unwindOwnedLocalClass for the plain-var forms).
                    if (isBuiltinOwnedTransferCall(*call)) {
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
    // (b0d) §28.3 / #1250: `using var r := t.Result` where `t : OwningTask<T:class>`.
    // The synchronous .Result property on OwningTask<T> extracts the owned T from the
    // task's result buffer and transfers ownership to the caller (§30.3 / §30.5).
    // The initializer is a MemberAccessExpr (not a CallExpr or UnaryExpr await), so
    // cases (b0)/(b1) above missed it; no type annotation, so (a) missed it too.
    // Dispose dynamically through the object's vtable (concrete type may differ from
    // the declared T if T is an interface). This is safe: the alloca is stored with
    // the loaded class pointer in the `result.ok` basic block (established by
    // lowerExpr(tcs.Task.Result) before lowerLocalDecl registers the disposal), and
    // the dynamic dispose null-guards the load — the `result.fault` path bypasses
    // the inner `do` block entirely and never triggers this disposal.
    if (!ld->type) {
        if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                ld->initializer.get())) {
            if (ma->member == "Result" && ma->target) {
                const sema::TypeRef* targetTy = ctx_.exprType(*ma->target);
                if (targetTy && targetTy->kind == sema::TypeRef::Kind::Named &&
                    !targetTy->declaration && targetTy->astNode &&
                    targetTy->astNode->name &&
                    !targetTy->astNode->name->parts.empty() &&
                    targetTy->headName() == "OwningTask" &&
                    !targetTy->genericArgs.empty() && targetTy->genericArgs[0]) {
                    const sema::TypeRef* initTy = ctx_.exprType(*ld->initializer);
                    if (isClassOrInterfaceRef(initTy) ||
                        isClassOrInterfaceRef(resolveTypeParamRef(initTy)))
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
        auto it = genericSubstSemaTy_.find(rt->headName());
        if (it != genericSubstSemaTy_.end())
            return isClassOrInterfaceRef(it->second);
    }
    return false;
}


bool Emitter::usingVarIsDrainedOwningElement(const ast::LocalDecl* ld) {
    // #990 / §4.5 / §14.2: `using var _ := item` where `item` is the loop
    // variable of `for var item in coll.Drain()` over an OWNING sequence
    // collection (OwningArray/Stack/Queue/LinkedList).  Such an element is a class
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
    // OwningDictionary drain (its loop var is a DrainedEntry, handled separately)
    // and the borrowing collections (which yield borrows, not owned elements).
    auto* ce = dynamic_cast<const ast::CallExpr*>(fs->iterable.get());
    if (!ce) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(ce->callee.get());
    if (!ma || ma->member != "Drain" || !ma->target) return false;
    const sema::TypeRef* ct = ctx_.exprType(*ma->target);
    if (!ct || ct->kind != sema::TypeRef::Kind::Named) return false;
    std::string_view cn;
    if (ct->astNode && ct->astNode->name && !ct->astNode->name->parts.empty())
        cn = ct->headName();
    else if (!ct->importedName.empty())
        cn = ct->importedName;
    return cn == "OwningArray" || cn == "OwningStack" ||
           cn == "OwningQueue" || cn == "OwningLinkedList";
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
// heap collection (Array<T> / OwningArray<T> / Stack / OwningStack / Queue /
// OwningQueue). Reuses the SAME runtime construction the `new Array<T>()` path
// emits (bz_array_alloc) and the SAME element store the `.Add(...)` path emits
// (bz_array_add) — so a literal-built array is byte-for-byte identical to one
// built by `new` + `.Add`. Textual element order maps to the array's natural
// construction order (§15.4): Array/OwningArray index 0 = first element; Stack
// last element on top; Queue first element at the front — all achieved by adding
// in textual order to the bz_array_t (which append-stores), matching each type's
// runtime Pop/Dequeue contract. Set/OwningSet are NOT handled here (they are
// hash-backed with dedup, allocated via bz_dict_alloc_generic — a separate
// path); this helper returns null for them so the caller can fall back.
llvm::Value* Emitter::materializeHeapCollectionLiteral(
        const ast::CollectionLiteralExpr* cl, const sema::TypeRef* collTy) {
    if (!cl || cl->isDict || !collTy) return nullptr;

    // §15.4 (R39): the empty literal `[]` parses as a NON-dict
    // CollectionLiteralExpr (the empty form is syntactically ambiguous), but
    // with a Dictionary<K,V>/OwningDictionary<K,V> target it must construct an
    // empty dictionary. Delegate to the dict materializer. Without this the
    // ordered-collection path below returns null for a dict target (it is not
    // an isCollectionTypeName), leaving the local/field slot uninitialized — a
    // `using var d: Dictionary<…> := []` then free()s a garbage pointer
    // (SIGABRT). Every empty-`[]` site calls this helper first, so this single
    // delegation also covers field initializers and `return []`.
    if (cl->elements.empty() && cl->keys.empty() && isDictTypeName(collTy))
        return materializeHeapDictLiteral(cl, collTy);

    // §4.6/§4.7 (#738): LinkedList<T>/OwningLinkedList<T> are bz_llist_t-backed,
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
            if (!ev) {
                // #1386: ICE — lowerExpr returned null for a LinkedList
                // literal element after nested-literal materialization also failed.
                ctx_.diagnostics().report(
                    Severity::Error, elemPtr->span.begin,
                    "internal: lowerExpr returned null for LinkedList "
                    "collection literal element — #1386");
                continue;
            }
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
    // Dispatch Set/OwningSet to the dedicated hash-backed materializer (they use
    // bz_dict_alloc_generic + bz_dict_set_gen, not the array path below).
    std::string_view name = collTy->astNode && collTy->astNode->name &&
                            !collTy->astNode->name->parts.empty()
                                ? collTy->headName()
                                : std::string_view{};
    if (name == "Set" || name == "OwningSet")
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
    // §15.4 L2170 (#1292): if an element expression throws partway, the
    // already-added owned elements and the partially-filled temporary array must
    // be disposed on the exception path (§28.3), exactly as the scope-end
    // disposal would — "as for an OwningArray<U> literal". Route a throwing
    // element eval through a cleanup that frees `arr` with the element disposer
    // matching this collection's ownership (an owning collection's class elements
    // cascade-dispose; an Array<string> releases its co-owned +1; a value array
    // just frees the backing — elementDisposerArg mirrors the scope-end choice),
    // then re-raise. Without this the partial owning array (and its elements)
    // leaked. On the NORMAL path `arr` is returned to the caller, which disposes
    // it; the throw path takes this cleanup instead, so there is no double-free.
    // Installed only around the element-eval loop.
    llvm::BasicBlock* clitSavedUnwind = unwindTarget_;
    std::size_t clitSavedDepth = unwindScopeDepth_;
    {
        auto* clitSavedInsert = builder_.GetInsertBlock();
        llvm::BasicBlock* clitEnclosing = unwindBB();
        bool clitOwnsElems = common::isOwningPrefixedName(name);
        auto* clitCleanup =
            llvm::BasicBlock::Create(llvm_, "clit.elem.cleanup", currentFn_);
        builder_.SetInsertPoint(clitCleanup);
        auto* clitFreeFn = declareC("bz_array_free",
            llvm::Type::getVoidTy(llvm_), {ptrTy(), ptrTy()});
        builder_.CreateCall(clitFreeFn,
            {arr, elementDisposerArg(elemRef, clitOwnsElems)});
        builder_.CreateBr(clitEnclosing);
        builder_.SetInsertPoint(clitSavedInsert);
        unwindTarget_ = clitCleanup;
        unwindScopeDepth_ = disposeStack_.size();
    }
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
        if (!ev) {
            // #1386: ICE — lowerExpr returned null for a heap collection
            // literal element after nested-literal materialization also failed.
            ctx_.diagnostics().report(
                Severity::Error, elemPtr->span.begin,
                "internal: lowerExpr returned null for heap collection "
                "literal element — #1386");
            continue;
        }
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
    // #1292: restore the enclosing unwind target now the element evals are done.
    unwindTarget_ = clitSavedUnwind;
    unwindScopeDepth_ = clitSavedDepth;
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
// materializeHeapDictLiteral; every sequence form (Array/OwningArray, Set/OwningSet,
// Stack/Queue and owned, LinkedList/OwningLinkedList) routes to
// materializeHeapCollectionLiteral (which itself dispatches Set and LinkedList).
// Returns a `ptr` to the constructed heap collection, or null when `collTy` is
// not a recognised heap-collection target (caller declines without miscompiling).
llvm::Value* Emitter::materializeCollectionLiteralForArg(
        const ast::CollectionLiteralExpr* cl, const sema::TypeRef* collTy) {
    if (!cl || !collTy) return nullptr;
    // Dictionary literal `[k: v, …]` (or empty `[]` with a dict target).
    if (cl->isDict || isDictTypeName(collTy))
        return materializeHeapDictLiteral(cl, collTy);
    // Every sequence-literal form (Array/OwningArray/Set/OwningSet/Stack/Queue and
    // owned forms, LinkedList/OwningLinkedList). materializeHeapCollectionLiteral
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

// #1501 / stdlib §6.7 (line 2884) / §30.10: build (once) a concrete
// `Array<Task>` sema::TypeRef for the Task.WhenAny/WhenAll `tasks` argument.
// `Task.WhenAny(tasks: Array<Task>)` and `Task.WhenAll(tasks: Array<Task>)`
// are compiler intrinsics with no MethodDecl params, so — exactly like the
// byte-array FFI factories (#999) — sema never assigns the literal argument
// a target type, and a bare `lowerExpr` on a CollectionLiteralExpr returns
// null (§15.4: a collection literal is not a general expression value). The
// element type is a bare `Task` (Named, no declaration, no generic args) —
// mirrors sema's `makeTaskTypeRef` (ExprTypecheck_Calls.cpp) — since a
// borrowing `Array<Task>` element is an opaque task handle regardless of the
// awaited T; `lowerTypeRef`/`collectionElemSize` resolve it to a bare `ptr`.
const sema::TypeRef* Emitter::builtinTaskArrayTypeRef() {
    static const ast::Type* kArrayAstNode = []() -> ast::Type* {
        auto* qn = new ast::QualifiedName;
        qn->parts.push_back("Array");
        auto* t = new ast::Type;
        t->kind = ast::TypeKind::Named;
        t->name.reset(qn);
        return t;
    }();
    static const ast::Type* kTaskAstNode = []() -> ast::Type* {
        auto* qn = new ast::QualifiedName;
        qn->parts.push_back("Task");
        auto* t = new ast::Type;
        t->kind = ast::TypeKind::Named;
        t->name.reset(qn);
        return t;
    }();
    auto* elem = ctx_.makeTypeRef();
    elem->kind = sema::TypeRef::Kind::Named;
    elem->declaration = nullptr;
    elem->astNode = kTaskAstNode;
    auto* tr = ctx_.makeTypeRef();
    tr->kind = sema::TypeRef::Kind::Named;
    tr->declaration = nullptr;
    tr->astNode = kArrayAstNode;
    tr->genericArgs.push_back(elem);
    return tr;
}

// #1501 / stdlib §6.7 (line 2884) / §30.10: lower the `tasks` argument of
// Task.WhenAny/WhenAll. When the argument is an inline collection literal
// `[t1, t2, …]` (the natural "select" idiom pairing WhenAny with per-branch
// Receive tasks), materialize it into an owned `Array<Task>` temporary via the
// same construction machinery a `using var tasks: Array<Task> := […]` local
// uses. Returns {value, isLiteralTemp}: the caller (WhenAny/WhenAll lowering)
// must free a literal temp SYNCHRONOUSLY, immediately after the runtime call
// that consumes it — NOT via scope-end disposeStack_ registration, and NOT
// via a spilled `using var`-style local. Reason: bz_task_when_any/bz_task_
// when_all read every element out of the array synchronously (bz_array_get in
// their loop) before returning and never retain the array pointer, so the
// temp has no use beyond that one call — but an intervening `await` (the
// aggregate is essentially always awaited next) can genuinely suspend the
// enclosing async state machine, and collectSuspendLocals (LlvmBackend_
// Iterators.cpp) only scans AST-level `LocalDeclStmt`s to decide which
// allocas get a frame field spilled/restored across a suspend/resume
// boundary (LlvmBackend_Async.cpp emitAwaitSuspend). A synthetic temp with no
// LocalDeclStmt is invisible to that pass, so a deferred (scope-end) free
// read back an uninitialized/reused native stack slot from the FRESH stack
// frame of the resumed continuation — bz_array_free on garbage, observed as a
// flaky `free(): invalid pointer` abort. Freeing immediately, before any
// suspend point can occur, sidesteps the hazard entirely. A non-literal
// argument (an existing `Array<Task>` local or expression, unaffected by any
// of this — its own `using var`/registerUnwindOwnedLocal disposal already
// correctly spills/restores) is plain lowerExpr, isLiteralTemp=false.
std::pair<llvm::Value*, bool> Emitter::lowerTaskArrayArg(
        const ast::Expr& argExpr) {
    if (auto* cl = dynamic_cast<const ast::CollectionLiteralExpr*>(&argExpr)) {
        llvm::Value* arr =
            materializeCollectionLiteralForArg(cl, builtinTaskArrayTypeRef());
        return {arr, arr != nullptr};
    }
    return {lowerExpr(argExpr), false};
}

// #1501: free a literal temp `Array<Task>` built by lowerTaskArrayArg,
// synchronously, right after the runtime call that borrowed it — see
// lowerTaskArrayArg's comment for why this must NOT be a deferred
// (scope-end) disposal. The array holds borrowed (non-owning) Task element
// pointers (bare `Task`'s ElementPolicy is Value/no-disposer — see
// builtinTaskArrayTypeRef), so freeing it never touches the individual tasks.
void Emitter::freeTaskArrayArgTemp(llvm::Value* arr) {
    auto* freeFn = declareC("bz_array_free", llvm::Type::getVoidTy(llvm_),
                            {ptrTy(), ptrTy()});
    builder_.CreateCall(freeFn,
        {arr, llvm::ConstantPointerNull::get(ptrTy())});
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
        common::isOwningPrefixedName(collTy->headName());
    auto isStrElem = [&](const sema::TypeRef* el, bool owning) {
        if (!el || el->nullable) return false;
        ElementPolicy p = elementPolicyFor(el, owning);
        return p.cat == ElementPolicy::Category::RuntimeManaged &&
               el->kind == sema::TypeRef::Kind::Primitive &&
               el->primitive == sema::PrimitiveType::String;
    };

    // LinkedList / OwningLinkedList → bz_llist_free.
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

    // Dictionary / OwningDictionary → bz_dict_free.
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

    // #1012 / §4.10: a dict-backed Set/OwningSet (a generic-dict-eligible element —
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
        (collTy->headName() == "Set" ||
         collTy->headName() == "OwningSet");
    bool setDictBacked = isSetName &&
        genericBackingType(collTy, 0, /*isSet=*/true, nullptr) != nullptr;
    if (setDictBacked) {
        Disposal d{alloca, nullptr};
        d.dict = true;
        if (isStrElem(el, /*owning=*/false)) d.dictStringKeys = true;
        return d;
    }

    // Array-backed: Array/OwningArray/Set/OwningSet/Stack/Queue and owned forms
    // → bz_array_free with the per-element disposer.
    Disposal d{alloca, nullptr};
    d.collection = true;
    d.collectionElemDisposer = elementPolicyFor(el, isOwning).disposerThunk;
    if (!d.collectionElemDisposer && isStrElem(el, /*owning=*/false))
        d.collectionElemDisposer = stringElementReleaseThunk();
    return d;
}


// #559 / stdlib §4.10: materialize a Set<T>/OwningSet<T> sequence literal
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
// Set/OwningSet TypeRef with a recognised element type (caller then falls
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
        if (!ev) {
            // #1386: ICE — lowerExpr returned null for a Set/OwningSet
            // literal element; the expression kind is not yet handled.
            ctx_.diagnostics().report(
                Severity::Error, elemPtr->span.begin,
                "internal: lowerExpr returned null for Set/OwningSet "
                "literal element — #1386");
            continue;
        }
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
// into a freshly-allocated Dictionary<K,V> (or OwningDictionary<K,V>).
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
// recognized Dictionary/OwningDictionary TypeRef.
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
        if (!k || !v) {
            // #1386: ICE — lowerExpr returned null for a dict literal key or
            // value; the expression kind is not yet handled by codegen.
            ctx_.diagnostics().report(
                Severity::Error,
                (!k && cl->keys[i]) ? cl->keys[i]->span.begin
                                    : cl->elements[i]->span.begin,
                "internal: lowerExpr returned null for dictionary literal "
                "key or value — #1386");
            continue;
        }
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


// per §4.6/§4.7: recover a local's LinkedList<T>/OwningLinkedList<T> TypeRef.
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
    // #394: recover a local's resolved Dictionary/OwningDictionary TypeRef (with
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


// stdlib §6.7 / §30.5: true when `rt` is a Task / Task<T> or OwningTask<T> —
// a builtin runtime-managed async task type with no user TypeDecl. Freed via
// bz_task_free. Recognises the names "Task" and "OwningTask" in a TypeRef that
// has no user declaration (declaration == null or declaration->decl == null
// with name "Task"/"OwningTask") and no user TypeDecl entry.
bool Emitter::isTaskTypeName(const sema::TypeRef* rt) {
    if (!rt || rt->kind != sema::TypeRef::Kind::Named) return false;
    // Path A: no declaration at all — name is in astNode->name->parts[0].
    if (!rt->declaration) {
        if (rt->astNode && rt->astNode->name &&
            !rt->astNode->name->parts.empty()) {
            auto n = rt->headName();
            return n == "Task" || n == "OwningTask";
        }
        return false;
    }
    // Path B: builtinClassSym — a declaration with no user-defined decl and
    // whose name is "Task" or "OwningTask".
    if (!rt->declaration->decl) {
        return rt->declaration->name == "Task" ||
               rt->declaration->name == "OwningTask";
    }
    return false;
}

// stdlib §6.7 / §30.5: recover the Task/Task<T>/OwningTask<T> TypeRef for a
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

// §30.15 (v0.179, #1524): recognise `ld`'s initializer as a no-arg `Receive()`
// call on a `Channel<T>` receiver — the exact same syntactic shape
// tryLowerChannelAndDomainCall matches when lowering the call
// (LlvmBackend_Calls3.cpp: `mname == "Receive" && call.args.empty()` on a
// receiver whose builtinTypeName is "Channel"). Re-checked here (rather than
// threaded through as codegen state) because disposal registration runs in a
// separate pass over the LocalDecl, with only the AST/type info available.
bool Emitter::isChannelReceiveInitializer(const ast::LocalDecl* ld) {
    if (!ld->initializer) return false;
    auto* call = dynamic_cast<const ast::CallExpr*>(ld->initializer.get());
    if (!call || !call->callee || !call->args.empty()) return false;
    auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(call->callee.get());
    if (!ma || !ma->target || ma->member != "Receive") return false;
    auto* recvTy = ctx_.exprType(*ma->target);
    return recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
           builtinTypeName(recvTy) == "Channel";
}

// §28.3 v0.190 SB-01 (#1786 / #1870): a task-typed local that merely BORROWS
// the frame — its initializer is a bare-identifier ALIAS of another task
// binding (`var t2 := t;`), or a READ of task-typed STORAGE: a field/property
// access (`obj.taskField`, incl. the null-conditional `obj?.taskField`) or an
// indexer read (`arr[i]`) — does NOT own the task frame. Under "calls mint,
// storage lends" the OWNER is the mint CALL that produced the frame; it
// disposes it exactly once. Registering a scope-end `d.task` for such a borrow
// too would delete the SAME `bz_task_t*` a second time — a double-free / UAF
// that corrupts async continuation state (symptom: a hang; ASan traps no report
// before it). Mirrors producerState (src/ownership/Ownership.cpp): only a
// CallExpr on the task-family-mint path returns Owned — a MemberAccessExpr /
// NullCondMemberExpr / IndexExpr / IdentifierExpr never reaches that block and
// stays Borrowed ("storage lends"). Recognised syntactically here (this pass
// walks the LocalDecl with only AST/type info), the same way
// isChannelReceiveInitializer above matches its shape. // per §28.3 v0.190
// SB-01 (storage lends)
bool Emitter::taskLocalInitializerIsBorrowSource(const ast::LocalDecl* ld) {
    const ast::Expr* init = ld->initializer.get();
    if (!init) return false;
    return dynamic_cast<const ast::IdentifierExpr*>(init) != nullptr ||
           dynamic_cast<const ast::MemberAccessExpr*>(init) != nullptr ||
           dynamic_cast<const ast::NullCondMemberExpr*>(init) != nullptr ||
           dynamic_cast<const ast::IndexExpr*>(init) != nullptr;
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
                            rt->headName());
                        if (it != genericSubstSemaTy_.end() && it->second &&
                            it->second->kind == sema::TypeRef::Kind::Named)
                            return it->second->declaration;
                    }
                }
            }
        }
    }
    // (Before v0.188 this function also looked through a WithExpr initializer
    // — `using var h := new C() with f := v end` — to find the class for
    // registerUsingDisposal, #747 R43. That was the CLASS object-initializer
    // reading of `with` (former §22.11), which proposal 0037 retired: a
    // class-typed `with` source is now a compile error (§9.5), so a `using`
    // initializer of that shape can no longer reach codegen. Retired with it
    // — #1735.)
    // #639 / §28.11: `using var u := item` where `item` is a drain-loop
    // variable of user class type (an IdentifierExpr with no explicit
    // type annotation). The ownership checker confirms the transfer; here
    // we just need to recover the concrete class from the identifier's
    // sema-resolved exprType so that registerUsingDisposal can push a
    // static disposal entry (destructor runs at scope end).
    if (!ld->type &&
        dynamic_cast<const ast::IdentifierExpr*>(ld->initializer.get())) {
        // §28.10 (#1274): a `using var` initialized from a bare identifier that
        // resolves to a BORROW — a non-`owned` parameter — does NOT assume a
        // disposal obligation: the value is owned elsewhere (the caller), which
        // disposes it. Returning the class here would register a SECOND disposal
        // at this scope's end → a double-free (the borrowed object is freed here
        // and again by its real owner). Only an OWNED source transfers ownership
        // to the using var (an `owned`/`out owned` parameter — kept below — a
        // drain-loop element or a moved owned local, which are LocalSymbols and
        // unaffected by this parameter check). The canonical borrow forms
        // (`using var c := obj.f`, `:= methodReturningBorrow()`) are
        // MemberAccess/Call exprs, not bare identifiers, and already return null.
        if (auto* ps = dynamic_cast<sema::ParameterSymbol*>(
                ctx_.exprSymbol(*ld->initializer))) {
            if (ps->modifier != "owned" && ps->modifier != "out owned")
                return nullptr;
        }
        // §28.10 (#1285): the borrow-LOCAL sibling of the parameter check above.
        // A `using var c := b` where `b` is a local that aliases a borrow holds
        // no ownership obligation — the ownership pass flags it; skip disposal
        // so the borrowed value is not freed here and again by its real owner.
        if (ld->usingVarSourceIsBorrow)
            return nullptr;
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


// §8.6 / §2.5–§2.6 (#1945): the box-key name for a boxable VALUE-BUILTIN by its
// spelling. The scalar/struct value builtins (DateTime/TimeSpan → i64,
// DateOnly/TimeOnly → i32, Guid → {i64,i64}) box into Object like a record; a
// CancellationToken lowers to a pointer (reference-like) and Range/spans are
// separate value shapes, so none of those are boxed here (not the #1945 defect).
static const char* valueBuiltinBoxNameStr(std::string_view n) {
    if (n == "DateTime")  return "DateTime";
    if (n == "TimeSpan")  return "TimeSpan";
    if (n == "DateOnly")  return "DateOnly";
    if (n == "TimeOnly")  return "TimeOnly";
    if (n == "Guid")      return "Guid";
    return nullptr;
}


// §2.5–§2.6 (#1945/#1954): the value-builtin name (DateTime/TimeSpan/DateOnly/
// TimeOnly/Guid) a TypeRef denotes, resolved across the three shapes value-builtins
// take (builtinClassSym decl with decl==nullptr; declaration-less annotation via
// astNode headName; imported via importedName), IGNORING nullability. Shared by
// valueBuiltinBoxName (which additionally excludes nullable) and
// isNullableValueBuiltinTy (which requires nullable), so the value-builtin name set
// lives in ONE place.
const char* Emitter::valueBuiltinName(const sema::TypeRef* rt) {
    using K = sema::TypeRef::Kind;
    if (!rt || rt->kind != K::Named) return nullptr;
    // (a) builtinClassSym form: a compiler-intrinsic TypeSymbol (decl==nullptr).
    if (rt->declaration)
        return rt->declaration->decl ? nullptr
                                     : valueBuiltinBoxNameStr(rt->declaration->name);
    // (b) annotation form: declaration null, name via astNode headName.
    if (rt->astNode && rt->astNode->name && !rt->astNode->name->parts.empty())
        return valueBuiltinBoxNameStr(rt->headName());
    // (c) imported form: declaration null, no astNode, importedName set.
    if (!rt->importedName.empty())
        return valueBuiltinBoxNameStr(rt->importedName);
    return nullptr;
}


// §8.6 / §2.5–§2.6 (#1945): the box-key name for a boxable value-builtin TypeRef.
// Nullable is excluded — a `DateTime?` → Object box is a separate shape; a nullable
// value-builtin's tagged-struct layout is keyed by isNullableValueBuiltinTy instead.
const char* Emitter::valueBuiltinBoxName(const sema::TypeRef* rt) {
    if (!rt || rt->nullable) return nullptr;
    return valueBuiltinName(rt);
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
        // §8.6 / §5.1 (#1311): a boxable VALUE primitive (not string — string
        // is a class reaching Object/interface through the class itable path)
        // implements IComparable<T>/IEquatable<T> (stdlib §9.1–§9.5). Build an
        // itable/itmap so dispatch on a builtin generic interface variable
        // holding the boxed primitive reaches a real CompareTo/Equals; null
        // itmap (the previous behaviour) crashed itable lookup (bz_itable_lookup
        // dereferenced the null vtable[1]). Mirrors ensureSpanBoxVtable.
        llvm::Constant* itmapSlot = llvm::ConstantPointerNull::get(ptrTy());
        if (p != P::String) {
            if (llvm::GlobalVariable* itmap = buildPrimBoxItmap(p, nm))
                itmapSlot = llvm::ConstantExpr::getPointerCast(itmap, ptrTy());
        }
        std::vector<llvm::Constant*> vtElems(kVirtualMethodsBase,
            llvm::ConstantPointerNull::get(ptrTy()));
        vtElems[0] = desc;
        vtElems[1] = itmapSlot;  // itmap (interfaces the primitive implements)
        if (builtinDisposeFn_)
            vtElems[2] = llvm::cast<llvm::Constant>(builtinDisposeFn_);  // Dispose
        vtElems[3] = tostr;          // ToString
        vtElems[4] = eqThunk;        // Equals
        vtElems[5] = hashThunk;      // GetHashCode
        auto* vtTy = llvm::ArrayType::get(ptrTy(), kVirtualMethodsBase);
        primBoxVtable_[nm] = new llvm::GlobalVariable(
            *module_, vtTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(vtTy, vtElems),
            std::string("pvt$") + nm);
        allVtables_.push_back(primBoxVtable_[nm]);  // §8.6 #417
    }

    // --- Value-builtins: descriptor + box vtable (#1945) ------------------
    // §8.6 / §2.5–§2.6: DateTime/TimeSpan/DateOnly/TimeOnly/Guid are scalar/struct
    // value types (i64 / i32 / {i64,i64}) that box into Object exactly like a
    // record. Without a box descriptor a boxed value-builtin carried no walkable
    // type tag, so `case o of DateTime` / `o is DateTime` never matched (and the
    // §19.3 type-parameter form was a documented never-match, #1852); and because
    // isBoxableValueType missed them, boxValue was a no-op, leaving the raw scalar
    // in an Object slot → a malformed-IR call in some shapes. Give each a box
    // vtable (keyed into primBoxDesc_/primBoxVtable_ by name — the keys never
    // collide with the primitive spellings). CRITICALLY, the box's type tag
    // (vtable[0]) must be the value-builtin's EXISTING class descriptor (`td$<nm>`
    // — every value-builtin has a compiler class TypeSymbol in descArray_ from
    // declareVtables) so it compares equal in emitIsA to the descriptor that
    // descriptorForPatternName / boxDescriptorFor resolve by NAME for the type-test
    // target; a freshly-minted descriptor would be a distinct global and never
    // match. A `[self, null]` descriptor is minted only as a fallback if the
    // builtin somehow lacks a class descriptor. The vtable interface slot points at
    // a shared empty (null-terminated) itmap so a stray itable lookup finds nothing
    // rather than dereferencing a null itmap (§26.5 crash the primitive block warns
    // about).
    static const char* const kValueBuiltins[] = {
        "DateTime", "TimeSpan", "DateOnly", "TimeOnly", "Guid"};
    // One shared empty itmap `[null]` for every value-builtin box vtable.
    llvm::GlobalVariable* vbEmptyItmap = nullptr;
    for (const char* nm : kValueBuiltins) {
        if (primBoxDesc_.count(nm)) continue;
        // Reuse the value-builtin's class descriptor (td$<nm>) so box and type-test
        // agree; fall back to a minted [self, null] descriptor only if absent.
        llvm::GlobalVariable* desc = nullptr;
        for (const auto& sym : ctx_.symbols()) {
            auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
            if (ts && ts->name == nm &&
                (ts->kind == ast::TypeDeclKind::Class ||
                 ts->kind == ast::TypeDeclKind::Interface)) {
                auto dit = descArray_.find(ts);
                if (dit != descArray_.end()) { desc = dit->second; break; }
            }
        }
        if (!desc) {
            auto* descTy = llvm::ArrayType::get(ptrTy(), 2);
            auto* minted = new llvm::GlobalVariable(
                *module_, descTy, /*isConstant=*/true,
                llvm::GlobalValue::InternalLinkage, nullptr,
                std::string("ptd$") + nm);
            minted->setInitializer(llvm::ConstantArray::get(
                descTy, {minted, llvm::ConstantPointerNull::get(ptrTy())}));
            desc = minted;
        }
        primBoxDesc_[nm] = desc;
        if (!vbEmptyItmap) {
            auto* itmapTy = llvm::ArrayType::get(ptrTy(), 1);
            vbEmptyItmap = new llvm::GlobalVariable(
                *module_, itmapTy, /*isConstant=*/true,
                llvm::GlobalValue::InternalLinkage,
                llvm::ConstantArray::get(
                    itmapTy, {llvm::ConstantPointerNull::get(ptrTy())}),
                "itmap$box$valuebuiltin");
        }
        std::vector<llvm::Constant*> vtElems(
            kVirtualMethodsBase, llvm::ConstantPointerNull::get(ptrTy()));
        vtElems[0] = desc;                                        // type descriptor
        vtElems[1] = llvm::ConstantExpr::getPointerCast(vbEmptyItmap, ptrTy());
        if (builtinDisposeFn_)
            vtElems[2] = llvm::cast<llvm::Constant>(builtinDisposeFn_);  // Dispose
        vtElems[kToStringSlot]    = valueBuiltinBoxToStringThunk(nm);
        vtElems[kEqualsSlot]      = boxDefaultEqualsThunk();
        vtElems[kGetHashCodeSlot] = boxDefaultHashCodeThunk();
        auto* vtTy = llvm::ArrayType::get(ptrTy(), kVirtualMethodsBase);
        primBoxVtable_[nm] = new llvm::GlobalVariable(
            *module_, vtTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(vtTy, vtElems),
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


// §8.6 / §2.5 (#1945): the ToString thunk for a boxed VALUE-BUILTIN. A boxed
// DateTime/Guid/… has no user ToString and no per-type formatter here (formatting
// belongs to the value-type's own methods, out of scope for the box-descriptor
// fix), so the box-vtable slot returns the type's name — mirroring the class
// default (§8.5 "the type's qualified name"). This exists so vtable slot 3 is a
// valid function (a null slot would fault if `Object.ToString()` is dispatched on
// a boxed value-builtin). Signature is the vtable-uniform `ptr(ptr this)`.
llvm::Constant* Emitter::valueBuiltinBoxToStringThunk(const char* name) {
    auto* fnTy = llvm::FunctionType::get(ptrTy(), {ptrTy()}, /*isVarArg=*/false);
    auto* thunk = llvm::Function::Create(
        fnTy, llvm::Function::InternalLinkage,
        std::string("bz$vbtostr$") + name, module_.get());
    auto* entry = llvm::BasicBlock::Create(llvm_, "entry", thunk);
    {
        ThunkScope scope(currentFn_, builder_, thunk, entry);
        builder_.CreateRet(
            lowerStringLiteral(std::string("\"") + name + "\"", false));
    }
    return thunk;
}


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
            // per §10.5 (v0.183): a user `Equals(E)` replaces the §8.3 default
            // (underlying-integer comparison) for boxed dispatch.
            llvm::Type* ety = enumLlvmType(ts);
            llvm::Value* sv = builder_.CreateLoad(ety, selfPayload,  "beq.sv");
            llvm::Value* ov = builder_.CreateLoad(ety, otherPayload, "beq.ov");
            eq = emitEnumEquals(sv, ov, ts);
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
            // per §10.5 (v0.183): a user `GetHashCode(): int` replaces the §8.3
            // default (hash of the underlying integer) for boxed dispatch.
            llvm::Type* ety = enumLlvmType(ts);
            llvm::Value* ev = builder_.CreateLoad(ety, payload, "bhash.ev");
            h = emitEnumHashCode(ev, ts);
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


// §8.6 / §5.1 (#1311): build the interface itable/itmap for a boxed PRIMITIVE
// so dispatch through a builtin generic interface it implements
// (IComparable<T>/IEquatable<T>, stdlib §9.1–§9.5) reaches a real
// CompareTo/Equals. Each boxable primitive (other than Boolean, which
// implements IEquatable<bool> only — §9.4) implements IComparable<T> AND
// IEquatable<T> where T is the primitive's own alias. The itable slots hold
// freshly-built thunks whose signature is `int/bool(ptr this, T other)`: they
// load the scalar payload from the box (offset 16, the same slot
// primBoxEqualsThunk reads) and run the same signed/unsigned/ordered-FP
// comparison the DIRECT primitive CompareTo/Equals lowering uses
// (LlvmBackend_Exprs.cpp, fix #586 / #863). At dispatch time the itable
// call's needsConcrete reconciliation builds the matching fnTy from the real
// arg types, so the i32-erased interface signature never mis-types the call.
// Mirrors ensureSpanBoxVtable's itable/itmap assembly. Returns nullptr when
// the builtin interface descriptors are unavailable (so the box vtable keeps
// its null itmap and dispatch on the interface bails rather than crashes).
llvm::GlobalVariable* Emitter::buildPrimBoxItmap(sema::PrimitiveType prim,
                                                 const char* nm) {
    using P = sema::PrimitiveType;
    // Boolean implements IEquatable<bool> only (§9.4 — no IComparable<bool>).
    bool hasCmp = prim != P::Bool;

    // Locate the builtin interface TypeSymbols and their descriptors.
    auto* cmpSym = ctx_.builtinInterfaceSym("IComparable");
    auto* eqSym  = ctx_.builtinInterfaceSym("IEquatable");
    auto cmpDescIt = hasCmp && cmpSym ? descArray_.find(cmpSym) : descArray_.end();
    auto eqDescIt  = eqSym ? descArray_.find(eqSym) : descArray_.end();
    bool haveCmp = hasCmp && cmpDescIt != descArray_.end();
    bool haveEq  = eqDescIt != descArray_.end();
    if (!haveCmp && !haveEq) return nullptr;

    llvm::Type* valTy = lowerPrimitive(prim);
    if (!valTy) return nullptr;

    // Comparison class — mirrors the direct primitive CompareTo/Equals lowering.
    bool isSignedInt   = prim == P::Int   || prim == P::Long  ||
                         prim == P::Short || prim == P::SByte ||
                         prim == P::Char;
    bool isUnsignedInt = prim == P::UInt  || prim == P::ULong ||
                         prim == P::UShort|| prim == P::Byte;
    bool isFP          = prim == P::Double || prim == P::Float;

    // Helper: load the scalar payload from the box (offset 16 = GEP index 2 on
    // a byte-strided ptr, the same slot primBoxEqualsThunk/primBoxHashThunk
    // read). `tb` is a standalone builder positioned at the thunk's entry.
    auto loadPayload = [&](llvm::IRBuilder<>& tb,
                           llvm::Value* box) -> llvm::Value* {
        llvm::Value* pay = tb.CreateGEP(
            ptrTy(), box, {llvm::ConstantInt::get(i32(), 2)}, "pbcmp.pay");
        return tb.CreateLoad(valTy, pay, "pbcmp.val");
    };

    // --- CompareTo thunk: i32(ptr this, T other) — (a>b) - (a<b) → -1/0/1 ---
    llvm::Function* cmpFn = nullptr;
    if (haveCmp) {
        auto* fnTy = llvm::FunctionType::get(i32(), {ptrTy(), valTy}, false);
        cmpFn = llvm::Function::Create(
            fnTy, llvm::Function::InternalLinkage,
            std::string("bz$primcmp$") + nm, module_.get());
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", cmpFn);
        llvm::IRBuilder<> tb(entry);
        llvm::Value* self  = cmpFn->getArg(0);
        llvm::Value* other = cmpFn->getArg(1);
        llvm::Value* a = loadPayload(tb, self);
        llvm::Value* b = other;
        llvm::Value* gt, *lt;
        if (isFP) {
            gt = tb.CreateFCmpOGT(a, b, "pbcmp.gt");
            lt = tb.CreateFCmpOLT(a, b, "pbcmp.lt");
        } else if (isUnsignedInt) {
            gt = tb.CreateICmpUGT(a, b, "pbcmp.gt");
            lt = tb.CreateICmpULT(a, b, "pbcmp.lt");
        } else {
            gt = tb.CreateICmpSGT(a, b, "pbcmp.gt");
            lt = tb.CreateICmpSLT(a, b, "pbcmp.lt");
        }
        llvm::Value* gtI = tb.CreateZExt(gt, i32(), "pbcmp.gti");
        llvm::Value* ltI = tb.CreateZExt(lt, i32(), "pbcmp.lti");
        tb.CreateRet(tb.CreateSub(gtI, ltI, "pbcmp.res"));
    }

    // --- Equals thunk: i1(ptr this, T other) — payload == other ---
    llvm::Function* eqFn = nullptr;
    if (haveEq) {
        auto* fnTy = llvm::FunctionType::get(i1(), {ptrTy(), valTy}, false);
        eqFn = llvm::Function::Create(
            fnTy, llvm::Function::InternalLinkage,
            std::string("bz$primeqT$") + nm, module_.get());
        auto* entry = llvm::BasicBlock::Create(llvm_, "entry", eqFn);
        llvm::IRBuilder<> tb(entry);
        llvm::Value* self  = eqFn->getArg(0);
        llvm::Value* other = eqFn->getArg(1);
        llvm::Value* a = loadPayload(tb, self);
        llvm::Value* r = isFP ? tb.CreateFCmpOEQ(a, other, "pbcmp.eq")
                             : tb.CreateICmpEQ(a, other, "pbcmp.eq");
        tb.CreateRet(r);
    }

    // --- Assemble itables + itmap (mirrors ensureSpanBoxVtable) ---
    auto buildItable = [&](llvm::Function* fn,
                           const char* iname) -> llvm::GlobalVariable* {
        auto* arrTy = llvm::ArrayType::get(ptrTy(), 1);
        return new llvm::GlobalVariable(
            *module_, arrTy, /*isConstant=*/true,
            llvm::GlobalValue::InternalLinkage,
            llvm::ConstantArray::get(
                arrTy, {llvm::cast<llvm::Constant>(fn)}),
            std::string("itab$prim$") + nm + "$" + iname);
    };

    std::vector<llvm::Constant*> pairs;  // [td$I, itab, …]
    if (haveCmp) {
        auto* itab = buildItable(cmpFn, "IComparable");
        pairs.push_back(llvm::cast<llvm::Constant>(cmpDescIt->second));
        pairs.push_back(llvm::ConstantExpr::getPointerCast(itab, ptrTy()));
    }
    if (haveEq) {
        auto* itab = buildItable(eqFn, "IEquatable");
        pairs.push_back(llvm::cast<llvm::Constant>(eqDescIt->second));
        pairs.push_back(llvm::ConstantExpr::getPointerCast(itab, ptrTy()));
    }
    pairs.push_back(llvm::ConstantPointerNull::get(ptrTy()));  // null terminator

    auto* itmapArrTy = llvm::ArrayType::get(ptrTy(), pairs.size());
    auto* itmapGV = new llvm::GlobalVariable(
        *module_, itmapArrTy, /*isConstant=*/true,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(itmapArrTy, pairs),
        std::string("itmap$prim$") + nm);
    return itmapGV;
}


// per §10.5 (v0.183): find a user-declared plain (non-static, non-override)
// enum member `name` of the given arity — `ToString()`/`GetHashCode()` (0
// params) or `Equals(E)` (1 param). Returns null when the enum declares no such
// member (so the caller emits the §8.5/§8.3 default).
sema::MethodSymbol* Emitter::enumUserMember(const sema::TypeSymbol* ts,
                                            std::string_view name,
                                            size_t nParams) {
    if (!ts || ts->kind != ast::TypeDeclKind::Enum) return nullptr;
    for (sema::Symbol* sym : ts->members) {
        auto* ms = dynamic_cast<sema::MethodSymbol*>(sym);
        if (ms && !isStaticMethod(ms) && ms->name == name &&
            ms->params.size() == nParams)
            return ms;
    }
    return nullptr;
}

// per §10.5 (v0.183): `enumVal` (the enum's underlying integer, §10.3) → its
// string form. A user `ToString(): string` replaces the §8.5 default (the
// type's qualified name) for the interpolation and boxed-dispatch consumers,
// matching the direct-call behavior.
llvm::Value* Emitter::emitEnumToString(llvm::Value* enumVal,
                                       const sema::TypeSymbol* ts) {
    if (sema::MethodSymbol* user = enumUserMember(ts, "ToString", 0)) {
        auto fit = methodFns_.find(user);
        if (fit != methodFns_.end()) {
            llvm::FunctionType* fty = fit->second->getFunctionType();
            llvm::Type* thisTy = fty->getParamType(0);
            if (enumVal->getType() != thisTy &&
                enumVal->getType()->isIntegerTy() && thisTy->isIntegerTy())
                enumVal = builder_.CreateZExtOrTrunc(enumVal, thisTy);
            return builder_.CreateCall(fty, fit->second, {enumVal});
        }
    }
    return lowerStringLiteral(
        std::string("\"") + std::string(ts->name) + "\"", false);
}

// per §10.5 (v0.183): equality of two enum values (`a`, `b` are the underlying
// integers). A user `Equals(E): bool` replaces the §8.3 default (integer
// comparison) for the boxed-dispatch and hash-container consumers.
llvm::Value* Emitter::emitEnumEquals(llvm::Value* a, llvm::Value* b,
                                     const sema::TypeSymbol* ts) {
    if (sema::MethodSymbol* user = enumUserMember(ts, "Equals", 1)) {
        auto fit = methodFns_.find(user);
        if (fit != methodFns_.end()) {
            llvm::FunctionType* fty = fit->second->getFunctionType();
            llvm::Type* selfTy  = fty->getParamType(0);
            llvm::Type* otherTy = fty->getParamType(1);
            // §10.5's sanctioned form is `Equals(E)`: both `this` and the
            // parameter lower to the enum's underlying integer. Dispatch only
            // when that holds (a non-integer parameter — e.g. an `Object?`
            // signature — would need boxing; leave that to the default rather
            // than emit a type-mismatched call).
            if (selfTy->isIntegerTy() && otherTy->isIntegerTy()) {
                if (a->getType() != selfTy && a->getType()->isIntegerTy())
                    a = builder_.CreateZExtOrTrunc(a, selfTy);
                if (b->getType() != otherTy && b->getType()->isIntegerTy())
                    b = builder_.CreateZExtOrTrunc(b, otherTy);
                return builder_.CreateCall(fty, fit->second, {a, b});
            }
        }
    }
    if (a->getType() != b->getType() && a->getType()->isIntegerTy() &&
        b->getType()->isIntegerTy())
        b = builder_.CreateZExtOrTrunc(b, a->getType());
    return builder_.CreateICmpEQ(a, b);
}

// per §10.5 (v0.183): hash of an enum value (`enumVal` is the underlying
// integer) → i32. A user `GetHashCode(): int` replaces the §8.3 default (a
// hash of the integer value) for the boxed-dispatch and hash-container
// consumers.
llvm::Value* Emitter::emitEnumHashCode(llvm::Value* enumVal,
                                       const sema::TypeSymbol* ts) {
    if (sema::MethodSymbol* user = enumUserMember(ts, "GetHashCode", 0)) {
        auto fit = methodFns_.find(user);
        if (fit != methodFns_.end()) {
            llvm::FunctionType* fty = fit->second->getFunctionType();
            llvm::Type* thisTy = fty->getParamType(0);
            if (enumVal->getType() != thisTy &&
                enumVal->getType()->isIntegerTy() && thisTy->isIntegerTy())
                enumVal = builder_.CreateZExtOrTrunc(enumVal, thisTy);
            llvm::Value* h = builder_.CreateCall(fty, fit->second, {enumVal});
            if (h->getType() != i32())
                h = builder_.CreateZExtOrTrunc(h, i32());
            return h;
        }
    }
    // §8.3 default: hash by the underlying integer value (emitValueHashCode
    // folds a wide underlying type the same way the record/box path did).
    sema::TypeRef enumRef;
    enumRef.kind        = sema::TypeRef::Kind::Named;
    enumRef.declaration = const_cast<sema::TypeSymbol*>(ts);
    llvm::Value* h = emitValueHashCode(enumVal, &enumRef);
    if (h->getType() != i32())
        h = builder_.CreateZExtOrTrunc(h, i32());
    return h;
}

// §8.5 / §8.6 (#417): the uniform ToString thunk for a boxed RECORD or boxed
// ENUM. The receiver is the BOX pointer { vtable*, i64 rc, payload }; the
// payload starts two words in (offset 16, BZ_BOX layout). For a record the
// thunk prefers a user `ToString()` override and otherwise emits the
// synthesized §8.5/§9.1/§9.2 property-listing form. For an enum the thunk
// dispatches to a user `ToString()` (per §10.5) else returns the type name.
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
            // per §10.5 (v0.183): a user `ToString(): string` replaces the §8.5
            // default (the type's qualified name) for boxed dispatch too. Load
            // the enum's underlying integer from the box payload (two words in)
            // and route through the shared user-or-default helper.
            llvm::Value* boxPtr = thunk->getArg(0);
            llvm::Value* payload = builder_.CreateGEP(
                ptrTy(), boxPtr, {builder_.getInt32(2)}, "box.valptr");
            llvm::Value* enumVal = builder_.CreateLoad(
                enumLlvmType(ts), payload, "box.enumval");
            result = emitEnumToString(enumVal, ts);
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
    // #1843 / §8.6 #417: the table must list EVERY vtable an object can carry —
    // including per-monomorphization specialized vtables (vt$C$specN), which are
    // created LAZILY during body lowering, AFTER a call site here would freeze an
    // eagerly-built table. bz_obj_is_string treats an object whose word-0 vtable
    // is NOT in the table as a string, so a missing specialized vtable makes
    // `is`/`as` on that instance silently fail (the #1843 first attempt regressed
    // the matching case this way). Emit a PLACEHOLDER global now (so body lowering
    // has a stable pointer to pass) and swap in the real, complete array at
    // finalizeVtableTable() once allVtables_ is fully populated.
    auto* emptyTy = llvm::ArrayType::get(ptrTy(), 0);
    vtableTableGV_ = new llvm::GlobalVariable(
        *module_, emptyTy, /*isConstant=*/true,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(emptyTy, {}), "bz$vtable$table$pending");
    return vtableTableGV_;
}


// #1843 / §8.6 #417: build the real `bz$vtable$table` from the now-complete
// allVtables_ and redirect every use of the placeholder to it. Called once at
// the end of run(), after all bodies/ctors/disposers (hence all lazily-created
// specialized/span/box vtables) exist. A no-op when no discriminator was ever
// needed (vtableTablePtr never called).
void Emitter::finalizeVtableTable() {
    if (!vtableTableGV_) return;
    std::vector<llvm::Constant*> entries;
    for (llvm::GlobalVariable* gv : allVtables_)
        entries.push_back(llvm::ConstantExpr::getPointerCast(gv, ptrTy()));
    entries.push_back(llvm::ConstantPointerNull::get(ptrTy()));  // terminator
    auto* arrTy = llvm::ArrayType::get(ptrTy(), entries.size());
    auto* real = new llvm::GlobalVariable(
        *module_, arrTy, /*isConstant=*/true,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(arrTy, entries), "bz$vtable$table");
    // Both the placeholder and the real global are `ptr`-typed values (only ever
    // passed as an opaque pointer to bz_obj_is_string), so RAUW is well-typed.
    vtableTableGV_->replaceAllUsesWith(real);
    vtableTableGV_->eraseFromParent();
    vtableTableGV_ = real;
}


// §33.3 / #1366: publish (once) a null-terminated table of the built-in
// exception vtables. bz_unhandled_exception uses it to gate the §31.4 field
// reads (message@8, CauseChain@24) to objects that actually have that layout.
llvm::Value* Emitter::builtinExcVtableTablePtr() {
    if (excVtableTableGV_) return excVtableTableGV_;
    std::vector<llvm::Constant*> entries;
    for (const auto& [name, gv] : builtinVtable_)
        entries.push_back(llvm::ConstantExpr::getPointerCast(gv, ptrTy()));
    entries.push_back(llvm::ConstantPointerNull::get(ptrTy()));  // terminator
    auto* arrTy = llvm::ArrayType::get(ptrTy(), entries.size());
    excVtableTableGV_ = new llvm::GlobalVariable(
        *module_, arrTy, /*isConstant=*/true,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(arrTy, entries), "bz$exc$vtable$table");
    return excVtableTableGV_;
}


// §4.16 / §8.6 (#970): synthesize (or return cached) the box vtable for a span
// boxed into IReadOnlySpan<T>. `elemTy` is the concrete LLVM element type
// (i32 for int, ptr for string/class, i64 for long, etc.).
//
// Box layout (inherited from §8.6 record boxing):
//   { vtable*, i64 rc, bz.arrayspan{ptr,i32,i32} }
//   GEP(box, 2) → bz.arrayspan slot.
//
// Itable slot order (must match declareVtables Pass 0 for IReadOnlySpan):
//   [0] IndexOf       (ptr box, elemTy item) → i32
//   [1] Contains      (ptr box, elemTy item) → i1
//   [2] SequenceEqual (ptr box, ptr other)   → i1
//   [3] Length getter (ptr box) → i32
//   [4] IsEmpty getter (ptr box) → i1
//   [5] Indexer getter (ptr box, i32 i) → elemTy
//
// Each thunk loads the bz.arrayspan from `box+16`, extracts src/start/len.
// `src` is a bz_array_t* (NOT a raw element pointer). For element access,
// load src->data (the first field of bz_array_t, at offset 0) to get the
// raw element buffer, then GEP into that buffer. Length/IsEmpty use `len`
// directly from the span struct (pre-computed by AsReadOnlySpan/AsSpan).
llvm::GlobalVariable* Emitter::ensureSpanBoxVtable(llvm::Type* elemTy) {
    auto cached = spanBoxVtables_.find(elemTy);
    if (cached != spanBoxVtables_.end()) return cached->second;

    // Locate the IReadOnlySpan TypeSymbol and its descriptor.
    sema::TypeSymbol* irsSym = ctx_.builtinInterfaceSym("IReadOnlySpan");
    auto descIt = irsSym ? descArray_.find(irsSym) : descArray_.end();
    if (!irsSym || descIt == descArray_.end()) {
        spanBoxVtables_[elemTy] = nullptr;
        return nullptr;
    }
    llvm::GlobalVariable* irsDesc = descIt->second;

    auto* spanSt = arraySpanStructType();  // {ptr, i32, i32}

    // Helper: build a standalone function body. Receives a builder already
    // positioned at the entry BB. Returns {src, start, len} from box arg 0.
    // `tb` is the builder; `boxArg` is the first arg (the box ptr).
    auto loadSpan = [&](llvm::IRBuilder<>& tb,
                        llvm::Value* boxArg) -> std::tuple<llvm::Value*,
                                                           llvm::Value*,
                                                           llvm::Value*> {
        // GEP past vtable*/rc to the span struct slot (offset = 2 pointers = 16B).
        llvm::Value* spanPtr = tb.CreateGEP(
            ptrTy(), boxArg, {llvm::ConstantInt::get(i32(), 2)}, "span.ptr");
        llvm::Value* spanVal = tb.CreateLoad(spanSt, spanPtr, "span");
        llvm::Value* src   = tb.CreateExtractValue(spanVal, {0u}, "src");
        llvm::Value* start = tb.CreateExtractValue(spanVal, {1u}, "start");
        llvm::Value* len   = tb.CreateExtractValue(spanVal, {2u}, "len");
        return {src, start, len};
    };

    // Helper: build an element-comparison predicate. For pointer elements
    // (class/interface/string) compare as pointers; for integers compare as
    // integers. Returns an i1.
    auto elemEq = [&](llvm::IRBuilder<>& tb,
                      llvm::Value* a, llvm::Value* b) -> llvm::Value* {
        if (elemTy->isPointerTy())
            return tb.CreateICmpEQ(a, b, "eq");
        return tb.CreateICmpEQ(a, b, "eq");
    };

    // --- [0] IndexOf(ptr box, elemTy item) → i32 ---
    // Linear scan: return k where src[start+k] == item, else -1.
    // src is bz_array_t*; its first field (offset 0) is the data pointer
    // (void* data = the raw element buffer). Load it once at entry.
    llvm::Function* indexOfFn;
    {
        auto* fnTy = llvm::FunctionType::get(i32(), {ptrTy(), elemTy}, false);
        indexOfFn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                           "bzspan$IndexOf", module_.get());
        auto ai = indexOfFn->arg_begin();
        llvm::Value* boxArg  = &*ai++;
        llvm::Value* itemArg = &*ai;

        auto* entryBB   = llvm::BasicBlock::Create(llvm_, "entry",   indexOfFn);
        auto* loopBB    = llvm::BasicBlock::Create(llvm_, "loop",    indexOfFn);
        auto* bodyBB    = llvm::BasicBlock::Create(llvm_, "body",    indexOfFn);
        auto* contBB    = llvm::BasicBlock::Create(llvm_, "cont",    indexOfFn);
        auto* foundBB   = llvm::BasicBlock::Create(llvm_, "found",   indexOfFn);
        auto* notFndBB  = llvm::BasicBlock::Create(llvm_, "notfnd",  indexOfFn);

        llvm::IRBuilder<> tb(entryBB);
        auto [src, start, len] = loadSpan(tb, boxArg);
        // src = bz_array_t*; data = src->data (element buffer, offset 0).
        llvm::Value* data = tb.CreateLoad(ptrTy(), src, "data");
        tb.CreateBr(loopBB);

        tb.SetInsertPoint(loopBB);
        auto* kPhi = tb.CreatePHI(i32(), 2, "k");
        kPhi->addIncoming(tb.getInt32(0), entryBB);
        llvm::Value* inRange = tb.CreateICmpULT(kPhi, len, "inrange");
        tb.CreateCondBr(inRange, bodyBB, notFndBB);

        tb.SetInsertPoint(bodyBB);
        llvm::Value* offset = tb.CreateAdd(start, kPhi, "off");
        llvm::Value* ptr    = tb.CreateGEP(elemTy, data, {offset}, "ep");
        llvm::Value* val    = tb.CreateLoad(elemTy, ptr, "v");
        llvm::Value* match  = elemEq(tb, val, itemArg);
        tb.CreateCondBr(match, foundBB, contBB);

        tb.SetInsertPoint(contBB);
        llvm::Value* k1 = tb.CreateAdd(kPhi, tb.getInt32(1), "k1");
        kPhi->addIncoming(k1, contBB);
        tb.CreateBr(loopBB);

        tb.SetInsertPoint(foundBB);
        tb.CreateRet(kPhi);

        tb.SetInsertPoint(notFndBB);
        tb.CreateRet(llvm::ConstantInt::get(i32(), -1));
    }

    // --- [1] Contains(ptr box, elemTy item) → i1 ---
    llvm::Function* containsFn;
    {
        auto* fnTy = llvm::FunctionType::get(i1(), {ptrTy(), elemTy}, false);
        containsFn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                            "bzspan$Contains", module_.get());
        auto ai = containsFn->arg_begin();
        llvm::Value* boxArg  = &*ai++;
        llvm::Value* itemArg = &*ai;
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", containsFn);
        llvm::IRBuilder<> tb(bb);
        // call IndexOf: result != -1
        auto* ioFnTy = indexOfFn->getFunctionType();
        llvm::Value* idx = tb.CreateCall(ioFnTy, indexOfFn, {boxArg, itemArg},
                                         "idx");
        llvm::Value* found = tb.CreateICmpNE(idx,
            llvm::ConstantInt::get(i32(), -1), "found");
        tb.CreateRet(found);
    }

    // --- [2] SequenceEqual(ptr box, ptr other_box) → i1 ---
    // Caller passes `other` as a ptr (IReadOnlySpan<T> box); needsConcrete
    // reconciles the i32-erased itable signature at dispatch time.
    llvm::Function* seqEqFn;
    {
        auto* fnTy = llvm::FunctionType::get(i1(), {ptrTy(), ptrTy()}, false);
        seqEqFn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                         "bzspan$SequenceEqual", module_.get());
        auto ai = seqEqFn->arg_begin();
        llvm::Value* selfBox  = &*ai++;
        llvm::Value* otherBox = &*ai;

        auto* entryBB  = llvm::BasicBlock::Create(llvm_, "entry",  seqEqFn);
        auto* loopBB   = llvm::BasicBlock::Create(llvm_, "loop",   seqEqFn);
        auto* bodyBB   = llvm::BasicBlock::Create(llvm_, "body",   seqEqFn);
        auto* trueBB   = llvm::BasicBlock::Create(llvm_, "ret1",   seqEqFn);
        auto* falseBB  = llvm::BasicBlock::Create(llvm_, "ret0",   seqEqFn);

        llvm::IRBuilder<> tb(entryBB);
        auto [sSrc, sStart, sLen] = loadSpan(tb, selfBox);
        auto [oSrc, oStart, oLen] = loadSpan(tb, otherBox);
        // src = bz_array_t*; data = src->data (element buffer, offset 0).
        llvm::Value* sData = tb.CreateLoad(ptrTy(), sSrc, "sdata");
        llvm::Value* oData = tb.CreateLoad(ptrTy(), oSrc, "odata");
        llvm::Value* lenEq = tb.CreateICmpEQ(sLen, oLen, "leneq");
        tb.CreateCondBr(lenEq, loopBB, falseBB);

        tb.SetInsertPoint(loopBB);
        auto* kPhi = tb.CreatePHI(i32(), 2, "k");
        kPhi->addIncoming(tb.getInt32(0), entryBB);
        llvm::Value* inRange = tb.CreateICmpULT(kPhi, sLen, "inrange");
        tb.CreateCondBr(inRange, bodyBB, trueBB);

        tb.SetInsertPoint(bodyBB);
        llvm::Value* sOff = tb.CreateAdd(sStart, kPhi, "soff");
        llvm::Value* oOff = tb.CreateAdd(oStart, kPhi, "ooff");
        llvm::Value* sEp  = tb.CreateGEP(elemTy, sData, {sOff}, "sep");
        llvm::Value* oEp  = tb.CreateGEP(elemTy, oData, {oOff}, "oep");
        llvm::Value* sV   = tb.CreateLoad(elemTy, sEp, "sv");
        llvm::Value* oV   = tb.CreateLoad(elemTy, oEp, "ov");
        llvm::Value* same = elemEq(tb, sV, oV);
        llvm::Value* k1   = tb.CreateAdd(kPhi, tb.getInt32(1), "k1");
        kPhi->addIncoming(k1, bodyBB);  // loop back when same
        tb.CreateCondBr(same, loopBB, falseBB);

        tb.SetInsertPoint(trueBB);
        tb.CreateRet(llvm::ConstantInt::get(i1(), 1));

        tb.SetInsertPoint(falseBB);
        tb.CreateRet(llvm::ConstantInt::get(i1(), 0));
    }

    // --- [3] Length getter (ptr box) → i32 ---
    llvm::Function* lenGetFn;
    {
        auto* fnTy = llvm::FunctionType::get(i32(), {ptrTy()}, false);
        lenGetFn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                          "bzspan$getLength", module_.get());
        auto ai = lenGetFn->arg_begin();
        llvm::Value* boxArg = &*ai;
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", lenGetFn);
        llvm::IRBuilder<> tb(bb);
        auto [src, start, len] = loadSpan(tb, boxArg);
        (void)src; (void)start;
        tb.CreateRet(len);
    }

    // --- [4] IsEmpty getter (ptr box) → i1 ---
    llvm::Function* isEmptyFn;
    {
        auto* fnTy = llvm::FunctionType::get(i1(), {ptrTy()}, false);
        isEmptyFn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                           "bzspan$getIsEmpty", module_.get());
        auto ai = isEmptyFn->arg_begin();
        llvm::Value* boxArg = &*ai;
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", isEmptyFn);
        llvm::IRBuilder<> tb(bb);
        auto [src, start, len] = loadSpan(tb, boxArg);
        (void)src; (void)start;
        llvm::Value* empty = tb.CreateICmpEQ(len, tb.getInt32(0), "empty");
        tb.CreateRet(empty);
    }

    // --- [5] Indexer getter (ptr box, i32 i) → elemTy ---
    // src = bz_array_t*; its first field (offset 0) is the data pointer
    // (void* data = the raw element buffer). Read data, then GEP into it.
    llvm::Function* idxGetFn;
    {
        auto* fnTy = llvm::FunctionType::get(elemTy, {ptrTy(), i32()}, false);
        idxGetFn = llvm::Function::Create(fnTy, llvm::Function::InternalLinkage,
                                          "bzspan$getAt", module_.get());
        auto ai = idxGetFn->arg_begin();
        llvm::Value* boxArg = &*ai++;
        llvm::Value* idxArg = &*ai;
        auto* bb = llvm::BasicBlock::Create(llvm_, "entry", idxGetFn);
        llvm::IRBuilder<> tb(bb);
        auto [src, start, len] = loadSpan(tb, boxArg);
        (void)len;
        llvm::Value* data   = tb.CreateLoad(ptrTy(), src, "data");
        llvm::Value* offset = tb.CreateAdd(start, idxArg, "off");
        llvm::Value* ep     = tb.CreateGEP(elemTy, data, {offset}, "ep");
        llvm::Value* val    = tb.CreateLoad(elemTy, ep, "v");
        tb.CreateRet(val);
    }

    // --- Build itable [IndexOf, Contains, SequenceEqual, Length, IsEmpty, Indexer] ---
    std::vector<llvm::Constant*> itabElems = {
        indexOfFn, containsFn, seqEqFn, lenGetFn, isEmptyFn, idxGetFn
    };
    auto* itabArrTy = llvm::ArrayType::get(ptrTy(), itabElems.size());
    auto* itabGV = new llvm::GlobalVariable(
        *module_, itabArrTy, /*isConstant=*/true,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(itabArrTy, itabElems),
        "itab$spanbox$irs");

    // --- Build itmap: [td$IReadOnlySpan, itab, null] ---
    std::vector<llvm::Constant*> itmapElems = {
        llvm::cast<llvm::Constant>(irsDesc),
        llvm::ConstantExpr::getPointerCast(itabGV, ptrTy()),
        llvm::ConstantPointerNull::get(ptrTy())
    };
    auto* itmapArrTy = llvm::ArrayType::get(ptrTy(), itmapElems.size());
    auto* itmapGV = new llvm::GlobalVariable(
        *module_, itmapArrTy, /*isConstant=*/true,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(itmapArrTy, itmapElems),
        "itmap$spanbox");

    // --- Build a minimal span-box descriptor: [self, null] ---
    auto* descArrTy = llvm::ArrayType::get(ptrTy(), 2);
    auto* boxDesc = new llvm::GlobalVariable(
        *module_, descArrTy, /*isConstant=*/true,
        llvm::GlobalValue::InternalLinkage, nullptr, "btd$spanbox");
    boxDesc->setInitializer(llvm::ConstantArray::get(
        descArrTy, {boxDesc, llvm::ConstantPointerNull::get(ptrTy())}));

    // --- Build box vtable ---
    std::vector<llvm::Constant*> vtElems(kVirtualMethodsBase,
                                          llvm::ConstantPointerNull::get(ptrTy()));
    vtElems[0] = boxDesc;
    vtElems[1] = llvm::ConstantExpr::getPointerCast(itmapGV, ptrTy());
    if (builtinDisposeFn_) vtElems[2] = builtinDisposeFn_;
    vtElems[kEqualsSlot]      = boxDefaultEqualsThunk();
    vtElems[kGetHashCodeSlot] = boxDefaultHashCodeThunk();
    auto* vtArrTy = llvm::ArrayType::get(ptrTy(), vtElems.size());
    auto* vtGV = new llvm::GlobalVariable(
        *module_, vtArrTy, /*isConstant=*/true,
        llvm::GlobalValue::InternalLinkage,
        llvm::ConstantArray::get(vtArrTy, vtElems),
        "bvt$spanbox");
    allVtables_.push_back(vtGV);

    spanBoxVtables_[elemTy] = vtGV;
    return vtGV;
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
    // §8.6 / §2.5–§2.6 (#1945): value-builtins (DateTime/TimeSpan/Guid/DateOnly/
    // TimeOnly) are scalar/struct value types that box into Object like a record.
    // Handled in BOTH TypeRef shapes (builtinClassSym decl and declaration-less
    // annotation/imported) before the record/enum decl gate — without this the
    // raw scalar was left in an Object slot (no box → malformed-IR call in some
    // shapes) and carried no box descriptor (`case o of DateTime` never matched).
    if (valueBuiltinBoxName(rt)) return true;
    if (rt->kind == K::Named && rt->declaration)
        return rt->declaration->kind == ast::TypeDeclKind::Record ||
               rt->declaration->kind == ast::TypeDeclKind::Enum;
    // §4.16 / §8.6 (#970): span value structs box into IReadOnlySpan<T>.
    if (isSpanLikeTypeName(rt)) return true;
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
    // §8.6 / §2.5 (#1945): a value-builtin's box descriptor (primBoxDesc_, keyed
    // by the builtin name). Checked FIRST so a builtinClassSym-shaped value-
    // builtin (declaration set but no descArray_ entry) resolves to its box
    // descriptor rather than the `Named && declaration` miss below.
    if (const char* vb = valueBuiltinBoxName(rt)) {
        auto it = primBoxDesc_.find(vb);
        return it != primBoxDesc_.end() ? it->second : nullptr;
    }
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
    // §8.6 / §2.5 (#1945): a value-builtin's box vtable (primBoxVtable_, keyed by
    // the builtin name). Checked FIRST so boxValue finds a vtable for a value-
    // builtin source and produces a proper box (rather than the no-op that left
    // the raw scalar in an Object slot).
    if (const char* vb = valueBuiltinBoxName(rt)) {
        auto it = primBoxVtable_.find(vb);
        return it != primBoxVtable_.end() ? it->second : nullptr;
    }
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
    // §4.16 / §8.6 (#970): span types box into IReadOnlySpan<T>.
    if (isSpanLikeTypeName(rt)) {
        llvm::Type* elemTy = i32();  // fallback: int element
        if (!rt->genericArgs.empty()) {
            llvm::Type* et = lowerTypeRef(rt->genericArgs[0]);
            if (et) elemTy = et;
        }
        return ensureSpanBoxVtable(elemTy);
    }
    return nullptr;
}


}  // namespace bezel::codegen
