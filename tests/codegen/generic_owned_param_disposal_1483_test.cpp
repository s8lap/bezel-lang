// Codegen + runtime — issue #1483: an `owned` parameter of a GENERIC method whose
// declared type is a type parameter `T` (`method Generic<T>(owned t: T) where
// T: class`) was never auto-disposed on normal exit nor disposed on the
// exception-unwind edge — a silent leak on both paths. The concrete (non-generic)
// owned-parameter case already worked; only the type-erased generic-specialization
// codegen path (buildSpecializedMethodFn) was missing the implicit disposal
// registration §28.3 mandates for every owned parameter.
//
// §28.3 ("Auto-drop (affine)", spec v0.183): "When an owned local — or an `owned`
// parameter — reaches a normal-exit scope end without having reached one of the
// above, and is not in moved-from state, it is disposed implicitly at that scope
// end (for an `owned` parameter, at the end of the method)... Auto-drop runs
// regardless." §28.3 ("Exception-path discharge", spec v0.183): "Owned locals and
// owned parameters that are live when an exception propagates out of their
// enclosing block are disposed implicitly during stack unwinding."
//
// THE BUG (two layers, both in buildSpecializedMethodFn — LlvmBackend_Calls2.cpp):
//   Layer 1 — the specialized function bound its parameters with a manual loop
//     that wrote paramSlots_/refParams_ directly and never touched
//     pendingOwnedParams_. bindParameters (LlvmBackend_Setup.cpp) is the ONLY path
//     that fills pendingOwnedParams_, which lowerScopedBlock drains at the body
//     root scope to call registerUnwindOwnedParam. The manual loop registered no
//     owned param at all for a specialized body. FIX: route through bindParameters
//     and seed unwindIneligible_/unwindIneligibleFn_ for the specialized function
//     (the gate registerUnwindOwnedParam consults), saved/restored around the
//     nested build.
//   Layer 2 — registerUnwindOwnedParam gated on isClassOrInterfaceRef(rt); for
//     `owned t: T` with T a bare (erased) type parameter, rt->declaration is null
//     so the check was false and the function returned early. FIX: fall back
//     through resolveTypeParamRef to the active substitution's concrete
//     class/interface, mirroring the sibling disposal-registration sites.
//
// HARNESS NOTE: §28.3 normal-exit auto-drop is driven by the ownership pass, which
// sets ParameterSymbol::autoDropAtScopeEnd (registerUnwindOwnedParam picks the
// both-edges dynamicDispose path when it is set, vs the exception-only unwindOnly
// path when it is not). The default runJit harness does NOT run the ownership
// pass, so the affine auto-drop flags are never populated there. This file uses a
// full-pipeline runner that runs ownership::checkOwnership before codegen (the
// same precedent as array_getenumerator_1002 / iterator_reenum_943), matching what
// the AOT driver does — otherwise the normal-exit case cannot be exercised at all.
//
// DISPOSE-EXACTLY-ONCE DISCIPLINE (no-GC): the destructor-COUNT idiom — `R._n` is
// incremented in the destructor, so returning `R._n` yields how many times the
// object was disposed. 1 = disposed exactly once (the fix); 0 = leaked (the bug,
// on the normal and exception edges); 2 = double-freed (a regression the
// no-double-free cases guard against).

#include <memory>
#include <string>

#include "ast/Node.hpp"
#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

// Full-pipeline runner: runs the ownership pass before codegen so §28.3 affine
// auto-drop flags (autoDropAtScopeEnd) are populated and normal-exit auto-drop is
// actually emitted, then JIT-executes `main` and returns its value. Mirrors the
// local runner in array_getenumerator_1002_test / iterator_reenum_943_test.
int run(const std::string& src) {
    bezel::DiagnosticEngine diags;
    bezel::Lexer lexer(src, diags);
    bezel::Parser parser(lexer, diags);
    auto unit = parser.parseCompilationUnit();
    if (!unit) return -999;
    bezel::sema::SemaContext ctx(diags);
    bezel::sema::collectSymbols(*unit, ctx);
    bezel::sema::resolveTypes(*unit, ctx);
    bezel::sema::typecheckExpressions(*unit, ctx);
    bezel::ownership::checkOwnership(*unit, ctx);
    if (diags.hasErrors()) return -999;
    bezel::test::ensureNativeTargetInitialized();
    auto llvmCtx = std::make_unique<llvm::LLVMContext>();
    auto module = bezel::codegen::emitModule(*unit, ctx, *llvmCtx,
                                             bezel::codegen::CodegenOptions{});
    if (!module) return -999;
    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) { llvm::consumeError(jitOrErr.takeError()); return -999; }
    auto jit = std::move(*jitOrErr);
    if (auto gen = llvm::orc::DynamicLibrarySearchGenerator::GetForCurrentProcess(
            jit->getDataLayout().getGlobalPrefix())) {
        jit->getMainJITDylib().addGenerator(std::move(*gen));
    } else {
        llvm::consumeError(gen.takeError());
    }
    llvm::orc::ThreadSafeModule tsm(std::move(module), std::move(llvmCtx));
    if (auto err = jit->addIRModule(std::move(tsm))) {
        llvm::consumeError(std::move(err));
        return -999;
    }
    auto sym = jit->lookup("main");
    if (!sym) { llvm::consumeError(sym.takeError()); return -999; }
    auto fn = sym->toPtr<int (*)()>();
    return fn();
}

