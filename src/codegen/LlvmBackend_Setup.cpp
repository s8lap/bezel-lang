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
    if (!cd || !cd->value) return nullptr;
    return lowerExpr(*cd->value);
}


void Emitter::declareStatics() {
    for (const auto& sym : ctx_.symbols()) {
        auto* fs = dynamic_cast<const sema::FieldSymbol*>(sym.get());
        if (!fs || !isStaticField(fs)) continue;
        auto* fd = dynamic_cast<const ast::FieldDecl*>(fs->decl);
        llvm::Type* ty = fd ? lowerType(fd->type.get()) : i32();
        std::string cls = fs->parent ? std::string(fs->parent->name) : "";
        auto* gv = new llvm::GlobalVariable(
            *module_, ty, /*isConstant=*/false,
            llvm::GlobalValue::InternalLinkage,
            constInitFor(fd, ty),
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
    llvm::Constant* strVal = llvm::ConstantStruct::get(stringType(), {dataGV, lenC, widC});
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
        emitImplicitReturn(currentFn_);
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
    if (ci.bodyKind == ast::MethodBodyKind::Block && ci.stmts)
        collectUnwindIneligible(*ci.stmts);

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
    // #394 / stdlib §4.9: a `using var` bound to a hash-backed dictionary
    // (Dictionary<K,V>/OwnedDictionary<K,V>). Dicts are excluded from
    // isCollectionTypeName (they free via bz_dict_free, not bz_array_free), so
    // they get their own disposal kind here. For an OwnedDictionary with a class
    // VALUE type the disposal cascades into each surviving value (the #394 leak
    // fixed here); a borrowing Dictionary (or a value-typed V) frees only the
    // backing table and its key bytes. Keys are never disposed (§4.9: keys are
    // value-like; the dictionary owns its values only).
    if (const sema::TypeRef* dictTy = dictTypeOfLocal(ld)) {
        Disposal d{it->second, nullptr};
        d.dict = true;
        d.dictValueDisposer = owningDictValueDisposer(dictTy);
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
        // the BOX pointer whose first word is the vtable; the record bytes start
        // one word in. So the box vtable stores a small THUNK that advances
        // `this` past the vtable word, then tail-calls the real method. This
        // keeps the class-dispatch convention (this at offset 0) untouched.
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
            // this (arg 0): box ptr → record bytes (+1 word).
            llvm::Value* recThis = tb.CreateGEP(
                ptrTy(), &*ai, {llvm::ConstantInt::get(i32(), 1)}, "boxthis");
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
