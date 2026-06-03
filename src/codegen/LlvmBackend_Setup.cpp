#include "codegen/LlvmBackend_impl.hpp"

namespace bezel::codegen {

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

    // CG13: declare a Dispose function `void(this)` for every class.
    for (const auto& sym : ctx_.symbols()) {
        auto* ts = dynamic_cast<const sema::TypeSymbol*>(sym.get());
        if (!ts || ts->kind != ast::TypeDeclKind::Class) continue;
        auto* fnType = llvm::FunctionType::get(
            llvm::Type::getVoidTy(llvm_), {ptrTy()}, /*isVarArg=*/false);
        auto* fn = llvm::Function::Create(
            fnType, llvm::Function::ExternalLinkage,
            "bz$" + std::string(ts->name) + "$Dispose", module_.get());
        disposerFns_[ts] = fn;
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


void Emitter::emitThrowValue(llvm::Value* excVal) {
    if (excVal) {
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
    unwindScopeDepth_ = 0;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));
    bool terminated = false;
    if (const ast::DestructorDecl* dd = findDestructor(ts)) {
        terminated = lowerScopedBlock(dd->body);
    }
    // §28.4: after the destructor body, dispose owned fields (reverse
    // declaration order), then chain to the base class's Dispose.
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
        emitDynamicDispose(val);
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
    for (std::size_t i = disposeStack_.size(); i-- > unwindScopeDepth_;)
        emitDisposals(disposeStack_[i]);
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
    // vtable layout: [0]=type descriptor, [1]=Dispose, [2+]=methods.
    llvm::Value* vptr = builder_.CreateLoad(ptrTy(), obj);
    llvm::Value* slot = builder_.CreateGEP(ptrTy(), vptr,
                                           {builder_.getInt32(1)});
    llvm::Value* disposeFn = builder_.CreateLoad(ptrTy(), slot);
    auto* fnTy = llvm::FunctionType::get(llvm::Type::getVoidTy(llvm_),
                                         {ptrTy()}, /*isVarArg=*/false);
    builder_.CreateCall(fnTy, disposeFn, {obj});
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


void Emitter::emitBaseDisposeChain(const sema::TypeSymbol* ts) {
    for (const sema::TypeSymbol* base : ts->baseTypes) {
        if (!base || base->kind != ast::TypeDeclKind::Class) continue;
        auto it = disposerFns_.find(base);
        if (it != disposerFns_.end()) builder_.CreateCall(it->second,
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
    unwindScopeDepth_ = 0;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));

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
    currentThis_ = nullptr;
    locals_.clear();
    paramSlots_.clear();
    refParams_.clear();
    disposeStack_.clear();
    loopStack_.clear(); finallyStack_.clear();
    propagateBB_ = nullptr;
    unwindTarget_ = nullptr;
    currentCaughtExc_ = nullptr;
    unwindScopeDepth_ = 0;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));
    if (!lowerScopedBlock(cd->body)) builder_.CreateRetVoid();
}


void Emitter::emitConstructorBody(sema::ConstructorSymbol* cs, llvm::Function* fn) {
    auto* cd = dynamic_cast<const ast::ConstructorDecl*>(cs->decl);
    if (!cd) return;
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
    unwindScopeDepth_ = 0;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(llvm_, "entry", fn));
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
    llvm::Type* declaredTy = declType ? lowerType(declType) : nullptr;
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
        // Dict/Set disposal is not wired in this slice; leave as-is (the
        // pre-existing behaviour) to avoid changing other test outcomes.
    }

    const sema::TypeSymbol* cls = classOfLocal(ld);
    if (!cls) return;
    if (disposerFns_.find(cls) == disposerFns_.end()) return;
    disposeStack_.back().push_back({it->second, cls});
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
    bool terminated = false;
    for (std::size_t i = startIdx; i < body.size(); ++i) {
        if (body[i] && lowerStmt(*body[i])) { terminated = true; break; }
    }
    if (!terminated) emitDisposals(disposeStack_.back());
    disposeStack_.pop_back();
    return terminated;
}


void Emitter::emitDisposals(const std::vector<Disposal>& scope) {
    for (auto it = scope.rbegin(); it != scope.rend(); ++it) {
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
        if (disposerFns_.find(it->cls) == disposerFns_.end()) continue;
        // CG18: dispatch Dispose dynamically through the object's
        // vtable, so a `using var` bound to a subclass instance runs
        // the correct runtime Dispose.
        // §27.5: a captured ("boxed") `using var` local lives in a shared
        // heap cell; load the object through the cell, not the cell pointer.
        llvm::Type* ety = nullptr;
        llvm::Value* addr = boxedStorage(it->slot, ety);
        llvm::Value* obj = builder_.CreateLoad(ptrTy(), addr);
        emitDynamicDispose(obj);
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


}  // namespace bezel::codegen
