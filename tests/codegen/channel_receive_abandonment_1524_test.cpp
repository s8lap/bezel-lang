// Codegen/runtime — §30.15 v0.179 (#1524): Channel<T> RECEIVE ABANDONMENT.
//
// Derives from the v0.179 give-back-protocol paragraph (language spec §30.15,
// stdlib §6.9 "Receive abandonment"):
//
//   "Abandoning a pending receive deregisters, never consumes (v0.179).
//   Disposing a Receive task before an item has been dequeued into it
//   deregisters the waiter; no message is consumed or lost — the next item
//   goes to the next receive. An item being handed to the waiter at the
//   instant of disposal is returned to the channel's head (an internal
//   give-back; items remain exactly-once). This is the one place §30.5's
//   dispose-continuation semantics do not apply to a pending result: the
//   channel's item is given back rather than produced-then-disposed."
//
// Two cases are exercised, both from stdlib §6.9's "Applies to both the
// OwningTask<T> and Task<T> forms of Receive" (class T and value T):
//
//   (a) Deregister: a `using var`/plain `var` bound to `Receive()` is dropped
//       WITHOUT await while the channel is still empty (the task is parked in
//       the waiter queue) — the waiter must deregister; a subsequently sent
//       item must reach a LATER Receive() untouched (not lost, not delivered
//       into a freed/abandoned task).
//
//   (b) Give-back: a `Receive()` local is dropped WITHOUT await after the
//       channel already had a buffered item (so Receive()'s runtime fast path,
//       bz_channel_receive, completed the task synchronously with that item
//       before it was ever bound) — the produced item must be returned to the
//       channel's head, NOT disposed by the default §30.5 "Disposing an
//       unconsumed OwningTask<T>" dispose-continuation machinery, so a
//       subsequent Receive() still observes it (same identity, same value).
//
// Both cases replace the default §30.5 behavior (bz_task_fire_and_forget +
// bz_task_set_result_disposer, LlvmBackend_Disposal.cpp) with
// bz_channel_receive_abandon (bezel_runtime_concurrency.cpp) for a task-local
// whose initializer is syntactically `Channel<T>.Receive()`
// (isChannelReceiveInitializer, LlvmBackend_Setup.cpp / LlvmBackend_Stmts.cpp).
//
// These run the FULL front-end including the ownership pass (checkOwnership),
// mirroring bounded_channel_809_test.cpp: single-domain programs, so an
// abandoned/parked Receive() and a later Send()/Receive() on the same domain
// never deadlock (run-to-completion serializes them through the domain queue).

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

extern "C" void bz_flush_static_owned(void);

namespace {

// Run the whole pipeline (INCLUDING ownership) and JIT-run `main`; returns its
// exit code, or -999 on any front-end / JIT failure. Mirrors
// bounded_channel_809_test.cpp::run.
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

namespace fs = std::filesystem;

// Per-process unique working dir, under the shared scratch base (never
// /tmp — see support/scratch_dir.hpp) and reaped at process exit.
static const std::filesystem::path& wtDir1524() {
    return wtScratchDir("wt-1524");
}

// AOT ASan probe (mirrors bounded_channel_809_test.cpp::runUnderAsan): emit an
// object from `source`, compile the runtime under ASan, link, run with leak
// detection. Returns 0 (clean), 1 (ASan/LSan hit — leak/double-free/UAF), or
// -1 when clang++/the sub-compile is unavailable (infra skip, not a failure).
int runUnderAsan(const std::string& source) {
    if (std::system("clang++ --version > /dev/null 2>&1") != 0) return -1;
    auto dir = wtDir1524();
    std::string stem = unique_stem("bezel_1524_asan");
    std::string objPath = (dir / (stem + ".o")).string();
    std::string rtObj   = (dir / (stem + "_rt.o")).string();
    std::string exe     = (dir / stem).string();

    {
        bezel::DiagnosticEngine diags;
        bezel::Lexer lexer(source, diags);
        bezel::Parser parser(lexer, diags);
        auto unit = parser.parseCompilationUnit();
        if (!unit) return -2;
        bezel::sema::SemaContext ctx(diags);
        bezel::sema::collectSymbols(*unit, ctx);
        bezel::sema::resolveTypes(*unit, ctx);
        bezel::sema::typecheckExpressions(*unit, ctx);
        bezel::ownership::checkOwnership(*unit, ctx);
        if (diags.hasErrors()) return -2;
        auto llvmCtx = std::make_unique<llvm::LLVMContext>();
        auto module = bezel::codegen::emitModule(*unit, ctx, *llvmCtx,
                                                 bezel::codegen::CodegenOptions{});
        if (!module) return -2;
        std::string err;
        if (!bezel::codegen::emitObjectFile(*module, objPath, err)) return -2;
    }
    auto cleanup = [&] {
        std::remove(objPath.c_str());
        std::remove(rtObj.c_str());
        std::remove(exe.c_str());
    };
    if (!::bezel::native_test::buildRuntimeSanitizerObject("address", rtObj)) {
        cleanup();
        return -1;
    }
    std::string linkCmd = "clang++ -fsanitize=address -g " + objPath + " " +
                          rtObj + " -lm -lpthread -lstdc++ -o " + exe +
                          " > /dev/null 2>&1";
    if (std::system(linkCmd.c_str()) != 0) { cleanup(); return -3; }
    std::string runCmd =
        "ASAN_OPTIONS=detect_leaks=1:exitcode=99 " + exe + " > /dev/null 2>&1";
    int status = std::system(runCmd.c_str());
    cleanup();
    if (status == -1) return -4;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 99) return 1;  // sanitizer hit
    return 0;                                                       // clean
}

