// Codegen + sema — #1304: an `out owned` parameter of a USER-DEFINED method
// transfers an owned value to the caller (§22.4 inline out declarations,
// §22.3 `out owned` modifier, §28.8 ownership transfer). The callee writes the
// owned value through the out pointer WITHOUT disposing it; the caller adopts
// the value written and discharges it (§28.3 — typically `out using var` at the
// call site, §22.4).
//
// Root cause (pre-fix): the inline `out using var x` recipient of a
// user-defined method's `out owned T` parameter was left UNTYPED by sema. The
// inline-out-local creation in ExprTypecheck_Exprs.cpp derived the recipient's
// type only for the BUILTIN owning-collection / Weak Try* methods (which have no
// MethodDecl); for a user-defined method it never set LocalSymbol::declType, so
// a later member access on the recipient (`res!.V`) could not resolve the field
// and lowered to a default value. The callee had correctly stored the owned
// value through the out pointer (no callee-side disposal), but the caller could
// not READ it back — `res!.V` returned 0 / the wrong value. (The `out var` and
// `out owned var` forms already derived the type from the callee's formal
// parameter; only `out using var` was missing that general case.)
//
// The fix (ExprTypecheck_Exprs.cpp): the `out using var` handler now derives
// declType from the callee's formal `out owned T` parameter — mirroring the
// `out owned var` handler — and falls back to the builtin-receiver derivation
// only when no MethodDecl is present.
//
// Each case runs through the FULL pipeline (sema + the ownership pass + codegen
// + JIT), NOT the shared runJit helper: the disposal assertions (Cell.n)
// require the ownership pass to arm the using-var scope-end disposal, which
// runJit skips. Disposal is checked with a static instance counter Cell.n
// (ctor +1, dtor -1): a final 0 means each instance was disposed EXACTLY once
// (no leak, no double-free). Mirrors iterator_owned_param_945_test.cpp's runner.

#include "support/codegen_support.hpp"
#include "support/ownership_support.hpp"
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

#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

namespace {
// Full-pipeline runner: sema + ownership::checkOwnership + codegen + JIT. The
// shared runJit skips the ownership pass, so it cannot arm the using-var
// scope-end disposal these disposal assertions depend on. Mirrors
// codegen_support.hpp's runJitWithOptions plus the ownership pass (same shape
// as iterator_owned_param_945_test.cpp's run()).
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
    bezel::ownership::checkOwnership(*unit, ctx);   // arms the using-var disposal
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
// Cell: a static instance counter (ctor +1, dtor -1) so a final Cell.n == 0
// proves exactly-once disposal. V carries a constructor-supplied value so the
// caller's read-back of the owned value is observable.
const char* kCell =
    "class Cell public static n: int := 0; public V: int := 0; "
    "  public constructor(x: int) Cell.n := Cell.n + 1; V := x; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; ";
const char* kTake =
    "class C "
    "  public static method Take(out owned r: Cell?): bool "
    "    r := new Cell(42); "
    "    return true; "
    "  end Take; ";
}  // namespace

// §22.4 / §22.3 / §28.8 (#1304 repro): the callee produces an owned Cell(42)
// through an `out owned r: Cell?` parameter; the caller's `out using var res`
// receives ownership. The recipient's member read `res!.V` must return 42 —
// pre-fix it returned 0 (the untyped recipient left `res!.V` unresolved, so
// codegen lowered the member access to a default value).
BEZEL_TEST(out_owned_param_propagate_1304, value_propagated_to_caller) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kTake +
        "  public static method Main(): int "
        "    var ok := C.Take(out using var res); "
        "    if res = null then return -1; end; "
        "    return res!.V; "  // 42
        "  end Main; end C;"), 42);
}

// §28.3 / §28.8 (#1304): the caller SOLELY owns the transferred value and
// discharges it via the `out using var` scope-end disposal — disposed EXACTLY
// once (Cell.n returns to 0: no leak, no double-free). The helper method
// returns before Main reads Cell.n so the using-var has gone out of scope.
BEZEL_TEST(out_owned_param_propagate_1304, disposed_exactly_once_by_caller) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell + kTake +
        "  private static method Run(): int "
        "    var ok := C.Take(out using var res); "
        "    return 0; "  // res disposed at Run's scope end
        "  end Run; "
        "  public static method Main(): int "
        "    var d := C.Run(); "
        "    return Cell.n; "  // 0 — the caller disposed res exactly once
        "  end Main; end C;"), 0);
}

// §22.4 (#1304): the inline `out owned var res` sibling form (the other
// call-site binding for an `out owned` parameter) must likewise receive the
// owned value and let the caller read it back.
BEZEL_TEST(out_owned_param_propagate_1304, out_owned_var_form_reads_back) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static method Take(out owned r: Cell?): bool "
        "    r := new Cell(7); "
        "    return true; "
        "  end Take; "
        "  public static method Main(): int "
        "    var ok := C.Take(out var res); "
        "    if res = null then return -1; end; "
        "    return res!.V; "  // 7
        "  end Main; end C;"), 7);
}

// §28.8.1 / §28.8 (#1304 alt repro): the callee MOVES an owned value into the
// out parameter (`r := move src`). The transferred value must reach the caller
// — pre-fix `res!.V` returned 0 because the recipient was untyped.
BEZEL_TEST(out_owned_param_propagate_1304, moved_value_propagated_to_caller) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class C "
        "  public static method TakeMove(owned src: Cell, out owned r: Cell?): bool "
        "    r := move src; "
        "    return true; "
        "  end TakeMove; "
        "  public static method Main(): int "
        "    var ok := C.TakeMove(new Cell(9), out using var res); "
        "    if res = null then return -1; end; "
        "    return res!.V; "  // 9
        "  end Main; end C;"), 9);
}
