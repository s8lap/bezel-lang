// Codegen — #943 / §29.11: a second (or Nth) `for` loop over the same
// IEnumerable<T> source must replay from the start, not SIGSEGV.
//
// Root cause (pre-fix): a yield iterator method (`P.Seq(): IEnumerable<int>`)
// lowered so the value the method returned WAS the enumerator — the IEnumerable
// and the IEnumerator were the same single heap object. The first `for` loop
// advanced it to exhaustion and then disposed (freed) it; a second `for` loop
// over the same source value dereferenced the freed frame → use-after-free
// SIGSEGV. Two separate `Seq()` calls did not crash; only re-enumerating the
// SAME source value did.
//
// Fix (§29.11 "Each GetEnumerator() call produces an independent enumerator"):
// every sync IEnumerable vtable producer (yield iterator, array wrapper, user-
// class wrapper) now carries a GetEnumerator at vtable slot 4 that returns a
// FRESH, INDEPENDENT enumerator. The value the factory returns is the
// IEnumerABLE; for-in calls GetEnumerator to obtain its own enumerator, which it
// alone advances and disposes, leaving the source alive so the next loop replays
// from the start. The source is disposed exactly once at its own lifetime end.
//
// These tests run the FULL pipeline (including the ownership pass) so the source
// local's scope-end disposal and the loop enumerator's disposal are both emitted
// and exercised (a leak/double-free would surface as a wrong counter value, and
// is independently checked under ASan in the lane's manual verification).

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
// Full-pipeline runner (mirrors iterator_owned_param_945_test): runs the
// ownership pass before codegen so scope-end / loop disposals are emitted, then
// JIT-executes `main` and returns its value.
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
const char* kCell =
    "class Cell public static n: int := 0; public V: int := 7; "
    "  public constructor() Cell.n := Cell.n + 1; end constructor; "
    "  destructor Cell.n := Cell.n - 1; end destructor; end Cell; ";
}  // namespace

// §29.11: the issue's repro — a second `for` loop over the same source replays
// (sum 3 twice = 6) instead of crashing on the exhausted-and-freed enumerator.
BEZEL_TEST(iterator_reenum_943, second_for_loop_replays) {
    BEZEL_EXPECT_EQ(run(
        "module M; class P "
        "  public static method Seq(): IEnumerable<int> "
        "    yield return 1; yield return 2; "
        "  end Seq; "
        "  public static method Main(): int "
        "    var source := P.Seq(); "
        "    var s1 := 0; for var x in source do s1 := s1 + x; end; "
        "    var s2 := 0; for var x in source do s2 := s2 + x; end; "
        "    return s1 + s2; "  // 3 + 3 = 6
        "  end Main; end P;"), 6);
}

// §29.11: a THIRD enumeration of the same source still replays.
BEZEL_TEST(iterator_reenum_943, third_enumeration_replays) {
    BEZEL_EXPECT_EQ(run(
        "module M; class P "
        "  public static method Seq(): IEnumerable<int> "
        "    yield return 1; yield return 2; "
        "  end Seq; "
        "  public static method Main(): int "
        "    var source := P.Seq(); "
        "    var s1 := 0; for var x in source do s1 := s1 + x; end; "
        "    var s2 := 0; for var x in source do s2 := s2 + x; end; "
        "    var s3 := 0; for var x in source do s3 := s3 + x; end; "
        "    return s1 + s2 + s3; "  // 3 * 3 = 9
        "  end Main; end P;"), 9);
}

// §29.11: a single enumeration still works (no regression).
BEZEL_TEST(iterator_reenum_943, single_enumeration_still_works) {
    BEZEL_EXPECT_EQ(run(
        "module M; class P "
        "  public static method Seq(): IEnumerable<int> "
        "    yield return 1; yield return 2; yield return 3; "
        "  end Seq; "
        "  public static method Main(): int "
        "    var source := P.Seq(); "
        "    var s := 0; for var x in source do s := s + x; end; "
        "    return s; "  // 6
        "  end Main; end P;"), 6);
}

