// fix #1263 / §28.3 / §28.10: the explicit `finally` of a try-with-finally runs
// BEFORE the implicit auto-dispose of the owned locals declared in the try body.
//
// §28.3 (disposal ordering): "The implicit auto-dispose composes with explicit
// `finally` blocks: the explicit `finally` runs first (with the local still in
// scope), then the implicit auto-dispose runs as the scope unwinds further."
// §28.10: "`using var` compiles to `try`/`finally`, so `using var` disposals and
// explicit `finally` clauses form one establishment-ordered stack — on every
// exit edge, pending cleanups run in reverse order of establishment."
//
// Pre-fix, codegen ran the try body's implicit auto-dispose BEFORE the explicit
// finally (the throw site's emitUnwindDisposals reached the body scope, and the
// normal-exit lowerScopedBlock auto-disposed on fall-through — both fired before
// the finally body). The #1263 repro saw seq 214 (R2=2, R1=1, then finally=4)
// instead of the spec-mandated 421 (finally=4, then R2=2, R1=1): the destructors
// of the try-body owned locals ran before the finally's own event, so a finally
// observing a side effect of those destructors via shared state saw stale order.
//
// The fix (LlvmBackend_Stmts.cpp lowerTry) pushes the try body's disposal scope
// BEFORE setting the unwind depth / disposeDepthAtEntry, so the body scope sits
// at an index below thisTryDepth: a throw from the body no longer auto-disposes
// it (the throw site's range skips it), and the finally's post-body dispatch
// (mode 0 / finUnwindBB / modes 1-5) disposes it AFTER the finally body runs.
//
// Each test encodes the disposal order in a shared static `_seq` (each event
// does `_seq := _seq * 10 + k`); the final value pins the exact order. The
// full-pipeline `run()` runner includes the ownership pass (the shared runJit
// skips it, so the §28.3 auto-drop flag would never be set).

#include "support/codegen_support.hpp"
#include "test_harness.hpp"

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"

#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {

// Full-pipeline runner: parse → sema → ownership pass → codegen → JIT. The
// ownership pass is mandatory here: the §28.3 auto-drop that flags the body's
// owned locals for scope-end disposal is set by ownership::checkOwnership, which
// the shared runJit helper does not run. Mirrors iterator_owned_param_945's run().
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
    bezel::ownership::checkOwnership(*unit, ctx);   // §28.3 auto-drop flag lives here
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

}  // namespace

// §28.3 (#1263 repro): on the exception-unwind path through a try-with-finally,
// the explicit finally runs BEFORE the implicit auto-dispose of the try body's
// owned locals (reverse declaration order). Expected seq 421 (finally=4, R2=2,
// R1=1). Pre-fix this returned 214 (R2, R1, then finally).
BEZEL_TEST(finally_before_autodispose_1263, exception_path_finally_first_spec_28_3) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") +
        "class E: Exception end E; "
        "class C public static _seq: int := 0; "
        "  public static method Main(): int "
        "    C._seq := 0; "
        "    try "
        "      try "
        "        var r1 := new R1(); "
        "        var r2 := new R2(); "
        "        throw new E(); "
        "      finally "
        "        C._seq := C._seq * 10 + 4; "   // event F=4: finally runs first
        "      end; "
        "    catch E => "
        "      return C._seq; "                 // spec: 421 (F then r2 then r1)
        "    end; "
        "    return -1; "
        "  end Main; "
        "end C; "
        "class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1; "
        "class R2 destructor C._seq := C._seq * 10 + 2; end destructor; end R2; "),
        421);
}

// §28.3 / §28.10: the ordering is uniform across exit edges — on the NORMAL
// (no-throw) exit the finally also runs first, then the auto-dispose. The body
// sets event 9, then finally (4), then R2 (2), R1 (1) → 9421. Pre-fix this
// returned 9214 (R2, R1, finally).
BEZEL_TEST(finally_before_autodispose_1263, normal_path_finally_first_spec_28_3) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") +
        "class C public static _seq: int := 0; "
        "  public static method Main(): int "
        "    C._seq := 0; "
        "    try "
        "      var r1 := new R1(); "
        "      var r2 := new R2(); "
        "      C._seq := C._seq * 10 + 9; "
        "    finally "
        "      C._seq := C._seq * 10 + 4; "
        "    end; "
        "    return C._seq; "
        "  end Main; "
        "end C; "
        "class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1; "
        "class R2 destructor C._seq := C._seq * 10 + 2; end destructor; end R2; "),
        9421);
}

// §28.10: `using var` locals in the try body are disposed AFTER the finally too
// (using var compiles to the same establishment-ordered stack). Expected 421
// (finally=4, R2=2, R1=1). Pre-fix this returned 214.
BEZEL_TEST(finally_before_autodispose_1263, using_var_disposed_after_finally_spec_28_10) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") +
        "class E: Exception end E; "
        "class C public static _seq: int := 0; "
        "  public static method Main(): int "
        "    C._seq := 0; "
        "    try "
        "      try "
        "        using var r1 := new R1(); "
        "        using var r2 := new R2(); "
        "        throw new E(); "
        "      finally "
        "        C._seq := C._seq * 10 + 4; "
        "      end; "
        "    catch E => return C._seq; end; "
        "    return -1; "
        "  end Main; "
        "end C; "
        "class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1; "
        "class R2 destructor C._seq := C._seq * 10 + 2; end destructor; end R2; "),
        421);
}

// §28.3: the finally-first ordering holds with both a catch and a finally on
// the same try. The catch runs, then the finally (locals still alive), then the
// auto-dispose in reverse declaration order: catch=7, finally=4, R2=2, R1=1 →
// 7421. Pre-fix this returned 2174 (R2, R1 at the throw site, then catch, then
// finally).
BEZEL_TEST(finally_before_autodispose_1263, catch_then_finally_then_autodispose_spec_28_3) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") +
        "class E: Exception end E; "
        "class C public static _seq: int := 0; "
        "  public static method Main(): int "
        "    C._seq := 0; "
        "    try "
        "      var r1 := new R1(); "
        "      var r2 := new R2(); "
        "      throw new E(); "
        "    catch E => "
        "      C._seq := C._seq * 10 + 7; "
        "    finally "
        "      C._seq := C._seq * 10 + 4; "
        "    end; "
        "    return C._seq; "
        "  end Main; "
        "end C; "
        "class R1 destructor C._seq := C._seq * 10 + 1; end destructor; end R1; "
        "class R2 destructor C._seq := C._seq * 10 + 2; end destructor; end R2; "),
        7421);
}
