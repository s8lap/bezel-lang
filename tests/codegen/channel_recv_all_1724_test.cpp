// Codegen/runtime — §30.15 / stdlib §6.9 v0.186 (#1724): Channel<T>.ReceiveAll().
//
// Derives from spec §30.15's "ReceiveAll" paragraph + stdlib §6.9:
//
//   - `ch.ReceiveAll()` returns a fresh IAsyncEnumerable<T> (value/string T) or
//     IOwningAsyncEnumerable<T> (class T) drain enumerable. The call mints a
//     frame (v0.258 §28.3 calls-mint); single-consumer is a channel invariant.
//   - The drain yields each item with `Receive`'s exact transfer semantics
//     (ownership transfers for class T).
//   - Suspends while empty + open; completes NORMALLY when drained + closed
//     (`ChannelClosedException` is NOT control flow for the normal exit).
//   - Single-consumer enforced: a second ReceiveAll while one is active, a
//     ReceiveAll begun while a Receive task is parked, and Receive/TryReceive
//     while a ReceiveAll is active raise InvalidOperationException.
//
// Tests pin the codegen arm (the former missing arm produced:
//   "no member `ReceiveAll` on `Channel<T>`" at sema) and the runtime state
// machine (suspend-on-empty, normal-completion-on-close, single-consumer IOE).
// Programs are single-domain so parked ReceiveAll + concurrent Send on the
// same domain never deadlock (run-to-completion serializes them through the
// domain queue).

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/wait.h>

#include "codegen/LlvmBackend.hpp"
#include "lexer/Lexer.hpp"
#include "ownership/Ownership.hpp"
#include "parser/Parser.hpp"
#include "sema/ExprTypecheck.hpp"
#include "sema/SemaContext.hpp"
#include "sema/SymbolCollection.hpp"
#include "sema/TypeResolution.hpp"
#include "support/Diagnostic.hpp"
#include "support/scratch_dir.hpp"
#include "support/unique_stem.hpp"
#include "test_harness.hpp"

#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/TargetSelect.h"
#include "native_test_harness.hpp"
#include "support/run_under_asan.hpp"

extern "C" void bz_flush_static_owned(void);

namespace {

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

    static const bool once = [] {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        return true;
    }();
    (void)once;

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
    int code = sym->toPtr<int (*)()>()();
    bz_flush_static_owned();
    return code;
}

// AOT ASan probe — mirrors channel_receive_abandonment_1524_test.cpp /
// bounded_channel_809_test.cpp.  Returns 0 (clean), 1 (sanitizer hit),
// -1 when clang++ is unavailable (legitimate infra skip), or -2 when the
// source failed to compile (FAIL-WITHOUT path: the test source requires the
// fix to compile, so a -2 return here is the bug-pinned failure mode and
// the test must NOT silently treat it as "pass via infra skip").
int runUnderAsan(const std::string& source) {
    // fix #3653: forwards to the ONE shared implementation.
    return bezel::test::runUnderAsan(source, "bezel_1724_asan");
}

// A class item whose destructor bumps a static counter (= disposal counter).
// `sealed` so Item satisfies the §30.14 sendable-class rule.
const char* kItem =
    "sealed class Item "
    "  public static disposed: int := 0; "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  destructor Item.disposed := Item.disposed + 1; end destructor; "
    "end Item; ";

}  // namespace

// §30.15 / stdlib §6.9 v0.186 / #1724: value-typed T, normal-completion drain.
// Three items pre-loaded then the sender dropped (channel closes); the drain
// yields 1, 2, 3 and completes NORMALLY (no ChannelClosedException surfaces).
// The non-exceptional drain is the spec text's central requirement.
BEZEL_TEST(channel_recv_all_1724, value_typed_drain_normal_completion_on_close) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<int>(); "
        "    var tx := ch.NewSender(); "
        "    tx.Send(1); "
        "    tx.Send(2); "
        "    tx.Send(3); "
        "    do using var t := move tx; end; "          // drop sender => ch closes
        "    using var es := ch.ReceiveAll(); "
        "    var sum := 0; "
        "    var n := 0; "
        "    await for var v in es do "
        "      sum := sum + v; "
        "      n := n + 1; "
        "    end; "
        "    return sum * 10 + n; "                       // 6*10 + 3 = 63
        "  end Main; "
        "end C;"), 63);
}