// A class item whose destructor bumps a static counter (= disposal counter).
// `sealed` so Item satisfies the §30.14 sendable-class rule (Channel<Item>).
// Mirrors bounded_channel_809_test.cpp::kItem.
const char* kItem =
    "sealed class Item "
    "  public static disposed: int := 0; "
    "  public V: int := 0; "
    "  public constructor(v: int) V := v; end constructor; "
    "  destructor Item.disposed := Item.disposed + 1; end destructor; "
    "end Item; ";

}  // namespace

// §30.15 v0.179 case (a) / class T: a `using var` bound to `Receive()` is
// dropped while the channel is still empty (the task is parked in the
// channel's waiter queue). The waiter must deregister — a subsequently sent
// item is untouched and reaches a LATER Receive() exactly once.
BEZEL_TEST(channel_receive_abandonment_1524, deregister_still_parked_owning_task) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Threading; ") + kItem +
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<Item>(); "
        "    using var tx := ch.NewSender(); "
        "    do "
        "      using var t := ch.Receive(); "   // channel empty -> parks
        "    end; "                              // t dropped -> deregister
        "    tx.Send(new Item(42)); "
        "    var v := 0; "
        "    do "
        "      using var item := await ch.Receive(); "
        "      v := item.V; "
        "    end; "                               // item disposed HERE (disposed -> 1)
        // `Item.disposed` is read AFTER item's own using-var scope end (not in
        // the same expression as the receive) so it observes the post-
        // disposal count, not the pre-disposal one (§28.10 scope-end timing).
        "    return v * 100 + Item.disposed; "     // 42*100 + 1 = 4201
        "  end Main; "
        "end C;"), 4201);
}

// §30.15 v0.179 case (b) / class T: a `using var` bound to `Receive()` is
// dropped WITHOUT await after the channel already had a buffered item, so the
// runtime fast path (bz_channel_receive) completed the OwningTask<Item>
// synchronously with that item before `t` was ever bound. The produced item
// must be given back to the channel's head — NOT disposed by the default
// §30.5 unconsumed-OwningTask<T> machinery — so a later Receive() still
// observes the SAME item (same V, and disposed exactly once overall: the
// abandonment must not dispose it, and the later real receive must).
BEZEL_TEST(channel_receive_abandonment_1524, giveback_already_produced_owning_task) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Threading; ") + kItem +
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<Item>(); "
        "    using var tx := ch.NewSender(); "
        "    tx.Send(new Item(7)); "             // buffered before Receive() is even called
        "    do "
        "      using var t := ch.Receive(); "    // fast path: item already available
        "    end; "                               // t dropped -> give back, NOT dispose
        "    var v := 0; "
        "    do "
        "      using var item := await ch.Receive(); "  // must still observe the SAME item
        "      v := item.V; "
        "    end; "                               // item disposed HERE (disposed -> 1)
        "    return v * 100 + Item.disposed; "     // 7*100 + 1 = 701 (disposed ONCE, not zero/twice)
        "  end Main; "
        "end C;"), 701);
}