// §29.11: re-enumeration with a yield method that captures a PARAMETER replays
// the captured value each time (fresh state, no stale capture).
BEZEL_TEST(iterator_reenum_943, reenum_with_captured_param) {
    BEZEL_EXPECT_EQ(run(
        "module M; class P "
        "  public static method Seq(n: int): IEnumerable<int> "
        "    yield return n; yield return n + 1; "
        "  end Seq; "
        "  public static method Main(): int "
        "    var source := P.Seq(10); "
        "    var s1 := 0; for var x in source do s1 := s1 + x; end; "  // 21
        "    var s2 := 0; for var x in source do s2 := s2 + x; end; "  // 21
        "    return s1 + s2; "  // 42
        "  end Main; end P;"), 42);
}

// §29.11: two enumerators of the same source alive at once (nested loops) each
// iterate independently — the inner loop replays for every outer element.
BEZEL_TEST(iterator_reenum_943, nested_loops_independent_enumerators) {
    BEZEL_EXPECT_EQ(run(
        "module M; class P "
        "  public static method Seq(): IEnumerable<int> "
        "    yield return 1; yield return 2; "
        "  end Seq; "
        "  public static method Main(): int "
        "    var source := P.Seq(); "
        "    var sum := 0; "
        "    for var x in source do "
        "      for var y in source do sum := sum + x * y; end; "
        "    end; "
        "    return sum; "  // (1*1+1*2)+(2*1+2*2) = 3 + 6 = 9
        "  end Main; end P;"), 9);
}

// §29.11: re-enumeration after an EARLY BREAK in the first loop — the second
// loop still replays the full sequence (the broken-out enumerator was disposed,
// the source is intact).
BEZEL_TEST(iterator_reenum_943, reenum_after_early_break) {
    BEZEL_EXPECT_EQ(run(
        "module M; class P "
        "  public static method Seq(): IEnumerable<int> "
        "    yield return 1; yield return 2; yield return 3; "
        "  end Seq; "
        "  public static method Main(): int "
        "    var source := P.Seq(); "
        "    var s1 := 0; "
        "    for var x in source do s1 := s1 + x; if x = 2 then break; end; end; "
        "    var s2 := 0; for var x in source do s2 := s2 + x; end; "
        "    return s1 * 100 + s2; "  // 3*100 + 6 = 306 (full int via JIT)
        "  end Main; end P;"), 306);
}

// §29.11 / §29.6: re-enumeration of an iterator with a try/finally — each loop's
// enumerator runs the finally on completion; the source replays.
BEZEL_TEST(iterator_reenum_943, reenum_with_try_finally) {
    BEZEL_EXPECT_EQ(run(
        "module M; class P "
        "  public static method Seq(): IEnumerable<int> "
        "    var t := 0; "
        "    try yield return 1; yield return 2; finally t := 99; end; "
        "  end Seq; "
        "  public static method Main(): int "
        "    var source := P.Seq(); "
        "    var s1 := 0; for var x in source do s1 := s1 + x; end; "
        "    var s2 := 0; for var x in source do s2 := s2 + x; end; "
        "    return s1 + s2; "  // 6
        "  end Main; end P;"), 6);
}

// §29.8 / §29.11 regression: a single enumeration of an owned-parameter iterator
// still disposes the owned param EXACTLY ONCE (Cell.n nets to 0). The clone path
// must not multi-dispose the owned param — only the source (template) owns it.
BEZEL_TEST(iterator_reenum_943, owned_param_single_use_disposed_once) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; ") + kCell +
        "class P "
        "  public static method UseCell(owned c: Cell): IEnumerable<int> "
        "    yield return c.V; yield return c.V + 1; "
        "  end UseCell; "
        "  public static method Main(): int "
        "    for var x in P.UseCell(new Cell()) do end; "
        "    return Cell.n; "  // 0 — disposed exactly once
        "  end Main; end P;"), 0);
}
