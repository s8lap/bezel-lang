#include "codegen/LlvmBackend_impl.hpp"
#include "common/OwnershipFacts.hpp"  // fix #3189: calleeReturnsOwned — the M2 owned-return channel
#include "sema/GenericParams.hpp"  // fix #3143: the ONE both-sources generic-param walk (§14.1/§12.9/§2.5)

#include <limits>

// §4.4 fix #521: decimal literal compile-time parse.
// bz_decimal_parse_cstr is compiled into the same process (the runtime is linked
// into bezel_tests and also available in the driver binary).  Calling it from the
// compiler during code generation is safe for both AOT and JIT modes.
// per fix #521: ove_vtable/fe_vtable are nullptr here because literals are
// already validated by the lexer; the cstr call cannot fail at compile time.
extern "C" void bz_decimal_parse_cstr(const char* s, void* out,
                                       void* ove_vtable, void* fe_vtable);

namespace bezel::codegen {

// fix #2733 / §28.3 *Unbound owned temporaries* (v0.240): the K8 full-expression
// boundary hook. `lowerExprInner` (below) is the real lowering; this wrapper
// tracks nesting depth so that the OUTERMOST lowerExpr call — which is exactly a
// K8 full expression, since every statement expression, initializer, condition,
// `for` source and `case` scrutinee reaches lowering as a fresh top-level call —
// discharges the FFI buffer temporaries produced inside it. Hooking here rather
// than at statement granularity is what gives K8.2 ("Each **evaluation** of a
// condition, `when` arm condition, or pattern guard is its own boundary … so
// condition temporaries are disposed per evaluation and never accumulate across
// iterations") for free: a loop condition is its own top-level lowerExpr.
// A NESTED FUNCTION BODY is a boundary too, and finding that out cost a real
// leak: `lambda(): int => strlen(CString.From(s))` is lowered while the ENCLOSING
// expression is still open, so nothing in the lambda's own function ever sees
// depth 0. Measured at 64 x 85 B leaked before this clause was added — the
// disposeStack_ backstop does not cover it either, because a nested-function
// emitter does not leave the enclosing scope stack in place for the body.
// Detecting it needs no hook at any of the ~95 `currentFn_ =` sites: a lowerExpr
// frame whose function differs from its PARENT frame's is by construction the
// outermost expression of a different function, hence that function's own K8
// boundary. `fullExprFn_` carries the parent frame's function down.
llvm::Value* Emitter::lowerExpr(const ast::Expr& e) {
    llvm::Function* parentFn = fullExprFn_;
    llvm::Function* thisFn = currentFn_;
    fullExprFn_ = thisFn;
    ++fullExprDepth_;
    llvm::Value* v = lowerExprInner(e);
    --fullExprDepth_;
    fullExprFn_ = parentFn;
    const bool atBoundary = (fullExprDepth_ == 0 || parentFn != thisFn);
    if (!ffiExprTemps_.empty() && atBoundary)
        flushFfiExprTemps();
    // fix #3070 / §30.5 + §28.3 (v0.249): the same K8 boundary discharges an
    // awaited Pattern A task temporary — `Process(await Producer())` disposes the
    // task "at the expression's end, after every use of the borrow" (§30.5), not
    // at the await.
    if (!awaitedTaskTemps_.empty() && atBoundary)
        flushAwaitedTaskTemps();
    // fix #3421 (spec §4.7 *The unit type*, §21.1 *Variable declarations*,
    // v0.283): ONE representation for "this expression produced no lowered
    // value". LLVM `void` is not a value — it has no size and no alignment, so
    // it cannot be stored, alloca'd, boxed, or passed — and the backend is
    // already written throughout against the invariant that a unit-typed
    // expression lowers to `nullptr`: the `()` LITERAL has always returned null
    // here, and every unit rescue downstream keys on `!v` (`boxForTarget`'s
    // #2915 arm, its #3095 `()?` sibling, `unitValueForTarget`,
    // `rescueUnitAssignRhs`, `lowerLocalDecl`'s #2434 zero-init fallback).
    //
    // A DIRECT call to a method declared without a return type broke that
    // invariant. §4.7 makes such a call's type unit ("It is the return type of
    // methods declared without an explicit return type"), but `CreateCall`
    // hands back a real void-typed `CallInst` and it was returned verbatim, so
    // every consumer's `if (v)` took the has-a-value branch on a value that does
    // not exist. Three consumers reached LLVM with it and killed the compiler
    // outright, with no diagnostic of any kind: `lowerLocalDecl`'s `CreateStore`
    // (`var r := C.Nothing();`) and the assignment statement's `CreateStore`
    // (`r := C.Nothing();`) both trap inside `DataLayout::getABITypeAlign` —
    // SIGILL, because this libLLVM build compiles that `llvm_unreachable` to
    // `ud2` — and `boxValue`'s `getTypeAllocSize` (`var o: Object :=
    // C.Nothing();`) fails the `Ty->isSized()` assertion.
    //
    // That the fix belongs HERE, at the producer, is settled by the CLOSURE
    // form: `var f := C.Nothing; var r := f();` already compiles and runs
    // correctly today, because the closure-call path performs exactly this
    // normalization at its own return (`retTy->isVoidTy() ? nullptr : r`,
    // LlvmBackend_Calls2.cpp). The direct-call form differs from it in nothing
    // the language recognises, so one normalization covering every producer is
    // the rule — not a fourth per-consumer repair. #2915, #3095 and #3105 were
    // each one such repair, filed and fixed separately, for this same root
    // cause; #3419 tracks that duplication shape.
    //
    // THE CALL ITSELF IS UNAFFECTED: `lowerExprInner` has already emitted it, so
    // its side effects stand. Only the (nonexistent) result is dropped.
    if (v && v->getType()->isVoidTy()) return nullptr;
    return v;
}

llvm::Value* Emitter::lowerExprInner(const ast::Expr& e) {
    diverged_ = false;  // CG40: cleared per expression; a `throw` sets it
    // §27.6 (v0.184, #1758): inside a primitive-intrinsic thunk body, a synthetic
    // argument node lowers to the thunk's incoming parameter (see
    // makePrimIntrinsicThunk). Checked only when the override map is non-empty.
    if (!exprValueOverride_.empty()) {
        if (auto it = exprValueOverride_.find(&e); it != exprValueOverride_.end())
            return it->second;
    }
    // CG30: an enum constant (`Color.Green`) is its integer value.
    if (llvm::Value* ev = enumConstant(e)) return ev;
    // §35.3 (#2045): a payload-free choice case access (`Opt.None`)
    // materializes as the choice struct constant.
    if (llvm::Value* cv = choiceCaseConstant(e)) return cv;
    if (llvm::Value* cv2 = constMemberValue(e)) return cv2;
    if (auto* lit = dynamic_cast<const ast::IntLiteralExpr*>(&e)) {
        // CG25: the literal's width comes from its resolved type
        // (e.g. an `L`-suffixed literal is i64).
        llvm::Type* ty = i32();
        if (auto* rt = ctx_.exprType(e))
            if (rt->kind == sema::TypeRef::Kind::Primitive)
                ty = lowerPrimitive(rt->primitive);
        return llvm::ConstantInt::get(
            ty, static_cast<std::uint64_t>(parseIntLiteral(lit->text)),
            /*isSigned=*/true);
    }
    if (auto* boolLit = dynamic_cast<const ast::BoolLiteralExpr*>(&e)) {
        return llvm::ConstantInt::get(i1(), boolLit->value ? 1 : 0);
    }
    // CG32: a char literal is its (i32) code point.
    if (auto* ch = dynamic_cast<const ast::CharLiteralExpr*>(&e)) {
        return llvm::ConstantInt::get(
            i32(), static_cast<std::uint64_t>(parseCharLiteral(ch->text)),
            /*isSigned=*/false);
    }
    // CG24: float/double literal — value parsed from the lexeme, type
    // (f32/f64) from the expr's resolved type (default double).
    // §4.4 fix #521: decimal literal (m/M suffix) → parse at compile time
    // into a { i32 lo32, i32 mid32, i32 hi32, i32 flags } constant aggregate.
    if (auto* fl = dynamic_cast<const ast::FloatLiteralExpr*>(&e)) {
        bool isDecimal = false;
        if (auto* rt = ctx_.exprType(e))
            if (rt->kind == sema::TypeRef::Kind::Primitive &&
                rt->primitive == sema::PrimitiveType::Decimal)
                isDecimal = true;
        if (isDecimal) {
            // Parse the decimal literal at compile time via the runtime's
            // bz_decimal_parse_cstr, which is available as a plain C function.
            // Strip the trailing m/M suffix before passing to the parser.
            std::string digits;
            for (char c : fl->text) {
                if (c == '_' || c == 'm' || c == 'M') continue;
                digits.push_back(c);
            }
            // Call the runtime parse function directly (declared at top of file).
            uint32_t dec[4] = {0, 0, 0, 0};
            bz_decimal_parse_cstr(digits.c_str(), dec, nullptr, nullptr);
            llvm::StructType* dst = decimalStructType();
            return llvm::ConstantStruct::get(dst, {
                llvm::ConstantInt::get(i32(), dec[0]),
                llvm::ConstantInt::get(i32(), dec[1]),
                llvm::ConstantInt::get(i32(), dec[2]),
                llvm::ConstantInt::get(i32(), dec[3])
            });
        }
        llvm::Type* ty = llvm::Type::getDoubleTy(llvm_);
        if (auto* rt = ctx_.exprType(e))
            if (rt->kind == sema::TypeRef::Kind::Primitive)
                ty = lowerPrimitive(rt->primitive);
        std::string digits;
        for (char c : fl->text) {
            if (c == '_' || c == 'f' || c == 'F' || c == 'd' || c == 'D')
                continue;
            digits.push_back(c);
        }
        double v = digits.empty() ? 0.0 : std::strtod(digits.c_str(),
                                                      nullptr);
        return llvm::ConstantFP::get(ty, v);
    }
    // CG27a (§4.6): string and verbatim-string literals → global bz_string.
    if (auto* sl = dynamic_cast<const ast::StringLiteralExpr*>(&e))
        return lowerStringLiteral(sl->text, /*verbatim=*/false);
    if (auto* vs = dynamic_cast<const ast::VerbatimStringExpr*>(&e))
        return lowerStringLiteral(vs->text, /*verbatim=*/true);
    // CG27d (§18.5): interpolated string — concatenate all parts.
    // Literal parts are materialized as string globals. Expression parts
    // call emitPrimitiveToString (for primitives) or string identity (for
    // string-typed exprs); other types call their ToString() intrinsic.
    if (auto* is = dynamic_cast<const ast::InterpolatedStringExpr*>(&e)) {
        // Start with the empty string and concat each part.
        llvm::Value* acc = lowerStringLiteral("\"\"", false);  // ""
        // FINDING [2] / §4.6 / §28.9: each step `acc := emitStringConcat(acc,pv)`
        // mallocs a fresh +1 result and the PREVIOUS accumulator becomes an
        // anonymous, single-owner intermediate with no scope-end release armed
        // for it (the named-local release path only covers LocalDecl nodes), so
        // it would leak — unboundedly in a loop. appendPart releases the prior
        // accumulator AFTER the new one is produced (it has been consumed by the
        // concat — no UAF, no double-free). The very first accumulator is the
        // `""` literal (sentinel rc → release is a no-op). The FINAL accumulator
        // is returned and is never released here (it flows to the target and
        // follows normal placement/escape rules). The part value `pv` is NOT
        // released here: it may be a borrow (a string-typed part is a shared
        // reference to a named local/field per §28.9).
        // #1030 / §4.6 / §28.9: `srcEv` is the lowered part expression (null for a
        // literal part). A non-string part is rendered via emitPrimitiveToString /
        // emitValueToString / ToString, producing a FRESH +1 string distinct from
        // `srcEv`; the concat borrows it (copies the bytes), so it must be released
        // after — else `$"{n}"` (and every non-string interpolation) leaks the
        // rendered intermediate. A STRING-typed part is a BORROW (the renderer
        // returns the lowered value unchanged, `pv == srcEv` — a shared reference to
        // a named local/field, §28.9) and must NOT be released. A literal / "null"
        // sentinel has a sentinel rc, so the release is a harmless no-op. The rule
        // `pv != srcEv` distinguishes the fresh-render case from the borrow case.
        //
        // fix #3352 (§8.6 / §23.4 / §28.3 / §28.9, v0.240): `inlineBoxMint` is
        // captured by the save/clear/restore around `lowerExpr(*part.expr)` in
        // the for-loop below — when set, it is the inline §8.6 box mint the
        // indexer/property inline-body path (`boxForTarget` →
        // `lastBoxMint_`) produced for this part. The `appendPart` lambda
        // reads it (captured by reference) and discharges the box alongside
        // the rendered string — see the lambda's `inlineBoxMint` arm. Declared
        // up here so the lambda can capture by reference and reset to nullptr
        // at the top of each iteration.
        llvm::Value* inlineBoxMint = nullptr;
        auto appendPart = [&](llvm::Value* pv, llvm::Value* srcEv,
                              const ast::Expr* srcExpr) {
            llvm::Value* prev = acc;
            acc = emitStringConcat(acc, pv);
            emitStringRelease(prev);
            // #1482 / §4.6 / §28.9: a STRING-typed hole that is itself a +1
            // PRODUCER (a call returning `string`, `i.ToString()`, a nested
            // `+`/interpolation, `string.Join`, a `() -> string` closure call)
            // is rendered with `pv == srcEv` — the renderer returns the lowered
            // string unchanged (the borrow case at the `pv = ev` fall-through) —
            // yet it still hands us its single +1 reference. The concat above
            // borrows it (copies the bytes), so it must be released or it leaks,
            // unboundedly in a loop. A string-typed BORROW hole (a named
            // local/field per §28.9) is NOT a producer, so isPlusOneStringProducer
            // is false and `pv` survives as the shared reference it is. A
            // freshly-rendered non-string part keeps the original `pv != srcEv`
            // release. (Mirrors the string `+` operand discipline, Exprs.cpp:6055.)
            //
            // fix #2879 (spec §8.5 / §28.3 / §28.9, v0.240): an OBJECT-typed hole
            // that is a +1 string producer METHOD CALL (`$"{m.Fresh()}"` where
            // `Fresh(): Object => "x" + S`) is the string twin of #2865's box
            // residual. `lowerObjectToString` RETAINs the receiver (per #2846),
            // so `pv != srcEv` is true here (the PHI is a new Value*) — the
            // existing `pv != srcEv` arm above discharges the render's +1. The
            // callee's OWN +1 (the fresh string the body minted) is then
            // orphaned and leaks, identical to the Console.WriteLine sibling.
            // `isUserMethodObjectStringReturnCall` recognizes the same method
            // shape `tryLowerConsoleCall` recognizes, so both consumption
            // positions close the leak in lockstep.
            if (pv != srcEv ||
                (srcExpr && isPlusOneStringProducer(srcExpr)))
                emitStringRelease(pv);
            if (srcExpr && isUserMethodObjectStringReturnCall(srcExpr)) {
                emitStringRelease(srcEv);
            }
            // fix #2876 (spec §8.6 / §28.3 / §28.9, v0.278): the BOX twin of
            // #2879. An Object-typed hole that is a +1 BOX producer method call
            // (`$"[{h.M()}]"` where `M(): Object => _n;`) transfers exactly +1
            // from the callee. `lowerObjectToString` does NOT retain the box
            // (only the string arm retains, per #2846); `pv` is the rendered
            // string (PHI result, distinct from `srcEv`) and the existing
            // `pv != srcEv` arm above discharges the render's +1. The callee's
            // OWN +1 (the BOX pointed to by `srcEv`) is orphaned unless we
            // release it here. Without this the box leaks 24 B/call (480 B / 20
            // iterations measured on `f693622`) — the interpolation twin of the
            // Console.WriteLine sibling in LlvmBackend_Calls.cpp. The
            // `isUserMethodBoxReturnCall` predicate is positive ONLY for a call
            // to a method in `boxReturnMethods_`, the SAME pre-pass #2865 and
            // the Console fix above use; the two predicates
            // (`isUserMethodObjectStringReturnCall` and
            // `isUserMethodBoxReturnCall`) classify disjoint sets, so this
            // branch does not double-release on the same hole.
            if (srcExpr && isUserMethodBoxReturnCall(srcExpr)) {
                emitBoxRelease(srcEv);
            }
            // fix #3352 (spec §8.6 / §23.4 / §28.3 / §28.9, v0.240): the inline
            // §8.6 box mint in a §18.5 interpolation hole — the
            // indexer/property inline-body half of the residual. `$"[{e[k]}]"`
            // where `E.property [i: int]: Object => i;` lowers as:
            // lowerExpr(e[k]) → lowerIndexerGet → boxForTarget mints a fresh
            // box AND records it in `lastBoxMint_`; `inlineBoxMint` captures
            // that pointer (set by the save/clear/restore around `lowerExpr`
            // in the loop below); `srcEv` IS the inline mint; the existing
            // `pv != srcEv` arm above discharges the rendered string's +1.
            // `lowerObjectToString` does NOT retain the box (only the string
            // arm retains, per #2846), so the box's +1 is orphaned unless
            // we release it here. Without this the box leaks 24 B/call
            // (480 B / 20 iterations measured on the issue's repro).
            //
            // Gating (parallel to the `isUserMethodBoxReturnCall` arm just
            // above, with a disjoint predicate):
            //   * `inlineBoxMint` — captured by the save/clear/restore
            //     around `lowerExpr(*part.expr)` in the for-loop below;
            //     positive ONLY when `lastBoxMint_` was set DURING THIS
            //     part's lowering;
            //   * `inlineBoxMint == srcEv` — the inline mint IS this part's
            //     lowered expression (the indexer/property inline-body
            //     result, not a converted intermediate);
            //   * `srcEv->getType()->isPointerTy()` — defensive guard, the
            //     inline mint is always a pointer;
            //   * the two predicates — `isUserMethodBoxReturnCall`
            //     (cross-frame mint, callee returns the box) and
            //     `inlineBoxMint` (inline mint at this hole's read site) —
            //     classify disjoint sets: a `CallExpr` is not an
            //     `IndexExpr`, so the `isUserMethodBoxReturnCall(srcExpr)`
            //     arm above is negative exactly when `inlineBoxMint == srcEv`
            //     is positive. The two arms never both fire for the same
            //     hole; the release count is exactly 1 (the box's +1) plus
            //     the rendered string's +1 discharged by `pv != srcEv`.
            if (inlineBoxMint && srcEv &&
                inlineBoxMint == srcEv &&
                srcEv->getType()->isPointerTy()) {
                emitBoxRelease(srcEv);
            }
        };
        for (const auto& part : is->parts) {
            llvm::Value* pv = nullptr;
            llvm::Value* ev = nullptr;  // #1030: lowered part expr (borrow check)
            if (!part.literal.empty()) {
                // Literal part: process escape sequences (e.g. \n → newline,
                // \t → tab) that were stored as raw chars by splitInterpolation.
                // Wrap in quotes so processStringText's strip-quotes logic works.
                // R36/#733: for a VERBATIM interpolated string (`$@"..."`, §18.7)
                // the literal parts must NOT have escapes decoded — the splitter
                // already kept the raw backslash. Thread isVerbatim through AND
                // wrap with the matching `@"` delimiter so processStringCodePoints'
                // verbatim strip (start = verbatim ? 2 : 1) lines up exactly as the
                // plain @"..." path does; otherwise `$@"x\ny"` decoded `\n` → LF.
                std::string wrapped =
                    (is->isVerbatim ? std::string("@\"") : std::string("\"")) +
                    part.literal + "\"";
                std::string content =
                    processStringText(wrapped, is->isVerbatim);
                llvm::Constant* dataArr = llvm::ConstantDataArray::getString(
                    llvm_, content, /*AddNull=*/true);
                unsigned id = stringLiteralIdx_++;
                auto* dataGV = new llvm::GlobalVariable(
                    *module_, dataArr->getType(), true,
                    llvm::GlobalValue::PrivateLinkage, dataArr,
                    ".bz.istr.data." + std::to_string(id));
                dataGV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
                dataGV->setAlignment(llvm::Align(1));
                llvm::Constant* lenC = llvm::ConstantInt::get(
                    i64(), content.size());
                // The literal part is built from a byte string → width 1.
                // #422: constant global literal — SENTINEL rc (never freed). The
                // struct now has 4 fields {data, len, width, rc}; supply all.
                llvm::Constant* widC = llvm::ConstantInt::get(
                    llvm::Type::getInt8Ty(llvm_), 1);
                llvm::Constant* strVal = llvm::ConstantStruct::get(
                    stringType(), {dataGV, lenC, widC, stringRcSentinel()});
                auto* strGV = new llvm::GlobalVariable(
                    *module_, stringType(), true,
                    llvm::GlobalValue::PrivateLinkage, strVal,
                    ".bz.istr." + std::to_string(id));
                pv = strGV;
            } else if (part.expr) {
                // Expression part: lower the expression, then convert to
                // string. If a format spec is present (§18.5), apply it.
                //
                // fix #3352 (§8.6 / §23.4 / §28.3 / §28.9, v0.240): save/
                // clear/restore `lastBoxMint_` around the part lowering so
                // `inlineBoxMint` records ONLY this part's inline §8.6
                // mint (a sibling part's `boxForTarget` cannot leak in,
                // and a previous part's mint cannot leak out). The pattern
                // mirrors the #2952 call-arg `boxForTarget` site
                // (LlvmBackend_Assign.cpp:11296) and the sibling
                // `tryLowerConsoleCall` arm this commit also adds — one
                // discipline, three call sites.
                llvm::Value* savedBoxMint = lastBoxMint_;
                lastBoxMint_ = nullptr;
                // fix #3513 (§18.5 / §28.3, v0.298): if `lowerExpr` itself
                // contains a throwable call and it throws, `emitThrowValue`
                // branches to `unwindBB` via `emitUnwindDisposals()` — but
                // `acc` (the accumulated string built so far) was not registered
                // in any disposal scope and is abandoned. Register it here so
                // the unwind path reclaims it. Save/restore `diverged_` since
                // it is a member that persists across calls; if it was already
                // true before this call (a prior part diverged), we propagate
                // that and skip `appendPart` on the normal path too.
                bool savedDiverged = diverged_;
                disposeStack_.push_back({});
                llvm::AllocaInst* accSlot = entryAllocaNullPtr();
                builder_.CreateStore(acc, accSlot);
                Disposal d{accSlot, nullptr};
                d.unwindOnly = true;
                d.stringRelease = true;
                disposeStack_.back().push_back(d);
                ev = lowerExpr(*part.expr);
                inlineBoxMint = lastBoxMint_;
                lastBoxMint_ = savedBoxMint;
                diverged_ = savedDiverged;
                if (diverged_) {
                    // Exception was thrown: `emitUnwindDisposals()` already
                    // fired at the throw site and disposed `accSlot`.
                    disposeStack_.pop_back();
                    return llvm::PoisonValue::get(ptrTy());
                }
                // Normal path: null the slot so the scope's string disposal is
                // a no-op (the prior `acc` was concat'd into the new string and
                // will be released by `appendPart` below), then pop the scope.
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), accSlot);
                disposeStack_.pop_back();
                // fix #3442 (§4.7 / §8.5 / §18.5, v0.283): a UNIT-typed hole is
                // the one expression kind that legitimately lowers to NO value
                // — `()` is LLVM `void`, so `lowerExpr` returns nullptr for it
                // by construction (#3421 normalizes the producer). The #1386
                // arm below exists for expression kinds codegen does not handle
                // and cannot tell the two apart, so `$"[{()}]"` and
                // `$"[{C.Nothing()}]"` were HARD COMPILE ERRORS on programs §4.7
                // makes legal (measured: `internal: lowerExpr returned null for
                // interpolated-string expression part — #1386`, exit 1). Give
                // the hole the inert placeholder byte the unit LOCAL slot
                // already stores (`unitPlaceholderTy`, the ONE definition of
                // that substitution) so nothing downstream sees a null; the
                // §8.5 arm further down answers `"()"` without reading it, and
                // the receiver/producer expression has already been lowered
                // above for its effects (§22.5's "observable … and is
                // normative"). `appendPart`'s `srcEv` bookkeeping is unaffected:
                // a non-pointer constant is neither a +1 string producer nor a
                // box mint, so every one of its arms declines exactly as it did
                // when this hole did not compile at all.
                if (!ev) {
                    const sema::TypeRef* uTy = ctx_.exprType(*part.expr);
                    if (uTy && uTy->kind == sema::TypeRef::Kind::Unit &&
                        !uTy->nullable)
                        ev = llvm::ConstantInt::get(unitPlaceholderTy(), 0);
                }
                if (!ev) {
                    // #1386: ICE — lowerExpr returned null for a non-null
                    // interpolated-string expression part; the expression kind
                    // is not yet handled by codegen.
                    ctx_.diagnostics().report(
                        Severity::Error, part.expr->span.begin,
                        "internal: lowerExpr returned null for "
                        "interpolated-string expression part — #1386");
                    continue;
                }
                // §18.5: format spec → translate to snprintf format.
                if (!part.formatSpec.empty()) {
                    auto* frt = ctx_.exprType(*part.expr);
                    // §18.5 / §7: a nullable VALUE part WITH a format spec —
                    // branch on hasValue (absent → "null"; present → apply the
                    // spec to the UNWRAPPED payload). emitWithFormatSpec expects
                    // the bare primitive, not the {i8, T} tagged struct: feeding
                    // it the struct sign-extended a struct (`{i8,i32}` → i64) or
                    // dropped the spec (#849).
                    if (frt && isNullableValueTy(frt) &&
                        ev->getType()->isStructTy()) {
                        llvm::Value* hasVal = nullablePrimHasValue(ev);
                        llvm::Value* primVal = nullablePrimValue(ev);
                        auto* valBB = llvm::BasicBlock::Create(
                            llvm_, "fs.val", currentFn_);
                        auto* nullBB = llvm::BasicBlock::Create(
                            llvm_, "fs.null", currentFn_);
                        auto* endBB = llvm::BasicBlock::Create(
                            llvm_, "fs.end", currentFn_);
                        builder_.CreateCondBr(hasVal, valBB, nullBB);
                        builder_.SetInsertPoint(valBB);
                        sema::TypeRef innerTy = *frt;
                        innerTy.nullable = false;
                        llvm::Value* vStr = emitWithFormatSpec(
                            primVal, part.formatSpec, &innerTy);
                        if (!vStr) vStr = emitValueToString(primVal, &innerTy);
                        auto* valEnd = builder_.GetInsertBlock();
                        builder_.CreateBr(endBB);
                        builder_.SetInsertPoint(nullBB);
                        // §18.5 (v0.184, #1691): the rendered "null" text
                        // participates in alignment exactly as any other
                        // rendered text — it was previously emitted bare,
                        // dropping the width/alignment for a null hole
                        // (`$"[{x,8}]"` printed "[null]" instead of
                        // "[    null]"). The FORMAT portion (if any) is still
                        // never dispatched on null — emitAlignOnly applies
                        // only the `,WIDTH` prefix.
                        llvm::Value* nStr = emitAlignOnly(
                            lowerStringLiteral("\"null\"", false),
                            part.formatSpec);
                        auto* nullEnd = builder_.GetInsertBlock();
                        builder_.CreateBr(endBB);
                        builder_.SetInsertPoint(endBB);
                        auto* phi = builder_.CreatePHI(ptrTy(), 2);
                        phi->addIncoming(vStr, valEnd);
                        phi->addIncoming(nStr, nullEnd);
                        appendPart(phi, ev, part.expr.get());
                        continue;
                    }
                    // §18.5 / §7 (v0.184, #1691): a nullable REFERENCE part
                    // (`string?`, a nullable class/interface) WITH a format
                    // spec — same null-participates-in-alignment rule as the
                    // nullable-value case above, plus a correctness fix: the
                    // fallthrough below fed `ev` (possibly a real null
                    // pointer) straight into emitWithFormatSpec, which
                    // dereferences it unconditionally in the alignment path
                    // (`,WIDTH` on `string?`) — a null value there crashed
                    // (SIGSEGV) instead of rendering "[    null]".
                    if (frt && isNullableRefTy(frt) &&
                        ev->getType()->isPointerTy()) {
                        llvm::Value* isNull = builder_.CreateICmpEQ(
                            ev, llvm::ConstantPointerNull::get(ptrTy()),
                            "fs.rnull");
                        auto* liveBB = llvm::BasicBlock::Create(
                            llvm_, "fs.rlive", currentFn_);
                        auto* nullBB2 = llvm::BasicBlock::Create(
                            llvm_, "fs.rnullstr", currentFn_);
                        auto* endBB2 = llvm::BasicBlock::Create(
                            llvm_, "fs.rend", currentFn_);
                        builder_.CreateCondBr(isNull, nullBB2, liveBB);
                        builder_.SetInsertPoint(liveBB);
                        sema::TypeRef liveTy = *frt;
                        liveTy.nullable = false;
                        llvm::Value* liveStr =
                            emitWithFormatSpec(ev, part.formatSpec, &liveTy);
                        if (!liveStr) liveStr = emitValueToString(ev, &liveTy);
                        auto* liveEnd = builder_.GetInsertBlock();
                        builder_.CreateBr(endBB2);
                        builder_.SetInsertPoint(nullBB2);
                        llvm::Value* nStr2 = emitAlignOnly(
                            lowerStringLiteral("\"null\"", false),
                            part.formatSpec);
                        auto* nullEnd2 = builder_.GetInsertBlock();
                        builder_.CreateBr(endBB2);
                        builder_.SetInsertPoint(endBB2);
                        auto* phi2 = builder_.CreatePHI(ptrTy(), 2);
                        phi2->addIncoming(liveStr, liveEnd);
                        phi2->addIncoming(nStr2, nullEnd2);
                        appendPart(phi2, ev, part.expr.get());
                        continue;
                    }
                    pv = emitWithFormatSpec(ev, part.formatSpec, frt);
                    if (pv) {
                        appendPart(pv, ev, part.expr.get());
                        continue;
                    }
                    // emitWithFormatSpec returned null: fall through.
                }
                auto* rt = ctx_.exprType(*part.expr);
                // CG-nullable: for nullable value types (primitives, and a
                // nullable enum — issue #364), extract the value. If null,
                // format as "null"; otherwise format the inner value (an enum's
                // live value is its type name per §8.5, via emitValueToString
                // with the non-nullable inner type).
                if (rt && isNullableValueTy(rt) && ev->getType()->isStructTy()) {
                    llvm::Value* hasVal = nullablePrimHasValue(ev);
                    llvm::Value* primVal = nullablePrimValue(ev);
                    auto* valBB = llvm::BasicBlock::Create(llvm_, "np.val", currentFn_);
                    auto* nullBB = llvm::BasicBlock::Create(llvm_, "np.null", currentFn_);
                    auto* endBB = llvm::BasicBlock::Create(llvm_, "np.end", currentFn_);
                    builder_.CreateCondBr(hasVal, valBB, nullBB);
                    builder_.SetInsertPoint(valBB);
                    sema::TypeRef innerTy = *rt;
                    innerTy.nullable = false;
                    llvm::Value* vStr = emitValueToString(primVal, &innerTy);
                    auto* valEnd = builder_.GetInsertBlock();
                    builder_.CreateBr(endBB);
                    builder_.SetInsertPoint(nullBB);
                    llvm::Value* nStr = lowerStringLiteral("\"null\"", false);
                    auto* nullEnd = builder_.GetInsertBlock();
                    builder_.CreateBr(endBB);
                    builder_.SetInsertPoint(endBB);
                    auto* phi = builder_.CreatePHI(ptrTy(), 2);
                    phi->addIncoming(vStr, valEnd);
                    phi->addIncoming(nStr, nullEnd);
                    pv = phi;
                } else if (rt && isNullableRefTy(rt) &&
                           ev->getType()->isPointerTy()) {
                    // R35: a nullable REFERENCE part (`string?`, `Object?`,
                    // nullable class/interface) is a plain ptr with a real null.
                    // Interpolating null must render the literal "null", not
                    // dereference it (the `string?` path fell into the Primitive
                    // branch below and emitPrimitiveToString read a null string's
                    // header → SIGSEGV; a `class?` reached the ToString path and
                    // called a method on null). Mirror emitValueToString's
                    // null-ref handling: null → "null"; non-null → format by the
                    // non-nullable rule.
                    llvm::Value* isNull = builder_.CreateICmpEQ(
                        ev, llvm::ConstantPointerNull::get(ptrTy()), "ip.null");
                    auto* liveBB =
                        llvm::BasicBlock::Create(llvm_, "ip.live", currentFn_);
                    auto* nullBB =
                        llvm::BasicBlock::Create(llvm_, "ip.nullstr", currentFn_);
                    auto* endBB =
                        llvm::BasicBlock::Create(llvm_, "ip.end", currentFn_);
                    builder_.CreateCondBr(isNull, nullBB, liveBB);
                    builder_.SetInsertPoint(liveBB);
                    sema::TypeRef liveTy = *rt;
                    liveTy.nullable = false;
                    // fix #2968: the LIVE arm of a nullable OPAQUE slot
                    // (`Object?` holding a vtable-less built-in) reaches
                    // `emitValueToString` → `lowerObjectToString` and
                    // double-frees exactly as the non-nullable spelling did.
                    // Narrow it the same way; the surrounding runtime null
                    // check still renders "null" on the null arm, and the
                    // narrowing only fires when the slot's initializer names a
                    // vtable-less built-in class (so it cannot be null here
                    // unless the local was reassigned, which declines).
                    llvm::Value* liveStr =
                        tryOpaqueSlotBuiltinRender(ev, &liveTy, part.expr.get());
                    if (!liveStr) liveStr = emitValueToString(ev, &liveTy);
                    auto* liveEnd = builder_.GetInsertBlock();
                    builder_.CreateBr(endBB);
                    builder_.SetInsertPoint(nullBB);
                    llvm::Value* nullStr = lowerStringLiteral("\"null\"", false);
                    auto* nullEnd = builder_.GetInsertBlock();
                    builder_.CreateBr(endBB);
                    builder_.SetInsertPoint(endBB);
                    auto* phi = builder_.CreatePHI(ptrTy(), 2);
                    phi->addIncoming(liveStr, liveEnd);
                    phi->addIncoming(nullStr, nullEnd);
                    pv = phi;
                } else if (rt && rt->kind == sema::TypeRef::Kind::Primitive) {
                    // fix #2968 (spec §8.1 / §8.5 / §18.5 / §28.9, v0.240):
                    // `Object` is a PRIMITIVE kind here (sema maps the
                    // universal root to `PrimitiveType::Object`), so an
                    // `Object`-typed hole falls straight into
                    // `emitPrimitiveToString`, whose `P::Object` arm calls
                    // `lowerObjectToString` — and `bz_obj_is_string`'s
                    // NEGATIVE test ("word 0 is not an emitted vtable ⇒ a
                    // string") mis-answers "string" for a vtable-less built-in
                    // sitting in that slot per §8.1. The receiver is then
                    // retained as a string and released by the consumer: the
                    // measured double free. Ask FIRST whether the AST source
                    // statically narrows the slot to a vtable-less built-in
                    // class; if so render via the §2923 helper. If not, fall
                    // through unchanged — correct for a genuine string, for a
                    // user class, and for a box.
                    if (llvm::Value* r = tryOpaqueSlotBuiltinRender(
                            ev, rt, part.expr.get())) {
                        pv = r;
                    } else {
                        pv = emitPrimitiveToString(ev, rt->primitive);
                    }
                } else if (rt && rt->kind == sema::TypeRef::Kind::Named &&
                           rt->declaration &&
                           rt->declaration->kind == ast::TypeDeclKind::Enum) {
                    // per §10.5 (v0.183): an enum interpolates via its user
                    // `ToString(): string` if declared, else the §8.5 default
                    // (the type's qualified name) — not its underlying integer.
                    // `ev` is the enum's underlying integer.
                    pv = emitEnumToString(ev, rt->declaration);
                } else if (!rt || rt->kind != sema::TypeRef::Kind::Primitive) {
                    // Type not set or not primitive — infer from LLVM type.
                    using P = sema::PrimitiveType;
                    auto* llvmTy = ev->getType();
                    // §2.18 / §18.5 (#1411): named builtin value types with
                    // their own no-format renderers (TimeSpan → "c", DateOnly/
                    // TimeOnly → ISO 8601, Guid → "D"). Route through
                    // emitValueToString before the LLVM-type integer fallback,
                    // which would render raw tick/day counts (§2.5/§2.18).
                    if (rt && rt->kind == sema::TypeRef::Kind::Named) {
                        std::string_view bn = builtinTypeName(rt);
                        if (bn == "TimeSpan" || bn == "DateOnly" ||
                            bn == "TimeOnly" || bn == "Guid")
                            pv = emitValueToString(ev, rt);
                    }
                    // fix #2913 (spec §8.5 inline-array row / §18.5, v0.240):
                    // an INLINE ARRAY interpolates as its type's NAME. Same
                    // reason as the builtin value types just above — without
                    // this it reaches the LLVM-type fallback below, matches no
                    // integer/float width, and leaves `pv` null, which renders
                    // as the EMPTY STRING with no diagnostic.
                    //
                    // MEASURED before: `$"[{a}]"` on `var a: [3]int` printed
                    // `[]`, while `o.ToString()` on the SAME value boxed into
                    // `Object` printed `[[3]int]` — two spellings of one §8.5
                    // rule disagreeing inside one program. #2804 fixed the boxed
                    // half; this is the unboxed half.
                    //
                    // Delegates to `emitValueToString` rather than rendering
                    // here, so this path and the boxed one cannot drift: both
                    // now bottom out in the single `inlineArrayTypeSpelling`.
                    // That is deliberate — the interpolation chain is already a
                    // second dispatch mirroring emitValueToString's, and adding
                    // a rendering rather than a delegation would make it a third
                    // place the §8.5 answer lives.
                    if (!pv && rt &&
                        rt->kind == sema::TypeRef::Kind::InlineArray)
                        pv = emitValueToString(ev, rt);
                    // fix #3442 (§8.5 unit row / §4.7 / §18.5, v0.283): the
                    // UNIT hole, exactly the #2913 shape one type over. §8.5
                    // puts unit in the SAME sentence as the inline arrays
                    // directly above — "and for the unit type: the type's name
                    // … `"()"` for unit" — and its first sentence puts
                    // interpolation squarely in scope ("`ToString` is invoked
                    // implicitly inside string interpolation ([§18.5]) via
                    // virtual dispatch"). Without an arm the hole reached the
                    // LLVM-shape fallback below, where the placeholder byte
                    // matched `isIntegerTy(8)` and rendered `"0"`. Delegated to
                    // `emitValueToString` rather than rendered here, for
                    // #2913's recorded reason: this chain is already a second
                    // dispatch mirroring that function's, and a rendering here
                    // would make it a third place the §8.5 answer lives.
                    if (!pv && rt && rt->kind == sema::TypeRef::Kind::Unit &&
                        !rt->nullable)
                        pv = emitValueToString(ev, rt);
                    // fix #2661 (spec §8.5 / §18.5, v0.252): a SPAN or a
                    // `Range<T>` hole. Exactly the #2913 shape one type over:
                    // both lower to a STRUCT whose TypeRef carries no user
                    // `TypeSymbol`, so they matched none of the integer/float/
                    // pointer tests below and none of the struct-typed record
                    // arm further down (which is gated on
                    // `structRt->declaration`); `pv` stayed null and the hole
                    // rendered as the EMPTY STRING with no diagnostic —
                    // MEASURED in #2661 section A: `Console.WriteLine($"{sp}")`
                    // printed an empty line, exit 0. Delegated to
                    // `emitValueToString` rather than rendered here, for #2913's
                    // recorded reason: the interpolation chain is already a
                    // second dispatch mirroring `emitValueToString`'s, and a
                    // rendering here would make it a third place the §8.5 answer
                    // lives.
                    if (!pv && rt && ev->getType()->isStructTy() &&
                        (isSpanLikeTypeName(rt) || isRangeType(rt) ||
                         (rt->kind == sema::TypeRef::Kind::Named &&
                          !rt->declaration && rt->astNode && rt->astNode->name &&
                          !rt->astNode->name->parts.empty() &&
                          rt->astNode->name->parts[0] == "Range")))
                        pv = emitValueToString(ev, rt);
                    if (!pv) {
                    if (llvmTy->isIntegerTy(1))
                        pv = emitPrimitiveToString(ev, P::Bool);
                    else if (llvmTy->isIntegerTy(8) || llvmTy->isIntegerTy(16) ||
                             llvmTy->isIntegerTy(32))
                        pv = emitPrimitiveToString(ev, P::Int);
                    else if (llvmTy->isIntegerTy(64))
                        pv = emitPrimitiveToString(ev, P::Long);
                    else if (llvmTy->isDoubleTy())
                        pv = emitPrimitiveToString(ev, P::Double);
                    else if (llvmTy->isFloatTy())
                        pv = emitPrimitiveToString(ev, P::Float);
                    else if (llvmTy->isPointerTy()) {
                        // Could be string (use as-is) or class (call ToString).
                        if (rt && rt->kind == sema::TypeRef::Kind::Named &&
                            rt->declaration) {
                            // Class/interface: call ToString() if available.
                            auto* ts = rt->declaration;
                            sema::MethodSymbol* toStr = nullptr;
                            for (auto* cur = ts; cur && !toStr;) {
                                for (auto* sym : cur->members) {
                                    auto* ms = dynamic_cast<sema::MethodSymbol*>(sym);
                                    if (ms && ms->name == "ToString" && ms->params.empty()) {
                                        toStr = ms; break;
                                    }
                                }
                                sema::TypeSymbol* nxt = nullptr;
                                for (auto* b : cur->baseTypes)
                                    if (b && b->kind == ast::TypeDeclKind::Class) { nxt = b; break; }
                                cur = nxt;
                            }
                            if (toStr) {
                                std::vector<llvm::Value*> args{ev};
                                pv = emitMethodCall(toStr, args, false);
                            } else {
                                // §8.5 (#1077): a class/interface with NO explicit
                                // ToString override interpolates as its type name
                                // (the §8.5 default), NOT the raw pointer.
                                // emitValueToString handles the Named class/iface
                                // case — the same path an Object-typed interpolation
                                // already uses. Previously this treated the class
                                // pointer as a bz_string*, so `$"{obj}"` produced an
                                // empty / garbage string.
                                pv = emitValueToString(ev, rt);
                            }
                        } else if (rt && rt->kind == sema::TypeRef::Kind::Named &&
                                   !rt->declaration) {
                            // §8.5 (#1118): a generic type-parameter operand
                            // (`x: T`) is a declaration-less Named TypeRef whose
                            // runtime value is a heap object. Route through
                            // emitValueToString, which resolves T via the generic
                            // substitution (→ the concrete type's ToString rule)
                            // and otherwise dispatches the dynamic ToString.
                            // Previously this fell to `pv = ev`, reading the object
                            // as a bz_string → "".
                            //
                            // fix #2969 (spec §8.5 / §8.1 / §18.5 / §28.9,
                            // v0.240): a declaration-less Named type whose head
                            // is a built-in INTERFACE head
                            // (`IList<T>`, `IEnumerable<T>`, `IDictionary<K,V>`,
                            // …) is the OTHER declaration-less-Named inhabitant
                            // besides the type-parameter one, and it may also be
                            // a vtable-less built-in class at runtime
                            // (`var e: IList<int> := new Array<int>(2);`).
                            // §2923 deliberately declined to handle it because
                            // the interface slot may also hold a USER class
                            // declaring the interface; that arm is correct for
                            // the user class, broken for the built-in collection.
                            // Ask first whether the AST source statically
                            // narrows the runtime value to a vtable-less built-in
                            // class — if yes, render via the §2923 helper and
                            // skip the broken `emitValueToString` call; if no,
                            // fall through (correct for user class; deferral per
                            // #2969 for the unanalysed built-in case).
                            //
                            // fix #2968: `Object` (§8.1's universal root) is
                            // the other opaque slot that reaches here, and it
                            // fails the same way — the helper now gates on it
                            // too, so `var o: Object := mx; $"{o}"` renders
                            // via the §2923 helper instead of double-freeing
                            // `mx` through `lowerObjectToString`.
                            if (llvm::Value* r = tryOpaqueSlotBuiltinRender(
                                    ev, rt, part.expr.get())) {
                                pv = r;
                            } else {
                                pv = emitValueToString(ev, rt);
                            }
                        } else {
                            pv = ev;  // string or unknown ptr — use as-is
                        }
                    }
                    }  // end if (!pv) — LLVM-type integer/pointer fallback
                    // For struct (record value): spill to alloca, call ToString.
                    // #2503 (§35.7/§9.1/§9.2): `rt->declaration` is null for a
                    // bare, unresolved generic type-parameter operand
                    // (`val: T`) — resolve it via the active generic
                    // substitution first, the SAME resolution
                    // `emitValueToString`'s bare-T redirect uses
                    // (LlvmBackend_String.cpp, the `genericSubstSemaTy_` block
                    // just above its final fallback) and the bare-T redirect
                    // `lowerCall` uses for a direct `.ToString()` call
                    // (LlvmBackend_Calls2.cpp, the `isToString &&
                    // recvTy->kind == Named && !recvTy->declaration` block) —
                    // so a struct-typed T instantiated to a record/choice
                    // dispatches by ITS rule here too, instead of being
                    // silently DROPPED from the interpolated string. Before
                    // this fix neither this block (gated on `rt->declaration`)
                    // nor the LLVM-type-inference fallback just above (gated
                    // on isPointerTy/isIntegerTy/isDoubleTy/isFloatTy — none
                    // of which match a struct) matched a struct-typed bare-T
                    // value, so `pv` stayed null and the `if (pv)
                    // appendPart(...)` below never fired: the hole vanished
                    // from the interpolated string entirely (not merely
                    // rendered as "" — confirmed by tracing `$"{val}"` for
                    // `val: T` bound to a Choice, LlvmBackend_Exprs.cpp, #2503).
                    const sema::TypeRef* structRt = rt;
                    if (structRt && structRt->kind == sema::TypeRef::Kind::Named &&
                        !structRt->declaration && structRt->astNode &&
                        structRt->astNode->name &&
                        structRt->astNode->name->parts.size() == 1 &&
                        !genericSubstSemaTy_.empty()) {
                        auto sit = genericSubstSemaTy_.find(structRt->headName());
                        if (sit != genericSubstSemaTy_.end() && sit->second &&
                            sit->second->declaration)
                            structRt = sit->second;
                    }
                    if (!pv && ev->getType()->isStructTy() && structRt &&
                        structRt->kind == sema::TypeRef::Kind::Named &&
                        structRt->declaration) {
                        auto* ts = structRt->declaration;
                        sema::MethodSymbol* toStr = nullptr;
                        for (auto* cur = ts; cur && !toStr;) {
                            for (auto* sym : cur->members) {
                                auto* ms = dynamic_cast<sema::MethodSymbol*>(sym);
                                if (ms && ms->name == "ToString" && ms->params.empty()) {
                                    toStr = ms; break;
                                }
                            }
                            sema::TypeSymbol* nxt = nullptr;
                            for (auto* b : cur->baseTypes)
                                if (b && b->kind == ast::TypeDeclKind::Class) { nxt = b; break; }
                            cur = nxt;
                        }
                        // §8.5 / §9.2: a positional / combined record, or an
                        // eligible body record, gets the synthesized
                        // property-listing ToString; an ineligible body record
                        // gets the type-name default.
                        // fix #2934 follow-up (§8.5 / §9.1, v0.252): the same
                        // AST-decl-only read as `lowerValueSemMethodOn`'s, with
                        // the same consequence one step further on — for an
                        // IMPORTED positional record this arm answered false,
                        // so `$"{p}"` rendered the bare head "Pt" instead of
                        // §9.1's "Pt(X: 1, Y: 2)". Asked through the shared
                        // predicate, which reads BOTH sources.
                        bool autoBearing = recordIsAutoBearing(ts);
                        if (toStr) {
                            // Spill the record value to an alloca so we have a ptr.
                            llvm::AllocaInst* spill = entryAlloca(ev->getType());
                            builder_.CreateStore(ev, spill);
                            std::vector<llvm::Value*> args{spill};
                            pv = emitMethodCall(toStr, args, false);
                        } else if (ts->kind == ast::TypeDeclKind::Record &&
                                   autoBearing) {
                            // §8.5 / §9.1 / §9.2: a record with no user ToString
                            // gets the synthesized `TypeName(Prop: value, ...)`
                            // form over its storage members.
                            pv = emitRecordToString(ev, ts);
                        } else if (ts->kind == ast::TypeDeclKind::Choice &&
                                   sema::isAutoEqualityBearingChoice(
                                       ts, ctx_, structRt)) {
                            // #2479 / §35.7: a choice-typed interpolation hole
                            // with no user ToString override, auto-equality-
                            // bearing, gets the synthesized `TypeName.CaseName
                            // (field: value, ...)` form — the same
                            // emitChoiceToString the direct `.ToString()` call
                            // (lowerValueSemMethodOn) and emitValueToString
                            // (nested-field formatting) now use. Before this
                            // fix, interpolation had its OWN hand-rolled
                            // dispatch here with no Choice case at all, so a
                            // choice-typed hole silently fell to the generic
                            // "no ToString → type name" `else` branch below —
                            // `$"{c}"` rendered just `"Color"`, dropping the
                            // active case entirely (a distinct gap from, and
                            // found while regression-testing, #2479's own
                            // bare-T-receiver scope).
                            pv = emitChoiceToString(ev, ts);
                        } else if (ts->kind == ast::TypeDeclKind::Choice) {
                            // §35.7's open question (see lowerValueSemMethodOn's
                            // Choice/ToString branch for the full citation):
                            // no compiler-defined default exists for a choice
                            // that is not auto-hashable. Diagnose
                            // rather than silently rendering the misleading
                            // bare type name (indistinguishable from a real
                            // implemented default) or crashing.
                            ctx_.diagnostics().report(
                                Severity::Error, part.expr->span.begin,
                                "member `ToString` on a choice that is not "
                                "auto-hashable (borrow-bearing, "
                                "owning, or a value-like choice with an "
                                "inadmissible payload, language spec "
                                "\xC2\xA7""35.7) has no compiler-defined "
                                "default; declare a plain `ToString(): "
                                "string` member to replace it (\xC2\xA7"
                                "10.5's choice model) (tracked #2479)");
                            pv = llvm::PoisonValue::get(ptrTy());
                        } else {
                            // No ToString — use type name (§8.5).
                            // fix #2934 (§8.5, v0.252): the arms above have
                            // already taken the positional / auto-equality
                            // record and the auto-hashable choice, so what
                            // reaches here is §8.5's name-only default — for a
                            // body record that does not satisfy auto-equality,
                            // which §8.5 renders QUALIFIED (stdlib §2.13
                            // `FullName`). Asked through the shared predicate
                            // rather than re-derived, so this hole and the
                            // direct `.ToString()` call cannot disagree.
                            pv = lowerStringLiteral(
                                std::string("\"") +
                                    spec85NameOnlyDefaultSpelling(ts) + "\"",
                                false);
                        }
                    }
                }
            }
            if (pv) appendPart(pv, ev, part.expr.get());
        }
        return acc;
    }
    // CG23: the `null` literal. For nullable value types (int?, …, and a
    // nullable enum — issue #364) produce the tagged-struct null; for reference
    // types produce nullptr.
    if (dynamic_cast<const ast::NullLiteralExpr*>(&e)) {
        if (auto* rt = ctx_.exprType(e))
            if (isNullableValueTy(rt))
                return nullableValueNull(rt);
        return llvm::ConstantPointerNull::get(ptrTy());
    }
    // CG37: `default(T)` is the zero value of T (0 / false / 0.0 / null).
    if (auto* de = dynamic_cast<const ast::DefaultExpr*>(&e)) {
        return llvm::Constant::getNullValue(lowerType(de->type.get()));
    }
    // fix #499 / §9.8: RangeExpr in value position (e.g. `var r := a..b`).
    // Builds a Range struct { i64 start, i64 end, i64 step, i8 flags } so the
    // range can be stored in a local variable and used via member access / `in`.
    // The inline-literal fast paths (`x in lo..hi`, `for var i in lo..hi`) are
    // handled elsewhere and bypass this; this path only fires when the RangeExpr
    // is lowered as a general expression (initializer of a local, etc.).
    if (auto* re = dynamic_cast<const ast::RangeExpr*>(&e)) {
        auto* sty = rangeStructType();
        // start: lo value widened to i64; 0 if lo is absent (open-ended low).
        llvm::Value* startV = builder_.getInt64(0);
        if (re->lo) {
            llvm::Value* lo = lowerExpr(*re->lo);
            if (!lo) return nullptr;
            startV = lo->getType() == i64()
                         ? lo
                         : builder_.CreateSExt(lo, i64(), "range.start");
        }
        // end: hi value widened to i64; 0 if hi is absent (open-ended high).
        llvm::Value* endV = builder_.getInt64(0);
        if (re->hi) {
            llvm::Value* hi = lowerExpr(*re->hi);
            if (!hi) return nullptr;
            endV = hi->getType() == i64()
                       ? hi
                       : builder_.CreateSExt(hi, i64(), "range.end");
        }
        // step: explicit step if present, else 1 (§16.4 default).
        llvm::Value* stepV = builder_.getInt64(1);
        if (re->step) {
            llvm::Value* s = lowerExpr(*re->step);
            if (!s) return nullptr;
            stepV = s->getType() == i64()
                        ? s
                        : builder_.CreateSExt(s, i64(), "range.step");
            // §16.4 (v0.242, #3052), verbatim: "A step of zero is rejected
            // **at construction**: … evaluating a range expression whose
            // computed step is zero raises `OverflowException` at the
            // expression." This is THE construction site for a `Range<T>`
            // value, and §16.4 continues: "Range expressions are the sole
            // producer of `Range<T>` values … so no `Range<T>` value with a
            // zero step can exist." The guard therefore replaces — not
            // supplements — the pre-v0.242 `GetEnumerator`-time
            // `ArgumentException`, which this revision deletes: the fault is
            // strictly earlier and strictly more programs fault (a
            // constructed-but-never-iterated zero-step range now faults).
            //
            // A CONSTANT zero step never reaches here (sema rejects it at
            // compile time), so the guard's compare folds away for every
            // constant step and costs one predicted branch otherwise.
            emitGuardThrow(builder_.CreateICmpEQ(stepV, builder_.getInt64(0),
                                                 "range.step0"),
                           "OverflowException");
        }
        // flags: bit 0 = IsEndInclusive; bit 1 = startOpen; bit 2 = endOpen.
        uint8_t flags = 0;
        if (re->inclusive) flags |= 1u;
        if (!re->lo)       flags |= 2u;
        if (!re->hi)       flags |= 4u;
        llvm::Value* flagsV =
            llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), flags);
        // Assemble the struct via insertvalue.
        llvm::Value* sv = llvm::UndefValue::get(sty);
        sv = builder_.CreateInsertValue(sv, startV, {0u}, "range.0");
        sv = builder_.CreateInsertValue(sv, endV,   {1u}, "range.1");
        sv = builder_.CreateInsertValue(sv, stepV,  {2u}, "range.2");
        sv = builder_.CreateInsertValue(sv, flagsV, {3u}, "range.3");
        return sv;
    }
    // §17.10 (#487): typeof(T) → Type.  The `Type` runtime class (stdlib §2.13)
    // provides type identity and lattice metadata.  Its runtime representation
    // (a global pointer to a type-descriptor object) is not yet emitted by the
    // compiler backend (deferred alongside the broader System.Reflection surface,
    // stdlib §11).  Until that milestone, typeof(T) in a codegen-bound context
    // emits a clean compile-time error rather than reaching LLVM with an untyped
    // expression and producing silent garbage or a crash (#487).
    if (auto* to = dynamic_cast<const ast::TypeOfExpr*>(&e)) {
        (void)to;
        ctx_.diagnostics().report(
            Severity::Error, e.span.begin,
            "`typeof` is not yet implemented in codegen: the runtime `Type` "
            "class (stdlib \xC2\xA7""2.13) requires a type-descriptor table that is "
            "not yet emitted (\xC2\xA7""17.10, #487). This expression will be "
            "supported in a future milestone.");
        return llvm::PoisonValue::get(ptrTy());
    }
    // §17.9: nameof(QualifiedName) → compile-time string constant equal to
    // the rightmost identifier of the operand (the only segment for a simple
    // name, the last segment for a dotted name such as BankAccount.Deposit →
    // "Deposit"). The QualifiedName is stored verbatim in the AST; no runtime
    // resolution is needed — we wrap the lexeme in quotes and emit it as a
    // string literal global via lowerStringLiteral.
    if (auto* no = dynamic_cast<const ast::NameOfExpr*>(&e)) {
        std::string_view ident =
            (no->name && !no->name->parts.empty())
                ? no->name->parts.back()
                : std::string_view{};
        // Synthesise a quoted string that processStringText will accept.
        std::string quoted = "\"";
        quoted += ident;
        quoted += "\"";
        return lowerStringLiteral(quoted, /*verbatim=*/false);
    }
    // CG23: postfix `!` (null-forgiving) — raise NullReferenceException
    // if the operand is null, otherwise yield it (§17.12).
    if (auto* bang = dynamic_cast<const ast::PostfixBangExpr*>(&e)) {
        llvm::Value* v = bang->operand ? lowerExpr(*bang->operand)
                                       : nullptr;
        if (!v) return nullptr;
        // CG-nullable: nullable value type (int?, bool?, …, and a nullable
        // enum — issue #364) — check hasValue field and extract the value.
        if (bang->operand) {
            if (auto* rt = ctx_.exprType(*bang->operand)) {
                if (isNullableValueTy(rt)) {
                    llvm::Value* hasVal = nullablePrimHasValue(v);
                    emitGuardThrow(builder_.CreateNot(hasVal),
                                   "NullReferenceException");
                    return nullablePrimValue(v);
                }
                // issue #604 / §14.1 / §22.4: a NULLABLE BARE TYPE-PARAMETER
                // (`TFirst?` where TFirst is a class generic param) lowers to
                // the {i8, T} nullable struct in a monomorphized body, but
                // isNullableValueTy returns false for it (the TypeRef has
                // kind=Named, nullable=true, no declaration — it looks like an
                // unresolved type, not a concrete value type). Without this
                // guard the `!` operator falls through and returns the entire
                // {i8, T} struct instead of extracting the value field, so a
                // `GetFirst(): TFirst` body `return _first!` emits a
                // `ret { i8, i32 } %2` against an `i32` function → IR-verify
                // failure. Fix: when the operand TypeRef is a nullable bare
                // type-param, resolve it through genericSubstSemaTy_ to its
                // concrete sema type (nullableTypeParamConcrete does this),
                // and if that concrete nullable IS a nullable value type,
                // treat it the same as the direct isNullableValueTy case.
                if (rt->nullable &&
                    rt->kind == sema::TypeRef::Kind::Named &&
                    !rt->declaration && !genericSubstSemaTy_.empty()) {
                    sema::TypeRef concNullable;
                    if (nullableTypeParamConcrete(rt, concNullable) &&
                        isNullableValueTy(&concNullable)) {
                        llvm::Value* hasVal = nullablePrimHasValue(v);
                        emitGuardThrow(builder_.CreateNot(hasVal),
                                       "NullReferenceException");
                        return nullablePrimValue(v);
                    }
                }
            }
        }
        // R33: the operand may already be the nullable-value struct
        // {i8 hasValue; <T> value} even when both isNullableValueTy and the
        // #604 type-param resolution above miss it — e.g. a nullable
        // type-parameter FIELD `_v: T?` (the field's TypeRef carries a
        // declaration, so the #604 guard `!rt->declaration` does not fire).
        // Without this, `_v!` returns the whole struct and a `return _v!`
        // emits `ret { i8, i32 }` against an `i32` function → IR-verify
        // failure.  Detect by LLVM shape (element 0 is the hasValue tag) and
        // extract the value slot, exactly as the null-comparison path does.
        if (v->getType()->isStructTy()) {
            auto* st = llvm::cast<llvm::StructType>(v->getType());
            if (st->getNumElements() >= 1 &&
                st->getElementType(0) == nullablePrimHasValTy()) {
                emitGuardThrow(builder_.CreateNot(nullablePrimHasValue(v)),
                               "NullReferenceException");
                return nullablePrimValue(v);
            }
        }
        if (v->getType()->isPointerTy()) {
            emitGuardThrow(builder_.CreateICmpEQ(
                               v, llvm::ConstantPointerNull::get(ptrTy())),
                           "NullReferenceException");
        }
        return v;
    }
    // CG40: `throw` in expression position (§31.2). Evaluate the
    // exception (if any), store + unwind exactly like the statement
    // form, then mark the expression as diverged: the current block is
    // now terminated, so phi-building consumers exclude this branch.
    // A poison placeholder is returned so non-null checks pass; it is
    // never consumed (the block does not reach any join).
    if (auto* th = dynamic_cast<const ast::ThrowExpr*>(&e)) {
        llvm::Value* exc = th->operand ? lowerExpr(*th->operand) : nullptr;
        // #397: pass the AST so emitThrowValue can detect a direct re-throw
        // `throw ex` in expression position (the one case that must not free
        // the caught exception — see emitThrowValue for the full rationale).
        emitThrowValue(exc, th->operand.get());
        diverged_ = true;
        return llvm::PoisonValue::get(ptrTy());
    }
    if (auto* bin = dynamic_cast<const ast::BinaryExpr*>(&e)) {
        return lowerBinary(*bin);
    }
    if (auto* un = dynamic_cast<const ast::UnaryExpr*>(&e)) {
        return lowerUnary(*un);
    }
    if (auto* cond = dynamic_cast<const ast::ConditionalExpr*>(&e)) {
        return lowerConditional(*cond);
    }
    if (auto* call = dynamic_cast<const ast::CallExpr*>(&e)) {
        // CG42: `x?.M(args)` — a call whose callee is a null-conditional
        // member access skips the call when the receiver is null.
        if (call->callee) {
            if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(
                    call->callee.get()))
                return lowerNullCondCall(*call, *nm);
        }
        // CG27c/27f: intrinsic methods on string/primitive receivers.
        if (call->callee) {
            if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(
                    call->callee.get())) {
                if (ma->target) {
                    // Pre-warm lambda param type hint for the target identifier.
                    if (!ctx_.exprType(*ma->target)) {
                        if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(
                                ma->target.get())) {
                            sema::Symbol* sym = ctx_.exprSymbol(*tid);
                            auto hit = lambdaParamTypeHint_.find(sym);
                            if (hit != lambdaParamTypeHint_.end())
                                ctx_.setExprType(ma->target.get(), hit->second);
                        }
                    }
                    auto* tt = ctx_.exprType(*ma->target);
                    // #730: a receiver of bare type-parameter type (`a: T` in a
                    // monomorphized generic body, e.g. `where T: IComparable<T>`
                    // with T=int calling `a.CompareTo(b)`) resolves to its
                    // concrete type via the active substitution. When the param
                    // binds to a PRIMITIVE, substitute it so the primitive-method
                    // paths below (CompareTo, etc.) fire — instead of falling
                    // through to an interface dispatch that returned 0.
                    if (tt && tt->kind == sema::TypeRef::Kind::Named &&
                        !tt->declaration && !genericSubstSemaTy_.empty() &&
                        tt->astNode && tt->astNode->name &&
                        tt->astNode->name->parts.size() == 1) {
                        auto it = genericSubstSemaTy_.find(
                            tt->headName());
                        if (it != genericSubstSemaTy_.end() && it->second &&
                            it->second->kind == sema::TypeRef::Kind::Primitive)
                            tt = const_cast<sema::TypeRef*>(it->second);
                    }
                    // fix #2661 (spec §8.5, v0.252): `Range<T>.ToString()` —
                    // §8.5's AUTO-EQUALITY BODY-RECORD default, which §8.5
                    // names for this type by name ("`Range<T>` (a compiler-
                    // intrinsic body record, stdlib §9.8) carries the auto-
                    // equality body-record default of the bullet above,
                    // rendering its auto-properties"). Was an ACCEPT-TRAP with
                    // the sharpest symptom of the family: sema typed the call
                    // `string` by the universal rule, no arm lowered it, and the
                    // WHOLE enclosing statement disappeared — `Console.WriteLine
                    // ("range: " + r.ToString())` printed nothing at all, exit
                    // 0, no diagnostic (#2661 section B). Placed beside
                    // `Contains` because that is where a Range receiver's member
                    // calls lower; the rendering itself is the shared
                    // `emitRangeToString`, so this and the §18.5 hole agree.
                    if (isRangeType(tt) && ma->member == "ToString" &&
                        call->args.empty()) {
                        llvm::Value* rv = lowerExpr(*ma->target);
                        if (rv) {
                            const sema::TypeRef* rElem =
                                tt->genericArgs.empty() ? nullptr
                                                        : tt->genericArgs[0];
                            if (llvm::Value* s = emitRangeToString(rv, rElem))
                                return s;
                        }
                    }
                    // fix #499 / §9.8: Range<T>.Contains(x) inside the
                    // CallExpr dispatch path (ma->target has Range type).
                    if (isRangeType(tt) && ma->member == "Contains" &&
                        call->args.size() == 1 && call->args[0].value) {
                        llvm::Value* rv = lowerExpr(*ma->target);
                        llvm::Value* x  = lowerExpr(*call->args[0].value);
                        if (rv && x) {
                            // §16.3 (v0.162, fix #1340): Contains = nonempty AND
                            // within-extent AND endOk. Step affects membership only
                            // through direction/emptiness; open-ended sides are never
                            // empty. For a DESCENDING range (step < 0, Start > End)
                            // the lo/hi roles are reversed; the excluded point is
                            // always End (the written endpoint), regardless of direction.
                            llvm::Value* st   = builder_.CreateExtractValue(rv, {0u}, "rc.st");
                            llvm::Value* en   = builder_.CreateExtractValue(rv, {1u}, "rc.en");
                            llvm::Value* step = builder_.CreateExtractValue(rv, {2u}, "rc.step");
                            llvm::Value* flg  = builder_.CreateExtractValue(rv, {3u}, "rc.flg");
                            llvm::Value* xW   = x->getType() == i64()
                                                    ? x
                                                    : builder_.CreateSExt(x, i64(), "rc.xw");
                            // §16.3 (v0.162, fix #1496): a null endpoint (flags bit
                            // 1 = startOpen, bit 2 = endOpen) imposes NO bound on its
                            // side. `st`/`en` hold a fake 0 sentinel on an open side
                            // (see the RangeExpr-in-value-position lowering above),
                            // so they must be excluded from the direction/extent
                            // computation rather than compared as real bounds.
                            auto* i8Ty      = llvm::Type::getInt8Ty(llvm_);
                            llvm::Value* startOpen = builder_.CreateICmpNE(
                                builder_.CreateAnd(flg, llvm::ConstantInt::get(i8Ty, 2)),
                                llvm::ConstantInt::get(i8Ty, 0), "rc.sopen");
                            llvm::Value* endOpen = builder_.CreateICmpNE(
                                builder_.CreateAnd(flg, llvm::ConstantInt::get(i8Ty, 4)),
                                llvm::ConstantInt::get(i8Ty, 0), "rc.eopen");
                            llvm::Value* closedR = builder_.CreateAnd(
                                builder_.CreateNot(startOpen),
                                builder_.CreateNot(endOpen), "rc.closed");
                            llvm::Value* asc  = builder_.CreateICmpSLE(st, en, "rc.asc");
                            llvm::Value* loClosed = builder_.CreateSelect(asc, st, en, "rc.loc");
                            llvm::Value* hiClosed = builder_.CreateSelect(asc, en, st, "rc.hic");
                            llvm::Value* negInf =
                                builder_.getInt64(std::numeric_limits<int64_t>::min());
                            llvm::Value* posInf =
                                builder_.getInt64(std::numeric_limits<int64_t>::max());
                            llvm::Value* lo = builder_.CreateSelect(
                                startOpen, negInf,
                                builder_.CreateSelect(closedR, loClosed, st), "rc.lo");
                            llvm::Value* hi = builder_.CreateSelect(
                                endOpen, posInf,
                                builder_.CreateSelect(closedR, hiClosed, en), "rc.hi");
                            llvm::Value* ge   = builder_.CreateICmpSGE(xW, lo, "rc.ge");
                            llvm::Value* le   = builder_.CreateICmpSLE(xW, hi, "rc.le");
                            llvm::Value* inR  = builder_.CreateAnd(ge, le, "rc.in");
                            llvm::Value* incl = builder_.CreateTrunc(flg, i1(), "rc.incl");
                            llvm::Value* notEnd =
                                builder_.CreateICmpNE(xW, en, "rc.notEnd");
                            // half-open (!incl) excludes the End point — only when
                            // End is actually present; an open End has no endpoint
                            // to exclude.
                            llvm::Value* endOk = builder_.CreateOr(
                                endOpen,
                                builder_.CreateOr(incl, notEnd), "rc.endOk");
                            // IsEmpty check (§16.3 v0.162): open-ended ranges never
                            // empty (flags bits 1/2); otherwise direction-dependent.
                            //   ascending (step >= 0): closed → st > en; half → st >= en
                            //   descending (step < 0):  closed → st < en; half → st <= en
                            llvm::Value* isOE =
                                builder_.CreateOr(startOpen, endOpen, "rc.oe");
                            llvm::Value* stepNeg =
                                builder_.CreateICmpSLT(step,
                                    llvm::ConstantInt::get(i64(), 0), "rc.sneg");
                            llvm::Value* ascIncl  = builder_.CreateICmpSGT(st, en, "rc.ai");
                            llvm::Value* ascHalf  = builder_.CreateICmpSGE(st, en, "rc.ah");
                            llvm::Value* descIncl = builder_.CreateICmpSLT(st, en, "rc.di");
                            llvm::Value* descHalf = builder_.CreateICmpSLE(st, en, "rc.dh");
                            llvm::Value* emptyAsc =
                                builder_.CreateSelect(incl, ascIncl, ascHalf, "rc.ea");
                            llvm::Value* emptyDesc =
                                builder_.CreateSelect(incl, descIncl, descHalf, "rc.ed");
                            llvm::Value* dirEmpty =
                                builder_.CreateSelect(stepNeg, emptyDesc, emptyAsc, "rc.de");
                            // notEmpty = isOpenEnded || !dirEmpty
                            llvm::Value* notEmpty =
                                builder_.CreateOr(isOE,
                                    builder_.CreateNot(dirEmpty), "rc.ne");
                            return builder_.CreateAnd(
                                notEmpty,
                                builder_.CreateAnd(inR, endOk, "rc.chk"), "rc.res");
                        }
                    }
                    if (tt && tt->kind == sema::TypeRef::Kind::Primitive) {
                        if (tt->primitive == sema::PrimitiveType::String) {
                            // CG27f: stdlib §9.6 string methods.
                            // #429 (OPERATOR-OPERAND category) / §4.6 / §28.9:
                            // pre-lower the receiver so we can release it after
                            // the method call when it is a +1 string producer
                            // (concat / interpolation / primitive-ToString /
                            // user-method-return / range-slice). The receiver is
                            // consumed by the method (only read; it is a borrow
                            // inside tryLowerStringMethod), and has no named-local
                            // owner — nothing else releases it.  Only pre-lower when
                            // the target IS a +1 producer, so non-producer receivers
                            // still flow through the existing path inside
                            // tryLowerStringMethod without any change.
                            bool selfIsProducer =
                                ma->target &&
                                isPlusOneStringProducer(ma->target.get());
                            llvm::Value* selfV =
                                selfIsProducer ? lowerExpr(*ma->target) : nullptr;
                            if (auto* v = tryLowerStringMethod(*call, *ma, selfV)) {
                                // Release the +1 producer receiver AFTER the
                                // method has been lowered (result is in hand;
                                // string still live for the method's reads).
                                if (selfIsProducer && selfV)
                                    emitStringRelease(selfV);
                                return v;
                            }
                            // tryLowerStringMethod returned nullptr (method not
                            // recognised by the structurally-matched §9.6 paths).
                            // If we pre-lowered the receiver, it is harmless to
                            // proceed: tryLowerStringMethod re-lowers when selfV
                            // is null, but since we passed selfV it skipped the
                            // lowerExpr call — so it just returned nullptr without
                            // side-effects. The fall-through paths below do NOT
                            // lower ma->target again for a string receiver type
                            // (they only apply to non-string primitives).
                            // Releasing selfV on the nullptr path is SAFE: nothing
                            // else will use the lowered value since we are about
                            // to return nullptr anyway. This avoids a leak if the
                            // receiver was a producer and tryLowerStringMethod
                            // happened to not recognise the method.
                            if (selfIsProducer && selfV)
                                emitStringRelease(selfV);
                        }
                        // CG27c: ToString() on any primitive (no arg). §8.5/§8.6
                        // (#417): an OBJECT-typed receiver is NOT a value
                        // primitive — `o.ToString()` dispatches through the
                        // uniform vtable ToString slot (a boxed value / class) or
                        // returns a contained string. emitPrimitiveToString on an
                        // Object pointer would snprintf the pointer as "%d".
                        if (call->args.empty() && ma->member == "ToString" &&
                            tt->primitive != sema::PrimitiveType::Object) {
                            llvm::Value* recv = lowerExpr(*ma->target);
                            // fix #3481 (§8.5 / §7.4 / §12.7, v0.283): a
                            // NULLABLE primitive receiver is not the bare
                            // primitive. `tt->kind == Primitive` is true of
                            // `int?` exactly as of `int`, and this arm never
                            // asked `tt->nullable` — so `v.ToString()` on a
                            // §7.4-narrowed `int?` handed the whole
                            // `{i8 hasValue; T}` tagged struct to
                            // `emitPrimitiveToString`. §7.4 makes the call
                            // legal (the narrowing is established at sema,
                            // §12.7's flow rule), so this is not a rejected
                            // program: it MISCOMPILED. Measured on 0c5ecd1,
                            // one statement per compile — `int? := 5` and
                            // `int? := 12345` both printed `1` (snprintf `%d`
                            // over the struct read the hasValue TAG, so the
                            // payload was unobservable), `decimal? := 2.5m`
                            // printed `109291918593`, and `bool?`/`double?`/
                            // `float?`/`char?`/`byte?` were IR-verifier aborts
                            // (`Trunc only operates on integer:
                            //  %2 = trunc { i8, i1 } %1 to i1`) because
                            // `emitPrimitiveToString`'s arms trunc/zext/pass
                            // the value as its bare LLVM type.
                            //
                            // `emitValueToString` is handed the NULLABLE
                            // TypeRef, not an unwrapped payload: it is #2913's
                            // shared §8.5 authority and its FIRST arm is
                            // exactly this shape — branch on the tag, recurse
                            // on the non-nullable inner type for a present
                            // value, and render §18.5's `null` otherwise. That
                            // arm is already what `Console.WriteLine(v)`
                            // (LlvmBackend_Calls.cpp's #581/#583 branch), a
                            // §18.5 hole, a record COMPONENT, and
                            // `v?.ToString()`
                            // (`lowerNullCondValueSemCall`) all reach for this
                            // very value — each of which rendered `12345`
                            // correctly while this spelling did not. Rendering
                            // the payload unconditionally here would answer a
                            // present value correctly and a null one with the
                            // zero-filled slot; delegating keeps ONE answer for
                            // both, and keeps this site from becoming a second
                            // spelling of a rule that already has one.
                            if (recv && isNullableValueTy(tt))
                                return emitValueToString(recv, tt);
                            if (recv)
                                return emitPrimitiveToString(recv, tt->primitive);
                        }
                        if (call->args.empty() && ma->member == "ToString" &&
                            tt->primitive == sema::PrimitiveType::Object) {
                            llvm::Value* recv = lowerExpr(*ma->target);
                            if (llvm::Value* r = lowerObjectToString(recv))
                                return r;
                        }
                        // §6.3 / stdlib §9.2: FP predicates as instance methods
                        // (x.IsNaN(), x.IsInfinity(), x.IsPositiveInfinity(),
                        //  x.IsNegativeInfinity(), x.IsFinite()).
                        // All produce i1 directly via inline fcmp sequences.
                        if (call->args.empty()) {
                            using P = sema::PrimitiveType;
                            bool isFP = tt->primitive == P::Double || tt->primitive == P::Float;
                            if (ma->member == "IsNaN" && isFP) {
                                llvm::Value* recv = lowerExpr(*ma->target);
                                if (!recv) return nullptr;
                                // fcmp uno %x, %x — true iff x is NaN (per §9.2).
                                return builder_.CreateFCmpUNO(recv, recv, "isnan");
                            }
                            if (ma->member == "IsInfinity" && isFP) {
                                llvm::Value* recv = lowerExpr(*ma->target);
                                if (!recv) return nullptr;
                                auto* fty = recv->getType();
                                auto* posInf = llvm::ConstantFP::getInfinity(fty, false);
                                auto* negInf = llvm::ConstantFP::getInfinity(fty, true);
                                return builder_.CreateOr(
                                    builder_.CreateFCmpOEQ(recv, posInf),
                                    builder_.CreateFCmpOEQ(recv, negInf));
                            }
                            // stdlib §9.2: IsPositiveInfinity — fcmp oeq %x, +inf.
                            if (ma->member == "IsPositiveInfinity" && isFP) {
                                llvm::Value* recv = lowerExpr(*ma->target);
                                if (!recv) return nullptr;
                                auto* posInf = llvm::ConstantFP::getInfinity(recv->getType(), false);
                                return builder_.CreateFCmpOEQ(recv, posInf, "isposinf");
                            }
                            // stdlib §9.2: IsNegativeInfinity — fcmp oeq %x, -inf.
                            if (ma->member == "IsNegativeInfinity" && isFP) {
                                llvm::Value* recv = lowerExpr(*ma->target);
                                if (!recv) return nullptr;
                                auto* negInf = llvm::ConstantFP::getInfinity(recv->getType(), true);
                                return builder_.CreateFCmpOEQ(recv, negInf, "isneginf");
                            }
                            if (ma->member == "IsFinite" && isFP) {
                                llvm::Value* recv = lowerExpr(*ma->target);
                                if (!recv) return nullptr;
                                // IsFinite = !IsNaN && !IsInfinity (per §9.2).
                                auto* fty = recv->getType();
                                llvm::Value* isNaN = builder_.CreateFCmpUNO(recv, recv);
                                llvm::Value* isInf = builder_.CreateOr(
                                    builder_.CreateFCmpOEQ(recv, llvm::ConstantFP::getInfinity(fty, false)),
                                    builder_.CreateFCmpOEQ(recv, llvm::ConstantFP::getInfinity(fty, true)));
                                return builder_.CreateNot(builder_.CreateOr(isNaN, isInf), "isfinite");
                            }
                            if (ma->member == "Abs") {
                                llvm::Value* recv = lowerExpr(*ma->target);
                                if (!recv) return nullptr;
                                if (recv->getType()->isFloatingPointTy()) {
                                    auto* fn = llvm::Intrinsic::getDeclaration(
                                        module_.get(), llvm::Intrinsic::fabs, {recv->getType()});
                                    return builder_.CreateCall(fn, {recv}, "fabs");
                                } else {
                                    llvm::Value* isNeg = builder_.CreateICmpSLT(
                                        recv, llvm::ConstantInt::get(recv->getType(), 0));
                                    return builder_.CreateSelect(isNeg, builder_.CreateNeg(recv), recv, "abs");
                                }
                            }
                        }
                        // fix #586 / §2.8 / stdlib §9.1–§9.5: CompareTo(other: T): int
                        // — IComparable<T> contract for numeric and char primitives.
                        // Not string (handled by tryLowerStringMethod above) and not
                        // bool (which does not implement IComparable, per §9.4).
                        // Returns: negative if self < other, 0 if equal, positive if
                        // self > other.  Formula: (a > b) - (a < b) → -1, 0, or 1.
                        // Unsigned types use unsigned comparison (no negation needed;
                        // the result int is still a signed i32).
                        // Floating-point uses ordered (quiet-NaN-aware) comparisons
                        // per §9.2; NaN < everything in the ordered sense (fcmp olt).
                        if (call->args.size() == 1 && call->args[0].value &&
                            ma->member == "CompareTo") {
                            using P = sema::PrimitiveType;
                            P pt = tt->primitive;
                            bool isSignedInt   = pt == P::Int   || pt == P::Long  ||
                                                 pt == P::Short || pt == P::SByte ||
                                                 pt == P::Char;
                            bool isUnsignedInt = pt == P::UInt  || pt == P::ULong ||
                                                 pt == P::UShort|| pt == P::Byte;
                            bool isFP          = pt == P::Double|| pt == P::Float;
                            if (isSignedInt || isUnsignedInt || isFP) {
                                llvm::Value* lhs = lowerExpr(*ma->target);
                                llvm::Value* rhs = lowerExpr(*call->args[0].value);
                                if (!lhs || !rhs) return nullptr;
                                // Widen both sides to a uniform LLVM type if needed
                                // (e.g. char is i32 code-point in codegen).
                                if (lhs->getType() != rhs->getType()) {
                                    if (isFP) {
                                        // Widen float to double.
                                        auto* dty = llvm::Type::getDoubleTy(llvm_);
                                        lhs = builder_.CreateFPExt(lhs, dty, "cmp.ldw");
                                        rhs = builder_.CreateFPExt(rhs, dty, "cmp.rdw");
                                    } else {
                                        // Integer mismatch: widen narrower side.
                                        unsigned lb = lhs->getType()->getIntegerBitWidth();
                                        unsigned rb = rhs->getType()->getIntegerBitWidth();
                                        if (lb < rb) {
                                            lhs = isUnsignedInt
                                                ? builder_.CreateZExt(lhs, rhs->getType(), "cmp.lzx")
                                                : builder_.CreateSExt(lhs, rhs->getType(), "cmp.lsx");
                                        } else {
                                            rhs = isUnsignedInt
                                                ? builder_.CreateZExt(rhs, lhs->getType(), "cmp.rzx")
                                                : builder_.CreateSExt(rhs, lhs->getType(), "cmp.rsx");
                                        }
                                    }
                                }
                                // Emit: (a > b) as i32 - (a < b) as i32
                                llvm::Value* gt, *lt;
                                if (isFP) {
                                    gt = builder_.CreateFCmpOGT(lhs, rhs, "cmp.gt");
                                    lt = builder_.CreateFCmpOLT(lhs, rhs, "cmp.lt");
                                } else if (isUnsignedInt) {
                                    gt = builder_.CreateICmpUGT(lhs, rhs, "cmp.gt");
                                    lt = builder_.CreateICmpULT(lhs, rhs, "cmp.lt");
                                } else {
                                    gt = builder_.CreateICmpSGT(lhs, rhs, "cmp.gt");
                                    lt = builder_.CreateICmpSLT(lhs, rhs, "cmp.lt");
                                }
                                llvm::Value* gtI = builder_.CreateZExt(gt, i32(), "cmp.gti");
                                llvm::Value* ltI = builder_.CreateZExt(lt, i32(), "cmp.lti");
                                return builder_.CreateSub(gtI, ltI, "cmp.res");
                            }
                        }
                        // #863 / stdlib §9.1–§9.5: Equals(other: T): bool —
                        // IEquatable<T> contract for numeric, char, and bool
                        // primitives (string is handled by tryLowerStringMethod
                        // above). Value equality; FP uses ordered eq (oeq). This
                        // is reached for `a.Equals(b)` on a generic `T` bound
                        // `where T: IEquatable<T>` after the #730 substitution
                        // resolves the bare type-parameter receiver to its concrete
                        // primitive type — without this handler the call fell
                        // through to an interface dispatch that returned false.
                        if (call->args.size() == 1 && call->args[0].value &&
                            ma->member == "Equals") {
                            using P = sema::PrimitiveType;
                            P pt = tt->primitive;
                            bool isUnsignedInt = pt == P::UInt || pt == P::ULong ||
                                                 pt == P::UShort || pt == P::Byte;
                            bool isInt = pt == P::Int   || pt == P::Long  ||
                                         pt == P::Short || pt == P::SByte ||
                                         pt == P::Char  || pt == P::Bool  ||
                                         isUnsignedInt;
                            bool isFP  = pt == P::Double || pt == P::Float;
                            if (isInt || isFP) {
                                llvm::Value* lhs = lowerExpr(*ma->target);
                                llvm::Value* rhs = lowerExpr(*call->args[0].value);
                                if (!lhs || !rhs) return nullptr;
                                if (lhs->getType() != rhs->getType()) {
                                    if (isFP) {
                                        auto* dty = llvm::Type::getDoubleTy(llvm_);
                                        lhs = builder_.CreateFPExt(lhs, dty, "eq.ldw");
                                        rhs = builder_.CreateFPExt(rhs, dty, "eq.rdw");
                                    } else {
                                        unsigned lb = lhs->getType()->getIntegerBitWidth();
                                        unsigned rb = rhs->getType()->getIntegerBitWidth();
                                        if (lb < rb)
                                            lhs = isUnsignedInt
                                                ? builder_.CreateZExt(lhs, rhs->getType(), "eq.lzx")
                                                : builder_.CreateSExt(lhs, rhs->getType(), "eq.lsx");
                                        else if (rb < lb)
                                            rhs = isUnsignedInt
                                                ? builder_.CreateZExt(rhs, lhs->getType(), "eq.rzx")
                                                : builder_.CreateSExt(rhs, lhs->getType(), "eq.rsx");
                                    }
                                }
                                return isFP
                                    ? builder_.CreateFCmpOEQ(lhs, rhs, "eq.res")
                                    : builder_.CreateICmpEQ(lhs, rhs, "eq.res");
                            }
                        }
                        // §9.x: ToString(format) — formatted numeric string.
                        if (call->args.size() == 1 && call->args[0].value &&
                            ma->member == "ToString") {
                            llvm::Value* recv = lowerExpr(*ma->target);
                            if (!recv) return nullptr;
                            // §9.3 (#1099B): decimal ToString(format) — route to
                            // the runtime decimal formatter (honors the "F"/"f"
                            // fixed-point specifier; other/malformed formats fall
                            // back to plain ToString). The generic numeric path
                            // below only handles float/double (F/E/G) and int
                            // (D/X), so a decimal "F2" fell to emitPrimitiveToString
                            // with the format DISCARDED (`1.5m.ToString("F2")` →
                            // "1.5" instead of "1.50"). Passing the lowered format
                            // value supports a non-literal format string too.
                            if (tt->primitive == sema::PrimitiveType::Decimal) {
                                llvm::Value* fmtVal =
                                    lowerExpr(*call->args[0].value);
                                if (!fmtVal) return nullptr;
                                llvm::AllocaInst* dslot =
                                    entryAlloca(decimalStructType());
                                builder_.CreateStore(recv, dslot);
                                auto* fn = declareC("bz_decimal_to_string_fmt",
                                                    ptrTy(), {ptrTy(), ptrTy()});
                                llvm::Value* res = builder_.CreateCall(
                                    fn, {dslot, fmtVal}, "dec.tostrf");
                                // nullptr → unrecognized specifier → FormatException
                                // (§2.18 strict-fault, #1412).
                                llvm::Value* isNull = builder_.CreateICmpEQ(
                                    res,
                                    llvm::Constant::getNullValue(ptrTy()),
                                    "dec.fmt.null");
                                emitGuardThrow(isNull, "FormatException");
                                return res;
                            }
                            // Try to extract the format specifier from a literal arg.
                            std::string bezelFmt;
                            if (auto* sl = dynamic_cast<const ast::StringLiteralExpr*>(
                                    call->args[0].value.get())) {
                                bezelFmt = processStringText(sl->text, false);
                            }
                            if (!bezelFmt.empty()) {
                                // Translate Bezel format spec to printf format.
                                // "Fn" / "fn" → "%.Nf" (fixed decimal)
                                // "En" / "en" → "%.Ne" (scientific)
                                // "Gn" / "gn" → "%.Ng" (general)
                                // "X" / "x" → "%X" / "%x" (hex integer)
                                // "D" / "d" → "%d" (decimal integer)
                                std::string printfFmt;
                                char spec = std::toupper(bezelFmt[0]);
                                std::string digits = bezelFmt.size() > 1 ? bezelFmt.substr(1) : "";
                                using P = sema::PrimitiveType;
                                bool isFP = tt->primitive == P::Double || tt->primitive == P::Float;
                                if ((spec == 'F' || spec == 'G' || spec == 'E') && isFP) {
                                    char pfspec = spec == 'F' ? 'f' : (spec == 'E' ? 'e' : 'g');
                                    printfFmt = "%." + (digits.empty() ? "2" : digits) + pfspec;
                                    // Widen float to double for snprintf.
                                    if (tt->primitive == P::Float)
                                        recv = builder_.CreateFPExt(recv, llvm::Type::getDoubleTy(llvm_));
                                } else if (spec == 'X' || spec == 'D') {
                                    // §18.5: D/X with digits = min width, zero-padded.
                                    // D5 → %05d, X4 → %04X.
                                    char pfspec2 = spec == 'X' ? 'X' : 'd';
                                    if (!digits.empty())
                                        printfFmt = std::string("%0") + digits + pfspec2;
                                    else
                                        printfFmt = spec == 'X' ? "%X" : "%d";
                                    if (!recv->getType()->isIntegerTy(32))
                                        recv = builder_.CreateFPToSI(recv, i32());
                                } else {
                                    return emitPrimitiveToString(recv, tt->primitive);
                                }
                                // Emit snprintf with the translated format.
                                llvm::Constant* fmtData = llvm::ConstantDataArray::getString(
                                    llvm_, printfFmt, true);
                                unsigned fid = stringLiteralIdx_++;
                                auto* fmtGV = new llvm::GlobalVariable(*module_, fmtData->getType(), true,
                                    llvm::GlobalValue::PrivateLinkage, fmtData, ".bz.ts.fmt." + std::to_string(fid));
                                fmtGV->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
                                auto* snpFn = module_->getFunction("snprintf");
                                if (!snpFn) {
                                    auto* ft = llvm::FunctionType::get(i32(), {ptrTy(), i64(), ptrTy()}, true);
                                    snpFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "snprintf", module_.get());
                                }
                                auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                                llvm::Value* buf = entryAlloca(llvm::ArrayType::get(i8Ty, 64));
                                llvm::Value* bufPtr = builder_.CreateGEP(llvm::ArrayType::get(i8Ty, 64), buf, {builder_.getInt64(0), builder_.getInt64(0)});
                                llvm::Value* wr = builder_.CreateCall(snpFn, {bufPtr, builder_.getInt64(64), fmtGV, recv}, "ts.wr");
                                auto* mallocFn = declareC("malloc", ptrTy(), {i64()});
                                auto* memcpyFn = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
                                llvm::Value* l64 = builder_.CreateSExt(wr, i64());
                                llvm::Value* dSz = builder_.CreateAdd(l64, builder_.getInt64(1));
                                llvm::Value* dc = builder_.CreateCall(mallocFn, {dSz}, "ts.data");
                                builder_.CreateCall(memcpyFn, {dc, bufPtr, l64});
                                llvm::Value* np = builder_.CreateGEP(i8Ty, dc, l64);
                                builder_.CreateStore(llvm::ConstantInt::get(i8Ty, 0), np);
                                uint64_t strBytes = module_->getDataLayout().getTypeAllocSize(stringType());
                                llvm::Value* so = builder_.CreateCall(mallocFn, {builder_.getInt64(strBytes)}, "ts.obj");
                                builder_.CreateStore(dc, builder_.CreateStructGEP(stringType(), so, 0));
                                builder_.CreateStore(l64, builder_.CreateStructGEP(stringType(), so, 1));
                                builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 1),
                                    builder_.CreateStructGEP(stringType(), so, 2));
                                initStringRc(so);  // #422: producer's reference, rc := 1
                                return so;
                            }
                            // stdlib §9.1 (#518): ToString(radix: int) — the arg is an
                            // integer (not a string format literal). Route to the same
                            // inline radix-conversion logic as Convert.ToString(n, radix).
                            // Supported radices: 2, 8, 10, 16 (per stdlib §9.1).
                            {
                                using P = sema::PrimitiveType;
                                bool isIntRecv = (tt->primitive == P::Int ||
                                                  tt->primitive == P::Long ||
                                                  tt->primitive == P::UInt ||
                                                  tt->primitive == P::ULong ||
                                                  tt->primitive == P::Byte ||
                                                  tt->primitive == P::SByte ||
                                                  tt->primitive == P::Short ||
                                                  tt->primitive == P::UShort);
                                auto* argTy = ctx_.exprType(*call->args[0].value);
                                bool argIsInt = argTy &&
                                    argTy->kind == sema::TypeRef::Kind::Primitive &&
                                    (argTy->primitive == P::Int ||
                                     argTy->primitive == P::Long);
                                if (isIntRecv && argIsInt) {
                                    llvm::Value* radix = lowerExpr(*call->args[0].value);
                                    if (!radix) return nullptr;
                                    // Normalize receiver to i32 (or i64 for long).
                                    bool isLong = (tt->primitive == P::Long ||
                                                   tt->primitive == P::ULong);
                                    if (isLong) {
                                        if (!recv->getType()->isIntegerTy(64))
                                            recv = builder_.CreateSExt(recv, i64());
                                    } else {
                                        if (!recv->getType()->isIntegerTy(32))
                                            recv = builder_.CreateSExt(recv, i32());
                                    }
                                    // Resolve radix from a constant int.
                                    const char* fmt = "%d";  // fallback: decimal
                                    bool isBinary = false;
                                    if (auto* cr = llvm::dyn_cast<llvm::ConstantInt>(radix)) {
                                        uint64_t r = cr->getZExtValue();
                                        if (r == 16) fmt = isLong ? "%llx" : "%x";
                                        else if (r == 8) fmt = isLong ? "%llo" : "%o";
                                        else if (r == 2) isBinary = true;
                                        else fmt = isLong ? "%lld" : "%d";
                                    } else {
                                        // Non-constant radix: fall back to decimal.
                                        fmt = isLong ? "%lld" : "%d";
                                    }
                                    // Binary path: emit a bit-scan loop.
                                    auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                                    const unsigned BUF = 66;  // 64 bits + sign + NUL
                                    if (isBinary) {
                                        // Reuse the Convert.ToString(n,2) inline logic.
                                        // Widen to i64 so the same loop handles both int and long.
                                        llvm::Value* n64 = isLong
                                            ? recv
                                            : builder_.CreateSExt(recv, i64());
                                        auto* buf2 = entryAlloca(llvm::ArrayType::get(i8Ty, BUF));
                                        llvm::Value* b2Ptr = builder_.CreateGEP(
                                            llvm::ArrayType::get(i8Ty, BUF), buf2,
                                            {builder_.getInt64(0), builder_.getInt64(0)});
                                        llvm::Value* isZero = builder_.CreateICmpEQ(
                                            n64, builder_.getInt64(0));
                                        auto* zeroBB = llvm::BasicBlock::Create(
                                            llvm_, "bn2.z", currentFn_);
                                        auto* nonBB  = llvm::BasicBlock::Create(
                                            llvm_, "bn2.nz", currentFn_);
                                        auto* binBB  = llvm::BasicBlock::Create(
                                            llvm_, "bn2.e", currentFn_);
                                        builder_.CreateCondBr(isZero, zeroBB, nonBB);
                                        // zero → "0"
                                        builder_.SetInsertPoint(zeroBB);
                                        builder_.CreateStore(
                                            llvm::ConstantInt::get(i8Ty, '0'), b2Ptr);
                                        llvm::Value* z1 = builder_.CreateGEP(
                                            i8Ty, b2Ptr, builder_.getInt64(1));
                                        builder_.CreateStore(
                                            llvm::ConstantInt::get(i8Ty, 0), z1);
                                        llvm::Value* zlen = builder_.getInt64(1);
                                        auto* zeroBBEnd = builder_.GetInsertBlock();
                                        builder_.CreateBr(binBB);
                                        // non-zero: scan MSB then emit each bit.
                                        builder_.SetInsertPoint(nonBB);
                                        auto* bitPos = entryAlloca(i32());
                                        builder_.CreateStore(builder_.getInt32(63), bitPos);
                                        auto* fhBB = llvm::BasicBlock::Create(
                                            llvm_, "bn2.fh", currentFn_);
                                        auto* fhBodyBB = llvm::BasicBlock::Create(
                                            llvm_, "bn2.fhb", currentFn_);
                                        auto* fhEndBB = llvm::BasicBlock::Create(
                                            llvm_, "bn2.fhe", currentFn_);
                                        builder_.CreateBr(fhBB);
                                        builder_.SetInsertPoint(fhBB);
                                        llvm::Value* bp = builder_.CreateLoad(i32(), bitPos);
                                        llvm::Value* bpGe0 = builder_.CreateICmpSGT(
                                            bp, builder_.getInt32(0));
                                        llvm::Value* bpW = builder_.CreateZExt(bp, i64());
                                        llvm::Value* bit = builder_.CreateAnd(
                                            builder_.CreateLShr(n64, bpW),
                                            builder_.getInt64(1));
                                        llvm::Value* isOne = builder_.CreateICmpEQ(
                                            bit, builder_.getInt64(1));
                                        builder_.CreateCondBr(
                                            builder_.CreateAnd(bpGe0,
                                                builder_.CreateNot(isOne)),
                                            fhBodyBB, fhEndBB);
                                        builder_.SetInsertPoint(fhBodyBB);
                                        builder_.CreateStore(
                                            builder_.CreateSub(bp, builder_.getInt32(1)),
                                            bitPos);
                                        builder_.CreateBr(fhBB);
                                        builder_.SetInsertPoint(fhEndBB);
                                        auto* emBP  = entryAlloca(i32());
                                        auto* emLen = entryAlloca(i64());
                                        llvm::Value* startBP = builder_.CreateLoad(
                                            i32(), bitPos);
                                        builder_.CreateStore(startBP, emBP);
                                        builder_.CreateStore(builder_.getInt64(0), emLen);
                                        auto* emCBB = llvm::BasicBlock::Create(
                                            llvm_, "bn2.ec", currentFn_);
                                        auto* emBBB = llvm::BasicBlock::Create(
                                            llvm_, "bn2.eb", currentFn_);
                                        auto* emEBB = llvm::BasicBlock::Create(
                                            llvm_, "bn2.ee", currentFn_);
                                        builder_.CreateBr(emCBB);
                                        builder_.SetInsertPoint(emCBB);
                                        llvm::Value* ep = builder_.CreateLoad(i32(), emBP);
                                        builder_.CreateCondBr(
                                            builder_.CreateICmpSGE(ep, builder_.getInt32(0)),
                                            emBBB, emEBB);
                                        builder_.SetInsertPoint(emBBB);
                                        llvm::Value* epB = builder_.CreateLoad(i32(), emBP);
                                        llvm::Value* lenV = builder_.CreateLoad(i64(), emLen);
                                        llvm::Value* epW = builder_.CreateZExt(epB, i64());
                                        llvm::Value* d = builder_.CreateAnd(
                                            builder_.CreateLShr(n64, epW),
                                            builder_.getInt64(1));
                                        llvm::Value* ch = builder_.CreateAdd(
                                            builder_.CreateTrunc(d, i8Ty),
                                            llvm::ConstantInt::get(i8Ty, '0'));
                                        llvm::Value* dp = builder_.CreateGEP(
                                            i8Ty, b2Ptr, lenV);
                                        builder_.CreateStore(ch, dp);
                                        builder_.CreateStore(
                                            builder_.CreateAdd(lenV, builder_.getInt64(1)),
                                            emLen);
                                        builder_.CreateStore(
                                            builder_.CreateSub(epB, builder_.getInt32(1)),
                                            emBP);
                                        builder_.CreateBr(emCBB);
                                        builder_.SetInsertPoint(emEBB);
                                        llvm::Value* nlen = builder_.CreateLoad(i64(), emLen);
                                        llvm::Value* np2 = builder_.CreateGEP(
                                            i8Ty, b2Ptr, nlen);
                                        builder_.CreateStore(
                                            llvm::ConstantInt::get(i8Ty, 0), np2);
                                        auto* nonBBEnd = builder_.GetInsertBlock();
                                        builder_.CreateBr(binBB);
                                        // merge
                                        builder_.SetInsertPoint(binBB);
                                        auto* phiLen = builder_.CreatePHI(i64(), 2, "bn2.len");
                                        phiLen->addIncoming(zlen, zeroBBEnd);
                                        phiLen->addIncoming(nlen, nonBBEnd);
                                        auto* mallocFn2 = declareC("malloc", ptrTy(), {i64()});
                                        auto* memcpyFn2 = declareC("memcpy", ptrTy(),
                                                                    {ptrTy(), ptrTy(), i64()});
                                        llvm::Value* dSz2 = builder_.CreateAdd(
                                            phiLen, builder_.getInt64(1));
                                        llvm::Value* dc2 = builder_.CreateCall(
                                            mallocFn2, {dSz2}, "bn2.d");
                                        builder_.CreateCall(memcpyFn2, {dc2, b2Ptr, phiLen});
                                        llvm::Value* np3 = builder_.CreateGEP(
                                            i8Ty, dc2, phiLen);
                                        builder_.CreateStore(
                                            llvm::ConstantInt::get(i8Ty, 0), np3);
                                        uint64_t sBytes2 = module_->getDataLayout()
                                            .getTypeAllocSize(stringType());
                                        llvm::Value* so2 = builder_.CreateCall(
                                            mallocFn2,
                                            {builder_.getInt64(sBytes2)}, "bn2.o");
                                        builder_.CreateStore(
                                            dc2,
                                            builder_.CreateStructGEP(stringType(), so2, 0));
                                        builder_.CreateStore(
                                            phiLen,
                                            builder_.CreateStructGEP(stringType(), so2, 1));
                                        builder_.CreateStore(
                                            llvm::ConstantInt::get(i8Ty, 1),
                                            builder_.CreateStructGEP(stringType(), so2, 2));
                                        initStringRc(so2);
                                        return so2;
                                    }
                                    // Non-binary: snprintf with the chosen format.
                                    llvm::Constant* fmtData2 = llvm::ConstantDataArray::getString(
                                        llvm_, fmt, true);
                                    unsigned fid2 = stringLiteralIdx_++;
                                    auto* fmtGV2 = new llvm::GlobalVariable(
                                        *module_, fmtData2->getType(), true,
                                        llvm::GlobalValue::PrivateLinkage, fmtData2,
                                        ".bz.rts.fmt." + std::to_string(fid2));
                                    fmtGV2->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
                                    auto* snpFn2 = module_->getFunction("snprintf");
                                    if (!snpFn2) {
                                        auto* ft2 = llvm::FunctionType::get(
                                            i32(), {ptrTy(), i64(), ptrTy()}, true);
                                        snpFn2 = llvm::Function::Create(
                                            ft2, llvm::Function::ExternalLinkage,
                                            "snprintf", module_.get());
                                    }
                                    auto* buf3 = entryAlloca(llvm::ArrayType::get(i8Ty, BUF));
                                    llvm::Value* bPtr3 = builder_.CreateGEP(
                                        llvm::ArrayType::get(i8Ty, BUF), buf3,
                                        {builder_.getInt64(0), builder_.getInt64(0)});
                                    llvm::Value* wr2 = builder_.CreateCall(
                                        snpFn2,
                                        {bPtr3, builder_.getInt64(BUF), fmtGV2, recv},
                                        "rts.wr");
                                    auto* mallocFn3 = declareC("malloc", ptrTy(), {i64()});
                                    auto* memcpyFn3 = declareC("memcpy", ptrTy(),
                                                               {ptrTy(), ptrTy(), i64()});
                                    llvm::Value* l642 = builder_.CreateSExt(wr2, i64());
                                    llvm::Value* dSz3 = builder_.CreateAdd(
                                        l642, builder_.getInt64(1));
                                    llvm::Value* dc3 = builder_.CreateCall(
                                        mallocFn3, {dSz3}, "rts.d");
                                    builder_.CreateCall(memcpyFn3, {dc3, bPtr3, l642});
                                    llvm::Value* np4 = builder_.CreateGEP(
                                        i8Ty, dc3, l642);
                                    builder_.CreateStore(
                                        llvm::ConstantInt::get(i8Ty, 0), np4);
                                    uint64_t sBytes3 = module_->getDataLayout()
                                        .getTypeAllocSize(stringType());
                                    llvm::Value* so3 = builder_.CreateCall(
                                        mallocFn3,
                                        {builder_.getInt64(sBytes3)}, "rts.o");
                                    builder_.CreateStore(
                                        dc3,
                                        builder_.CreateStructGEP(stringType(), so3, 0));
                                    builder_.CreateStore(
                                        l642,
                                        builder_.CreateStructGEP(stringType(), so3, 1));
                                    builder_.CreateStore(
                                        llvm::ConstantInt::get(i8Ty, 1),
                                        builder_.CreateStructGEP(stringType(), so3, 2));
                                    initStringRc(so3);
                                    return so3;
                                }
                            }
                            return emitPrimitiveToString(recv, tt->primitive);
                        }
                    }
                }
            }
        }
        return lowerCall(*call);
    }
    if (auto* ne = dynamic_cast<const ast::NewExpr*>(&e)) {
        return lowerNew(*ne);
    }
    if (auto* ix = dynamic_cast<const ast::IndexExpr*>(&e)) {
        if (!ix->target || ix->indices.empty() || !ix->indices[0].value)
            return nullptr;
        // CG27e (§4.6): string indexer `s[i]` → char (i32 code point).
        {
            // Pre-warm: if the target has no sema type yet (lambda param with
            // inferred type), lower it first so the type hint gets injected
            // into the sema map, then check the type.
            auto* tt = ctx_.exprType(*ix->target);
            if (!tt) {
                // Probe the type without side effects — just check the hint map.
                if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(ix->target.get())) {
                    sema::Symbol* sym = ctx_.exprSymbol(*id);
                    auto hit = lambdaParamTypeHint_.find(sym);
                    if (hit != lambdaParamTypeHint_.end()) {
                        ctx_.setExprType(ix->target.get(), hit->second);
                        tt = hit->second;
                    }
                }
            }
            if (tt && tt->kind == sema::TypeRef::Kind::Primitive &&
                tt->primitive == sema::PrimitiveType::String) {
                // §3.4 / issue #474: a Range index `s[a..b]` returns the
                // SUBSTRING spanning the range — a fresh `string` — rather than
                // a single `char`. Intercept the range form before the
                // code-point path (the index is a RangeExpr, not a value).
                if (auto* range = dynamic_cast<const ast::RangeExpr*>(
                        ix->indices[0].value.get())) {
                    llvm::Value* strPtr = lowerExpr(*ix->target);
                    if (!strPtr) return nullptr;
                    return emitStringRangeSlice(strPtr, *range);
                }
                // R33: the index may be a Range *value* (a variable / call
                // result of type Range) rather than a RangeExpr literal — e.g.
                // `var r := 0..2; s[r]`. It lowers to the %bz.range struct, so
                // the single-char path below would sext the whole struct → an
                // IR-verify failure. Detect the range-typed operand and slice
                // from the struct fields.
                if (auto* idxTy = ctx_.exprType(*ix->indices[0].value)) {
                    if (isRangeType(idxTy)) {
                        llvm::Value* strPtr = lowerExpr(*ix->target);
                        llvm::Value* rv = lowerExpr(*ix->indices[0].value);
                        if (!strPtr || !rv) return nullptr;
                        return emitStringRangeSliceValue(strPtr, rv);
                    }
                }
                // §4.6: a single int index `s[i]` returns the i-th char.
                llvm::Value* strPtr = lowerExpr(*ix->target);
                llvm::Value* idx = lowerExpr(*ix->indices[0].value);
                if (!strPtr || !idx) return nullptr;
                // fix #2799 / §7.4: a §7.4-flow-narrowed `int?` arrives here
                // still wearing its `{i8 hasVal; i32 payload}` box (sema left
                // the expression's declared type as `int?` and recorded the
                // narrowing in checkAssignable — see #2614's docs). Unwrap the
                // payload before the sext; checkIndexArgNullability already
                // rejects the un-narrowed form, so this is sound. Mirror of
                // #2614's getIntArg helper (codegen half — sema half is the
                // existing checkIndexArgNullability gate, v0.236).
                idx = unwrapNarrowedNullableOperand(idx, ix->indices[0].value.get());
                llvm::Value* idx64 = idx->getType() == i64() ? idx
                    : builder_.CreateSExt(idx, i64(), "str.idx64");
                // §4.1 / §4.6 / §31.2 (R33): bounds-check the index. Without
                // this `s[1000000]` (or a negative index) read out of bounds and
                // SEGFAULTed; the spec requires IndexOutOfRangeException. Use an
                // unsigned compare so a sign-extended negative index (a huge
                // unsigned value) is rejected too — mirrors the inline-array and
                // slice paths.
                {
                    llvm::Value* slen = loadStringLen(strPtr);
                    llvm::Value* oor =
                        builder_.CreateICmpUGE(idx64, slen, "str.oor");
                    emitGuardThrow(oor, "IndexOutOfRangeException");
                }
                return loadCodePoint(strPtr, idx64);
            }
        }
        // §22.10: class/record indexer property getter.
        if (auto* idxGetVal = lowerIndexerGet(*ix))
            return idxGetVal;
        // issue #388 / stdlib §4.4-§4.9: heap-collection indexer read
        // `a[i]` / `dict[key]` against the runtime backing.
        if (auto* collGetVal = lowerCollectionIndexerGet(*ix))
            return collGetVal;
        // §15.1: inline array `a[i]` — GEP the array slot and load.
        llvm::Type* ety = nullptr;
        llvm::Value* addr = lvalueAddr(*ix->target, ety);
        if (!addr || !ety) return nullptr;
        auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(ety);
        if (!arrTy) return nullptr;
        llvm::Value* idx = lowerExpr(*ix->indices[0].value);
        if (!idx) return nullptr;
        // fix #2799 / §7.4: unwrap a §7.4-narrowed nullable index before the
        // sext (same soundness story as the string indexer fix above;
        // checkIndexArgNullability already rejects the un-narrowed form).
        idx = unwrapNarrowedNullableOperand(idx, ix->indices[0].value.get());
        // §15.1 / §31.2: bounds-check idx against the array's compile-time
        // size; out-of-range raises IndexOutOfRangeException.
        {
            uint64_t sz = arrTy->getNumElements();
            llvm::Value* idx64 = idx->getType()->isIntegerTy(64)
                ? idx : builder_.CreateSExt(idx, i64());
            llvm::Value* szV = builder_.getInt64(sz);
            // Raise if idx < 0 (i.e., (uint64)idx >= sz when sign-extended)
            // OR idx >= sz — use an unsigned comparison covering both.
            llvm::Value* oor = builder_.CreateICmpUGE(idx64, szV, "arr.oor");
            emitGuardThrow(oor, "IndexOutOfRangeException");
        }
        llvm::Value* elemAddr = builder_.CreateGEP(
            arrTy, addr, {builder_.getInt64(0), idx});
        return builder_.CreateLoad(arrTy->getElementType(), elemAddr);
    }
    // CG34 / §9.5: non-destructive record update `recordExpr with f := v, …`
    // (v0.188, #1735: no longer a class object initializer — see lowerWith).
    if (auto* we = dynamic_cast<const ast::WithExpr*>(&e)) {
        return lowerWith(*we);
    }
    // CG19: `is` / `is not` / `as` type tests (§12.7).
    if (auto* te = dynamic_cast<const ast::TypeTestExpr*>(&e)) {
        return lowerTypeTest(*te);
    }
    // CG28: `case` expression (§19).
    if (auto* ce = dynamic_cast<const ast::CaseExpr*>(&e)) {
        return lowerCaseExpr(*ce);
    }
    // §17.15 (v0.216, #2098): the binding atom `(var x := E; body)`.
    if (auto* bx = dynamic_cast<const ast::BindExpr*>(&e)) {
        return lowerBindExpr(*bx);
    }
    // §28.3 K3.4/K7.4 (v0.209, #2080): the marked ARM transfer `owned x` —
    // consume-if-selected. The value is the binding's current object; the
    // slot is NULLED on this (selected) path so the K7.4 per-path scope-end
    // disposal — the null-guarded dynamicDispose the ownership pass's
    // dischargedByArmTransfer flag registers — runs exactly once: the
    // consumed path reloads null and skips; a not-selected path disposes
    // the still-live value. The ownership pass restricted the operand to a
    // plain `var` owned class/interface LOCAL, so locals_ always has the
    // slot.
    if (auto* mt = dynamic_cast<const ast::MarkedTransferExpr*>(&e)) {
        if (auto* ls = dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(e))) {
            auto it = locals_.find(ls->decl);
            if (it != locals_.end()) {
                llvm::Type* ety = nullptr;
                llvm::Value* addr = boxedStorage(it->second, ety);
                llvm::Value* v = builder_.CreateLoad(ptrTy(), addr, "arm.take");
                builder_.CreateStore(
                    llvm::ConstantPointerNull::get(ptrTy()), addr);
                return v;
            }
        }
        return llvm::PoisonValue::get(ptrTy());  // rejected upstream
    }
    // CG60: lambda expression `(params) => body` — emitted as a
    // nested LLVM function; the lambda value is its function pointer.
    // Non-capturing only (per §A.7, captures are a later slice).
    if (auto* lam = dynamic_cast<const ast::LambdaExpr*>(&e)) {
        return lowerLambda(*lam);
    }
    // §27.7 (#738): null-conditional invocation `f?(args)`. Invoke the nullable
    // function value when non-null, else skip; the result is the function's
    // return type made NULLABLE (null when f was null). Was unimplemented
    // (NullCondCallExpr produced a wrong value).
    if (auto* nc = dynamic_cast<const ast::NullCondCallExpr*>(&e)) {
        // §5.3 (#839): T?(x) — nullable-target primitive cast. The lexer fuses
        // `?` and `(` into the two-char `?(` punctuator, so `long?(x)` is
        // parsed as NullCondCallExpr{callee=IdentifierExpr{"long"}, args=[x]}
        // rather than a CallExpr.  When the callee is a bare primitive-type
        // keyword with no symbol this is NOT a null-conditional function call
        // but a §5.3 LIFTED CAST: null stays null, a value is cast to the
        // target primitive and re-wrapped in the nullable struct.
        if (nc->callee && nc->args.size() == 1 && nc->args[0].value) {
            if (auto* cid =
                    dynamic_cast<const ast::IdentifierExpr*>(nc->callee.get())) {
                if (!ctx_.exprSymbol(*nc->callee)) {
                    using P = sema::PrimitiveType;
                    P targetPt = sema::primitiveFromName(cid->name);
                    if (targetPt != P::None && targetPt != P::String &&
                        targetPt != P::Decimal) {
                        const ast::Expr& argE = *nc->args[0].value;
                        llvm::Value* srcV = lowerExpr(argE);
                        if (!srcV) return nullptr;
                        // Determine source primitive and whether it is nullable.
                        P srcPt = P::None;
                        bool srcNullable = false;
                        if (auto* rt = ctx_.exprType(argE)) {
                            if (rt->kind == sema::TypeRef::Kind::Primitive)
                                srcPt = rt->primitive;
                            srcNullable = rt->nullable;
                        }
                        // Extract hasValue + payload from a nullable source, or
                        // treat a non-nullable source as hasValue=true, payload=src.
                        llvm::Value* hasV;
                        llvm::Value* payload;
                        if (srcNullable && srcV->getType()->isStructTy() &&
                            isNullableValueTy(ctx_.exprType(argE))) {
                            hasV    = nullablePrimHasValue(srcV);  // i1
                            payload = nullablePrimValue(srcV);     // inner scalar
                        } else {
                            hasV    = builder_.getTrue();
                            payload = srcV;
                        }
                        // Convert the payload from source primitive to target.
                        // For an identity cast (same primitive) lowerPrimCastScalar
                        // returns the payload unchanged.
                        bool srcUns = (srcPt != P::None) ? isUnsignedPrim(srcPt)
                                                         : false;
                        llvm::Value* converted =
                            lowerPrimCastScalar(targetPt, srcPt, payload, srcUns);
                        if (!converted) return nullptr;
                        // Re-wrap as {i8 hasValue, targetScalar} nullable struct.
                        // Per §5.3: null stays null (hasV=0); value is re-wrapped
                        // with the same hasV (1) from the source.
                        llvm::StructType* st = nullablePrimStructTy(targetPt);
                        llvm::Value* hv8 =
                            builder_.CreateZExt(hasV, nullablePrimHasValTy(),
                                                "nc.hv8");
                        llvm::Value* res = builder_.CreateInsertValue(
                            llvm::UndefValue::get(st), hv8, {0u});
                        return builder_.CreateInsertValue(res, converted, {1u},
                                                          "nc.cast");
                    }
                }
            }
        }
        const sema::TypeRef* fnRt =
            nc->callee ? ctx_.exprType(*nc->callee) : nullptr;
        if (!fnRt || fnRt->kind != sema::TypeRef::Kind::Function ||
            !fnRt->funcReturn)
            return nullptr;
        llvm::Value* closurePtr = lowerExpr(*nc->callee);
        if (!closurePtr || !closurePtr->getType()->isPointerTy()) return nullptr;
        llvm::Type* rTy = lowerTypeRef(fnRt->funcReturn);  // R
        bool refResult = rTy->isPointerTy();
        // fix #747 R43 / §27.7: a null-conditional call on a VOID-returning
        // function (`((int) -> ())?`) is a statement-position no-op when null
        // and discards the result otherwise — there is no "nullable void" value
        // to materialize. The general path below built a `{i8, void}` nullable
        // struct and a PHI over it, which is malformed and aborted the compiler
        // (PHINode type-mismatch assertion). Emit the guarded call with no
        // result PHI and return no value (Unit), like an ordinary void call.
        bool voidResult = rTy->isVoidTy();
        // Result type R?: ptr for a reference R, tagged {i8 hasValue; R} struct
        // for a value R (matches lowerTypeRef of the sema-set R? result).
        llvm::Type* resTy = refResult
            ? static_cast<llvm::Type*>(ptrTy())
            : static_cast<llvm::Type*>(
                  llvm::StructType::get(llvm_, {nullablePrimHasValTy(), rTy}));
        auto* entryBB = builder_.GetInsertBlock();
        auto* callBB = llvm::BasicBlock::Create(llvm_, "nccall.do", currentFn_);
        auto* endBB = llvm::BasicBlock::Create(llvm_, "nccall.end", currentFn_);
        llvm::Value* isNull = builder_.CreateICmpEQ(
            closurePtr, llvm::ConstantPointerNull::get(ptrTy()));
        builder_.CreateCondBr(isNull, endBB, callBB);

        builder_.SetInsertPoint(callBB);
        // Closure ABI { ptr fn, ptr env }: call fn(env, user args...).
        llvm::Value* fnPtr = builder_.CreateLoad(ptrTy(), closurePtr);
        llvm::Value* envSlot =
            builder_.CreateGEP(ptrTy(), closurePtr, {builder_.getInt32(1)});
        llvm::Value* envPtr = builder_.CreateLoad(ptrTy(), envSlot);
        std::vector<llvm::Value*> args;
        args.push_back(envPtr);
        for (const auto& a : nc->args) {
            if (!a.value) return nullptr;
            llvm::Value* v = lowerExpr(*a.value);
            if (!v) return nullptr;
            args.push_back(v);
        }
        std::vector<llvm::Type*> argTys;
        for (auto* v : args) argTys.push_back(v->getType());
        auto* fnTy = llvm::FunctionType::get(rTy, argTys, false);
        // A void call must not carry a value name (LLVM forbids naming a void
        // instruction); only name the result when it is a real value.
        llvm::Value* callRes = builder_.CreateCall(
            fnTy, fnPtr, args, voidResult ? "" : "nccall.ret");
        // Wrap the non-void result into R? in THIS (call) block so it dominates
        // its PHI edge; the void case has no result to wrap.
        llvm::Value* wrapped =
            voidResult ? nullptr
            : (refResult ? callRes
                         : wrapNullableValue(fnRt->funcReturn, callRes));
        auto* callEndBB = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);

        builder_.SetInsertPoint(endBB);
        if (voidResult) return nullptr;  // Unit result — no PHI.
        auto* phi = builder_.CreatePHI(resTy, 2);
        llvm::Value* nullVal = refResult
            ? static_cast<llvm::Value*>(llvm::ConstantPointerNull::get(ptrTy()))
            : nullableValueNull(fnRt->funcReturn);
        phi->addIncoming(nullVal, entryBB);
        phi->addIncoming(wrapped, callEndBB);
        return phi;
    }
    // CG41: null-conditional member access `x?.M` (§7.5). Evaluate the
    // target; if null, the result is null, otherwise read the member.
    // The result is always nullable. Reference-typed storage members
    // only — value-typed results (`int?`, …) and computed-getter /
    // method `?.` are deferred.
    if (auto* nm = dynamic_cast<const ast::NullCondMemberExpr*>(&e)) {
        if (!nm->target) return nullptr;
        // §7.5 / issue #416: `b?.V` on a (NULLABLE) RECORD. The target lowers to
        // either the tagged {i8 hasValue; <record payload>} struct (nullable,
        // #416) or the bare by-value payload (non-nullable, #3210). The
        // reference-member path below is bypassed because the receiver is a
        // value not a pointer. If the receiver is null the result is null;
        // otherwise extract the payload field and lift it to the
        // (always-nullable) result type of §7.5 — a value-typed field wraps into
        // its tagged `T?`, a reference-typed field stays a (possibly-null) ptr.
        // For a NON-NULLABLE record (§7.5, v0.254 — the receiver cannot be null,
        // so the result is always hasValue=true) the receiver's `nv` IS the
        // payload and `hasVal` is the constant i1 1; the result lift collapses
        // to the same wrap the nullable branch builds on a present value.
        if (auto* tt = ctx_.exprType(*nm->target)) {
            if (tt->kind == sema::TypeRef::Kind::Named && tt->declaration &&
                tt->declaration->kind == ast::TypeDeclKind::Record) {
                bool ncRcvNullable = tt->nullable;
                llvm::Value* nv = lowerExpr(*nm->target);
                if (!nv || !nv->getType()->isStructTy()) return nullptr;
                auto* ms = storageMemberOf(ctx_.exprSymbol(e));
                if (!ms || !ms->parent) return nullptr;
                // issue #462 / §7.5 / §14.1: for a GENERIC record (nullable or
                // not — `Box<int>?` / `Box<int>`) install the receiver's
                // concrete type args so fieldIndex_ is registered against — and
                // the field type resolves under — the SPECIALIZED record
                // struct (the same layout the nullable wrapper's payload uses).
                // recordStruct(ms->parent) re-registers fieldIndex_ for this
                // instantiation; without it the placeholder layout's
                // index/type is used. A no-op for a non-generic record.
                RecordSubstGuard ncSubst(this, tt);
                recordStruct(ms->parent);  // register specialized fieldIndex_
                auto idxIt = fieldIndex_.find(ms);
                if (idxIt == fieldIndex_.end()) return nullptr;
                // Nullable: nv is the tagged {hasValue, payload}; extract both.
                // Non-nullable (#3210): nv IS the payload; hasValue is implicitly
                // true per §7.5 (a non-nullable receiver cannot be null), so
                // every branch below consumes a constant i1 1.
                llvm::Value* hasVal;
                llvm::Value* payload;
                if (ncRcvNullable) {
                    hasVal = nullablePrimHasValue(nv);
                    payload = nullablePrimValue(nv);
                } else {
                    hasVal = llvm::ConstantInt::get(
                        builder_.getInt1Ty(), 1);
                    payload = nv;
                }
                llvm::Value* fieldVal =
                    builder_.CreateExtractValue(payload, {idxIt->second});
                // Lift the field value to the §7.5 (always-nullable) result. Sema
                // does not record a TypeRef for the `?.` node, so derive the form
                // from the field's own type: a reference-typed field is a nullable
                // ptr (null when the receiver is null); a field that is ALREADY a
                // nullable value (the tagged struct) passes through unwrapped but
                // forced null when the receiver is null; any other value-typed
                // field wraps into its tagged {i8 hasValue; field value} `T?`.
                // #462: resolve a generic field's `T` to its concrete type so the
                // nullable-value detection below classifies it correctly.
                const sema::TypeRef* fieldTy = recordMemberTypeResolved(ms);
                if (fieldVal->getType()->isPointerTy()) {
                    // Reference-typed field → null when the receiver is null,
                    // else the field pointer. (For non-nullable #3210 `hasVal`
                    // is the constant 1, so the select is dead and `fieldVal`
                    // is returned directly — the optimizer collapses it.)
                    return builder_.CreateSelect(
                        hasVal, fieldVal,
                        llvm::ConstantPointerNull::get(ptrTy()));
                }
                if (fieldTy && isNullableValueTy(fieldTy) &&
                    fieldVal->getType()->isStructTy()) {
                    // Field already nullable: AND its own hasValue with the
                    // receiver's so a null receiver yields null.
                    // (Non-nullable #3210: `hasVal` is constant 1, so the AND
                    // collapses to `innerHas`.)
                    llvm::Value* innerHas = nullablePrimHasValue(fieldVal);
                    llvm::Value* combined = builder_.CreateAnd(hasVal, innerHas);
                    return builder_.CreateInsertValue(
                        fieldVal,
                        builder_.CreateZExt(combined, nullablePrimHasValTy()),
                        {0u});
                }
                // Value-typed (non-nullable) field → tagged {hasValue, fieldVal},
                // with the receiver's run-time hasValue tag. (#3210:
                // `hasVal` is constant 1.)
                auto* nst = llvm::StructType::get(
                    llvm_, {nullablePrimHasValTy(), fieldVal->getType()});
                llvm::Value* res = builder_.CreateInsertValue(
                    llvm::UndefValue::get(nst),
                    builder_.CreateZExt(hasVal, nullablePrimHasValTy()), {0u});
                res = builder_.CreateInsertValue(res, fieldVal, {1u});
                return res;
            }
        }
        // fix #2990 (§31.4 / §12.1 / §7.5, v0.240): `q?.M` on a nullable
        // exception-typed receiver, where M is one of the four §31.4 header
        // members (Message / StackTrace / CauseChain / ParamName). These carry
        // NO storage FieldSymbol — the `.` form reads them through the
        // byte-offset header GEPs in the MemberAccessExpr arm below (the
        // `ma->member == "Message"` handler at CG22+ and the
        // `isInheritedExcSurfaceAccess` block for the other three). That arm
        // never runs for a `?.` access: a NullCondMemberExpr is a distinct AST
        // node, so the access reached this storage-field path,
        // `storageMemberOf` returned null, and the `!ms` guard at FINDING [3]
        // returned nullptr — silently dropping the whole enclosing statement
        // (no diagnostic at any severity, exit 0). MEASURED on 8eab3f8: the
        // middle `Console.WriteLine(q?.Message ?? …)` line printed nothing at
        // all for a non-null receiver carrying `Message = "zz"`.
        //
        // Route the `?.` form through the SAME header reads, gated by the SAME
        // receiver-type predicates the `.` form uses (`isBuiltinExceptionTypeRef`
        // / `hasBuiltinExceptionBase`) — the one fact sema already computes that
        // decides how the access lowers — with a null-receiver short-circuit
        // first per §7.5 (the result is always nullable, and a null receiver
        // yields null). The `exprSymbol == nullptr` gate mirrors
        // `isInheritedExcSurfaceAccess`: a subclass that DECLARES its own member
        // of the same name resolves to a Symbol and is excluded, so this never
        // shadows a declared property with a header read. (`this?.M` is not a
        // concern: `this` is non-nullable, so `?.` on it is rejected upstream.)
        //
        // All four §31.4 members are reference-typed (string / Array<Exception
        // Info>), so the §7.5 (always-nullable) result is a pointer that is null
        // when the receiver is null and the header value otherwise — the same
        // null-pointer-as-null representation FINDING [3] uses for a
        // reference-typed storage member.
        if (nm->member == "Message" || nm->member == "StackTrace" ||
            nm->member == "CauseChain" || nm->member == "ParamName") {
            const sema::TypeRef* ncRtt = ctx_.exprType(*nm->target);
            bool ncExcReceiver = false;
            if (ncRtt && ncRtt->kind == sema::TypeRef::Kind::Named &&
                ctx_.exprSymbol(e) == nullptr) {
                if (isBuiltinExceptionTypeRef(ncRtt))
                    ncExcReceiver = true;
                else if (ncRtt->declaration &&
                         hasBuiltinExceptionBase(ncRtt->declaration))
                    ncExcReceiver = true;
            }
            if (ncExcReceiver) {
                llvm::Value* excObj = lowerExpr(*nm->target);
                if (!excObj || !excObj->getType()->isPointerTy()) return nullptr;
                auto* ncEntryBB = builder_.GetInsertBlock();
                auto* ncAccessBB = llvm::BasicBlock::Create(
                    llvm_, "ncexc.access", currentFn_);
                auto* ncEndBB = llvm::BasicBlock::Create(
                    llvm_, "ncexc.end", currentFn_);
                llvm::Value* ncIsNull = builder_.CreateICmpEQ(
                    excObj, llvm::ConstantPointerNull::get(ptrTy()));
                builder_.CreateCondBr(ncIsNull, ncEndBB, ncAccessBB);
                builder_.SetInsertPoint(ncAccessBB);
                // The header GEPs mirror the MemberAccessExpr arm below
                // (classStruct inserts all four slots at the SAME byte offsets
                // in a user subclass — Message@8, CauseChain@24, ParamName@32).
                auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                llvm::Value* ncAccessVal = nullptr;
                if (nm->member == "StackTrace") {
                    // §31.4 `StackTrace: string` — "" stub (no debug info yet),
                    // matching the `.` form. The receiver was loaded for the
                    // null check above; its bytes are not read.
                    ncAccessVal = lowerStringLiteral("\"\"", false);
                } else {
                    int64_t ncOff = nm->member == "Message"    ? kExcMsgOffset
                                  : nm->member == "CauseChain" ? kExcCauseOffset
                                  : /*ParamName*/              kExcParamOffset;
                    llvm::Value* ncSlot = builder_.CreateGEP(
                        i8Ty, excObj, builder_.getInt64(ncOff));
                    llvm::Value* ncPtr =
                        builder_.CreateLoad(ptrTy(), ncSlot);
                    if (nm->member == "Message") {
                        // §31.4: null message (no `message` given) → "" per the
                        // `.` form's contract.
                        auto* ncMsgNull = builder_.CreateICmpEQ(
                            ncPtr, llvm::ConstantPointerNull::get(ptrTy()));
                        ncAccessVal = builder_.CreateSelect(
                            ncMsgNull, lowerStringLiteral("\"\"", false),
                            ncPtr);
                    } else {
                        ncAccessVal = ncPtr;
                    }
                }
                auto* ncAccessEnd = builder_.GetInsertBlock();
                builder_.CreateBr(ncEndBB);
                builder_.SetInsertPoint(ncEndBB);
                auto* ncPhi = builder_.CreatePHI(ptrTy(), 2);
                ncPhi->addIncoming(
                    llvm::ConstantPointerNull::get(ptrTy()), ncEntryBB);
                ncPhi->addIncoming(ncAccessVal, ncAccessEnd);
                return ncPhi;
            }
        }
        // fix #3015 (§7.5 / §23.2 / §23.3, v0.240): `q?.P` on a nullable class
        // receiver, where P is a NON-VIRTUAL computed property getter —
        // expression-bodied `=> expr` (§23.2) or block-form `get => do … end;`
        // (§23.3). The `.` form evaluates the getter body (computedGetterOf /
        // blockGetterOf in the MemberAccessExpr arm below); that arm never runs
        // for `?.` (a NullCondMemberExpr is a distinct AST node), so the access
        // reached FINDING [3] below, `storageMemberOf` returned null (a computed
        // property carries NO storage FieldSymbol — only an auto-property does),
        // and the `!ms` guard returned nullptr — silently dropping the whole
        // enclosing statement (no diagnostic at any severity, exit 0). MEASURED
        // on f386fda: `Console.WriteLine("p=" + (q?.Computed ?? "<null>"))` on a
        // NON-NULL receiver printed nothing at all; the `start`/`end` markers
        // printed around the gap.
        //
        // Route `?.` through the SAME getter evaluation the `.` form uses —
        // computedGetterOf / blockGetterOf are the one fact the compiler already
        // computes that decides "this is a computed getter, call the body" — with
        // a null-receiver short-circuit first per §7.5 (the always-nullable
        // result is null when the receiver is null, else the getter value lifted
        // to nullable). VIRTUAL / INTERFACE properties are GATED OUT: they
        // dispatch dynamically through propGetSlot_ / ifacePropertyGetIndex_ (the
        // `.` form's lowerVirtualPropertyGet runs first and returns before the
        // computed-getter path), and a static inline of the declaring getter
        // would miscompile an override. The virtual-property-through-`?.` case is
        // tracked separately (the vtable dispatch needs a null short-circuit
        // before the slot load, a related but distinct lowering).
        //
        // The getter evaluation (generic substitution, `this` binding, result
        // wrap/box) mirrors the qualified MemberAccessExpr getter paths verbatim;
        // the §7.5 lift mirrors FINDING [3] below for a storage member.
        if (auto* ncPropSym =
                dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(e))) {
            if (!ncPropSym->isIndexer) {
                bool ncIsVirtual = propGetSlot_.count(ncPropSym) > 0 ||
                    ifacePropertyGetIndex_.count(ncPropSym) > 0;
                const ast::Expr* ncGetter = nullptr;
                const ast::PropertyAccessor* ncBlockAcc = nullptr;
                if (!ncIsVirtual) {
                    ncGetter = computedGetterOf(ctx_.exprSymbol(e));
                    ncBlockAcc = blockGetterOf(ctx_.exprSymbol(e));
                }
                if (ncGetter || ncBlockAcc) {
                    llvm::Value* obj = lowerExpr(*nm->target);
                    if (!obj || !obj->getType()->isPointerTy()) return nullptr;
                    auto* ncEntryBB = builder_.GetInsertBlock();
                    auto* ncAccessBB = llvm::BasicBlock::Create(
                        llvm_, "ncprop.access", currentFn_);
                    auto* ncEndBB = llvm::BasicBlock::Create(
                        llvm_, "ncprop.end", currentFn_);
                    llvm::Value* ncIsNull = builder_.CreateICmpEQ(
                        obj, llvm::ConstantPointerNull::get(ptrTy()));
                    builder_.CreateCondBr(ncIsNull, ncEndBB, ncAccessBB);
                    builder_.SetInsertPoint(ncAccessBB);
                    // Evaluate the getter on the (now known non-null) receiver.
                    auto* ncPd = dynamic_cast<const ast::PropertyDecl*>(
                        ncPropSym->decl);
                    const sema::TypeRef* ncDeclTy =
                        ncPd ? ctx_.resolvedType(*ncPd->type) : nullptr;
                    llvm::Value* ncRaw = nullptr;
                    if (ncGetter) {
                        // §23.2 expression-bodied getter: inline the body with
                        // this = obj. The generic-substitution setup mirrors the
                        // `.` form's qualified-getter path so a generic class's
                        // `T`-typed field read inside the body resolves against
                        // the receiver's concrete type args.
                        auto ncSavedSubst = genericSubst_;
                        auto ncSavedSemaTy = genericSubstSemaTy_;
                        if (ncPropSym->parent) {
                            const sema::TypeRef* ncRecvTy =
                                ctx_.exprType(*nm->target);
                            // fix #3143 (§14.1 / §2.5, v0.254): parameter names
                            // through the one both-sources accessor (and the
                            // `parent->decl` pre-guard dropped with it). Built
                            // only once the receiver guards have passed.
                            if (ncRecvTy &&
                                ncRecvTy->kind == sema::TypeRef::Kind::Named &&
                                ncRecvTy->declaration == ncPropSym->parent &&
                                !ncRecvTy->genericArgs.empty()) {
                                const std::vector<std::string_view> ncGpNames =
                                    sema::genericParamNames(ncPropSym->parent);
                                if (!ncGpNames.empty() &&
                                    ncGpNames.size() ==
                                        ncRecvTy->genericArgs.size()) {
                                    std::unordered_map<std::string_view,
                                                       llvm::Type*> ncSubst;
                                    std::unordered_map<std::string_view,
                                        const sema::TypeRef*> ncSema;
                                    for (std::size_t ncGi = 0;
                                         ncGi < ncGpNames.size();
                                         ++ncGi) {
                                        ncSubst[ncGpNames[ncGi]] =
                                            lowerTypeRef(
                                                ncRecvTy->genericArgs[ncGi]);
                                        ncSema[ncGpNames[ncGi]] =
                                            resolveTypeParamRef(
                                                ncRecvTy->genericArgs[ncGi]);
                                    }
                                    bool ncHasUnresolved = false;
                                    for (auto& [n, ty] : ncSubst)
                                        if (ty == i32()) {
                                            ncHasUnresolved = true; break;
                                        }
                                    if (ncHasUnresolved) {
                                        sema::Symbol* ncRecvSym =
                                            ctx_.exprSymbol(*nm->target);
                                        const ast::Node* ncDecl = nullptr;
                                        if (auto* ncLoc =
                                                dynamic_cast<sema::LocalSymbol*>(
                                                    ncRecvSym))
                                            ncDecl = ncLoc->decl;
                                        if (ncDecl) {
                                            auto ncLit =
                                                localGenericSubst_.find(ncDecl);
                                            if (ncLit !=
                                                localGenericSubst_.end())
                                                ncSubst = ncLit->second;
                                        }
                                    }
                                    genericSubst_ = std::move(ncSubst);
                                    genericSubstSemaTy_ = std::move(ncSema);
                                }
                            }
                        }
                        llvm::Value* ncSavedThis = currentThis_;
                        currentThis_ = obj;
                        ncRaw = lowerExpr(*ncGetter);
                        // §23.2/§23.3 (#1168/#2761): wrap/box the body result to
                        // the property's DECLARED type (not the §7.5-lifted
                        // nullable result) — a `T?`-declared body yielding bare
                        // `T` wraps here, an Object-typed value-type body boxes.
                        ncRaw = wrapNullableGetterResult(ncDeclTy, ncRaw);
                        ncRaw = boxForTarget(ncRaw, ncDeclTy, ncGetter);
                        currentThis_ = ncSavedThis;
                        genericSubst_ = std::move(ncSavedSubst);
                        genericSubstSemaTy_ = std::move(ncSavedSemaTy);
                    } else {
                        // §23.3 block-form getter: call the standalone getter
                        // function (ncPd is non-null — blockGetterOf returns
                        // null for an imported property with no PropertyDecl).
                        llvm::Function* ncGetterFn =
                            ensurePropertyBlockGetter(ncPd, ncBlockAcc);
                        ncRaw = builder_.CreateCall(
                            ncGetterFn->getFunctionType(), ncGetterFn, {obj});
                        emitPendingCheck();  // §31.1/§31.2: throw propagation
                    }
                    // Lift the getter result to the §7.5 (always-nullable)
                    // result — the same logic FINDING [3] uses for a storage
                    // member: a reference result is a pointer (null when the
                    // receiver is null); a value result wraps into the tagged
                    // {i8 hasValue; T} struct; a result already nullable passes
                    // through with the receiver's edge.
                    const sema::TypeRef* ncResultTy = ctx_.exprType(e);
                    bool ncRef = ncRaw->getType()->isPointerTy();
                    bool ncNullableMem = !ncRef && ncResultTy &&
                        isNullableValueTy(ncResultTy) &&
                        ncRaw->getType() == nullableValueStructTy(ncResultTy);
                    llvm::Type* ncResTy = ncRef
                        ? static_cast<llvm::Type*>(ptrTy())
                        : (ncNullableMem
                               ? ncRaw->getType()
                               : static_cast<llvm::Type*>(llvm::StructType::get(
                                     llvm_,
                                     {nullablePrimHasValTy(), ncRaw->getType()})));
                    llvm::Value* ncAccessVal;
                    if (ncRef || ncNullableMem) {
                        ncAccessVal = ncRaw;
                    } else {
                        ncAccessVal = builder_.CreateInsertValue(
                            llvm::UndefValue::get(ncResTy),
                            llvm::ConstantInt::get(nullablePrimHasValTy(), 1),
                            {0u});
                        ncAccessVal =
                            builder_.CreateInsertValue(ncAccessVal, ncRaw, {1u});
                    }
                    auto* ncAccessEnd = builder_.GetInsertBlock();
                    builder_.CreateBr(ncEndBB);
                    builder_.SetInsertPoint(ncEndBB);
                    llvm::Value* ncNullVal = ncRef
                        ? static_cast<llvm::Value*>(
                              llvm::ConstantPointerNull::get(ptrTy()))
                        : static_cast<llvm::Value*>(
                              llvm::ConstantAggregateZero::get(ncResTy));
                    auto* ncPhi = builder_.CreatePHI(ncResTy, 2);
                    ncPhi->addIncoming(ncNullVal, ncEntryBB);
                    ncPhi->addIncoming(ncAccessVal, ncAccessEnd);
                    return ncPhi;
                }
                // fix #3209 (§7.5 / §23.2 / §2.5, v0.254): the IMPORTED,
                // non-virtual, non-interface, NON-AUTO-STORAGE computed-property
                // getter. The local computed-getter path above (the #3015 block)
                // routes through `computedGetterOf` / `blockGetterOf`, both
                // AST-only — for an imported PropertySymbol, `ps->decl` is null
                // (ModuleLoader never assigns it; there is no AST on the importer
                // side), so BOTH return null and the `if (ncGetter || ncBlockAcc)`
                // gate never fires. `ncIsVirtual` was already set false above
                // (declareImportedPropertyGetters also skips virtual / interface
                // / indexer — LlvmBackend_Setup.cpp:2776-2804 — so the imported
                // stub is only populated for the shape we are now handling).
                //
                // The `.` form has the same import fallback for the computed-
                // property path, via `importedPropGetterFns_[ps]` at ~L5296-L5405
                // (the §2.5/§23 arm added by #2504 for the bare-identifier twin
                // of this shape). Add the SAME fallback here: call the external
                // getter stub with the null-checked receiver and lift the result
                // to nullable exactly as the local block-form-getter branch
                // above does.
                //
                // Auto-storage is gated out via `isAutoPropertyStorageSymbol`
                // (the both-sources predicate — see SymbolCollection.cpp:866).
                // For an IMPORTED auto-property, `storageMemberOf` below resolves
                // the backing FieldSymbol correctly (#2478's wire-bit fallback),
                // so the lower direct-field path lifts the slot value to nullable
                // itself. Routing that case here would (a) call a getter stub
                // `bz$<C>$<P>$propget` that `emitExportedPropertyGetters`
                // deliberately does NOT emit for an auto-property (it has no
                // body to wrap), and (b) lose the direct-field read's null-lift.
                // A local auto-property, in turn, has an `ast::PropertyDecl` and
                // IS the `ncBlockAcc`/`ncGetter` case above — the AST arms reach
                // it before this one can.
                //
                // A NON-nullable generic (sema #2884 decline — `q?.P` on a
                // nullable generic receiver) is rejected at sema, so no extra
                // generic-substitution gate is needed here. A virtual / interface
                // / indexer import is filtered by declareImportedPropertyGetters
                // itself, so the find below cannot return a non-existent stub.
                else if (!ncIsVirtual && !ncGetter && !ncBlockAcc) {
                    if (!sema::isAutoPropertyStorageSymbol(ncPropSym, ctx_)) {
                        auto ncIgit = importedPropGetterFns_.find(ncPropSym);
                        if (ncIgit != importedPropGetterFns_.end()) {
                            llvm::Value* obj = lowerExpr(*nm->target);
                            if (!obj || !obj->getType()->isPointerTy())
                                return nullptr;
                            // fix #2474 (§9.2/§23.1/§23.2): a record receiver
                            // lowers to a by-value struct, not a heap pointer,
                            // but `declareImportedPropertyGetters` declares the
                            // stub unconditionally as `T get(ptr %this)`. Mirror
                            // the `.`-form sibling's spill-to-slot (MemberAccess
                            // Expr arm ~L5377-L5384) — aggregate receiver,
                            // non-Enum (the Enum exemption matches the sibling
                            // path's own gate verbatim, and an Enum's exported
                            // property getter stub is gated the same way).
                            if (!obj->getType()->isPointerTy() &&
                                obj->getType()->isAggregateType()) {
                                bool ncIEnumRcv =
                                    ncPropSym->parent &&
                                    ncPropSym->parent->kind ==
                                        ast::TypeDeclKind::Enum;
                                if (!ncIEnumRcv) {
                                    llvm::AllocaInst* tmp =
                                        entryAlloca(obj->getType());
                                    builder_.CreateStore(obj, tmp);
                                    obj = tmp;
                                }
                            }
                            if (!obj->getType()->isPointerTy()) return nullptr;
                            auto* ncIEntryBB = builder_.GetInsertBlock();
                            auto* ncIAccessBB = llvm::BasicBlock::Create(
                                llvm_, "ncpropimp.access", currentFn_);
                            auto* ncIEndBB = llvm::BasicBlock::Create(
                                llvm_, "ncpropimp.end", currentFn_);
                            llvm::Value* ncIIsNull = builder_.CreateICmpEQ(
                                obj, llvm::ConstantPointerNull::get(ptrTy()));
                            builder_.CreateCondBr(
                                ncIIsNull, ncIEndBB, ncIAccessBB);
                            builder_.SetInsertPoint(ncIAccessBB);
                            // fix #2816 / §28.3 (v0.240): the temporary's
                            // disposal must be registered at its production so
                            // the imported getter's throw edge (emitPendingCheck
                            // below) unwinds through it. Mirrors the `.`-form
                            // twin at MemberAccessExpr ~L5385-L5405.
                            llvm::AllocaInst* ncIRecvUnwind =
                                registerFreshNewReceiverTempUnwind(
                                    nm->target.get(), obj,
                                    ctx_.exprType(e),
                                    /*calleeReturnsOwned=*/false);
                            llvm::Value* ncRaw = builder_.CreateCall(
                                ncIgit->second->getFunctionType(),
                                ncIgit->second, {obj});
                            // §31.1/§31.2: a throw out of the imported getter
                            // leaves the pending-exception global set; the
                            // caller must test it and route to the enclosing
                            // unwind target. Without this, the throw is
                            // swallowed and `ncRaw` is a garbage value (the
                            // exact twin of #3015's `ncBlockAcc` branch).
                            emitPendingCheck();
                            // Lift the getter result to the §7.5 (always-nullable)
                            // result — SAME logic as the `ncBlockAcc` branch
                            // above: a reference result is a pointer (null when
                            // the receiver is null); a value result wraps into
                            // the tagged {i8 hasValue; T} struct; a result
                            // already nullable passes through with the receiver's
                            // edge.
                            const sema::TypeRef* ncIResultTy = ctx_.exprType(e);
                            bool ncIRef = ncRaw->getType()->isPointerTy();
                            bool ncINullableMem = !ncIRef && ncIResultTy &&
                                isNullableValueTy(ncIResultTy) &&
                                ncRaw->getType() ==
                                    nullableValueStructTy(ncIResultTy);
                            llvm::Type* ncIResTy = ncIRef
                                ? static_cast<llvm::Type*>(ptrTy())
                                : (ncINullableMem
                                       ? ncRaw->getType()
                                       : static_cast<llvm::Type*>(
                                             llvm::StructType::get(
                                                 llvm_,
                                                 {nullablePrimHasValTy(),
                                                  ncRaw->getType()})));
                            llvm::Value* ncIAccessVal;
                            if (ncIRef || ncINullableMem) {
                                ncIAccessVal = ncRaw;
                            } else {
                                ncIAccessVal = builder_.CreateInsertValue(
                                    llvm::UndefValue::get(ncIResTy),
                                    llvm::ConstantInt::get(
                                        nullablePrimHasValTy(), 1),
                                    {0u});
                                ncIAccessVal = builder_.CreateInsertValue(
                                    ncIAccessVal, ncRaw, {1u});
                            }
                            auto* ncIAccessEnd = builder_.GetInsertBlock();
                            builder_.CreateBr(ncIEndBB);
                            builder_.SetInsertPoint(ncIEndBB);
                            llvm::Value* ncINullVal = ncIRef
                                ? static_cast<llvm::Value*>(
                                      llvm::ConstantPointerNull::get(ptrTy()))
                                : static_cast<llvm::Value*>(
                                      llvm::ConstantAggregateZero::get(
                                          ncIResTy));
                            auto* ncIPhi = builder_.CreatePHI(ncIResTy, 2);
                            ncIPhi->addIncoming(ncINullVal, ncIEntryBB);
                            ncIPhi->addIncoming(
                                ncIAccessVal, ncIAccessEnd);
                            // fix #2751 / §28.3 (v0.240): the IMPORTED `?.`-
                            // form getter spelling — the same reclaim as the
                            // `.`-form sibling path (~L5396-L5405).
                            if (!diverged_)
                                disposeFreshNewReceiverTemp(
                                    nm->target.get(), obj, ctx_.exprType(e),
                                    /*calleeReturnsOwned=*/false,
                                    ncIRecvUnwind);
                            return ncIPhi;
                        }
                    }
                }
            }
        }
        // fix #3016 (§7.5 / §12.1 / §13 / §23, v0.240): the VIRTUAL / INTERFACE
        // case that #3015 (above) deliberately gated out — `q?.VProp` where
        // VProp is a virtual (§12.1) or interface (§13) property getter. The `.`
        // form dispatches dynamically through lowerVirtualPropertyGet (the
        // vtable/itable slot); that path never runs for `?.` (a
        // NullCondMemberExpr is a distinct AST node), so the access reached
        // FINDING [3] below, `storageMemberOf` returned null (a virtual property
        // carries NO storage FieldSymbol), and the `!ms` guard dropped the
        // statement — MEASURED on d2a3d3e:
        // `Console.WriteLine("p=" + (q?.VProp ?? "<null>"))` on a NON-NULL
        // receiver printed nothing at all (start/end around the gap), exit 0.
        //
        // Route `?.` through the SAME vtable/itable slot dispatch the `.` form
        // uses, with a §7.5 null-receiver short-circuit FIRST (loading the slot
        // from a null receiver is the crash). The slot load + call mirror
        // lowerVirtualPropertyGet's core dispatch verbatim — crucially the
        // dispatch goes through the RUNTIME type's vtable, so an OVERRIDDEN
        // getter runs (the reason #3015 gated this out: a static inline of the
        // declaring getter body would miscompile the override). The §7.5 lift
        // mirrors #3015's block above.
        if (auto* ncVirtSym =
                dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(e))) {
            if (!ncVirtSym->isIndexer) {
                bool ncVIsIface = ncVirtSym->parent &&
                    ncVirtSym->parent->kind == ast::TypeDeclKind::Interface;
                auto ncVGetIt = propGetSlot_.find(ncVirtSym);
                auto ncVIfGetIt = ifacePropertyGetIndex_.find(ncVirtSym);
                if ((!ncVIsIface && ncVGetIt != propGetSlot_.end()) ||
                    (ncVIsIface && ncVIfGetIt != ifacePropertyGetIndex_.end())) {
                    // For an interface property, verify the itable descriptor
                    // exists (mirrors lowerVirtualPropertyGet). A missing
                    // descriptor cannot dispatch — fall through to the next arm
                    // rather than crash mid-block.
                    if (!ncVIsIface || descArray_.find(ncVirtSym->parent) !=
                                           descArray_.end()) {
                        // Derive the getter's return type (mirrors
                        // lowerVirtualPropertyGet). pd is null for an IMPORTED
                        // property; the type rides importedMemberType.
                        auto* ncVpd = dynamic_cast<const ast::PropertyDecl*>(
                            ncVirtSym->decl);
                        llvm::Type* ncVValTy = nullptr;
                        if (ncVpd) {
                            ncVValTy = lowerType(ncVpd->type.get());
                        } else if (sema::TypeRef* tr =
                                       ctx_.importedMemberType(ncVirtSym)) {
                            ncVValTy = lowerTypeRef(tr);
                        }
                        if (!ncVValTy) ncVValTy = i32();
                        llvm::Value* obj = lowerExpr(*nm->target);
                        if (!obj || !obj->getType()->isPointerTy()) return nullptr;
                        auto* ncVEntryBB = builder_.GetInsertBlock();
                        auto* ncVAccessBB = llvm::BasicBlock::Create(
                            llvm_, "ncvprop.access", currentFn_);
                        auto* ncVEndBB = llvm::BasicBlock::Create(
                            llvm_, "ncvprop.end", currentFn_);
                        llvm::Value* ncVIsNull = builder_.CreateICmpEQ(
                            obj, llvm::ConstantPointerNull::get(ptrTy()));
                        builder_.CreateCondBr(ncVIsNull, ncVEndBB, ncVAccessBB);
                        builder_.SetInsertPoint(ncVAccessBB);
                        // Dispatch through the vtable/itable slot — SAME logic
                        // as lowerVirtualPropertyGet's core dispatch.
                        auto* ncVFnTy = llvm::FunctionType::get(
                            ncVValTy, {ptrTy()}, false);
                        llvm::Value* ncVFnPtr = nullptr;
                        if (ncVIsIface) {
                            // §13 itable dispatch.
                            llvm::Value* ncVVptr =
                                builder_.CreateLoad(ptrTy(), obj);
                            auto* ncVLookupFn = declareC(
                                "bz_itable_lookup", ptrTy(),
                                {ptrTy(), ptrTy()});
                            llvm::Value* ncVItmapPtr = builder_.CreateLoad(
                                ptrTy(),
                                builder_.CreateGEP(
                                    ptrTy(), ncVVptr,
                                    {builder_.getInt32(1)}));
                            llvm::Value* ncVItab = builder_.CreateCall(
                                llvm::FunctionType::get(
                                    ptrTy(), {ptrTy(), ptrTy()}, false),
                                ncVLookupFn,
                                {ncVItmapPtr,
                                 descArray_.find(ncVirtSym->parent)->second},
                                "ncvitab");
                            ncVFnPtr = builder_.CreateLoad(
                                ptrTy(),
                                builder_.CreateGEP(
                                    ptrTy(), ncVItab,
                                    {builder_.getInt32(ncVIfGetIt->second)}),
                                "ncvpget");
                        } else {
                            // §12.1 class virtual dispatch: vt[slot].
                            llvm::Value* ncVVptr =
                                builder_.CreateLoad(ptrTy(), obj);
                            llvm::Value* ncVSlotPtr = builder_.CreateGEP(
                                ptrTy(), ncVVptr,
                                {builder_.getInt32(ncVGetIt->second)});
                            ncVFnPtr = builder_.CreateLoad(ptrTy(), ncVSlotPtr);
                        }
                        llvm::Value* ncVRaw =
                            builder_.CreateCall(ncVFnTy, ncVFnPtr, {obj});
                        emitPendingCheck();  // §31.1/§31.2: throw propagation
                        // Lift the getter result to the §7.5 (always-nullable)
                        // result — SAME logic as #3015's block above and FINDING
                        // [3] below: a reference result is a pointer (null when
                        // the receiver is null); a value result wraps into the
                        // tagged {i8 hasValue; T} struct; a result already
                        // nullable passes through with the receiver's edge.
                        const sema::TypeRef* ncVResultTy = ctx_.exprType(e);
                        bool ncVRef = ncVRaw->getType()->isPointerTy();
                        bool ncVNullableMem = !ncVRef && ncVResultTy &&
                            isNullableValueTy(ncVResultTy) &&
                            ncVRaw->getType() ==
                                nullableValueStructTy(ncVResultTy);
                        llvm::Type* ncVResTy = ncVRef
                            ? static_cast<llvm::Type*>(ptrTy())
                            : (ncVNullableMem
                                   ? ncVRaw->getType()
                                   : static_cast<llvm::Type*>(
                                         llvm::StructType::get(llvm_,
                                             {nullablePrimHasValTy(),
                                              ncVRaw->getType()})));
                        llvm::Value* ncVAccessVal;
                        if (ncVRef || ncVNullableMem) {
                            ncVAccessVal = ncVRaw;
                        } else {
                            ncVAccessVal = builder_.CreateInsertValue(
                                llvm::UndefValue::get(ncVResTy),
                                llvm::ConstantInt::get(nullablePrimHasValTy(), 1),
                                {0u});
                            ncVAccessVal = builder_.CreateInsertValue(
                                ncVAccessVal, ncVRaw, {1u});
                        }
                        auto* ncVAccessEnd = builder_.GetInsertBlock();
                        builder_.CreateBr(ncVEndBB);
                        builder_.SetInsertPoint(ncVEndBB);
                        llvm::Value* ncVNullVal = ncVRef
                            ? static_cast<llvm::Value*>(
                                  llvm::ConstantPointerNull::get(ptrTy()))
                            : static_cast<llvm::Value*>(
                                  llvm::ConstantAggregateZero::get(ncVResTy));
                        auto* ncVPhi = builder_.CreatePHI(ncVResTy, 2);
                        ncVPhi->addIncoming(ncVNullVal, ncVEntryBB);
                        ncVPhi->addIncoming(ncVAccessVal, ncVAccessEnd);
                        return ncVPhi;
                    }
                }
            }
        }
        // §7.5 / FINDING [3]: the receiver is a class/string reference, so a null
        // receiver is the null pointer. Resolve the accessed member to either a
        // storage field (class receiver) or the `Length` intrinsic (string
        // receiver). The MEMBER may be value-typed (`int`, `bool`, an enum, a
        // record) — its §7.5 (always-nullable) result is the tagged
        // {i8 hasValue; T value} struct, with hasValue=false when the receiver is
        // null. Reference-typed members keep the null-pointer-as-null
        // representation (the prior path).
        llvm::Value* obj = lowerExpr(*nm->target);
        if (!obj || !obj->getType()->isPointerTy()) return nullptr;
        // Determine the accessed member's LLVM type without yet emitting the
        // access: a storage field's element type, or i32 for `string.Length`.
        auto* targetTy = ctx_.exprType(*nm->target);
        bool stringLength = nm->member == "Length" && targetTy &&
            targetTy->kind == sema::TypeRef::Kind::Primitive &&
            targetTy->primitive == sema::PrimitiveType::String;
        auto* ms = storageMemberOf(ctx_.exprSymbol(e));
        llvm::StructType* st = nullptr;
        llvm::Type* memTy = nullptr;
        unsigned memIdx = 0;
        if (stringLength) {
            memTy = i32();  // stdlib §9.6: Length: int
        } else {
            if (!ms || !ms->parent) return nullptr;
            auto idxIt = fieldIndex_.find(ms);
            // fix #3218 (§7.5 / §2.5, v0.254): a `?.` on a CLASS-TYPED FIELD of
            // an IMPORTED receiver silently dropped the enclosing statement
            // (exit 0, `internal: … #3079`), masked whenever an earlier `.`-form
            // access to the same field had already forced `classStruct` to run.
            // `fieldIndex_` is populated lazily by `classStruct`'s FIRST call for
            // `ms->parent`; `declareVtables` runs it up-front for every LOCAL
            // class but never for an imported one, so an imported class's struct
            // is built only when the FIRST access reaches this lookup. The `.`
            // spelling (`fieldAddr`, line ~5799) already retries `classStruct`
            // on a missing entry, exactly the #2474 discipline; this `?.` arm
            // did not. Apply the same retry here so `q?.F` on an imported
            // `class.F` produces the same `?.` lowering as the local twin.
            if (idxIt == fieldIndex_.end()) {
                classStruct(ms->parent);
                idxIt = fieldIndex_.find(ms);
            }
            if (idxIt == fieldIndex_.end()) return nullptr;
            st = classStruct(ms->parent);
            memIdx = idxIt->second;
            memTy = st->getElementType(memIdx);
        }
        // The §7.5 result form the member lifts to. A reference-typed member is a
        // (possibly-null) pointer; a value-typed member is the tagged
        // {i8 hasValue; T value} struct (StructType::get is uniqued, so building
        // it from the member's LLVM type matches the consumer's slot type). The
        // `?.` TypeRef recorded by sema (the nullable member type) tells us when
        // the member is ITSELF already nullable.
        const sema::TypeRef* resultTy = ctx_.exprType(e);
        bool refMember = memTy->isPointerTy();
        // A member that is ITSELF nullable (`int?` field) already loads as the
        // tagged {i8 hasValue; T} struct; its own LLVM type equals the result's
        // nullable struct. A NON-nullable value member (`int`, a record) loads as
        // a bare value whose LLVM type is strictly the payload, so it must be
        // wrapped. Distinguish by comparing the member's loaded type against the
        // result's tagged struct — NOT by `resultTy` alone (the §7.5-lifted
        // result is nullable for both, so it cannot tell them apart).
        bool nullableMember = !refMember && resultTy &&
            isNullableValueTy(resultTy) &&
            memTy == nullableValueStructTy(resultTy);
        llvm::Type* resTy = refMember
            ? static_cast<llvm::Type*>(ptrTy())
            : (nullableMember
                   ? memTy  // member already a {i8 hasValue; T} struct
                   : static_cast<llvm::Type*>(llvm::StructType::get(
                         llvm_, {nullablePrimHasValTy(), memTy})));
        auto* entryBB = builder_.GetInsertBlock();
        auto* accessBB =
            llvm::BasicBlock::Create(llvm_, "nullcond.access", currentFn_);
        auto* endBB =
            llvm::BasicBlock::Create(llvm_, "nullcond.end", currentFn_);
        llvm::Value* isNull = builder_.CreateICmpEQ(
            obj, llvm::ConstantPointerNull::get(ptrTy()));
        builder_.CreateCondBr(isNull, endBB, accessBB);
        builder_.SetInsertPoint(accessBB);
        llvm::Value* val;
        if (stringLength) {
            llvm::Value* len64 = loadStringLen(obj);
            val = builder_.CreateTrunc(len64, i32(), "str.len.i32");
        } else {
            llvm::Value* addr = builder_.CreateStructGEP(st, obj, memIdx);
            val = builder_.CreateLoad(memTy, addr);
        }
        // Lift the accessed value to the §7.5 (always-nullable) result. A
        // reference member's pointer is its own null sentinel; a member that is
        // itself an `int?` passes its own hasValue through (the receiver is
        // non-null on this edge); any other value member wraps into {i8 1, val}.
        llvm::Value* accessVal;
        if (refMember || nullableMember) {
            accessVal = val;
        } else {
            accessVal = builder_.CreateInsertValue(
                llvm::UndefValue::get(resTy),
                llvm::ConstantInt::get(nullablePrimHasValTy(), 1), {0u});
            accessVal = builder_.CreateInsertValue(accessVal, val, {1u});
        }
        auto* accessEnd = builder_.GetInsertBlock();
        builder_.CreateBr(endBB);
        builder_.SetInsertPoint(endBB);
        // The null-receiver incoming value: a null pointer for reference members,
        // or the all-zero (hasValue=false) tagged struct for value members.
        llvm::Value* nullVal = refMember
            ? static_cast<llvm::Value*>(
                  llvm::ConstantPointerNull::get(ptrTy()))
            : static_cast<llvm::Value*>(
                  llvm::ConstantAggregateZero::get(resTy));
        auto* phi = builder_.CreatePHI(resTy, 2);
        phi->addIncoming(nullVal, entryBB);
        phi->addIncoming(accessVal, accessEnd);
        return phi;
    }
    // §3.4: static properties on primitive type names (`int.MaxValue`,
    // `double.NaN`, etc.) — the target is a primitive-name identifier.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) {
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                    ma->target.get())) {
                sema::PrimitiveType pt = sema::primitiveFromName(id->name);
                if (pt != sema::PrimitiveType::None) {
                    const auto& mbr = ma->member;
                    using P = sema::PrimitiveType;
                    if (mbr == "MaxValue") {
                        switch (pt) {
                            case P::Int:  return llvm::ConstantInt::get(i32(), 0x7FFFFFFF, true);
                            case P::UInt: return llvm::ConstantInt::get(i32(), 0xFFFFFFFFu, false);
                            case P::Long: return llvm::ConstantInt::get(i64(), 0x7FFFFFFFFFFFFFFFll, true);
                            case P::ULong:return llvm::ConstantInt::get(i64(), 0xFFFFFFFFFFFFFFFFull, false);
                            case P::Short:return llvm::ConstantInt::get(llvm::Type::getInt16Ty(llvm_), 32767, true);
                            case P::UShort:return llvm::ConstantInt::get(llvm::Type::getInt16Ty(llvm_), 65535, false);
                            case P::Byte: return llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 255, false);
                            case P::SByte:return llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 127, true);
                            case P::Double:return llvm::ConstantFP::get(llvm::Type::getDoubleTy(llvm_), std::numeric_limits<double>::max());
                            case P::Float: return llvm::ConstantFP::get(llvm::Type::getFloatTy(llvm_), std::numeric_limits<float>::max());
                            // stdlib §char: MaxValue = U+10FFFF (char is a 32-bit
                            // code point, lowered to i32). #747 R42: was missing,
                            // so the read hit an uninitialized alloca.
                            case P::Char: return llvm::ConstantInt::get(i32(), 1114111, false);
                            default: break;
                        }
                    }
                    if (mbr == "MinValue") {
                        switch (pt) {
                            case P::Int:  return llvm::ConstantInt::get(i32(), int32_t(-2147483648ll), true);
                            case P::Long: return llvm::ConstantInt::get(i64(), int64_t(-9223372036854775808ll), true);
                            case P::Short:return llvm::ConstantInt::get(llvm::Type::getInt16Ty(llvm_), int16_t(-32768), true);
                            case P::SByte:return llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), int8_t(-128), true);
                            // stdlib §char: MinValue = U+0000 (0). #747 R42.
                            case P::UInt: case P::ULong: case P::UShort: case P::Byte: case P::Char:
                                return llvm::ConstantInt::get(lowerPrimitive(pt), 0, false);
                            case P::Double: return llvm::ConstantFP::get(llvm::Type::getDoubleTy(llvm_), std::numeric_limits<double>::lowest());
                            case P::Float:  return llvm::ConstantFP::get(llvm::Type::getFloatTy(llvm_), std::numeric_limits<float>::lowest());
                            default: break;
                        }
                    }
                    if (mbr == "NaN" && (pt == P::Double || pt == P::Float)) {
                        llvm::Type* ft = pt == P::Double ? llvm::Type::getDoubleTy(llvm_) : llvm::Type::getFloatTy(llvm_);
                        return llvm::ConstantFP::getNaN(ft);
                    }
                    if (mbr == "PositiveInfinity" && (pt == P::Double || pt == P::Float)) {
                        llvm::Type* ft = pt == P::Double ? llvm::Type::getDoubleTy(llvm_) : llvm::Type::getFloatTy(llvm_);
                        return llvm::ConstantFP::getInfinity(ft, false);
                    }
                    if (mbr == "NegativeInfinity" && (pt == P::Double || pt == P::Float)) {
                        llvm::Type* ft = pt == P::Double ? llvm::Type::getDoubleTy(llvm_) : llvm::Type::getFloatTy(llvm_);
                        return llvm::ConstantFP::getInfinity(ft, true);
                    }
                    if (mbr == "Epsilon" && (pt == P::Double || pt == P::Float)) {
                        // Smallest positive value: 5e-324 for double, ~1.4e-45 for float.
                        return pt == P::Double
                            ? llvm::ConstantFP::get(llvm::Type::getDoubleTy(llvm_),
                                std::numeric_limits<double>::denorm_min())
                            : llvm::ConstantFP::get(llvm::Type::getFloatTy(llvm_),
                                std::numeric_limits<float>::denorm_min());
                    }
                }
                // §2.3: built-in class constant properties (Math.PI, Math.E, etc.)
                // These are MemberAccess on a CLASS name (not a primitive type).
                if (pt == sema::PrimitiveType::None) {
                    // Not a primitive keyword — check for built-in class names.
                }
            } else {
                // Not a primitive keyword identifier — check for built-in class names.
            }
        }
    }
    // §2.3: Math.PI / Math.E + Environment.NewLine / TickCount etc.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) {
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get())) {
                auto* dbl = llvm::Type::getDoubleTy(llvm_);
                if (id->name == "Math") {
                    // §2.2 (v0.285): the spec constants are `Pi` and `Tau`.
                    // fix #3559: `E` was removed with the transcendental block
                    // at v0.284 and the legacy `PI` spelling with it (G1,
                    // issue 3411); sema rejects both before codegen runs.
                    if (ma->member == "Pi")
                        return llvm::ConstantFP::get(dbl, 3.14159265358979323846);
                    if (ma->member == "Tau") return llvm::ConstantFP::get(dbl, 6.28318530717958647692);
                }
                if (id->name == "Environment") {
                    // §2.3: Environment static properties.
                    if (ma->member == "NewLine") {
                        // Platform newline: "\n" on Unix, "\r\n" on Windows.
                        // Emit a global string constant for "\n" (Unix default).
                        return lowerStringLiteral("\"\\n\"", false);
                    }
                    if (ma->member == "TickCount") {
                        // Milliseconds since process start (via clock_gettime).
                        auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                        auto* clockFn = module_->getFunction("clock");
                        if (!clockFn) {
                            auto* ft = llvm::FunctionType::get(i64(), {}, false);
                            clockFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "clock", module_.get());
                        }
                        llvm::Value* clk = builder_.CreateCall(clockFn, {}, "clock.ret");
                        // CLOCKS_PER_SEC ≈ 1000000 on Linux; scale to ms.
                        llvm::Value* ms = builder_.CreateSDiv(
                            clk, builder_.getInt64(1000), "tick.ms");
                        return builder_.CreateTrunc(ms, i32(), "tick.i32");
                    }
                    if (ma->member == "OSVersion") {
                        return lowerStringLiteral("\"Linux\"", false);
                    }
                    if (ma->member == "MachineName") {
                        return lowerStringLiteral("\"localhost\"", false);
                    }
                    if (ma->member == "ProcessId") {
                        auto* getpidFn = module_->getFunction("getpid");
                        if (!getpidFn) {
                            auto* ft = llvm::FunctionType::get(i32(), {}, false);
                            getpidFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "getpid", module_.get());
                        }
                        return builder_.CreateCall(getpidFn, {}, "pid.ret");
                    }
                    if (ma->member == "ProcessorCount") {
                        // nproc via sysconf(_SC_NPROCESSORS_ONLN) = sysconf(84)
                        auto* scFn = module_->getFunction("sysconf");
                        if (!scFn) {
                            auto* ft = llvm::FunctionType::get(i64(), {i32()}, false);
                            scFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "sysconf", module_.get());
                        }
                        llvm::Value* n = builder_.CreateCall(scFn, {builder_.getInt32(84)});
                        return builder_.CreateTrunc(n, i32(), "nproc");
                    }
                    if (ma->member == "CurrentDirectory") {
                        // getcwd(NULL, 0) on Linux allocates a buffer; wrap as bz.string
                        auto* getcwdFn = module_->getFunction("getcwd");
                        if (!getcwdFn) {
                            auto* ft = llvm::FunctionType::get(ptrTy(), {ptrTy(), i64()}, false);
                            getcwdFn = llvm::Function::Create(ft, llvm::Function::ExternalLinkage, "getcwd", module_.get());
                        }
                        auto* strlenFn = module_->getFunction("strlen");
                        if (!strlenFn) {
                            auto* ft2 = llvm::FunctionType::get(i64(), {ptrTy()}, false);
                            strlenFn = llvm::Function::Create(ft2, llvm::Function::ExternalLinkage, "strlen", module_.get());
                        }
                        llvm::Value* raw = builder_.CreateCall(getcwdFn, {
                            llvm::ConstantPointerNull::get(ptrTy()), builder_.getInt64(0)});
                        llvm::Value* len = builder_.CreateCall(strlenFn, {raw});
                        // Build bz.string from malloc'd C string (raw, len)
                        auto* callocFn2 = declareC("calloc", ptrTy(), {i64(), i64()});
                        uint64_t strSz = module_->getDataLayout().getTypeAllocSize(stringType());
                        llvm::Value* strObj = builder_.CreateCall(callocFn2,
                            {builder_.getInt64(1), builder_.getInt64(strSz)});
                        builder_.CreateStore(raw, builder_.CreateStructGEP(stringType(), strObj, 0));
                        builder_.CreateStore(len, builder_.CreateStructGEP(stringType(), strObj, 1));
                        builder_.CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 1),
                            builder_.CreateStructGEP(stringType(), strObj, 2));
                            initStringRc(strObj);  // #422: producer's reference, rc := 1
                        return strObj;
                    }
                }
                // §2.5: DateTime static properties accessed as property reads
                // (without parentheses): DateTime.Now, DateTime.UtcNow, etc.
                // Also handles TimeSpan.Zero and TimeSpan.FromX() zero args.
                if (id->name == "DateTime") {
                    const auto& mbr = ma->member;
                    if (mbr == "Now") {
                        auto* fn = declareC("bz_datetime_now_ticks", i64(), {});
                        return builder_.CreateCall(fn, {}, "dt.now");
                    }
                    if (mbr == "UtcNow") {
                        auto* fn = declareC("bz_datetime_utcnow_ticks", i64(), {});
                        return builder_.CreateCall(fn, {}, "dt.utcnow");
                    }
                    if (mbr == "Today") {
                        auto* fn = declareC("bz_datetime_now_ticks", i64(), {});
                        llvm::Value* t = builder_.CreateCall(fn, {}, "dt.today.ticks");
                        llvm::Value* tpd = builder_.getInt64(864000000000LL);
                        llvm::Value* d = builder_.CreateSDiv(t, tpd);
                        return builder_.CreateMul(d, tpd, "dt.today");
                    }
                    if (mbr == "MinValue")
                        return builder_.getInt64(0LL);
                    if (mbr == "MaxValue")
                        return builder_.getInt64(3155378975999999999LL);
                }
                if (id->name == "TimeSpan") {
                    const auto& mbr = ma->member;
                    if (mbr == "Zero")
                        return builder_.getInt64(0LL);
                    // TimeSpan.FromX static factories are handled in
                    // tryLowerBuiltinClassCall (CallExpr path); but if accessed
                    // without () (not valid Bezel for factories), return nullptr.
                }
                // §32.7 / stdlib §10 (#1512): IntPtr.Zero / UIntPtr.Zero — the
                // null pointer (integer 0). IntPtr/UIntPtr lower to a
                // pointer-sized integer (i64 host), so the null value is i64 0.
                if ((id->name == "IntPtr" || id->name == "UIntPtr") &&
                    ma->member == "Zero")
                    return builder_.getInt64(0LL);
                if (id->name == "int") {
                    if (ma->member == "MaxValue")
                        return llvm::ConstantInt::getSigned(i32(), 2147483647);
                    if (ma->member == "MinValue")
                        return llvm::ConstantInt::getSigned(i32(), -2147483648LL);
                }
                if (id->name == "long") {
                    if (ma->member == "MaxValue")
                        return llvm::ConstantInt::getSigned(i64(), 9223372036854775807LL);
                    if (ma->member == "MinValue")
                        return llvm::ConstantInt::getSigned(i64(), (int64_t)(-9223372036854775807LL - 1));
                }
                if (id->name == "double") {
                    if (ma->member == "MaxValue")
                        return llvm::ConstantFP::get(dbl, 1.7976931348623157e308);
                    if (ma->member == "MinValue")
                        return llvm::ConstantFP::get(dbl, 2.2250738585072014e-308);
                    if (ma->member == "Epsilon")
                        return llvm::ConstantFP::get(dbl, 5e-324);
                    if (ma->member == "PositiveInfinity")
                        return llvm::ConstantFP::getInfinity(dbl, false);
                    if (ma->member == "NegativeInfinity")
                        return llvm::ConstantFP::getInfinity(dbl, true);
                    if (ma->member == "NaN")
                        return llvm::ConstantFP::getNaN(dbl);
                }
            }
        }
    }
    // CG27a (§4.6): intrinsic string member access (Length, etc.).
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) {
            auto* tt = ctx_.exprType(*ma->target);
            if (tt && tt->kind == sema::TypeRef::Kind::Primitive &&
                tt->primitive == sema::PrimitiveType::String) {
                // fix #4243 (§26.1/§4.6, v0.298): `Length`/`IsAscii` are the
                // ONLY two true string intrinsics reachable through a bare
                // (no-parens) MemberAccessExpr — every other string METHOD
                // (Trim, Substring, ...) is called with parens and goes
                // through lowerCall instead, never this arm. The comment
                // below this block ("Other string members deferred — sema
                // would have flagged any unknown member already") predates
                // fix #702 (ExprTypecheck_Calls.cpp, "before reporting 'not
                // yet supported', try a user-defined extension method on
                // string"): sema's CG27a twin now ALSO accepts a bare
                // access whose name resolves to a user `extension string`
                // COMPUTED PROPERTY (§26.1's own worked `IsBlank` example),
                // via the identical no-parens syntax. Unconditionally
                // intercepting every non-Length/IsAscii name here — as the
                // old code did — swallowed that access before it ever
                // reached the generic computed/block-getter dispatch
                // (CG33b, further down this function) that every OTHER
                // extension-property receiver (int/bool/record/class) already
                // goes through, returning a bare `nullptr` that degraded the
                // whole read to the #2491 zero/null backstop with only a
                // Warning — silently wrong, not rejected. Gate the intercept
                // to the two real intrinsics; anything else that is a real
                // extension property (computedGetterOf/blockGetterOf resolve
                // it — the same predicate the existing degenerate-receiver
                // `Length` fallback arm a few lines below already checks)
                // falls through this whole `if` unevaluated, so CG33b lowers
                // it exactly like any other receiver kind. `ma->target` is
                // deliberately NOT lowered on that path here — CG33b lowers
                // it itself, and lowering it twice would double any receiver
                // side effect / +1 string-producer refcount.
                if (ma->member != "Length" && ma->member != "IsAscii") {
                    if (!computedGetterOf(ctx_.exprSymbol(e)) &&
                        !blockGetterOf(ctx_.exprSymbol(e)))
                        return nullptr;
                    // fall through — handled by CG33b below.
                } else {
                    llvm::Value* strPtr = lowerExpr(*ma->target);
                    if (!strPtr) return nullptr;
                    if (ma->member == "Length") {
                        // stdlib §9.6: Length: int — number of char code points.
                        llvm::Value* len64 = loadStringLen(strPtr);
                        // #429 (OPERATOR-OPERAND category) / §4.6 / §28.9: if the
                        // receiver is a +1 string producer (concat, interpolation,
                        // primitive-ToString, user-method-return, range-slice), it has
                        // no named-local owner and no scope-end release — release it
                        // here, AFTER loading the length, so the string lives long
                        // enough for the length-field read.  bz_string_release on a
                        // sentinel is a no-op (safe for literal receivers).
                        if (isPlusOneStringProducer(ma->target.get()))
                            emitStringRelease(strPtr);
                        return builder_.CreateTrunc(len64, i32(), "str.len.i32");
                    }
                    // stdlib §9.6 (#741): IsAscii — true iff every code point < 128.
                    // No codegen case existed, so the read returned an uninitialized
                    // i1 slot (always false). Scan via bz_string_is_ascii.
                    if (ma->member == "IsAscii") {
                        auto* fn = declareC("bz_string_is_ascii", i32(), {ptrTy()});
                        llvm::Value* r = builder_.CreateCall(fn, {strPtr}, "str.ascii");
                        if (isPlusOneStringProducer(ma->target.get()))
                            emitStringRelease(strPtr);
                        return builder_.CreateICmpNE(r, builder_.getInt32(0),
                                                     "str.isascii");
                    }
                }
            }
            // FIX #2933: positive list. Return true ONLY when the receiver
            // gives us NO information to rule a `%bz.string` layout out
            // (`!tt`, or a degenerate TypeRef with no astNode/name) — every
            // named receiver routes through the dedicated arms `lowerExpr`
            // runs AFTER this predicate: StringBuilder (§3), FileStream
            // (§7.2), FFI buffers via isFfiBufferTypeRef (§10), OwningBuffer<T>
            // (§4.14), collections (§4.14), LinkedList/Dictionary (§4.17), span/
            // ReadOnlyStringSpan (§3.4). The explicit exclusions the previous
            // NEGATIVE list carried (StringBuilder, Buffer, FileStream,
            // isCollectionTypeName, isFfiBufferBuiltinName) are now REDUNDANT
            // — each dedicated arm's predicate stands on its own — and any
            // future reserved builtin added to isReservedBuiltinTypeName
            // without a corresponding `.Length` arm will simply NOT be claimed
            // here, which is the loud-fail shape the standing 2026-08-06
            // category-defect taxonomy mandates. // per §4.6 (v0.240)
            auto isStringFallbackTarget = [&]() -> bool {
                if (!tt) return true;  // unknown type — nothing safer to do
                if (!tt->astNode || !tt->astNode->name ||
                    tt->astNode->name->parts.empty())
                    return true;       // degenerate TypeRef; no name to consult
                return false;
            };
            if (ma->member == "Length" && isStringFallbackTarget() &&
                (!tt ||
                 (tt->kind == sema::TypeRef::Kind::Named &&
                  !tt->declaration))) {
                // Only intercept if it's not a computed property on the target.
                if (!computedGetterOf(ctx_.exprSymbol(e)) &&
                    !blockGetterOf(ctx_.exprSymbol(e))) {
                    llvm::Value* strPtr = lowerExpr(*ma->target);
                    if (strPtr && strPtr->getType()->isPointerTy()) {
                        // If the value is a bz_string struct pointer, load Length.
                        llvm::Value* len64 = loadStringLen(strPtr);
                        return builder_.CreateTrunc(len64, i32(), "str.len.i32");
                    }
                }
            }
        }
    }
    // §6.1 / §30.8: CancellationToken and CancellationTokenSource property reads
    // (accessed WITHOUT call parentheses — `tok.IsCancellationRequested` etc.).
    // Mirrors the pattern used for string.Length above: recognise the builtin
    // type name from the receiver's exprType astNode name part.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) {
            auto* tt = ctx_.exprType(*ma->target);
            std::string_view cn = builtinTypeName(tt);
            // CancellationToken property reads.
            if (cn == "CancellationToken" &&
                ma->member == "IsCancellationRequested") {
                llvm::Value* tok = lowerExpr(*ma->target);
                if (!tok) return builder_.getFalse();
                auto* fn = declareC("bz_ctoken_is_cancelled", i32(), {ptrTy()});
                llvm::Value* flag = builder_.CreateCall(fn, {tok}, "ct.iscancelled");
                return builder_.CreateICmpNE(flag, builder_.getInt32(0), "ct.icr.b");
            }
            // §8.1 / issue #646: Stopwatch instance property reads.
            // Receiver is a bz_stopwatch_t* (heap pointer).
            if (cn == "Stopwatch") {
                llvm::Value* sw = lowerExpr(*ma->target);
                if (!sw) return nullptr;
                if (ma->member == "IsRunning") {
                    auto* fn = declareC("bz_stopwatch_is_running", i32(), {ptrTy()});
                    llvm::Value* flag = builder_.CreateCall(fn, {sw}, "sw.running");
                    return builder_.CreateICmpNE(flag, builder_.getInt32(0), "sw.running.b");
                }
                if (ma->member == "ElapsedMilliseconds") {
                    auto* fn = declareC("bz_stopwatch_elapsed_ms", i64(), {ptrTy()});
                    return builder_.CreateCall(fn, {sw}, "sw.elms");
                }
                if (ma->member == "Elapsed") {
                    // Returns TimeSpan (i64, 100-ns ticks).
                    auto* fn = declareC("bz_stopwatch_elapsed_ticks", i64(), {ptrTy()});
                    return builder_.CreateCall(fn, {sw}, "sw.el");
                }
            }
            // stdlib §7.1/§7.2 / issue #645: FileStream instance property reads.
            // Receiver is a bz_filestream_t* (heap pointer).
            if (cn == "FileStream") {
                llvm::Value* fs = lowerExpr(*ma->target);
                if (!fs) return nullptr;
                // §7.2: Path → a FRESH +1 bz.string (the same producer shape as
                // File.ReadAllText), so the reader owns the reference it gets.
                if (ma->member == "Path") {
                    auto* fn = declareC("bz_filestream_path", ptrTy(), {ptrTy()});
                    return builder_.CreateCall(fn, {fs}, "fs.path");
                }
                if (ma->member == "Length") {
                    auto* fn = declareC("bz_filestream_length", i64(), {ptrTy()});
                    return builder_.CreateCall(fn, {fs}, "fs.len");
                }
                if (ma->member == "Position") {
                    auto* fn = declareC("bz_filestream_position", i64(), {ptrTy()});
                    return builder_.CreateCall(fn, {fs}, "fs.pos");
                }
                if (ma->member == "CanRead" || ma->member == "CanWrite" ||
                    ma->member == "CanSeek") {
                    const char* rt = ma->member == "CanRead"  ? "bz_filestream_can_read"
                                   : ma->member == "CanWrite" ? "bz_filestream_can_write"
                                                              : "bz_filestream_can_seek";
                    auto* fn = declareC(rt, i32(), {ptrTy()});
                    llvm::Value* flag = builder_.CreateCall(fn, {fs}, "fs.can");
                    return builder_.CreateICmpNE(flag, builder_.getInt32(0), "fs.can.b");
                }
            }
            // §6.4 / issue #647: Atomic<T> and AtomicInteger<T> — Value getter.
            // `atom.Value` → bz_atomic_load(atom, cell_size).
            // T is in genericArgs[0] of the receiver's TypeRef.
            if ((cn == "Atomic" || cn == "AtomicInteger") &&
                ma->member == "Value") {
                llvm::Value* atom = lowerExpr(*ma->target);
                if (!atom) return nullptr;
                // Determine cell_size from T.
                int cellSize = 4;
                if (tt && !tt->genericArgs.empty() && tt->genericArgs[0]) {
                    const sema::TypeRef* tArg = tt->genericArgs[0];
                    if (tArg->kind == sema::TypeRef::Kind::Primitive) {
                        using P = sema::PrimitiveType;
                        switch (tArg->primitive) {
                            case P::Bool: case P::Byte: case P::SByte:
                                cellSize = 1; break;
                            case P::Short: case P::UShort: cellSize = 2; break;
                            case P::Long: case P::ULong:   cellSize = 8; break;
                            default: cellSize = 4; break;
                        }
                    } else {
                        cellSize = 8;  // IntPtr/UIntPtr
                    }
                }
                auto* loadFn = declareC("bz_atomic_load", i64(), {ptrTy(), i32()});
                llvm::Value* raw = builder_.CreateCall(
                    loadFn, {atom, builder_.getInt32(cellSize)}, "atom.load");
                // Truncate to the declared T width.
                llvm::Type* tTy = i32();
                if (tt && !tt->genericArgs.empty() && tt->genericArgs[0])
                    tTy = lowerTypeRef(tt->genericArgs[0]);
                if (!tTy) tTy = i32();
                if (tTy->isIntegerTy() && !tTy->isIntegerTy(64))
                    return builder_.CreateTrunc(raw, tTy, "atom.val");
                return raw;  // i64 or bool (i1 from trunc handled above)
            }
            // CancellationTokenSource property reads.
            if (cn == "CancellationTokenSource") {
                llvm::Value* cts = lowerExpr(*ma->target);
                if (!cts) return nullptr;
                if (ma->member == "IsCancellationRequested") {
                    auto* fn = declareC("bz_cts_is_cancelled", i32(), {ptrTy()});
                    llvm::Value* flag = builder_.CreateCall(fn, {cts}, "cts.iscancelled");
                    return builder_.CreateICmpNE(flag, builder_.getInt32(0), "cts.icr.b");
                }
                if (ma->member == "Token") {
                    auto* fn = declareC("bz_cts_token", ptrTy(), {ptrTy()});
                    return builder_.CreateCall(fn, {cts}, "cts.token.prop");
                }
            }
            // stdlib §6.10 (v0.175, #808): TaskScope.Token property read —
            // the scope's internal CancellationToken (borrowed cell pointer).
            if (cn == "TaskScope" && ma->member == "Token") {
                llvm::Value* sc = lowerExpr(*ma->target);
                if (!sc) return nullptr;
                auto* fn = declareC("bz_task_scope_token", ptrTy(), {ptrTy()});
                return builder_.CreateCall(fn, {sc}, "tscope.token.prop");
            }
            // stdlib §5 (#651): IGrouping<TKey, T>.Key property read.
            // IGrouping objects are bz_grouping_t* pointers; the key is
            // stored as key_esz bytes at group->key_data. bz_grouping_key
            // copies the key bytes into a caller-allocated slot.
            // The key type (TKey) determines the LLVM type for the slot.
            if (cn == "IGrouping" && ma->member == "Key" && tt &&
                !tt->genericArgs.empty() && tt->genericArgs[0]) {
                llvm::Value* grp = lowerExpr(*ma->target);
                if (!grp) return nullptr;
                sema::TypeRef* keyRef = tt->genericArgs[0];
                llvm::Type* keyTy = lowerTypeRef(keyRef);
                if (!keyTy) keyTy = i32();
                llvm::AllocaInst* keySlot = entryAlloca(keyTy);
                auto* fn = declareC("bz_grouping_key", llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {grp, keySlot});
                return builder_.CreateLoad(keyTy, keySlot, "grp.key");
            }
            // stdlib §3.2 v0.136: Encoding.Utf8.Name / Utf16.Name / Utf32.Name /
            // Ascii.Name — read-only property returning the IANA encoding label.
            // The shape is: ma->member == "Name", ma->target is a
            // MemberAccessExpr(target=Id("Encoding"), member=<singleton>).
            if (ma->member == "Name") {
                if (auto* inner2 =
                        dynamic_cast<const ast::MemberAccessExpr*>(
                            ma->target.get())) {
                    if (auto* encId =
                            dynamic_cast<const ast::IdentifierExpr*>(
                                inner2->target.get())) {
                        if (encId->name == "Encoding") {
                            const char* nameStr = nullptr;
                            if (inner2->member == "UTF8")  nameStr = "utf-8";
                            if (inner2->member == "UTF16") nameStr = "utf-16le";
                            if (inner2->member == "UTF32") nameStr = "utf-32le";
                            if (inner2->member == "ASCII") nameStr = "us-ascii";
                            if (nameStr) {
                                // Emit as a string literal (constant global).
                                // Wrap in quotes — lowerStringLiteral strips them.
                                return lowerStringLiteral(
                                    std::string("\"") + nameStr + "\"", false);
                            }
                        }
                    }
                }
            }
            // CancellationToken.None (static property, target is type-name identifier).
            if (auto* id2 = dynamic_cast<const ast::IdentifierExpr*>(ma->target.get())) {
                if (id2->name == "CancellationToken" && ma->member == "None") {
                    return llvm::ConstantPointerNull::get(ptrTy());
                }
                // stdlib §2.6: Guid.Empty — the all-zero Guid (static readonly property).
                if (id2->name == "Guid" && ma->member == "Empty") {
                    return llvm::ConstantAggregateZero::get(guidStructType());
                }
                // stdlib §9.3 / fix #521: Decimal static constants.
                if (id2->name == "Decimal" || id2->name == "decimal") {  // §5: both spellings (spec L243)
                    llvm::StructType* dst = decimalStructType();
                    const auto& mbr = ma->member;
                    if (mbr == "Zero")
                        return llvm::ConstantAggregateZero::get(dst);
                    if (mbr == "One") {
                        uint32_t dec[4] = {0, 0, 0, 0};
                        bz_decimal_parse_cstr("1", dec, nullptr, nullptr);
                        return llvm::ConstantStruct::get(dst, {
                            llvm::ConstantInt::get(i32(), dec[0]),
                            llvm::ConstantInt::get(i32(), dec[1]),
                            llvm::ConstantInt::get(i32(), dec[2]),
                            llvm::ConstantInt::get(i32(), dec[3])
                        });
                    }
                    if (mbr == "MinValue") {
                        uint32_t dec[4] = {0, 0, 0, 0};
                        bz_decimal_parse_cstr("-79228162514264337593543950335", dec, nullptr, nullptr);
                        return llvm::ConstantStruct::get(dst, {
                            llvm::ConstantInt::get(i32(), dec[0]),
                            llvm::ConstantInt::get(i32(), dec[1]),
                            llvm::ConstantInt::get(i32(), dec[2]),
                            llvm::ConstantInt::get(i32(), dec[3])
                        });
                    }
                    if (mbr == "MaxValue") {
                        uint32_t dec[4] = {0, 0, 0, 0};
                        bz_decimal_parse_cstr("79228162514264337593543950335", dec, nullptr, nullptr);
                        return llvm::ConstantStruct::get(dst, {
                            llvm::ConstantInt::get(i32(), dec[0]),
                            llvm::ConstantInt::get(i32(), dec[1]),
                            llvm::ConstantInt::get(i32(), dec[2]),
                            llvm::ConstantInt::get(i32(), dec[3])
                        });
                    }
                }
            }
        }
    }
    // fix #499 / §9.8: Range<T> instance member access.
    // The Range struct is { i64 start, i64 end, i64 step, i8 flags }.
    // flags bit 0 = IsEndInclusive; bit 1 = startOpen; bit 2 = endOpen.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) {
            auto* tt = ctx_.exprType(*ma->target);
            if (isRangeType(tt)) {
                llvm::Value* rv = lowerExpr(*ma->target);
                if (!rv) return nullptr;
                const auto& mbr = ma->member;
                if (mbr == "Start") {
                    // §9.8: Start is T? (nullable). For the value-in-var case we
                    // return the i64 lo field directly, cast to i32 (int range).
                    // TODO: open-ended ranges (bit 1 of flags) should return null.
                    llvm::Value* lo = builder_.CreateExtractValue(rv, {0u}, "r.start");
                    // Narrow to i32 for int-range Start (T = int → i32).
                    return builder_.CreateTrunc(lo, i32(), "r.start.i32");
                }
                if (mbr == "End") {
                    llvm::Value* hi = builder_.CreateExtractValue(rv, {1u}, "r.end");
                    return builder_.CreateTrunc(hi, i32(), "r.end.i32");
                }
                if (mbr == "Step") {
                    // §9.8: Step is long (i64).
                    return builder_.CreateExtractValue(rv, {2u}, "r.step");
                }
                if (mbr == "IsEndInclusive") {
                    // flags bit 0.
                    llvm::Value* flg = builder_.CreateExtractValue(rv, {3u}, "r.flg");
                    return builder_.CreateTrunc(flg, i1(), "r.incl");
                }
                if (mbr == "IsOpenEnded") {
                    // §9.8: true if Start or End is null (bits 1 or 2 of flags).
                    llvm::Value* flg = builder_.CreateExtractValue(rv, {3u}, "r.flg");
                    // (flags >> 1) & 3 != 0  →  startOpen or endOpen set.
                    llvm::Value* shifted = builder_.CreateLShr(
                        flg, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 1));
                    llvm::Value* masked  = builder_.CreateAnd(
                        shifted,
                        llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 3));
                    return builder_.CreateICmpNE(
                        masked,
                        llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0),
                        "r.open");
                }
                if (mbr == "IsEmpty") {
                    // §16: a range is empty when it has no values in the
                    // ITERATION DIRECTION (set by the Step's sign, §16 L2333 —
                    // a direction that disagrees with the step is empty), or both
                    // endpoints are equal with an exclusive upper bound.
                    //   ascending (step >= 0): empty if Start > End (closed) /
                    //                          Start >= End (half-open)
                    //   descending (step <  0): empty if Start < End (closed) /
                    //                          Start <= End (half-open)
                    // The prior code ignored Step → wrong for descending /
                    // direction-mismatch ranges (e.g. `10..0 step -1` reported
                    // empty; `0..10 step -1` reported non-empty).
                    llvm::Value* st   = builder_.CreateExtractValue(rv, {0u}, "r.st");
                    llvm::Value* en   = builder_.CreateExtractValue(rv, {1u}, "r.en");
                    llvm::Value* step = builder_.CreateExtractValue(rv, {2u}, "r.step");
                    llvm::Value* flg  = builder_.CreateExtractValue(rv, {3u}, "r.flg");
                    llvm::Value* incl = builder_.CreateTrunc(flg, i1(), "r.incl");
                    llvm::Value* stepNeg = builder_.CreateICmpSLT(
                        step, llvm::ConstantInt::get(i64(), 0), "r.stepNeg");
                    llvm::Value* ascIncl  = builder_.CreateICmpSGT(st, en, "r.ai");
                    llvm::Value* ascHalf  = builder_.CreateICmpSGE(st, en, "r.ah");
                    llvm::Value* descIncl = builder_.CreateICmpSLT(st, en, "r.di");
                    llvm::Value* descHalf = builder_.CreateICmpSLE(st, en, "r.dh");
                    llvm::Value* emptyAsc =
                        builder_.CreateSelect(incl, ascIncl, ascHalf, "r.ea");
                    llvm::Value* emptyDesc =
                        builder_.CreateSelect(incl, descIncl, descHalf, "r.ed");
                    llvm::Value* dirEmpty = builder_.CreateSelect(
                        stepNeg, emptyDesc, emptyAsc, "r.direm");
                    // §16.3 (v0.162, fix #1496): an open-ended range (flags bit
                    // 1 = startOpen, bit 2 = endOpen) is NEVER empty — a missing
                    // endpoint imposes no bound, so the direction-based test
                    // above (which reads a fake 0 sentinel on the open side) must
                    // not decide emptiness for it. This mirrors the `notEmpty =
                    // isOpenEnded || !dirEmpty` short-circuit already used by
                    // Contains and the `in` operator lowerings.
                    llvm::Value* openFlg = builder_.CreateAnd(
                        flg, llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 6),
                        "r.oflg");
                    llvm::Value* isOE = builder_.CreateICmpNE(
                        openFlg,
                        llvm::ConstantInt::get(llvm::Type::getInt8Ty(llvm_), 0),
                        "r.oe");
                    return builder_.CreateAnd(
                        builder_.CreateNot(isOE), dirEmpty, "r.empty");
                }
            }
        }
    }
    // §2.5: DateTime and TimeSpan instance property reads.
    // DateTime/TimeSpan are scalar i64 tick values; properties are computed
    // by calling the appropriate bz_datetime_* / bz_timespan_* runtime helpers.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) {
            auto* tt = ctx_.exprType(*ma->target);
            std::string_view cn = builtinTypeName(tt);
            if (cn == "DateTime") {
                llvm::Value* ticks = lowerExpr(*ma->target);
                if (!ticks) return nullptr;
                const auto& mbr = ma->member;
                // Integer (i32) component properties.
                const char* fnName = nullptr;
                if      (mbr == "Year")        fnName = "bz_datetime_year";
                else if (mbr == "Month")       fnName = "bz_datetime_month";
                else if (mbr == "Day")         fnName = "bz_datetime_day";
                else if (mbr == "Hour")        fnName = "bz_datetime_hour";
                else if (mbr == "Minute")      fnName = "bz_datetime_minute";
                else if (mbr == "Second")      fnName = "bz_datetime_second";
                else if (mbr == "Millisecond") fnName = "bz_datetime_millisecond";
                else if (mbr == "DayOfWeek")   fnName = "bz_datetime_day_of_week";
                else if (mbr == "DayOfYear")   fnName = "bz_datetime_day_of_year";
                if (fnName) {
                    auto* fn = declareC(fnName, i32(), {i64()});
                    return builder_.CreateCall(fn, {ticks}, llvm::Twine(mbr));
                }
                // Ticks property — return the raw i64.
                if (mbr == "Ticks") return ticks;
                // per §2.5 / #1357: Date → DateOnly (i32 day number from epoch).
                // TICKS_PER_DAY = 864000000000LL
                if (mbr == "Date") {
                    llvm::Value* tpd = builder_.getInt64(864000000000LL);
                    return builder_.CreateTrunc(
                        builder_.CreateSDiv(ticks, tpd, "dt.date.dn"),
                        i32(), "dt.date");
                }
                // per §2.5 / #1357: TimeOfDay → TimeOnly (i32 ms from midnight).
                // ms = (ticks % TICKS_PER_DAY) / TICKS_PER_MS
                if (mbr == "TimeOfDay") {
                    llvm::Value* tpd = builder_.getInt64(864000000000LL);
                    llvm::Value* tpm = builder_.getInt64(10000LL);
                    llvm::Value* rem = builder_.CreateSRem(ticks, tpd, "dt.tod.rem");
                    return builder_.CreateTrunc(
                        builder_.CreateSDiv(rem, tpm, "dt.tod.ms"),
                        i32(), "dt.tod");
                }
            }
            if (cn == "TimeSpan") {
                llvm::Value* ticks = lowerExpr(*ma->target);
                if (!ticks) return nullptr;
                const auto& mbr = ma->member;
                // Total* properties return double.
                if (mbr == "TotalDays") {
                    auto* fn = declareC("bz_timespan_total_days",
                                        llvm::Type::getDoubleTy(llvm_), {i64()});
                    return builder_.CreateCall(fn, {ticks}, "ts.totaldays");
                }
                if (mbr == "TotalHours") {
                    auto* fn = declareC("bz_timespan_total_hours",
                                        llvm::Type::getDoubleTy(llvm_), {i64()});
                    return builder_.CreateCall(fn, {ticks}, "ts.totalhours");
                }
                if (mbr == "TotalMinutes") {
                    auto* fn = declareC("bz_timespan_total_minutes",
                                        llvm::Type::getDoubleTy(llvm_), {i64()});
                    return builder_.CreateCall(fn, {ticks}, "ts.totalmins");
                }
                if (mbr == "TotalSeconds") {
                    auto* fn = declareC("bz_timespan_total_seconds",
                                        llvm::Type::getDoubleTy(llvm_), {i64()});
                    return builder_.CreateCall(fn, {ticks}, "ts.totalsecs");
                }
                if (mbr == "TotalMilliseconds") {
                    auto* fn = declareC("bz_timespan_total_milliseconds",
                                        llvm::Type::getDoubleTy(llvm_), {i64()});
                    return builder_.CreateCall(fn, {ticks}, "ts.totalms");
                }
                // Component (int) properties — decompose via bz_timespan_components.
                // Each allocates output slots, calls the helper, returns one slot.
                auto emitComponent = [&](int slot) -> llvm::Value* {
                    llvm::Value* dS  = entryAlloca(i32());
                    llvm::Value* hS  = entryAlloca(i32());
                    llvm::Value* miS = entryAlloca(i32());
                    llvm::Value* sS  = entryAlloca(i32());
                    llvm::Value* msS = entryAlloca(i32());
                    auto* fn = declareC("bz_timespan_components",
                        llvm::Type::getVoidTy(llvm_),
                        {i64(), ptrTy(), ptrTy(), ptrTy(), ptrTy(), ptrTy()});
                    builder_.CreateCall(fn, {ticks, dS, hS, miS, sS, msS});
                    llvm::Value* slots[] = {dS, hS, miS, sS, msS};
                    return builder_.CreateLoad(i32(), slots[slot]);
                };
                if (mbr == "Days")         return emitComponent(0);
                if (mbr == "Hours")        return emitComponent(1);
                if (mbr == "Minutes")      return emitComponent(2);
                if (mbr == "Seconds")      return emitComponent(3);
                if (mbr == "Milliseconds") return emitComponent(4);
                // Ticks property — raw i64.
                if (mbr == "Ticks") return ticks;
            }
            // per §2.5 / #1357: DateOnly property reads.
            // DateOnly is stored as i32 day-number (ticks / TICKS_PER_DAY since epoch).
            if (cn == "DateOnly") {
                llvm::Value* dayNum = lowerExpr(*ma->target);
                if (!dayNum) return nullptr;
                if (!dayNum->getType()->isIntegerTy(32))
                    dayNum = builder_.CreateTrunc(dayNum, i32(), "do.dn");
                const auto& mbr = ma->member;
                // Expand day number → ticks for decomposition via bz_datetime_* helpers.
                llvm::Value* dn64 = builder_.CreateSExt(dayNum, i64(), "do.dn64");
                llvm::Value* tpd = builder_.getInt64(864000000000LL);
                llvm::Value* dticks = builder_.CreateMul(dn64, tpd, "do.ticks");
                if (mbr == "Year") {
                    auto* fn = declareC("bz_datetime_year", i32(), {i64()});
                    return builder_.CreateCall(fn, {dticks}, "do.year");
                }
                if (mbr == "Month") {
                    auto* fn = declareC("bz_datetime_month", i32(), {i64()});
                    return builder_.CreateCall(fn, {dticks}, "do.month");
                }
                if (mbr == "Day") {
                    auto* fn = declareC("bz_datetime_day", i32(), {i64()});
                    return builder_.CreateCall(fn, {dticks}, "do.day");
                }
            }
            // per §2.5 / #1357: TimeOnly property reads.
            // TimeOnly is stored as i32 ms from midnight (0..86399999).
            if (cn == "TimeOnly") {
                llvm::Value* ms = lowerExpr(*ma->target);
                if (!ms) return nullptr;
                if (!ms->getType()->isIntegerTy(32))
                    ms = builder_.CreateTrunc(ms, i32(), "to.ms");
                const auto& mbr = ma->member;
                if (mbr == "Hour") {
                    // hour = ms / 3600000
                    return builder_.CreateSDiv(ms, builder_.getInt32(3600000), "to.hour");
                }
                if (mbr == "Minute") {
                    // minute = (ms % 3600000) / 60000
                    llvm::Value* r = builder_.CreateSRem(ms, builder_.getInt32(3600000), "to.mr");
                    return builder_.CreateSDiv(r, builder_.getInt32(60000), "to.min");
                }
                if (mbr == "Second") {
                    // second = (ms % 60000) / 1000
                    llvm::Value* r = builder_.CreateSRem(ms, builder_.getInt32(60000), "to.sr");
                    return builder_.CreateSDiv(r, builder_.getInt32(1000), "to.sec");
                }
                if (mbr == "Millisecond") {
                    // millisecond = ms % 1000
                    return builder_.CreateSRem(ms, builder_.getInt32(1000), "to.msec");
                }
            }
        }
    }
    // §15.1: inline array `a.Length` — compile-time constant equal to N.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target && ma->member == "Length") {
            auto* tt = ctx_.exprType(*ma->target);
            if (tt && tt->kind == sema::TypeRef::Kind::InlineArray) {
                // The array size is encoded in tt->arraySize (IntLiteralExpr).
                // Evaluate it as a compile-time constant.
                if (auto* slit = dynamic_cast<const ast::IntLiteralExpr*>(tt->arraySize)) {
                    int64_t sz = (int64_t)parseIntLiteral(slit->text);
                    return llvm::ConstantInt::get(i32(), sz, true);
                }
                if (tt->arraySizeValue >= 0)  // #734: folded member-const size
                    return llvm::ConstantInt::get(i32(), tt->arraySizeValue,
                                                  true);
            }
            // Also handle the case where the target is an inline-array lvalue
            // and we can get the array type from the LLVM type.
            llvm::Type* elemTy = nullptr;
            if (llvm::Value* addr = lvalueAddr(*ma->target, elemTy)) {
                if (auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(elemTy)) {
                    return llvm::ConstantInt::get(i32(), arrTy->getNumElements(), true);
                }
            }
        }
    }
    // §4.15 (#549): ArraySpan<T>/ReadOnlyArraySpan<T> readonly property reads.
    // Both types are value structs { ptr source, i32 start, i32 length }.
    // Length extracts field 2; IsEmpty compares field 2 to 0.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target &&
            (ma->member == "Length" || ma->member == "IsEmpty" ||
             ma->member == "Start")) {
            auto* tt = ctx_.exprType(*ma->target);
            // §3.3 (#883): ReadOnlyStringSpan shares the {ptr,i32,i32} layout, so
            // Length/IsEmpty are the same field-2 read as the array-backed spans.
            bool isStrSpan = tt && tt->kind == sema::TypeRef::Kind::Named &&
                !tt->declaration && tt->astNode && tt->astNode->name &&
                !tt->astNode->name->parts.empty() &&
                tt->headName() == "ReadOnlyStringSpan";
            if (isSpanLikeTypeName(tt) || isStrSpan) {
                llvm::Value* sv = lowerExpr(*ma->target);
                if (!sv) return nullptr;
                // §4.14 (#1277): `Start` is field 1 of the { ptr source, i32
                // start, i32 length } span struct. It had no handler, so `sp.Start`
                // fell through to the generic member-access path and read 0 (also
                // poisoning a sibling `sp.Length` in the same expression). Read
                // field 1 directly.
                if (ma->member == "Start")
                    return builder_.CreateExtractValue(sv, {1u}, "sp.start");
                llvm::Value* slen = builder_.CreateExtractValue(
                    sv, {2u}, "sp.len");
                if (ma->member == "Length")
                    return slen;
                // IsEmpty: length == 0
                return builder_.CreateICmpEQ(
                    slen, builder_.getInt32(0), "sp.empty");
            }
        }
    }
    // stdlib §10.1 / §10.2 / fix #983: CString.Length / WString.Length property
    // reads. A CString is a bare null-terminated char* (bz_string_to_utf8z /
    // bz_cstring_from_bytes); its Length is strlen, the byte count excluding the
    // terminator (§10.1). A WString is a bare null-terminated uint16_t*
    // (bz_string_to_utf16z); its Length is the UTF-16 code-unit count via
    // bz_wstrlen, excluding the terminating zero unit (§10.2). Both are builtin
    // FFI-boundary types whose TypeRef declaration is the builtin class symbol.
    // Before this, a member read fell through every handler and `Length` was 0.
    // fix #985: RawBytes.Length now reads the `len` field of the { ptr, len }
    // struct (bz_rawbytes_length) — the prior bare-buffer rep dropped the length.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target && ma->member == "Length") {
            auto* tt = ctx_.exprType(*ma->target);
            // fix #2809 (v0.240): match through `isFfiBufferTypeRef`, which reads
            // BOTH shapes a §10 builtin reaches codegen in — the FACTORY result
            // (`CString.From(s)`, declaration = builtinClassSym, set in
            // ExprTypecheck_Calls.cpp) and the ANNOTATION (`c: CString` on a
            // parameter or field, declaration = null, head name only). The
            // original declaration-only test covered only the first, so a
            // `CString` PARAMETER's `.Length` fell through to the string
            // fallback above and read the raw buffer at %bz.string field 1.
            bool isCStr = isFfiBufferTypeRef(tt) &&
                builtinTypeName(tt) == "CString";
            bool isWStr = isFfiBufferTypeRef(tt) &&
                builtinTypeName(tt) == "WString";
            bool isRaw = isFfiBufferTypeRef(tt) &&
                builtinTypeName(tt) == "RawBytes";
            if (isCStr || isWStr || isRaw) {
                llvm::Value* buf = lowerExpr(*ma->target);
                if (!buf) return nullptr;
                // fix #2828 / §28.3 (v0.240) *Unbound owned temporaries*:
                // the receiver is borrowed across the member read (§28.3 names
                // a member access as one of the listed borrowing uses), so the
                // §32.5/§32.6 owned buffer the factory minted must be reclaimed
                // "at the end of the full expression in which it is produced,
                // after every use of it within that expression has completed".
                // The use here is the read itself; the disposal runs at the K8
                // boundary via flushFfiExprTemps. Pre-fix the slot was simply
                // dropped, leaking the malloc'd buffer (LSan: 6 B per `CString`
                // iteration, 12 B per `WString` iteration, plus the {ptr,len}
                // struct and inner buffer for `RawBytes`).
                //
                // `isFfiBufferFactoryCall` admits ONLY a directly-written
                // `CString.From/FromBytes` / `WString.From` / `RawBytes.From`
                // — by construction bound to no name, exactly §28.3's
                // *unbound* case. A `using var`-bound receiver is a plain
                // `IdentifierExpr` and never matches, so the `using var`'s own
                // discharge is untouched (row G of the regression test pins
                // this with runAotChecked).
                //
                // The RawBytes arm sets `isRawBytes=true`, so the slot reaches
                // the RawBytes free (the {ptr,len} struct, not the inner
                // buffer alone). Deferred to a follow-up wave:
                // `LlvmBackend_Calls.cpp` sites for `.ToBytes` and `.CopyTo`
                // (the method arms of the same family) — file-disjoint with
                // this one.
                {
                    bool recvIsRawBytes = false;
                    if (isFfiBufferFactoryCall(*ma->target, recvIsRawBytes)) {
                        auto* tmpSlot = entryAllocaNullPtr();
                        builder_.CreateStore(buf, tmpSlot);
                        registerFfiExprTemp(tmpSlot, recvIsRawBytes);
                    }
                }
                llvm::Value* len;
                if (isRaw) {
                    // §10.3: Length is the explicit `len` field of the struct.
                    auto* fn = declareC("bz_rawbytes_length", i64(), {ptrTy()});
                    len = builder_.CreateCall(fn, {buf}, "raw.len");
                } else if (isWStr) {
                    auto* fn = declareC("bz_wstrlen", i64(), {ptrTy()});
                    len = builder_.CreateCall(fn, {buf}, "wstr.len");
                } else {
                    auto* fn = declareC("strlen", i64(), {ptrTy()});
                    len = builder_.CreateCall(fn, {buf}, "cstr.len");
                }
                // §10.1/§10.2/§10.3: Length is `int` (i32) — truncate the count.
                return builder_.CreateTrunc(len, i32(), "ffi.len.i32");
            }
        }
    }
    // stdlib §10.1/§10.2/§10.3 (fix #985, #2825/#2810): the `Pointer` readonly
    // property of the THREE §10 FFI-boundary buffer classes — the underlying
    // buffer address as an IntPtr (i64 on the host, §32.7).
    //
    // fix #2825 / #2810 (v0.240): this arm used to test `RawBytes` ALONE, so
    // `CString.Pointer` and `WString.Pointer` had no lowering at all: the read
    // fell through to CG8's permissive member fall-through and the slot degraded
    // to zero (#2547/#2434), handing the program a NULL address behind two
    // `internal:` WARNINGS — the compile succeeded. That is the worst failure
    // mode this member has, because stdlib §10.1 names the consumer: `Pointer`
    // is "the element source for `char*[]` argv-building over
    // `OwningBuffer<IntPtr>`", so the null is not caught on the Bezel side at
    // all — it is dereferenced in C. Enumerating the three heads per-arm is what
    // produced the defect (`.Length` directly above already had all three), so
    // the family test is now the shared registry predicate, once.
    //
    // Lifetime, per spec §32.10 (v0.240) verbatim: "For addresses of Bezel-owned
    // mutable storage (`RawBytes.Pointer`, `CString`/`WString.Pointer`,
    // `OwningBuffer<T>.DangerousPointer()` — stdlib §10, §4.14): native retention
    // of such an address past the synchronous call is a use-after-free hazard no
    // rule in this chapter can track — validity ends at a disposal the native
    // side is never told about." So the address is an ALIAS of the receiver's
    // buffer, never a fresh allocation: nothing here is owned, nothing here is
    // freed, and a read after the owner's disposal is the programmer's §32.10
    // obligation, not a rule the compiler enforces (§32.12 item 7: "every
    // address the language exposes is stable until its owner's disposal").
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target && ma->member == "Pointer") {
            auto* tt = ctx_.exprType(*ma->target);
            // fix #2809: both TypeRef shapes (see the `.Length` arm above) —
            // `isFfiBufferTypeRef` reads the set through `builtinTypeName`, the
            // accessor that handles the FACTORY result (`CString.From(s)`,
            // declaration = builtinClassSym) and the ANNOTATION (`c: CString` on
            // a parameter/field, declaration = null) alike.
            if (isFfiBufferTypeRef(tt)) {
                llvm::Value* buf = lowerExpr(*ma->target);
                if (!buf) return nullptr;
                // fix #2828 / §28.3 (v0.240) *Unbound owned temporaries*:
                // mirror of the `.Length` arm directly above. `CString.From(s)`
                // mints an owned §32.5/§32.6 buffer that the member read
                // borrows — the buffer is owed a §28.3 reclaim at the K8
                // boundary of the full expression. Pre-fix the slot was
                // dropped, leaking the malloc'd buffer (LSan: 6 B per
                // `CString.Pointer` iteration, 12 B per `WString.Pointer`,
                // plus the {ptr,len} struct for `RawBytes.Pointer`).
                // `isFfiBufferFactoryCall` admits only a directly-written
                // factory call — by construction bound to no name — so the
                // `using var` spelling (a plain `IdentifierExpr`) is
                // untouched.
                {
                    bool recvIsRawBytes = false;
                    if (isFfiBufferFactoryCall(*ma->target, recvIsRawBytes)) {
                        auto* tmpSlot = entryAllocaNullPtr();
                        builder_.CreateStore(buf, tmpSlot);
                        registerFfiExprTemp(tmpSlot, recvIsRawBytes);
                    }
                }
                llvm::Value* p;
                if (builtinTypeName(tt) == "RawBytes") {
                    // §10.3: the value is a { ptr, len } struct — read field 0.
                    auto* fn = declareC("bz_rawbytes_ptr", ptrTy(), {ptrTy()});
                    p = builder_.CreateCall(fn, {buf}, "raw.ptr");
                } else {
                    // §10.1/§10.2: a CString is a bare null-terminated char*
                    // and a WString a bare null-terminated uint16_t*
                    // (bz_string_to_utf8z / bz_string_to_utf16z /
                    // bz_cstring_from_bytes each return the malloc'd buffer
                    // itself). The runtime value IS the address — no helper.
                    p = buf;
                }
                // §32.7: IntPtr is a pointer-sized integer (i64 host).
                return builder_.CreatePtrToInt(p, i64(), "ffi.ptr.i");
            }
        }
    }
    // fix #667 / §30.15 / stdlib §6.9: Channel<T>.IsClosed and Channel<T>.Count
    // as property reads (MemberAccessExpr without parens). These are "readonly
    // property" in the stdlib, so the parser generates a MemberAccessExpr (not
    // a CallExpr), bypassing tryLowerChannelAndDomainCall.  Handle them here so
    // the MemberAccess path correctly calls bz_channel_is_closed /
    // bz_channel_count.  The null-guards in those runtime functions (fix #667)
    // ensure that null (post-Dispose slot) returns the expected sentinel values.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target &&
            (ma->member == "IsClosed" || ma->member == "Count")) {
            auto* tt = ctx_.exprType(*ma->target);
            bool isChProp = (tt && tt->kind == sema::TypeRef::Kind::Named &&
                             !tt->declaration && tt->astNode &&
                             tt->astNode->name &&
                             !tt->astNode->name->parts.empty() &&
                             tt->headName() == "Channel");
            if (isChProp) {
                llvm::Value* ch = lowerExpr(*ma->target);
                if (ch) {
                    if (ma->member == "IsClosed") {
                        auto* fn = declareC("bz_channel_is_closed",
                                           i32(), {ptrTy()});
                        llvm::Value* r = builder_.CreateCall(
                            fn, {ch}, "ch.isclosed");
                        return builder_.CreateICmpNE(r, builder_.getInt32(0));
                    } else {
                        auto* fn = declareC("bz_channel_count",
                                           i64(), {ptrTy()});
                        llvm::Value* c = builder_.CreateCall(fn, {ch});
                        return builder_.CreateTrunc(c, i32(), "ch.count.i");
                    }
                }
            }
        }
    }
    // §30.3 (#942): Task.IsCompleted / IsFaulted / IsCancelled property reads.
    // These are "readonly property" in the stdlib, so the parser produces a
    // MemberAccessExpr (no parens), bypassing the Task static-call lowering.
    // Semantics (spec §30.3 — faulted and cancelled are distinct states):
    //   IsCompleted = the task is done (bz_task_is_done — true for a
    //                 successful, faulted, OR cancelled task).
    //   IsCancelled = the task is errored AND its error object is an
    //                 OperationCanceledException (cancellation is represented as
    //                 a faulted task carrying an OCE — see Task.FromCancelled /
    //                 the cancellation-aware Task.Delay).
    //   IsFaulted   = the task is errored with a NON-cancellation exception
    //                 (is_error AND not an OCE), so IsFaulted and IsCancelled are
    //                 mutually exclusive.
    // The OCE is distinguished by its vtable word (the same vtable codegen
    // installs in slot 0 of the OCE object Task.FromCancelled allocates).
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target &&
            (ma->member == "IsCompleted" || ma->member == "IsFaulted" ||
             ma->member == "IsCancelled")) {
            auto* tt = ctx_.exprType(*ma->target);
            if (isTaskTypeName(tt)) {
                llvm::Value* task = lowerExpr(*ma->target);
                if (task) {
                    if (ma->member == "IsCompleted") {
                        auto* fn = declareC("bz_task_is_done", i1(), {ptrTy()});
                        return builder_.CreateCall(fn, {task},
                                                   "task.iscompleted");
                    }
                    // IsFaulted / IsCancelled both need is_error + the OCE test.
                    auto* isErrFn =
                        declareC("bz_task_is_error", i1(), {ptrTy()});
                    llvm::Value* isErr =
                        builder_.CreateCall(isErrFn, {task}, "task.iserr");
                    // isOce: is_error AND error_obj != null AND its vtable word
                    // == the OperationCanceledException vtable. Compute with a
                    // null-guarded load (loading from a null error_obj is UB).
                    llvm::Value* isOce = builder_.getFalse();
                    auto vit = builtinVtable_.find("OperationCanceledException");
                    if (vit != builtinVtable_.end()) {
                        auto* errObjFn =
                            declareC("bz_task_error_obj", ptrTy(), {ptrTy()});
                        llvm::Value* errObj = builder_.CreateCall(
                            errObjFn, {task}, "task.errobj");
                        llvm::Value* notNull = builder_.CreateICmpNE(
                            errObj,
                            llvm::ConstantPointerNull::get(ptrTy()));
                        auto* entryBB = builder_.GetInsertBlock();
                        auto* loadBB = llvm::BasicBlock::Create(
                            llvm_, "task.oce.load", currentFn_);
                        auto* contBB = llvm::BasicBlock::Create(
                            llvm_, "task.oce.cont", currentFn_);
                        builder_.CreateCondBr(notNull, loadBB, contBB);
                        builder_.SetInsertPoint(loadBB);
                        llvm::Value* vt =
                            builder_.CreateLoad(ptrTy(), errObj, "task.errvt");
                        llvm::Value* oceVt = builder_.CreateBitCast(
                            vit->second, ptrTy());
                        llvm::Value* eq =
                            builder_.CreateICmpEQ(vt, oceVt, "task.isoce.eq");
                        builder_.CreateBr(contBB);
                        builder_.SetInsertPoint(contBB);
                        auto* phi = builder_.CreatePHI(i1(), 2, "task.isoce");
                        phi->addIncoming(builder_.getFalse(), entryBB);
                        phi->addIncoming(eq, loadBB);
                        isOce = builder_.CreateAnd(isErr, phi);
                    }
                    if (ma->member == "IsCancelled")
                        return isOce;
                    // IsFaulted = is_error AND not a cancellation.
                    return builder_.CreateAnd(
                        isErr, builder_.CreateNot(isOce), "task.isfaulted");
                }
            }
        }
    }
    // §30.3 / §30.5 (#941 / #1127): `t.Result` — the synchronous escape hatch
    // on a Task<T> or OwningTask<T> receiver (a builtin class, no MethodSymbol).
    // This is the SYNCHRONOUS read used from non-async code (e.g. inside a
    // plain `method Main()`): it blocks the calling thread until the task
    // completes (bz_task_await_sync — the same blocking primitive Wait() uses),
    // then loads the result buffer by value at the element type T. It must NOT
    // route through emitAwaitSuspend, which builds state-machine suspend/resume
    // blocks valid only inside an async resume function.
    // For Task<T>: T is a value/reference type; the buffer holds T directly.
    // For OwningTask<T>: T is an owned heap pointer (class/collection);
    // the buffer holds the T* pointer (8 bytes); loading it transfers ownership
    // to the caller. The task's result buffer (the malloc'd 8-byte slot) is
    // freed by bz_task_free when the task is disposed — it frees only the slot,
    // not the owned T itself, so there is no double-dispose with the caller.
    // Spec §30.3: "Reading Result on a faulted task re-raises the exception" —
    // we re-raise the ORIGINAL exception object (bz_task_error_obj) through the
    // ordinary synchronous-throw machinery (emitThrowValue), so an enclosing
    // try/catch sees the real exception type and the function's unwind boundary
    // propagates it otherwise. Both Task<T> and OwningTask<T> are handled here;
    // isTaskTypeName fires for both names.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target && ma->member == "Result") {
            auto* tt = ctx_.exprType(*ma->target);
            if (isTaskTypeName(tt) && tt->kind == sema::TypeRef::Kind::Named &&
                !tt->genericArgs.empty() && tt->genericArgs[0]) {
                llvm::Value* task = lowerExpr(*ma->target);
                if (task && task->getType()->isPointerTy()) {
                    llvm::Type* resTy = lowerTypeRef(tt->genericArgs[0]);
                    // §30.3 (v0.162 / #1362): catchable at-call-site guard —
                    // reading Result on an INCOMPLETE task inside a domain
                    // continuation raises InvalidOperationException (mirrors
                    // the Wait() guard; runtime abort remains the backstop).
                    {
                        auto* isDoneFn = declareC("bz_task_is_done",
                                                  builder_.getInt1Ty(), {ptrTy()});
                        auto* curDomFn = declareC("bz_current_domain_ptr",
                                                  ptrTy(), {});
                        llvm::Value* done = builder_.CreateCall(
                            isDoneFn, {task}, "result.done");
                        llvm::Value* dom =
                            builder_.CreateCall(curDomFn, {}, "result.dom");
                        llvm::Value* inDomain = builder_.CreateICmpNE(
                            dom, llvm::Constant::getNullValue(ptrTy()),
                            "result.indomain");
                        llvm::Value* wouldDeadlock = builder_.CreateAnd(
                            builder_.CreateNot(done), inDomain,
                            "result.deadlock");
                        emitGuardThrow(wouldDeadlock,
                                       "InvalidOperationException");
                    }
                    // Block until complete; bz_task_await_sync returns a pointer
                    // to the result buffer (already-done → returns immediately).
                    auto* awaitFn =
                        declareC("bz_task_await_sync", ptrTy(), {ptrTy()});
                    llvm::Value* resPtr = builder_.CreateCall(
                        awaitFn, {task}, "result.ptr");
                    // §30.3 / §30.9: a faulted task re-raises its exception.
                    auto* isErrFn =
                        declareC("bz_task_is_error", i1(), {ptrTy()});
                    llvm::Value* faulted = builder_.CreateCall(
                        isErrFn, {task}, "result.faulted");
                    auto* faultBB = llvm::BasicBlock::Create(
                        llvm_, "result.fault", currentFn_);
                    auto* okBB = llvm::BasicBlock::Create(
                        llvm_, "result.ok", currentFn_);
                    builder_.CreateCondBr(faulted, faultBB, okBB);
                    builder_.SetInsertPoint(faultBB);
                    auto* errObjFn =
                        declareC("bz_task_error_obj", ptrTy(), {ptrTy()});
                    llvm::Value* excObj = builder_.CreateCall(
                        errObjFn, {task}, "result.errobj");
                    // §30.4 / §30.9 (#1196): for a BOUND task (`t.Result` where
                    // `t` is a local variable, not an anonymous CallExpr producer)
                    // the error_obj is BORROWED from the task — the task still holds
                    // a reference and disposes error_obj when its own scope ends
                    // (emitDisposals §30.5 task-disposal path, LlvmBackend_Setup.cpp
                    // ~L12534). Mark it borrowed so the catching handler
                    // (caughtException path in emitDisposals, ~L12140) skips reclaim
                    // and only unmarks it. Without this mark, the catch arm frees
                    // error_obj first, then the task scope-end disposes it again →
                    // double-free (SIGSEGV/heap-UAF at the vtable read on second
                    // Dispose). Mirrors the freeTaskAfter==false mark in lowerAwait
                    // (LlvmBackend_Async.cpp ~L1889). An anonymous producer
                    // (`C.Boom().Result`) has no binder that will free error_obj, so
                    // do NOT mark it — the lone catch arm must reclaim it exactly once.
                    // §30.10 / #1529: an anonymous-producer WhenAll/WhenAll<T>
                    // AGGREGATE (`Task.WhenAll(tasks).Result`, isBoundTask ==
                    // false) never owns its error_obj either — it only borrows
                    // a reference from whichever input task produced it (spec
                    // §30.10: "an aggregate task owns no fault of its own").
                    // That input task is still alive and will dispose the SAME
                    // exception at its own, independent scope end, so mark it
                    // borrowed here too (bz_task_fault_borrowed is false for
                    // every ordinarily-faulted task, so this is a no-op there).
                    bool isBoundTask =
                        dynamic_cast<const ast::CallExpr*>(ma->target.get()) ==
                        nullptr;
                    llvm::Value* shouldMarkBorrowed;
                    if (isBoundTask) {
                        shouldMarkBorrowed = builder_.getTrue();
                    } else {
                        auto* fbFn = declareC("bz_task_fault_borrowed",
                                              llvm::Type::getInt8Ty(llvm_),
                                              {ptrTy()});
                        llvm::Value* fb = builder_.CreateCall(
                            fbFn, {task}, "result.faultborrowed");
                        shouldMarkBorrowed = builder_.CreateICmpNE(
                            fb, llvm::ConstantInt::get(
                                    llvm::Type::getInt8Ty(llvm_), 0));
                    }
                    {
                        auto* markBB = llvm::BasicBlock::Create(
                            llvm_, "result.eo.mark", currentFn_);
                        auto* noMarkBB = llvm::BasicBlock::Create(
                            llvm_, "result.eo.nomark", currentFn_);
                        builder_.CreateCondBr(shouldMarkBorrowed, markBB,
                                              noMarkBB);
                        builder_.SetInsertPoint(markBB);
                        auto* markFn = declareC("bz_mark_exc_borrowed",
                                                llvm::Type::getVoidTy(llvm_),
                                                {ptrTy()});
                        builder_.CreateCall(markFn, {excObj});
                        builder_.CreateBr(noMarkBB);
                        builder_.SetInsertPoint(noMarkBB);
                    }
                    // §30.5 (#1127): anonymous producer (`C.Method().Result`)
                    // has no binder that will dispose the task struct; reclaim
                    // it here on the fault path before re-throwing. (excObj is
                    // already in a register and survives the task free.)
                    if (!isBoundTask) {
                        auto* freeTaskFn =
                            declareC("bz_task_free", llvm::Type::getVoidTy(llvm_),
                                     {ptrTy()});
                        builder_.CreateCall(freeTaskFn, {task});
                    }
                    // Re-raise the original exception object through the
                    // synchronous-throw path (stores bz$exc, runs unwind
                    // disposals, branches to the unwind boundary).
                    emitThrowValue(excObj, nullptr);
                    builder_.SetInsertPoint(okBB);
                    // Load the result by value. For a bound task the buffer
                    // lives until the task is disposed by its binder. For an
                    // anonymous producer the buffer was malloc'd by
                    // bz_task_complete and freed when we call bz_task_free
                    // below — but the value is already in a register at that
                    // point so the freed buffer does not alias the returned val.
                    //
                    // ⚠ fix #3099 (§28.3 *Unbound owned temporaries* / §30.3 /
                    // §30.5, v0.252): that reasoning holds ONLY for a value /
                    // `string` / runtime-managed result. For a Pattern A CLASS
                    // (or intrinsic-collection) result the loaded "value" IS the
                    // heap pointer, and since #1503 bz_task_free cascades into
                    // the per-`T` result disposer and frees the pointee — so the
                    // load does NOT decouple them. This is the exact twin of the
                    // `await` defect fixed in #3070, and §28.3 names the two in
                    // ONE clause: "the operand of a Pattern A `await` OR
                    // `.Result` read whose borrowed result is itself confined to
                    // the same full expression … for such an operand the
                    // disposal is deferred to the end of the full expression
                    // rather than performed at the await, so the borrowed result
                    // outlives its uses."
                    llvm::Value* resultVal =
                        builder_.CreateLoad(resTy, resPtr, "result.value");
                    // §30.5 (#1127): reclaim anonymous task on the ok path too.
                    if (!isBoundTask) {
                        // fix #3099: the predicate is patternAResultDisposerFor
                        // itself — the ONE function that decides "disposing this
                        // task disposes its result" — exactly as lowerAwait asks
                        // it (#3070), so the `await` and `.Result` sides cannot
                        // drift apart again. Null for `Task<value/string/
                        // runtime-managed T>` and for `OwningTask<T>` (Pattern
                        // B), each of which keeps freeing at the read.
                        if (patternAResultDisposerFor(tt) != nullptr) {
                            // §28.3: park the task in a slot, register the
                            // reclaim on the cleanup stack (exception edge +
                            // conservative backstop) and queue it for the K8
                            // full-expression boundary, where flushAwaited-
                            // TaskTemps frees it "after every use of the borrow".
                            //
                            // The slot's INITIALIZATION differs from #3070's, and
                            // the difference is load-bearing. The rule is stated
                            // CODEGEN-side, not sema-side, because that is what
                            // the code actually rests on:
                            //
                            //   entryAlloca(ptrTy())  ⟺  the slot is pushed into
                            //   asyncHandlerSpillSlots_, i.e. force-promoted into
                            //   the frame's CALLOC'd spill area.
                            //
                            // Those are the same `inAsyncResume` condition below,
                            // deliberately: the calloc is what zeroes the slot,
                            // and it does so ONCE PER FRAME rather than once per
                            // resume — which is the whole point, since
                            // entryAllocaNullPtr's entry-block `store null`
                            // re-executes on EVERY resume and wipes the saved
                            // pointer (measured, then reverted, at a69b9d9). A
                            // slot that is NOT frame-promoted therefore MUST be
                            // null-initialized here, or the K8 flush frees
                            // uninitialized stack on any path that skipped the
                            // read — measured on
                            //   `var n := (if F() then Use(P().Result) else 7);`
                            // as a SEGV inside free().
                            //
                            // The guard is `currentFn_ == asyncResumeFn_` rather
                            // than anything sema knows, and that is on purpose.
                            // §30.3 rejects `Wait()`/`Result` only "lexically
                            // inside `async` methods and `async` lambdas", so
                            // "this is a synchronous body whose entry block runs
                            // exactly once" is NOT a safe reading of it: a
                            // synchronous ITERATOR's MoveNext re-enters its entry
                            // block on every call, and a resume landing in a
                            // later state block jumps past the store while the
                            // registered scope-end disposal is still reachable —
                            // entryAllocaNullPtr is required there too, for that
                            // second reason. Keying on the emitter's own
                            // frame-promotion decision is correct in both frames
                            // and stays correct wherever the lexical check has
                            // holes (a default-argument expression containing
                            // `.Result` is checked in a non-async context yet can
                            // be lowered inside a resume function).
                            const bool inAsyncResume =
                                smStateStruct_ && asyncResumeFn_ &&
                                currentFn_ == asyncResumeFn_;
                            llvm::AllocaInst* taskSlot =
                                inAsyncResume ? entryAlloca(ptrTy())
                                              : entryAllocaNullPtr();
                            builder_.CreateStore(task, taskSlot);
                            registerAwaitedTaskTemp(taskSlot);
                            if (inAsyncResume)
                                asyncHandlerSpillSlots_.push_back(taskSlot);
                        } else {
                            auto* freeTaskFn =
                                declareC("bz_task_free",
                                         llvm::Type::getVoidTy(llvm_), {ptrTy()});
                            builder_.CreateCall(freeTaskFn, {task});
                        }
                    }
                    // §28.3 / #1250: for a BOUND OwningTask<T:class> .Result read,
                    // null the task's result buffer after loading so the TCS
                    // disposal (bz_tcs_dispose_owned_result) sees no result and
                    // skips dispose — preventing a double-free of the caller-owned
                    // class object.  Check: OwningTask (not plain Task) + ClassOwned.
                    if (isBoundTask) {
                        bool isOwnedClass = false;
                        if (tt && tt->kind == sema::TypeRef::Kind::Named &&
                            !tt->declaration && tt->astNode && tt->astNode->name &&
                            !tt->astNode->name->parts.empty() &&
                            tt->headName() == "OwningTask" &&
                            !tt->genericArgs.empty() && tt->genericArgs[0]) {
                            isOwnedClass =
                                (elementPolicyFor(tt->genericArgs[0],
                                                  /*owningFamily=*/true).cat ==
                                 ElementPolicy::Category::ClassOwned);
                        }
                        if (isOwnedClass) {
                            // fix #4189 (§30.5, v0.298): a `.Result` read is
                            // never the operand of a §30.5 chained return
                            // (`detectAsyncPatternAKind` only matches a
                            // literal `await <...>` value expression, never a
                            // property/member access — see
                            // LlvmBackend_Async.cpp's own detection), so this
                            // extraction is always terminal: drain `task`'s
                            // whole upstream chain unconditionally, the same
                            // as a terminal bound `await` (lowerAwait's
                            // `drainChainOnTake`).
                            auto* takeResultFn = declareC(
                                "bz_task_take_result", llvm::Type::getVoidTy(llvm_),
                                {ptrTy(), llvm::Type::getInt8Ty(llvm_)});
                            llvm::Value* drainArg = llvm::ConstantInt::get(
                                llvm::Type::getInt8Ty(llvm_), 1);
                            builder_.CreateCall(takeResultFn, {task, drainArg});
                        }
                    }
                    return resultVal;
                }
            }
        }
    }
    // §6.6 / issue #974: tcs.Task — readonly property on
    // TaskCompletionSource<T> / OwningTaskCompletionSource<T> /
    // TaskCompletionSource. Returns the inner task pointer (borrowed).
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target && ma->member == "Task") {
            auto* tt = ctx_.exprType(*ma->target);
            bool isTcs = (tt && tt->kind == sema::TypeRef::Kind::Named &&
                          !tt->declaration && tt->astNode &&
                          tt->astNode->name &&
                          !tt->astNode->name->parts.empty() &&
                          (tt->headName() == "TaskCompletionSource" ||
                           tt->headName() == "OwningTaskCompletionSource"));
            if (isTcs) {
                llvm::Value* tcsPtr = lowerExpr(*ma->target);
                if (tcsPtr) {
                    auto* fn = declareC("bz_tcs_task", ptrTy(), {ptrTy()});
                    return builder_.CreateCall(fn, {tcsPtr}, "tcs.task");
                }
            }
        }
    }
    // §3: Array<T>.Length / .Count — runtime bz_array_len.
    // StringBuilder.Length — sum of part lengths (total chars).
    // Dictionary<K,V>.Count — runtime bz_dict_count.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target && (ma->member == "Length" || ma->member == "Count")) {
            auto* tt = ctx_.exprType(*ma->target);
            // StringBuilder.Length: sum of all part lengths.
            bool isSB = (tt && tt->kind == sema::TypeRef::Kind::Named &&
                         !tt->declaration && tt->astNode && tt->astNode->name &&
                         !tt->astNode->name->parts.empty() &&
                         tt->headName() == "StringBuilder");
            if (isSB) {
                llvm::Value* sb = lowerExpr(*ma->target);
                if (sb) {
                    constexpr int64_t SB_ESZ2 = 8;
                    auto* lenFn2 = declareC("bz_array_len", i64(), {ptrTy()});
                    auto* ptrFn2 = declareC("bz_array_ptr_at", ptrTy(), {ptrTy(), i64(), i64()});
                    llvm::Value* nP = builder_.CreateCall(lenFn2, {sb});
                    auto* sumSlot = entryAlloca(i64());
                    auto* idxSlot = entryAlloca(i64());
                    builder_.CreateStore(builder_.getInt64(0), sumSlot);
                    builder_.CreateStore(builder_.getInt64(0), idxSlot);
                    auto* cBB = llvm::BasicBlock::Create(llvm_, "sbl.c", currentFn_);
                    auto* bBB = llvm::BasicBlock::Create(llvm_, "sbl.b", currentFn_);
                    auto* eBB = llvm::BasicBlock::Create(llvm_, "sbl.e", currentFn_);
                    builder_.CreateBr(cBB);
                    builder_.SetInsertPoint(cBB);
                    llvm::Value* si = builder_.CreateLoad(i64(), idxSlot);
                    builder_.CreateCondBr(builder_.CreateICmpSLT(si, nP), bBB, eBB);
                    builder_.SetInsertPoint(bBB);
                    llvm::Value* ep = builder_.CreateCall(ptrFn2, {sb, si, builder_.getInt64(SB_ESZ2)});
                    llvm::Value* part = builder_.CreateLoad(ptrTy(), ep);
                    llvm::Value* plen = loadStringLen(part);
                    llvm::Value* cur = builder_.CreateLoad(i64(), sumSlot);
                    builder_.CreateStore(builder_.CreateAdd(cur, plen), sumSlot);
                    builder_.CreateStore(builder_.CreateAdd(si, builder_.getInt64(1)), idxSlot);
                    builder_.CreateBr(cBB);
                    builder_.SetInsertPoint(eBB);
                    return builder_.CreateTrunc(builder_.CreateLoad(i64(), sumSlot), i32());
                }
            }
            // per §4.6/§4.7: LinkedList<T>/OwningLinkedList<T>.Count via bz_llist_count.
            if (isLinkedListTypeName(tt)) {
                llvm::Value* obj = lowerExpr(*ma->target);
                if (obj) {
                    auto* fn = declareC("bz_llist_count", i64(), {ptrTy()});
                    llvm::Value* cnt64 = builder_.CreateCall(fn, {obj});
                    return builder_.CreateTrunc(cnt64, i32(), "ll.cnt");
                }
            }
            // issue #545: Buffer<T>.Length → i64 header field load.
            // The header layout is { i64 len, i64 elem_size } (bz_buffer_hdr_t).
            // GEP field[0] from the buffer pointer and load as i64, then trunc to
            // i32 (Bezel `int`) for consistency with other collection Length values.
            {
                bool isBuf = tt && tt->kind == sema::TypeRef::Kind::Named &&
                             !tt->declaration && tt->astNode && tt->astNode->name &&
                             !tt->astNode->name->parts.empty() &&
                             tt->headName() == "Buffer";
                if (isBuf && ma->member == "Length") {
                    llvm::Value* obj = lowerExpr(*ma->target);
                    if (obj) {
                        // bz_buffer_hdr_t: { int64 len, int64 elem_size }.
                        // The `len` field is at byte offset 0.
                        // Use a GEP into the raw bytes (i8*) at offset 0 and
                        // load as i64 to avoid struct-type aliasing surprises.
                        llvm::Value* len64 =
                            builder_.CreateLoad(i64(), obj, "buf.len");
                        return builder_.CreateTrunc(len64, i32(), "buf.len.i32");
                    }
                }
            }
            // §4.14 / §9.8 / issue #550: OwningBuffer<T>.Count → bz_array_len.
            // OwningBuffer is array-backed (bz_array_t*) but is not in
            // isCollectionTypeName (which lists element-owning collections).
            {
                bool isOB = tt && tt->kind == sema::TypeRef::Kind::Named &&
                            !tt->declaration && tt->astNode && tt->astNode->name &&
                            !tt->astNode->name->parts.empty() &&
                            tt->headName() == "OwningBuffer";
                if (isOB) {
                    llvm::Value* obj = lowerExpr(*ma->target);
                    if (obj) {
                        auto* fn = declareC("bz_array_len", i64(), {ptrTy()});
                        llvm::Value* cnt64 = builder_.CreateCall(fn, {obj});
                        return builder_.CreateTrunc(cnt64, i32(), "ob.cnt");
                    }
                }
            }
            if (isCollectionTypeName(tt)) {
                // issues #343 / D8 / §4.10: a Set is dict-backed (records,
                // primitives, AND strings — see lowerNew), so its Count comes
                // from bz_dict_count, not the array length.
                bool isSetName = tt->astNode && tt->astNode->name &&
                                 !tt->astNode->name->parts.empty() &&
                                 (tt->headName() == "Set" ||
                                  tt->headName() == "OwningSet" ||
                                  // stdlib §4.18 (#1426): SortedSet<T> shares
                                  // the generic-key hash table (bz_dict_t*).
                                  tt->headName() == "SortedSet");
                bool dictBacked = isSetName &&
                    genericBackingType(tt, 0, /*isSet=*/true, nullptr) != nullptr;
                llvm::Value* obj = lowerExpr(*ma->target);
                if (obj) {
                    // §28.2 v0.135 / issue #627: checked-disposal guard for
                    // array-backed collection Count/Length property access.
                    // Skip for Set<T> (dict-backed); they use bz_dict_count.
                    if (checkedDisposal_ && !dictBacked) {
                        llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvm_);
                        auto* isFn = declareC("bz_array_is_disposed", i8Ty,
                                              {ptrTy()});
                        llvm::Value* flag =
                            builder_.CreateCall(isFn, {obj}, "cd.arrdis");
                        llvm::Value* disposed = builder_.CreateICmpNE(
                            flag, llvm::ConstantInt::get(i8Ty, 0), "cd.disposed");
                        emitGuardThrow(disposed, "DisposedAccessException");
                    }
                    auto* fn = dictBacked
                        ? declareC("bz_dict_count", i64(), {ptrTy()})
                        : declareC("bz_array_len", i64(), {ptrTy()});
                    llvm::Value* len64 = builder_.CreateCall(fn, {obj});
                    llvm::Value* col = builder_.CreateTrunc(len64, i32(),
                                                            "col.len");
                    // #1006(b): a `.Length`/`.Count` PROPERTY read is a borrow
                    // position — the scalar count is fully computed above, so an
                    // UNBOUND owned borrowing-array receiver (`P.Fold(s).Length`)
                    // leaves the temporary otherwise unreferenced and is reclaimed
                    // here. No-op unless the receiver is exactly that shape (a
                    // bound/`move`d/aliased receiver is never matched → no UAF).
                    freeUnboundOwnedBorrowingArrayTemp(ma->target.get(), obj, tt);
                    return col;
                }
            }
            // fix #853 / stdlib §4.1–§4.3: `.Count` on a LIST-INTERFACE receiver
            // (IReadOnlyList<T> / IList<T> / IOwningList<T>). These interfaces are
            // implemented ONLY by Array<T> / OwningArray<T> (both bz_array_t*-
            // backed), so an interface-typed receiver carries the same array
            // pointer as the concrete collection and `.Count` is bz_array_len —
            // the same bridge the indexer get/set already uses (#401). Previously
            // an interface-typed `.Count` fell through to a stub that returned 0
            // (#853). (The non-list collection interfaces — IReadOnlyCollection /
            // ICollection — are polymorphic over incompatible backings and are
            // diagnosed below, not bridged.)
            if (isListInterfaceName(tt)) {
                llvm::Value* obj = lowerExpr(*ma->target);
                if (obj) {
                    auto* fn = declareC("bz_array_len", i64(), {ptrTy()});
                    llvm::Value* len64 = builder_.CreateCall(fn, {obj});
                    return builder_.CreateTrunc(len64, i32(), "ilist.len");
                }
            }
            // fix #853 / stdlib §4.1–§4.3: `.Count` on a polymorphic COLLECTION-
            // INTERFACE receiver (IReadOnlyCollection<T> / ICollection<T> /
            // IOwningCollection<T>). Unlike the list interfaces, these are
            // implemented by collections with incompatible runtime
            // representations (Array → bz_array_t* count@8; LinkedList →
            // bz_llist_t* count@16; Dictionary/Set → bz_dict_t*) and the
            // interface-typed receiver erases which one it holds. With no shared
            // kind tag and no collection itable, no single runtime call is sound
            // here (bz_array_len would read garbage for a LinkedList/Dictionary).
            // Diagnose instead of silently returning 0 (the #853 miscompile).
            // A real fix needs a runtime foundation; deferred (#853).
            if (isCollectionInterfaceName(tt)) {
                ctx_.diagnostics().report(
                    Severity::Error, e.span.begin,
                    "`.Count` through a polymorphic collection interface "
                    "(IReadOnlyCollection<T> / ICollection<T> / "
                    "IOwningCollection<T>) is not yet supported in codegen "
                    "(stdlib \xC2\xA7""4.1\xE2\x80\x93\xC2\xA7""4.3, #853): these "
                    "interfaces are implemented by collections with incompatible "
                    "runtime representations (array / linked-list / hash-table) "
                    "and the concrete backing is erased at the interface, so no "
                    "single runtime dispatch is sound. Call `.Count` on the "
                    "concrete collection, or pass a list interface "
                    "(IReadOnlyList<T> / IList<T>) which is array-backed.");
                return llvm::PoisonValue::get(i32());
            }
            if (isDictTypeName(tt)) {
                llvm::Value* obj = lowerExpr(*ma->target);
                if (obj) {
                    // §28.2 v0.135 / issue #627: checked-disposal guard for
                    // dict Count property access.
                    if (checkedDisposal_) {
                        llvm::Type* i8Ty = llvm::Type::getInt8Ty(llvm_);
                        auto* isFn = declareC("bz_dict_is_disposed", i8Ty,
                                              {ptrTy()});
                        llvm::Value* flag =
                            builder_.CreateCall(isFn, {obj}, "cd.ddis");
                        llvm::Value* disposed = builder_.CreateICmpNE(
                            flag, llvm::ConstantInt::get(i8Ty, 0), "cd.disposed");
                        emitGuardThrow(disposed, "DisposedAccessException");
                    }
                    auto* fn = declareC("bz_dict_count", i64(), {ptrTy()});
                    llvm::Value* cnt64 = builder_.CreateCall(fn, {obj});
                    llvm::Value* cnt =
                        builder_.CreateTrunc(cnt64, i32(), "dict.cnt");
                    // #1028: a `.Count` PROPERTY read is a borrow position — the
                    // scalar count is fully computed above, so an UNBOUND fresh
                    // `owned Dictionary` materializer receiver
                    // (`src.ToDictionary(k).Count`) leaves the temporary otherwise
                    // unreferenced and leaks. Reclaim it here via bz_dict_free,
                    // mirroring the array-backed `.Count`/`.Length` path (#1006b
                    // above). No-op unless the receiver is exactly that shape (a
                    // bound/`move`d/aliased dict receiver is never matched → no
                    // UAF / double-free).
                    freeUnboundOwnedBorrowingArrayTemp(ma->target.get(), obj, tt);
                    return cnt;
                }
            }
        }
    }
    // fix #1426 / stdlib §4.18: SortedSet<T>.First (least) and SortedSet<T>.Last
    // (greatest) as MemberAccessExpr (no parens — stdlib properties, not methods).
    // The parser generates a MemberAccessExpr for zero-arg property-style access
    // that bypasses tryLowerCollectionInstanceCall (which handles CallExpr only).
    // fix #1426 / stdlib §4.17: SortedDictionary<K,V>.FirstKey / LastKey — same.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) {
            bool isSortedSetMember  = (ma->member == "First"    || ma->member == "Last");
            bool isSortedDictMember = (ma->member == "FirstKey" || ma->member == "LastKey");
            if (isSortedSetMember || isSortedDictMember) {
                auto* tt = ctx_.exprType(*ma->target);
                if (tt && !tt->declaration && tt->astNode && tt->astNode->name &&
                    !tt->astNode->name->parts.empty()) {
                    auto tyn = tt->headName();
                    bool isSortedSet  = isSortedSetMember  && tyn == "SortedSet";
                    bool isSortedDict = isSortedDictMember && tyn == "SortedDictionary";
                    if (isSortedSet || isSortedDict) {
                        // Determine the key TypeRef.
                        const sema::TypeRef* keyTy = nullptr;
                        bool isStringKey = false;
                        if (isSortedSet) {
                            // Element type (arg 0) — use the generic-backing path.
                            keyTy = genericBackingType(tt, 0, /*isSet=*/true, nullptr);
                        } else {
                            // SortedDictionary<K,V>: K is arg 0.
                            if (!tt->genericArgs.empty() && tt->genericArgs[0]) {
                                keyTy = tt->genericArgs[0];
                                isStringKey =
                                    keyTy->kind == sema::TypeRef::Kind::Primitive &&
                                    keyTy->primitive == sema::PrimitiveType::String &&
                                    !keyTy->nullable;
                            }
                        }
                        // fix #3865 (stdlib §4.18, v0.294): `keyTy` above is
                        // ALREADY gated by `genericBackingType` for the
                        // `isSortedSet` case, so null here means T does not
                        // satisfy `where T: IComparable<T>` — the PROPERTY-
                        // syntax (`s.First`) twin of the CALL-syntax
                        // (`s.First()`) crash/silent-degrade this issue's own
                        // sweep measured in `tryLowerCollectionInstanceCall`
                        // (LlvmBackend_Calls.cpp). Before this guard the block
                        // below was simply skipped (the `if (keyTy)` gate),
                        // and the caller (e.g. `lowerLocalDecl`) silently
                        // zero-degraded (#2434) with no diagnostic at all.
                        // `SortedDictionary` is NOT in scope here: its own
                        // `keyTy` (below) is the RAW generic argument, not
                        // eligibility-gated, so this specific null case never
                        // arises for it — its analogous defect is caught by
                        // the `!cmpFn` guard a few lines further down.
                        if (!keyTy && isSortedSet) {
                            const sema::TypeRef* rawElem =
                                (!tt->genericArgs.empty())
                                    ? resolveTypeParamRef(tt->genericArgs[0])
                                    : nullptr;
                            std::string elemName = "the element type";
                            if (rawElem &&
                                rawElem->kind == sema::TypeRef::Kind::Named &&
                                rawElem->declaration)
                                elemName = "`" +
                                    std::string(rawElem->declaration->name) + "`";
                            ctx_.diagnostics().report(
                                Severity::Error, ma->span.begin,
                                "`." + std::string(ma->member) +
                                "` on `SortedSet<T>` (stdlib \xC2\xA7""4.18) "
                                "requires the element type to implement "
                                "`IComparable<T>`; " + elemName + " does not "
                                "(it declares no dispatchable "
                                "single-parameter `CompareTo`, and is not "
                                "auto-hashable either).");
                            llvm::Type* resTy = i32();
                            if (sema::TypeRef* rt = ctx_.exprType(e))
                                if (llvm::Type* lt = lowerTypeRef(rt))
                                    resTy = lt;
                            return llvm::PoisonValue::get(resTy);
                        }
                        if (keyTy) {
                            llvm::Value* obj = lowerExpr(*ma->target);
                            if (obj) {
                                bool isMax = (ma->member == "Last" ||
                                             ma->member == "LastKey");
                                // Guard: empty → InvalidOperationException.
                                auto* cntFn = declareC("bz_dict_count", i64(),
                                                       {ptrTy()});
                                llvm::Value* cnt = builder_.CreateCall(
                                    cntFn, {obj}, "sp.cnt");
                                emitGuardThrow(
                                    builder_.CreateICmpEQ(cnt, builder_.getInt64(0)),
                                    "InvalidOperationException");
                                if (isStringKey) {
                                    // bz_dict_min/max_key_str fills data+len slots.
                                    // Build a heap bz_string from returned bytes
                                    // (same pattern as FirstKey/LastKey in
                                    // tryLowerCollectionInstanceCall / #1426).
                                    const char* rn = isMax ? "bz_dict_max_key_str"
                                                           : "bz_dict_min_key_str";
                                    auto* skFn = declareC(rn, i1(),
                                        {ptrTy(), ptrTy(), ptrTy()});
                                    llvm::AllocaInst* dSlot = entryAlloca(ptrTy());
                                    llvm::AllocaInst* lSlot = entryAlloca(i64());
                                    builder_.CreateStore(
                                        llvm::ConstantPointerNull::get(ptrTy()), dSlot);
                                    builder_.CreateStore(builder_.getInt64(0), lSlot);
                                    builder_.CreateCall(skFn, {obj, dSlot, lSlot});
                                    llvm::Value* kdata =
                                        builder_.CreateLoad(ptrTy(), dSlot, "sp.kd");
                                    llvm::Value* klen =
                                        builder_.CreateLoad(i64(), lSlot, "sp.kl");
                                    auto* i8Ty2 = llvm::Type::getInt8Ty(llvm_);
                                    uint64_t strSz2 = module_->getDataLayout()
                                        .getTypeAllocSize(stringType());
                                    auto* callocFn2 = declareC("calloc", ptrTy(),
                                        {i64(), i64()});
                                    llvm::Value* sObj = builder_.CreateCall(
                                        callocFn2, {builder_.getInt64(1),
                                                    builder_.getInt64((int64_t)strSz2)});
                                    builder_.CreateStore(kdata,
                                        builder_.CreateStructGEP(stringType(),sObj,0));
                                    builder_.CreateStore(klen,
                                        builder_.CreateStructGEP(stringType(),sObj,1));
                                    builder_.CreateStore(
                                        llvm::ConstantInt::get(i8Ty2, 1),
                                        builder_.CreateStructGEP(stringType(),sObj,2));
                                    initStringRc(sObj);
                                    return sObj;
                                }
                                // Generic key path.
                                llvm::Function* cmpFn =
                                    buildSortedKeyComparator(keyTy);
                                if (cmpFn) {
                                    const char* rn2 = isMax ? "bz_dict_max_key_gen"
                                                            : "bz_dict_min_key_gen";
                                    auto* mmFn = declareC(rn2, ptrTy(),
                                                          {ptrTy(), ptrTy()});
                                    llvm::Value* kPtr = builder_.CreateCall(
                                        mmFn, {obj, cmpFn}, "sp.mm");
                                    llvm::Type* kLlvm = lowerTypeRef(keyTy);
                                    if (kLlvm) {
                                        llvm::Value* kVal = builder_.CreateLoad(
                                            kLlvm, kPtr, "sp.kv");
                                        auto* ffn = declareC("free",
                                            llvm::Type::getVoidTy(llvm_),
                                            {ptrTy()});
                                        builder_.CreateCall(ffn, {kPtr});
                                        return kVal;
                                    }
                                }
                                // fix #3865 (stdlib §4.17/§4.18, v0.294): no
                                // comparator could be built for this key/
                                // element — either it has no `CompareTo` /
                                // `IComparable<T>` conformance at all (an
                                // illegal instantiation), or it is an
                                // AUTO-HASHABLE record with no `CompareTo`
                                // (hashable per §9.2 is NOT the same property
                                // as orderable per §4.17/§4.18 — measured:
                                // such a key/element was silently admitted by
                                // `genericBackingType`'s auto-hashable
                                // eligibility for `.Contains`/`.Add`, then
                                // `buildSortedKeyComparator` declined it here,
                                // degrading `.First`/`.FirstKey`/etc. to the
                                // #2434 zero-fill with no diagnostic at all).
                                // Report loudly rather than let the caller's
                                // local-decl silently degrade.
                                {
                                    const sema::TypeRef* rawElem =
                                        resolveTypeParamRef(keyTy);
                                    std::string elemName = "the element type";
                                    if (rawElem &&
                                        rawElem->kind ==
                                            sema::TypeRef::Kind::Named &&
                                        rawElem->declaration)
                                        elemName = "`" +
                                            std::string(
                                                rawElem->declaration->name) +
                                            "`";
                                    const char* slotWord =
                                        isSortedSet ? "element" : "key";
                                    const char* head =
                                        isSortedSet ? "SortedSet<T>"
                                                    : "SortedDictionary<K, V>";
                                    const char* sec =
                                        isSortedSet ? "\xC2\xA7""4.18"
                                                    : "\xC2\xA7""4.17";
                                    const char* tparam =
                                        isSortedSet ? "T" : "K";
                                    ctx_.diagnostics().report(
                                        Severity::Error, ma->span.begin,
                                        "`." + std::string(ma->member) +
                                        "` on `" + head + "` (stdlib " + sec +
                                        ") requires the " + slotWord +
                                        " type to implement `IComparable<" +
                                        tparam + ">`; " + elemName +
                                        " does not (it declares no "
                                        "dispatchable single-parameter "
                                        "`CompareTo`, and being auto-hashable "
                                        "is not sufficient — hashable and "
                                        "orderable are different properties).");
                                    llvm::Type* resTy = i32();
                                    if (sema::TypeRef* rt = ctx_.exprType(e))
                                        if (llvm::Type* lt = lowerTypeRef(rt))
                                            resTy = lt;
                                    return llvm::PoisonValue::get(resTy);
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    // stdlib §4.4/§4.5/§4.14 (#741): `.Capacity` on a `bz_array_t`-backed type
    // → bz_array_capacity. The spec declares `Capacity` only on Array/OwningArray
    // (and OwningBuffer exposes it as `Length`), but every bz_array_t-backed
    // collection has a valid backing capacity; the allowlist mirrors the
    // construction branch (LlvmBackend_Exprs.cpp ~L6004). LinkedList (bz_llist)
    // and Dictionary (bz_dict) use different backings and are NOT included. No
    // codegen case existed, so the read previously returned an uninitialized slot.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target && ma->member == "Capacity") {
            auto* tt = ctx_.exprType(*ma->target);
            auto headName = [&](const sema::TypeRef* t) -> std::string_view {
                return (t && t->astNode && t->astNode->name &&
                        !t->astNode->name->parts.empty())
                           ? t->headName() : std::string_view{};
            };
            std::string_view cn = headName(tt);
            bool isArrayBacked =
                cn == "Array" || cn == "OwningArray" || cn == "OwningBuffer" ||
                cn == "Stack" || cn == "OwningStack" ||
                cn == "Queue" || cn == "OwningQueue" ||
                cn == "Set"   || cn == "OwningSet";
            if (isArrayBacked) {
                llvm::Value* obj = lowerExpr(*ma->target);
                if (obj) {
                    auto* fn = declareC("bz_array_capacity", i64(), {ptrTy()});
                    llvm::Value* cap64 = builder_.CreateCall(fn, {obj});
                    return builder_.CreateTrunc(cap64, i32(), "col.cap");
                }
            }
        }
    }
    // per §4.6/§4.7: LinkedList<T>/OwningLinkedList<T> node-returning properties
    // (FirstNode, LastNode, First, Last) and LinkedListNode<T> accessor
    // properties (Value, Next, Previous).
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target) {
            auto* tt = ctx_.exprType(*ma->target);
            bool isLL = isLinkedListTypeName(tt);
            // LinkedList<T> node properties.
            if (isLL && (ma->member == "FirstNode" || ma->member == "LastNode" ||
                         ma->member == "First" || ma->member == "Last")) {
                llvm::Value* list = lowerExpr(*ma->target);
                if (!list) return nullptr;
                int64_t esz = collectionElemSize(tt);
                llvm::Type* elemLLTy = i32();
                if (tt && !tt->genericArgs.empty() && tt->genericArgs[0])
                    elemLLTy = lowerTypeRef(tt->genericArgs[0]);
                if (ma->member == "FirstNode" || ma->member == "LastNode") {
                    const char* fn = ma->member == "FirstNode"
                        ? "bz_llist_first_node" : "bz_llist_last_node";
                    auto* f = declareC(fn, ptrTy(), {ptrTy()});
                    return builder_.CreateCall(f, {list}, "ll.nodeptr");
                }
                // per §4.6 (stdlib L1264–1265): `First`/`Last` are typed `T?`.
                // The getter yields `null` when the list is empty (the node
                // pointer is null) and the element value otherwise. This null
                // representation is required for §7.11 lifting and `??` to
                // interoperate with the result — every consumer reads `l.First`
                // as the nullable form sema assigned it.
                {
                    const char* fn = ma->member == "First"
                        ? "bz_llist_first_node" : "bz_llist_last_node";
                    auto* nodeF = declareC(fn, ptrTy(), {ptrTy()});
                    llvm::Value* nodePtr = builder_.CreateCall(nodeF, {list}, "ll.fnode");
                    llvm::Value* nodeNull = builder_.CreateICmpEQ(
                        nodePtr, llvm::ConstantPointerNull::get(ptrTy()),
                        "ll.empty");
                    const sema::TypeRef* resTy = ctx_.exprType(e);
                    // For a NULLABLE VALUE element type (int?, double?, …) the
                    // result is the tagged {i8 hasValue; T value} struct, built
                    // only on the present branch so the value load never runs on
                    // an empty list (a null `valPtr`).
                    if (resTy && isNullableValueTy(resTy)) {
                        auto* entryBB = builder_.GetInsertBlock();
                        auto* presentBB =
                            llvm::BasicBlock::Create(llvm_, "ll.first.val", currentFn_);
                        auto* mergeBB =
                            llvm::BasicBlock::Create(llvm_, "ll.first.end", currentFn_);
                        auto* st = nullableValueStructTy(resTy);
                        builder_.CreateCondBr(nodeNull, mergeBB, presentBB);
                        builder_.SetInsertPoint(presentBB);
                        auto* valF = declareC("bz_llist_node_value_ptr",
                                              ptrTy(), {ptrTy()});
                        llvm::Value* valPtr =
                            builder_.CreateCall(valF, {nodePtr}, "ll.vptr");
                        llvm::Value* loaded =
                            builder_.CreateLoad(elemLLTy, valPtr, "ll.val");
                        llvm::Value* wrapped = wrapNullableValue(resTy, loaded);
                        auto* presentEndBB = builder_.GetInsertBlock();
                        builder_.CreateBr(mergeBB);
                        builder_.SetInsertPoint(mergeBB);
                        auto* phi = builder_.CreatePHI(st, 2, "ll.first");
                        phi->addIncoming(nullableValueNull(resTy), entryBB);
                        phi->addIncoming(wrapped, presentEndBB);
                        return phi;
                    }
                    // For a reference (class/string/interface) element type, T?
                    // is a nullable pointer: the node-value slot holds the
                    // pointer; an empty list yields a null pointer directly.
                    auto* valF = declareC("bz_llist_node_value_ptr",
                                          ptrTy(), {ptrTy()});
                    llvm::Value* valPtr =
                        builder_.CreateCall(valF, {nodePtr}, "ll.vptr");
                    if (elemLLTy->isPointerTy()) {
                        // Read the slot only when the node is present; an empty
                        // list returns a null reference.
                        auto* entryBB = builder_.GetInsertBlock();
                        auto* presentBB =
                            llvm::BasicBlock::Create(llvm_, "ll.first.ref", currentFn_);
                        auto* mergeBB =
                            llvm::BasicBlock::Create(llvm_, "ll.first.refend", currentFn_);
                        builder_.CreateCondBr(nodeNull, mergeBB, presentBB);
                        builder_.SetInsertPoint(presentBB);
                        llvm::Value* loaded =
                            builder_.CreateLoad(elemLLTy, valPtr, "ll.refval");
                        auto* presentEndBB = builder_.GetInsertBlock();
                        builder_.CreateBr(mergeBB);
                        builder_.SetInsertPoint(mergeBB);
                        auto* phi = builder_.CreatePHI(elemLLTy, 2, "ll.first.ref");
                        phi->addIncoming(
                            llvm::ConstantPointerNull::get(
                                llvm::cast<llvm::PointerType>(elemLLTy)),
                            entryBB);
                        phi->addIncoming(loaded, presentEndBB);
                        return phi;
                    }
                    // Fallback (no resolved nullable result type): load the raw
                    // element value as before.
                    return builder_.CreateLoad(elemLLTy, valPtr, "ll.val");
                }
            }
            // LinkedListNode<T> accessor properties.
            bool isNode = (tt && tt->kind == sema::TypeRef::Kind::Named &&
                           !tt->declaration && tt->astNode && tt->astNode->name &&
                           !tt->astNode->name->parts.empty() &&
                           tt->headName() == "LinkedListNode");
            if (isNode) {
                llvm::Value* nodePtr = lowerExpr(*ma->target);
                if (!nodePtr) return nullptr;
                if (ma->member == "Value") {
                    // elem type is the first generic arg.
                    llvm::Type* elemLLTy2 = ptrTy();
                    if (tt && !tt->genericArgs.empty() && tt->genericArgs[0])
                        elemLLTy2 = lowerTypeRef(tt->genericArgs[0]);
                    auto* valF = declareC("bz_llist_node_value_ptr", ptrTy(), {ptrTy()});
                    llvm::Value* vp = builder_.CreateCall(valF, {nodePtr}, "nd.vptr");
                    return builder_.CreateLoad(elemLLTy2, vp, "nd.val");
                }
                if (ma->member == "Next") {
                    auto* f = declareC("bz_llist_node_next", ptrTy(), {ptrTy()});
                    return builder_.CreateCall(f, {nodePtr}, "nd.next");
                }
                if (ma->member == "Previous") {
                    auto* f = declareC("bz_llist_node_prev", ptrTy(), {ptrTy()});
                    return builder_.CreateCall(f, {nodePtr}, "nd.prev");
                }
            }
        }
    }
    // fix #571 / §4.8: Dictionary<K,V>.Keys — property returning Array<K> of
    // all keys.  Previously this always called bz_dict_keys_str, which reads the
    // string-key table and returns garbage/crash for non-string (generic-table)
    // keys (int, enum, record).  Now dispatches like lowerDictKeysFor (Stmts.cpp):
    // generic-table keys (K != string) → bz_dict_keys_gen path; string keys →
    // existing bz_dict_keys_str path.  The for-in handler (lowerDictKeysFor) is
    // the correctness reference for both paths.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target && ma->member == "Keys") {
            auto* tt = ctx_.exprType(*ma->target);
            if (isDictTypeName(tt)) {
                llvm::Value* dictObj = lowerExpr(*ma->target);
                if (!dictObj) return nullptr;

                // Determine key type and whether it routes through the generic table.
                const sema::TypeRef* genKeyTy =
                    genericBackingType(tt, 0, /*isSet=*/false, nullptr);

                if (genKeyTy) {
                    // fix #571 / §4.8: non-string key — use bz_dict_keys_gen.
                    // bz_dict_keys_gen returns a malloc'd void** array of `n`
                    // pointers, each a malloc'd copy of one key's raw bytes.
                    // Load each key with genKeyTy's LLVM type, store into the
                    // result Array<K> — mirrors emitGenericKeyTableFor (Stmts.cpp)
                    // and the Values expression handler above.
                    llvm::Type* keyLlvm = lowerTypeRef(genKeyTy);
                    if (!keyLlvm) return nullptr;
                    int64_t esz = collectionElemSize(ctx_.exprType(e));

                    auto* keysOutSlot = entryAlloca(ptrTy());
                    auto* cntSlot     = entryAlloca(i64());
                    auto* keysFn = declareC("bz_dict_keys_gen",
                                            llvm::Type::getVoidTy(llvm_),
                                            {ptrTy(), ptrTy(), ptrTy()});
                    builder_.CreateCall(keysFn, {dictObj, keysOutSlot, cntSlot});
                    llvm::Value* keysArr = builder_.CreateLoad(ptrTy(), keysOutSlot);
                    llvm::Value* cnt     = builder_.CreateLoad(i64(), cntSlot);
                    auto* freeFn = declareC("free",
                                            llvm::Type::getVoidTy(llvm_), {ptrTy()});
                    auto* allocFn = declareC("bz_array_alloc", ptrTy(), {i64(), i64()});
                    llvm::Value* result =
                        builder_.CreateCall(allocFn,
                                            {builder_.getInt64(esz), cnt}, "dk.arr");
                    auto* addFn = declareC("bz_array_add",
                                           llvm::Type::getVoidTy(llvm_),
                                           {ptrTy(), ptrTy(), i64()});

                    auto* idxSlot = entryAlloca(i64());
                    builder_.CreateStore(builder_.getInt64(0), idxSlot);
                    auto* condBB = llvm::BasicBlock::Create(llvm_, "dk2.c", currentFn_);
                    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "dk2.b", currentFn_);
                    auto* endBB  = llvm::BasicBlock::Create(llvm_, "dk2.e", currentFn_);
                    builder_.CreateBr(condBB);

                    builder_.SetInsertPoint(condBB);
                    llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
                    builder_.CreateCondBr(
                        builder_.CreateICmpSLT(idx, cnt), bodyBB, endBB);

                    builder_.SetInsertPoint(bodyBB);
                    llvm::Value* idxB    = builder_.CreateLoad(i64(), idxSlot);
                    llvm::Value* kpPtr   = builder_.CreateGEP(ptrTy(), keysArr, idxB);
                    llvm::Value* keyPtr  = builder_.CreateLoad(ptrTy(), kpPtr);
                    // Load the key value from its raw-bytes copy (mirrors
                    // emitGenericKeyTableFor: CreateLoad(elemTy, keyPtr)).
                    llvm::Value* keyVal  = builder_.CreateLoad(keyLlvm, keyPtr,
                                                               "dk2.kv");
                    llvm::AllocaInst* keySlot = entryAlloca(keyLlvm);
                    builder_.CreateStore(keyVal, keySlot);
                    builder_.CreateCall(addFn,
                                        {result, keySlot, builder_.getInt64(esz)});
                    // Free this iteration's key-bytes copy (mirrors lowerDictKeysFor
                    // and emitGenericKeyTableFor).
                    builder_.CreateCall(freeFn, {keyPtr});
                    builder_.CreateStore(
                        builder_.CreateAdd(idxB, builder_.getInt64(1)), idxSlot);
                    builder_.CreateBr(condBB);

                    builder_.SetInsertPoint(endBB);
                    builder_.CreateCall(freeFn, {keysArr});
                    return result;
                }

                // String keys: existing bz_dict_keys_str path (unchanged).
                auto* keysOutSlot = entryAlloca(ptrTy());
                auto* lensOutSlot = entryAlloca(ptrTy());
                auto* cntSlot2 = entryAlloca(i64());
                auto* keysFn2 = declareC("bz_dict_keys_str", llvm::Type::getVoidTy(llvm_),
                    {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(keysFn2, {dictObj, keysOutSlot, lensOutSlot, cntSlot2});
                llvm::Value* keysArr = builder_.CreateLoad(ptrTy(), keysOutSlot);
                llvm::Value* lensArr = builder_.CreateLoad(ptrTy(), lensOutSlot);
                llvm::Value* cnt2 = builder_.CreateLoad(i64(), cntSlot2);
                auto* allocFn2 = declareC("bz_array_alloc", ptrTy(), {i64(), i64()});
                llvm::Value* result2 = builder_.CreateCall(allocFn2,
                    {builder_.getInt64(8), cnt2});
                auto* addFn2 = declareC("bz_array_add", llvm::Type::getVoidTy(llvm_),
                                        {ptrTy(), ptrTy(), i64()});
                auto* mallocFn2 = declareC("malloc", ptrTy(), {i64()});
                auto* memcpyFn2 = declareC("memcpy", ptrTy(), {ptrTy(), ptrTy(), i64()});
                auto* callocFn3 = declareC("calloc", ptrTy(), {i64(), i64()});
                auto* i8Ty2 = llvm::Type::getInt8Ty(llvm_);
                uint64_t strSz2 = module_->getDataLayout().getTypeAllocSize(stringType());
                auto* idxSlot3 = entryAlloca(i64());
                builder_.CreateStore(builder_.getInt64(0), idxSlot3);
                auto* condBB2b = llvm::BasicBlock::Create(llvm_, "ks2.c", currentFn_);
                auto* bodyBB2b = llvm::BasicBlock::Create(llvm_, "ks2.b", currentFn_);
                auto* endBB2b  = llvm::BasicBlock::Create(llvm_, "ks2.e", currentFn_);
                builder_.CreateBr(condBB2b);
                builder_.SetInsertPoint(condBB2b);
                llvm::Value* idx3 = builder_.CreateLoad(i64(), idxSlot3);
                builder_.CreateCondBr(builder_.CreateICmpSLT(idx3, cnt2), bodyBB2b, endBB2b);
                builder_.SetInsertPoint(bodyBB2b);
                llvm::Value* kpPtr2 = builder_.CreateGEP(ptrTy(), keysArr, idx3);
                llvm::Value* kp2 = builder_.CreateLoad(ptrTy(), kpPtr2);
                llvm::Value* lpPtr2 = builder_.CreateGEP(i64(), lensArr, idx3);
                llvm::Value* kl3 = builder_.CreateLoad(i64(), lpPtr2);
                llvm::Value* dataSz3 = builder_.CreateAdd(kl3, builder_.getInt64(1));
                llvm::Value* dataCopy3 = builder_.CreateCall(mallocFn2, {dataSz3});
                builder_.CreateCall(memcpyFn2, {dataCopy3, kp2, kl3});
                llvm::Value* nulPtr3 = builder_.CreateGEP(i8Ty2, dataCopy3, kl3);
                builder_.CreateStore(llvm::ConstantInt::get(i8Ty2, 0), nulPtr3);
                llvm::Value* strObj3 = builder_.CreateCall(callocFn3,
                    {builder_.getInt64(1), builder_.getInt64(strSz2)});
                builder_.CreateStore(dataCopy3, builder_.CreateStructGEP(stringType(), strObj3, 0));
                builder_.CreateStore(kl3, builder_.CreateStructGEP(stringType(), strObj3, 1));
                builder_.CreateStore(llvm::ConstantInt::get(i8Ty2, 1),
                    builder_.CreateStructGEP(stringType(), strObj3, 2));
                    initStringRc(strObj3);  // #422: producer's reference, rc := 1
                auto* slotK2 = entryAlloca(ptrTy());
                builder_.CreateStore(strObj3, slotK2);
                builder_.CreateCall(addFn2, {result2, slotK2, builder_.getInt64(8)});
                builder_.CreateStore(builder_.CreateAdd(idx3, builder_.getInt64(1)), idxSlot3);
                builder_.CreateBr(condBB2b);
                builder_.SetInsertPoint(endBB2b);
                return result2;
            }
        }
    }
    // fix #556 / fix #563 / §4.8: `Dictionary<K,V>.Values` as an expression —
    // returns a fresh bz_array_t* of decoded values.  Mirrors the Keys handler
    // above but uses genericArgs[1] (the value type) and the bz_dict_values_*
    // snapshots.  The element decoding follows the same void*-encoding rules as
    // lowerDictValuesFor (LlvmBackend_Stmts.cpp): record → load the struct from
    // the heap box (fix #563); nullable value type → unbox tagged struct;
    // scalar → ptrtoint + trunc; pointer (string / class) → void* IS the value.
    // fix #563: the old code stored the raw box pointer (ptrTy()) into elemSlot
    // and called bz_array_add with the struct's byte-count; bz_array_add then
    // copied struct-size bytes out of an 8-byte pointer slot → garbage.  The
    // correct decode mirrors lowerCollectionIndexerGet §4.8: load the struct
    // bytes out of the box (CreateLoad(valLlvm, slotV)) and put the struct value
    // into a properly-typed elemSlot so bz_array_add copies the real bytes.
    // The resulting bz_array_t* is typed as Array<V> by sema (fix #556), so
    // collectionElemSize will compute the correct per-element stride and
    // downstream .Count / .ToArray() / LINQ will work correctly.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->target && ma->member == "Values") {
            auto* dictTy = ctx_.exprType(*ma->target);
            if (isDictTypeName(dictTy) &&
                dictTy->genericArgs.size() >= 2 &&
                dictTy->genericArgs[1]) {
                const sema::TypeRef* valTy = dictTy->genericArgs[1];
                llvm::Type* valLlvm = lowerTypeRef(valTy);
                if (valLlvm) {
                    // Determine value category (matches lowerDictValuesFor).
                    const sema::TypeSymbol* valRec =
                        (valTy->kind == sema::TypeRef::Kind::Named &&
                         valTy->declaration &&
                         valTy->declaration->kind == ast::TypeDeclKind::Record)
                            ? valTy->declaration : nullptr;
                    bool valNullableVal = isNullableValueTy(valTy);

                    llvm::Value* dictObj = lowerExpr(*ma->target);
                    if (!dictObj) return nullptr;

                    // Snapshot the values array via the string-key or generic-key
                    // helper (same dispatch as lowerDictValuesFor).
                    auto* valsOutSlot = entryAlloca(ptrTy());
                    auto* cntSlot     = entryAlloca(i64());
                    bool useGen =
                        genericBackingType(dictTy, 0, /*isSet=*/false, nullptr)
                        != nullptr;
                    if (useGen) {
                        auto* fn = declareC("bz_dict_values_gen",
                                            llvm::Type::getVoidTy(llvm_),
                                            {ptrTy(), ptrTy(), ptrTy()});
                        builder_.CreateCall(fn, {dictObj, valsOutSlot, cntSlot});
                    } else {
                        auto* fn = declareC("bz_dict_values_str",
                                            llvm::Type::getVoidTy(llvm_),
                                            {ptrTy(), ptrTy(), ptrTy()});
                        builder_.CreateCall(fn, {dictObj, valsOutSlot, cntSlot});
                    }
                    llvm::Value* valsArr =
                        builder_.CreateLoad(ptrTy(), valsOutSlot);
                    llvm::Value* cnt = builder_.CreateLoad(i64(), cntSlot);

                    // Allocate the result array with the correct element stride.
                    int64_t esz = collectionElemSize(
                        ctx_.exprType(e));  // exprType(e) is the Array<V> TypeRef
                    auto* allocFn = declareC("bz_array_alloc", ptrTy(),
                                             {i64(), i64()});
                    llvm::Value* result =
                        builder_.CreateCall(allocFn,
                                            {builder_.getInt64(esz), cnt},
                                            "dv.arr");
                    auto* addFn = declareC("bz_array_add",
                                           llvm::Type::getVoidTy(llvm_),
                                           {ptrTy(), ptrTy(), i64()});
                    auto* freeFn = declareC("free",
                                            llvm::Type::getVoidTy(llvm_),
                                            {ptrTy()});

                    // Loop over the snapshot, decode each void* → V, add to result.
                    auto* idxSlot = entryAlloca(i64());
                    builder_.CreateStore(builder_.getInt64(0), idxSlot);
                    auto* condBB = llvm::BasicBlock::Create(llvm_, "dve.c",
                                                            currentFn_);
                    auto* bodyBB = llvm::BasicBlock::Create(llvm_, "dve.b",
                                                            currentFn_);
                    auto* endBB  = llvm::BasicBlock::Create(llvm_, "dve.e",
                                                            currentFn_);
                    builder_.CreateBr(condBB);

                    builder_.SetInsertPoint(condBB);
                    llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
                    builder_.CreateCondBr(
                        builder_.CreateICmpSLT(idx, cnt), bodyBB, endBB);

                    builder_.SetInsertPoint(bodyBB);
                    llvm::Value* idxB  = builder_.CreateLoad(i64(), idxSlot);
                    llvm::Value* vpPtr = builder_.CreateGEP(ptrTy(), valsArr,
                                                            idxB);
                    llvm::Value* slotV = builder_.CreateLoad(ptrTy(), vpPtr);

                    // Decode void* → V (same encoding as lowerDictValuesFor,
                    // fixed by #563 to match lowerCollectionIndexerGet §4.8).
                    llvm::Value* decoded;
                    if (valRec) {
                        // fix #563 / §4.8: load the record struct from the
                        // heap box.  The runtime boxes records (malloc+memcpy)
                        // so they fit in the void* slot; decode by loading the
                        // struct type back out of the box pointer — the same
                        // path the direct dict indexer get uses.
                        decoded = builder_.CreateLoad(valLlvm, slotV, "dve.rec");
                    } else if (valNullableVal && valLlvm->isStructTy()) {
                        decoded = builder_.CreateLoad(valLlvm, slotV,
                                                      "dve.nvval");
                    } else if (!valLlvm->isPointerTy()) {
                        llvm::Value* asI64 =
                            builder_.CreatePtrToInt(slotV, i64());
                        if (valLlvm->isDoubleTy())
                            decoded = builder_.CreateBitCast(asI64, valLlvm);
                        else if (valLlvm->isFloatTy())
                            decoded = builder_.CreateBitCast(
                                builder_.CreateTrunc(asI64, i32()), valLlvm);
                        else if (valLlvm->isIntegerTy(64))
                            decoded = asI64;
                        else
                            decoded = builder_.CreateTrunc(asI64, valLlvm);
                    } else {
                        decoded = slotV;  // pointer (string / class object)
                    }

                    // Store the decoded element into a stack slot and add to
                    // array.  fix #563: elemSlot must be typed as decoded's
                    // type (always decoded->getType() now that records produce
                    // a struct value from the load above, not a ptrTy()).
                    // bz_array_add copies esz bytes from elemSlot; the slot
                    // must hold the actual struct bytes, not a pointer to them.
                    llvm::AllocaInst* elemSlot = entryAlloca(decoded->getType());
                    builder_.CreateStore(decoded, elemSlot);
                    builder_.CreateCall(addFn,
                                        {result, elemSlot,
                                         builder_.getInt64(esz)});

                    builder_.CreateStore(
                        builder_.CreateAdd(idxB, builder_.getInt64(1)),
                        idxSlot);
                    builder_.CreateBr(condBB);

                    builder_.SetInsertPoint(endBB);
                    builder_.CreateCall(freeFn, {valsArr});
                    return result;
                }
            }
        }
    }
    // CG22+: Exception.Message — built-in exceptions store a message string
    // pointer at byte offset 8 (struct slot 1) of the exception object.
    //
    // #595: user-defined classes that directly inherit a built-in exception
    // (e.g. `class ErrA: Exception`) embed the same message-ptr at struct
    // slot 1 (byte offset 8) as part of the inherited layout inserted by
    // classStruct.  Both cases use the same GEP-at-byte-8 path; the
    // recognition is extended to cover the user-class case via
    // hasBuiltinExceptionBase.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        // §31.4: ExceptionInfo.TypeName/.Message/.StackTrace — a 24-byte record
        // field, NOT a built-in-exception object. Intercept before the Message
        // handler below, whose isBuiltinExceptionType (declaration == null) would
        // otherwise mis-treat the ExceptionInfo value as an exception object and
        // GEP offset 8 on a struct value (malformed IR).
        if (ma->target &&
            (ma->member == "TypeName" || ma->member == "Message" ||
             ma->member == "StackTrace")) {
            const sema::TypeRef* rtt = ctx_.exprType(*ma->target);
            bool isExcInfo = rtt && rtt->kind == sema::TypeRef::Kind::Named &&
                !rtt->declaration &&
                ((rtt->importedName == "ExceptionInfo") ||
                 (rtt->astNode && rtt->astNode->name &&
                  !rtt->astNode->name->parts.empty() &&
                  rtt->astNode->name->parts.back() == "ExceptionInfo"));
            if (isExcInfo) {
                unsigned fieldIdx = ma->member == "TypeName" ? 0
                                  : ma->member == "Message"  ? 1 : 2;
                return lowerExceptionInfoField(*ma->target, fieldIdx);
            }
        }
        if (ma->target && ma->member == "Message") {
            auto* tt = ctx_.exprType(*ma->target);
            // Case A: receiver is a built-in exception TypeRef (no user declaration).
            bool isBuiltinEx = tt && isBuiltinExceptionType(tt);
            // Case B: receiver is a user class that directly inherits a built-in
            // exception — it embeds the message slot at the same byte offset 8.
            bool isUserDerivedEx = false;
            if (!isBuiltinEx && tt &&
                tt->kind == sema::TypeRef::Kind::Named && tt->declaration)
                isUserDerivedEx = hasBuiltinExceptionBase(tt->declaration);
            // fix #2972 (§31.4 / §12.3 / §11 / §21.7, v0.240): Case B seen from
            // INSIDE the subclass — `this.Message` (and `base.Message`, when the
            // class base is itself a user exception subclass, which is the only
            // spelling of `base` sema admits here today).
            //
            // Both tests above start `tt && …`, and `tt` is null for a `this` /
            // `base` receiver: sema deliberately never types that identifier in
            // a named-type body (see `isThisOrBaseExpr`). So this whole handler
            // was skipped, the access fell through every remaining arm of
            // lowerExpr, and the caller substituted a degrade. MEASURED on
            // 618c0c8 across five frames — method, constructor, destructor,
            // property accessor, lambda — every one of them broken, and the
            // first three showing three DIFFERENT outcomes from the SAME
            // missing receiver type:
            //   * method body   — `return this.Message;` printed
            //     "internal: return-value expression failed to lower … #2491"
            //     and returned a NULL string; the caller SIGSEGV'd on it
            //     (exit 139; under ASan "SEGV on unknown address 0x…08 …
            //     address points to the zero page").
            //   * constructor body — `_n := this.Message.Length;` compiled with
            //     NO diagnostic at all and stored 0 (the message was "abcd").
            //   * destructor body — `Console.WriteLine("…" + this.Message);`
            //     compiled with NO diagnostic and the whole statement vanished.
            // (The property accessor lost its access site's whole statement,
            // also silently; the lambda returned 0.)
            // The UNQUALIFIED spelling (`Message`, #625) works in all of them:
            // its own bare-identifier arm keys on `currentThis_`, never on a
            // receiver type. §11 puts no restriction on `this.`, and §12.3
            // makes `base.` the ordinary way to reach a base implementation, so
            // all three spellings must read the one slot.
            //
            // THE GATE, and why it reads SEMA'S RESOLUTION rather than
            // re-deriving the receiver type here.
            //
            // What has to be true for the byte-8 read below to be correct is
            // that the object `this` denotes carries the §31.4 header — which
            // `classStruct` inserts for exactly the classes
            // `hasBuiltinExceptionBase` accepts, i.e. `builtinExceptionBaseOf`,
            // the ONE walk #2950 established. Sema's own arm for this access
            // (`ExprTC::resolveMemberAccess`'s `isBuiltinExInheritedMember`)
            // asks that SAME function, of the LEXICALLY ENCLOSING type — which
            // is precisely the type of `this`. So sema having resolved the
            // access through that arm IS the property, already decided by the
            // same predicate the layout gate uses; the two cannot diverge.
            //
            // That arm is observable here as: the access is typed `string` and
            // carries NO member symbol. A class that declares its own `Message`
            // member instead resolves to that member's FieldSymbol /
            // PropertySymbol and is excluded. A class with no `Message` at all
            // is rejected by sema before codegen runs. (#2984 has since made
            // such a declaration a §12.2 hiding error when the class DOES
            // derive from a built-in exception, so that exclusion is now
            // reachable only on a class that does not — which is exactly the
            // case it must still get right.)
            //
            // Re-deriving the enclosing type in codegen was written first and
            // ABANDONED, because there is no premise-free way to do it: the
            // recovery available (reverse-look `currentFn_` in the emitters'
            // memo caches, as `currentReceiverRecordType` / `choiceTypeOf` do)
            // rests on "currentFn_'s owner is the type of currentThis_", and
            // that is FALSE wherever an accessor body is INLINED at its access
            // site — `currentThis_` is re-pointed at the receiver while
            // `currentFn_` still belongs to the enclosing method. It also
            // covered no lambda body. Keying on sema's resolution is total over
            // frames — method, constructor, destructor, accessor (inlined or
            // not), lambda — with no frame enumeration at all.
            if (!isBuiltinEx && !isUserDerivedEx && !tt &&
                    isThisOrBaseExpr(*ma->target)) {
                const sema::TypeRef* accessTy = ctx_.exprType(*ma);
                isUserDerivedEx =
                    accessTy &&
                    accessTy->kind == sema::TypeRef::Kind::Primitive &&
                    accessTy->primitive == sema::PrimitiveType::String &&
                    ctx_.exprSymbol(*ma) == nullptr;
            }
            // #1266 / stdlib §2.17 / §31.4: `ex.InnerException!.Message` (a DIRECT
            // chain). sema does not type `ex.InnerException` (the builtin
            // TypeInitializationException carries no PropertyDecl for it), so `tt`
            // is null here and the two checks above miss — the access then fell to
            // a generic getter that returned the empty string. Recognise the chain
            // structurally: unwrap the postfix `!` and check the target is an
            // `InnerException` access on a TypeInitializationException. lowering
            // `ma->target` runs the InnerException handler below (offset 16 → the
            // inner exception ptr), whose message sits at offset 8 like any builtin
            // exception. (The local-bind form `var ie: Exception? := ex.Inner…;
            // ie!.Message` already works because the explicit type restores `tt`.)
            bool isInnerExcChain = false;
            if (!isBuiltinEx && !isUserDerivedEx) {
                const ast::Expr* t = ma->target.get();
                if (auto* pb = dynamic_cast<const ast::PostfixBangExpr*>(t))
                    t = pb->operand.get();
                if (auto* inner = dynamic_cast<const ast::MemberAccessExpr*>(t)) {
                    if (inner->member == "InnerException" && inner->target) {
                        auto* itt = ctx_.exprType(*inner->target);
                        isInnerExcChain =
                            itt && itt->kind == sema::TypeRef::Kind::Named &&
                            ((itt->declaration &&
                              itt->declaration->name ==
                                  "TypeInitializationException") ||
                             (!itt->declaration && itt->astNode &&
                              itt->astNode->name &&
                              !itt->astNode->name->parts.empty() &&
                              itt->headName() ==
                                  "TypeInitializationException"));
                    }
                }
            }
            if (isBuiltinEx || isUserDerivedEx || isInnerExcChain) {
                llvm::Value* excObj = lowerExpr(*ma->target);
                if (!excObj) return nullptr;
                // The message ptr is always at byte offset 8 — slot 1 of the
                // raw built-in layout AND slot 1 of the user-class struct
                // (classStruct inserts it before the disposed flag).
                auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                llvm::Value* msgSlot = builder_.CreateGEP(
                    i8Ty, excObj, builder_.getInt64(8));
                llvm::Value* msgPtr = builder_.CreateLoad(ptrTy(), msgSlot);
                // If null (no message given), return empty string constant.
                auto* isNull = builder_.CreateICmpEQ(
                    msgPtr,
                    llvm::ConstantPointerNull::get(ptrTy()));
                llvm::Value* emptyStr = lowerStringLiteral("\"\"", false);
                return builder_.CreateSelect(isNull, emptyStr, msgPtr);
            }
        }
        // §31.4 (v0.162/v0.163): the built-in Exception surface beyond Message.
        //  * StackTrace — the "" stub (no debug info yet).
        //  * CauseChain — the #811 snapshot at offset kExcCauseOffset (a
        //    bz_array_t* of ExceptionInfo; null → empty, safe: bz_array_len(null)
        //    == 0 and any index is out of bounds).
        //  * ParamName  — offset kExcParamOffset (bz_string*; null when absent).
        //  * ExceptionInfo.TypeName/.Message/.StackTrace — fields 0/1/2 of the
        //    24-byte record element.
        //
        // fix #2971 (§31.4 / §12.1 / §11.3, v0.240): the receiver test was
        // `isBuiltinExceptionTypeRef(rtt)`, i.e. a receiver STATICALLY TYPED as
        // a built-in. That was in lockstep with sema, which admitted only
        // `Message` on a user subclass — so a subclass exposed one quarter of
        // its own inherited §31.4 surface. Now that sema admits all four, these
        // readers must reach the same three receiver shapes sema does, or the
        // sema half alone would produce #2972's failure mode: accepted at
        // compile time, degraded to a null/zero at run time. See
        // `isInheritedExcSurfaceAccess` for the shapes and why the header GEPs
        // are correct on each (the header is at the SAME byte offsets in a
        // user subclass — `classStruct` mirrors all four slots).
        if (ma->target) {
            auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
            if (ma->member == "StackTrace" && isInheritedExcSurfaceAccess(ma)) {
                // §31.4 `StackTrace: string` — "formatted; "" when
                // unavailable". No debug info yet, so the "" stub, exactly as
                // for a built-in receiver. (The receiver is deliberately NOT
                // lowered, matching the pre-#2971 behaviour for built-ins.)
                return lowerStringLiteral("\"\"", false);
            }
            if (ma->member == "CauseChain" && isInheritedExcSurfaceAccess(ma)) {
                llvm::Value* excObj = lowerExpr(*ma->target);
                if (!excObj) return nullptr;
                llvm::Value* ccSlot = builder_.CreateGEP(
                    i8Ty, excObj, builder_.getInt64(kExcCauseOffset));
                return builder_.CreateLoad(ptrTy(), ccSlot, "exc.causechain");
            }
            if (ma->member == "ParamName" && isInheritedExcSurfaceAccess(ma)) {
                llvm::Value* excObj = lowerExpr(*ma->target);
                if (!excObj) return nullptr;
                llvm::Value* pnSlot = builder_.CreateGEP(
                    i8Ty, excObj, builder_.getInt64(kExcParamOffset));
                return builder_.CreateLoad(ptrTy(), pnSlot, "exc.paramname");
            }
        }
        // stdlib §2.17 / §31.4 (#1107/#11-13): `ex.InnerException` on a
        // `TypeInitializationException` reads the wrapped original exception
        // stored at byte offset 16 of the 24-byte object (the static-init-failure
        // wrapper stores it there; LlvmBackend_Setup.cpp). It is the only
        // built-in with surface beyond `Exception` (stdlib §2.17) — `Message`
        // sits at offset 8, the inner exception ptr at offset 16. Returns the
        // inner exception pointer (nullable `Exception?`). Without this the
        // access fell to a generic property getter that returned null, so
        // `inner is not null` / `inner is ArgumentException` were false and
        // binding the result then testing `is`/`as` dereferenced null (SIGSEGV).
        if (ma->target && ma->member == "InnerException") {
            auto* tt = ctx_.exprType(*ma->target);
            bool isTIE =
                tt && tt->kind == sema::TypeRef::Kind::Named &&
                ((tt->declaration &&
                  tt->declaration->name == "TypeInitializationException") ||
                 (!tt->declaration && tt->astNode && tt->astNode->name &&
                  !tt->astNode->name->parts.empty() &&
                  tt->headName() ==
                      "TypeInitializationException"));
            if (isTIE) {
                llvm::Value* excObj = lowerExpr(*ma->target);
                if (!excObj) return nullptr;
                auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                llvm::Value* innerSlot = builder_.CreateGEP(
                    i8Ty, excObj, builder_.getInt64(16));
                return builder_.CreateLoad(ptrTy(), innerSlot);
            }
        }
    }
    // issue #494 / §29.2 / §29.3: explicit `e.Current` property access where
    // `e` is typed as the builtin IEnumerator<T> (no sema TypeDecl, hence no
    // PropertySymbol — lowerVirtualPropertyGet returns null). The iterator state
    // struct's vtable layout is [0]=td, [1]=Dispose, [2]=MoveNext(→i1),
    // [3]=Current(→T). Dispatch through slot 3 returning T.
    // This complements the MoveNext fix in lowerCall (issue #494).
    // #1349 / §29.1 v0.161: IOwningEnumerator<T>/IOwningAsyncEnumerator<T> no longer
    // have Current — they use TakeCurrent() dispatched in lowerCall below.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (ma->member == "Current" && ma->target) {
            const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
            bool isBuiltinEnumerator =
                recvTy &&
                recvTy->kind == sema::TypeRef::Kind::Named &&
                !recvTy->declaration &&
                recvTy->astNode && recvTy->astNode->name &&
                !recvTy->astNode->name->parts.empty() &&
                // issue #759: async enumerators share the slot-3 layout.
                // #1349: IOwningEnumerator/IOwningAsyncEnumerator excluded (TakeCurrent).
                (recvTy->headName() == "IEnumerator" ||
                 recvTy->headName() == "IAsyncEnumerator");
            if (isBuiltinEnumerator) {
                llvm::Value* recv = lowerExpr(*ma->target);
                if (recv && recv->getType()->isPointerTy()) {
                    llvm::Type* elemTy =
                        (!recvTy->genericArgs.empty() && recvTy->genericArgs[0])
                        ? lowerTypeRef(recvTy->genericArgs[0])
                        : i32();
                    llvm::Value* vtable = builder_.CreateLoad(ptrTy(), recv);
                    auto* currentTy =
                        llvm::FunctionType::get(elemTy, {ptrTy()}, false);
                    llvm::Value* curSlotPtr = builder_.CreateGEP(
                        ptrTy(), vtable, {builder_.getInt32(3)});
                    llvm::Value* curFnPtr =
                        builder_.CreateLoad(ptrTy(), curSlotPtr);
                    return builder_.CreateCall(
                        currentTy, curFnPtr, {recv}, "cur");
                }
            }
        }
    }
    // §27.6 (#756): a property reference in FUNCTION-VALUE position. sema's
    // target-typed promotion (checkAssignable) typed `b.Count` as `() -> T` when
    // it is bound to a function-typed slot; produce the getter as a closure
    // (≡ `() => b.Count`) instead of evaluating the getter here (which would
    // store the int into a function-pointer slot and SIGSEGV at the call). Gate
    // strictly on exprType == Function so ordinary value-position reads (typed as
    // the property value) are unaffected and fall through to the read paths below.
    if (const sema::TypeRef* et = ctx_.exprType(e)) {
        if (et->kind == sema::TypeRef::Kind::Function) {
            if (auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(e))) {
                // per §23.1/§23.2 / §27.6 (#1187): the #756 getter-as-function-
                // value promotion applies only when a NON-function property read
                // is target-typed up to `() -> T`. When the property's DECLARED
                // type is ITSELF a function type, the read is an ordinary value
                // read — the getter already yields the function value — and the
                // thunk must be skipped. Promoting here would wrap the getter as a
                // `() -> ((…) -> T)` closure and bind it into a `(…) -> T` slot,
                // so the later call passes the user args at the wrong arity
                // (static SIGSEGV / instance garbage). Discriminate on the
                // declared type from the PropertyDecl (or the imported member
                // type for a cross-module property, which has no AST decl).
                bool declaredFunc = false;
                if (const auto* pd =
                        dynamic_cast<const ast::PropertyDecl*>(ps->decl)) {
                    declaredFunc =
                        pd->type && pd->type->kind == ast::TypeKind::Function;
                } else if (sema::TypeRef* tr = ctx_.importedMemberType(ps)) {
                    declaredFunc = tr->kind == sema::TypeRef::Kind::Function;
                }
                if (!ps->isIndexer && !declaredFunc) {
                    const auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e);
                    llvm::Value* recv =
                        (ma && ma->target) ? lowerExpr(*ma->target) : currentThis_;
                    if (llvm::Value* th = makePropertyThunk(ps, recv, ma))
                        return th;
                }
            }
        }
    }
    // #481 / §12.1 / §12.5 / §13 / §23: a VIRTUAL or INTERFACE property read
    // through a (potentially) polymorphic receiver dispatches dynamically — load
    // the accessor function from the receiver's vtable/itable and call it. This
    // precedes the static getter paths below (which would bind the *static*
    // type's accessor and miscompile / crash an override).
    if (llvm::Value* v = lowerVirtualPropertyGet(e)) return v;
    // CG33b: a computed (arrow-getter) property read `obj.P` evaluates
    // the getter with `this` = obj.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (const ast::Expr* getter = computedGetterOf(ctx_.exprSymbol(*ma))) {
            if (!ma->target) return nullptr;
            // CG33b: for static properties (accessed via type name), the
            // receiver is a class type identifier, not an instance — skip
            // the obj/currentThis_ setup and evaluate the body directly.
            auto* propSym = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*ma));
            bool isStaticProp = propSym && !propSym->isIndexer &&
                std::any_of(propSym->modifiers.begin(),
                            propSym->modifiers.end(),
                            [](const auto& m){ return m == "static"; });
            if (isStaticProp) {
                // Static property: no 'this', just evaluate the getter body.
                // §23.2/§23.3 (#1168): wrap a bare-inner `T` body result to the
                // declared `T?` optional, same as the instance paths below.
                llvm::Value* sv = wrapNullableGetterResult(
                    ctx_.exprType(*ma), lowerExpr(*getter));
                // fix #2761 (§8.6 "Boxing", v0.240): and box it when the
                // property's declared type is Object/an interface — the static
                // spelling of the instance-path box below; see there.
                return boxForTarget(sv, ctx_.exprType(*ma), getter);
            }
            llvm::Value* obj = lowerExpr(*ma->target);
            if (!obj) return nullptr;
            // fix #2816 / §28.3 "Unbound owned temporaries" (v0.240): establish
            // the temporary's disposal AT ITS PRODUCTION, so an inlined getter
            // body that throws unwinds through it. Discharged by the reclaim
            // below. See registerFreshNewReceiverTempUnwind.
            llvm::AllocaInst* recvUnwindSlot =
                registerFreshNewReceiverTempUnwind(ma->target.get(), obj,
                                                   ctx_.exprType(*ma),
                                                   /*calleeReturnsOwned=*/false);
            // issue #347 / §9.2: a record is a by-value struct, not a heap
            // pointer. The getter body reads its fields against `this` via a
            // StructGEP, which requires a pointer base; binding a struct
            // *value* as `this` produces malformed IR (GEP on a non-pointer)
            // that crashes the backend. When the receiver is a record value
            // (not already a pointer), spill it to a stack slot and bind that
            // slot's address as `this` — mirroring the record field-access
            // path in fieldAddr (CG29/#254) and the §9.2 record-storage
            // access used by the synthesized ToString.
            // #574 / §10.4: enums are value types (i32), but their getter
            // bodies use `this` as a plain integer (e.g. `case this ...`),
            // NOT as a struct pointer. Skip the spill for enum receivers so
            // that `scrut = lowerExpr("this")` inside `lowerCaseExpr` yields
            // the raw i32 value that can be compared against enum constants
            // via icmp. Spilling an i32 to a ptr and then comparing the ptr
            // against an i32 constant would produce malformed IR (icmp with
            // mismatched types → IR verifier failure).
            bool isEnumReceiver = propSym && propSym->parent &&
                                  propSym->parent->kind == ast::TypeDeclKind::Enum;
            // sweep #8: an extension property on a PRIMITIVE value type
            // (`extension int … property IsZero => this = 0`) uses `this` as the
            // scalar value (i32/i1/…), exactly like an enum — NOT a struct
            // pointer. Only a non-pointer AGGREGATE (a record/struct receiver)
            // needs the spill-to-pointer so the getter's `this.field` access works;
            // spilling a scalar and using the pointer as `this` makes the getter
            // body read `this` at the wrong type (`this = 0` → `icmp ptr, i32` →
            // LLVM assertion / IR_FAIL). Spill only non-pointer aggregates.
            if (!obj->getType()->isPointerTy() &&
                obj->getType()->isAggregateType() && !isEnumReceiver) {
                llvm::AllocaInst* tmp = entryAlloca(obj->getType());
                builder_.CreateStore(obj, tmp);
                obj = tmp;
            }
            // CG-generic: set up genericSubst_ for the receiver's type so
            // that fields accessed inside the getter use the right struct.
            auto savedSubst = genericSubst_;
            auto savedSemaTy = genericSubstSemaTy_;
            if (auto* sym = ctx_.exprSymbol(*ma)) {
                if (auto* ps = dynamic_cast<sema::PropertySymbol*>(sym)) {
                    if (ps->parent) {
                        sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
                        // fix #3143 (§14.1 / §2.5, v0.254): parameter names
                        // through the one both-sources accessor (and the
                        // `parent->decl` pre-guard dropped with it). Built only
                        // once the receiver guards have passed.
                        if (recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
                            recvTy->declaration == ps->parent &&
                            !recvTy->genericArgs.empty()) {
                            const std::vector<std::string_view> gpNames =
                                sema::genericParamNames(ps->parent);
                            if (!gpNames.empty() &&
                                gpNames.size() == recvTy->genericArgs.size()) {
                                std::unordered_map<std::string_view, llvm::Type*> newSubst;
                                // §14.1 (#768): build the SEMA-type substitution
                                // too — the getter body resolves the member type
                                // (e.g. `_v: T?`) through genericSubstSemaTy_, so
                                // without it a generic nullable field read with
                                // the wrong type (wrong value / mismatched ICmp).
                                std::unordered_map<std::string_view,
                                                   const sema::TypeRef*> newSema;
                                for (std::size_t gi = 0; gi < gpNames.size(); ++gi) {
                                    llvm::Type* concrTy = lowerTypeRef(recvTy->genericArgs[gi]);
                                    newSubst[gpNames[gi]] = concrTy;
                                    newSema[gpNames[gi]] =
                                        resolveTypeParamRef(recvTy->genericArgs[gi]);
                                }
                                // Check unresolved (i32 fallback) → use localGenericSubst_
                                bool hasUnresolved = false;
                                for (auto& [n, ty] : newSubst)
                                    if (ty == i32()) { hasUnresolved = true; break; }
                                if (hasUnresolved) {
                                    sema::Symbol* recvSym = ctx_.exprSymbol(*ma->target);
                                    const ast::Node* decl = nullptr;
                                    if (auto* loc = dynamic_cast<sema::LocalSymbol*>(recvSym))
                                        decl = loc->decl;
                                    if (decl) {
                                        auto lit = localGenericSubst_.find(decl);
                                        if (lit != localGenericSubst_.end())
                                            newSubst = lit->second;
                                    }
                                }
                                genericSubst_ = std::move(newSubst);
                                genericSubstSemaTy_ = std::move(newSema);
                            }
                        } else if (recvTy &&
                                   recvTy->kind == sema::TypeRef::Kind::Named &&
                                   recvTy->declaration &&
                                   recvTy->declaration != ps->parent &&
                                   !ps->parent->genericParams.empty()) {
                            // fix #3346 / #3348 (§14.1 / §23.3 / §23.4, v0.278):
                            // an INHERITED NAMED PROPERTY on a generic base reached
                            // through a derived receiver (`g.P` where `g: GD`,
                            // the property P is in `GBase<T>`). The DIRECT gate
                            // above (`recvTy->declaration == ps->parent`) returns
                            // unchanged, the getter body lowers against the
                            // T→i32 placeholder, and `P` reads only the first 4
                            // bytes of the substituted `_v: T` field (where `T=Pt`
                            // is an 8-byte record). The INDEXER twin of this gate
                            // is `setupReceiverGenericSubst` (LlvmBackend_Calls.cpp),
                            // which `lowerIndexerGet`'s inline path already routes
                            // through. The NAMED-PROPERTY inline path uses an
                            // INLINED copy of the same gate here — the two were
                            // out of sync and #3346/#3348 measured the divergence
                            // (NAME returns 22 where 23 is correct; INDEXER
                            // returns 11). Walk the base chain via
                            // `computeAncestorGenericSubst` — same vehicle the
                            // inherited METHOD-call path uses (#585/#902/#3347).
                            std::unordered_map<std::string_view, llvm::Type*>
                                ancSubst;
                            std::unordered_map<std::string_view,
                                               const sema::TypeRef*>
                                ancSema;
                            if (computeAncestorGenericSubst(recvTy, ps->parent,
                                                            ancSubst, ancSema)) {
                                genericSubst_ = std::move(ancSubst);
                                genericSubstSemaTy_ = std::move(ancSema);
                            }
                        }
                    }
                }
            }
            llvm::Value* saved = currentThis_;
            currentThis_ = obj;
            // §28.2 v0.134 / issue #601: checked disposal mode guard for
            // expression-bodied property getters inlined at call site. The
            // guard is emitted against the already-loaded `obj` pointer;
            // only class receivers carry a disposed flag (records and enums
            // are value types with no disposal lifetime).
            if (checkedDisposal_ && propSym && propSym->parent &&
                propSym->parent->kind == ast::TypeDeclKind::Class &&
                obj->getType()->isPointerTy()) {
                llvm::StructType* st = classStruct(propSym->parent);
                unsigned fi = disposedFlagIndex(propSym->parent);
                llvm::Value* flagAddr =
                    builder_.CreateStructGEP(st, obj, fi, "cd.flag");
                llvm::Value* disposed = builder_.CreateICmpNE(
                    builder_.CreateLoad(disposedFlagTy(), flagAddr),
                    llvm::ConstantInt::get(disposedFlagTy(), 0), "cd.disposed");
                emitGuardThrow(disposed, "DisposedAccessException");
            }
            // #574 / §10.4 / §19.2 (CG30): an enum computed property whose
            // getter body is `case this Red => …` relies on enumTypeOf() to
            // resolve `this` as the declaring enum type so patternMatches can
            // compare the enum constant patterns. enumTypeOf() uses
            // currentEnumMethod_ for a `this` identifier because sema does not
            // attach an exprType to `this` — the same reason CG30 sets it for
            // enum instance methods. Set it here for the duration of the
            // inline getter body so `case this` inside an enum property works
            // exactly as it does inside an enum method.
            const sema::TypeSymbol* savedEnumMethod = currentEnumMethod_;
            if (propSym && propSym->parent &&
                propSym->parent->kind == ast::TypeDeclKind::Enum)
                currentEnumMethod_ = propSym->parent;
            // fix #4113 (§35.2/§35.5, v0.295): the choice twin of the enum
            // workaround directly above — a choice computed property whose
            // getter body is `case this of …` relies on choiceTypeOf() to
            // resolve `this` as the declaring choice type (sema attaches no
            // exprType to `this` here either). Without this, `case this of`
            // found no scrutinee choice type, patternMatches returned null,
            // and lowerCaseExpr bailed leaving `case.end` unterminated — an
            // IR-verification crash ("Basic Block ... does not have
            // terminator!"), degraded through the #2491 backstop.
            //
            // fix #4143 (§35.2/§35.5, v0.295): SET-FROM-OWNER, not
            // conditionally-set-else-inherit. This is an INLINE evaluation
            // (currentFn_ stays the CALLER's function; there is no
            // NestedBodyScope boundary here at all, unlike the block-getter/
            // itable-accessor family), so a bare `if (Choice) set` — leaving
            // the field untouched for every OTHER receiver kind — passes a
            // caller's choice window straight through into THIS getter's own
            // body unmodified. Measured: `Animal.Sound` (a CLASS property
            // whose getter is `get => (case this of Dog => …, Cat => …, _ =>
            // …)`, itself reached through this SAME inline path since an
            // accessor with an EXPRESSION body classifies as
            // `computedGetterOf` regardless of whether the enclosing
            // property is arrow- or block-declared — see that function's own
            // comment), first touched from inside `ShapeQ.Kind`'s
            // choice-property getter (`(case this of CircleQ(r) => new
            // Dog().Sound, …)`), inherited `currentChoiceMethod_ == ShapeQ`
            // straight through this unconditional-else-untouched code and
            // fell into lowerCaseExpr's "scrutTy not already a Choice"
            // overwrite guard (LlvmBackend_Exprs2.cpp) — silently
            // reclassifying `Animal`'s `this` as a `ShapeQ` scrutinee.
            // `patternMatches` then found no `Dog`/`Cat` cases in `ShapeQ`
            // and fell through to `_`, printing "..." instead of "Woof": a
            // SILENT WRONG ANSWER, not a crash.
            const sema::TypeSymbol* savedChoiceMethod = currentChoiceMethod_;
            currentChoiceMethod_ =
                (propSym && propSym->parent &&
                 propSym->parent->kind == ast::TypeDeclKind::Choice)
                    ? propSym->parent
                    : nullptr;
            llvm::Value* v = lowerExpr(*getter);
            // §23.2/§23.3 (#1168): a `T?`-declared expression-bodied property
            // whose body yields the bare inner `T` must wrap the result to the
            // declared `{i8 hasValue; T}` optional, just as a method/lambda
            // return does (#541). Wrap while the receiver's generic context is
            // still active so a generic `T?` payload lowers correctly.
            v = wrapNullableGetterResult(ctx_.exprType(*ma), v);
            // fix #2761 (§8.6 "Boxing" / §23.2, v0.240): an EXPRESSION-bodied
            // property whose declared type is `Object` (or an interface) but
            // whose body yields a VALUE type must box here — this inline is the
            // getter's only value-production point, so it is the only place the
            // §8.6 representation can be established. Every DOWNSTREAM
            // boxForTarget keys on the sema type of its source EXPRESSION,
            // which for a property read is already `Object`, so all of them
            // no-op and the raw scalar used to travel into an `Object` slot:
            //   `property O: Object => _n;` then `var o := h.O; o is int`
            //   returned FALSE with no diagnostic (lowerTypeTest gives up on a
            //   non-pointer operand and lowerIf's null-condition arm branches
            //   past the body — reproduced, `if.body: No predecessors!`), and
            //   `F(h.O)` for `F(o: Object)` failed IR verification outright
            //   ("Call parameter type does not match function signature").
            // The BLOCK-bodied twin has done this since #2703 (lowerReturn's
            // §8.6 arm); this restores the #2688 agreement invariant between
            // the two body forms. Boxed value types are runtime-managed and
            // "exempt from manual disposal" (§28.9), so no release is added
            // here — the box's lifetime question is #2760, shared with the
            // block form. Emitted while the receiver's generic substitution is
            // still installed, like the nullable wrap above, so a `T`-typed
            // body under monomorphisation boxes as its CONCRETE type (#1279).
            v = boxForTarget(v, ctx_.exprType(*ma), getter);
            // fix #2815 / §23.2 / §29.2 (v0.240): an expression-bodied
            // property whose declared type is a pure `IEnumerable<T>` /
            // `IOwningEnumerable<T>` but whose getter body yields an
            // Array-backed collection (`public property Items: IEnumerable<int>
            // get => _items;` where `_items: Array<int>`) lowers the raw
            // `bz_array_t*`, not a vtable-backed IEnumerable. Every consumer
            // (a local-decl-init / argument / return / direct for-in over the
            // read) would otherwise drive vtable dispatch on the raw array
            // descriptor — issue #2815's SIGSEGV at slot 4. `wrapPropertyArray
            // AsIEnumerable` is a no-op when the declared type is not a pure
            // IEnumerable-family interface or the getter's type is not in the
            // array-backed family, so it composes with the nullable / box
            // wraps above without re-doing their work.
            v = wrapPropertyArrayAsIEnumerable(v, ctx_.exprType(*ma), getter, ma);
            currentEnumMethod_ = savedEnumMethod;
            currentChoiceMethod_ = savedChoiceMethod;
            currentThis_ = saved;
            genericSubst_ = std::move(savedSubst);
            genericSubstSemaTy_ = std::move(savedSemaTy);
            // fix #2751 / §28.3 "Unbound owned temporaries" (v0.240): the
            // EXPRESSION-BODIED getter spelling of the unbound-owned-receiver
            // reclaim — `(new Src()).N` where `N`'s getter is `=> _v`. See
            // lowerVirtualPropertyGet for the shared rationale and the
            // soundness argument. `obj` is the receiver pointer this path
            // already lowered (never re-lowered — that would run the producer
            // twice); the getter body has been fully inlined and its result
            // wrapped/boxed into `v` by now, so nothing reads the receiver
            // after this point. Emitted after the generic-substitution restore
            // because the disposal is the CALLER's (it dispatches through the
            // receiver's vtable, so it is substitution-independent anyway).
            if (!diverged_)
                disposeFreshNewReceiverTemp(ma->target.get(), obj,
                                            ctx_.exprType(*ma),
                                            /*calleeReturnsOwned=*/false,
                                            recvUnwindSlot);
            // fix #4320 (§26.1 / §4.6 / §28.9, v0.298): the STRING-RECEIVER
            // sibling of the class-receiver reclaim immediately above. The
            // #2751 reclaim only fires for a heap-CLASS producer
            // (`isFreshOwnedClassProducer`'s TypeDeclKind::Class gate) — a
            // string producer (`P.Make()`, `"a" + "b"`, `s.Trim()`,
            // `i.ToString()` — §28.9's full +1 list) goes through `obj` as a
            // plain `ptr` and never reaches the reclaim, so the receiver
            // string allocation leaks exactly once per use site. The CG27a
            // Length/IsAscii intrinsic path already handled the TWO real
            // intrinsics (#4243), but a USER-DEFINED computed extension
            // property (e.g. the spec's own §26.1 worked `IsBlank` example,
            // accessed on a +1 producer like `P.Make().IsBlank`) falls
            // through to this CG33b arm and so falls through the leak.
            //
            // SOUNDNESS — the result type must NOT alias the receiver. A
            // string-typed result may be a borrow of the receiver (e.g. an
            // identity getter `=> this`); releasing the receiver's +1 there
            // would free the underlying string while `v` (== obj) still
            // points at it — a use-after-free. The non-string-result gate
            // matches CG27a's implicit precondition (Length → int, IsAscii →
            // bool: neither aliases), and the empty-body identity getter
            // sibling test in extension_property_string_4243_test.cpp pins
            // that an alias-shaped getter is NOT released (its receiver is a
            // named local, not a +1 producer — the `isPlusOneStringProducer`
            // gate below declines, matching the pre-fix safe behavior).
            //
            // Gated on `isPlusOneStringProducer` (not the type-name check)
            // because it is the same predicate the §28.9 producer-releases
            // across the rest of this backend already key on; a literal /
            // sentinel receiver is correctly released as a no-op
            // (`bz_string_release` is sentinel-safe). Unwind safety is
            // delegated to the parallel class-receiver path's `recvUnwindSlot`
            // when the same expr is BOTH a class and a string — that's
            // unreachable today (`freshNewReceiverTempEligible` rejects
            // strings), so on a string receiver the slot is null and the
            // inline call below is the only release; throws inside the
            // getter body skip it (matches CG27a's own behavior on its
            // simpler intrinsics — `loadStringLen` / `bz_string_is_ascii`
            // are non-throwing, the IsBlank getter body is `=`/`or`/
            // `Length`/`Trim` — none of which throws on this input — and
            // a throwing-getter edge is out of scope for the issue's repro).
            if (!diverged_)
                disposeFreshNewStringReceiverTemp(ma->target.get(), obj,
                                                  ctx_.exprType(*ma),
                                                  recvUnwindSlot);
            return v;
        }
        // CG33b: block-form getter (`get => do … end;`) — emit as a
        // standalone function so that `return` inside the body terminates
        // only the getter, not the enclosing method (§21.7).
        if (const ast::PropertyAccessor* acc =
                blockGetterOf(ctx_.exprSymbol(*ma))) {
            if (!ma->target) return nullptr;
            llvm::Value* obj = lowerExpr(*ma->target);
            if (!obj) return nullptr;
            // fix #2816 / §28.3 "Unbound owned temporaries" (v0.240): establish
            // the temporary's disposal AT ITS PRODUCTION so the standalone
            // getter's throw edge (emitPendingCheck below) unwinds through it.
            llvm::AllocaInst* recvUnwindSlot =
                registerFreshNewReceiverTempUnwind(ma->target.get(), obj,
                                                   ctx_.exprType(*ma),
                                                   /*calleeReturnsOwned=*/false);
            // issue #347 / §9.2: the block getter is a function taking a
            // `this` pointer; a record receiver is a by-value struct, so spill
            // it to a stack slot and pass that slot's address (see the
            // expression-getter path above for the full rationale).
            if (!obj->getType()->isPointerTy()) {
                llvm::AllocaInst* tmp = entryAlloca(obj->getType());
                builder_.CreateStore(obj, tmp);
                obj = tmp;
            }
            auto* pd = dynamic_cast<const ast::PropertyDecl*>(
                dynamic_cast<sema::PropertySymbol*>(
                    ctx_.exprSymbol(*ma))->decl);
            llvm::Function* getterFn = ensurePropertyBlockGetter(pd, acc);
            llvm::Value* res = builder_.CreateCall(getterFn->getFunctionType(),
                                                   getterFn, {obj});
            // §31.1/§31.2 (FINDING [4]): a block-form getter is a standalone
            // function; a `throw` inside it (the §23.3 idiom) leaves the
            // pending-exception global set and returns. The caller must test
            // that flag and route to the enclosing unwind target — exactly as
            // an ordinary method call does (emitMethodCall → emitPendingCheck).
            // Without this the throw is swallowed and `res` is a garbage value.
            emitPendingCheck();
            // fix #2751 / §28.3 "Unbound owned temporaries" (v0.240): the
            // BLOCK-BODIED getter spelling (`get => do … end;`). See
            // lowerVirtualPropertyGet for the shared rationale. `obj` may have
            // been spilled to an alloca just above for a by-value RECORD
            // receiver — harmless, because isFreshOwnedClassProducer (inside
            // the helper) admits only a heap CLASS receiver, so the spilled
            // form never reaches the disposal.
            if (!diverged_)
                disposeFreshNewReceiverTemp(ma->target.get(), obj,
                                            ctx_.exprType(*ma),
                                            /*calleeReturnsOwned=*/false,
                                            recvUnwindSlot);
            // fix #4320 (§26.1 / §4.6 / §28.9, v0.298): the block-getter
            // spelling of the string-receiver reclaim. Same predicate /
            // soundness gate as the expression-bodied twin above; the
            // surrounding `disposeFreshNewReceiverTemp` is class-only, so
            // a `+1` string producer would otherwise leak here too.
            if (!diverged_)
                disposeFreshNewStringReceiverTemp(ma->target.get(), obj,
                                                  ctx_.exprType(*ma),
                                                  recvUnwindSlot);
            return res;
        }
        // §2.5 / §23 (issue #503): imported non-virtual property getter.
        // computedGetterOf and blockGetterOf both returned null because the
        // imported PropertySymbol has no ast::PropertyDecl. Call the external
        // getter function `bz$<Class>$<Prop>$propget` declared by
        // declareImportedPropertyGetters.
        if (auto* ps =
                dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*ma))) {
            // fix #3231 (§12.3 / §23 / §2.8.2, v0.252): `base.P` on an
            // IMPORTED VIRTUAL base property now ROUTES through the OWNER's
            // direct entry — `importedPropGetterDirectFns_[ps]`, populated
            // by declareImportedPropertyGetters with the symbol the
            // widened emitExportedPropertyGetters defines for VIRTUAL
            // members. Runs BEFORE `rejectImportedVirtualBaseProperty`
            // (which now only fires for an ABSTRACT virtual member whose
            // wrapper the exporter erased — §12.5-correct, since no OWNER
            // body exists to call from `base.P` either way).
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(
                    ma->target.get());
                id && id->name == "base") {
                if (auto dgit = importedPropGetterDirectFns_.find(ps);
                    dgit != importedPropGetterDirectFns_.end()) {
                    llvm::Function* fn = dgit->second;
                    llvm::Value* obj = lowerExpr(*ma->target);
                    if (!obj) return nullptr;
                    // Record-receiver spill mirrors the `git` arm below:
                    // declareImportedPropertyGetters declares the stub as
                    // `T get(ptr %this)` (#2474's record-receiver rationale
                    // applies here too).
                    bool isEnumReceiver = ps->parent &&
                                          ps->parent->kind ==
                                              ast::TypeDeclKind::Enum;
                    if (obj && !obj->getType()->isPointerTy() &&
                        obj->getType()->isAggregateType() &&
                        !isEnumReceiver) {
                        llvm::AllocaInst* tmp = entryAlloca(obj->getType());
                        builder_.CreateStore(obj, tmp);
                        obj = tmp;
                    }
                    llvm::Value* res = builder_.CreateCall(
                        fn->getFunctionType(), fn, {obj});
                    emitPendingCheck();
                    return res;
                }
                if (rejectImportedVirtualBaseProperty(*ma, ps))
                    return nullptr;
            } else if (rejectImportedVirtualBaseProperty(*ma, ps)) {
                return nullptr;
            }
            // fix #2885 (§22.12/§22.13/§2.8.2 clause 1/§23, v0.240): an
            // imported STATIC property reads through its own no-`this` stub.
            // `ma->target` names a TYPE here, so the receiver lowering below
            // produced nothing pointer-shaped and this whole `if` fell through
            // — the read left `lowerExpr` as a bare nullptr, which the callers
            // turn into a silent 0 (`var r := HS.SP;` hit lowerLocalDecl's
            // #2434 zero-init backstop) or a CRASH (`"x" + HS.SP.ToString()`
            // SIGSEGV'd, both measured on a real two-module build).
            if (auto sgit = importedStaticPropGetterFns_.find(ps);
                sgit != importedStaticPropGetterFns_.end()) {
                // §22.13: a static-member access triggers the declaring type's
                // lazy initialization — the same guard the cross-module static
                // FIELD read emits (memberOwner/emitEnsureStaticInit above).
                emitEnsureStaticInit(ps->parent);
                llvm::Value* res = builder_.CreateCall(
                    sgit->second->getFunctionType(), sgit->second, {});
                emitPendingCheck();   // §31.1/§31.2: a throw out of the getter
                return res;
            }
            auto git = importedPropGetterFns_.find(ps);
            if (git != importedPropGetterFns_.end() && ma->target) {
                llvm::Value* obj = lowerExpr(*ma->target);
                // fix #2474 (§9.2/§23.1/§23.2, v0.235): issue #347's own rationale
                // applies here exactly as it does to the LOCAL computed-getter
                // path above (~L4122-4156) and the block-getter path above
                // that (~L4257-4267) — a RECORD receiver lowers to a by-value
                // struct, not a heap pointer, but declareImportedPropertyGetters
                // unconditionally declares the external stub as `T get(ptr
                // %this)` (LlvmBackend_Setup.cpp declareImportedPropertyGetters,
                // `{ptrTy()}`). Pre-fix, a non-pointer `obj` (any record
                // receiver) failed `isPointerTy()` and fell through this whole
                // `if`, past CG8's field-read fallback (which does not resolve
                // a PropertySymbol either), out of lowerExpr entirely as a bare
                // `nullptr` — a silent, contextless failure: `return r.Getter;`
                // alone degraded to a bare 0 with no diagnostic, `var g :=
                // r.Getter;` hit lowerLocalDecl's #2434 zero-init backstop
                // (loud, but still 0), and `r.x + r.Getter` returned nullptr
                // from lowerBinary's `if (!l || !r) return nullptr;` (line
                // ~5537) — discarding r.x's already-correctly-lowered value
                // too, not just the failing operand. Spill the record value to
                // a stack slot and pass that slot's address as `this`,
                // mirroring the sibling paths exactly.
                //
                // Guard the spill to AGGREGATE receivers only (mirroring the
                // LOCAL computed-getter path's own `isAggregateType() &&
                // !isEnumReceiver` condition exactly, not just its
                // `isPointerTy()` half) — an ENUM receiver lowers to a bare
                // scalar `i32`, never a struct, so it is not this fix's
                // concern. Review caught an over-broad first draft here that
                // guarded on `!isPointerTy()` alone: `declareImportedProperty
                // Getters` (LlvmBackend_Setup.cpp) declares the external
                // stub for EVERY imported PropertySymbol with no kind filter,
                // including an enum's expression-bodied property (§10.4
                // permits one), so an unconditional spill made THAT call fire
                // too — but this fix's exporter-side half (
                // emitExportedPropertyGetters) only widened its gate to
                // `Class || Record`, not `Enum`, so the stub the call now
                // reached had no definition anywhere. Confirmed mechanically:
                // the over-broad draft turned a program that compiles and
                // links TODAY (silently reading the documented `0` degrade,
                // exactly like the record case pre-fix) into a hard
                // `undefined reference` LINK ERROR — strictly worse than the
                // preexisting silent-wrong-value gap it did not intend to
                // touch. Restricting the spill to aggregates restores the
                // exact preexisting (still-silently-wrong, still out of this
                // record-scoped issue's scope) enum behavior. Filed as its
                // own issue rather than fixed here or by widening layer 1 to
                // `Enum` — that would additionally require confirming
                // ensurePropertyAccessorFn's enum `this` handling is correct
                // when reached through this NEW external-stub calling
                // convention (today only exercised, if at all, through
                // itable dispatch, a different call shape), which is exactly
                // the kind of "one more layer" scope creep #2474 (a
                // RECORD-scoped issue) should not silently absorb.
                bool isEnumReceiver = ps->parent &&
                                      ps->parent->kind == ast::TypeDeclKind::Enum;
                if (obj && !obj->getType()->isPointerTy() &&
                    obj->getType()->isAggregateType() && !isEnumReceiver) {
                    llvm::AllocaInst* tmp = entryAlloca(obj->getType());
                    builder_.CreateStore(obj, tmp);
                    obj = tmp;
                }
                if (obj && obj->getType()->isPointerTy()) {
                    // fix #2816 / §28.3 (v0.240): establish the temporary's
                    // disposal at its production, so the imported getter's
                    // throw edge (emitPendingCheck below) unwinds through it.
                    llvm::AllocaInst* recvUnwindSlot =
                        registerFreshNewReceiverTempUnwind(
                            ma->target.get(), obj, ctx_.exprType(*ma),
                            /*calleeReturnsOwned=*/false);
                    llvm::Value* res = builder_.CreateCall(
                        git->second->getFunctionType(), git->second, {obj});
                    emitPendingCheck();
                    // fix #2751 / §28.3 "Unbound owned temporaries" (v0.240):
                    // the IMPORTED (cross-module) getter spelling — the same
                    // reclaim as the two local getter paths above. See
                    // lowerVirtualPropertyGet for the shared rationale.
                    if (!diverged_)
                        disposeFreshNewReceiverTemp(ma->target.get(), obj,
                                                    ctx_.exprType(*ma),
                                                    /*calleeReturnsOwned=*/false,
                                                    recvUnwindSlot);
                    return res;
                }
            }
        }
    }
    // CG8: field read `obj.f`.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        // §24.3 (#747): an event read in VALUE position (`var ev := b.Clicked;`,
        // `return Clicked;` — not immediately `.Subscribe`'d) yields the bare
        // bz_event_source_t* (a BORROW of the declaring instance's backing
        // source — the same pointer the direct `.Subscribe` path uses). Without
        // this, fieldAddr's storageMemberOf returns null for an EventSymbol and
        // the IEvent<T> local was loaded with garbage, so its Subscribe never
        // fired. The IEvent<T> value is non-owning (the class disposes the
        // source), so this read takes no ownership.
        if (eventSymbolOf(e))
            if (llvm::Value* src = eventSourcePtr(e))
                return src;
        // #548 / stdlib §4.8 v0.135: `entry.Key` / `entry.Value` on a
        // DictionaryEntry<K,V> loop variable. The entry alloca is a
        // {K, V} struct (laid down by lowerDictEntryFor); field 0 is Key,
        // field 1 is Value. DictionaryEntry has no user TypeSymbol, so
        // fieldAddr cannot locate its fields — intercept here first.
        // #2471 (stdlib §4.8/§4.12; language spec §29.1, v0.236): the target
        // need not be a loop-variable local — it may be a TEMPORARY value like
        // `e.Current` (the result of an explicit `.GetEnumerator()` call on a
        // Dictionary<K,V>, dispatched via IEnumerator<DictionaryEntry<K,V>>
        // slot 3) or any other DictionaryEntry-producing expression. Spill a
        // non-local target to a fresh alloca before GEP-ing into the field,
        // so the same field-access path serves both `for var entry in dict`
        // (entry is a local) and `e.Current.Value` (e.Current is a temporary).
        // fix #2794: written `DictionaryEntry<K,V>` annotations carry an astNode
        // (their `importedName` is empty); the old synthetic-only importedName
        // check now uses `headName()`, which reads the syntactic head from the
        // astNode for written refs and falls back to importedName for the
        // synthetic iterator TypeRef.
        if (ma->target && (ma->member == "Key" || ma->member == "Value")) {
            if (auto* tt = ctx_.exprType(*ma->target)) {
                if (tt->kind == sema::TypeRef::Kind::Named &&
                    !tt->declaration &&
                    tt->headName() == "DictionaryEntry" &&
                    tt->genericArgs.size() >= 2) {
                    // The entry alloca type is a 2-element struct {K, V}.
                    llvm::Type* kTy = lowerTypeRef(tt->genericArgs[0]);
                    llvm::Type* vTy = lowerTypeRef(tt->genericArgs[1]);
                    llvm::StructType* entryStTy =
                        llvm::StructType::get(llvm_, {kTy, vTy});
                    unsigned fi = ma->member == "Key" ? 0 : 1;
                    // Prefer an existing local alloca (the loop-variable case)
                    // — its storage already holds the {K,V} struct. Otherwise
                    // lower the target expression to its value and spill it to
                    // a fresh alloca so CreateStructGEP has an addressable
                    // slot (LLVM GEP needs a pointer; a struct SSA value must
                    // be spilled first).
                    if (llvm::AllocaInst* slot = localSlot(*ma->target)) {
                        // fix #3876 (§27.5 / stdlib §4.8, v0.295): a
                        // DictionaryEntry local CAPTURED by a closure (§27.5)
                        // has its `locals_` entry REBOUND to the ptr-sized
                        // cell-HOLDER alloca (boxLocalSlot / lowerLambda's
                        // env-restore loop, LlvmBackend_Exprs2.cpp) — the real
                        // {K,V} struct lives in the heap cell the holder
                        // points AT, not in the holder itself. GEP'ing
                        // directly into `slot` (as this arm did pre-fix)
                        // treated the 8-byte pointer holder as if it WERE the
                        // {K,V} struct: in-bounds-but-garbage for a same-size
                        // DictionaryEntry<int,int> ({i32,i32}, 8 B) and a
                        // genuine stack-buffer-overflow for a wider one like
                        // DictionaryEntry<string,int> ({ptr,i32}, 16 B) —
                        // issue #3876's measured ASan stack-buffer-overflow.
                        // `boxedStorage` is the existing §27.5 funnel every
                        // other boxed read/write already goes through: it
                        // dereferences the holder when `slot` is a capture
                        // cell (loading the cell pointer and handing back ITS
                        // address) and returns `slot` unchanged otherwise (an
                        // ordinary, non-captured local) — so the pre-fix
                        // non-captured behaviour is unaffected.
                        llvm::Type* baseElemTy = nullptr;
                        llvm::Value* base = boxedStorage(slot, baseElemTy);
                        llvm::Value* fieldPtr = builder_.CreateStructGEP(
                            entryStTy, base, fi, "de.fld");
                        return builder_.CreateLoad(
                            entryStTy->getElementType(fi), fieldPtr, "de.load");
                    } else if (llvm::Value* v = lowerExpr(*ma->target)) {
                        if (v->getType()->isStructTy()) {
                            llvm::AllocaInst* tmp = entryAlloca(entryStTy);
                            builder_.CreateStore(v, tmp);
                            llvm::Value* fieldPtr = builder_.CreateStructGEP(
                                entryStTy, tmp, fi, "de.fld");
                            return builder_.CreateLoad(
                                entryStTy->getElementType(fi), fieldPtr,
                                "de.load");
                        }
                    }
                }
                // #628 / stdlib §4.9 v0.135: `e.Key` / `e.Value` on a
                // DrainedEntry<K,V> loop variable (from lowerDictDrainFor).
                // The entry alloca is a {K, V*} struct — same layout as
                // DictionaryEntry: field 0 = Key, field 1 = Value (owned ptr).
                // fix #2794: headName() rather than importedName — same
                // written-vs-synthetic carve-out as DictionaryEntry above.
                if (tt->kind == sema::TypeRef::Kind::Named &&
                    !tt->declaration &&
                    tt->headName() == "DrainedEntry" &&
                    tt->genericArgs.size() >= 2) {
                    if (llvm::AllocaInst* slot = localSlot(*ma->target)) {
                        unsigned fi = ma->member == "Key" ? 0 : 1;
                        llvm::Type* kTy = lowerTypeRef(tt->genericArgs[0]);
                        llvm::Type* vTy = lowerTypeRef(tt->genericArgs[1]);
                        llvm::StructType* entryStTy =
                            llvm::StructType::get(llvm_, {kTy, vTy});
                        // fix #3876 (§27.5 / stdlib §4.9, v0.295): same
                        // capture-cell-holder-vs-heap-cell defect as the
                        // DictionaryEntry arm above — see its comment. Route
                        // through boxedStorage so a captured DrainedEntry
                        // loop variable dereferences the shared cell instead
                        // of GEP'ing the ptr-sized holder alloca directly.
                        llvm::Type* baseElemTy = nullptr;
                        llvm::Value* base = boxedStorage(slot, baseElemTy);
                        llvm::Value* fieldPtr = builder_.CreateStructGEP(
                            entryStTy, base, fi, "dde.fld");
                        return builder_.CreateLoad(
                            entryStTy->getElementType(fi), fieldPtr, "dde.load");
                    }
                }
            }
        }
        llvm::Type* elemTy = nullptr;
        llvm::Value* recvPtr = nullptr;
        if (llvm::Value* addr = fieldAddr(*ma, elemTy, &recvPtr)) {
            llvm::Value* val = builder_.CreateLoad(elemTy, addr);
            // #1039: a fresh `new <class>().valueField` read leaves the receiver
            // temp owned by nothing — dispose it now that the value is copied
            // out (no-op unless the receiver is a fresh `new` of a heap class
            // and the field result is a primitive/enum/unit value). Guarded by
            // !diverged_ so the disposal's control flow is never emitted into a
            // terminated block.
            if (!diverged_)
                disposeFreshNewReceiverTemp(ma->target.get(), recvPtr,
                                            ctx_.exprType(e));
            return val;
        }
        // #213: a qualified static-field read `Type.Count` — static
        // fields have no instance slot, so fieldAddr misses; read the
        // backing global directly.
        if (llvm::GlobalVariable* gv = staticFieldGlobal(e)) {
            // #720 / §22.13 / #1513: lazily initialize the owning type — or,
            // for a generic instantiation's per-instantiation global, the
            // SHARED cross-module instantiation state (§2.8.3 rule 4) — on
            // first use.
            if (auto giIt = genericInstGlobalKeyOf_.find(gv);
                giIt != genericInstGlobalKeyOf_.end()) {
                emitEnsureGenericInstStaticInit(giIt->second);
            } else if (auto* sym = ctx_.exprSymbol(e)) {
                emitEnsureStaticInit(memberOwner(sym));
            }
            if (auto* sym = ctx_.exprSymbol(e)) {
                // #1553 / §22.13 (v0.181): a re-entrant read, on the owner thread,
                // of a static this type's in-progress initializer has not assigned
                // yet raises the cycle backstop rather than observing partial state.
                // (No-op for a generic-instantiation field: initProgressGV_ has no
                // entry for a generic TypeSymbol, matching pre-#1513 behavior.)
                //
                // fix #3774 (§22.13 / §23.1, v0.286): the gate is MemberSymbol,
                // not FieldSymbol — `staticFieldGlobal` resolves a static
                // auto-property's §23.1 backing global at this very site, so a
                // read of one used to skip the guard entirely whoever was
                // reading it and whenever. `staticInitSlotIndex` returns -1 for
                // every member sfinit does not runtime-store (a method symbol
                // reaching here included), so the widened gate stays a no-op
                // everywhere it was one before.
                if (auto* fsym = dynamic_cast<const sema::MemberSymbol*>(sym))
                    emitStaticPartialReadGuard(memberOwner(sym), fsym);
            }
            return builder_.CreateLoad(gv->getValueType(), gv);
        }
        // fix #2547 (sibling of #2477): the third #605 / generic-new(T) silent-
        // zero family shape — a bare MemberAccessExpr read `obj.f` (no parens,
        // not a CallExpr callee) on a concrete-type receiver. The #2477 fix
        // added the specific diagnostic at lowerCall's `if (!ms)` fall-through
        // and at lowerNew's genericSubstSemaTy_ substitution; the CG8 field-read
        // path was not covered. By the time control reaches this point both
        // fieldAddr and staticFieldGlobal have returned null (no instance slot
        // and no backing global), so the only prior signal was the generic
        // #2434 internal warning emitted by lowerLocalDecl's zero-init fallback
        // — which named the slot but not the member or the receiver type, so
        // the slot-degraded-to-zero case was silently wrong. The SPECIFIC
        // complement that names the member and the receiver type is added here.
        // Gated on !hasErrors() to mirror the lowerCall / lowerNew guards
        // (same message-quality rationale: don't stack this on top of an
        // already-diagnosed shape). The pre-existing #2434 generic warning
        // remains unchanged for the existing control test
        // `tests/lit/local_decl_zero_init_fallback_warns_2434.bzl`.
        //
        // §27.6 carve-out: a bare MemberAccessExpr whose sema symbol is a
        // MethodSymbol (a method reference like `a.GetVal` assigned to a
        // function-typed local) is NOT a fall-through — CG60 below
        // (`Class.StaticMethod` / `obj.InstanceMethod` used as a value)
        // builds the closure struct. Without this carve-out, the canonical
        // §27.6 idiom falsely trips the #2547 warning on the first reading.
        // §27.6 (v0.184, #1758) carve-out: same goes for a primitive-
        // intrinsic static-method reference (`int.Parse`, `char.IsDigit`,
        // …) handled by the `isPrimIntrinsicMethodRef` branch immediately
        // below — these have no MethodSymbol (intrinsics are name-recognized)
        // and would otherwise fall through to CG8's nullptr return, falsely
        // tripping this diagnostic on a legitimate method-reference read.
        if (!ctx_.diagnostics().hasErrors() &&
            !dynamic_cast<const sema::MethodSymbol*>(ctx_.exprSymbol(e)) &&
            !isPrimIntrinsicMethodRef(&e)) {
            const sema::TypeRef* recvTy = ma->target
                ? ctx_.exprType(*ma->target) : nullptr;
            using K = sema::TypeRef::Kind;
            using P = sema::PrimitiveType;
            // Shape 1: receiver is a concrete type whose declared member set
            // is known to the type system (Named + declaration, or a primitive)
            // — the silent-zero degradation is observable here. Object is
            // included because Object has no documented members beyond the
            // universal 5; an unknown member on Object is exactly the §3 /
            // #605 case the lowerCall diagnostic also handles (with parens).
            bool isConcreteType = recvTy && (
                (recvTy->kind == K::Named && recvTy->declaration) ||
                (recvTy->kind == K::Primitive &&
                 recvTy->primitive != P::None));
            // The universal Object surface (ToString/GetHashCode/Equals/
            // GetType/Dispose) is always present on a value even when the
            // static type is opaque. Anything else on a concrete type is
            // genuinely undefined.
            bool isUniversalObjMember =
                ma->member == "ToString" ||
                ma->member == "GetHashCode" ||
                ma->member == "Equals" ||
                ma->member == "GetType" ||
                ma->member == "Dispose";
            // Shape 2: bare type parameter in the dead template body (no
            // monomorphization binding for this type parameter) — the
            // abstract-generic-body form per #2477's analysis. Mirrors the
            // lowerCall guard.
            bool isBareTypeParamInDeadTemplate =
                emittingGenericTemplateBody_ &&
                recvTy && recvTy->kind == K::Named &&
                !recvTy->declaration && recvTy->astNode &&
                recvTy->astNode->name &&
                !recvTy->astNode->name->parts.empty() &&
                templateGenericParamNames_.count(
                    recvTy->astNode->name->parts.back());
            if (isConcreteType ? !isUniversalObjMember
                                : isBareTypeParamInDeadTemplate) {
                std::string recvName;
                if (recvTy->kind == K::Primitive &&
                    recvTy->astNode && recvTy->astNode->name &&
                    !recvTy->astNode->name->parts.empty()) {
                    recvName = std::string(
                        recvTy->astNode->name->parts.back());
                } else if (recvTy->declaration) {
                    recvName = recvTy->declaration->name;
                } else {
                    recvName = std::string(recvTy->headName());
                }
                ctx_.diagnostics().report(
                    Severity::Warning, ma->span.begin,
                    "internal: codegen cannot read member '" +
                    std::string(ma->member) + "' on receiver of type '" +
                    recvName + "': permissive acceptance (spec §3 / "
                    "issue #605) admits this site but no concrete "
                    "member dispatch exists (CG8's fall-through "
                    "degrades the slot to zero via lowerLocalDecl's "
                    "initializer-failed fallback — #2547)");
            }
        }
    }
    if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        // §24.3 (#747): a BARE event identifier read in VALUE position (e.g.
        // `return Clicked;` returning the event as an IEvent<T>) yields the
        // bz_event_source_t* — the same borrow the `b.Clicked` member-access form
        // produces. Without this, a bare event in a value position lowered as a
        // field load of garbage (IR_FAIL on the method-returned IEvent<T> form).
        if (eventSymbolOf(e))
            if (llvm::Value* src = eventSourcePtr(e))
                return src;
        // Inject lambda-param type hints so type-gated branches below
        // (string subscript, member access) see the correct type.
        if (!ctx_.exprType(e)) {
            sema::Symbol* sym = ctx_.exprSymbol(e);
            auto hit = lambdaParamTypeHint_.find(sym);
            if (hit != lambdaParamTypeHint_.end())
                ctx_.setExprType(&e, hit->second);
        }
        // CG9: `this` (and `base`, same instance) is the receiver ptr.
        if (id->name == "this" || id->name == "base") return currentThis_;
        // CG33b: a bare computed-property read evaluates its getter
        // against the current `this`.
        if (const ast::Expr* getter = computedGetterOf(ctx_.exprSymbol(e))) {
            // §23.2/§23.3 (#1168): wrap a bare-inner `T` body result to the
            // property's declared `T?` optional (see the qualified `obj.P`
            // read above for the rationale).
            llvm::Value* bv =
                wrapNullableGetterResult(ctx_.exprType(e), lowerExpr(*getter));
            // fix #2761 (§8.6 "Boxing", v0.240): the BARE (`implicit this`)
            // spelling of the same read — `var o := O;` inside the declaring
            // class was silently unboxed too. See the qualified path above.
            bv = boxForTarget(bv, ctx_.exprType(e), getter);
            // fix #2815 / §23.2 / §29.2 (v0.240): the bare-`this` spelling of
            // #2815 — `for var x in P` where `P` is a property of the current
            // class declared `IEnumerable<T>` and `=> _items` / `_items` field
            // shares the same crash as the qualified `obj.P` path. See the
            // qualified site above for the full rationale and the no-op
            // guard list (the helper does nothing when the declared type is
            // not a pure IEnumerable-family interface or the getter's type is
            // not array-backed, so the bare read keeps its existing shape for
            // every other property).
            llvm::Value* result =
                wrapPropertyArrayAsIEnumerable(bv, ctx_.exprType(e), getter, &e);
            // fix issue 4193 (stdlib §8.1/§23.2-§23.3, v0.298): record the
            // footprint right after this getter's own evaluation completes —
            // see `postGetterFootprint_`'s declaration for why a bare
            // getter-backed identifier is NOT "one non-side-effecting load"
            // for `emittedSideEffectSince(dispatchEntry)`'s purposes, and why
            // anchoring here (rather than at `dispatchEntry`) lets a caller
            // tell "the getter ran" apart from "the MEMBER dispatched".
            postGetterFootprint_[&e] = irFootprint();
            return result;
        }
        // CG33b: block-form getter (`get => do … end;`) accessed bare.
        if (const ast::PropertyAccessor* acc = blockGetterOf(ctx_.exprSymbol(e))) {
            auto* ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(e));
            auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
            llvm::Function* fn = ensurePropertyBlockGetter(pd, acc);
            llvm::Value* res =
                builder_.CreateCall(fn->getFunctionType(), fn, {currentThis_});
            // §31.1/§31.2 (FINDING [4]): see the qualified block-getter read
            // above — a throw inside the standalone getter must propagate.
            emitPendingCheck();
            // fix issue 4193: see the `computedGetterOf` arm above.
            postGetterFootprint_[&e] = irFootprint();
            return res;
        }
        // CG12: a bare identifier resolving to a static field reads
        // its backing global.
        if (llvm::GlobalVariable* gv = staticFieldGlobal(e)) {
            // #720 / §22.13 / #1513: lazily initialize the owning type — or,
            // for a generic instantiation's per-instantiation global, the
            // SHARED cross-module instantiation state (§2.8.3 rule 4) — on
            // first use.
            if (auto giIt = genericInstGlobalKeyOf_.find(gv);
                giIt != genericInstGlobalKeyOf_.end()) {
                emitEnsureGenericInstStaticInit(giIt->second);
            } else if (auto* sym = ctx_.exprSymbol(e)) {
                emitEnsureStaticInit(memberOwner(sym));
            }
            if (auto* sym = ctx_.exprSymbol(e)) {
                // #1553 / §22.13 (v0.181): a re-entrant read, on the owner thread,
                // of a static this type's in-progress initializer has not assigned
                // yet raises the cycle backstop rather than observing partial state.
                // (No-op for a generic-instantiation field: initProgressGV_ has no
                // entry for a generic TypeSymbol, matching pre-#1513 behavior.)
                //
                // fix #3774 (§22.13 / §23.1, v0.286): the gate is MemberSymbol,
                // not FieldSymbol — `staticFieldGlobal` resolves a static
                // auto-property's §23.1 backing global at this very site, so a
                // read of one used to skip the guard entirely whoever was
                // reading it and whenever. `staticInitSlotIndex` returns -1 for
                // every member sfinit does not runtime-store (a method symbol
                // reaching here included), so the widened gate stays a no-op
                // everywhere it was one before.
                if (auto* fsym = dynamic_cast<const sema::MemberSymbol*>(sym))
                    emitStaticPartialReadGuard(memberOwner(sym), fsym);
            }
            return builder_.CreateLoad(gv->getValueType(), gv);
        }
        // CG9: a bare identifier resolving to an instance field is an
        // implicit-`this` field read.
        llvm::Type* elemTy = nullptr;
        if (llvm::Value* addr = implicitFieldAddr(e, elemTy)) {
            return builder_.CreateLoad(elemTy, addr);
        }
        // #625 / §12 / §31: bare `Message` inside a user class that inherits
        // a built-in exception — sema resolved it as the inherited Exception
        // property (no FieldSymbol, type=string).  Emit the same GEP-at-byte-8
        // read as `this.Message` in the qualified MemberAccessExpr path above
        // (CG22+): the message pointer lives at byte offset 8 in the exception
        // object layout regardless of whether the receiver is the builtin class
        // itself or a user-derived class (§31 / classStruct layout rule).
        //
        // fix #2971 (§31.4 / §12.1 / §11.3, v0.240): the other three §31.4
        // members reach here the same way, through the same sema arm — this
        // one read `id2->name == "Message"` and so covered a quarter of the
        // surface. The gate is unchanged in shape (the name, NO resolved
        // symbol — a local/field/parameter of the same spelling carries one and
        // is excluded — and a live `this`); only the name set widened, and each
        // member reads its own fixed §31.4 byte offset out of the header
        // `currentThis_` points at.
        if (auto* id2 = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
            const bool bareExcSurface =
                id2->name == "Message" || id2->name == "StackTrace" ||
                id2->name == "CauseChain" || id2->name == "ParamName";
            if (bareExcSurface && !ctx_.exprSymbol(e) && currentThis_) {
                auto* i8Ty = llvm::Type::getInt8Ty(llvm_);
                // §31.4 `StackTrace: string` — the "" stub (no debug info yet),
                // the same value the qualified spellings produce.
                if (id2->name == "StackTrace")
                    return lowerStringLiteral("\"\"", false);
                if (id2->name == "CauseChain") {
                    // §31.4 `CauseChain: Array<ExceptionInfo>` @24 — null reads
                    // as the empty chain (bz_array_len(null) == 0).
                    llvm::Value* ccSlot = builder_.CreateGEP(
                        i8Ty, currentThis_,
                        builder_.getInt64(kExcCauseOffset), "cc.slot");
                    return builder_.CreateLoad(ptrTy(), ccSlot, "cc.val");
                }
                if (id2->name == "ParamName") {
                    // §31.4 `ParamName: string?` @32 — null when not supplied,
                    // which is the property's own declared value, so NO
                    // empty-string substitution here (unlike Message).
                    llvm::Value* pnSlot = builder_.CreateGEP(
                        i8Ty, currentThis_,
                        builder_.getInt64(kExcParamOffset), "pn.slot");
                    return builder_.CreateLoad(ptrTy(), pnSlot, "pn.val");
                }
                llvm::Value* msgSlot = builder_.CreateGEP(
                    i8Ty, currentThis_, builder_.getInt64(8), "msg.slot");
                llvm::Value* msgPtr =
                    builder_.CreateLoad(ptrTy(), msgSlot, "msg.ptr");
                auto* isNull = builder_.CreateICmpEQ(
                    msgPtr,
                    llvm::ConstantPointerNull::get(ptrTy()), "msg.isnull");
                llvm::Value* emptyStr = lowerStringLiteral("\"\"", false);
                return builder_.CreateSelect(
                    isNull, emptyStr, msgPtr, "msg.val");
            }
        }
        // fix #3090 (§12.7 / §8.6, v0.252): a read through a §12.7 narrowing to
        // a VALUE type yields the boxed value — §8.6's "a boxed value, when
        // unboxed, yields the original", and §17.4's "copying the value out".
        // Placed ahead of the two storage arms below because those load the
        // `Object` SLOT, whose bytes are the box POINTER: `var i: int := o;`
        // inside `if o is int then … end` stored that pointer into an i32-typed
        // read (silent under opaque pointers — `store ptr` + `load i32` through
        // one alloca is valid IR, so verifyModule could not see it) and printed
        // the low 32 bits of a heap address. Declines for every read that is not
        // a §12.7 value-narrowing, so the ordinary local/param load is unchanged.
        if (llvm::Value* unboxed = unboxNarrowedRead(e)) return unboxed;
        // CG50: a ref/out parameter — load through the incoming pointer.
        llvm::Type* refElem = nullptr;
        if (llvm::Value* a = refParamAddr(e, refElem)) {
            return builder_.CreateLoad(refElem, a);
        }
        // §22.16 (v0.250, #3038): the contextual keyword `result` inside an
        // `ensures` clause. Sema binds a synthetic LocalSymbol with the
        // declared return type; codegen recognises it here and loads from
        // `ensuresResultSlot_` (already populated by emitReturn at every
        // normal-completion path). The slot lives in the entry block, so the
        // load in the epilogue sees the final return value bound. Sema
        // already rejected `result` outside ensures, so any LocalSymbol
        // named "result" reaching codegen is the §22.16 borrow.
        if (auto* ls =
                dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(e))) {
            if (ls->name == "result" && ensuresResultSlot_) {
                return builder_.CreateLoad(
                    ensuresResultSlot_->getAllocatedType(),
                    ensuresResultSlot_, "result");
            }
        }
        // CG3: a reference to a local variable loads from its slot.
        // §27.5: a captured ("boxed") local reads through its shared cell.
        if (llvm::AllocaInst* slot = localSlot(e)) {
            llvm::Type* ety = nullptr;
            llvm::Value* addr = boxedStorage(slot, ety);
            return builder_.CreateLoad(ety, addr);
        }
        // CG60: a bare identifier resolving to a static method —
        // produce a closure struct { thunk_ptr, null_env } so it can
        // be passed as a function-typed value (§A.7 / §17.14).
        // §27.6/§24.4, #880 #881: a bare identifier resolving to an INSTANCE
        // method used in value position binds the implicit `this` receiver,
        // exactly as `this.MethodName` does in the qualified MemberAccessExpr
        // path below. Without this, the env block held a null/garbage receiver
        // — a silent no-op for event handlers (#880) and a SIGSEGV at the
        // call site (#881). Route through makeMethodThunk with currentThis_.
        if (auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(e))) {
            // A bare (implicit-`this`) reference has no receiver EXPRESSION, so
            // there is no static interface type to instantiate from; the #2766
            // substitution simply does not apply and the erased signature is
            // used, exactly as before.
            if (isStaticMethod(ms)) return makeMethodThunk(ms, nullptr);
            else if (currentThis_) return makeMethodThunk(ms, currentThis_);
        }
    }
    // CG60: `Class.StaticMethod` or `obj.InstanceMethod` used as a
    // value (not called). Produce a closure struct for it.
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        // fix #2781 / §8.1 / §14.2: a method-group reference to Object's
        // three universal instance members on an Object-typed receiver —
        // `o.ToString`, `o.Equals`, `o.GetHashCode` — has no MethodSymbol
        // bound (Object is a primitive; its virtuals are runtime intrinsics).
        // The CG60 fallthrough used to return nullptr, silently zero-initing
        // the closure cell (so `var f := o.GetHashCode;` made `f()` return
        // 0; `var f := o.ToString` ICEd inside an interpolation on mistyped
        // ICmp). Mint a thunk closure here that mirrors lowerObjectToString /
        // lowerObjectEquals / lowerObjectGetHashCode — null → §8.3/§8.4/§8.5
        // default; bz_string* → string intrinsic (+ retain for ToString);
        // class/box → vtable[fixed slot] dispatch to the override or the
        // per-class default thunk. GetType (§8.2) is non-virtual — left
        // alone here; ReferenceEquals (§8.4) is a static method — likewise.
        if (ma->target) {
            const sema::TypeRef* recvTy = ctx_.exprType(*ma->target);
            if (recvTy && recvTy->kind == sema::TypeRef::Kind::Primitive &&
                recvTy->primitive == sema::PrimitiveType::Object) {
                std::string_view m = ma->member;
                if (m == "ToString" || m == "Equals" || m == "GetHashCode") {
                    // fix #2781 / §8.1 / §27.6: build the Function TypeRef for
                    // the §8.1 universal signature and bind the MA's exprType
                    // AND — when this MA is the direct initializer of a local
                    // (`var f := o.GetHashCode;`) — the local's declType to it.
                    // Without the declType patch, `f()`'s lowerCall arm (line
                    // ~4770 of LlvmBackend_Calls2.cpp) sees a null declType on
                    // the LocalSymbol and silently degrades to the #2434
                    // zero-init — the closure cell is BUILT but never called.
                    sema::TypeRef* fnTy = ctx_.makeTypeRef();
                    fnTy->kind = sema::TypeRef::Kind::Function;
                    sema::TypeRef* objTy = ctx_.makeTypeRef();
                    objTy->kind = sema::TypeRef::Kind::Primitive;
                    objTy->primitive = sema::PrimitiveType::Object;
                    objTy->nullable = true;
                    sema::TypeRef* retTy = ctx_.makeTypeRef();
                    if (m == "ToString") {
                        retTy->kind = sema::TypeRef::Kind::Primitive;
                        retTy->primitive = sema::PrimitiveType::String;
                        fnTy->funcReturn = retTy;
                    } else if (m == "Equals") {
                        retTy->kind = sema::TypeRef::Kind::Primitive;
                        retTy->primitive = sema::PrimitiveType::Bool;
                        fnTy->funcReturn = retTy;
                        sema::TypeRef::FuncParam fp;
                        fp.type = objTy;
                        fnTy->funcParams.push_back(std::move(fp));
                    } else {  // GetHashCode
                        retTy->kind = sema::TypeRef::Kind::Primitive;
                        retTy->primitive = sema::PrimitiveType::Int;
                        fnTy->funcReturn = retTy;
                    }
                    ctx_.setExprType(&e, fnTy);
                    // Sema left the binding local's declType null because the
                    // MemberAccessExpr's resolved symbol was null. Restore it
                    // here from the exprType we just set so `f()` dispatches
                    // through emitClosureCall. Walk ctx_.symbols() and patch
                    // any LocalSymbol whose LocalDecl's initializer IS this
                    // exact MA — a small linear scan, O(symbols) per Object-
                    // member method-group reference, and a one-time cost at
                    // first use (subsequent call sites of the same MethodGroup
                    // hit the #3153 reject less because the existing reject
                    // already gates `loc->declType->kind != Function`). The
                    // LocalSymbol::declType field is mutable (Symbol.hpp); a
                    // post-sema codegen-only patch is safe — sema never reads
                    // it again after the LocalDecl initializer is typechecked.
                    for (const auto& sym : ctx_.symbols()) {
                        auto* ls = dynamic_cast<sema::LocalSymbol*>(sym.get());
                        if (!ls || !ls->decl) continue;
                        auto* ld = dynamic_cast<const ast::LocalDecl*>(ls->decl);
                        if (!ld || ld->initializer.get() != &e) continue;
                        ls->declType = fnTy;
                        break;
                    }
                    if (llvm::Value* oc = makeObjectMemberThunk(*ma, m))
                        return oc;
                }
            }
        }
        if (auto* ms = dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(e))) {
            llvm::Value* recv = nullptr;
            // §27.6 (#1306 rework): an extension-method reference is EITHER
            // unbound (`Vec.Scaled`, type-name qualifier — the receiver is the
            // function's first parameter, §27.6) or bound (`x.Double`, instance
            // qualifier — the receiver is captured, #457). For a type-name
            // qualifier, lowerExpr yields null (no value to lower); for an
            // instance qualifier, it yields the receiver value. makeMethodThunk
            // uses the null-vs-value distinction to route the two cases:
            // extRef = isExtensionMethod && !receiverVal. So lower the target
            // uniformly for every non-static method ref.
            if (!isStaticMethod(ms) && ma->target)
                recv = lowerExpr(*ma->target);
            // fix #2766: hand the RECEIVER's static type to makeMethodThunk. For
            // `eq.Equals` with `eq: IEquatable<string>` that is the only place
            // the type ARGUMENT (`string`) is recorded — sema leaves the
            // reference's own function-type parameters null — and the thunk's
            // ABI has to match it, not the interface's erased `T`.
            const sema::TypeRef* recvTy =
                (!isStaticMethod(ms) && ma->target) ? ctx_.exprType(*ma->target)
                                                    : nullptr;
            return makeMethodThunk(ms, recv, recvTy);
        }
        // §27.6 (v0.184, #1758): a bare reference to a primitive-intrinsic static
        // method (`int.Parse`, `char.IsDigit`, …) — no MethodSymbol (intrinsics
        // are name-recognized) — that sema typed as a function value. Back it with
        // a synthesized thunk closure instead of falling through to `ret nullptr`
        // (which produced a null fn pointer → SIGSEGV when the pipeline invoked
        // it). isPrimIntrinsicMethodRef keys the disposal sites to this same set.
        if (isPrimIntrinsicMethodRef(&e))
            return makePrimIntrinsicThunk(*ma, ctx_.exprType(e));
    }
    return nullptr;
}


// #481 / §12.1 / §12.5 / §13 / §23: dynamic dispatch of a virtual/interface
// property GETTER read. Mirrors the virtual/interface METHOD dispatch in
// LlvmBackend_Calls.cpp::emitMethodCall — load the receiver's vtable (class) or
// look up the per-interface itable, GEP to the accessor's slot, load the fn ptr,
// and call `T get(ptr this)`.
llvm::Value* Emitter::lowerVirtualPropertyGet(const ast::Expr& e) {
    // The receiver is either an explicit `obj.P` target, or — for a bare `P`
    // read inside an instance member — the current `this`. A static-property
    // read (receiver is a type name) has no `this`; it is excluded below by the
    // requirement that the lowered receiver be a pointer value.
    const ast::Expr* recvExpr = nullptr;
    sema::PropertySymbol* ps = nullptr;
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&e)) {
        if (!ma->target) return nullptr;
        ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*ma));
        recvExpr = ma->target.get();
        // `base.P` (§12.3): bypass dispatch — the static getter path handles it.
        if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(recvExpr))
            if (tid->name == "base") return nullptr;
    } else if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&e)) {
        ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*id));
        // bare `P` → implicit `this.P`; no recvExpr (currentThis_ is the recv).
    } else {
        return nullptr;
    }
    if (!ps || ps->isIndexer) return nullptr;

    bool isIfaceProp = ps->parent &&
                       ps->parent->kind == ast::TypeDeclKind::Interface;
    auto getIt = propGetSlot_.find(ps);
    auto ifGetIt = ifacePropertyGetIndex_.find(ps);
    if (!isIfaceProp && getIt == propGetSlot_.end()) return nullptr;
    if (isIfaceProp && ifGetIt == ifacePropertyGetIndex_.end()) return nullptr;

    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    // §2.5 / §22.9 / §12.1 (issue #503): for an IMPORTED virtual/override
    // property, ps->decl is null (no ast::PropertyDecl in the importer). The
    // vtable slot was still populated by buildVtable (which ran for imported
    // classes too); we only need to derive the return type, which rides the
    // importedMemberType side table (set by the loader from MetaMember::type).
    //
    // fix #2944 (§12.1 / §14.1 / §23, v0.240): the call-site TYPE MUST be
    // computed from the sema-resolved expr type — not from the AST `pd->type` —
    // when the static type of the receiver BINDs a generic parameter the base
    // declared `T` against. Otherwise `g.Item` where `g: GBase<string>` lowers
    // the call's expected return type from the base declaration's bare `T`
    // (which `lowerType` resolves to the i32 ERASED PLACEHOLDER, because no
    // active genericSubst_ binds T here) while the per-instantiation vtable
    // slot holds a spec body whose actual return type is `string` (`ptr`) —
    // LLVM IR verifier aborts the build with "Function return type does not
    // match operand type of return inst!" (and the read of the returned i32
    // as a `%bz.string` base pointer fails first, "GEP base pointer is not a
    // vector or a vector of pointers"). `ctx_.exprType(e)` is the post-
    // substitution sema type (§14.1) — T is already bound to `string` here —
    // and `lowerTypeRef` on it returns `ptr`, matching the slot. The AST
    // `pd->type` fallback is kept for the case where sema did not annotate
    // the expression (no static type), and for IMPORTED properties whose
    // `pd` is null (where the importedMemberType side table IS the post-
    // substitution carrier — same idea, different mechanism).
    llvm::Type* valTy = nullptr;
    if (sema::TypeRef* resolved = ctx_.exprType(e)) {
        valTy = lowerTypeRef(resolved);
    }
    if (!valTy) {
        if (pd) {
            valTy = lowerType(pd->type.get());
        } else {
            // Imported property: get the resolved type from the side table.
            if (sema::TypeRef* tr = ctx_.importedMemberType(ps))
                valTy = lowerTypeRef(tr);
        }
    }
    if (!valTy) valTy = i32();
    auto* fnTy = llvm::FunctionType::get(valTy, {ptrTy()}, false);

    llvm::Value* recv = recvExpr ? lowerExpr(*recvExpr) : currentThis_;
    // §14.2 (record-conforms-to-interface) + §9.4 (record = value type) /
    // §23 (properties): when a generic method `M<T>(x: T) where T: IFoo` is
    // monomorphized with T = a RECORD and the body reads `x.Prop` (an
    // interface property), sema resolves `x.Prop` to the INTERFACE property
    // `IFoo.Prop`. The receiver `x` is a record VALUE (a struct, not a
    // pointer), so the itable-dispatch path below would either need a pointer
    // receiver or return nullptr at the `!recv->getType()->isPointerTy()`
    // guard — and the fallback paths (computedGetterOf/blockGetterOf) see the
    // INTERFACE property symbol (which has no body) and also return nullptr →
    // the property access silently returns 0. This is the property analog of
    // the record-method devirtualization in lowerCall (commit 738a3aa, §14.2
    // record constraint fix). Since T is concretely known at the
    // monomorphization site, devirtualize: resolve the receiver's concrete
    // type through the active generic substitution; if it is a record, redirect
    // `ps` to the record's own property via classPropertyByName and lower the
    // getter directly (spilling the record value as needed — the same path a
    // non-generic `square.Area` uses). Records are sealed (§9.6), so the
    // implementation is exactly the one declared on the record.
    if (isIfaceProp && recv && !recv->getType()->isPointerTy() && recvExpr) {
        const sema::TypeRef* recvTy = ctx_.exprType(*recvExpr);
        // Resolve a bare type-parameter receiver (`T`, no declaration) through
        // the active substitution to its concrete sema type.
        const sema::TypeRef* concrete = recvTy;
        if (recvTy && recvTy->kind == sema::TypeRef::Kind::Named &&
            !recvTy->declaration && recvTy->astNode &&
            recvTy->astNode->name &&
            recvTy->astNode->name->parts.size() == 1) {
            auto sit = genericSubstSemaTy_.find(
                recvTy->headName());
            if (sit != genericSubstSemaTy_.end() && sit->second)
                concrete = sit->second;
        }
        if (concrete && concrete->declaration &&
            concrete->declaration->kind == ast::TypeDeclKind::Record) {
            // Redirect ps to the record's own implementing property.
            if (sema::PropertySymbol* impl = classPropertyByName(
                    concrete->declaration, ps->name)) {
                // Spill the record value to a stack slot so the getter body
                // can access fields via `this` pointer (§9.2 record storage).
                llvm::Value* obj = recv;
                if (!obj->getType()->isPointerTy()) {
                    llvm::AllocaInst* tmp = entryAlloca(obj->getType());
                    builder_.CreateStore(obj, tmp);
                    obj = tmp;
                }
                // Evaluate the record's own getter with `this` = spilled ptr.
                if (const ast::Expr* getter = computedGetterOf(impl)) {
                    llvm::Value* saved = currentThis_;
                    currentThis_ = obj;
                    llvm::Value* v = lowerExpr(*getter);
                    // fix #2761 (§8.6 "Boxing" / §14.2, v0.240): the
                    // record-devirtualised spelling of the inline
                    // expression-getter read. `ctx_.exprType(e)` is the
                    // INTERFACE property's declared type at this use site; when
                    // that is `Object`/an interface and the record's body
                    // yields a value type, the read owes a box exactly as the
                    // direct path does.
                    v = boxForTarget(v, ctx_.exprType(e), getter);
                    currentThis_ = saved;
                    return v;
                }
                if (const ast::PropertyAccessor* acc = blockGetterOf(impl)) {
                    auto* ipd = dynamic_cast<const ast::PropertyDecl*>(impl->decl);
                    if (ipd) {
                        llvm::Function* getterFn =
                            ensurePropertyBlockGetter(ipd, acc);
                        llvm::Value* res = builder_.CreateCall(
                            getterFn->getFunctionType(), getterFn, {obj});
                        emitPendingCheck();
                        return res;
                    }
                }
            }
            // #815 / §9.1: the implementation may be a POSITIONAL record field
            // (which generates a read-only property but is a FieldSymbol, not a
            // PropertySymbol). Spill the record and load the field directly.
            if (sema::FieldSymbol* pfs = classPositionalFieldProperty(
                    concrete->declaration, ps->name)) {
                llvm::Value* obj = recv;
                if (!obj->getType()->isPointerTy()) {
                    llvm::AllocaInst* tmp = entryAlloca(obj->getType());
                    builder_.CreateStore(obj, tmp);
                    obj = tmp;
                }
                llvm::StructType* rst = classStruct(concrete->declaration);
                auto idxIt = fieldIndex_.find(pfs);
                if (rst && idxIt != fieldIndex_.end()) {
                    llvm::Value* slot = builder_.CreateStructGEP(
                        rst, obj, idxIt->second);
                    return builder_.CreateLoad(valTy, slot);
                }
            }
        }
    }
    if (!recv) return nullptr;
    // issue #490 / §14.2: in the UNSPECIALIZED generic template body a type
    // parameter T is erased to i32. A constraint property `x.Name` (x: T)
    // dispatches through an itable/vtable that requires a pointer receiver, but
    // `recv` is i32 here. The record-devirtualization block above (lines
    // 1692-1739) already ran and found no concrete record substitution (the
    // unspecialized body has no active genericSubstSemaTy_), so we fell
    // through. Returning nullptr propagates to lowerLocalDecl, which then picks
    // i32 as the slot type for an inferred-type local `var n := x.Name`; a
    // subsequent `n.Length` or any string/class member call on `n` then emits
    // a malformed IR instruction (string GEP with an i32 base) → LLVM
    // assertion / bezelc crash.
    //
    // Fix: return llvm::UndefValue::get(valTy) — a properly-typed placeholder.
    // This gives lowerLocalDecl the correct slot type (ptr for string, ptr for
    // any class reference) even in the unspecialized stub, so `n.Length` emits
    // well-formed IR. The stub body is never executed (the specialization path
    // in emitGenericSpecialization compiles a concrete version with a real ptr
    // receiver), so the undef value is harmless. This mirrors the #484 guard
    // in emitMethodCall/emitVirtualDispatch (which returns nullptr for a non-ptr
    // receiver, but there the nullptr only affects the call return value, not
    // a local's slot type — the local's slot type is correct because the
    // property-result path was the gap).
    if (!recv->getType()->isPointerTy()) return llvm::UndefValue::get(valTy);

    llvm::Value* fnPtr = nullptr;
    if (isIfaceProp) {
        // CG35-itable (§13): itab lookup then GEP to the property's index.
        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recv);
        auto* lookupFn = declareC("bz_itable_lookup", ptrTy(),
                                  {ptrTy(), ptrTy()});
        llvm::Value* itmapPtr = builder_.CreateLoad(
            ptrTy(),
            builder_.CreateGEP(ptrTy(), vptr, {builder_.getInt32(1)}));
        auto descIt = descArray_.find(ps->parent);
        if (descIt == descArray_.end()) return nullptr;
        llvm::Value* itab = builder_.CreateCall(
            llvm::FunctionType::get(ptrTy(), {ptrTy(), ptrTy()}, false),
            lookupFn, {itmapPtr, descIt->second}, "itab");
        unsigned k = ifGetIt->second;
        fnPtr = builder_.CreateLoad(
            ptrTy(),
            builder_.CreateGEP(ptrTy(), itab, {builder_.getInt32(k)}), "pget");
    } else {
        // Class virtual dispatch: vt[slot].
        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recv);
        llvm::Value* slotPtr = builder_.CreateGEP(
            ptrTy(), vptr, {builder_.getInt32(getIt->second)});
        fnPtr = builder_.CreateLoad(ptrTy(), slotPtr);
    }
    // fix #2816 / §28.3 "Unbound owned temporaries" (v0.240): establish the
    // temporary's disposal BEFORE the dispatched accessor runs, so its throw
    // edge (the emitPendingCheck immediately below) unwinds through it. Placed
    // here rather than at `recv`'s own lowering (line ~5022) because every
    // early return between the two — the non-pointer guard, the missing
    // type-descriptor guard — hands the read back to the static getter paths,
    // which lower the receiver again; registering before them could strand an
    // entry for a receiver this path no longer owns. A property read takes no
    // arguments, so the accessor call is the whole throw window and nothing is
    // lost by registering at its edge.
    llvm::AllocaInst* recvUnwindSlot = registerFreshNewReceiverTempUnwind(
        recvExpr, recv, ctx_.exprType(e), /*calleeReturnsOwned=*/false);
    llvm::Value* r = builder_.CreateCall(fnTy, fnPtr, {recv});
    emitPendingCheck();  // the accessor body may throw (§23.3)
    // fix #2751 / §28.3 "Unbound owned temporaries" (v0.240) — THE SHARED
    // RATIONALE for all four property-read sites (this one, and the computed-
    // getter / block-getter / imported-getter paths in lowerExprInner).
    //
    // §28.3 enumerates the member read among the borrowing uses that produce an
    // unbound owned temporary: "An owned value may also be produced as an
    // **unbound temporary**: the result of an `owned`-returning call (or other
    // owned producer) that is not bound to any name, not `move`d, and not
    // returned, but used only by *borrowing* operations within the expression —
    // as the receiver of a further call, as a non-`owned` argument, as a member
    // or index access, or as the scrutinee of a `case` … Such a temporary is
    // disposed at the **end of the full expression** in which it is produced,
    // after every use of it within that expression has completed."
    //
    // #1039 covered exactly two of the member-read shapes: the FIELD read
    // (fieldAddr path in lowerExprInner) and the METHOD call (emitMethodCall).
    // A PROPERTY read is neither — it resolves to a PropertySymbol and is
    // lowered by one of the four getter paths, none of which disposed the
    // receiver — so `(new Src()).N` leaked the `Src` in every position tested
    // (local-variable initializer, field initializer, bare statement), for the
    // arrow-bodied, block-bodied and virtual getter forms alike. Only the AUTO
    // property (`property N: int := 5;`) was already covered, because it is
    // backed by a field and travels the #1039 field path.
    //
    // SOUNDNESS — the disposal is delegated whole to disposeFreshNewReceiverTemp,
    // which fires only when the receiver is a fresh owned-class producer AND the
    // extracted result is a non-reference value (numeric/bool/char/decimal
    // primitive, enum, unit). `calleeReturnsOwned` is FALSE at every property
    // site by construction: §A.3's property grammar has no `owned` marker on a
    // property type (only `StaticReadonlyDecl` carries one), so a property read
    // can never announce a fresh transfer, and the #1859/#2205 class/interface/
    // owning-collection widenings stay unreachable from here. That keeps this
    // fix strictly inside the provably-safe subset and away from #2261's
    // use-after-free shape (a deferred iterator/task frame that still borrows a
    // freed receiver): a property returning `string`, a class reference, a span,
    // an inline array, a record, `Object`, or an iterable/`Task` interface is
    // NOT disposed here — it keeps leaking, which is the sound direction.
    //
    // Emitted after emitPendingCheck, so it lands in the normal-return
    // continuation only: on the accessor's throw edge the temporary leaks, the
    // same pre-existing safe direction the argument string-release at
    // emitMethodCall (LlvmBackend_Calls2.cpp) documents. `recvExpr` is null for
    // a bare `P` read (implicit `this`), where the helper returns immediately.
    if (!diverged_)
        disposeFreshNewReceiverTemp(recvExpr, recv, ctx_.exprType(e),
                                    /*calleeReturnsOwned=*/false,
                                    recvUnwindSlot);
    return r;
}


// #481: dynamic dispatch of a virtual/interface property SETTER for
// `obj.P := v`, with the already-lowered value `v`. Mirrors the getter dispatch
// above; the call is `void set(ptr this, T value)`.
bool Emitter::lowerVirtualPropertySet(const ast::Expr& target, llvm::Value* v,
                                      const ast::Expr* valueExpr,
                                      bool valueIsPlusOne) {
    if (!v) return false;
    // fix #2849 (§12.1/§6.6/§23.3, v0.240): ACCEPT THE RECEIVER-LESS TARGET.
    //
    // The receiver is either an explicit `obj.P` target, or — for a bare `P`
    // write inside an instance member — the current `this`. This function used
    // to require an `ast::MemberAccessExpr` and bail on anything else, while
    // its getter twin `lowerVirtualPropertyGet` (above) deliberately handles
    // BOTH spellings. That asymmetry was the whole defect: a bare `P` target is
    // an `ast::IdentifierExpr`, so every caller fell through to the STATICALLY
    // bound setter and ran the compile-time type's accessor.
    //
    // §12.1, verbatim: "A method, property, or indexer is non-virtual unless
    // marked `virtual`. To override a virtual member in a derived class, mark
    // the matching member `override`."
    //
    // MEASURED on b0fa41a with a `virtual`/`override` pair whose accessors are
    // +100/+1 (base) and +200/+2 (derived), receiver always a `D`:
    //   * `P := 7`   stored 8 where 9 is correct — the BASE setter ran.
    //   * `P += 1`   yielded 202 where 203 is correct — the diagnostic value:
    //     the getter DISPATCHED (D's 200, +1 = 201) and the setter did NOT
    //     (B's `value + 1` = 202). The two halves of one statement disagreed.
    // `this.P := 7` / `this.P += 1` / `b.P += 1` were already correct, so the
    // defect is specific to the receiver-less spelling — the most idiomatic
    // way to write to one's own property.
    //
    // A STATIC property cannot reach the dispatch below even inside an instance
    // member (where `currentThis_` is a live pointer): §12.1, verbatim,
    // "Fields, constructors, destructors, and static members may not be
    // virtual", and buildVtable's `isVirtualProperty`/`isVirtualIndexer` gate
    // (LlvmBackend_Setup.cpp) is what populates propSetSlot_ — so a static
    // property has no slot and the `setIt == propSetSlot_.end()` guard below
    // declines it. Interfaces declare no static members, so the itable half is
    // likewise unreachable. The getter twin relies on exactly this.
    const ast::Expr* recvExpr = nullptr;
    sema::PropertySymbol* ps = nullptr;
    if (auto* ma = dynamic_cast<const ast::MemberAccessExpr*>(&target)) {
        if (!ma->target) return false;
        ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*ma));
        recvExpr = ma->target.get();
        // `base.P` (§12.3, verbatim: "Inside a derived member, `base.Name(...)`
        // calls the base class's implementation, bypassing virtual dispatch"):
        // decline — the static setter path handles it.
        if (auto* tid = dynamic_cast<const ast::IdentifierExpr*>(recvExpr))
            if (tid->name == "base") return false;  // §12.3
    } else if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(&target)) {
        ps = dynamic_cast<sema::PropertySymbol*>(ctx_.exprSymbol(*id));
        // bare `P` → implicit `this.P`; no recvExpr (currentThis_ is the recv).
    } else {
        return false;
    }
    if (!ps || ps->isIndexer) return false;

    bool isIfaceProp = ps->parent &&
                       ps->parent->kind == ast::TypeDeclKind::Interface;
    auto setIt = propSetSlot_.find(ps);
    auto ifSetIt = ifacePropertySetIndex_.find(ps);
    if (!isIfaceProp && setIt == propSetSlot_.end()) return false;
    if (isIfaceProp && ifSetIt == ifacePropertySetIndex_.end()) return false;

    auto* pd = dynamic_cast<const ast::PropertyDecl*>(ps->decl);
    // fix #2406 (§2.5 / §22.9 / §12.1, mirroring lowerVirtualPropertyGet's
    // #503 fallback above): for an IMPORTED interface's own read-write
    // PropertySymbol, ps->decl is null (no ast::PropertyDecl in the
    // importer). propertyHasSetter's #2406 companion fix now reserves an
    // itable setter slot for such a property (ifacePropertySetIndex_
    // contains it, so the `!isIfaceProp && ...` guard above admits us here),
    // so this function must be able to derive the value type without `pd`
    // too, instead of bailing out — the value type rides the
    // importedMemberType side table, exactly like the getter already reads.
    llvm::Type* valTy = nullptr;
    if (pd) {
        valTy = lowerType(pd->type.get());
    } else {
        if (sema::TypeRef* tr = ctx_.importedMemberType(ps))
            valTy = lowerTypeRef(tr);
    }
    if (!valTy) valTy = i32();
    auto* fnTy = llvm::FunctionType::get(
        llvm::Type::getVoidTy(llvm_), {ptrTy(), valTy}, false);

    // fix #2849: `recvExpr` is null for the bare `P` spelling — the receiver is
    // then `currentThis_`, exactly as in lowerVirtualPropertyGet. The
    // pointer-typed requirement below is what excludes a static context (no
    // `this`) and the erased-generic receiver, as it does in the getter.
    llvm::Value* recv = recvExpr ? lowerExpr(*recvExpr) : currentThis_;
    if (!recv || !recv->getType()->isPointerTy()) return false;
    // fix #2927 (§7.2/§5.1/§12.1/§23.3, v0.240): coerce the incoming value to
    // the SETTER SLOT, not merely through §5.1's numeric widening.
    //
    // This site used to be a bare `coerceNumericToType(v, valTy, …)`, which
    // handles §5.1's widening (`i.LongP := 5` and `i.DblP := 7` through an
    // interface were both MEASURED correct on 10bd6d3 — the issue's inferred
    // §5.1 sibling does NOT reproduce) but has no case for a NULLABLE VALUE
    // slot. §7.2 makes that slot the tagged `{i8 hasValue; T}` struct while
    // the RHS arrives as the bare payload `T`, so `i.IP := 4` on an `int?`
    // property handed `i32 4` to a setter declared `void(ptr, {i8, i32})` and
    // ABORTED THE COMPILE: "Call parameter type does not match function
    // signature! i32 4 { i8, i32 } call void %pset(ptr %15, i32 4)" — an ICE
    // on a legal program, with no diagnostic pointing at any source construct.
    //
    // The two helpers below are the SAME pair every other store into a
    // nullable slot uses — #2903's one definition of an initializer's slot
    // width (§5.1 in BOTH directions, with the nullable slot's payload as the
    // width to match) followed by §7.2's present-wrap, which also normalizes a
    // `null` literal to the all-zero form. Applied here rather than in
    // `lowerStmt`'s `:=` arm on purpose: EVERY caller of this function needs
    // them, not just that one. `i.IP ??= 4` (#2905) has been pre-wrapping at
    // its own call site precisely because this one did not — which is how the
    // gap became visible — and the §22.3 `ref`/`out` write-back and the
    // compound-assign paths reach the same setter through here too. Both
    // helpers are no-ops when `v` already IS the slot type, so the callers
    // that wrap first are unaffected.
    v = coerceInitializerToSlot(v, valTy, isUnsignedTargetExpr(target));
    v = coerceToNullableSlot(valTy, v);
    // fix #2500 (§8.6/§23.3/§28.9, v0.236): virtual/interface-dispatch twin of
    // lowerImportedPropertySetValue's boxing — an Object/interface property's
    // setter slot expects a `ptr` (box) value parameter, but a value-typed RHS
    // arrived bare; box it first. boxForTarget no-ops for a reference source
    // and for a null valueExpr. Without this the itable/vtable setter call
    // passed i32 where ptr is required and IR verification rejected the module.
    v = boxForTarget(v, ctx_.exprType(target), valueExpr);

    llvm::Value* fnPtr = nullptr;
    // fix #2506: retained across the dispatch so the post-call release can
    // read the trailing owner-kind flag (class-backed vs box/record-backed) —
    // see the release block below.
    llvm::Value* itabForFlag = nullptr;
    if (isIfaceProp) {
        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recv);
        auto* lookupFn = declareC("bz_itable_lookup", ptrTy(),
                                  {ptrTy(), ptrTy()});
        llvm::Value* itmapPtr = builder_.CreateLoad(
            ptrTy(),
            builder_.CreateGEP(ptrTy(), vptr, {builder_.getInt32(1)}));
        auto descIt = descArray_.find(ps->parent);
        if (descIt == descArray_.end()) return false;
        llvm::Value* itab = builder_.CreateCall(
            llvm::FunctionType::get(ptrTy(), {ptrTy(), ptrTy()}, false),
            lookupFn, {itmapPtr, descIt->second}, "itab");
        itabForFlag = itab;
        unsigned k = ifSetIt->second;
        fnPtr = builder_.CreateLoad(
            ptrTy(),
            builder_.CreateGEP(ptrTy(), itab, {builder_.getInt32(k)}), "pset");
    } else {
        llvm::Value* vptr = builder_.CreateLoad(ptrTy(), recv);
        llvm::Value* slotPtr = builder_.CreateGEP(
            ptrTy(), vptr, {builder_.getInt32(setIt->second)});
        fnPtr = builder_.CreateLoad(ptrTy(), slotPtr);
    }
    builder_.CreateCall(fnTy, fnPtr, {recv, v});
    emitPendingCheck();
    // fix #2490/#2500/#2506 (§23.3/§4.6/§27.5/§28.9, v0.236): the virtual/
    // interface-dispatch twin of lowerImportedPropertySetValue's ARGUMENT-PASS
    // release — see that function's comment for the full rationale. The callee
    // (ensurePropertyAccessorFn) retains its incoming `value` ONLY for a
    // CLASS-owned property (§9/§9.7 — a RECORD's auto-property backing field
    // is a borrow with no independent +1 and no disposer, so its setter keeps
    // the plain store); a +1 producer passed directly as the value then holds
    // an independent, doubled reference nothing else releases, and must be
    // released HERE to balance the class setter's retain. A bare borrow (a
    // named local/field) is unaffected either way.
    //
    // The release must fire ONLY when the concrete setter actually retained:
    //   * `!isIfaceProp` — a class's own vtable-slot virtual/override
    //     property. `ps->parent` IS reliably the declaring CLASS (records
    //     cannot enter a vtable hierarchy at all: "A record cannot derive
    //     from another record or from a class", §9), so the callee is
    //     guaranteed to be the CLASS-gated retaining setter → unconditional.
    //   * `isIfaceProp` — itable dispatch. `ps->parent` is always the
    //     Interface, which carries no information about the CONCRETE runtime
    //     receiver, which per §11 may be a class instance OR a boxed record.
    //     fix #2506 added a trailing owner-kind flag word to every class and
    //     record itable (non-null = class-backed retaining setter, null =
    //     box/record plain-store setter) baked in by the concrete type's OWN
    //     module, so the flag travels inside the itable bytes and is sound to
    //     read cross-module regardless of vtable/descriptor linkage. Release
    //     ONLY on the class-backed path; the box/record path skips (releasing
    //     a producer the record's plain-store setter never retained drops the
    //     field's only reference → UAF, the live failure #2490's adversarial
    //     pass found for the direct-set record case). If the interface's flag
    //     index is somehow unavailable, stay conservative and skip (leak-
    //     over-UAF, the safe direction).
    auto emitProducerRelease = [&] {
        // fix #2586 (§23.3/§28.9/§4.6, v0.236): the COMPOUND-ASSIGN twin of the
        // valueExpr-gated release below. A compound `obj.P OP= rhs` passes the
        // emitCompoundOp RESULT as `v` (res) — a fresh +1 from emitStringConcat
        // for a string target, ALWAYS +1 regardless of whether the rhs AST is a
        // producer or a borrow. The valueExpr-based predicates below cannot
        // recognize this (res has no source AST node — it is a computed value),
        // so the compound caller sets valueIsPlusOne=true to force the string
        // release here. The surrounding class-backed vs record-backed flag
        // branching (this lambda is called only on the retaining path) gates
        // the actual emission, so a boxed-RECORD's plain-store setter (which
        // never retained `v`) is NOT double-released — the recordBB branch in
        // the iface path skips the call, and the !isIfaceProp path is always a
        // class vtable (records cannot be virtual per §9). Mirrors #2576's
        // res-release structure in lowerPropertyCompound exactly.
        if (valueIsPlusOne && v && v->getType()->isPointerTy()) {
            emitStringRelease(v);
            return;
        }
        if (valueExpr && isPlusOneStringProducer(valueExpr))
            emitStringRelease(v);
        else if (valueExpr && (isPlusOneClosureProducer(valueExpr) ||
                               isUserMethodClosureReturnCall(valueExpr)))
            emitClosureRelease(v);
        else if (valueExpr && v->getType()->isPointerTy()) {
            // fix #2500 (§23.3/§28.9, v0.236): the box twin of the string/
            // closure release above — same isProducer predicate as
            // lowerBoxFieldStore / lowerImportedPropertySetValue. See those
            // functions' comments for the full rationale. Gated on the TARGET
            // being Object/interface (and v a ptr): a value-typed property's
            // source may be boxable in isolation but is not boxed here.
            const sema::TypeRef* tgtTy = ctx_.exprType(target);
            if (tgtTy && isObjectOrInterfaceTarget(tgtTy)) {
                const sema::TypeRef* srcTy = ctx_.exprType(*valueExpr);
                if ((srcTy && (isBoxableValueType(srcTy) ||
                               isNullableValueTy(srcTy))) ||
                    isUserMethodBoxReturnCall(valueExpr))
                    emitBoxRelease(v);
            }
        }
    };
    // fix #2535 (§23.3/§28.9/§11, v0.236): the NAMED-LOCAL twin of
    // emitProducerRelease, pairing with the StringEscapeWalker's #2535
    // AssignmentStmt relaxation (LlvmBackend_Setup.cpp ~14604 — the
    // class-or-interface property-SET safe-read gate, closed in this
    // commit). The walker now treats `iref.Name := s;` as a CALL-BOUNDED
    // BORROW when `s` is the tracked local, arming its scope-end release.
    // That release needs a paired +1 on the heap it can drop without
    // danging any other reference — the setter provides it on the CLASS-
    // backed path (setter retains → field has +1, local keeps its +1,
    // scope-end release drops the local's, sound); on the RECORD-backed
    // path the setter keeps the PLAIN STORE (§9/§9.7 — a boxed record's
    // reference fields are borrows with no disposer), so the field ALIASES
    // the local's only reference and the scope-end release would free the
    // field's only ref → UAF. Mint that extra +1 here, on the record path
    // ONLY, so the scope-end release has a balanced +1 to drop. Restricted
    // to the NAMED LOCAL case (an IdentifierExpr whose LocalSymbol's decl
    // is in stringReleaseEligible_ / stringBorrowRetainEligible_ — the
    // SAME sets that the walker uses to decide whether to arm the scope-
    // end release) so a producer-temporary RHS (whose only release is
    // emitProducerRelease above on the class path) doesn't get a phantom
    // retain — that would leave a stranded +1 on the heap (heap rc+1,
    // field rc-1 at disposal, heap rc=1, leak; emitStringRetain/release
    // are null-safe but the +1 still has no owner). Mirrors #2506's
    // producer-release gating: a runtime owner-kind flag read from the
    // itable's trailing word (ifaceItabSlotCount_[iface]) selects class-
    // backed (release the producer, no extra retain) vs record-backed
    // (skip the producer release, mint the named-local retain). On the
    // class-vtable path (`!isIfaceProp`) no such retain is needed: the
    // setter's own retain is unconditional for the class-gated branch
    // (lowerImportedPropertySetValue's `classOwned` gate, mirrored by
    // ensurePropertyAccessorFn's class-gated retain — a vtable slot is
    // always a class's own slot, never a record's, so the class-retain
    // invariant holds soundly and the scope-end release is balanced
    // without help).
    auto isTrackedStringLocalForRelease = [&](const ast::Expr* e) -> bool {
        if (!e || currentFn_ != stringReleaseEligibleFn_) return false;
        auto* id = dynamic_cast<const ast::IdentifierExpr*>(e);
        if (!id) return false;
        auto* loc = dynamic_cast<sema::LocalSymbol*>(ctx_.exprSymbol(*id));
        if (!loc || !loc->decl) return false;
        const void* declNode = static_cast<const void*>(loc->decl);
        return stringReleaseEligible_.count(declNode) ||
               stringBorrowRetainEligible_.count(declNode);
    };
    bool ifaceNamedLocalEligible =
        isIfaceProp && valueExpr && v && v->getType()->isPointerTy() &&
        isTrackedStringLocalForRelease(valueExpr);
    if (!isIfaceProp) {
        emitProducerRelease();
    } else if (itabForFlag) {
        auto cntIt = ifaceItabSlotCount_.find(ps->parent);
        if (cntIt != ifaceItabSlotCount_.end()) {
            llvm::Value* flagAddr = builder_.CreateGEP(
                ptrTy(), itabForFlag,
                {builder_.getInt32(cntIt->second)}, "pset.flagaddr");
            llvm::Value* flag = builder_.CreateLoad(
                ptrTy(), flagAddr, "pset.flagval");
            llvm::Value* isClassBacked = builder_.CreateICmpNE(
                flag, llvm::ConstantPointerNull::get(ptrTy()),
                "pset.isclass");
            llvm::BasicBlock* curBB = builder_.GetInsertBlock();
            llvm::Function* fn = curBB->getParent();
            llvm::BasicBlock* classBB =
                llvm::BasicBlock::Create(llvm_, "pset.class", fn);
            llvm::BasicBlock* recordBB =
                llvm::BasicBlock::Create(llvm_, "pset.record", fn);
            llvm::BasicBlock* doneBB =
                llvm::BasicBlock::Create(llvm_, "pset.done", fn);
            builder_.CreateCondBr(isClassBacked, classBB, recordBB);
            // CLASS-backed branch: setter retained → producer release (if RHS
            // is a producer) balances it; no extra retain needed (the class
            // setter's retain is the field's +1; scope-end release on the
            // local drops the local's +1; both +1's are accounted for).
            builder_.SetInsertPoint(classBB);
            emitProducerRelease();
            builder_.CreateBr(doneBB);
            // RECORD-backed branch: setter plain-stored → skip producer
            // release (would UAF the field's only ref). For a tracked-string
            // NAMED LOCAL RHS, mint the extra +1 here so the walker's
            // scope-end release on the local has a balanced +1 to drop.
            builder_.SetInsertPoint(recordBB);
            if (ifaceNamedLocalEligible)
                emitStringRetain(v);
            builder_.CreateBr(doneBB);
            builder_.SetInsertPoint(doneBB);
        }
    }
    return true;
}


// §8.4 (#848): null-safe `=`/`<>` for a record that overloads `operator =`/`<>`,
// when at least one operand is nullable (`T?`) or the `null` literal. Per §8.4,
// `null = null` is `true`, `null = nonNull` is `false`, and both-present invokes
// the equality (here the user operator). The operator (declared on bare `T`) is
// called ONLY on the both-present path — branch-guarded so it never runs on a
// null/zero payload (a record with a reference-typed field would otherwise have
// its comparison dereference a null field). The operator `opm` matches `bin.op`
// (`operator =` for `=`, `operator <>` for `<>`); the not-both-present answer is
// `both-null` for `=` and `exactly-one-null` for `<>`.
llvm::Value* Emitter::lowerNullSafeRecordEquality(const ast::BinaryExpr& bin,
                                                  sema::MethodSymbol* opm) {
    struct Side { llvm::Value* has; llvm::Value* payload; };
    auto eval = [&](const ast::Expr* e) -> Side {
        if (dynamic_cast<const ast::NullLiteralExpr*>(e))
            return {builder_.getFalse(), nullptr};  // payload filled once recTy known
        llvm::Value* v = lowerExpr(*e);
        if (!v) return {nullptr, nullptr};
        const sema::TypeRef* t = ctx_.exprType(*e);
        if (t && isNullableValueTy(t) && v->getType()->isStructTy())
            return {nullablePrimHasValue(v), nullablePrimValue(v)};
        return {builder_.getTrue(), v};  // a bare non-nullable record value
    };
    Side L = eval(bin.left.get());
    Side R = eval(bin.right.get());
    if (!L.has || !R.has) return nullptr;
    llvm::Type* recTy = L.payload ? L.payload->getType()
                      : R.payload ? R.payload->getType() : nullptr;
    if (!recTy) return nullptr;  // both null literals — sema would not resolve opm
    if (!L.payload) L.payload = llvm::Constant::getNullValue(recTy);
    if (!R.payload) R.payload = llvm::Constant::getNullValue(recTy);

    llvm::BasicBlock* entryBB = builder_.GetInsertBlock();
    llvm::Function* fn = entryBB->getParent();
    llvm::Value* bothPresent = builder_.CreateAnd(L.has, R.has, "eq.bothpr");
    auto* callBB  = llvm::BasicBlock::Create(llvm_, "eq.op", fn);
    auto* mergeBB = llvm::BasicBlock::Create(llvm_, "eq.merge", fn);
    builder_.CreateCondBr(bothPresent, callBB, mergeBB);

    builder_.SetInsertPoint(callBB);
    std::vector<llvm::Value*> args{L.payload, R.payload};
    llvm::Value* opres = emitMethodCall(opm, args, /*baseQualified=*/false);
    if (!opres) return nullptr;
    llvm::BasicBlock* callEnd = builder_.GetInsertBlock();  // emitMethodCall may split
    builder_.CreateBr(mergeBB);

    builder_.SetInsertPoint(mergeBB);
    auto* present = builder_.CreatePHI(builder_.getInt1Ty(), 2, "eq.present");
    present->addIncoming(builder_.getFalse(), entryBB);
    present->addIncoming(opres, callEnd);
    // Not-both-present answer: `=` → both-null; `<>` → exactly-one-null.
    llvm::Value* tail =
        (bin.op == "=")
            ? builder_.CreateAnd(builder_.CreateNot(L.has),
                                 builder_.CreateNot(R.has), "eq.bothnull")
            : builder_.CreateXor(L.has, R.has, "eq.onenull");
    return builder_.CreateOr(present, tail, "eq.nullsafe");
}


llvm::Value* Emitter::lowerBinary(const ast::BinaryExpr& bin) {
    if (!bin.left || !bin.right) return nullptr;
    // CG: short-circuit `and`/`or` (§17) — the right operand is
    // evaluated only when needed, so a side-effecting or throwing RHS
    // is not run when the result is already determined.
    if (bin.op == "and" || bin.op == "or")
        return lowerShortCircuit(bin);
    // CG39: null-coalescing `??` (§17.1 L12) short-circuits — the right
    // operand is evaluated only when the left is null.
    if (bin.op == "??")
        return lowerCoalesce(bin);
    // §25: a user-defined operator overload — sema resolved the binary
    // expression to a static `operator` method; call it with the two
    // operands.
    sema::MethodSymbol* opm =
        dynamic_cast<sema::MethodSymbol*>(ctx_.exprSymbol(bin));
    if (opm && !opm->isOperator) opm = nullptr;
    // §11.8 (v0.176, #807): constraint-operator dispatch — sema resolved this
    // binary expression to a STATIC INTERFACE operator member through a
    // `where T: I<T>` constraint (the operands are T-typed). At this
    // specialization T is concrete: a USER type redirects to its own operator
    // (a direct static call through the block below); a PRIMITIVE falls
    // through to the intrinsic op lowering further down — the §2.19
    // compiler-provided conformance's operator IS the intrinsic op (§3.5
    // lowering reused). An unbound T (the dead unspecialized template body,
    // #817 posture) also falls through: its operands lower as the placeholder
    // scalar, which the intrinsic path handles type-correctly.
    if (opm && opm->parent &&
        opm->parent->kind == ast::TypeDeclKind::Interface) {
        const sema::TypeRef* lt =
            bin.left ? ctx_.exprType(*bin.left) : nullptr;
        const sema::TypeRef* concrete = resolveTypeParamRef(lt);
        sema::MethodSymbol* impl = nullptr;
        // §25.2 (v0.181, #1498): resolve by the ACTUAL operator token used at
        // this call site (`bin.op`), NOT the witness member's token. Under the
        // complete-group entitlement sema may bind `a > b` to the constraint's
        // `<` witness (INumber<T> declares `<` alone but entitles all four of
        // `<`,`>`,`<=`,`>=`); the conforming type declares the full group, so
        // dispatch to ITS `>` — matching on `opm->operatorToken` (`<`) would
        // mis-route to the wrong member.
        if (concrete && concrete != lt &&
            concrete->kind == sema::TypeRef::Kind::Named &&
            concrete->declaration) {
            for (sema::Symbol* m : concrete->declaration->members) {
                auto* cand = dynamic_cast<sema::MethodSymbol*>(m);
                if (cand && cand->isOperator &&
                    cand->operatorToken == bin.op &&
                    cand->params.size() == 2) {
                    impl = cand;
                    break;
                }
            }
        }
        opm = impl;  // null → primitive/placeholder: intrinsic path below
    }
    if (opm) {
        {
            // §8.4 (#848): a user `operator =`/`<>` on a record is invoked
            // through the null-safe equality wrapper when either operand is a
            // nullable `T?` or the `null` literal — both-null → equal, one-null
            // → not-equal, both-present → the operator on the UNWRAPPED payloads.
            // Without this the `{i8, T}` wrapper (or `ptr null`) was passed
            // straight to the operator (which takes bare `T`) → malformed IR.
            if (bin.op == "=" || bin.op == "<>") {
                auto nullishOperand = [&](const ast::Expr* e) {
                    if (!e) return false;
                    if (dynamic_cast<const ast::NullLiteralExpr*>(e)) return true;
                    const sema::TypeRef* t = ctx_.exprType(*e);
                    return t && isNullableValueTy(t);
                };
                if (nullishOperand(bin.left.get()) ||
                    nullishOperand(bin.right.get()))
                    return lowerNullSafeRecordEquality(bin, opm);
            }
            llvm::Value* ol = lowerExpr(*bin.left);
            llvm::Value* orr = lowerExpr(*bin.right);
            if (!ol || !orr) return nullptr;
            std::vector<llvm::Value*> args{ol, orr};
            // #1059: an operator overload on a GENERIC record/class is a
            // (non-generic) static method whose parameter / return types use the
            // declaring type's parameter `T` (e.g. `operator +(a: Vec<T>, b:
            // Vec<T>): Vec<T>`). Set up the per-instantiation substitution from
            // the operand that is an instance of the operator's declaring type
            // (e.g. `Vec<int>` → T=int) so emitMethodCall SPECIALIZES the
            // operator — its params lower to the specialized record struct,
            // matching the already-specialized lowered operand types. Without
            // this the operator was emitted once with the unspecialized `T`
            // placeholder layout and the call's specialized args mismatched
            // (IR_FAIL "Call parameter type does not match function signature").
            auto savedOpSubst = genericSubst_;
            auto savedOpSema = genericSubstSemaTy_;
            for (const ast::Expr* operand : {bin.left.get(), bin.right.get()}) {
                const sema::TypeRef* opTy = ctx_.exprType(*operand);
                if (!opTy || opTy->kind != sema::TypeRef::Kind::Named ||
                    opTy->declaration != opm->parent ||
                    opTy->genericArgs.empty())
                    continue;
                // fix #3143 (§14.1 / §2.5, v0.254): parameter names through the
                // one both-sources walk — an IMPORTED generic record's binary
                // operator was emitted unspecialized and called with
                // specialized operands (the IR_FAIL this block exists to stop).
                const std::vector<std::string_view> gpNames =
                    sema::genericParamNames(opTy->declaration);
                if (gpNames.empty() ||
                    gpNames.size() != opTy->genericArgs.size())
                    continue;
                std::unordered_map<std::string_view, llvm::Type*> ns;
                std::unordered_map<std::string_view, const sema::TypeRef*> nsem;
                for (std::size_t gi = 0; gi < gpNames.size(); ++gi) {
                    ns[gpNames[gi]] = lowerTypeRef(opTy->genericArgs[gi]);
                    nsem[gpNames[gi]] =
                        resolveTypeParamRef(opTy->genericArgs[gi]);
                }
                genericSubst_ = std::move(ns);
                genericSubstSemaTy_ = std::move(nsem);
                break;
            }
            llvm::Value* res =
                emitMethodCall(opm, args, /*baseQualified=*/false);
            genericSubst_ = std::move(savedOpSubst);
            genericSubstSemaTy_ = std::move(savedOpSema);
            // §28.3 / §25.1 (#912, #965): an operand of a class-operator call
            // that is itself an OWNED TEMPORARY — a chained owned-returning
            // operator result (`(a + b)` in `a + b + c`), a bare `new C(...)`,
            // or an `owned`-returning method/function call (`Make(1)`) — is
            // passed here as a borrow argument and, once the call returns, has
            // no owner. Dispose it now. A named-local / member-access borrow
            // operand is none of these shapes, so it is left untouched (no
            // double-free). The result `res` is returned to the enclosing owner
            // (the next operator call, or a `using var` that disposes it at
            // scope end).
            auto classOrIfaceRef = [](const sema::TypeRef* rt) -> bool {
                return rt && rt->kind == sema::TypeRef::Kind::Named &&
                       rt->declaration &&
                       (rt->declaration->kind == ast::TypeDeclKind::Class ||
                        rt->declaration->kind == ast::TypeDeclKind::Interface);
            };
            auto isOwnedTempOperand = [&](const ast::Expr* e) -> bool {
                if (!e) return false;
                const sema::TypeRef* ty = ctx_.exprType(*e);
                if (!classOrIfaceRef(ty) &&
                    !classOrIfaceRef(resolveTypeParamRef(ty)))
                    return false;  // value-typed result owns nothing to dispose
                // (1) a bare `new ClassType(...)` operand.
                if (dynamic_cast<const ast::NewExpr*>(e)) return true;
                // (2) a chained owned-returning operator result.
                //
                // fix #3189 (family M2 of umbrella #3152): this arm is drift #5
                // of the audit — it sat THIRTEEN LINES above its own already-
                // fixed sibling (3) and read the AST decl only, so an imported
                // operator's intermediate temporary was never disposed. Both
                // arms now go through the one both-sources accessor.
                if (auto* be = dynamic_cast<const ast::BinaryExpr*>(e)) {
                    auto* ms = dynamic_cast<sema::MethodSymbol*>(
                        ctx_.exprSymbol(*be));
                    return ms && ms->isOperator &&
                           sema::methodReturnsOwned(*ms, ctx_);
                }
                // (3) an `owned`-returning method/function call result. The
                // owned-return flag lives on the MethodDecl, or (imported, no
                // AST decl) in SemaContext::importedReturnOwned_. A borrow
                // return must NOT be disposed, so require the owned flag.
                if (auto* call = dynamic_cast<const ast::CallExpr*>(e))
                    return common::calleeReturnsOwned(*call, ctx_);
                return false;
            };
            // Soundness guard: if THIS operator's parameter is `owned` (it
            // consumes the operand and takes over disposal), the intermediate
            // must NOT be disposed here — that would double-free. A normal
            // operator takes borrows, so paramConsumes is false and the
            // temporary is disposed exactly once.
            //
            // fix #3189 (family M2 of umbrella #3152): the guard reads the
            // formals through `sema::formalParamModifier` (#3176's both-sources
            // accessor) rather than through `opm->decl`, which is null for an
            // IMPORTED operator and made `paramConsumes` answer `false` for
            // every formal of one.
            //
            // ⚠ MEASURED INERT, and the honest statement is worth keeping: the
            // change fixes NO live double free, because §25.2 (v0.254) rejects
            // `ref`/`out`/`owned`/`scoped` on an operator PARAMETER outright —
            // *"operands are received as plain borrows … the return slot's
            // optional `owned` is the sole ownership annotation on operators"*.
            // The probe that would have exercised it (`operator +(owned a,
            // b)`) does not compile, in either module. So `paramConsumes` is
            // false for every well-formed program whichever source it reads.
            // It moves anyway because it is the guard that stands between arm
            // (2)'s new imported-operator admission and a double free: leaving
            // it keyed on a null decl would make the ONE thing protecting this
            // widening depend on a fact the imported side cannot see, and the
            // §25.2 bound is the only reason that is currently harmless.
            auto paramConsumes = [&](std::size_t i) -> bool {
                std::string_view m = sema::formalParamModifier(*opm, i);
                return m == "owned" || m == "out owned";
            };
            if (res) {
                if (!paramConsumes(0) && isOwnedTempOperand(bin.left.get()))
                    emitDisposeAndReclaim(ol);
                if (!paramConsumes(1) && isOwnedTempOperand(bin.right.get()))
                    emitDisposeAndReclaim(orr);
            }
            return res;
        }
    }
    // §17.5: `x in lo..hi` — true if lo <= x <= hi (inclusive) or
    // lo <= x < hi (half-open). The right operand is a RangeExpr, not a
    // value, so we intercept before the general lowerExpr path.
    if (bin.op == "in") {
        if (!bin.left || !bin.right) return nullptr;
        auto* range = dynamic_cast<const ast::RangeExpr*>(bin.right.get());
        if (range && range->lo && range->hi && !range->step) {
            llvm::Value* x  = lowerExpr(*bin.left);
            // §17.5 / §7.11 (v0.286, #3298): the PAIR — sema rejects a
            // NON-narrowed nullable left operand at a range receiver
            // (ExprTypecheck_Exprs.cpp, `op == "in"`), so a nullable-typed
            // operand reaching here is one §7.4 narrowed non-null and its
            // `{i8, T}` box must be unwrapped to the scalar the comparisons
            // below need. Without the unwrap this emitted `icmp {i8,i32}, i32`
            // and died in IR verification.
            x = unwrapNarrowedNullableOperand(x, bin.left.get());
            llvm::Value* st = lowerExpr(*range->lo);
            llvm::Value* en = lowerExpr(*range->hi);
            if (!x || !st || !en) return nullptr;
            // §16.3 (v0.162, fix #1340): a stepless literal uses default step=1
            // (ascending). Contains = nonempty AND within-extent AND endOk.
            // Ascending IsEmpty: closed → Start > End; half-open → Start >= End.
            // A reversed literal like `50 in 100..0` returns false (empty range).
            llvm::Value* asc = builder_.CreateICmpSLE(st, en);
            llvm::Value* lo  = builder_.CreateSelect(asc, st, en);
            llvm::Value* hi  = builder_.CreateSelect(asc, en, st);
            llvm::Value* inR = builder_.CreateAnd(
                builder_.CreateICmpSGE(x, lo), builder_.CreateICmpSLE(x, hi));
            if (range->inclusive) {
                // nonempty (ascending closed): Start <= End.
                llvm::Value* notEmpty = builder_.CreateICmpSLE(st, en, "ri.ne");
                return builder_.CreateAnd(notEmpty, inR, "ri.res");
            }
            // half-open: nonempty (ascending): Start < End; also exclude End.
            llvm::Value* notEmpty = builder_.CreateICmpSLT(st, en, "ri.ne");
            return builder_.CreateAnd(
                notEmpty,
                builder_.CreateAnd(inR, builder_.CreateICmpNE(x, en)), "ri.res");
        }
        // fix #499 / §9.8: `x in r` where r is a Range-typed variable, OR
        // (fix #1496) a RangeExpr literal the closed-literal fast path above
        // didn't handle (open-ended, or carrying a step). Load/construct the
        // { i64 start, i64 end, i64 step, i8 flags } struct and compute
        // Contains(x) per §16.3 v0.162: nonempty AND within-extent AND endOk.
        // Step determines emptiness/direction; open-ended sides are never
        // empty (flags bits 1/2).
        if (bin.right && !(range && range->lo && range->hi && !range->step)) {
            auto* rTy = ctx_.exprType(*bin.right);
            if (isRangeType(rTy)) {
                llvm::Value* x  = lowerExpr(*bin.left);
                // §17.5 / §7.11 (v0.286, #3298): same pair as the literal
                // fast path above — sema's range gate rejects the
                // non-narrowed nullable operand, so unwrapping the narrowed
                // one here is sound (see unwrapNarrowedNullableOperand's
                // soundness precondition).
                x = unwrapNarrowedNullableOperand(x, bin.left.get());
                llvm::Value* rv = lowerExpr(*bin.right);
                if (!x || !rv) return nullptr;
                llvm::Value* st   = builder_.CreateExtractValue(rv, {0u}, "r.st");
                llvm::Value* en   = builder_.CreateExtractValue(rv, {1u}, "r.en");
                llvm::Value* step = builder_.CreateExtractValue(rv, {2u}, "r.step");
                llvm::Value* flg  = builder_.CreateExtractValue(rv, {3u}, "r.flg");
                // Widen x to i64 for comparison against i64 start/end.
                llvm::Value* xW = x->getType() == i64()
                                      ? x
                                      : builder_.CreateSExt(x, i64(), "r.xw");
                // §16.3 (v0.162, fix #1496): a null endpoint (flags bit 1 =
                // startOpen, bit 2 = endOpen) imposes NO bound on its side.
                // `st`/`en` hold a fake 0 sentinel on an open side, so they
                // must be excluded from the direction/extent computation
                // rather than compared as real bounds.
                auto* i8Ty       = llvm::Type::getInt8Ty(llvm_);
                llvm::Value* startOpen = builder_.CreateICmpNE(
                    builder_.CreateAnd(flg, llvm::ConstantInt::get(i8Ty, 2)),
                    llvm::ConstantInt::get(i8Ty, 0), "r.sopen");
                llvm::Value* endOpen = builder_.CreateICmpNE(
                    builder_.CreateAnd(flg, llvm::ConstantInt::get(i8Ty, 4)),
                    llvm::ConstantInt::get(i8Ty, 0), "r.eopen");
                llvm::Value* closedR = builder_.CreateAnd(
                    builder_.CreateNot(startOpen),
                    builder_.CreateNot(endOpen), "r.closed");
                // Extent: [min(Start,End), max(Start,End)]; End excluded when half-open.
                llvm::Value* asc = builder_.CreateICmpSLE(st, en, "r.asc");
                llvm::Value* loClosed = builder_.CreateSelect(asc, st, en, "r.loc");
                llvm::Value* hiClosed = builder_.CreateSelect(asc, en, st, "r.hic");
                llvm::Value* negInf =
                    builder_.getInt64(std::numeric_limits<int64_t>::min());
                llvm::Value* posInf =
                    builder_.getInt64(std::numeric_limits<int64_t>::max());
                llvm::Value* lo = builder_.CreateSelect(
                    startOpen, negInf,
                    builder_.CreateSelect(closedR, loClosed, st), "r.lo");
                llvm::Value* hi = builder_.CreateSelect(
                    endOpen, posInf,
                    builder_.CreateSelect(closedR, hiClosed, en), "r.hi");
                llvm::Value* inR = builder_.CreateAnd(
                    builder_.CreateICmpSGE(xW, lo, "r.ge"),
                    builder_.CreateICmpSLE(xW, hi, "r.le"), "r.inR");
                llvm::Value* incl   = builder_.CreateTrunc(flg, i1(), "r.incl");
                llvm::Value* notEnd = builder_.CreateICmpNE(xW, en, "r.notEnd");
                // half-open (!incl) excludes the End point — only when End is
                // actually present; an open End has no endpoint to exclude.
                llvm::Value* endOk = builder_.CreateOr(
                    endOpen,
                    builder_.CreateOr(incl, notEnd), "r.endOk");
                // IsEmpty check (§16.3 v0.162): open-ended ranges never empty.
                //   ascending (step >= 0): closed → st > en; half-open → st >= en
                //   descending (step < 0):  closed → st < en; half-open → st <= en
                llvm::Value* isOE =
                    builder_.CreateOr(startOpen, endOpen, "r.oe");
                llvm::Value* stepNeg =
                    builder_.CreateICmpSLT(step,
                        llvm::ConstantInt::get(i64(), 0), "r.sneg");
                llvm::Value* ascIncl  = builder_.CreateICmpSGT(st, en, "r.ai");
                llvm::Value* ascHalf  = builder_.CreateICmpSGE(st, en, "r.ah");
                llvm::Value* descIncl = builder_.CreateICmpSLT(st, en, "r.di");
                llvm::Value* descHalf = builder_.CreateICmpSLE(st, en, "r.dh");
                llvm::Value* emptyAsc =
                    builder_.CreateSelect(incl, ascIncl, ascHalf, "r.ea");
                llvm::Value* emptyDesc =
                    builder_.CreateSelect(incl, descIncl, descHalf, "r.ed");
                llvm::Value* dirEmpty =
                    builder_.CreateSelect(stepNeg, emptyDesc, emptyAsc, "r.de");
                // notEmpty = isOpenEnded || !dirEmpty
                llvm::Value* notEmpty =
                    builder_.CreateOr(isOE,
                        builder_.CreateNot(dirEmpty), "r.ne");
                return builder_.CreateAnd(
                    notEmpty,
                    builder_.CreateAnd(inR, endOk, "r.chk"), "r.in");
            }
        }
        // §17.5/§4.6: `c in s` — char membership test in a string.
        // True when the char c appears at any position in s.
        // fix #558 / §17.5/§4.6: `sub in s` — substring containment when LHS is
        // also a string. Lowers to `bz_str_contains(s, sub)` — the same runtime
        // function that backs `s.Contains(sub: string): bool` (§9.6). Both forms
        // require a `string` RHS; they differ only in LHS type (Char vs String).
        if (bin.right) {
            auto* rTy = ctx_.exprType(*bin.right);
            if (rTy && rTy->kind == sema::TypeRef::Kind::Primitive &&
                rTy->primitive == sema::PrimitiveType::String) {
                auto* lTy = ctx_.exprType(*bin.left);
                // §17.5/§4.6 (fix #558): `string in string` — substring test.
                // Reuses bz_str_contains(haystack, needle): i32 which is the same
                // function backing string.Contains(sub: string) (§9.6, #535).
                if (lTy && lTy->kind == sema::TypeRef::Kind::Primitive &&
                    lTy->primitive == sema::PrimitiveType::String) {
                    llvm::Value* needle   = lowerExpr(*bin.left);
                    llvm::Value* haystack = lowerExpr(*bin.right);
                    if (!needle || !haystack) return nullptr;
                    auto* fn = declareC("bz_str_contains", i32(), {ptrTy(), ptrTy()});
                    llvm::Value* res = builder_.CreateCall(fn, {haystack, needle},
                                                           "sin.r");
                    return builder_.CreateTrunc(res, i1(), "sin");
                }
                if (lTy && lTy->kind == sema::TypeRef::Kind::Primitive &&
                    lTy->primitive == sema::PrimitiveType::Char) {
                    // Implement as: scan through string code points for a match.
                    llvm::Value* needle = lowerExpr(*bin.left);
                    llvm::Value* haystack = lowerExpr(*bin.right);
                    if (!needle || !haystack) return nullptr;
                    llvm::Value* len = loadStringLen(haystack);
                    auto* idxS = entryAlloca(i64());
                    builder_.CreateStore(builder_.getInt64(0), idxS);
                    auto* cBB = llvm::BasicBlock::Create(llvm_, "cin.c", currentFn_);
                    auto* bBB = llvm::BasicBlock::Create(llvm_, "cin.b", currentFn_);
                    auto* sBB = llvm::BasicBlock::Create(llvm_, "cin.s", currentFn_);  // success
                    auto* eBB = llvm::BasicBlock::Create(llvm_, "cin.e", currentFn_);  // end
                    builder_.CreateBr(cBB);
                    builder_.SetInsertPoint(cBB);
                    llvm::Value* idx = builder_.CreateLoad(i64(), idxS);
                    builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len), bBB, eBB);
                    builder_.SetInsertPoint(bBB);
                    llvm::Value* idxB = builder_.CreateLoad(i64(), idxS);
                    llvm::Value* cp = loadCodePoint(haystack, idxB);
                    llvm::Value* found = builder_.CreateICmpEQ(cp, needle, "cin.found");
                    auto* incBB = llvm::BasicBlock::Create(llvm_, "cin.inc", currentFn_);
                    builder_.CreateCondBr(found, sBB, incBB);
                    builder_.SetInsertPoint(incBB);
                    builder_.CreateStore(builder_.CreateAdd(idxB, builder_.getInt64(1)), idxS);
                    builder_.CreateBr(cBB);
                    builder_.SetInsertPoint(sBB);
                    auto* sBBEnd = builder_.GetInsertBlock();
                    builder_.CreateBr(eBB);
                    builder_.SetInsertPoint(eBB);
                    auto* phi = builder_.CreatePHI(i1(), 2, "cin.result");
                    phi->addIncoming(builder_.getFalse(), cBB);  // loop exhausted
                    phi->addIncoming(builder_.getTrue(), sBBEnd); // found match
                    return phi;
                }
            }
        }
        // fix #524 / §17.5: `elem in set` ≡ set.Contains(elem) for a
        // dict-backed Set<T>.  The right operand must be a Set<T> whose
        // element type has a dict backing (genericBackingType returns non-null);
        // the left operand is the element to look up.
        if (bin.right) {
            auto* rTy = ctx_.exprType(*bin.right);
            bool isSetRHS = rTy && rTy->kind == sema::TypeRef::Kind::Named &&
                            !rTy->declaration && rTy->astNode &&
                            rTy->astNode->name &&
                            !rTy->astNode->name->parts.empty() &&
                            (rTy->headName() == "Set" ||
                             rTy->headName() == "OwningSet");
            if (isSetRHS) {
                const sema::TypeRef* elemTy =
                    genericBackingType(rTy, 0, /*isSet=*/true, nullptr);
                if (elemTy) {
                    bool elemIsRecord = recordTypeOfRef(elemTy) != nullptr;
                    llvm::Value* elem = lowerExpr(*bin.left);
                    llvm::Value* set  = lowerExpr(*bin.right);
                    if (!elem || !set) return nullptr;
                    // Spill elem to a stack slot; a record already spilled to ptr.
                    llvm::Value* ep;
                    if (elemIsRecord && elem->getType()->isPointerTy()) {
                        ep = elem;
                    } else {
                        llvm::AllocaInst* slot = entryAlloca(elem->getType());
                        builder_.CreateStore(elem, slot);
                        ep = slot;
                    }
                    auto* fn = declareC("bz_dict_contains_gen", i1(),
                                        {ptrTy(), ptrTy()});
                    return builder_.CreateCall(fn, {set, ep}, "set.in");
                }
            }
        }
        // fix #524 / §4.8: `key in dict` ≡ dict.ContainsKey(key) for a
        // Dictionary<K,V>.  String keys use the dedicated fast path
        // (bz_dict_contains_str); generic keys use bz_dict_contains_gen.
        if (bin.right) {
            auto* rTy = ctx_.exprType(*bin.right);
            if (isDictTypeName(rTy)) {
                const sema::TypeRef* keyTy =
                    genericBackingType(rTy, 0, /*isSet=*/false, nullptr);
                bool keyIsRecord = keyTy && recordTypeOfRef(keyTy) != nullptr;
                llvm::Value* key  = lowerExpr(*bin.left);
                llvm::Value* dict = lowerExpr(*bin.right);
                if (!key || !dict) return nullptr;
                if (keyTy) {
                    // Generic (non-string) key: spill to a slot, pass pointer.
                    llvm::Value* kp;
                    if (keyIsRecord && key->getType()->isPointerTy()) {
                        kp = key;
                    } else {
                        llvm::AllocaInst* slot = entryAlloca(key->getType());
                        builder_.CreateStore(key, slot);
                        kp = slot;
                    }
                    auto* fn = declareC("bz_dict_contains_gen", i1(),
                                        {ptrTy(), ptrTy()});
                    return builder_.CreateCall(fn, {dict, kp}, "dict.in");
                }
                // String key fast path.
                llvm::Value* kdata = loadStringData(key);
                llvm::Value* klen  = loadStringLen(key);
                auto* fn = declareC("bz_dict_contains_str", i1(),
                                    {ptrTy(), ptrTy(), i64()});
                llvm::Value* r =
                    builder_.CreateCall(fn, {dict, kdata, klen}, "dict.in");
                // fix #3703 / §28.9: `key in dict` never stores the key — the
                // table copies kdata/klen's bytes, never retaining `key`
                // itself (unlike a VALUE slot, per #429). Release a fresh +1
                // KEY producer now, after the sole use of its buffer.
                if (isPlusOneStringProducer(bin.left.get()))
                    emitStringRelease(key);
                return r;
            }
        }
        // §17.5: `x in coll` where `coll` is an Array<T>-, Stack<T>-, or
        // Queue<T>-typed expression (all bz_array_t-backed collections except
        // Set/OwningSet, which are dict-backed and handled above).
        // Lowers to the same linear scan as Array<T>.Contains(x): iterate via
        // bz_array_len / bz_array_ptr_at and compare each element using
        // emitRemoveElementEquality (record/string/value-type equality per §8.3).
        if (bin.right) {
            auto* rTy = ctx_.exprType(*bin.right);
            // isCollectionTypeName covers Array, OwningArray, Stack, OwningStack,
            // Queue, OwningQueue, Set, OwningSet — but Set/OwningSet are already
            // handled above by the dict-backed path; they only reach here when
            // genericBackingType returned null (no generic arg), which is a
            // degenerate case not reached in practice.  isLinkedListTypeName
            // covers LinkedList and OwningLinkedList (bz_llist_t-backed).
            bool isArr = isCollectionTypeName(rTy);
            bool isLL  = isLinkedListTypeName(rTy);
            if (isArr || isLL) {
                const sema::TypeRef* elemRef =
                    (rTy && !rTy->genericArgs.empty()) ? rTy->genericArgs[0]
                                                       : nullptr;
                bool elemIsClass = collectionElemIsClass(rTy, 0);
                llvm::Value* elem = lowerExpr(*bin.left);
                llvm::Value* coll = lowerExpr(*bin.right);
                if (!elem || !coll) return nullptr;

                if (isLL) {
                    // fix #560 / §17.5/§4.6/§8.3: walk the linked list applying
                    // emitRemoveElementEquality for per-element comparison so that
                    // string content equality (§4.6), record structural equality
                    // (§8.3/§9.1) and class Equals-dispatch (§8.4) are all correct.
                    // The previous bz_llist_find path used memcmp on raw element
                    // bytes — wrong for string elements (compared bz_string* pointer,
                    // not content) and for records with string fields.
                    llvm::Type* elemLLTy = elemRef ? lowerTypeRef(elemRef) : i64();
                    if (!elemLLTy) elemLLTy = i64();
                    auto* firstFn = declareC("bz_llist_first_node", ptrTy(),
                                            {ptrTy()});
                    auto* nextFn  = declareC("bz_llist_node_next", ptrTy(),
                                            {ptrTy()});
                    auto* valFn   = declareC("bz_llist_node_value_ptr", ptrTy(),
                                            {ptrTy()});
                    auto* nullP = llvm::ConstantPointerNull::get(ptrTy());
                    // Spill `elem` (the search target) to a stable slot so that
                    // emitRemoveElementEquality receives consistent operand types.
                    llvm::AllocaInst* tgtSlot = entryAlloca(elem->getType());
                    builder_.CreateStore(elem, tgtSlot);
                    // nodeSlot holds the current node pointer across iterations.
                    llvm::AllocaInst* nodeSlot = entryAlloca(ptrTy());
                    llvm::Value* firstNode = builder_.CreateCall(
                        firstFn, {coll}, "ll.in.first");
                    builder_.CreateStore(firstNode, nodeSlot);
                    auto* condBB  = llvm::BasicBlock::Create(llvm_, "llin.c",  currentFn_);
                    auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "llin.b",  currentFn_);
                    auto* foundBB = llvm::BasicBlock::Create(llvm_, "llin.f",  currentFn_);
                    auto* nextBB  = llvm::BasicBlock::Create(llvm_, "llin.n",  currentFn_);
                    auto* endBB   = llvm::BasicBlock::Create(llvm_, "llin.e",  currentFn_);
                    builder_.CreateBr(condBB);
                    builder_.SetInsertPoint(condBB);
                    llvm::Value* curNode = builder_.CreateLoad(ptrTy(), nodeSlot,
                                                               "ll.in.cur");
                    builder_.CreateCondBr(
                        builder_.CreateICmpNE(curNode, nullP, "ll.in.nnull"),
                        bodyBB, endBB);
                    builder_.SetInsertPoint(bodyBB);
                    llvm::Value* vp = builder_.CreateCall(valFn, {curNode},
                                                          "ll.in.vp");
                    llvm::Value* el = builder_.CreateLoad(elemLLTy, vp, "ll.in.el");
                    // Load `elem` back from slot to match the type `el` has.
                    llvm::Value* tgt = builder_.CreateLoad(elem->getType(),
                                                           tgtSlot, "ll.in.tgt");
                    llvm::Value* eq = emitRemoveElementEquality(el, tgt,
                                                                elemRef,
                                                                elemIsClass);
                    if (!eq) return nullptr;
                    builder_.CreateCondBr(eq, foundBB, nextBB);
                    builder_.SetInsertPoint(foundBB);
                    builder_.CreateBr(endBB);
                    builder_.SetInsertPoint(nextBB);
                    llvm::Value* nxt = builder_.CreateCall(nextFn, {curNode},
                                                            "ll.in.next");
                    builder_.CreateStore(nxt, nodeSlot);
                    builder_.CreateBr(condBB);
                    builder_.SetInsertPoint(endBB);
                    auto* phi = builder_.CreatePHI(i1(), 2, "llin.result");
                    phi->addIncoming(builder_.getTrue(),  foundBB);
                    phi->addIncoming(builder_.getFalse(), condBB);
                    return phi;
                }

                // Array-backed path: emit the same scan loop as
                // tryLowerCollectionInstanceCall's Contains branch.
                llvm::Type* elemTy = elemRef ? lowerTypeRef(elemRef) : i64();
                if (!elemTy) elemTy = i64();
                int64_t esz = collectionElemSize(rTy);
                auto* lenFn = declareC("bz_array_len", i64(), {ptrTy()});
                llvm::Value* len = builder_.CreateCall(lenFn, {coll}, "coll.len");
                auto* idxSlot = entryAlloca(i64());
                builder_.CreateStore(builder_.getInt64(0), idxSlot);
                auto* condBB  = llvm::BasicBlock::Create(llvm_, "cin2.c", currentFn_);
                auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "cin2.b", currentFn_);
                auto* foundBB = llvm::BasicBlock::Create(llvm_, "cin2.f", currentFn_);
                auto* nextBB  = llvm::BasicBlock::Create(llvm_, "cin2.n", currentFn_);
                auto* endBB   = llvm::BasicBlock::Create(llvm_, "cin2.e", currentFn_);
                builder_.CreateBr(condBB);
                builder_.SetInsertPoint(condBB);
                llvm::Value* idx = builder_.CreateLoad(i64(), idxSlot);
                builder_.CreateCondBr(builder_.CreateICmpSLT(idx, len),
                                      bodyBB, endBB);
                builder_.SetInsertPoint(bodyBB);
                auto* ptrFn = declareC("bz_array_ptr_at", ptrTy(),
                                       {ptrTy(), i64(), i64()});
                llvm::Value* ep = builder_.CreateCall(
                    ptrFn, {coll, idx, builder_.getInt64(esz)});
                llvm::Value* el = builder_.CreateLoad(elemTy, ep);
                llvm::Value* eq =
                    emitRemoveElementEquality(el, elem, elemRef, elemIsClass);
                if (!eq) return nullptr;
                builder_.CreateCondBr(eq, foundBB, nextBB);
                builder_.SetInsertPoint(foundBB);
                builder_.CreateBr(endBB);
                builder_.SetInsertPoint(nextBB);
                builder_.CreateStore(
                    builder_.CreateAdd(idx, builder_.getInt64(1)), idxSlot);
                builder_.CreateBr(condBB);
                builder_.SetInsertPoint(endBB);
                auto* phi = builder_.CreatePHI(i1(), 2, "coll.in");
                phi->addIncoming(builder_.getTrue(),  foundBB);
                phi->addIncoming(builder_.getFalse(), condBB);
                return phi;
            }
        }
        // §17.5 / §15.1.2 / #1342: `x in inlineArr` where `inlineArr` is a
        // `[N]T`-typed expression.  Lowers to the same linear scan as
        // tryLowerInlineArrayCall's Contains branch — element equality uses
        // emitRemoveElementEquality per §9.2 (value, string content, class
        // identity/Equals, record structural).
        if (bin.right) {
            auto* rTy = ctx_.exprType(*bin.right);
            if (rTy && rTy->kind == sema::TypeRef::Kind::InlineArray) {
                sema::TypeRef* elemRef = rTy->arrayElement;
                int64_t N = rTy->arraySizeValue;
                llvm::Type* elemTy = elemRef ? lowerTypeRef(elemRef) : i64();
                if (!elemTy) elemTy = i64();

                // Get receiver address (lvalue or spill to alloca).
                llvm::Type* arrSlotTy = nullptr;
                llvm::Value* arrAddr = lvalueAddr(*bin.right, arrSlotTy);
                if (!arrAddr || !arrSlotTy) {
                    llvm::Value* v = lowerExpr(*bin.right);
                    if (!v) return nullptr;
                    auto* at = llvm::dyn_cast<llvm::ArrayType>(v->getType());
                    if (!at) return nullptr;
                    llvm::AllocaInst* tmp = entryAlloca(at);
                    builder_.CreateStore(v, tmp);
                    arrAddr = tmp;
                    arrSlotTy = at;
                }
                auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(arrSlotTy);
                if (!arrTy) return nullptr;

                llvm::Value* target = lowerExpr(*bin.left);
                if (!target) return nullptr;

                if (N == 0) return builder_.getFalse();

                bool elemIsClass = elemRef &&
                    elemRef->kind == sema::TypeRef::Kind::Named &&
                    elemRef->declaration &&
                    (elemRef->declaration->kind == ast::TypeDeclKind::Class ||
                     elemRef->declaration->kind == ast::TypeDeclKind::Interface);

                auto* idxSlot = entryAlloca(i32());
                builder_.CreateStore(builder_.getInt32(0), idxSlot);
                auto* condBB  = llvm::BasicBlock::Create(llvm_, "iin.cond",  currentFn_);
                auto* bodyBB  = llvm::BasicBlock::Create(llvm_, "iin.body",  currentFn_);
                auto* foundBB = llvm::BasicBlock::Create(llvm_, "iin.found", currentFn_);
                auto* nextBB  = llvm::BasicBlock::Create(llvm_, "iin.next",  currentFn_);
                auto* endBB   = llvm::BasicBlock::Create(llvm_, "iin.end",   currentFn_);
                builder_.CreateBr(condBB);

                builder_.SetInsertPoint(condBB);
                llvm::Value* idx = builder_.CreateLoad(i32(), idxSlot);
                builder_.CreateCondBr(
                    builder_.CreateICmpSLT(idx,
                        builder_.getInt32(static_cast<uint32_t>(N))),
                    bodyBB, endBB);

                builder_.SetInsertPoint(bodyBB);
                llvm::Value* cur = builder_.CreateLoad(i32(), idxSlot);
                llvm::Value* ep = builder_.CreateGEP(
                    arrTy, arrAddr, {builder_.getInt64(0), cur});
                llvm::Value* el = builder_.CreateLoad(elemTy, ep);
                llvm::Value* eq =
                    emitRemoveElementEquality(el, target, elemRef, elemIsClass);
                if (!eq) return nullptr;
                builder_.CreateCondBr(eq, foundBB, nextBB);

                builder_.SetInsertPoint(foundBB);
                builder_.CreateBr(endBB);

                builder_.SetInsertPoint(nextBB);
                builder_.CreateStore(
                    builder_.CreateAdd(cur, builder_.getInt32(1)), idxSlot);
                builder_.CreateBr(condBB);

                builder_.SetInsertPoint(endBB);
                auto* phi = builder_.CreatePHI(i1(), 2, "iin.result");
                phi->addIncoming(builder_.getTrue(),  foundBB);
                phi->addIncoming(builder_.getFalse(), condBB);
                return phi;
            }
        }
        // #628 (part 1): unhandled `in` form — emit a hard error so the
        // caller sees a failed compile rather than silent garbage IR.
        ctx_.diagnostics().report(
            Severity::Error, bin.span.begin,
            "not yet implemented: codegen for this `in` expression form");
        return nullptr;
    }
    llvm::Value* l = lowerExpr(*bin.left);
    llvm::Value* r = lowerExpr(*bin.right);
    if (!l || !r) return nullptr;
    const auto& op = bin.op;
    // fix #2497 / §8.3 / §9.2 / §35.2: `this` (or `base`) inside a record/
    // choice instance method is lowered as a POINTER to the receiver storage
    // (CG9 — "this is the receiver ptr"; §35.2 — "this in a choice instance
    // method is a borrow of the receiver value"; the record analog is the
    // same receiver-ptr convention, #575/§9.5's lowerWith documents it too).
    // When such a bare-`this` operand is compared against a same-typed
    // by-value operand with `=`/`<>`, the `this` operand is a pointer while
    // the other side is a struct value — the struct-equality dispatch below
    // keys on isStructTy() and would skip it, falling through to a raw
    // ICmpEQ over mismatched operand types (ptr vs struct) and aborting in
    // the IR verifier (LLVM ICmpInst::AssertOK operand-type-mismatch). Load
    // the receiver struct through the pointer so both operands are
    // struct-typed and the record/choice auto-equality dispatch proceeds —
    // the same load `this with {…}` already does (#575, §9.5). This block
    // runs only for the AUTO-equality path: a user-declared `operator =`/
    // `<>` is intercepted earlier (the `opm` block above) and never reaches
    // here. The other operand must be a bare (non-nullable) record/choice:
    // its lowered struct matches the receiver storage shape exactly, and a
    // nullable value-typed operand lowers to a different {i8, T} wrapper
    // shape than the bare receiver (sema rejects or lifts that mix before
    // codegen, so this guard must not fire for it).
    if (op == "=" || op == "<>") {
        auto isThisIdent = [](const ast::Expr* e) -> bool {
            if (auto* id = dynamic_cast<const ast::IdentifierExpr*>(e))
                return id->name == "this" || id->name == "base";
            return false;
        };
        auto bareRecordOrChoiceStructTy = [&](const ast::Expr* other,
                                              llvm::Value* thisVal,
                                              llvm::Value* otherVal) -> llvm::Type* {
            if (!other || !thisVal || !otherVal) return nullptr;
            if (!thisVal->getType()->isPointerTy()) return nullptr;
            llvm::Type* ot = otherVal->getType();
            if (!ot->isStructTy()) return nullptr;
            const sema::TypeRef* t = ctx_.exprType(*other);
            if (!t || t->nullable) return nullptr;
            if (t->kind != sema::TypeRef::Kind::Named || !t->declaration) return nullptr;
            if (t->declaration->kind != ast::TypeDeclKind::Record &&
                t->declaration->kind != ast::TypeDeclKind::Choice) return nullptr;
            return ot;
        };
        if (isThisIdent(bin.left.get())) {
            if (llvm::Type* st = bareRecordOrChoiceStructTy(bin.right.get(), l, r))
                l = builder_.CreateLoad(st, l, "this.val");
        } else if (isThisIdent(bin.right.get())) {
            if (llvm::Type* st = bareRecordOrChoiceStructTy(bin.left.get(), r, l))
                r = builder_.CreateLoad(st, r, "this.val");
        }
    }
    bool lUnwrapped_ = false;
    bool rUnwrapped_ = false;
    // §7.11 / #914 / #1117 (proposal 0114, v0.267): the §7.11 lifted-operator
    // path that this site used to gate was deleted — sema now rejects every
    // nullable primitive operand at the operator itself. But a NARROWED
    // operand (one §7.4 has established non-null) still lowers as the
    // nullable-form `{i8, T}` struct regardless of its declared nullable
    // type. The sema EXPR type at a binary operand site is the declared
    // type (sema doesn't rewrite exprType for narrowed reads), so the
    // unwrap condition has to key on the SHAPE of the lowered value being
    // a tagged-optional struct (and being the only such shape the binary
    // operand site ever sees), not on the declared nullable flag.
    // unwrapNarrowedNullable itself verifies that the struct's payload
    // element matches the requested target type — extra-paranoid against
    // arbitrary record/class/choice boxes (#2614's "isNullablePrimTy" gate).
    {
        const sema::TypeRef* lTy = bin.left  ? ctx_.exprType(*bin.left)  : nullptr;
        const sema::TypeRef* rTy = bin.right ? ctx_.exprType(*bin.right) : nullptr;
        // The `=`/`<>` (null-check) operator already handles nullable
        // operands in its own way (extracts the hasValue tag for null
        // comparison; lines ~7740 below). Unwrapping here would clobber the
        // boxed operand the null-check is looking at. Skip for equality.
        auto unwrapIfTagged = [&](llvm::Value* v,
                                  const sema::TypeRef* t) -> llvm::Value* {
            if (!v || !t) return v;
            // §7.11 / §10.3 (#3298): a nullable ENUM operand is a nullable
            // VALUE operand — sema rejects the un-narrowed form at the
            // relational gate, so one reaching here is §7.4-narrowed and its
            // `{i8, <underlying>}` box must be unwrapped exactly as a
            // primitive's is. Restricted to primitives and enums: a record or
            // choice operand routes through a user-declared operator, which
            // takes its own argument binding.
            const bool enumOperand =
                t->kind == sema::TypeRef::Kind::Named && t->declaration &&
                t->declaration->kind == ast::TypeDeclKind::Enum;
            if (t->kind != sema::TypeRef::Kind::Primitive && !enumOperand)
                return v;
            // Build a NON-NULLABLE clone of `t` so `lowerTypeRef` produces the
            // underlying scalar type (e.g. `i32` from `int?`), not the nullable
            // `{i8, i32}` struct. unwrapNarrowedNullable then matches the
            // struct's payload element against `i32` and extracts it.
            sema::TypeRef nonnull = *t;
            nonnull.nullable = false;
            return unwrapNarrowedNullable(v, lowerTypeRef(&nonnull));
        };
        if (op != "=" && op != "<>") {
            llvm::Value* lBefore = l;
            llvm::Value* rBefore = r;
            l = unwrapIfTagged(l, lTy);
            r = unwrapIfTagged(r, rTy);
            // §7.11 (v0.286, #3298): record whether the unwrap actually fired.
            // Sema does not rewrite a narrowed operand's exprType — it stays
            // `T?` — so every downstream block keying on `!ty->nullable` reads
            // the DECLARED type and mis-routes a narrowed operand. The unwrap
            // is the only place that knows.
            // ⚠ The test is "did the value CHANGE", not "is it no longer a
            // struct": a `decimal` payload is itself a `%bz.decimal` STRUCT, so
            // an isStructTy() test reports `decimal?` as still-boxed and
            // reintroduces exactly the mis-route this flag exists to prevent.
            lUnwrapped_ = (l != lBefore);
            rUnwrapped_ = (r != rBefore);
        }
    }
    const bool lUnwrapped = lUnwrapped_;
    const bool rUnwrapped = rUnwrapped_;
    // §4.4 / §6.4 fix #521: decimal arithmetic — route via bz_decimal_* runtime.
    // Decimal values are { i32 lo32, i32 mid32, i32 hi32, i32 flags } structs.
    // All runtime functions take pointers, so we alloca the operands, store,
    // pass their addresses, then load the result.
    {
        using P = sema::PrimitiveType;
        // §11.8 (v0.176, #807): resolve T-typed operands through the active
        // substitution so a `T = decimal` constraint-operator specialization
        // routes through the decimal runtime instead of the integer ops.
        const sema::TypeRef* lTy = bin.left
            ? resolveTypeParamRef(ctx_.exprType(*bin.left)) : nullptr;
        const sema::TypeRef* rTy = bin.right
            ? resolveTypeParamRef(ctx_.exprType(*bin.right)) : nullptr;
        // #3298 (v0.286): the §7.11 lifted-arithmetic block this comment
        // historically referenced was deleted by proposal 0114 — an
        // un-narrowed nullable operand is rejected at sema and never reaches
        // codegen. A NARROWED one does, and it keeps its declared `decimal?`
        // exprType, so `!lTy->nullable` is the wrong test: it said "not a
        // decimal operand" about a bare `%bz.decimal` and routed
        // `var a: decimal? := 5; var d: decimal := a + 1;` into the INTEGER
        // path, emitting `add %bz.decimal, i32` (an IR-verification abort at
        // exit 141). An earlier revision of this comment asserted "a decimal?
        // operand therefore never reaches this decimal block" — measured
        // false. What decides is whether the unwrap above produced a bare
        // value: `decimalOperand` is "declared decimal AND lowered as a bare
        // decimal", which covers the non-nullable and the narrowed operand
        // alike and still excludes a live `{i8, %bz.decimal}` box.
        auto decimalOperand = [&](const sema::TypeRef* ty, bool unwrapped) {
            return ty && ty->kind == sema::TypeRef::Kind::Primitive &&
                   ty->primitive == P::Decimal && (!ty->nullable || unwrapped);
        };
        bool lDec = decimalOperand(lTy, lUnwrapped);
        bool rDec = decimalOperand(rTy, rUnwrapped);
        // §5.1: integers implicitly widen to decimal — promote an integer operand.
        // We detect this by: result type is decimal, and one operand is integer.
        const sema::TypeRef* resTy = resolveTypeParamRef(ctx_.exprType(bin));
        bool resDec = resTy && resTy->kind == sema::TypeRef::Kind::Primitive &&
                      resTy->primitive == P::Decimal && !resTy->nullable;
        // fix #663: if EITHER side is nullable decimal (decimal?), skip this
        // block entirely. The lifted-arithmetic block above handles decimal? op
        // decimal? (both nullable). A decimal? op decimal (mixed nullable vs.
        // non-null) comparison reaches the nullable-value equality path at
        // line ~3989, where the hasValue tag is checked and the inner payload
        // is routed through emitValueEquality → bz_decimal_cmp. Entering here
        // with a nullable side would pass the full {i8, %bz.decimal} wrapper
        // struct to bz_decimal_cmp as if it were a bare %bz.decimal → wrong.
        // #3298 (v0.286): "still a box" is the condition, not "declared
        // nullable". `=`/`<>` skip the unwrap above by design, so a genuinely
        // nullable decimal operand there is still a `{i8, %bz.decimal}` and
        // #663's skip stands unchanged; a §7.4-narrowed arithmetic operand has
        // been unwrapped and must NOT skip.
        bool eitherNullableDecimal =
            (lTy && lTy->kind == sema::TypeRef::Kind::Primitive &&
             lTy->primitive == P::Decimal && lTy->nullable && !lUnwrapped) ||
            (rTy && rTy->kind == sema::TypeRef::Kind::Primitive &&
             rTy->primitive == P::Decimal && rTy->nullable && !rUnwrapped);
        if (!eitherNullableDecimal && (lDec || rDec || resDec)) {
            llvm::StructType* dst = decimalStructType();
            // Helper: promote value to decimal struct (alloca + runtime call).
            auto toDecSlot = [&](llvm::Value* v,
                                 const sema::TypeRef* ty) -> llvm::Value* {
                if (ty && ty->kind == sema::TypeRef::Kind::Primitive &&
                    ty->primitive == P::Decimal)
                    return v;  // already decimal struct
                // fix #666: if the LLVM value is already a %bz.decimal struct
                // (e.g. the return value of a LINQ Sum/Average selector lambda
                // whose sema type was not annotated as decimal by the type
                // checker), return it directly.  Without this guard the code
                // below would try CreateSExt(%bz.decimal, i32) — invalid IR.
                if (v->getType() == dst)
                    return v;
                // Integer widening to decimal (§5.1).
                llvm::AllocaInst* slot = entryAlloca(dst);
                if (ty && ty->kind == sema::TypeRef::Kind::Primitive) {
                    // #743: honour unsigned sources (ulong/uint above the signed
                    // max) — emitIntToDecimal picks the correct conversion.
                    emitIntToDecimal(v, ty->primitive, slot);
                }
                return builder_.CreateLoad(dst, slot, "dec.promoted");
            };
            llvm::Value* lv = toDecSlot(l, lTy);
            llvm::Value* rv = toDecSlot(r, rTy);

            // Arithmetic ops: store operands into alloca slots, call runtime,
            // load result.  §6.4: div/mod are guarded against zero divisor.
            if (op == "+" || op == "-" || op == "*" ||
                op == "/" || op == "mod") {
                // §6.4: check divisor for div/mod.
                if (op == "/" || op == "mod") {
                    // Zero check: mantissa = lo32 | mid32 | hi32.
                    llvm::Value* lo  = builder_.CreateExtractValue(rv, {0u});
                    llvm::Value* mid = builder_.CreateExtractValue(rv, {1u});
                    llvm::Value* hi  = builder_.CreateExtractValue(rv, {2u});
                    llvm::Value* orv = builder_.CreateOr(
                        builder_.CreateOr(lo, mid), hi);
                    llvm::Value* isZero = builder_.CreateICmpEQ(
                        orv, llvm::ConstantInt::get(i32(), 0));
                    emitGuardThrow(isZero, "DivideByZeroException");
                }
                llvm::AllocaInst* aSlot = entryAlloca(dst);
                llvm::AllocaInst* bSlot = entryAlloca(dst);
                llvm::AllocaInst* oSlot = entryAlloca(dst);
                builder_.CreateStore(lv, aSlot);
                builder_.CreateStore(rv, bSlot);
                const char* fn_name =
                    op == "+"   ? "bz_decimal_add" :
                    op == "-"   ? "bz_decimal_sub" :
                    op == "*"   ? "bz_decimal_mul" :
                    op == "/"   ? "bz_decimal_div" :
                    /* mod */     "bz_decimal_mod";
                // per fix #521 / §6.4: pass OverflowException vtable so the
                // runtime can set bz_exc on overflow; emitPendingCheck() unwinds.
                auto getOVEVtable = [&]() -> llvm::Value* {
                    auto vit = builtinVtable_.find("OverflowException");
                    if (vit == builtinVtable_.end())
                        return llvm::ConstantPointerNull::get(ptrTy());
                    return vit->second;
                };
                auto* fn = declareC(fn_name, llvm::Type::getVoidTy(llvm_),
                                    {ptrTy(), ptrTy(), ptrTy(), ptrTy()});
                builder_.CreateCall(fn, {aSlot, bSlot, oSlot, getOVEVtable()});
                emitPendingCheck();  // per fix #521 / §6.4: runtime sets bz_exc on overflow
                return builder_.CreateLoad(dst, oSlot, "dec.res");
            }
            // Negation (unary - on decimal is handled separately via lowerUnary,
            // but mixed decimal comparisons are the binary ops below).
            // Comparisons: call bz_decimal_cmp, compare result with 0.
            if (op == "=" || op == "<>" ||
                op == "<" || op == "<=" || op == ">" || op == ">=") {
                llvm::AllocaInst* aSlot = entryAlloca(dst);
                llvm::AllocaInst* bSlot = entryAlloca(dst);
                builder_.CreateStore(lv, aSlot);
                builder_.CreateStore(rv, bSlot);
                auto* cmpFn = declareC("bz_decimal_cmp", i32(),
                                       {ptrTy(), ptrTy()});
                llvm::Value* cmp = builder_.CreateCall(cmpFn, {aSlot, bSlot},
                                                       "dec.cmp");
                llvm::Value* zero = llvm::ConstantInt::get(i32(), 0);
                if (op == "=")  return builder_.CreateICmpEQ(cmp, zero);
                if (op == "<>") return builder_.CreateICmpNE(cmp, zero);
                if (op == "<")  return builder_.CreateICmpSLT(cmp, zero);
                if (op == "<=") return builder_.CreateICmpSLE(cmp, zero);
                if (op == ">")  return builder_.CreateICmpSGT(cmp, zero);
                /* >= */        return builder_.CreateICmpSGE(cmp, zero);
            }
        }
    }
    // CG24 (§6.3): floating-point arithmetic and comparisons (IEEE 754,
    // no overflow/div0 checks). If one operand is FP and the other an
    // integer, promote the integer (§6.2 mixed-type promotion).
    // #1981 / §7.11 / §8.4: EXCLUDE a `=`/`<>` where one operand is a nullable-
    // value STRUCT ({i8 hasValue, T}) and the other is a bare FP scalar — e.g.
    // `double? = 3.14` (or `float? = 1.5f`). Nullable arithmetic/relational is
    // already intercepted by the lifted §7.11 block above; equality is NOT
    // lifted there, so such a comparison reaches here with the FP scalar operand
    // tripping this block's `isFloatingPointTy` guard. Without the exclusion,
    // `toFP()` below emitted `sitofp {i8,double} to double` on the whole wrapper
    // struct (malformed IR → verifier failure). Fall through to the null-safe
    // nullable-value equality path below, which unwraps the payload before
    // comparing.
    bool eqWithNullableStruct =
        (op == "=" || op == "<>") &&
        (l->getType()->isStructTy() || r->getType()->isStructTy());
    if ((l->getType()->isFloatingPointTy() ||
         r->getType()->isFloatingPointTy()) &&
        !eqWithNullableStruct) {
        bool lFP = l->getType()->isFloatingPointTy();
        bool rFP = r->getType()->isFloatingPointTy();
        // The result FP type is the wider of two FP operands (so `float`
        // mixed with `double` widens the float), else the lone FP type.
        llvm::Type* fty =
            (lFP && rFP)
                ? (l->getType()->getPrimitiveSizeInBits() >=
                           r->getType()->getPrimitiveSizeInBits()
                       ? l->getType()
                       : r->getType())
                : (lFP ? l->getType() : r->getType());
        auto toFP = [&](llvm::Value* v) -> llvm::Value* {
            if (!v->getType()->isFloatingPointTy())
                return builder_.CreateSIToFP(v, fty);  // §6.2 int promote
            if (v->getType() != fty)
                return builder_.CreateFPExt(v, fty);   // float → double
            return v;
        };
        l = toFP(l);
        r = toFP(r);
        if (op == "+") return builder_.CreateFAdd(l, r);
        if (op == "-") return builder_.CreateFSub(l, r);
        if (op == "*") return builder_.CreateFMul(l, r);
        if (op == "/") return builder_.CreateFDiv(l, r);
        // §4.3.1 (L308/L314): "NaN = NaN is true" — reflexive equality.
        // Emit: oeq(a,b) OR (isNaN(a) AND isNaN(b)).
        // isNaN(x) is fcmp uno x, x (unordered compare: true iff x is NaN).
        // IEEE <,<=,>,>= are NOT changed (NaN comparisons stay false, §4.3.1).
        if (op == "=") {
            llvm::Value* ieee_eq  = builder_.CreateFCmpOEQ(l, r, "feq");
            llvm::Value* l_is_nan = builder_.CreateFCmpUNO(l, l, "lnan");
            llvm::Value* r_is_nan = builder_.CreateFCmpUNO(r, r, "rnan");
            llvm::Value* both_nan = builder_.CreateAnd(l_is_nan, r_is_nan, "bnan");
            return builder_.CreateOr(ieee_eq, both_nan, "feq_ref");
        }
        // §4.3.1: `<>` is the negation of `=` (which is true for NaN=NaN),
        // so `NaN <> NaN` is false.  Emit NOT(oeq OR both-NaN).
        if (op == "<>") {
            llvm::Value* ieee_eq  = builder_.CreateFCmpOEQ(l, r, "feq");
            llvm::Value* l_is_nan = builder_.CreateFCmpUNO(l, l, "lnan");
            llvm::Value* r_is_nan = builder_.CreateFCmpUNO(r, r, "rnan");
            llvm::Value* both_nan = builder_.CreateAnd(l_is_nan, r_is_nan, "bnan");
            llvm::Value* eq_ref   = builder_.CreateOr(ieee_eq, both_nan, "feq_ref");
            return builder_.CreateNot(eq_ref, "fne_ref");
        }
        if (op == "<") return builder_.CreateFCmpOLT(l, r);
        if (op == ">") return builder_.CreateFCmpOGT(l, r);
        if (op == "<=") return builder_.CreateFCmpOLE(l, r);
        if (op == ">=") return builder_.CreateFCmpOGE(l, r);
        return nullptr;
    }
    // CG25b: unsigned integer operands select unsigned operations
    // (udiv/urem/lshr/unsigned compares, zext widening, uXXX overflow).
    bool uns = isUnsignedBinOperand(bin);
    // CG25 (§6.2): mixed-width integer operands — widen the narrower to
    // the wider (zero-extend for unsigned, sign-extend otherwise) so the
    // op and any overflow check run at one width.
    // §6.5 (#1094): a SHIFT is EXCLUDED — its result type is the LEFT operand's
    // type and the count never promotes the left operand (§6.2 promotion does
    // not apply to `<<`/`>>`; sema keeps the narrow type). Widening the narrow
    // left operand to the (wider) count's width here made the count mask use the
    // count's bit-width (`byte 1 << int 8` masked &31, shifted i32 1<<8=256, then
    // truncated back to byte = 0). Keep each operand at its own width; the shift
    // block below masks the count by the LEFT operand's width and coerces the
    // count to the left's type for the LLVM shift.
    if (l->getType()->isIntegerTy() && r->getType()->isIntegerTy() &&
        !l->getType()->isIntegerTy(1) && !r->getType()->isIntegerTy(1) &&
        l->getType() != r->getType() && op != "<<" && op != ">>") {
        unsigned lw = l->getType()->getIntegerBitWidth();
        unsigned rw = r->getType()->getIntegerBitWidth();
        llvm::Type* wide = lw >= rw ? l->getType() : r->getType();
        // #814: widen each narrower operand by ITS OWN signedness (an unsigned
        // operand zero-extends, a signed operand sign-extends), independent of
        // the operation's common-type signedness (`uns`).
        bool lU = bin.left && isUnsignedTargetExpr(*bin.left);
        bool rU = bin.right && isUnsignedTargetExpr(*bin.right);
        if (lw < rw) l = lU ? builder_.CreateZExt(l, wide)
                            : builder_.CreateSExt(l, wide);
        if (rw < lw) r = rU ? builder_.CreateZExt(r, wide)
                            : builder_.CreateSExt(r, wide);
    }
    // CG27a (§4.6): string binary operations — intercept before the
    // integer arithmetic path (string variables are ptr-typed, not int).
    // When either operand is a null literal, fall through to pointer
    // comparison (`s = null` / `s <> null` is a reference/null check).
    {
        auto isStr = [&](const ast::Expr* e) {
            if (!e) return false;
            auto* rt = ctx_.exprType(*e);
            return rt && rt->kind == sema::TypeRef::Kind::Primitive &&
                   rt->primitive == sema::PrimitiveType::String;
        };
        // Check if an expression has an unresolved generic type (T) that
        // is concretely substituted to string via genericSubstSemaTy_.
        auto isGenericString = [&](const ast::Expr* e) {
            if (!e || genericSubstSemaTy_.empty()) return false;
            auto* rt = ctx_.exprType(*e);
            if (!rt || rt->kind != sema::TypeRef::Kind::Named ||
                rt->declaration) return false;
            // Unresolved generic param — check if any substitution is String.
            if (rt->astNode && rt->astNode->name &&
                rt->astNode->name->parts.size() == 1) {
                auto sit = genericSubstSemaTy_.find(
                    rt->headName());
                if (sit != genericSubstSemaTy_.end() && sit->second) {
                    auto* srt = sit->second;
                    return srt->kind == sema::TypeRef::Kind::Primitive &&
                           srt->primitive == sema::PrimitiveType::String;
                }
            }
            return false;
        };
        bool eitherNull =
            (bin.left && dynamic_cast<const ast::NullLiteralExpr*>(
                              bin.left.get())) ||
            (bin.right && dynamic_cast<const ast::NullLiteralExpr*>(
                               bin.right.get()));
        // CG-nullable: `x = null` / `x <> null` for nullable value types
        // (primitives, and a nullable enum — issue #364). The tagged struct is
        // not ptr-typed, so the normal ptr-null check below wouldn't fire.
        // Extract the hasValue field.
        if (eitherNull && (op == "=" || op == "<>")) {
            // Determine which side is the nullable value (non-null side).
            const ast::Expr* nvExpr = nullptr;
            llvm::Value* nvVal = nullptr;
            if (bin.left && !dynamic_cast<const ast::NullLiteralExpr*>(bin.left.get())) {
                if (auto* rt = ctx_.exprType(*bin.left))
                    if (isNullableValueTy(rt)) { nvExpr = bin.left.get(); nvVal = l; }
            } else if (bin.right && !dynamic_cast<const ast::NullLiteralExpr*>(bin.right.get())) {
                if (auto* rt = ctx_.exprType(*bin.right))
                    if (isNullableValueTy(rt)) { nvExpr = bin.right.get(); nvVal = r; }
            }
            // R33: a nullable TYPE-PARAMETER field (`T?`) lowers to the
            // nullable-value struct {i8 hasValue; <T> value} even when T
            // instantiates to a class; isNullableValueTy is false for the
            // unresolved param, so the above misses it and `T? = null` would
            // fall through to struct-equality with a null record type — crashing
            // with the LLVM checkGEPType assertion.  Detect the nullable-value
            // struct by its LLVM shape (element 0 is the hasValue tag) so the
            // null comparison lowers to the hasValue check.
            if (!nvVal) {
                llvm::Value* cand =
                    (bin.left &&
                     !dynamic_cast<const ast::NullLiteralExpr*>(bin.left.get())) ? l
                  : (bin.right &&
                     !dynamic_cast<const ast::NullLiteralExpr*>(bin.right.get())) ? r
                  : nullptr;
                if (cand && cand->getType()->isStructTy()) {
                    auto* st = llvm::cast<llvm::StructType>(cand->getType());
                    if (st->getNumElements() >= 1 &&
                        st->getElementType(0) == nullablePrimHasValTy())
                        nvVal = cand;
                }
            }
            if (nvVal && nvVal->getType()->isStructTy()) {
                // hasValue field (index 0). isNull when hasValue=false.
                llvm::Value* hasVal = nullablePrimHasValue(nvVal);
                // = null → !hasValue; <> null → hasValue
                return op == "=" ? builder_.CreateNot(hasVal) : hasVal;
            }
        }
        // CG-nullable / issue #364: `x = v` / `x <> v` where ONE side is a
        // nullable value (the tagged {i8 hasValue; T value} struct) and the
        // other is a non-null plain value of the inner type T — e.g.
        // `(c: Color?) = Color.Green`, the core test that a null `enum?` is
        // distinct from any enum value (including the value-0 member). The
        // struct-equality path below (op==`=` && l isStructTy) would feed a
        // scalar T into emitStructEquality and crash; intercept here. The
        // null-safe `=` of §8.4: null = nonNull is false; otherwise the inner
        // T's `=`. So result = hasValue AND (innerValue = v).
        if ((op == "=" || op == "<>") && !eitherNull) {
            llvm::Value* structSide = nullptr;
            llvm::Value* plainSide = nullptr;
            const sema::TypeRef* nvTy = nullptr;
            if (bin.left) {
                if (auto* rt = ctx_.exprType(*bin.left))
                    if (isNullableValueTy(rt) && l->getType()->isStructTy()) {
                        structSide = l; plainSide = r; nvTy = rt;
                    }
            }
            if (!structSide && bin.right) {
                if (auto* rt = ctx_.exprType(*bin.right))
                    if (isNullableValueTy(rt) && r->getType()->isStructTy()) {
                        structSide = r; plainSide = l; nvTy = rt;
                    }
            }
            // #1113: a narrow nullable payload (`byte?`/`short?` → i8/i16)
            // compared to an int LITERAL (which lowers at i32) arrives here with a
            // WIDER plainSide; the strict `payloadTy == plainSide` gate then fell
            // through to a path that ICmp'd the i8 payload against the i32 literal
            // — an LLVM "both operands same type" assert (a compiler crash). Mirror
            // the non-nullable comparison: WIDEN the narrower operand to the wider
            // (NOT truncate the literal — truncating wraps an out-of-range value
            // into the payload range, e.g. `byte? = 261` would wrongly be equal to
            // a `5` payload). Each side widens by ITS OWN signedness so the value
            // is preserved (`sbyte -5` sign-extends; `byte 5` zero-extends). Only
            // matching integer kinds; a non-integer / equal-width pair is unchanged.
            llvm::Value* eqPayload =
                structSide ? nullablePrimValue(structSide) : nullptr;
            if (structSide && plainSide && nvTy && eqPayload &&
                eqPayload->getType() != plainSide->getType() &&
                eqPayload->getType()->isIntegerTy() &&
                plainSide->getType()->isIntegerTy()) {
                const ast::Expr* structExpr =
                    (structSide == l) ? bin.left.get() : bin.right.get();
                const ast::Expr* plainExpr =
                    (plainSide == l) ? bin.left.get() : bin.right.get();
                bool payloadUns =
                    structExpr && isUnsignedTargetExpr(*structExpr);
                bool plainUns = plainExpr && isUnsignedTargetExpr(*plainExpr);
                unsigned pw = eqPayload->getType()->getIntegerBitWidth();
                unsigned sw = plainSide->getType()->getIntegerBitWidth();
                if (pw < sw)
                    eqPayload = builder_.CreateIntCast(
                        eqPayload, plainSide->getType(), /*isSigned=*/!payloadUns);
                else
                    plainSide = builder_.CreateIntCast(
                        plainSide, eqPayload->getType(), /*isSigned=*/!plainUns);
            }
            // #1981 / §6.2 / §7.11: an FP nullable payload (`double?`/`float?`)
            // compared to a differently-typed SCALAR plain operand needs the same
            // §6.2 numeric promotion the non-nullable path applies — on the
            // UNWRAPPED payload/plain scalars, never the wrapper struct. Promote
            // an integer operand to the FP type (SIToFP/UIToFP) and widen a
            // narrower float to the wider FP type (FPExt), so the value-equality
            // below sees matching scalar types. Excludes a struct operand (a
            // both-nullable `double? = double?` keeps plainSide as its own
            // {i8,T} struct — handled by the struct-equality fall-through, not
            // here) and decimal? (payload is a %bz.decimal struct, not FP).
            if (structSide && plainSide && nvTy && eqPayload &&
                eqPayload->getType() != plainSide->getType() &&
                !eqPayload->getType()->isStructTy() &&
                !plainSide->getType()->isStructTy() &&
                (eqPayload->getType()->isFloatingPointTy() ||
                 plainSide->getType()->isFloatingPointTy())) {
                bool payFP   = eqPayload->getType()->isFloatingPointTy();
                bool plainFP = plainSide->getType()->isFloatingPointTy();
                // Common FP type: the wider of two FP operands, else the lone FP.
                llvm::Type* fty =
                    (payFP && plainFP)
                        ? (eqPayload->getType()->getPrimitiveSizeInBits() >=
                                   plainSide->getType()->getPrimitiveSizeInBits()
                               ? eqPayload->getType()
                               : plainSide->getType())
                        : (payFP ? eqPayload->getType() : plainSide->getType());
                const ast::Expr* structExpr =
                    (structSide == l) ? bin.left.get() : bin.right.get();
                const ast::Expr* plainExpr =
                    (plainSide == l) ? bin.left.get() : bin.right.get();
                bool payUns   = structExpr && isUnsignedTargetExpr(*structExpr);
                bool plainUns = plainExpr && isUnsignedTargetExpr(*plainExpr);
                auto toFP = [&](llvm::Value* v, bool uns) -> llvm::Value* {
                    if (!v->getType()->isFloatingPointTy())
                        return uns ? builder_.CreateUIToFP(v, fty)
                                   : builder_.CreateSIToFP(v, fty);
                    if (v->getType() != fty)
                        return builder_.CreateFPExt(v, fty);
                    return v;
                };
                eqPayload = toFP(eqPayload, payUns);
                plainSide = toFP(plainSide, plainUns);
            }
            if (structSide && plainSide && nvTy && eqPayload &&
                plainSide->getType() == eqPayload->getType()) {
                llvm::Value* hasVal = nullablePrimHasValue(structSide);
                // Inner equality of the (possibly widened) value slot against the
                // plain value, by the inner (non-nullable) type's rule (an enum
                // compares its i32 value; a float would use FCmp). Equality is
                // sign-agnostic, but the widening above preserved each value.
                sema::TypeRef innerTy = *nvTy;
                innerTy.nullable = false;
                llvm::Value* innerEq =
                    emitValueEquality(eqPayload, plainSide, &innerTy);
                llvm::Value* eq = builder_.CreateAnd(hasVal, innerEq);
                return op == "=" ? eq : builder_.CreateNot(eq);
            }
        }
        bool lIsStr = isStr(bin.left.get()) || isGenericString(bin.left.get());
        bool rIsStr = isStr(bin.right.get()) || isGenericString(bin.right.get());
        bool lIsPtr = l->getType()->isPointerTy();
        bool rIsPtr = r->getType()->isPointerTy();
        // Equality/inequality: use string ops when at least one side is a
        // known string AND both are ptr-typed (the other ptr is also a string
        // from a generic context where the sema type is unresolved T=string).
        bool useStrEq = (lIsStr || rIsStr) && lIsPtr && rIsPtr && !eitherNull;
        if (useStrEq && (op == "=" || op == "<>")) {
            llvm::Value* cmpResult;
            if (op == "=") cmpResult = emitStringEq(l, r);
            else           cmpResult = builder_.CreateNot(emitStringEq(l, r));
            // #429 (OPERATOR-OPERAND category) / §4.6 / §28.9: a +1 string
            // producer (concat / interpolation / range-slice / primitive-ToString /
            // user-method-return) fed as a COMPARISON OPERAND is consumed by the
            // comparison (= / <>) but not by any concat chain — nothing else holds
            // its reference.  Release it after emitStringEq has produced the
            // boolean result.  Both sides may be producers independently.
            if (bin.left  && isPlusOneStringProducer(bin.left.get()))
                emitStringRelease(l);
            if (bin.right && isPlusOneStringProducer(bin.right.get()))
                emitStringRelease(r);
            return cmpResult;
        }
        // FINDING [2] / §4.6 / §28.9: release a concat OPERAND that is itself a
        // fresh +1 string intermediate this codegen produced — i.e. a nested
        // string `+` or interpolation (`isPlusOneStringProducer`), so its
        // lowered value has no named-local owner and no scope-end release armed
        // for it (the named-local release path only covers LocalDecl nodes). For
        // a chained concat `a+b+c` the inner `a+b` result is consumed by the
        // outer concat and would otherwise leak. The release runs only AFTER
        // emitStringConcat has produced the result (its fresh +1 copy), so the
        // intermediate is fully consumed first — no UAF, no double-free. A plain
        // identifier / member / call operand is NOT a +1 producer here, so a
        // borrowed value (named local, field, call result) is never released.
        auto releaseConcatIntermediate = [&](const ast::Expr* operandExpr,
                                             llvm::Value* operandVal) {
            if (operandExpr && isPlusOneStringProducer(operandExpr))
                emitStringRelease(operandVal);
        };
        // §4.6: string concat when at least one side is a known string and
        // both operands are ptr-typed (handles e.g. string.From(ch) + s where
        // the From(ch) result may not be sema-annotated as string yet).
        if ((lIsStr || rIsStr) && lIsPtr && rIsPtr && !eitherNull && op == "+") {
            llvm::Value* res = emitStringConcat(l, r);
            releaseConcatIntermediate(bin.left.get(), l);
            releaseConcatIntermediate(bin.right.get(), r);
            return res;
        }
        if (lIsStr && !eitherNull) {
            if (op == "=") return emitStringEq(l, r);
            if (op == "<>") return builder_.CreateNot(emitStringEq(l, r));
            if (op == "+") {
                // §4.6: string + T — if the right operand is not a string,
                // convert it via emitPrimitiveToString first.
                llvm::Value* rStr = r;
                auto* rt = ctx_.exprType(*bin.right);
                if (rt && rt->kind == sema::TypeRef::Kind::Primitive &&
                    rt->primitive != sema::PrimitiveType::String) {
                    rStr = emitPrimitiveToString(r, rt->primitive);
                } else if (!r->getType()->isPointerTy()) {
                    // Fallback: r has unknown/non-primitive sema type but is
                    // an integer-typed LLVM value (e.g. char from a generic
                    // context T=char). Infer the type from the LLVM type and
                    // convert to string. i32 → Char (preserves code-point).
                    using P = sema::PrimitiveType;
                    if (r->getType()->isIntegerTy(1))
                        rStr = emitPrimitiveToString(r, P::Bool);
                    else if (r->getType()->isIntegerTy(32))
                        rStr = emitPrimitiveToString(r, P::Char);
                    else if (r->getType()->isIntegerTy(64))
                        rStr = emitPrimitiveToString(r, P::Long);
                    else if (r->getType()->isDoubleTy())
                        rStr = emitPrimitiveToString(r, P::Double);
                    else if (r->getType()->isFloatTy())
                        rStr = emitPrimitiveToString(r, P::Float);
                }
                llvm::Value* res = emitStringConcat(l, rStr);
                // FINDING [2]: release the left operand if it is a fresh +1
                // string intermediate (a nested `+`/interpolation). Also release
                // a freshly-converted right operand (emitPrimitiveToString
                // allocates a +1 temporary distinct from the original `r`); when
                // no conversion happened `rStr == r` is a borrow and is left
                // alone. Both run after the concat result is produced.
                releaseConcatIntermediate(bin.left.get(), l);
                if (rStr != r) emitStringRelease(rStr);
                return res;
            }
            // CG27b (§4.6): ordinal comparison operators.
            if (op == "<" || op == ">" || op == "<=" || op == ">=") {
                llvm::Value* cmp = emitStringOrdCmp(l, r);
                llvm::Value* zero = llvm::ConstantInt::get(i32(), 0);
                llvm::Value* ordResult;
                if (op == "<")       ordResult = builder_.CreateICmpSLT(cmp, zero);
                else if (op == ">")  ordResult = builder_.CreateICmpSGT(cmp, zero);
                else if (op == "<=") ordResult = builder_.CreateICmpSLE(cmp, zero);
                else                 ordResult = builder_.CreateICmpSGE(cmp, zero);
                // #429 (OPERATOR-OPERAND category) / §4.6 / §28.9: release +1
                // string producer operands after the ordinal comparison, mirroring
                // the equality-operator release above.
                if (bin.left  && isPlusOneStringProducer(bin.left.get()))
                    emitStringRelease(l);
                if (bin.right && isPlusOneStringProducer(bin.right.get()))
                    emitStringRelease(r);
                return ordResult;
            }
        }
        // §4.6: primitive + string — convert lhs to string first.
        if (isStr(bin.right.get()) && op == "+" && !eitherNull) {
            auto* lt = ctx_.exprType(*bin.left);
            if (lt && lt->kind == sema::TypeRef::Kind::Primitive &&
                lt->primitive != sema::PrimitiveType::String) {
                llvm::Value* lStr = emitPrimitiveToString(l, lt->primitive);
                llvm::Value* res = emitStringConcat(lStr, r);
                // FINDING [2]: lStr is a fresh +1 conversion temporary; release
                // it (consumed by the concat). Release the right operand too if
                // it is a fresh +1 string intermediate. Both after the result.
                emitStringRelease(lStr);
                releaseConcatIntermediate(bin.right.get(), r);
                return res;
            }
        }
    }
    // CG22 (§6.1): integer + - * are overflow-checked; / and mod also
    // raise on a zero divisor and on INT_MIN / -1. Applies to integer
    // operands only (bool uses and/or; floats — CG24 — are unchecked).
    bool checkedInt = l->getType()->isIntegerTy() &&
                      !l->getType()->isIntegerTy(1);
    if (checkedInt) {
        if (op == "+")
            return emitCheckedArith(
                uns ? llvm::Intrinsic::uadd_with_overflow
                    : llvm::Intrinsic::sadd_with_overflow, l, r);
        if (op == "-")
            return emitCheckedArith(
                uns ? llvm::Intrinsic::usub_with_overflow
                    : llvm::Intrinsic::ssub_with_overflow, l, r);
        if (op == "*")
            return emitCheckedArith(
                uns ? llvm::Intrinsic::umul_with_overflow
                    : llvm::Intrinsic::smul_with_overflow, l, r);
        if (op == "/" || op == "mod") {
            emitGuardThrow(builder_.CreateICmpEQ(r, zeroLike(r)),
                           "DivideByZeroException");
            if (uns)
                return op == "/" ? builder_.CreateUDiv(l, r)
                                 : builder_.CreateURem(l, r);
            // Signed INT_MIN op -1 (§6.1): division's mathematical result
            // (-INT_MIN) is not representable in the result type → raise
            // OverflowException. Modulo's mathematical result is 0, which IS
            // representable, so `mod` must NOT raise. But LLVM `srem` of
            // INT_MIN,-1 is undefined behaviour (LangRef) and x86 IDIV faults
            // (SIGFPE), so on that one path force the divisor to 1 and select 0.
            llvm::Value* minV = minIntLike(r);
            llvm::Value* negOne =
                llvm::ConstantInt::getSigned(r->getType(), -1);
            llvm::Value* isMinNegOne =
                builder_.CreateAnd(builder_.CreateICmpEQ(l, minV),
                                   builder_.CreateICmpEQ(r, negOne));
            if (op == "/") {
                emitGuardThrow(isMinNegOne, "OverflowException");
                return builder_.CreateSDiv(l, r);
            }
            llvm::Value* safeR = builder_.CreateSelect(
                isMinNegOne, llvm::ConstantInt::getSigned(r->getType(), 1), r);
            llvm::Value* rem = builder_.CreateSRem(l, safeR);
            return builder_.CreateSelect(
                isMinNegOne, llvm::ConstantInt::get(r->getType(), 0), rem);
        }
    }
    if (op == "+") return builder_.CreateAdd(l, r);
    if (op == "-") return builder_.CreateSub(l, r);
    if (op == "*") return builder_.CreateMul(l, r);
    if (op == "/") return uns ? builder_.CreateUDiv(l, r)
                              : builder_.CreateSDiv(l, r);
    if (op == "mod") return uns ? builder_.CreateURem(l, r)
                                : builder_.CreateSRem(l, r);
    // Bitwise ops only valid on integer-typed operands — pointers (string refs)
    // would crash LLVM IR construction.
    if (l->getType()->isIntegerTy() && r->getType()->isIntegerTy()) {
        if (op == "&") return builder_.CreateAnd(l, r);
        if (op == "|") return builder_.CreateOr(l, r);
        if (op == "^") return builder_.CreateXor(l, r);
        // §6.5 (v0.242, #3045): the shift count's DOMAIN is `0 .. width−1` of
        // the LEFT operand's type; outside it the shift FAULTS with
        // `OverflowException` (§31.4) instead of being reduced modulo the
        // width. #1094's point still holds and is what makes the domain
        // width-dependent: `bits` is the LEFT operand's width — the count does
        // not promote it (§6.2 exclusion above) — so `byte << 8` is out of
        // domain (8 for byte, 16 for short, 32/64 for the rest). A CONSTANT
        // out-of-domain count is rejected in sema (§17.14), so only a runtime
        // count reaches the guard. emitShiftCountInDomain also coerces the
        // count to the left operand's type, which LLVM requires.
        if (op == "<<" || op == ">>") {
            bool cntUns = bin.right && isUnsignedTargetExpr(*bin.right);
            llvm::Value* cnt = emitShiftCountInDomain(l, r, cntUns);
            if (op == "<<")  return builder_.CreateShl(l, cnt);
            // #814 / §6.5: `>>` fill is logical (LShr) iff the SHIFTED value (the
            // LEFT operand) is unsigned; arithmetic (AShr) for a signed value.
            // The shift count's signedness is irrelevant.
            bool lShiftUns = bin.left && isUnsignedTargetExpr(*bin.left);
            return lShiftUns ? builder_.CreateLShr(l, cnt)
                             : builder_.CreateAShr(l, cnt);
        }
    }
    // §8.3 (equality table) / §15.1.2: inline arrays `[N]T` compare
    // element-by-element value equality — dispatch before the scalar integer
    // path (ICmp doesn't accept aggregate/array operands). An inline array
    // lowers to an LLVM ArrayType, not a StructType, so the record branch below
    // does not catch it. Recover the array's element type from either operand so
    // per-element comparison knows the Bezel element type (string elements
    // compare by content, record elements recurse — per §15.1.2).
    if ((op == "=" || op == "<>") && l->getType()->isArrayTy()) {
        const sema::TypeRef* elemTy = nullptr;
        for (const ast::Expr* side : {bin.left.get(), bin.right.get()}) {
            if (!side) continue;
            if (auto* t = ctx_.exprType(*side))
                if (t->kind == sema::TypeRef::Kind::InlineArray &&
                    t->arrayElement) {
                    elemTy = t->arrayElement; break;
                }
        }
        llvm::Value* eq = emitInlineArrayEquality(
            l, r, llvm::cast<llvm::ArrayType>(l->getType()), elemTy);
        return op == "=" ? eq : builder_.CreateNot(eq);
    }
    // §8.4 / §6.2 / #1991: BOTH sides are nullable value types but with
    // DIFFERENT payload widths — `float? = double?` / `float? <> double?`. The
    // same-type branch below (l->getType() == r->getType()) does not fire, the
    // mixed nullable-vs-scalar #1981 promotion excludes a struct operand, and
    // the bare struct-equality fall-through would extract MISMATCHED payload
    // types → `fcmp oeq float, double` (malformed IR, an IR-verify abort). Do
    // here what #1981 did on the scalar path: unwrap both payloads, promote the
    // narrower FP payload to the wider FP type (FPExt) — integer widths widen by
    // each side's own signedness — then apply the null-safe §8.4 rule (both-null
    // → equal; exactly-one-null → not-equal; both-present → the promoted-payload
    // equality). The op runs on the promoted scalars, so no `fcmp` on mismatched
    // FP widths is ever emitted.
    if ((op == "=" || op == "<>") && l->getType()->isStructTy() &&
        r->getType()->isStructTy() && l->getType() != r->getType()) {
        const sema::TypeRef* lnv = nullptr;
        const sema::TypeRef* rnv = nullptr;
        if (bin.left)
            if (auto* t = ctx_.exprType(*bin.left))
                if (isNullableValueTy(t) &&
                    l->getType() == nullableValueStructTy(t))
                    lnv = t;
        if (bin.right)
            if (auto* t = ctx_.exprType(*bin.right))
                if (isNullableValueTy(t) &&
                    r->getType() == nullableValueStructTy(t))
                    rnv = t;
        if (lnv && rnv) {
            llvm::Value* lHas = nullablePrimHasValue(l);   // i1
            llvm::Value* rHas = nullablePrimHasValue(r);
            llvm::Value* lPay = nullablePrimValue(l);
            llvm::Value* rPay = nullablePrimValue(r);
            bool lFP = lPay->getType()->isFloatingPointTy();
            bool rFP = rPay->getType()->isFloatingPointTy();
            bool lUns = bin.left  && isUnsignedTargetExpr(*bin.left);
            bool rUns = bin.right && isUnsignedTargetExpr(*bin.right);
            if (lFP || rFP) {
                // §6.2: the wider of two FP payloads, else the lone FP type.
                llvm::Type* fty =
                    (lFP && rFP)
                        ? (lPay->getType()->getPrimitiveSizeInBits() >=
                                   rPay->getType()->getPrimitiveSizeInBits()
                               ? lPay->getType()
                               : rPay->getType())
                        : (lFP ? lPay->getType() : rPay->getType());
                auto toFP = [&](llvm::Value* v, bool u) -> llvm::Value* {
                    if (!v->getType()->isFloatingPointTy())
                        return u ? builder_.CreateUIToFP(v, fty)
                                 : builder_.CreateSIToFP(v, fty);
                    if (v->getType() != fty)
                        return builder_.CreateFPExt(v, fty);
                    return v;
                };
                lPay = toFP(lPay, lUns);
                rPay = toFP(rPay, rUns);
            } else if (lPay->getType()->isIntegerTy() &&
                       rPay->getType()->isIntegerTy() &&
                       lPay->getType() != rPay->getType()) {
                // §6.2: widen the narrower integer payload by its own signedness.
                unsigned lb = lPay->getType()->getIntegerBitWidth();
                unsigned rb = rPay->getType()->getIntegerBitWidth();
                if (lb < rb)
                    lPay = builder_.CreateIntCast(lPay, rPay->getType(), !lUns);
                else
                    rPay = builder_.CreateIntCast(rPay, lPay->getType(), !rUns);
            }
            // Null-safe §8.4 on the promoted payloads: tags equal AND (both-null
            // OR the inner values equal). emitValueEquality on the two scalars
            // applies the inner type's rule (FP uses the NaN-aware oeq of §4.3.1).
            sema::TypeRef innerTy = *lnv;
            innerTy.nullable = false;
            llvm::Value* tagsEq =
                builder_.CreateICmpEQ(lHas, rHas, "eq.nv2tag");
            llvm::Value* bothHave =
                builder_.CreateAnd(lHas, rHas, "eq.nv2both");
            llvm::Value* payEq = emitValueEquality(lPay, rPay, &innerTy);
            llvm::Value* inner = builder_.CreateOr(
                builder_.CreateNot(bothHave), payEq, "eq.nv2inner");
            llvm::Value* eq = builder_.CreateAnd(tagsEq, inner, "eq.nv2");
            return op == "=" ? eq : builder_.CreateNot(eq);
        }
    }
    // §8.4 / issue #416: BOTH sides are nullable value types (the tagged
    // {i8 hasValue; T value} struct) — e.g. `record? = record?`, `int? = int?`,
    // `enum? = enum?`. Route to emitValueEquality's nullable-value branch, which
    // applies the null-safe `=` (both-null → equal; one-null → not-equal;
    // neither-null → inner T equality). This must precede the bare struct path
    // below: a nullable RECORD now lowers to the tagged struct too, so without
    // this it would reach emitStructEquality with the wrong (tag) shape.
    if ((op == "=" || op == "<>") && l->getType()->isStructTy() &&
        l->getType() == r->getType()) {
        const sema::TypeRef* nvTy = nullptr;
        for (const ast::Expr* side : {bin.left.get(), bin.right.get()}) {
            if (!side) continue;
            if (auto* t = ctx_.exprType(*side))
                if (isNullableValueTy(t) &&
                    l->getType() == nullableValueStructTy(t)) {
                    nvTy = t; break;
                }
        }
        if (nvTy) {
            llvm::Value* eq = emitValueEquality(l, r, nvTy);
            return op == "=" ? eq : builder_.CreateNot(eq);
        }
    }
    // CG29b (§8.3): records compare structurally — dispatch before the
    // scalar integer path (ICmp doesn't accept struct operands). Recover the
    // record's declared type from either operand so per-field comparison knows
    // the Bezel field types (string fields compare by content — per §8.3).
    // §35.7 (v0.210, #2083 rework finding 2): a BARE CHOICE operand
    // (`Color.Green(7) = Color.Green(7)`, sema-legal for value-like choices
    // per the §35.7 use-site rule) is ALSO a struct-typed operand and must
    // dispatch emitChoiceEquality — the record-only scan let it fall through
    // to emitStructEquality's null-recTy shape-compare, which compares every
    // payload slot including the inactive-case `undef` slots (spurious
    // inequality, and an undef-read for pointer payloads).
    if ((op == "=" || op == "<>") && l->getType()->isStructTy()) {
        const sema::TypeSymbol* recTy = nullptr;
        const sema::TypeRef* recTyRef = nullptr;
        const sema::TypeSymbol* choTy = nullptr;
        const sema::TypeRef* choTyRef = nullptr;
        for (const ast::Expr* side : {bin.left.get(), bin.right.get()}) {
            if (!side) continue;
            if (auto* t = ctx_.exprType(*side))
                if (t->kind == sema::TypeRef::Kind::Named && t->declaration) {
                    if (t->declaration->kind == ast::TypeDeclKind::Record) {
                        recTy = t->declaration; recTyRef = t; break;
                    }
                    if (t->declaration->kind == ast::TypeDeclKind::Choice) {
                        choTy = t->declaration; choTyRef = t; break;
                    }
                }
        }
        if (choTy) {
            // §35.7 / §14.1 (#2047): for a GENERIC choice (`Result<int, string>
            // = Result<int, string>`) install the operand's concrete type args so
            // emitChoiceEquality resolves each payload `T`/`E` → its concrete type
            // — a `string` payload compares by CONTENT, not pointer identity.
            // RecordSubstGuard now covers choices (broadened for #2047); a no-op
            // for a non-generic choice.
            RecordSubstGuard choEqSubst(this, choTyRef);
            llvm::Value* eq = emitChoiceEquality(l, r, choTy);
            return op == "=" ? eq : builder_.CreateNot(eq);
        }
        // issue #462 / §8.3 / §14.1: for a GENERIC record (`Box<string> =
        // Box<string>`) install the operand's concrete type args so the
        // synthesized per-field equality resolves `T` → its concrete type (the
        // string field compares by CONTENT, not pointer identity). A no-op for a
        // non-generic record. The operand values already carry the specialized
        // struct shape (construction is specialized since #460), so only the
        // per-field declared-type resolution needs the subst here.
        RecordSubstGuard eqSubst(this, recTyRef);
        llvm::Value* eq = emitStructEquality(
            l, r, llvm::cast<llvm::StructType>(l->getType()), recTy);
        return op == "=" ? eq : builder_.CreateNot(eq);
    }
    // Guard against type mismatch when one operand is a null literal
    // (ptrTy()) and the other is a non-pointer value type (e.g. `n is null`
    // desugars to `n = null` but `n: int` can never be null).
    {
    bool eitherNull2 =
        (bin.left && dynamic_cast<const ast::NullLiteralExpr*>(bin.left.get())) ||
        (bin.right && dynamic_cast<const ast::NullLiteralExpr*>(bin.right.get()));
    if (l->getType() != r->getType()) {
        if ((op == "=" || op == "<>") && eitherNull2) {
            // Type mismatch with a null literal: one side is a null ptr,
            // the other is a non-pointer (value) type that can never be null.
            bool lIsPtr = l->getType()->isPointerTy();
            bool rIsPtr = r->getType()->isPointerTy();
            if (lIsPtr && rIsPtr) {
                // Both pointer — compare normally (fall through).
            } else if (lIsPtr) {
                // Left is ptr, right is non-ptr value — compare left to null.
                llvm::Value* eq = builder_.CreateICmpEQ(
                    l, llvm::ConstantPointerNull::get(ptrTy()));
                return op == "=" ? eq : builder_.CreateNot(eq);
            } else if (rIsPtr) {
                // Right is ptr (the null literal), left is non-ptr — non-ptr
                // can never be null: = null is always false, <> null is true.
                return builder_.getInt1(op == "<>");
            } else {
                // Neither is a ptr — both are value types (shouldn't be null).
                return builder_.getInt1(op == "<>");
            }
        }
        // For other mismatched integer comparisons, extend to match widths.
        if (l->getType()->isIntegerTy() && r->getType()->isIntegerTy()) {
            unsigned lb = l->getType()->getIntegerBitWidth();
            unsigned rb = r->getType()->getIntegerBitWidth();
            if (lb < rb) l = builder_.CreateSExt(l, r->getType());
            else r = builder_.CreateSExt(r, l->getType());
        }
    }
    } // end eitherNull2 guard
    // §8.4 (#1106): `=`/`<>` on USER-CLASS-typed operands invokes the class's
    // `Equals` (null-safe), never a raw pointer compare. Classes cannot overload
    // `=`/`<>` (§25.2); they customize equality via an `Equals` override, and `=`
    // must dispatch it. Route both operands through lowerObjectEquals (vtable
    // kEqualsSlot dispatch — the override, or the default reference-identity
    // thunk for a class with none, so a non-overriding class keeps ptr-compare
    // semantics; null-safe on the receiver). Excludes a null-literal operand
    // (`a = null` is the reference-equality-to-null form, handled by the scalar
    // path below) and fires only when BOTH operands are user-class pointers
    // (Object/interface/record/builtin handled elsewhere). Without this, `a = b`
    // on a class with an overridden Equals did a raw ICmp ptr-compare and
    // bypassed the override (defect 2).
    if ((op == "=" || op == "<>") &&
        l->getType()->isPointerTy() && r->getType()->isPointerTy()) {
        auto isUserClassExpr = [&](const ast::Expr* e) -> bool {
            if (!e || dynamic_cast<const ast::NullLiteralExpr*>(e)) return false;
            const sema::TypeRef* t = ctx_.exprType(*e);
            return t && t->kind == sema::TypeRef::Kind::Named && t->declaration &&
                   t->declaration->kind == ast::TypeDeclKind::Class;
        };
        if (isUserClassExpr(bin.left.get()) && isUserClassExpr(bin.right.get())) {
            if (llvm::Value* eq = lowerObjectEquals(l, r))
                return op == "=" ? eq : builder_.CreateNot(eq);
        }
    }
    // §27.7.1 (v0.180): `=`/`<>` on FUNCTION-typed operands — a lambda equals
    // only itself, but a method/property reference equals another reference
    // to the same method (same receiver, for an instance reference) even
    // though each reference formation mints a FRESH closure cell (#1170,
    // makeMethodThunk/buildMethodClosureCell). A raw ICmp pointer compare
    // (the fallback below) is therefore wrong for two separately-formed
    // references to the same method — it always says "unequal". Route
    // through bz_closure_value_equals, the SAME {fn, receiver} match `-=`
    // already uses (bz_event_remove_closure / bz_closure_same), so the
    // general operator and the `-=` desugaring agree. Excludes a null-
    // literal operand (handled by the plain pointer compare below) and
    // fires only when BOTH operands are function-typed.
    if ((op == "=" || op == "<>") &&
        l->getType()->isPointerTy() && r->getType()->isPointerTy()) {
        auto isFunctionExpr = [&](const ast::Expr* e) -> bool {
            if (!e || dynamic_cast<const ast::NullLiteralExpr*>(e)) return false;
            const sema::TypeRef* t = ctx_.exprType(*e);
            return t && t->kind == sema::TypeRef::Kind::Function;
        };
        if (isFunctionExpr(bin.left.get()) && isFunctionExpr(bin.right.get())) {
            auto* eqFn = declareC("bz_closure_value_equals",
                                  llvm::Type::getInt32Ty(llvm_), {ptrTy(), ptrTy()});
            llvm::Value* raw = builder_.CreateCall(eqFn, {l, r}, "fn.eqraw");
            llvm::Value* eq = builder_.CreateICmpNE(
                raw, builder_.getInt32(0), "fn.eq");
            return op == "=" ? eq : builder_.CreateNot(eq);
        }
    }
    // CG4: integer comparisons → i1.
    if (op == "=") return builder_.CreateICmpEQ(l, r);
    if (op == "<>") return builder_.CreateICmpNE(l, r);
    // #1049: a relational comparison must be UNSIGNED when one operand is an
    // unsigned type and the other is a NON-NEGATIVE constant (§5.1: the constant
    // fits and is treated as the unsigned type). `isUnsignedBinOperand` requires
    // BOTH operands unsigned (correct for arithmetic signedness), so
    // `ulongVar > 1000` — unsigned value vs a signed int literal — used a SIGNED
    // compare and gave the wrong result once the unsigned value exceeded the
    // signed max (its high bit set): `ulong 18e18 > 1000` returned false. Gate on
    // the OTHER operand being a non-negative constant so a possibly-negative
    // signed operand keeps the signed compare (`u > -5` stays correct).
    bool unsCmp = uns;
    if (!unsCmp) {
        bool lU = bin.left  && isUnsignedTargetExpr(*bin.left);
        bool rU = bin.right && isUnsignedTargetExpr(*bin.right);
        auto nonNegConst = [](llvm::Value* v) {
            auto* ci = llvm::dyn_cast<llvm::ConstantInt>(v);
            return ci && !ci->isNegative();
        };
        if ((lU && nonNegConst(r)) || (rU && nonNegConst(l)))
            unsCmp = true;
    }
    if (op == "<") return unsCmp ? builder_.CreateICmpULT(l, r)
                                 : builder_.CreateICmpSLT(l, r);
    if (op == ">") return unsCmp ? builder_.CreateICmpUGT(l, r)
                                 : builder_.CreateICmpSGT(l, r);
    if (op == "<=") return unsCmp ? builder_.CreateICmpULE(l, r)
                                  : builder_.CreateICmpSLE(l, r);
    if (op == ">=") return unsCmp ? builder_.CreateICmpUGE(l, r)
                                  : builder_.CreateICmpSGE(l, r);
    // CG4: logical and/or on i1. NOTE: §17 specifies short-circuit
    // evaluation; this eager form is observationally identical while
    // operands are side-effect-free (no calls lowered yet) — true
    // short-circuiting lands once call lowering exists.
    return nullptr;  // `and`/`or` handled by lowerShortCircuit
}


}  // namespace bezel::codegen