// §30.15 v0.179, both cases combined / class T: two abandonments in sequence
// (deregister then give-back) followed by two real receives — proves no item
// is lost, none is double-consumed, and Item.disposed ends at exactly 2 (one
// per item that was actually received; the two abandoned tasks contribute
// zero disposals of their own).
BEZEL_TEST(channel_receive_abandonment_1524, both_cases_no_loss_no_double_dispose) {
    BEZEL_EXPECT_EQ(run(
        std::string("module M; import System.Threading; ") + kItem +
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<Item>(); "
        "    using var tx := ch.NewSender(); "
        "    do using var t := ch.Receive(); end; "   // case (a): parks, deregisters
        "    tx.Send(new Item(42)); "
        "    var a := 0; "
        "    do "
        "      using var itemA := await ch.Receive(); "
        "      a := itemA.V; "
        "    end; "
        "    tx.Send(new Item(7)); "
        "    do using var t2 := ch.Receive(); end; "  // case (b): fast-done, gives back
        "    var b := 0; "
        "    do "
        "      using var itemB := await ch.Receive(); "
        "      b := itemB.V; "
        "    end; "
        "    return a * 1000000 + b * 1000 + Item.disposed; "  // 42*1e6+7*1000+2 = 42007002
        "  end Main; "
        "end C;"), 42007002);
}

// §30.15 v0.179 / stdlib §6.9 "Applies to both the OwningTask<T> and Task<T>
// forms of Receive": the SAME deregister + give-back protocol for a value-typed
// T (Task<int>, no ownership transfer — the runtime item encoding is an
// inttoptr-boxed int rather than a class pointer, exercising the other branch
// of bz_channel_receive_abandon / channel_giveback_item).
BEZEL_TEST(channel_receive_abandonment_1524, deregister_and_giveback_value_typed_task) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<int>(); "
        "    using var tx := ch.NewSender(); "
        "    do var t := ch.Receive(); end; "        // case (a): channel empty -> parks
        "    tx.Send(99); "
        "    var a := await ch.Receive(); "           // must observe 99, not lost
        "    tx.Send(13); "
        "    do var t2 := ch.Receive(); end; "        // case (b): fast path -> gives back
        "    var b := await ch.Receive(); "           // must observe 13, not lost
        "    return a * 1000 + b; "                     // 99*1000 + 13 = 99013
        "  end Main; "
        "end C;"), 99013);
}

// §30.15 v0.179 case (a), plain `var` (not `using var`): #445/§30.5 already
// disposes a plain `var` task local at scope end via the same fire-and-forget
// path as `using var` (LlvmBackend_Stmts.cpp); a Receive() bound to a plain
// `var` and dropped must be abandoned the same way — deregister, no loss.
// Value-typed T (Task<int>, not OwningTask<Item>): a bare `var` bound to an
// OwningTask<T:class> is an ownership-pass error demanding explicit disposal
// (§28.3 — `using var`/`.Dispose()`/`move`/`return`), so only the non-owned
// Task<T> form of Receive() admits the plain-`var`-silently-dropped shape
// this test isolates (the owning-form / class-T abandonment is covered via
// `using var` in the tests above).
BEZEL_TEST(channel_receive_abandonment_1524, deregister_plain_var_not_using) {
    BEZEL_EXPECT_EQ(run(
        "module M; import System.Threading; "
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<int>(); "
        "    using var tx := ch.NewSender(); "
        "    do "
        "      var t := ch.Receive(); "     // plain var, not using var -> still parks
        "    end; "                          // dropped at scope end -> deregister
        "    tx.Send(5); "
        "    var v := await ch.Receive(); "
        "    return v; "                     // 5
        "  end Main; "
        "end C;"), 5);
}

// §30.15 v0.179 ASan: the full combined-cases program (both_cases_... above)
// under AddressSanitizer + LeakSanitizer — the give-back path (case b) must
// not leak the task's own result buffer (a regression caught during this
// fix's own development: an earlier version nulled task->result after taking
// the item, which skipped bz_task_free's free(task->result) for the 8-byte
// pointer-sized buffer) and the core-ref fix (bz_channel_receive /
// bz_task_free, mirroring fix #634's Channel/ChannelSender refcounting) must
// not UAF the channel core from a deferred give-back continuation.
BEZEL_TEST(channel_receive_abandonment_1524, asan_both_cases_no_leak_no_uaf) {
    int r = runUnderAsan(
        std::string("module M; import System.Threading; ") + kItem +
        "class C "
        "  public static async method Main(): Task<int> "
        "    using var ch := new Channel<Item>(); "
        "    using var tx := ch.NewSender(); "
        "    do using var t := ch.Receive(); end; "
        "    tx.Send(new Item(42)); "
        "    do "
        "      using var itemA := await ch.Receive(); "
        "    end; "
        "    tx.Send(new Item(7)); "
        "    do using var t2 := ch.Receive(); end; "
        "    do "
        "      using var itemB := await ch.Receive(); "
        "    end; "
        "    return 0; "
        "  end Main; "
        "end C;");
    if (r != -1) BEZEL_EXPECT_EQ(r, 0);
}