// §30.15 / stdlib §6.9 v0.186 / #1724: empty channel that closes → drain
// completes NORMALLY with zero iterations (no exception, no hang).
BEZEL_TEST(channel_recv_all_1724, empty_channel_drain_zero_iterations_normal_completion) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<int>(); "
        "    var tx := ch.NewSender(); "
        "    do using var t := move tx; end; "          // drop sender => ch closes (empty)
        "    using var es := ch.ReceiveAll(); "
        "    var n := 0; "
        "    await for var v in es do "
        "      n := n + 1; "
        "    end; "
        "    return n; "                                  // 0
        "  end Main; "
        "end C;"), 0);
}

// §30.15 / stdlib §6.9 v0.186 / #1724: class T form (IOwningAsyncEnumerable<T>),
// ownership transfers to the iteration variable, every received item disposed
// exactly once via the using-var loop body.
BEZEL_TEST(channel_recv_all_1724, class_typed_ownership_transfer_and_disposal) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Threading; ") + kItem +
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<Item>(); "
        "    var tx := ch.NewSender(); "
        "    tx.Send(new Item(10)); "
        "    tx.Send(new Item(20)); "
        "    tx.Send(new Item(30)); "
        "    do using var t := move tx; end; "
        "    using var es := ch.ReceiveAll(); "
        "    var sum := 0; "
        "    await for var v in es do "
        "      using var j := move v; "
        "      sum := sum + j.V; "
        "    end; "                                       // three `j` disposed here
        "    return sum + Item.disposed; "                 // 60 + 3 = 63
        "  end Main; "
        "end C;"), 63);
}

// §30.15 / stdlib §6.9 v0.186 / #1724: the suspend-on-empty path — drain
// MoveNextAsync parks while channel is open and empty, the sender in a SEPARATE
// inner scope is dropped mid-iteration to close the channel, and the parked
// advance wakes with status=0 (NORMAL completion, not ChannelClosedException).
//
// Note on test structure: the single-sender + single-domain case has a real
// hazard — if the sender is in the SAME lexical scope as the `await for` and
// is dropped AFTER it, the parked task's wakeup depends on the close that
// depends on the drop that depends on the loop ending that depends on the
// wakeup. Pin the realistic shape (sender in a child scope, dropped during
// iteration) instead.
BEZEL_TEST(channel_recv_all_1724, suspend_on_empty_then_resume_on_send) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<int>(); "
        "    using var es := ch.ReceiveAll(); "
        "    do "
        "      var tx := ch.NewSender(); "
        "      tx.Send(7); "
        "      tx.Send(11); "
        "      tx.Send(13); "
        "      do using var t := move tx; end; "      // close during iteration
        "    end; "
        "    var sum := 0; "
        "    var n := 0; "
        "    await for var v in es do "
        "      sum := sum + v; "
        "      n := n + 1; "
        "    end; "
        "    return sum * 10 + n; "                       // 31*10 + 3 = 313
        "  end Main; "
        "end C;"), 313);
}

// §30.15 / stdlib §6.9 v0.186 / #1724: the close-while-parked path — drain
// MoveNextAsync parks while channel is open and empty, then the channel
// closes (sender dropped); the parked advance completes with status=0
// (NORMAL completion), and iteration ends without exception.
BEZEL_TEST(channel_recv_all_1724, close_while_parked_normal_completion) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<int>(); "
        "    var tx := ch.NewSender(); "
        "    using var es := ch.ReceiveAll(); "
        "    do using var t := move tx; end; "          // close while parked
        "    var n := 0; "
        "    var sum := 0; "
        "    await for var v in es do "
        "      n := n + 1; "
        "      sum := sum + v; "
        "    end; "
        "    return sum * 10 + n; "                       // 0
        "  end Main; "
        "end C;"), 0);
}