// Shared prelude: a throwable marker class `E` and a disposal-counting class `R`.
const char* kPre =
    "module M; "
    "class E end E; "
    "class R "
    "  public static _n: int := 0; "
    "  public constructor() end constructor; "
    "  destructor R._n := R._n + 1; end destructor; "
    "end R; ";

}  // namespace

// §28.3 exception-path discharge (HEADLINE #1483): a generic method with an
// undischarged `owned t: T` throws; the value is live on the exception edge and
// must be disposed during unwinding. Before the fix R._n stayed 0 (leak).
BEZEL_TEST(generic_owned_param_disposal_1483,
           exception_path_disposes_generic_owned_param) {
    BEZEL_EXPECT_EQ(run(
        std::string(kPre) +
        "class C "
        "  public static method Generic<T>(owned t: T) where T: class "
        "    throw new E(); "
        "  end Generic; "
        "  public static method Main(): int "
        "    try C.Generic<R>(new R()); "
        "    catch E => return R._n; end; "  // 1: disposed on unwind
        "    return -1; "
        "  end Main; "
        "end C;"), 1);
}

// §28.3 auto-drop (affine): a generic method whose `owned t: T` is never
// discharged falls off the end normally; it must be auto-dropped at method end
// (autoDropAtScopeEnd -> dynamicDispose). Before the fix R._n stayed 0 (leak).
BEZEL_TEST(generic_owned_param_disposal_1483,
           normal_exit_auto_drops_generic_owned_param) {
    BEZEL_EXPECT_EQ(run(
        std::string(kPre) +
        "class C "
        "  public static method Keep<T>(owned t: T) where T: class "
        "    return; "
        "  end Keep; "
        "  public static method Main(): int "
        "    C.Keep<R>(new R()); "
        "    return R._n; "  // 1: auto-dropped once at method end
        "  end Main; "
        "end C;"), 1);
}

// §28.3 (no double-free): an explicit `t.Dispose()` inside the generic method
// discharges the param on the normal path. The ownership walk marks it
// unwind-INELIGIBLE, so the added registration must NOT also dispose it — R._n
// must be exactly 1, never 2.
BEZEL_TEST(generic_owned_param_disposal_1483,
           explicit_dispose_in_generic_no_double_free) {
    BEZEL_EXPECT_EQ(run(
        std::string(kPre) +
        "class C "
        "  public static method Disp<T>(owned t: T) where T: class "
        "    t.Dispose(); "
        "  end Disp; "
        "  public static method Main(): int "
        "    C.Disp<R>(new R()); "
        "    return R._n; "  // 1: disposed exactly once (2 = double-free)
        "  end Main; "
        "end C;"), 1);
}

// §28.8 (transfer-out, no double-free): the `owned t: T` is RETURNED (ownership
// transferred to the caller). The callee must NOT dispose it; the caller's
// `using var` disposes it exactly once. A callee-side dispose would make R._n 2
// and corrupt the caller's live value.
BEZEL_TEST(generic_owned_param_disposal_1483,
           transfer_out_return_not_disposed_in_callee) {
    BEZEL_EXPECT_EQ(run(
        std::string(kPre) +
        "class C "
        "  public static method Pass<T>(owned t: T): owned T where T: class "
        "    return t; "
        "  end Pass; "
        "  public static method Main(): int "
        "    do using var x := C.Pass<R>(new R()); end; "
        "    return R._n; "  // 1: caller disposes once; callee transferred out
        "  end Main; "
        "end C;"), 1);
}

// §28.3 (CONCRETE control / regression guard): the CONCRETE (non-generic)
// `owned t: R` throw path already worked (per the issue). It must keep disposing
// exactly once — the generic fix must not perturb the concrete path.
BEZEL_TEST(generic_owned_param_disposal_1483,
           concrete_owned_param_control_still_disposes) {
    BEZEL_EXPECT_EQ(run(
        std::string(kPre) +
        "class C "
        "  public static method Concrete(owned t: R) "
        "    throw new E(); "
        "  end Concrete; "
        "  public static method Main(): int "
        "    try C.Concrete(new R()); "
        "    catch E => return R._n; end; "  // 1: disposed on unwind
        "    return -1; "
        "  end Main; "
        "end C;"), 1);
}

// §28.3 (CONCRETE normal-exit parity): the concrete owned-param normal-exit
// auto-drop and the generic one must behave identically (both 1). Guards against
// a future divergence between the concrete and specialized disposal paths.
BEZEL_TEST(generic_owned_param_disposal_1483,
           concrete_normal_exit_auto_drop_parity) {
    BEZEL_EXPECT_EQ(run(
        std::string(kPre) +
        "class C "
        "  public static method Keep(owned t: R) "
        "    return; "
        "  end Keep; "
        "  public static method Main(): int "
        "    C.Keep(new R()); "
        "    return R._n; "  // 1: auto-dropped once at method end
        "  end Main; "
        "end C;"), 1);
}
