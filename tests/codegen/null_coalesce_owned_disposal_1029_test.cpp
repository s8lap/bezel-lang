// Codegen — issue #1029 (§28.3): `var x := maybe ?? new Cell(7)` leaked the
// RHS-allocated Cell when `maybe` was null.  The ownership pass did not recognise
// the built-in `??` operator as an owned-producing expression, so x.autoDropAtScopeEnd
// was never set and no scope-end disposal was registered.  Fix:
//   1. producerState (Ownership.cpp): `??` with an Owned RHS returns Owned.
//   2. usingVarOwnsClassObjectDynamically (LlvmBackend_Setup.cpp): the same gate
//      registers a dynamicDispose for the binding.
//   3. unwindOwnedLocalClass (LlvmBackend_Setup.cpp): mirrors (1)/(2) for the
//      exception-unwind path.
//   4. lowerCoalesce (LlvmBackend_Exprs.cpp): when the RHS is a fresh owned-class
//      producer, null the LHS slot before the conditional branch so the LHS
//      variable's null-guarded scope-end disposal is a no-op (prevents double-
//      dispose when the LHS was itself an owned class reference). // §28.3
//
// The disposal tests require the FULL front-end including checkOwnership because
// autoDropAtScopeEnd (set by the ownership pass) is the gate that triggers
// scope-end disposal registration in codegen; runJit alone omits that pass.
// Pattern mirrors undisposed_leak_test.cpp::runOwn. // per §28.3

#include <memory>
#include <string>
#include <string_view>

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

namespace {

// JIT a program with the FULL front-end INCLUDING the ownership pass, then run
// `main` and return its exit code (-999 if it did not compile/run).
// The ownership pass sets autoDropAtScopeEnd on `var x := maybe ?? new Cell(7)`
// so that codegen registers scope-end disposal for x. Without this pass,
// autoDropAtScopeEnd stays false and no disposal is emitted — the Cell leaks.
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
        llvm::consumeError(std::move(err)); return -999;
    }
    auto sym = jit->lookup("main");
    if (!sym) { llvm::consumeError(sym.takeError()); return -999; }
    return sym->toPtr<int (*)()>()();
}

// Cell.n is incremented in the destructor (= disposal counter).
const char* kCell =
    "class Cell "
    "  public static n: int := 0; "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  destructor Cell.n := Cell.n + 1; end destructor; "
    "end Cell; ";
}  // namespace

// §28.3 / #1029 example A: null LHS → x owns the new RHS cell → disposed once.
// Before fix: Cell.n=0 (leak, 16 bytes); after fix: Cell.n=1.
BEZEL_TEST(null_coalesce_owned_disposal_1029,
           null_lhs_rhs_disposed_spec_28_3) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    var maybe: Cell? := null; "
        "    var x := maybe ?? new Cell(7); "
        "  end; "
        "  return Cell.n; "  // 1: Cell(7) disposed at scope end of `do`
        "end Main; end C;"), 1);
}

// §28.3 / #1029 example B: non-null LHS → x owns the LHS cell → disposed exactly
// once (LHS slot is nulled in codegen so the LHS variable's own disposal is a no-op;
// no double-dispose). RHS is never created.
BEZEL_TEST(null_coalesce_owned_disposal_1029,
           nonnull_lhs_disposed_exactly_once_spec_28_3) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    var maybe: Cell? := new Cell(1); "
        "    var x := maybe ?? new Cell(7); "
        "  end; "
        "  return Cell.n; "  // 1: Cell(1) disposed exactly once (via x)
        "end Main; end C;"), 1);
}

// §28.3 / #1029 non-owned RHS: `maybe ?? borrowedLocal` — RHS is a borrow, so
// `??` is not an owned producer and x carries no disposal obligation.  The LHS
// `maybe` keeps its own disposal registration (its slot is NOT nulled because RHS
// is not a fresh owned producer). Both allocated cells are disposed (one via the
// using var, one via maybe's auto-drop).
BEZEL_TEST(null_coalesce_owned_disposal_1029,
           borrow_rhs_no_spurious_dispose_spec_28_3) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    using var defaultCell := new Cell(99); "
        "    var maybe: Cell? := new Cell(1); "
        "    var x := maybe ?? defaultCell; "
        "  end; "
        "  return Cell.n; "  // 2: Cell(99) via using var + Cell(1) via maybe auto-drop
        "end Main; end C;"), 2);
}

// §28.3 / #1029: value read through the ?? result is correct (7 on null path).
BEZEL_TEST(null_coalesce_owned_disposal_1029,
           null_lhs_rhs_value_correct_spec_28_3) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    var maybe: Cell? := null; "
        "    var x := maybe ?? new Cell(7); "
        "    return x.V; "  // 7: RHS was used
        "  end; "
        "end Main; end C;"), 7);
}

// §28.3 / #1029: value read through the ?? result is correct (1 on non-null path).
BEZEL_TEST(null_coalesce_owned_disposal_1029,
           nonnull_lhs_value_correct_spec_28_3) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C public static method Main(): int "
        "  do "
        "    var maybe: Cell? := new Cell(1); "
        "    var x := maybe ?? new Cell(7); "
        "    return x.V; "  // 1: LHS (Cell.V=1) was used, not RHS
        "  end; "
        "end Main; end C;"), 1);
}