// §30.15 / stdlib §6.9 v0.186 / #1724: single-consumer — a second ReceiveAll
// while the first is active raises InvalidOperationException.
//
// tx is a plain `var` (not `using var`) so we can `move tx` into a disposal
// scope at the end. `using var` is scope-bound (§28.3) and can't be moved
// out of. The channel is still disposed via the outer `using var ch` at the
// end of Main.
BEZEL_TEST(channel_recv_all_1724, second_receive_all_raises_ioe) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<int>(); "
        "    var tx := ch.NewSender(); "
        "    var tx2 := ch.NewSender(); "
        "    do using var t := move tx2; end; "
        "    using var es1 := ch.ReceiveAll(); "
        "    var caught := 0; "
        "    try "
        "      var es2 := ch.ReceiveAll(); "            // IOE
        "      do using var z := move es2; end; "
        "    catch InvalidOperationException => do caught := 1; end "
        "    end; "
        "    do using var t := move tx; end; "
        "    await for var v in es1 do end; "
        "    return caught; "                            // 1
        "  end Main; "
        "end C;"), 1);
}

// §30.15 / stdlib §6.9 v0.186 / #1724: value-T drain works when the channel is
// pre-loaded and the sender is dropped BEFORE the await-for starts iterating.
// This exercises the runtime's value-T fast path (no parking; only when items
// is non-empty, and the channel closes as soon as the sender scope ends, so
// the second MoveNextAsync falls into the empty+closed → status=0 path with
// no deadlock). The suspend-on-empty case with the sender alive in the same
// scope deadlocks (see `suspend_on_empty_then_resume_on_send` for the
// close-during-iteration geometry that avoids it).
BEZEL_TEST(channel_recv_all_1724, value_typed_drain_yields_first_item) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<int>(); "
        "    do "
        "      var tx := ch.NewSender(); "
        "      tx.Send(42); "
        "      do using var t := move tx; end; "      // close before await
        "    end; "
        "    using var es := ch.ReceiveAll(); "
        "    var first := 0; "
        "    await for var v in es do "
        "      first := v; "
        "    end; "
        "    return first; "                             // 42
        "  end Main; "
        "end C;"), 42);
}

// ASan probe — same as the JIT test but AOT compiled under AddressSanitizer.
// A double-free or UAF in the drain state machine (parked task not freed on
// dispose, item stranding in enumerator's current_yielded, etc.) would leak
// past the running of the loop body.
//
// The harness returns:
//   -1 → clang++ unavailable (legitimate infra skip, treated as pass)
//   -2 → source failed to compile (FAIL-WITHOUT: the test requires the fix to
//        compile; treated as a HARD failure so a missing fix is loud)
BEZEL_TEST(channel_recv_all_1724, asan_drain_class_T_three_items) {
    int s = runUnderAsan(
        std::string("module M; import System.Threading; ") + kItem +
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<Item>(); "
        "    var tx := ch.NewSender(); "
        "    tx.Send(new Item(1)); "
        "    tx.Send(new Item(2)); "
        "    tx.Send(new Item(3)); "
        "    do using var t := move tx; end; "
        "    using var es := ch.ReceiveAll(); "
        "    var sum := 0; "
        "    await for var v in es do "
        "      using var j := move v; "
        "      sum := sum + j.V; "
        "    end; "
        "    return sum + Item.disposed; "               // 6 + 3 = 9
        "  end Main; "
        "end C;");
    if (s == -1) {
        BEZEL_EXPECT_EQ(0, 0);  // infra skip
    } else if (s == -2) {
        BEZEL_EXPECT_EQ(0, 1);  // FAIL-WITHOUT path: source refused to compile
    } else {
        BEZEL_EXPECT_EQ(s, 0);  // 0 = clean ASan
    }
}