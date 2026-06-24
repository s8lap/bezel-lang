// Codegen — #1001 / §29.8 / §29.11: a yield iterator's owned parameter is
// disposed exactly once when the IEnumerable<T> source is bound to a named
// local (plain `var`, annotated `var`, or `using var`), not only when iterated
// as a loop temporary.
//
// Background: after #943 introduced source/clone separation (each `for` loop
// obtains a fresh, independent enumerator via GetEnumerator), the loop-temporary
// path disposes the SOURCE frame at loop-end; the named-local path correctly
// skips that to avoid double-disposal and instead relies on
// registerIterableSourceDisposal / registerUsingDisposal to fire at the
// ENCLOSING BLOCK'S CLOSE.
//
// Key observation (§28.3): a `return EXPR` statement evaluates EXPR first, then
// disposes in-scope resources, then returns.  If the source is declared at the
// same level as the `return`, Cell.n is snapshotted BEFORE disposal — this is
// correct spec behaviour, not a leak.  To observe Cell.n == 0 the source must be
// declared in a tighter `do … end` inner scope that closes BEFORE `return Cell.n`
// is reached; all tests below use that pattern.
//
// These tests run the full pipeline including the ownership pass so the
// `iteratorAutoDispose` flag is set and the iterator's Dispose calls
// `disposeOwnedParams`.  Each Cell.n == 0 result means the owned argument was
// disposed exactly once (no leak, no double-free).
//
// §29.8: "The state machine disposes each `owned` parameter in the iterator's
// Dispose, in reverse declaration order, after body cleanup."
// §29.11: "Each GetEnumerator() call produces an independent enumerator; the
// IEnumerable source survives across multiple enumerations."

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
// Full-pipeline runner (mirrors iterator_reenum_943_test): includes the
// ownership pass so iteratorAutoDispose is set, then JIT-executes `main`.
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
// Shared Cell class: static counter n (ctor +1, dtor -1).
// Cell.n == 0 after `return Cell.n` iff the instance was disposed exactly once
// in the preceding `do … end` block.
const char* kCell =
    "class Cell public static n: int := 0; public V: int := 7; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; ";
}  // namespace

// §29.8 / §29.11 #1001: plain `var` named local — source declared in a
// `do … end` block so disposal fires before `return Cell.n`.
BEZEL_TEST(iterator_owned_param_named_local_1001, plain_var_named_local_disposed) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method UseCell(owned c: Cell): IEnumerable<int> "
        "    yield return c.V; yield return c.V + 1; "
        "  end UseCell; "
        "  public static method Main(): int "
        "    do "
        "      var source := P.UseCell(new Cell()); "
        "      for var x in source do end; "
        "    end; "
        "    return Cell.n; "  // 0: source (+ its Cell) disposed when do-block closed
        "  end Main; end P;"), 0);
}

// §29.8 / §29.11 #1001: explicit type annotation — disposal fires at
// the enclosing `do … end` block's close.
BEZEL_TEST(iterator_owned_param_named_local_1001, annotated_var_named_local_disposed) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method UseCell(owned c: Cell): IEnumerable<int> "
        "    yield return c.V; yield return c.V + 1; "
        "  end UseCell; "
        "  public static method Main(): int "
        "    do "
        "      var source: IEnumerable<int> := P.UseCell(new Cell()); "
        "      for var x in source do end; "
        "    end; "
        "    return Cell.n; "  // 0
        "  end Main; end P;"), 0);
}

// §29.8 / §29.11 #1001: `using var` form — disposal fires at the enclosing
// `do … end` block's close (before `return Cell.n`).
BEZEL_TEST(iterator_owned_param_named_local_1001, using_var_named_local_disposed) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method UseCell(owned c: Cell): IEnumerable<int> "
        "    yield return c.V; yield return c.V + 1; "
        "  end UseCell; "
        "  public static method Main(): int "
        "    do "
        "      using var source := P.UseCell(new Cell()); "
        "      for var x in source do end; "
        "    end; "
        "    return Cell.n; "  // 0
        "  end Main; end P;"), 0);
}

// §29.11 / §29.8 #1001: double enumeration of a named-local source — the Cell
// is disposed exactly once when the source goes out of scope, not once per loop.
BEZEL_TEST(iterator_owned_param_named_local_1001, double_enum_named_local_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method UseCell(owned c: Cell): IEnumerable<int> "
        "    yield return c.V; yield return c.V + 1; "
        "  end UseCell; "
        "  public static method Main(): int "
        "    do "
        "      var source := P.UseCell(new Cell()); "
        "      for var x in source do end; "
        "      for var x in source do end; "
        "    end; "
        "    return Cell.n; "  // 0 — disposed once at do-block close
        "  end Main; end P;"), 0);
}

// §29.11 / §29.8 #1001: nested loops (two enumerators alive simultaneously)
// over the same named-local source — Cell disposed exactly once at block close.
BEZEL_TEST(iterator_owned_param_named_local_1001, nested_loops_named_local_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method UseCell(owned c: Cell): IEnumerable<int> "
        "    yield return c.V; yield return c.V + 1; "
        "  end UseCell; "
        "  public static method Main(): int "
        "    do "
        "      var source := P.UseCell(new Cell()); "
        "      for var x in source do "
        "        for var y in source do end; "
        "      end; "
        "    end; "
        "    return Cell.n; "  // 0
        "  end Main; end P;"), 0);
}

// §29.8 / §29.11 #1001: early break in the first enumeration, then a second
// full enumeration — Cell disposed exactly once at block close.
BEZEL_TEST(iterator_owned_param_named_local_1001, early_break_then_reenum_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method UseCell(owned c: Cell): IEnumerable<int> "
        "    yield return c.V; yield return c.V + 1; yield return c.V + 2; "
        "  end UseCell; "
        "  public static method Main(): int "
        "    do "
        "      var source := P.UseCell(new Cell()); "
        "      for var x in source do break; end; "  // break after first element
        "      for var x in source do end; "          // full second enumeration
        "    end; "
        "    return Cell.n; "  // 0
        "  end Main; end P;"), 0);
}

// §29.8 / §29.11 #1001: loop-temporary form still works (no regression from
// the #943 / #945 fixes).
BEZEL_TEST(iterator_owned_param_named_local_1001, loop_temporary_still_works) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method UseCell(owned c: Cell): IEnumerable<int> "
        "    yield return c.V; yield return c.V + 1; "
        "  end UseCell; "
        "  public static method Main(): int "
        "    for var x in P.UseCell(new Cell()) do end; "
        "    return Cell.n; "  // 0 — loop-temporary disposed at for-loop end
        "  end Main; end P;"), 0);
}
